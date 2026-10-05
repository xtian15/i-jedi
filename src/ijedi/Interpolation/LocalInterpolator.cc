/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/LocalInterpolator.h"

#include <utility>
#include <algorithm>
#include <map>
#include <limits>
#include <cmath>
#include <set>

#include "eckit/config/Configuration.h"
#include "eckit/exception/Exceptions.h"
#include "oops/base/Variables.h"
#include "oops/generic/UnstructuredInterpolator.h"

#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Increment/MpasIncrementBackend.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/State/State.h"

namespace ijedi {

LocalInterpolator::LocalInterpolator(const eckit::Configuration &config, const Geometry &geometry,
                                     const std::vector<double> &latitudes,
                                     const std::vector<double> &longitudes)
    : mpas_(geometry.isMpas()), targetCount_(latitudes.size()) {
  if (latitudes.size() != longitudes.size()) {
    throw eckit::BadParameter(
        "LocalInterpolator target latitude/longitude arrays must have equal extents", Here());
  }
  if (mpas_) {
    windNames_ = geometry.mpasContext()->geovalWindNames();
    const std::string method = config.getString("method", "finite-element");
    const std::string maskSemantics =
        config.getString("mask semantics", "all_unmasked_error_on_unmapped");
    if (method != "finite-element") {
      throw eckit::BadParameter(
          "MPAS Atlas LocalInterpolator supports exactly method=finite-element", Here());
    }
    if (maskSemantics != "all_unmasked_error_on_unmapped") {
      throw eckit::BadParameter("MPAS Atlas LocalInterpolator mask semantics are receipt-fixed",
                                Here());
    }
    if (geometry.comm().size() != 1) {
      throw eckit::BadParameter("MPAS Atlas LocalInterpolator supports serial-global geometry only",
                                Here());
    }
    if (targetCount_ != 0) {
      mpasOperator_ = geometry.mpasAtlasGeometry().pointOperator(
          latitudes, longitudes, geometry.modelData().getString("atlas_compiler_identity"));
      cacheKey_ = mpasOperator_->cacheKey();
    }
    horizontalReceipt_ = geometry.mpasContext()->horizontalGeometryReceipt();
    bundleReceipt_ = geometry.mpasContext()->geometryReceipt();
    schemaDigest_ = geometry.mpasContext()->stateSchemaDigest();
  } else {
    generic_ = std::make_unique<oops::UnstructuredInterpolator>(config, geometry.geometryData(),
                                                                latitudes, longitudes);
  }
}

LocalInterpolator::~LocalInterpolator() = default;

LocalTargetPartitioner LocalInterpolator::makeTargetPartitioner(const Geometry &geometry) {
  geometry.validateIterationBoundary();
  if (geometry.isMpas() && geometry.comm().size() != 1) {
    throw eckit::BadParameter("MPAS target partitioning supports serial-global geometry only",
                              Here());
  }
  // In the supported one-rank MPAS capability every target is necessarily
  // assigned to rank zero. Reuse OOPS's partitioner type so GetValues retains
  // one compile-time path for all I-JEDI geometries.
#ifdef IJEDI_HAS_OOPS_TARGET_PARTITIONER
  return oops::makeSourceProximityPartitioner(geometry.functionSpace(), geometry.comm());
#else
  if (geometry.comm().size() != 1) {
    throw eckit::NotImplemented(
        "this installed OOPS lacks target partitioning support for custom interpolators", Here());
  }
  return LocalTargetPartitioner();
#endif
}

void LocalInterpolator::preprocess(State &state) {
  if (!state.isMpas()) {
    oops::UnstructuredInterpolator::preprocess(state.fieldSet());
  }
}

void LocalInterpolator::preprocess(Increment &increment) {
  if (increment.isMpas()) {
    if (increment.analysisNamespace() != "geoval") {
      throw eckit::BadParameter(
          "MPAS interpolation requires the model-owned GeoVaL TL transform first", Here());
    }
    (void)increment.analysisArrays();
    return;
  }
  oops::UnstructuredInterpolator::preprocess(increment.fieldSet());
}

void LocalInterpolator::preprocessAD(Increment &increment) {
  if (increment.isMpas()) {
    if (increment.analysisNamespace() != "geoval") {
      throw eckit::BadParameter("MPAS interpolation AD requires a GeoVaL carrier", Here());
    }
    (void)increment.analysisArrays();
    return;
  }
  oops::UnstructuredInterpolator::preprocessAD(increment.fieldSet());
}

void LocalInterpolator::apply(const oops::Variables &variables, const State &state,
                              const std::vector<bool> &mask, std::vector<double> &buffer) const {
  if (!mpas_) {
    generic_->apply(variables, state.fieldSet(), mask, buffer);
    return;
  }
  if (!state.isMpas()) {
    throw eckit::BadParameter("MPAS interpolator received a non-MPAS State", Here());
  }
  (void)state.geometry().variableSizes(variables);
  for (const auto &variable : variables) {
    if (state.geometry().mpasContext()->variableNamespace(variable.name()) != "geoval") {
      throw eckit::BadParameter("MPAS sampling requires public GeoVaL variables", Here());
    }
  }
  if (state.geometry().mpasContext()->horizontalGeometryReceipt() != horizontalReceipt_ ||
      state.geometry().mpasContext()->geometryReceipt() != bundleReceipt_ ||
      state.geometry().mpasContext()->stateSchemaDigest() != schemaDigest_) {
    throw eckit::BadParameter(
        "MPAS State and Atlas interpolator have different horizontal geometry", Here());
  }
  if (mask.size() != targetCount_) {
    throw eckit::BadParameter("MPAS interpolation mask has the wrong target extent", Here());
  }
  if (targetCount_ == 0) {
    buffer.clear();
    return;
  }
  const auto names = variables.variables();
  const bool hasEast = std::find(names.begin(), names.end(), windNames_[0]) != names.end();
  const bool hasNorth = std::find(names.begin(), names.end(), windNames_[1]) != names.end();
  const auto fields = state.materializeTypedFields(
      MpasIncrementBackend::expandVectorDependencies(*state.geometry().mpasContext(), variables));
  std::map<std::string, const MpasTypedField *> byName;
  for (const auto &field : fields) {
    if (field.horizontalGeometryReceipt != horizontalReceipt_ ||
        field.horizontalLocation != "cell") {
      throw eckit::BadValue("MPAS typed field and Atlas source space identities differ", Here());
    }
    if (!byName.emplace(field.name, &field).second) {
      throw eckit::BadValue("duplicate MPAS interpolation variable", Here());
    }
  }
  MpasAnalysisArrays arrays;
  for (const auto &field : fields) {
    arrays.emplace(field.name, MpasAnalysisArray{{field.values.size() / field.levels, field.levels},
                                                 field.values});
  }
  if (hasEast || hasNorth) {
    const auto &east = *byName.at(windNames_[0]), &north = *byName.at(windNames_[1]);
    if (east.levels != north.levels || east.trajectoryReceipt != north.trajectoryReceipt ||
        east.componentBasis != "local_east_north" || north.componentBasis != "local_east_north") {
      throw eckit::BadValue("paired MPAS wind descriptors or trajectories differ", Here());
    }
  }
  applyArrays(variables, arrays, mask, buffer);
}

void LocalInterpolator::applyArrays(const oops::Variables &variables,
                                    const MpasAnalysisArrays &arrays, const std::vector<bool> &mask,
                                    std::vector<double> &buffer) const {
  if (mask.size() != targetCount_) {
    throw eckit::BadParameter("MPAS interpolation target mask extent differs", Here());
  }
  if (targetCount_ == 0) {
    buffer.clear();
    return;
  }
  const auto names = variables.variables();
  if (std::set<std::string>(names.begin(), names.end()).size() != names.size()) {
    throw eckit::BadParameter("MPAS interpolation variable order contains duplicates", Here());
  }
  const bool hasEast = std::find(names.begin(), names.end(), windNames_[0]) != names.end();
  const bool hasNorth = std::find(names.begin(), names.end(), windNames_[1]) != names.end();
  const auto levelsFor = [](const MpasAnalysisArray &array) {
    return array.shape.size() == 1 ? size_t{1} : array.shape.at(1);
  };
  std::array<std::vector<double>, 2> wind;
  if (hasEast || hasNorth) {
    const auto &east = arrays.at(windNames_[0]), &north = arrays.at(windNames_[1]);
    if (east.shape != north.shape) {
      throw eckit::BadValue("paired MPAS wind tangent extents differ", Here());
    }
    wind = mpasOperator_->applyVector(east.values, north.values, levelsFor(east));
  }
  size_t totalLevels = 0;
  MpasAtlasPointOperator::ScalarFields scalarFields;
  std::vector<size_t> scalarLevels;
  for (const auto &variable : variables) {
    const size_t levels = levelsFor(arrays.at(variable.name()));
    if (variable.name() != windNames_[0] && variable.name() != windNames_[1]) {
      scalarFields.emplace_back(std::cref(arrays.at(variable.name()).values));
      scalarLevels.push_back(levels);
    }
    if (levels > std::numeric_limits<size_t>::max() - totalLevels) {
      throw eckit::BadValue("MPAS interpolation level count overflow", Here());
    }
    totalLevels += levels;
  }
  if (totalLevels && targetCount_ > std::numeric_limits<size_t>::max() / totalLevels) {
    throw eckit::BadValue("MPAS interpolation target buffer overflow", Here());
  }
  const size_t requiredSize = targetCount_ * totalLevels;
  const auto scalars = scalarFields.empty() ? std::vector<std::vector<double>>{} :
      mpasOperator_->applyBatch(scalarFields, scalarLevels);
  // Commit only after every requested field has passed typed validation and
  // execution. Preserve inactive targets, including their caller-owned bits.
  std::vector<double> result(buffer);
  if (result.size() != requiredSize) {
    result.resize(requiredSize);
  }
  size_t offset = 0;
  size_t scalarIndex = 0;
  for (const auto &variable : variables) {
    const auto &field = arrays.at(variable.name());
    const size_t levels = levelsFor(field);
    std::vector<double> values;
    if (variable.name() == windNames_[0]) {
      values = wind[0];
    } else if (variable.name() == windNames_[1]) {
      values = wind[1];
    } else {
      values = scalars.at(scalarIndex++);
    }
    for (size_t target = 0; target < targetCount_; ++target) {
      if (!mask[target]) {
        continue;
      }
      for (size_t level = 0; level < levels; ++level) {
        result[offset + target * levels + level] = values[target * levels + level];
      }
    }
    offset += targetCount_ * levels;
  }
  if (offset != result.size()) {
    throw eckit::BadValue("MPAS interpolation buffer accounting failed", Here());
  }
  buffer.swap(result);
}

void LocalInterpolator::apply(const oops::Variables &variables, const Increment &increment,
                              const std::vector<bool> &mask, std::vector<double> &buffer) const {
  if (mpas_) {
    (void)increment.geometry().variableSizes(variables);
    for (const auto &variable : variables) {
      if (increment.geometry().mpasContext()->variableNamespace(variable.name()) != "geoval") {
        throw eckit::BadParameter("MPAS sampling requires public GeoVaL variables", Here());
      }
    }
    if (!increment.isMpas() || increment.analysisNamespace() != "geoval" ||
        increment.geometry().mpasContext()->geometryReceipt() != bundleReceipt_ ||
        increment.geometry().mpasContext()->stateSchemaDigest() != schemaDigest_) {
      throw eckit::BadParameter("MPAS interpolation TL needs a matching GeoVaL carrier", Here());
    }
    applyArrays(variables, increment.analysisArrays(), mask, buffer);
    return;
  }
  generic_->apply(variables, increment.fieldSet(), mask, buffer);
}

void LocalInterpolator::applyAD(const oops::Variables &variables, Increment &increment,
                                const std::vector<bool> &mask,
                                const std::vector<double> &buffer) const {
  if (mpas_) {
    (void)increment.geometry().variableSizes(variables);
    for (const auto &variable : variables) {
      if (increment.geometry().mpasContext()->variableNamespace(variable.name()) != "geoval") {
        throw eckit::BadParameter("MPAS sampling requires public GeoVaL variables", Here());
      }
    }
    if (!increment.isMpas() || increment.analysisNamespace() != "geoval" ||
        increment.geometry().mpasContext()->geometryReceipt() != bundleReceipt_ ||
        increment.geometry().mpasContext()->stateSchemaDigest() != schemaDigest_ ||
        mask.size() != targetCount_) {
      throw eckit::BadParameter("MPAS interpolation AD needs matching GeoVaL carrier/mask", Here());
    }
    auto accumulated = increment.analysisArrays();
    const auto names = variables.variables();
    if (std::set<std::string>(names.begin(), names.end()).size() != names.size()) {
      throw eckit::BadParameter("MPAS AD variable order contains duplicates", Here());
    }
    const auto masses = increment.analysisMeasures();
    const auto levelsFor = [](const MpasAnalysisArray &array) {
      return array.shape.size() == 1 ? size_t{1} : array.shape.at(1);
    };
    std::map<std::string, std::vector<double>> seeds;
    size_t required = 0;
    for (const auto &var : variables) {
      const size_t levels = levelsFor(accumulated.at(var.name()));
      if (levels && targetCount_ > (std::numeric_limits<size_t>::max() - required) / levels) {
        throw eckit::BadValue("MPAS AD buffer extent overflow", Here());
      }
      required += targetCount_ * levels;
    }
    if (buffer.size() != required) {
      throw eckit::BadParameter("MPAS AD buffer extent differs", Here());
    }
    if (targetCount_ == 0) {
      return;
    }
    size_t offset = 0;
    for (const auto &var : variables) {
      const size_t levels = levelsFor(accumulated.at(var.name()));
      auto &seed = seeds[var.name()];
      seed.assign(targetCount_ * levels, 0.);
      for (size_t target = 0; target < targetCount_; ++target) {
        if (mask[target]) {
          for (size_t level = 0; level < levels; ++level) {
            const double value = buffer.at(offset + target * levels + level);
            if (!std::isfinite(value)) {
              throw eckit::BadValue("MPAS AD active observation seed is nonfinite", Here());
            }
            seed[target * levels + level] = value;
          }
        }
      }
      offset += targetCount_ * levels;
    }
    const bool east = seeds.count(windNames_[0]), north = seeds.count(windNames_[1]);
    const auto accumulate = [&](const std::string &name, const std::vector<double> &covector) {
      auto &values = accumulated.at(name).values;
      const auto &mass = masses.at(name).values;
      if (covector.size() != values.size() || mass.size() != values.size()) {
        throw eckit::BadValue("MPAS AD weighted source extent differs", Here());
      }
      for (size_t i = 0; i < values.size(); ++i) {
        if (covector[i] != 0.) {
          values[i] += covector[i] / mass[i];
        }
      }
    };
    if (east || north) {
      const size_t levels = levelsFor(accumulated.at(windNames_[0]));
      if (!east) {
        seeds[windNames_[0]].assign(targetCount_ * levels, 0.);
      }
      if (!north) {
        seeds[windNames_[1]].assign(targetCount_ * levels, 0.);
      }
      const auto gradient = mpasOperator_->applyVectorTranspose(seeds.at(windNames_[0]),
                                                                seeds.at(windNames_[1]), levels);
      accumulate(windNames_[0], gradient[0]);
      accumulate(windNames_[1], gradient[1]);
    }
    MpasAtlasPointOperator::ScalarFields scalarSeeds;
    std::vector<size_t> scalarLevels;
    std::vector<std::string> scalarNames;
    for (const auto &var : variables) {
      if (var.name() != windNames_[0] && var.name() != windNames_[1]) {
        scalarSeeds.emplace_back(std::cref(seeds.at(var.name())));
        scalarLevels.push_back(levelsFor(accumulated.at(var.name())));
        scalarNames.push_back(var.name());
      }
    }
    const auto scalarGradients = scalarSeeds.empty() ? std::vector<std::vector<double>>{} :
        mpasOperator_->applyBatchTranspose(scalarSeeds, scalarLevels);
    for (size_t i = 0; i < scalarNames.size(); ++i) {
      accumulate(scalarNames[i], scalarGradients[i]);
    }
    increment.replaceAnalysis(increment.variables(), accumulated);
    return;
  }
  generic_->applyAD(variables, increment.fieldSet(), mask, buffer);
}

void LocalInterpolator::print(std::ostream &stream) const {
  if (mpas_) {
    stream << "MPAS native Atlas point/vector operator: targets=" << targetCount_
           << ", cache_key=" << cacheKey_;
  } else {
    stream << *generic_;
  }
}

}  // namespace ijedi
