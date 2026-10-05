/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/MpasAtlasConservativeOperator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/interpolation/Cache.h"
#include "atlas/interpolation/Interpolation.h"
#include "atlas/library/Library.h"
#include "atlas/linalg/sparse/MakeEckitSparseMatrix.h"
#include "atlas/linalg/sparse/MakeSparseMatrixStorageEckit.h"
#include "atlas/option.h"
#include "atlas/util/Config.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasCellMeanEndpoint.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Interpolation/AtlasMeshIntegrity.h"

namespace ijedi {
namespace {
using Json = nlohmann::json;
constexpr const char *schema = "atlas-cell-mean-declared-measure-cache-v1";

void checkValues(const std::vector<double> &values, size_t rows, size_t levels) {
  if (!levels || rows > std::numeric_limits<size_t>::max() / levels ||
      values.size() != rows * levels ||
      std::any_of(values.begin(), values.end(), [](double x) { return !std::isfinite(x); })) {
    throw std::invalid_argument("Atlas conservative field has invalid extent or nonfinite values");
  }
}
}  // namespace

struct MpasAtlasConservativeOperator::Impl {
  atlas::FunctionSpace source, target;
  const std::vector<double> ms, mt;
  atlas::interpolation::MatrixCache matrix;
  std::unique_ptr<atlas::Interpolation> op;
  std::string key, receipt;
  Json payload;
  double rowResidual = 0, columnResidual = 0;
  const std::shared_ptr<const AtlasMeshIntegrity> sourceIntegrity, targetIntegrity;

  Impl(const AtlasCellMeanEndpoint &s, const AtlasCellMeanEndpoint &t, const std::string &compiler,
       const std::string *serialized = nullptr, const std::string *trusted = nullptr)
      : source(s.space()),
        target(t.space()),
        ms(s.measures()),
        mt(t.measures()),
        sourceIntegrity(s.storageIntegrity()),
        targetIntegrity(t.storageIntegrity()) {
    sourceIntegrity->validate();
    targetIntegrity->validate();
    if (compiler.size() != 40 || compiler != atlas::Library::instance().gitsha1(40) ||
        ms.size() != static_cast<size_t>(source.size()) ||
        mt.size() != static_cast<size_t>(target.size()) || ms.empty() || mt.empty()) {
      throw std::invalid_argument("Atlas conservative compiler or endpoint identity mismatch");
    }
    AtlasOperatorReceipt hash;
    hash.string(schema);
    hash.string(compiler);
    hash.string(s.receipt());
    hash.string(t.receipt());
    hash.string(s.contentReceipt());
    hash.string(t.contentReceipt());
    for (const auto *measures : {&ms, &mt}) {
      hash.integer(measures->size());
      for (double value : *measures) {
        if (!std::isfinite(value) || value <= 0) {
          throw std::invalid_argument("Atlas conservative endpoint has an invalid owned measure");
        }
        hash.real(value);
      }
    }
    key = hash.finish();
    atlas::util::Config config("type", "conservative-spherical-polygon");
    config.set("order", 1);
    config.set("adjoint", true);
    config.set("declared_measures", true);
    config.set("source_measures", ms);
    config.set("target_measures", mt);
    if (serialized) {
      if (serialized->size() > 256 * 1024 * 1024) {
        throw std::invalid_argument("Atlas conservative cache exceeds bounded artifact capacity");
      }
      payload = Json::parse(*serialized);
      const std::set<std::string> required{"schema", "key",     "atlas_uid",
                                           "rows",   "columns", "entries"};
      std::set<std::string> found;
      for (auto item = payload.begin(); item != payload.end(); ++item) {
        found.insert(item.key());
      }
      if (found != required || payload.at("schema") != schema || payload.at("key") != key ||
          payload.at("rows") != target.size() || payload.at("columns") != source.size() ||
          !payload.at("atlas_uid").is_string() ||
          payload.at("atlas_uid").get<std::string>().empty()) {
        throw std::invalid_argument("Atlas conservative cache endpoint/schema mismatch");
      }
      receipt = validateAndDigest();
      if (!trusted || receipt != *trusted) {
        throw std::invalid_argument(
            "Atlas conservative cache trusted coefficient receipt mismatch");
      }
      std::vector<eckit::linalg::Triplet> entries;
      entries.reserve(payload.at("entries").size());
      for (const auto &entry : payload.at("entries")) {
        entries.emplace_back(entry[0].get<size_t>(), entry[1].get<size_t>(),
                             entry[2].get<double>());
      }
      eckit::linalg::SparseMatrix decoded(target.size(), source.size(), entries);
      auto owned = std::make_shared<const atlas::linalg::SparseMatrixStorage>(
          atlas::linalg::make_sparse_matrix_storage(std::move(decoded)));
      matrix = atlas::interpolation::MatrixCache(owned, payload.at("atlas_uid").get<std::string>());
      if (matrixPayload().at("entries") != payload.at("entries")) {
        throw std::invalid_argument(
            "Atlas conservative hydration changed coefficient order or bytes");
      }
      // Atlas independently authenticates the endpoint/measure/coefficient UID.
      op = std::make_unique<atlas::Interpolation>(config, source, target, matrix);
    } else {
      op = std::make_unique<atlas::Interpolation>(config, source, target);
      matrix = atlas::interpolation::MatrixCache(*op);
      payload = matrixPayload();
      receipt = validateAndDigest();
    }
  }

