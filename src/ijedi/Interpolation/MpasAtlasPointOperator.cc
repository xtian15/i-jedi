/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/MpasAtlasPointOperator.h"

#include <openssl/evp.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/functionspace/PointCloud.h"
#include "atlas/interpolation/Cache.h"
#include "atlas/interpolation/Interpolation.h"
#include "atlas/interpolation/method/sphericalvector/SphericalVector.h"
#include "atlas/library/Library.h"
#include "atlas/linalg/sparse/MakeEckitSparseMatrix.h"
#include "atlas/linalg/sparse/MakeSparseMatrixStorageEckit.h"
#include "atlas/option.h"
#include "atlas/util/Config.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Interpolation/AtlasMeshIntegrity.h"

namespace ijedi {
namespace {
using Json = nlohmann::json;

using Digest = AtlasOperatorReceipt;

atlas::util::Config scalarConfig() {
  atlas::util::Config config("type", "finite-element");
  config.set("adjoint", true);
  config.set("treat_failure_as_missing_value", false);
  config.set("max_fraction_elems_to_try", 1.);
  return config;
}

void checkValues(const std::vector<double> &values, size_t rows, size_t levels) {
  if (!levels || rows > std::numeric_limits<size_t>::max() / levels ||
      values.size() != rows * levels) {
    throw std::invalid_argument("Atlas MPAS field has incorrect horizontal/level extent");
  }
  for (double value : values) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("Atlas MPAS field contains nonfinite values");
    }
  }
}

}  // namespace

struct MpasAtlasPointOperator::Impl {
  atlas::FunctionSpace source, target;
  atlas::interpolation::MatrixCache matrix;
  std::unique_ptr<atlas::Interpolation> scalar, vector;
  std::string horizontal, key, receipt;
  Json payload;
  const std::vector<double> sourceMeasures;
  const std::shared_ptr<const AtlasMeshIntegrity> integrity;

  Impl(const MpasAtlasGeometry &geometry, const std::vector<double> &lat,
       const std::vector<double> &lon, const std::string &compiler,
       const std::string *serialized = nullptr, const std::string *trusted = nullptr)
      : source(geometry.cellNodes()),
        horizontal(geometry.receipt()),
        sourceMeasures(geometry.cellNodeMeasures()),
        integrity(geometry.storageIntegrity()) {
    integrity->validate();
    if (lat.size() != lon.size() || compiler.empty() || lat.empty()) {
      throw std::invalid_argument(
          "Atlas MPAS point operator requires ordered nonempty targets and compiler identity");
    }
    atlas::Field coordinates("lonlat", atlas::array::make_datatype<double>(),
                             atlas::array::make_shape(lat.size(), 2));
    auto ll = atlas::array::make_view<double, 2>(coordinates);
    for (size_t i = 0; i < lat.size(); ++i) {
      if (!std::isfinite(lat[i]) || !std::isfinite(lon[i]) || std::abs(lat[i]) > 90.) {
        throw std::invalid_argument("Atlas MPAS target coordinates are invalid");
      }
      ll(i, 0) = lon[i];
      ll(i, 1) = lat[i];
    }
    key = MpasAtlasPointOperator::keyFor(geometry, lat, lon, compiler);
    target = atlas::functionspace::PointCloud(coordinates);
    if (serialized) {
      if (serialized->size() > 256 * 1024 * 1024) {
        throw std::invalid_argument("Atlas cache payload exceeds bounded capacity");
      }
      payload = Json::parse(*serialized);
      const std::set<std::string> required{"schema",  "key",     "rows",
                                           "columns", "entries", "vector_entries"};
      std::set<std::string> found;
      for (auto entry = payload.begin(); entry != payload.end(); ++entry) {
        found.insert(entry.key());
      }
      if (found != required || payload.at("schema") != "atlas-point-cache-v3" ||
          payload.at("key") != key || payload.at("rows") != target.size() ||
          payload.at("columns") != source.size() || !payload.at("entries").is_array() ||
          payload.at("entries").empty() || payload.at("entries").size() > 3 * lat.size()) {
        throw std::invalid_argument("Atlas cache geometry/schema/dimensions mismatch");
      }
      // Verify the received entry order and binary64 bytes before allocating or
      // passing them to Atlas. The expected receipt never comes from payload.
      receipt = payloadReceipt(payload);
      if (!trusted || receipt != *trusted) {
        throw std::invalid_argument("Atlas cache trusted coefficient receipt mismatch");
      }
      std::vector<eckit::linalg::Triplet> entries;
      for (const auto &entry : payload.at("entries")) {
        entries.emplace_back(entry.at(0).get<size_t>(), entry.at(1).get<size_t>(),
                             entry.at(2).get<double>());
      }
      eckit::linalg::SparseMatrix decoded(target.size(), source.size(), entries);
      auto owned = std::make_shared<const atlas::linalg::SparseMatrixStorage>(
          atlas::linalg::make_sparse_matrix_storage(std::move(decoded)));
      matrix = atlas::interpolation::MatrixCache(owned, key);
      if (matrixPayload(matrix).at("entries") != payload.at("entries")) {
        throw std::invalid_argument("Atlas cache hydration changed coefficient order or bytes");
      }
      scalar = std::make_unique<atlas::Interpolation>(scalarConfig(), source, target, matrix);
    } else {
      scalar = std::make_unique<atlas::Interpolation>(scalarConfig(), source, target);
      matrix = atlas::interpolation::MatrixCache(*scalar);
      payload = matrixPayload(matrix);
    }
    atlas::util::Config vectorConfig("type", "spherical-vector");
    vectorConfig.set("scheme", scalarConfig());
    vectorConfig.set("adjoint", true);
    vector = std::make_unique<atlas::Interpolation>(vectorConfig, source, target, matrix);
    const auto *method =
        dynamic_cast<const atlas::interpolation::method::SphericalVector *>(vector->get());
    if (!method) {
      throw std::runtime_error("Atlas did not construct the declared spherical-vector method");
    }
    const auto &effective = method->vectorMatrix();
    Json coefficients = Json::array();
    for (int row = 0; row < effective.rows(); ++row) {
      for (auto entry = effective.rowIter(row); entry; ++entry) {
        coefficients.push_back(
            {entry.row(), entry.col(), entry.value().real(), entry.value().imag()});
      }
    }
    if (serialized) {
      if (coefficients != payload.at("vector_entries")) {
        throw std::invalid_argument(
            "Atlas effective vector coefficients differ from the authenticated cache");
      }
    } else {
      payload["vector_entries"] = std::move(coefficients);
      receipt = payloadReceipt(payload);
    }
  }

