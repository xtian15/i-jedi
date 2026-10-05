/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/State/StateBackend.h"

#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <set>

#include <nlohmann/json.hpp>

#include "atlas/array.h"
#include "eckit/exception/Exceptions.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Increment/MpasIncrementBackend.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/State/MpasFieldSetOwner.h"
#include "oops/generic/GlobalInterpolator.h"
#include "oops/util/FieldSetHelpers.h"
#include "oops/util/FieldSetOperations.h"

namespace ijedi {
namespace {

atlas::FieldSet allocateFields(const Geometry &geom, const oops::Variables &vars) {
  return util::createFieldSet(geom.functionSpace(), geom.variableSizes(vars), vars.variables());
}

atlas::FieldSet selectFields(const atlas::FieldSet &source, const oops::Variables &vars) {
  atlas::FieldSet selected;
  for (const auto &var : vars) {
    if (!source.has(var.name())) {
      throw eckit::BadValue("State variable '" + var.name() + "' is absent from source backend",
                            Here());
    }
    selected.add(source.field(var.name()));
  }
  return selected;
}

void shareFields(const atlas::FieldSet &source, atlas::FieldSet &target) {
  target.clear();
  for (const auto &field : source) {
    target.add(field);
  }
}

std::string dateTimeString(const util::DateTime &time) {
  std::ostringstream stream;
  stream << time;
  return stream.str();
}

void requireCompatibleTypedMetadata(const atlas::Field &field, const std::string &key,
                                    const std::string &expected) {
  if (field.metadata().has(key) && field.metadata().getString(key) != expected) {
    throw eckit::BadParameter("MPAS typed request for '" + field.name() + "' changes " + key +
                                  " from '" + expected + "' to '" +
                                  field.metadata().getString(key) + "'",
                              Here());
  }
}

const atlas::FunctionSpace &typedSpace(const Geometry &geom, const MpasTypedField &source) {
  return source.horizontalLocation == "edge" ? geom.mpasAtlasGeometry().edgeNormals()
                                             : geom.mpasAtlasGeometry().cellNodes();
}

void checkTypedField(const Geometry &geom, const atlas::Field &field, const MpasTypedField &source,
                     bool requireMetadata) {
  if (field.name() != source.name || field.datatype() != atlas::array::make_datatype<double>() ||
      field.rank() != source.shape.size() ||
      field.functionspace().get() != typedSpace(geom, source).get()) {
    throw eckit::BadValue("MPAS State view has a wrong name, dtype, rank or source space", Here());
  }
  for (size_t dimension = 0; dimension < source.shape.size(); ++dimension) {
    if (field.shape(dimension) != source.shape[dimension]) {
      throw eckit::BadValue("MPAS State view has a wrong stagger/tracer extent", Here());
    }
  }
  if (field.levels() != source.levels ||
      field.variables() != (source.shape.size() == 3 ? source.shape[2] : 0)) {
    throw eckit::BadValue("MPAS State view has a wrong Atlas level/component count", Here());
  }
  const std::map<std::string, std::string> expected{
      {"semantic_id", source.semanticId},
      {"units", source.units},
      {"horizontal_location", source.horizontalLocation},
      {"vertical_stagger", source.verticalStagger},
      {"component_basis", source.componentBasis},
      {"trajectory_receipt", source.trajectoryReceipt},
      {"horizontal_geometry_receipt", source.horizontalGeometryReceipt},
      {"mpas_descriptor_binding", source.descriptorBinding}};
  for (const auto &[key, value] : expected) {
    if (requireMetadata && !field.metadata().has(key)) {
      throw eckit::BadValue("MPAS State view lacks required semantic/owner metadata", Here());
    }
    requireCompatibleTypedMetadata(field, key, value);
  }
}

MpasAnalysisArray readTypedField(const Geometry &geom, const atlas::Field &field,
                                 const MpasTypedField &source) {
  MpasAnalysisArray result{source.shape, {}};
  if (source.verticalStagger == "surface") {
    result.shape.pop_back();
  }
  result.values.resize(field.size());
  const size_t stride = field.size() / field.shape(0);
  for (size_t row = 0; row < source.shape[0]; ++row) {
    const auto atlasRow = source.horizontalLocation == "edge"
                              ? geom.mpasAtlasGeometry().nativeToAtlasEdges().at(row)
                              : row;
    for (size_t column = 0; column < stride; ++column) {
      const double value = field.rank() == 2
                               ? atlas::array::make_view<double, 2>(field)(atlasRow, column)
                               : atlas::array::make_view<double, 3>(field)(
                                     atlasRow, column / source.shape[2], column % source.shape[2]);
      if (!std::isfinite(value)) {
        throw eckit::BadValue("nonfinite MPAS State view", Here());
      }
      result.values[row * stride + column] = value;
    }
  }
  return result;
}

std::string payloadHash(const MpasAnalysisArray &array) {
  AtlasOperatorReceipt hash;
  for (double value : array.values) {
    hash.real(value);
  }
  return hash.finish();
}

void fillTypedField(const Geometry &geom, atlas::Field field, const MpasTypedField &source) {
  const size_t stride = source.values.size() / source.shape[0];
  for (size_t row = 0; row < source.shape[0]; ++row) {
    const auto atlasRow = source.horizontalLocation == "edge"
                              ? geom.mpasAtlasGeometry().nativeToAtlasEdges().at(row)
                              : row;
    for (size_t column = 0; column < stride; ++column) {
      const double value = source.values[row * stride + column];
      if (field.rank() == 2) {
        atlas::array::make_view<double, 2>(field)(atlasRow, column) = value;
      } else {
        atlas::array::make_view<double, 3>(field)(atlasRow, column / source.shape[2],
                                                  column % source.shape[2]) = value;
      }
    }
  }
  field.metadata().set("semantic_id", source.semanticId);
  field.metadata().set("units", source.units);
  field.metadata().set("horizontal_location", source.horizontalLocation);
  field.metadata().set("vertical_stagger", source.verticalStagger);
  field.metadata().set("component_basis", source.componentBasis);
  field.metadata().set("trajectory_receipt", source.trajectoryReceipt);
  field.metadata().set("horizontal_geometry_receipt", source.horizontalGeometryReceipt);
  field.metadata().set("mpas_descriptor_binding", source.descriptorBinding);
  field.metadata().set("mpas_payload_receipt", source.payloadReceipt);
  field.metadata().set("interp_type", "default");
}

atlas::FieldSet allocateTypedFields(const Geometry &geom,
                                    const std::vector<MpasTypedField> &sources) {
  atlas::FieldSet result;
  for (const auto &source : sources) {
    atlas::array::ArrayShape shape;
    for (size_t extent : source.shape) {
      shape.push_back(extent);
    }
    atlas::Field field(source.name, atlas::array::make_datatype<double>(), shape);
    field.set_functionspace(typedSpace(geom, source));
    field.set_levels(source.levels);
    if (source.shape.size() == 3) {
      field.set_variables(source.shape[2]);
    }
    fillTypedField(geom, field, source);
    result.add(field);
  }
  return ownMpasFieldSet(result);
}

}  // namespace

void StateBackend::advanceModel(const util::Duration &) {
  throw eckit::NotImplemented("This State backend has no nonlinear model step", Here());
}

std::string StateBackend::regressionManifest() const {
  throw eckit::NotImplemented("This State backend has no MPAS regression manifest", Here());
}

std::string StateBackend::continuationManifest() const {
  throw eckit::NotImplemented("This State backend has no MPAS continuation manifest", Here());
}

std::string StateBackend::typedTransformManifest() const {
  throw eckit::NotImplemented("This State backend has no typed transform manifest", Here());
}

std::vector<MpasTypedField> StateBackend::materializeTypedFields(const oops::Variables &) const {
  throw eckit::NotImplemented("This State backend has no MPAS typed field view", Here());
}

AtlasStateBackend::AtlasStateBackend(const Geometry &geom, const oops::Variables &vars,
                                     const util::DateTime &time, bool initToZero)
    : geom_(geom), fields_(time, geom.comm()) {
  fields_.deepCopy(allocateFields(geom, vars));
  if (initToZero) {
    fields_.zero();
  }
}

AtlasStateBackend::AtlasStateBackend(const Geometry &geom, const AtlasStateBackend &other)
    : geom_(geom), fields_(other.validTime(), geom.comm()) {
  const std::string targetGridUid = util::getGridUid(geom.functionSpace());
  const std::string sourceGridUid = other.fields_.getGridUid();
  if (targetGridUid == sourceGridUid) {
    fields_.deepCopy(other.fields_);
    return;
  }

  // Preserve the pre-backend State contract for existing Atlas/FV3/MOM6
  // workflows: State(targetGeometry, sourceState) performs the established
  // OOPS horizontal interpolation.  MPAS never enters this backend and has a
  // separate receipt-checked clone contract.
  eckit::LocalConfiguration interpolationConfig;
  interpolationConfig.set("local interpolator type", "oops unstructured grid interpolator");
  const oops::GlobalInterpolator interpolator(interpolationConfig, other.geom_.geometryData(),
                                              geom.functionSpace(), geom.comm());
  atlas::FieldSet interpolated;
  interpolator.apply(other.fieldSet(), interpolated);
  fields_.deepCopy(interpolated);
}

AtlasStateBackend::AtlasStateBackend(const oops::Variables &vars, const AtlasStateBackend &other)
    : geom_(other.geom_), fields_(other.validTime(), other.geom_.comm()) {
  fields_.deepCopy(selectFields(other.fieldSet(), vars));
}

AtlasStateBackend::AtlasStateBackend(const AtlasStateBackend &other)
    : geom_(other.geom_), fields_(other.fields_) {}

std::unique_ptr<StateBackend> AtlasStateBackend::clone() const {
  return std::make_unique<AtlasStateBackend>(*this);
}

std::unique_ptr<StateBackend> AtlasStateBackend::clone(const oops::Variables &vars) const {
  return std::make_unique<AtlasStateBackend>(vars, *this);
}

std::unique_ptr<StateBackend> AtlasStateBackend::clone(const Geometry &geom) const {
  return std::make_unique<AtlasStateBackend>(geom, *this);
}

const AtlasStateBackend &AtlasStateBackend::checked(const StateBackend &other) {
  const auto *atlasBackend = dynamic_cast<const AtlasStateBackend *>(&other);
  if (atlasBackend == nullptr) {
    throw eckit::BadParameter("State backend types do not match", Here());
  }
  return *atlasBackend;
}

void AtlasStateBackend::assign(const StateBackend &other) {
  const AtlasStateBackend &rhs = checked(other);
  if (&geom_ != &rhs.geom_ && fields_.getGridUid() != rhs.fields_.getGridUid()) {
    throw eckit::BadParameter("Cannot assign Atlas states on different grids", Here());
  }
  fields_.validTime() = rhs.fields_.validTime();
  fields_.deepCopy(rhs.fields_);
}

const util::DateTime AtlasStateBackend::validTime() const { return fields_.validTime(); }

void AtlasStateBackend::updateTime(const util::Duration &dt) { fields_.validTime() += dt; }

const oops::Variables &AtlasStateBackend::variables() const { return fields_.variables(); }

void AtlasStateBackend::zero() { fields_.zero(); }

void AtlasStateBackend::accumul(double weight, const StateBackend &other) {
  const AtlasStateBackend &rhs = checked(other);
  oops::FieldSet3D scaled(rhs.fields_);
  scaled *= weight;
  fields_ += scaled;
}

double AtlasStateBackend::norm() const { return fields_.norm(fields_.variables()); }

atlas::FieldSet &AtlasStateBackend::fieldSet() { return fields_.fieldSet(); }

const atlas::FieldSet &AtlasStateBackend::fieldSet() const { return fields_.fieldSet(); }

void AtlasStateBackend::toFieldSet(atlas::FieldSet &target) const {
  shareFields(fields_.fieldSet(), target);
}

void AtlasStateBackend::fromFieldSet(const atlas::FieldSet &source) {
  if (source.empty()) {
    throw eckit::BadParameter("Cannot import an empty State FieldSet", Here());
  }
  fields_.deepCopy(source);
}

size_t AtlasStateBackend::serialSize() const { return fields_.serialSize(); }

void AtlasStateBackend::serialize(std::vector<double> &buffer) const { fields_.serialize(buffer); }

void AtlasStateBackend::deserialize(const std::vector<double> &buffer, size_t &index) {
  size_t timeIndex = index;
  util::DateTime serializedTime;
  serializedTime.deserialize(buffer, timeIndex);
  fields_.validTime() = serializedTime;
  fields_.deserialize(buffer, index);
}

MpasStateBackend::MpasStateBackend(const Geometry &geom, const oops::Variables &variables,
                                   const util::DateTime &time)
    : geom_(geom),
      context_(geom.mpasContext()),
      variables_(variables),
      time_(time) {
  if (!geom.isMpas()) {
    throw eckit::BadParameter("MpasStateBackend requires an MPAS geometry", Here());
  }
  if (time != util::DateTime(context_->initialValidTime())) {
    throw eckit::BadParameter(
        "MPAS initial State date differs from the receipt-bound model start time", Here());
  }
  if (variables_.size() == 0) {
    throw eckit::BadParameter("MPAS State requires an explicit nonempty variable inventory",
                              Here());
  }
  (void)geom_.variableSizes(variables_);
  state_ = context_->initialState();
  // Resolve semantics through the owner, including requested read-only views.
  (void)stateFields(variables_);
}

MpasStateBackend::MpasStateBackend(const MpasStateBackend &other)
    : geom_(other.geom_),
      context_(other.context_),
      state_(context_->clone(*other.state_)),
      variables_(other.variables_),
      time_(other.time_) {}

MpasStateBackend::MpasStateBackend(const Geometry &geom, const MpasStateBackend &other)
    : geom_(geom), context_(geom.mpasContext()), variables_(other.variables_), time_(other.time_) {
  if (!geom.isMpas() || context_->geometryReceipt() != other.context_->geometryReceipt() ||
      context_->configurationReceipt() != other.context_->configurationReceipt()) {
    throw eckit::BadParameter(
        "MPAS State clone target has a different geometry/configuration bundle", Here());
  }
  const std::uint64_t generation = other.context_->stateGeneration(*other.state_);
  if (context_ == other.context_) {
    state_ = context_->clone(*other.state_);
  } else {
    const std::string envelope =
        other.context_->serializeState(*other.state_, dateTimeString(other.time_));
    state_ = context_->deserializeState(envelope, dateTimeString(other.time_), generation);
  }
}

MpasStateBackend::~MpasStateBackend() = default;

MpasAnalysisArrays StateBackend::nativeAnalysisValues() const {
  throw eckit::BadParameter("native MPAS analysis requires an MPAS State backend", Here());
}
MpasAnalysisArrays StateBackend::controlToNative(const MpasAnalysisArrays &) const {
  throw eckit::BadParameter("MPAS control mapping requires an MPAS State backend", Here());
}
MpasAnalysisArrays StateBackend::nativeCovectorsToControl(const MpasAnalysisArrays &) const {
  throw eckit::BadParameter("MPAS covector mapping requires an MPAS State backend", Here());
}
MpasAnalysisArrays StateBackend::nativeGeovalJvp(const MpasAnalysisArrays &,
                                                 const oops::Variables &) const {
  throw eckit::BadParameter("MPAS JVP requires an MPAS State backend", Here());
}
MpasAnalysisArrays StateBackend::nativeGeovalVjp(const MpasAnalysisArrays &,
                                                 const oops::Variables &) const {
  throw eckit::BadParameter("MPAS VJP requires an MPAS State backend", Here());
}
void StateBackend::addNativeAnalysis(const MpasAnalysisArrays &) {
  throw eckit::BadParameter("MPAS analysis updates require an MPAS State backend", Here());
}
MpasAnalysisArrays MpasStateBackend::nativeAnalysisValues() const {
  return context_->nativeAnalysisValues(*state_);
}
MpasAnalysisArrays MpasStateBackend::controlToNative(const MpasAnalysisArrays &x) const {
  return context_->controlToNative(*state_, x);
}
MpasAnalysisArrays MpasStateBackend::nativeCovectorsToControl(const MpasAnalysisArrays &x) const {
  return context_->nativeCovectorsToControl(*state_, x);
}
MpasAnalysisArrays MpasStateBackend::nativeGeovalJvp(const MpasAnalysisArrays &x,
                                                     const oops::Variables &vars) const {
  (void)geom_.variableSizes(vars);
  return context_->nativeGeovalJvp(*state_, x, vars.variables(), dateTimeString(time_));
}
MpasAnalysisArrays MpasStateBackend::nativeGeovalVjp(const MpasAnalysisArrays &x,
                                                     const oops::Variables &vars) const {
  (void)geom_.variableSizes(vars);
  return context_->nativeGeovalVjp(*state_, x, vars.variables(), dateTimeString(time_));
}
void MpasStateBackend::addNativeAnalysis(const MpasAnalysisArrays &x) {
  context_->addNativeAnalysis(*state_, x);
  invalidateViews();
}

std::unique_ptr<StateBackend> MpasStateBackend::clone() const {
  return std::make_unique<MpasStateBackend>(*this);
}

void MpasStateBackend::requireSameVariables(const oops::Variables &left,
                                            const oops::Variables &right) {
  if (left.variables() != right.variables()) {
    throw eckit::BadParameter(
        "MPAS State variable subsetting is not a valid native-state operation", Here());
  }
}

std::unique_ptr<StateBackend> MpasStateBackend::clone(const oops::Variables &variables) const {
  (void)stateFields(variables);
  auto result = std::make_unique<MpasStateBackend>(*this);
  result->variables_ = variables;
  return result;
}

std::unique_ptr<StateBackend> MpasStateBackend::clone(const Geometry &geometry) const {
  if (!geometry.isMpas() ||
      geometry.mpasContext()->geometryReceipt() != context_->geometryReceipt()) {
    throw eckit::BadParameter(
        "MPAS State clone target has a different geometry/configuration bundle; "
        "resolution change requires a receipt-bound interpolation operator",
        Here());
  }
  return std::make_unique<MpasStateBackend>(geometry, *this);
}

const MpasStateBackend &MpasStateBackend::checked(const StateBackend &other) {
  const auto *mpas = dynamic_cast<const MpasStateBackend *>(&other);
  if (mpas == nullptr) {
    throw eckit::BadParameter("State backend types do not match", Here());
  }
  return *mpas;
}

void MpasStateBackend::assign(const StateBackend &other) {
  const MpasStateBackend &rhs = checked(other);
  if (context_->geometryReceipt() != rhs.context_->geometryReceipt() ||
      context_->configurationReceipt() != rhs.context_->configurationReceipt()) {
    throw eckit::BadParameter(
        "Cannot assign MPAS states with different geometry/configuration bundles", Here());
  }
  if (context_ == rhs.context_) {
    state_ = context_->clone(*rhs.state_);
  } else {
    // A handle is meaningful only under the context that created it.  Crossing
    // contexts through clone() would bypass the sealed envelope's package and
    // schema checks, even when the geometry receipts happen to agree.
    const std::uint64_t generation = rhs.context_->stateGeneration(*rhs.state_);
    const std::string envelope =
        rhs.context_->serializeState(*rhs.state_, dateTimeString(rhs.time_));
    state_ = context_->deserializeState(envelope, dateTimeString(rhs.time_), generation);
  }
  time_ = rhs.time_;
  variables_ = rhs.variables_;
  invalidateViews();
}

const util::DateTime MpasStateBackend::validTime() const { return time_; }
void MpasStateBackend::updateTime(const util::Duration &duration) {
  if (duration.toSeconds() != 0) {
    throw eckit::BadParameter(
        "MPAS State time advances only with the model or an authenticated restore", Here());
  }
  time_ += duration;
  invalidateViews();
}
const oops::Variables &MpasStateBackend::variables() const { return variables_; }

void MpasStateBackend::zero() {
  throw eckit::NotImplemented("MPAS native-state zeroing requires the typed analysis control transform",
                              Here());
}

void MpasStateBackend::accumul(double, const StateBackend &) {
  throw eckit::NotImplemented(
      "MPAS native-state accumulation requires the typed analysis control transform", Here());
}

double MpasStateBackend::norm() const {
  long double total = 0.;
  for (const auto &field : stateFields(variables_)) {
    for (double value : field.values) {
      total += static_cast<long double>(value) * value;
    }
  }
  return static_cast<double>(std::sqrt(total));
}

void MpasStateBackend::advanceModel(const util::Duration &duration) {
  const double seconds = static_cast<double>(duration.toSeconds());
  context_->advance(*state_, seconds);
  time_ += duration;
  invalidateViews();
}

std::string MpasStateBackend::regressionManifest() const {
  return context_->regressionManifest(*state_);
}

std::string MpasStateBackend::continuationManifest() const {
  return context_->continuationManifest(*state_);
}

std::string MpasStateBackend::typedTransformManifest() const {
  return context_->typedTransformManifest(*state_, dateTimeString(time_));
}

std::vector<MpasTypedField> MpasStateBackend::materializeTypedFields(
    const oops::Variables &variables) const {
  (void)geom_.variableSizes(variables);
  return context_->materializeTypedFields(*state_, variables.variables(), dateTimeString(time_));
}

atlas::FieldSet &MpasStateBackend::fieldSet() {
  (void)static_cast<const MpasStateBackend &>(*this).fieldSet();
  return views_;
}
const atlas::FieldSet &MpasStateBackend::fieldSet() const {
  if (viewDescriptors_.empty()) {
    auto descriptors = stateFields(variables_);
    auto fields = allocateTypedFields(geom_, descriptors);
    for (auto &descriptor : descriptors) {
      std::vector<double>().swap(descriptor.values);
    }
    views_ = std::move(fields);
    viewDescriptors_ = std::move(descriptors);
  }
  validateViews();
  return views_;
}
void MpasStateBackend::invalidateViews() {
  views_.clear();
  viewDescriptors_.clear();
}
std::vector<MpasTypedField> MpasStateBackend::stateFields(const oops::Variables &variables) const {
  (void)geom_.variableSizes(variables);
  geom_.mpasAtlasGeometry().validateStorage();
  return context_->materializeTypedFields(*state_, variables.variables(), dateTimeString(time_),
                                          true);
}
void MpasStateBackend::validateViews() const {
  geom_.mpasAtlasGeometry().validateStorage();
  if (views_.size() != viewDescriptors_.size()) {
    throw eckit::BadValue("cached MPAS State view inventory changed", Here());
  }
  for (const auto &source : viewDescriptors_) {
    const auto field = views_.field(source.name);
    checkTypedField(geom_, field, source, true);
    if (field.metadata().getString("mpas_payload_receipt") != source.payloadReceipt ||
        payloadHash(readTypedField(geom_, field, source)) != source.payloadReceipt) {
      throw eckit::BadValue("cached MPAS State view was edited without an explicit native write",
                            Here());
    }
  }
}
void MpasStateBackend::toFieldSet(atlas::FieldSet &target) const {
  validateViews();
  if (target.empty()) {
    target = allocateTypedFields(geom_, stateFields(variables_));
    return;
  }
  std::vector<oops::Variable> requested;
  requested.reserve(target.size());
  for (const atlas::Field &field : target) {
    requested.emplace_back(field.name());
  }
  const oops::Variables variables(requested);
  const std::vector<MpasTypedField> materialized = stateFields(variables);
  if (materialized.size() != target.size()) {
    throw eckit::BadValue("MPAS typed transform changed the requested field count", Here());
  }
  // Preflight the complete typed request before writing any destination value.
  // Missing metadata is populated below for ordinary OOPS name-only requests;
  // supplied metadata is a semantic constraint and may never be overwritten.
  for (const MpasTypedField &source : materialized) {
    atlas::Field field = target.field(source.name);
    checkTypedField(geom_, field, source, false);
  }
  for (const MpasTypedField &source : materialized) {
    atlas::Field field = target.field(source.name);
    fillTypedField(geom_, field, source);
  }
}
void MpasStateBackend::fromFieldSet(const atlas::FieldSet &source) {
  if (source.empty()) {
    throw eckit::BadValue("empty MPAS State write", Here());
  }
  std::vector<std::string> names;
  bool control = true;
  for (const auto &field : source) {
    names.push_back(field.name());
    control = control && field.name().rfind("control_", 0) == 0;
  }
  const oops::Variables requested(names);
  if (control) {
    // These are explicitly bound increments, never an absolute control State
    // or an inverse diagnostic reconstruction.
    MpasIncrementBackend increment(geom_, requested, time_);
    increment.fromFieldSet(source);
    addNativeAnalysis(controlToNative(increment.completeArrays()));
    return;
  }
  const auto descriptors = stateFields(requested);
  auto native = nativeAnalysisValues();
  bool groupedTracers = false, individualTracers = false;
  std::set<std::pair<std::string, int>> destinations;
  for (const auto &descriptor : descriptors) {
    if (descriptor.nameSpace != "native") {
      throw eckit::BadValue("diagnostic/static/GeoVaL State writes are read-only", Here());
    }
    const auto field = source.field(descriptor.name);
    checkTypedField(geom_, field, descriptor, true);
    const auto array = readTypedField(geom_, field, descriptor);
    if (field.metadata().getString("mpas_payload_receipt") != payloadHash(array)) {
      throw eckit::BadValue("MPAS native State write payload receipt differs from actual bytes",
                            Here());
    }
    const auto binding = nlohmann::json::parse(descriptor.descriptorBinding);
    const auto &semantic = binding.at("descriptor");
    const auto raw = semantic.at("native_name").get<std::string>();
    const int destinationSlot =
        semantic.at("tracer_index").is_null() ? -1 : semantic.at("tracer_index").get<int>();
    if (!destinations.emplace(raw, destinationSlot).second) {
      throw eckit::BadValue("aliases overlap in an MPAS native State write", Here());
    }
    if (semantic.at("tracer_index").is_null()) {
      if (raw == "scalars") {
        groupedTracers = true;
      }
      native.at(raw) = array;
    } else {
      individualTracers = true;
      const size_t slot = semantic.at("tracer_index").get<size_t>();
      auto &tracers = native.at("scalars");
      for (size_t i = 0; i < array.values.size(); ++i) {
        tracers.values.at(i * tracers.shape.back() + slot) = array.values[i];
      }
    }
  }
  if (groupedTracers && individualTracers) {
    throw eckit::BadValue("overlapping grouped and individual tracer State writes", Here());
  }
  context_->replaceNativeAnalysis(*state_, native);
  invalidateViews();
}
size_t MpasStateBackend::serialSize() const {
  const std::string bytes = context_->serializeState(*state_, dateTimeString(time_));
  return time_.serialSize() + 3 + (bytes.size() + sizeof(double) - 1) / sizeof(double);
}
void MpasStateBackend::serialize(std::vector<double> &buffer) const {
  constexpr double magic = 9042027.0;
  const std::string bytes = context_->serializeState(*state_, dateTimeString(time_));
  const std::uint64_t generation = context_->stateGeneration(*state_);
  if (bytes.size() > (1ULL << 53)) {
    throw eckit::BadValue("MPAS serialized State exceeds exact double integer range", Here());
  }
  if (generation > (1ULL << 53)) {
    throw eckit::BadValue("MPAS State generation exceeds exact double integer range", Here());
  }
  time_.serialize(buffer);
  buffer.push_back(magic);
  buffer.push_back(static_cast<double>(generation));
  buffer.push_back(static_cast<double>(bytes.size()));
  const size_t words = (bytes.size() + sizeof(double) - 1) / sizeof(double);
  const size_t start = buffer.size();
  buffer.resize(start + words, 0.0);
  std::memcpy(static_cast<void *>(buffer.data() + start), bytes.data(), bytes.size());
}
void MpasStateBackend::deserialize(const std::vector<double> &buffer, size_t &index) {
  constexpr double magic = 9042027.0;
  // Stage both the caller's cursor and the time. A rejected envelope must not
  // change either the destination State or the caller's framing position.
  size_t cursor = index;
  util::DateTime restoredTime = time_;
  if (cursor > buffer.size() || restoredTime.serialSize() > buffer.size() - cursor) {
    throw eckit::BadValue("MPAS serialized State has a truncated time", Here());
  }
  restoredTime.deserialize(buffer, cursor);
  if (cursor > buffer.size() || buffer.size() - cursor < 3 || buffer[cursor++] != magic) {
    throw eckit::BadValue("MPAS serialized State has an invalid framing marker", Here());
  }
  const double encodedGeneration = buffer[cursor++];
  if (!std::isfinite(encodedGeneration) || encodedGeneration < 0.0 ||
      std::floor(encodedGeneration) != encodedGeneration || encodedGeneration > (1ULL << 53)) {
    throw eckit::BadValue("MPAS serialized State has an invalid generation", Here());
  }
  const std::uint64_t generation = static_cast<std::uint64_t>(encodedGeneration);
  const double encodedLength = buffer[cursor++];
  if (!std::isfinite(encodedLength) || encodedLength < 0.0 ||
      std::floor(encodedLength) != encodedLength || encodedLength > (1ULL << 53)) {
    throw eckit::BadValue("MPAS serialized State has an invalid byte length", Here());
  }
  const size_t byteLength = static_cast<size_t>(encodedLength);
  const size_t words = byteLength / sizeof(double) + (byteLength % sizeof(double) != 0);
  if (words > buffer.size() - cursor) {
    throw eckit::BadValue("MPAS serialized State is truncated", Here());
  }
  std::string bytes(byteLength, '\0');
  std::memcpy(bytes.data(), static_cast<const void *>(buffer.data() + cursor), byteLength);
  cursor += words;
  auto restored = context_->deserializeState(bytes, dateTimeString(restoredTime), generation);
  state_ = std::move(restored);
  time_ = restoredTime;
  invalidateViews();
  index = cursor;
}

}  // namespace ijedi