  Json matrixPayload() const {
    const auto sparse = atlas::linalg::make_non_owning_eckit_sparse_matrix(matrix.matrix());
    Json entries = Json::array();
    for (auto entry = sparse.begin(); entry != sparse.end(); ++entry) {
      entries.push_back({entry.row(), entry.col(), *entry});
    }
    return {{"schema", schema},          {"key", key},
            {"atlas_uid", matrix.uid()}, {"rows", sparse.rows()},
            {"columns", sparse.cols()},  {"entries", std::move(entries)}};
  }

  std::string validateAndDigest() {
    const auto &entries = payload.at("entries");
    if (!entries.is_array() || entries.empty()) {
      throw std::invalid_argument("Atlas conservative cache has no sparse entries");
    }
    AtlasOperatorReceipt hash;
    hash.string(schema);
    hash.string(key);
    hash.string(payload.at("atlas_uid").get<std::string>());
    hash.integer(source.size());
    hash.integer(target.size());
    hash.integer(entries.size());
    std::vector<long double> rows(mt.size(), 0), columns(ms.size(), 0);
    std::set<std::pair<size_t, size_t>> support;
    size_t previousRow = 0;
    bool first = true;
    for (const auto &entry : entries) {
      if (!entry.is_array() || entry.size() != 3 || !entry[0].is_number_integer() ||
          !entry[1].is_number_integer() || !entry[2].is_number() ||
          entry[0].get<std::int64_t>() < 0 || entry[1].get<std::int64_t>() < 0) {
        throw std::invalid_argument("Atlas conservative cache has malformed sparse entries");
      }
      const size_t r = entry[0].get<size_t>(), c = entry[1].get<size_t>();
      const double value = entry[2].get<double>();
      if (r >= mt.size() || c >= ms.size() || !std::isfinite(value) || value < 0 ||
          (!first && r < previousRow) || !support.emplace(r, c).second) {
        throw std::invalid_argument(
            "Atlas conservative cache has invalid indices/order/coefficients");
      }
      rows[r] += value;
      columns[c] += static_cast<long double>(value) * mt[r];
      hash.integer(r);
      hash.integer(c);
      hash.real(value);
      previousRow = r;
      first = false;
    }
    for (size_t r = 0; r < rows.size(); ++r) {
      rowResidual = std::max(rowResidual, static_cast<double>(std::abs(rows[r] - 1.L)));
    }
    for (size_t c = 0; c < columns.size(); ++c) {
      columnResidual =
          std::max(columnResidual, static_cast<double>(std::abs(columns[c] / ms[c] - 1.L)));
    }
    if (!std::isfinite(rowResidual) || !std::isfinite(columnResidual) || rowResidual > 2.e-14 ||
        columnResidual > 2.e-14) {
      throw std::invalid_argument("Atlas conservative cache fails owned-measure marginal bars");
    }
    return hash.finish();
  }