  Json matrixPayload(const atlas::interpolation::MatrixCache &cache) const {
    const auto sparse = atlas::linalg::make_non_owning_eckit_sparse_matrix(cache.matrix());
    Json entries = Json::array();
    for (auto entry = sparse.begin(); entry != sparse.end(); ++entry) {
      entries.push_back({entry.row(), entry.col(), *entry});
    }
    return {{"schema", "atlas-point-cache-v3"},
            {"key", key},
            {"rows", sparse.rows()},
            {"columns", sparse.cols()},
            {"entries", entries}};
  }

  std::string payloadReceipt(const Json &document) const {
    Digest hash;
    hash.string("atlas-point-cache-v3");
    hash.string(key);
    hash.integer(source.size());
    hash.integer(target.size());
    hash.integer(document.at("entries").size());
    size_t previousRow = 0;
    bool first = true;
    std::vector<size_t> counts(target.size(), 0);
    std::vector<long double> sums(target.size(), 0);
    std::set<std::pair<size_t, size_t>> occupied;
    for (const auto &entry : document.at("entries")) {
      if (!entry.is_array() || entry.size() != 3 || !entry.at(0).is_number_integer() ||
          !entry.at(1).is_number_integer() || !entry.at(2).is_number() ||
          (!entry.at(0).is_number_unsigned() && entry.at(0).get<std::int64_t>() < 0) ||
          (!entry.at(1).is_number_unsigned() && entry.at(1).get<std::int64_t>() < 0)) {
        throw std::invalid_argument("Atlas cache malformed entry");
      }
      const size_t row = entry.at(0).get<size_t>(), col = entry.at(1).get<size_t>();
      const double value = entry.at(2).get<double>();
      if (row >= target.size() || col >= source.size() || !std::isfinite(value) ||
          (!first && row < previousRow) || !occupied.emplace(row, col).second ||
          ++counts[row] > 3) {
        throw std::invalid_argument("Atlas cache invalid indices/values/support order");
      }
      sums[row] += value;
      hash.integer(row);
      hash.integer(col);
      hash.real(value);
      previousRow = row;
      first = false;
    }
    for (size_t row = 0; row < sums.size(); ++row) {
      if (!counts[row] || std::abs(sums[row] - 1.L) > 1.e-13L) {
        throw std::invalid_argument(
            "Atlas point matrix leaves a target unmapped or fails constant preservation");
      }
    }
    const auto &vectorEntries = document.at("vector_entries");
    if (!vectorEntries.is_array() || vectorEntries.size() != occupied.size()) {
      throw std::invalid_argument("Atlas vector cache coefficient extent mismatch");
    }
    std::set<std::pair<size_t, size_t>> vectorSupport;
    hash.integer(vectorEntries.size());
    for (const auto &entry : vectorEntries) {
      if (!entry.is_array() || entry.size() != 4 || !entry[0].is_number_integer() ||
          !entry[1].is_number_integer() || !entry[2].is_number() || !entry[3].is_number() ||
          entry[0].get<std::int64_t>() < 0 || entry[1].get<std::int64_t>() < 0) {
        throw std::invalid_argument("Atlas vector cache malformed coefficient");
      }
      const size_t row = entry[0].get<size_t>(), col = entry[1].get<size_t>();
      const double real = entry[2].get<double>(), imaginary = entry[3].get<double>();
      if (!occupied.count({row, col}) || !vectorSupport.emplace(row, col).second ||
          !std::isfinite(real) || !std::isfinite(imaginary)) {
        throw std::invalid_argument("Atlas vector cache support or coefficient mismatch");
      }
      hash.integer(row);
      hash.integer(col);
      hash.real(real);
      hash.real(imaginary);
    }
    return hash.finish();
  }

