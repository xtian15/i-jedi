/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Increment/MpasIncrementBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "eckit/exception/Exceptions.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/State/MpasFieldSetOwner.h"

namespace ijedi {
namespace {
using Json = nlohmann::json;

std::string dateString(const util::DateTime &time) {
  std::ostringstream out;
  out << time;
  return out.str();
}

std::string rawName(const std::string &name, const std::string &space) {
  return space == "control" ? name.substr(8) : name;
}

std::string metricFor(const Json &descriptor) {
  const auto location = descriptor.at("horizontal_location").get<std::string>();
  const auto stagger = descriptor.at("vertical_stagger").get<std::string>();
  if (location != "cell" && location != "edge") {
    throw eckit::BadParameter("unsupported MPAS Increment horizontal location", Here());
  }
  return location == "edge"
             ? "edge_layer"
             : (stagger == "surface" ? "cell_surface"
                                     : (stagger == "interface" ? "cell_interface" : "cell_layer"));
}

MpasAnalysisArray zeroFor(const MpasAnalysisArrays &metrics, const Json &descriptor) {
  MpasAnalysisArray zero = metrics.at(metricFor(descriptor));
  if (!descriptor.at("tracer_order").empty()) {
    zero.shape.push_back(descriptor.at("tracer_order").size());
    zero.values.resize(zero.values.size() * zero.shape.back());
  }
  std::fill(zero.values.begin(), zero.values.end(), 0.);
  return zero;
}

std::string spaceFor(const MpasBackendContext &context, const oops::Variables &variables) {
  std::string result;
  for (const auto &var : variables) {
    const auto &name = var.name();
    const std::string space = context.variableNamespace(name);
    if (space != "native" && space != "control" && space != "geoval") {
      throw eckit::BadParameter("unsupported MPAS Increment namespace", Here());
    }
    if (space != "geoval") {
      const auto inventory = context.analysisInventory(space);
      const auto raw = rawName(name, space);
      if (!std::binary_search(inventory.begin(), inventory.end(), raw)) {
        throw eckit::BadParameter("unsupported MPAS Increment analysis name: " + name, Here());
      }
    }
    if (!result.empty() && result != space) {
      throw eckit::BadParameter("MPAS Increment cannot mix native/control/GeoVaL namespaces",
                                Here());
    }
    result = space;
  }
  if (result.empty()) {
    throw eckit::BadParameter("MPAS Increment needs a nonempty inventory", Here());
  }
  return result;
}

MpasAnalysisArray read(const atlas::Field &field, bool surface,
                       const std::vector<atlas::idx_t> *order) {
  if (field.datatype() != atlas::array::make_datatype<double>() ||
      (field.rank() != 2 && field.rank() != 3)) {
    throw eckit::BadValue("typed MPAS Increment dtype/rank mismatch", Here());
  }
  MpasAnalysisArray result;
  for (auto extent : field.shape()) {
    result.shape.push_back(static_cast<size_t>(extent));
  }
  if (surface) {
    result.shape.pop_back();
  }
  result.values.resize(field.size());
  const size_t stride = field.size() / field.shape(0);
  for (atlas::idx_t row = 0; row < field.shape(0); ++row) {
    const atlas::idx_t sourceRow = order ? order->at(row) : row;
    for (size_t column = 0; column < stride; ++column) {
      const double value = field.rank() == 2
                               ? atlas::array::make_view<double, 2>(field)(sourceRow, column)
                               : atlas::array::make_view<double, 3>(field)(
                                     sourceRow, column / field.shape(2), column % field.shape(2));
      if (!std::isfinite(value)) {
        throw eckit::BadValue("nonfinite typed MPAS Increment", Here());
      }
      result.values[row * stride + column] = value;
    }
  }
  return result;
}
}  // namespace

oops::FieldSet3D &IncrementBackend::atlasFields() {
  throw eckit::BadValue("MPAS Increment has no single-space FieldSet3D authority", Here());
}
const oops::FieldSet3D &IncrementBackend::atlasFields() const {
  throw eckit::BadValue("MPAS Increment has no single-space FieldSet3D authority", Here());
}
MpasIncrementBackend &IncrementBackend::typed() {
  throw eckit::BadValue("Atlas Increment has no MPAS typed carrier", Here());
}
const MpasIncrementBackend &IncrementBackend::typed() const {
  throw eckit::BadValue("Atlas Increment has no MPAS typed carrier", Here());
}

std::string MpasIncrementBackend::namespaceFor(const MpasBackendContext &context,
                                               const oops::Variables &variables) {
  return spaceFor(context, variables);
}

oops::Variables MpasIncrementBackend::expandVectorDependencies(const MpasBackendContext &context,
                                                              const oops::Variables &variables) {
  for (const auto &variable : variables) (void)context.variableNamespace(variable.name());
  const auto names = variables.variables();
  std::vector<oops::Variable> expanded;
  for (const auto &variable : variables) expanded.push_back(variable);
  const auto wind = context.geovalWindNames();
  const bool east = std::find(names.begin(), names.end(), wind[0]) != names.end();
  const bool north = std::find(names.begin(), names.end(), wind[1]) != names.end();
  if (east && !north) {
    expanded.emplace_back(wind[1]);
  }
  if (north && !east) {
    expanded.emplace_back(wind[0]);
  }
  return oops::Variables(expanded);
}

MpasIncrementBackend::MpasIncrementBackend(const Geometry &geometry,
                                           const oops::Variables &variables,
                                           const util::DateTime &time)
    : geom_(geometry),
      variables_(variables),
      carriedVariables_(variables),
      time_(time),
      namespace_(spaceFor(*geometry.mpasContext(), variables)) {
  const auto context = geom_.mpasContext();
  (void)geom_.variableSizes(variables_);
  carriedVariables_ = expandVectorDependencies(*context, variables_);
  (void)geom_.variableSizes(carriedVariables_);
  metrics_ = context->analysisMeasures();
  MpasAnalysisArrays zeros;
  for (const auto &var : carriedVariables_) {
    const std::string name = var.name();
    const auto binding = context->variableBinding(name, namespace_, dateString(time_));
    if (!bindings_.emplace(name, binding).second) {
      throw eckit::BadParameter("duplicate MPAS Increment variable", Here());
    }
    const auto descriptor = Json::parse(binding).at("descriptor");
    metricsFor_.emplace(name, metricFor(descriptor));
    zeros.emplace(rawName(name, namespace_), zeroFor(metrics_, descriptor));
  }
  fields_ = allocate(zeros);
}

MpasAnalysisArrays MpasIncrementBackend::completeArrays() const {
  if (namespace_ != "native" && namespace_ != "control") {
    throw eckit::BadParameter("MPAS zero embedding requires native or control Increment", Here());
  }
  auto result = arrays();  // Validates all bindings and storage before owner calls.
  const auto context = geom_.mpasContext();
  const auto inventory = context->analysisInventory(namespace_);
  for (const auto &[name, array] : result) {
    if (!std::binary_search(inventory.begin(), inventory.end(), name)) {
      throw eckit::BadParameter("MPAS Increment is outside the owner's analysis inventory", Here());
    }
  }
  for (const auto &raw : inventory) {
    if (!result.count(raw)) {
      const auto name = namespace_ == "control" ? "control_" + raw : raw;
      const auto descriptor =
          Json::parse(context->variableBinding(name, namespace_, dateString(time_)))
              .at("descriptor");
      result.emplace(raw, zeroFor(metrics_, descriptor));
    }
  }
  return result;
}

atlas::FieldSet MpasIncrementBackend::allocate(const MpasAnalysisArrays &arrays) const {
  if (arrays.size() != bindings_.size()) {
    throw eckit::BadValue("MPAS Increment array inventory differs", Here());
  }
  geom_.mpasAtlasGeometry().validateStorage();
  atlas::FieldSet result;
  for (const auto &var : carriedVariables_) {
    const std::string name = var.name();
    const auto &array = arrays.at(rawName(name, namespace_));
    const auto descriptor = Json::parse(bindings_.at(name)).at("descriptor");
    const bool surface = descriptor.at("vertical_stagger") == "surface";
    std::vector<size_t> shape = metrics_.at(metricsFor_.at(name)).shape;
    if (!descriptor.at("tracer_order").empty()) {
      shape.push_back(descriptor.at("tracer_order").size());
    }
    size_t count = 1;
    for (size_t extent : shape) {
      count *= extent;
    }
    if (array.shape != shape || array.values.size() != count ||
        !std::all_of(array.values.begin(), array.values.end(),
                     [](double x) { return std::isfinite(x); })) {
      throw eckit::BadValue("MPAS Increment shape/stagger/tracer/finite-value mismatch", Here());
    }
    if (surface) {
      shape.push_back(1);
    }
    const bool edge = descriptor.at("horizontal_location") == "edge";
    const auto &space =
        edge ? geom_.mpasAtlasGeometry().edgeNormals() : geom_.mpasAtlasGeometry().cellNodes();
    atlas::array::ArrayShape atlasShape;
    for (auto extent : shape) {
      atlasShape.push_back(extent);
    }
    atlas::Field field(name, atlas::array::make_datatype<double>(), atlasShape);
    field.set_functionspace(space);
    field.set_levels(shape.at(1));
    if (shape.size() == 3) {
      field.set_variables(shape.at(2));
    }
    const size_t stride = count / shape.at(0);
    for (size_t row = 0; row < shape.at(0); ++row) {
      const auto target = edge ? geom_.mpasAtlasGeometry().nativeToAtlasEdges().at(row) : row;
      for (size_t column = 0; column < stride; ++column) {
        const double value = array.values.at(row * stride + column);
        if (shape.size() == 2) {
          atlas::array::make_view<double, 2>(field)(target, column) = value;
        } else {
          atlas::array::make_view<double, 3>(field)(target, column / shape[2], column % shape[2]) =
              value;
        }
      }
    }
    field.metadata().set("mpas_descriptor_binding", bindings_.at(name));
    for (const std::string key :
         {"semantic_id", "units", "horizontal_location", "vertical_stagger", "component_basis"}) {
      field.metadata().set(key, descriptor.at(key).get<std::string>());
    }
    field.metadata().set("interp_type", "default");
    result.add(field);
  }
  return ownMpasFieldSet(result);
}

void MpasIncrementBackend::validate() const {
  geom_.mpasAtlasGeometry().validateStorage();
  if (fields_.size() != bindings_.size()) {
    throw eckit::BadValue("MPAS Increment field inventory changed", Here());
  }
  for (const auto &[name, binding] : bindings_) {
    if (!fields_.has(name) || fields_.field(name).name() != name) {
      throw eckit::BadValue("MPAS Increment field name changed", Here());
    }
    const auto field = fields_.field(name);
    const auto descriptor = Json::parse(binding).at("descriptor");
    const bool edge = descriptor.at("horizontal_location") == "edge";
    const auto &space =
        edge ? geom_.mpasAtlasGeometry().edgeNormals() : geom_.mpasAtlasGeometry().cellNodes();
    auto shape = metrics_.at(metricsFor_.at(name)).shape;
    if (descriptor.at("vertical_stagger") == "surface") {
      shape.push_back(1);
    }
    if (!descriptor.at("tracer_order").empty()) {
      shape.push_back(descriptor.at("tracer_order").size());
    }
    if (field.functionspace().get() != space.get() || field.rank() != shape.size() ||
        field.datatype() != atlas::array::make_datatype<double>() ||
        field.metadata().getString("mpas_descriptor_binding") != binding) {
      throw eckit::BadValue(
          "MPAS Increment descriptor, dtype, source-space or owner binding changed", Here());
    }
    for (size_t dim = 0; dim < shape.size(); ++dim) {
      if (field.shape(dim) != shape[dim]) {
        throw eckit::BadValue("MPAS Increment extent changed", Here());
      }
    }
    if (field.levels() != shape.at(1) ||
        field.variables() != (shape.size() == 3 ? shape.at(2) : 0)) {
      throw eckit::BadValue("MPAS Increment Atlas level/component count changed", Here());
    }
    for (const std::string key :
         {"semantic_id", "units", "horizontal_location", "vertical_stagger", "component_basis"}) {
      if (field.metadata().getString(key) != descriptor.at(key).get<std::string>()) {
        throw eckit::BadValue("MPAS Increment semantic metadata changed", Here());
      }
    }
  }
}

MpasAnalysisArrays MpasIncrementBackend::arrays() const {
  validate();
  MpasAnalysisArrays result;
  for (const auto &[name, binding] : bindings_) {
    const auto descriptor = Json::parse(binding).at("descriptor");
    const bool edge = descriptor.at("horizontal_location") == "edge";
    result.emplace(rawName(name, namespace_),
                   read(fields_.field(name), descriptor.at("vertical_stagger") == "surface",
                        edge ? &geom_.mpasAtlasGeometry().nativeToAtlasEdges() : nullptr));
  }
  return result;
}

MpasAnalysisArrays MpasIncrementBackend::measures() const {
  validate();
  MpasAnalysisArrays result;
  for (const auto &[name, binding] : bindings_) {
    result.emplace(rawName(name, namespace_), metrics_.at(metricsFor_.at(name)));
  }
  return result;
}

void MpasIncrementBackend::setArrays(const MpasAnalysisArrays &arrays) {
  fields_ = allocate(arrays);
}
atlas::FieldSet &MpasIncrementBackend::fieldSet() {
  validate();
  return fields_;
}
const atlas::FieldSet &MpasIncrementBackend::fieldSet() const {
  validate();
  return fields_;
}

void MpasIncrementBackend::compatible(const MpasIncrementBackend &other, bool checkTime) const {
  validate();
  other.validate();
  if ((checkTime && time_ != other.time_) || variables_ != other.variables_ ||
      namespace_ != other.namespace_ ||
      geom_.mpasContext()->geometryReceipt() != other.geom_.mpasContext()->geometryReceipt() ||
      geom_.mpasContext()->configurationReceipt() !=
          other.geom_.mpasContext()->configurationReceipt() ||
      geom_.mpasContext()->stateSchemaDigest() != other.geom_.mpasContext()->stateSchemaDigest()) {
    throw eckit::BadParameter(
        "MPAS Increment time/variables/geometry/configuration/schema mismatch", Here());
  }
}

void MpasIncrementBackend::fill(double value) {
  if (!std::isfinite(value)) {
    throw eckit::BadValue("nonfinite MPAS Increment fill", Here());
  }
  auto result = arrays();
  for (auto &[name, array] : result) {
    std::fill(array.values.begin(), array.values.end(), value);
  }
  setArrays(result);
}
void MpasIncrementBackend::zero() { fill(0.); }
void MpasIncrementBackend::scale(double weight) {
  if (!std::isfinite(weight)) {
    throw eckit::BadValue("nonfinite MPAS Increment scale", Here());
  }
  auto result = arrays();
  for (auto &[name, array] : result) {
    for (double &value : array.values) {
      value *= weight;
    }
  }
  setArrays(result);
}
void MpasIncrementBackend::axpy(double weight, const MpasIncrementBackend &other, bool checkTime) {
  compatible(other, checkTime);
  if (!std::isfinite(weight)) {
    throw eckit::BadValue("nonfinite MPAS Increment axpy", Here());
  }
  auto result = arrays();
  const auto rhs = other.arrays();
  for (auto &[name, array] : result) {
    for (size_t i = 0; i < array.values.size(); ++i) {
      array.values[i] += weight * rhs.at(name).values[i];
    }
  }
  setArrays(result);
}
void MpasIncrementBackend::schur(const MpasIncrementBackend &other) {
  compatible(other, true);
  auto result = arrays();
  const auto rhs = other.arrays();
  for (auto &[name, array] : result) {
    for (size_t i = 0; i < array.values.size(); ++i) {
      array.values[i] *= rhs.at(name).values[i];
    }
  }
  setArrays(result);
}
void MpasIncrementBackend::squareRoot() {
  auto result = arrays();
  for (auto &[name, array] : result) {
    for (double &value : array.values) {
      if (value < 0) {
        throw eckit::BadValue("negative MPAS Increment square root", Here());
      }
      value = std::sqrt(value);
    }
  }
  setArrays(result);
}
void MpasIncrementBackend::random() {
  auto result = arrays();
  std::mt19937 generator(1729);
  std::normal_distribution<double> normal;
  for (auto &[name, array] : result) {
    for (double &value : array.values) {
      value = normal(generator);
    }
  }
  setArrays(result);
}
double MpasIncrementBackend::dot(const MpasIncrementBackend &other) const {
  compatible(other, true);
  const auto lhs = arrays(), rhs = other.arrays(), mass = measures();
  long double total = 0;
  for (const auto &[name, array] : lhs) {
    const auto &m = mass.at(name).values;
    const size_t repeat = array.values.size() / m.size();
    for (size_t i = 0; i < array.values.size(); ++i) {
      total +=
          static_cast<long double>(array.values[i]) * m.at(i / repeat) * rhs.at(name).values[i];
    }
  }
  const double result = static_cast<double>(total);
  if (!std::isfinite(result)) {
    throw eckit::BadValue("MPAS Increment dot overflow", Here());
  }
  return result;
}
double MpasIncrementBackend::norm() const { return std::sqrt(dot(*this)); }

void MpasIncrementBackend::updateTime(const util::Duration &duration) {
  const util::DateTime next = time_ + duration;
  std::map<std::string, std::string> bindings;
  for (const auto &[name, old] : bindings_) {
    bindings.emplace(name,
                     geom_.mpasContext()->variableBinding(name, namespace_, dateString(next)));
  }
  const auto values = arrays();
  bindings_.swap(bindings);
  try {
    const auto fields = allocate(values);
    fields_ = fields;
    time_ = next;
  } catch (...) {
    bindings_.swap(bindings);
    throw;
  }
}
void MpasIncrementBackend::toFieldSet(atlas::FieldSet &target) const {
  // Validate and allocate before committing the handle. clear/add would
  // destroy caller output on failure, or mutate our authority through an alias.
  if (&target == &fields_) {
    throw eckit::BadParameter("MPAS export cannot replace its authoritative FieldSet", Here());
  }
  const auto values = arrays();
  const auto replacement = allocate(values);
  target = replacement;
}
void MpasIncrementBackend::fromFieldSet(const atlas::FieldSet &source) {
  const auto previous = fields_;
  fields_ = source;
  try {
    const auto values = arrays();
    fields_ = allocate(values);
  } catch (...) {
    fields_ = previous;
    throw;
  }
}

std::string MpasIncrementBackend::serializationMetadata() const {
  Json result{{"schema", "ijedi-mpas-increment-float64-le-v2"},
              {"bindings", bindings_},
              {"requested_variables", variables_.variables()},
              {"namespace", namespace_},
              {"time", dateString(time_)},
              {"arrays", Json::object()}};
  for (const auto &[name, binding] : bindings_) {
    auto shape = metrics_.at(metricsFor_.at(name)).shape;
    const auto descriptor = Json::parse(binding).at("descriptor");
    if (!descriptor.at("tracer_order").empty()) {
      shape.push_back(descriptor.at("tracer_order").size());
    }
    result["arrays"][rawName(name, namespace_)] = shape;
  }
  return result.dump();
}
size_t MpasIncrementBackend::serializationBytes() const {
  size_t bytes = 8 + serializationMetadata().size() + 64;
  for (const auto &[name, binding] : bindings_) {
    const auto descriptor = Json::parse(binding).at("descriptor");
    const size_t tracers =
        descriptor.at("tracer_order").empty() ? 1 : descriptor.at("tracer_order").size();
    bytes += metrics_.at(metricsFor_.at(name)).values.size() * tracers * sizeof(double);
  }
  if (bytes > 512 * 1024 * 1024) {
    throw eckit::BadValue("MPAS Increment exceeds bounded serialization capacity", Here());
  }
  return bytes;
}
std::string MpasIncrementBackend::envelope() const {
  static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
  const auto metadata = serializationMetadata();
  std::string bytes;
  bytes.reserve(serializationBytes());
  const auto appendInteger = [&](std::uint64_t value) {
    for (size_t byte = 0; byte < 8; ++byte) {
      bytes.push_back(static_cast<char>(value >> (byte * 8)));
    }
  };
  appendInteger(metadata.size());
  bytes += metadata;
  for (const auto &[name, array] : arrays()) {
    for (double value : array.values) {
      std::uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      appendInteger(bits);
    }
  }
  AtlasOperatorReceipt hash;
  hash.string(bytes);
  bytes += hash.finish();
  if (bytes.size() != serializationBytes()) {
    throw eckit::BadValue("MPAS Increment serialization changed its declared layout", Here());
  }
  return bytes;
}
size_t MpasIncrementBackend::serialSize() const {
  validate();
  // OOPS calls this for object-size accounting on every construction. Exact
  // bytes are fixed by the sealed layout, not by decimal formatting of values.
  // Retain finite-value preflight without allocating or serializing tensors.
  for (const auto &field : fields_) {
    const size_t stride = field.size() / field.shape(0);
    for (atlas::idx_t row = 0; row < field.shape(0); ++row) {
      for (size_t column = 0; column < stride; ++column) {
        const double value = field.rank() == 2
                                 ? atlas::array::make_view<double, 2>(field)(row, column)
                                 : atlas::array::make_view<double, 3>(field)(
                                       row, column / field.shape(2), column % field.shape(2));
        if (!std::isfinite(value)) {
          throw eckit::BadValue("nonfinite MPAS Increment serialization", Here());
        }
      }
    }
  }
  const size_t bytes = serializationBytes();
  return 2 + (bytes + 7) / 8;
}
void MpasIncrementBackend::serialize(std::vector<double> &buffer) const {
  const auto bytes = envelope();
  buffer.push_back(9142028.);
  buffer.push_back(bytes.size());
  const size_t begin = buffer.size();
  buffer.resize(begin + (bytes.size() + 7) / 8, 0.);
  std::memcpy(buffer.data() + begin, bytes.data(), bytes.size());
}
void MpasIncrementBackend::deserialize(const std::vector<double> &buffer, size_t &index) {
  size_t cursor = index;
  if (cursor > buffer.size() || buffer.size() - cursor < 2 || buffer[cursor++] != 9142028.) {
    throw eckit::BadValue("invalid MPAS Increment frame", Here());
  }
  const double encoded = buffer[cursor++];
  if (!std::isfinite(encoded) || encoded < 0 || std::floor(encoded) != encoded ||
      encoded > 512 * 1024 * 1024 || encoded != serializationBytes()) {
    throw eckit::BadValue("invalid MPAS Increment byte count", Here());
  }
  const size_t count = static_cast<size_t>(encoded), words = (count + 7) / 8;
  if (words > buffer.size() - cursor) {
    throw eckit::BadValue("truncated MPAS Increment frame", Here());
  }
  std::string bytes(count, '\0');
  std::memcpy(bytes.data(), buffer.data() + cursor, count);
  const auto *framed = reinterpret_cast<const unsigned char *>(buffer.data() + cursor);
  for (size_t padding = count; padding < words * sizeof(double); ++padding) {
    if (framed[padding] != 0) {
      throw eckit::BadValue("MPAS Increment has noncanonical framing padding", Here());
    }
  }
  AtlasOperatorReceipt hash;
  hash.string(bytes.substr(0, count - 64));
  if (bytes.substr(count - 64) != hash.finish()) {
    throw eckit::BadValue("MPAS Increment payload receipt changed", Here());
  }
  size_t offset = 0;
  const auto readInteger = [&]() {
    if (offset > count - 64 || count - 64 - offset < 8) {
      throw eckit::BadValue("truncated MPAS Increment binary payload", Here());
    }
    std::uint64_t value = 0;
    for (size_t byte = 0; byte < 8; ++byte) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++]))
               << (byte * 8);
    }
    return value;
  };
  const size_t metadataBytes = readInteger();
  const auto expectedMetadata = serializationMetadata();
  if (metadataBytes != expectedMetadata.size() || metadataBytes > count - 64 - offset ||
      bytes.compare(offset, metadataBytes, expectedMetadata) != 0) {
    throw eckit::BadValue("MPAS Increment envelope belongs to a different typed space/time",
                          Here());
  }
  offset += metadataBytes;
  const auto record = Json::parse(expectedMetadata);
  MpasAnalysisArrays values;
  for (const auto &[name, shape] : record.at("arrays").items()) {
    MpasAnalysisArray array{shape.get<std::vector<size_t>>(), {}};
    size_t elements = 1;
    for (size_t extent : array.shape) {
      elements *= extent;
    }
    array.values.reserve(elements);
    for (size_t element = 0; element < elements; ++element) {
      const auto bits = readInteger();
      double value;
      std::memcpy(&value, &bits, sizeof(value));
      array.values.push_back(value);
    }
    values.emplace(name, std::move(array));
  }
  if (offset != count - 64) {
    throw eckit::BadValue("MPAS Increment has trailing binary payload", Here());
  }
  setArrays(values);
  index = cursor + words;
}
}  // namespace ijedi