  std::vector<double> execute(const std::vector<double> &values, size_t levels,
                              bool transpose) const {
    sourceIntegrity->validate();
    targetIntegrity->validate();
    const auto &from = transpose ? target : source, &to = transpose ? source : target;
    checkValues(values, from.size(), levels);
    if (static_cast<size_t>(to.size()) > std::numeric_limits<size_t>::max() / levels) {
      throw std::invalid_argument("Atlas conservative output extent overflow");
    }
    auto input = from.createField<double>(atlas::option::levels(levels));
    auto output = to.createField<double>(atlas::option::levels(levels));
    auto iv = atlas::array::make_view<double, 2>(input);
    auto ov = atlas::array::make_view<double, 2>(output);
    for (size_t r = 0; r < static_cast<size_t>(from.size()); ++r) {
      for (size_t k = 0; k < levels; ++k) {
        iv(r, k) = values[r * levels + k];
      }
    }
    ov.assign(0.);
    if (transpose) {
      op->execute_adjoint(output, input);
    } else {
      op->execute(input, output);
    }
    std::vector<double> result(to.size() * levels);
    for (size_t r = 0; r < static_cast<size_t>(to.size()); ++r) {
      for (size_t k = 0; k < levels; ++k) {
        result[r * levels + k] = ov(r, k);
      }
    }
    checkValues(result, to.size(), levels);
    return result;
  }
};

MpasAtlasConservativeOperator::MpasAtlasConservativeOperator(const MpasAtlasGeometry &s,
                                                             const MpasAtlasGeometry &t,
                                                             const std::string &compiler)
    : MpasAtlasConservativeOperator(AtlasCellMeanEndpoint::fromMpas(s),
                                    AtlasCellMeanEndpoint::fromMpas(t), compiler) {}
MpasAtlasConservativeOperator::MpasAtlasConservativeOperator(const MpasAtlasGeometry &s,
                                                             const MpasAtlasGeometry &t,
                                                             const std::string &compiler,
                                                             const std::string &payload,
                                                             const std::string &trusted)
    : MpasAtlasConservativeOperator(AtlasCellMeanEndpoint::fromMpas(s),
                                    AtlasCellMeanEndpoint::fromMpas(t), compiler, payload,
                                    trusted) {}
MpasAtlasConservativeOperator::MpasAtlasConservativeOperator(const AtlasCellMeanEndpoint &s,
                                                             const AtlasCellMeanEndpoint &t,
                                                             const std::string &compiler)
    : impl_(std::make_unique<Impl>(s, t, compiler)) {}
MpasAtlasConservativeOperator::MpasAtlasConservativeOperator(const AtlasCellMeanEndpoint &s,
                                                             const AtlasCellMeanEndpoint &t,
                                                             const std::string &compiler,
                                                             const std::string &payload,
                                                             const std::string &trusted)
    : impl_(std::make_unique<Impl>(s, t, compiler, &payload, &trusted)) {}
MpasAtlasConservativeOperator::~MpasAtlasConservativeOperator() = default;
size_t MpasAtlasConservativeOperator::sourceSize() const { return impl_->ms.size(); }
size_t MpasAtlasConservativeOperator::targetSize() const { return impl_->mt.size(); }
size_t MpasAtlasConservativeOperator::nonzeros() const {
  return impl_->payload.at("entries").size();
}
const std::string &MpasAtlasConservativeOperator::cacheKey() const { return impl_->key; }
const std::string &MpasAtlasConservativeOperator::cacheReceipt() const { return impl_->receipt; }
const std::vector<double> &MpasAtlasConservativeOperator::sourceMeasures() const {
  return impl_->ms;
}
const std::vector<double> &MpasAtlasConservativeOperator::targetMeasures() const {
  return impl_->mt;
}
double MpasAtlasConservativeOperator::rowResidual() const { return impl_->rowResidual; }
double MpasAtlasConservativeOperator::columnResidual() const { return impl_->columnResidual; }
std::string MpasAtlasConservativeOperator::serializeCache() const { return impl_->payload.dump(); }
std::vector<double> MpasAtlasConservativeOperator::apply(const std::vector<double> &v,
                                                         size_t levels) const {
  return impl_->execute(v, levels, false);
}
std::vector<double> MpasAtlasConservativeOperator::applyTranspose(const std::vector<double> &v,
                                                                  size_t levels) const {
  return impl_->execute(v, levels, true);
}
std::vector<double> MpasAtlasConservativeOperator::applyWeightedAdjoint(
    const std::vector<double> &v, size_t levels) const {
  checkValues(v, targetSize(), levels);
  auto weighted = v;
  for (size_t i = 0; i < v.size(); ++i) {
    weighted[i] *= impl_->mt[i / levels];
  }
  auto result = impl_->execute(weighted, levels, true);
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] /= impl_->ms[i / levels];
  }
  checkValues(result, sourceSize(), levels);
  return result;
}
}  // namespace ijedi