  std::array<std::vector<double>, 2> execute(const std::vector<double> &east,
                                             const std::vector<double> *north, size_t levels,
                                             bool transpose) const {
    const auto &inputSpace = transpose ? target : source;
    const auto &outputSpace = transpose ? source : target;
    checkValues(east, inputSpace.size(), levels);
    if (north) {
      checkValues(*north, inputSpace.size(), levels);
    }
    auto input = inputSpace.createField<double>(atlas::option::levels(levels) |
                                                atlas::option::variables(north ? 2 : 0));
    auto output = outputSpace.createField<double>(atlas::option::levels(levels) |
                                                  atlas::option::variables(north ? 2 : 0));
    if (north) {
      input.metadata().set("type", "vector");
      output.metadata().set("type", "vector");
      auto iv = atlas::array::make_view<double, 3>(input);
      auto ov = atlas::array::make_view<double, 3>(output);
      ov.assign(0);
      for (size_t i = 0; i < east.size(); ++i) {
        iv(i / levels, i % levels, 0) = east[i];
        iv(i / levels, i % levels, 1) = (*north)[i];
      }
      if (transpose) {
        vector->execute_adjoint(output, input);
      } else {
        vector->execute(input, output);
      }
    } else {
      auto iv = atlas::array::make_view<double, 2>(input);
      auto ov = atlas::array::make_view<double, 2>(output);
      ov.assign(0);
      for (size_t i = 0; i < east.size(); ++i) {
        iv(i / levels, i % levels) = east[i];
      }
      if (transpose) {
        scalar->execute_adjoint(output, input);
      } else {
        scalar->execute(input, output);
      }
    }
    std::array<std::vector<double>, 2> result;
    result[0].resize(outputSpace.size() * levels);
    if (north) {
      result[1].resize(result[0].size());
      const auto ov = atlas::array::make_view<double, 3>(output);
      for (size_t i = 0; i < result[0].size(); ++i) {
        result[0][i] = ov(i / levels, i % levels, 0);
        result[1][i] = ov(i / levels, i % levels, 1);
      }
      checkValues(result[1], outputSpace.size(), levels);
    } else {
      const auto ov = atlas::array::make_view<double, 2>(output);
      for (size_t i = 0; i < result[0].size(); ++i) {
        result[0][i] = ov(i / levels, i % levels);
      }
    }
    checkValues(result[0], outputSpace.size(), levels);
    return result;
  }
};

MpasAtlasPointOperator::MpasAtlasPointOperator(const MpasAtlasGeometry &g,
                                               const std::vector<double> &lat,
                                               const std::vector<double> &lon,
                                               const std::string &compiler)
    : impl_(std::make_unique<Impl>(g, lat, lon, compiler)) {}
MpasAtlasPointOperator::MpasAtlasPointOperator(
    const MpasAtlasGeometry &g, const std::vector<double> &lat, const std::vector<double> &lon,
    const std::string &compiler, const std::string &payload, const std::string &trusted)
    : impl_(std::make_unique<Impl>(g, lat, lon, compiler, &payload, &trusted)) {}
MpasAtlasPointOperator::~MpasAtlasPointOperator() = default;
std::string MpasAtlasPointOperator::keyFor(const MpasAtlasGeometry &geometry,
                                           const std::vector<double> &lat,
                                           const std::vector<double> &lon,
                                           const std::string &compiler) {
  if (lat.empty() || lat.size() != lon.size() || compiler.empty()) {
    throw std::invalid_argument(
        "Atlas point cache requires ordered targets and a compiler identity");
  }
  const std::string loadedCompiler = atlas::Library::instance().gitsha1(40);
  if (compiler.size() != 40 || loadedCompiler != compiler) {
    throw std::invalid_argument(
        "Atlas compiler identity does not match the loaded library revision");
  }
  Digest identity;
  identity.string("ijedi-atlas-mpas-dual-finite-element-vector-cell-area-obs-identity-v3");
  identity.string(geometry.receipt());
  identity.string(compiler);
  identity.string(geometry.atlasInputReceipt());
  identity.string(geometry.cellNodeMeasureReceipt());
  identity.integer(geometry.cellNodes().size());
  identity.integer(lat.size());
  for (size_t i = 0; i < lat.size(); ++i) {
    if (!std::isfinite(lon[i]) || !std::isfinite(lat[i]) || std::abs(lat[i]) > 90.) {
      throw std::invalid_argument("Atlas point cache has invalid target coordinates");
    }
    identity.real(lon[i]);
    identity.real(lat[i]);
  }
  return identity.finish();
}
size_t MpasAtlasPointOperator::sourceSize() const { return impl_->source.size(); }
size_t MpasAtlasPointOperator::targetSize() const { return impl_->target.size(); }
size_t MpasAtlasPointOperator::nonzeros() const { return impl_->matrix.matrix().nnz(); }
const std::string &MpasAtlasPointOperator::horizontalReceipt() const { return impl_->horizontal; }
const std::string &MpasAtlasPointOperator::cacheKey() const { return impl_->key; }
const std::string &MpasAtlasPointOperator::cacheReceipt() const { return impl_->receipt; }
std::string MpasAtlasPointOperator::serializeCache() const { return impl_->payload.dump(); }
std::vector<double> MpasAtlasPointOperator::apply(const std::vector<double> &x, size_t l) const {
  impl_->integrity->validate();
  return impl_->execute(x, nullptr, l, false)[0];
}
std::vector<double> MpasAtlasPointOperator::applyTranspose(const std::vector<double> &y,
                                                           size_t l) const {
  impl_->integrity->validate();
  return impl_->execute(y, nullptr, l, true)[0];
}
std::array<std::vector<double>, 2> MpasAtlasPointOperator::applyVector(const std::vector<double> &x,
                                                                       const std::vector<double> &y,
                                                                       size_t l) const {
  impl_->integrity->validate();
  return impl_->execute(x, &y, l, false);
}
std::array<std::vector<double>, 2> MpasAtlasPointOperator::applyVectorTranspose(
    const std::vector<double> &x, const std::vector<double> &y, size_t l) const {
  impl_->integrity->validate();
  return impl_->execute(x, &y, l, true);
}

std::vector<std::vector<double>> MpasAtlasPointOperator::applyBatch(
    const ScalarFields &fields, const std::vector<size_t> &levels) const {
  if (fields.size() != levels.size()) {
    throw std::invalid_argument("Atlas scalar batch has inconsistent field/level inventory");
  }
  impl_->integrity->validate();
  std::vector<std::vector<double>> result;
  result.reserve(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    result.push_back(impl_->execute(fields[i].get(), nullptr, levels[i], false)[0]);
  }
  return result;
}

std::vector<std::vector<double>> MpasAtlasPointOperator::applyBatchTranspose(
    const ScalarFields &fields, const std::vector<size_t> &levels) const {
  if (fields.size() != levels.size()) {
    throw std::invalid_argument("Atlas scalar batch has inconsistent field/level inventory");
  }
  impl_->integrity->validate();
  std::vector<std::vector<double>> result;
  result.reserve(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    result.push_back(impl_->execute(fields[i].get(), nullptr, levels[i], true)[0]);
  }
  return result;
}

const std::vector<double> &MpasAtlasPointOperator::sourceMeasures() const {
  return impl_->sourceMeasures;
}

std::vector<double> MpasAtlasPointOperator::applyWeightedAdjoint(const std::vector<double> &y,
                                                                 size_t levels) const {
  auto result = applyTranspose(y, levels);
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] /= impl_->sourceMeasures[i / levels];
  }
  checkValues(result, sourceSize(), levels);
  return result;
}

std::array<std::vector<double>, 2> MpasAtlasPointOperator::applyVectorWeightedAdjoint(
    const std::vector<double> &east, const std::vector<double> &north, size_t levels) const {
  auto result = applyVectorTranspose(east, north, levels);
  for (auto &component : result) {
    for (size_t i = 0; i < component.size(); ++i) {
      component[i] /= impl_->sourceMeasures[i / levels];
    }
    checkValues(component, sourceSize(), levels);
  }
  return result;
}
}  // namespace ijedi
