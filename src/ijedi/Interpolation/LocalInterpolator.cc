/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/LocalInterpolator.h"

#include "eckit/exception/Exceptions.h"
#include "oops/generic/UnstructuredInterpolator.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/State/State.h"

namespace ijedi {
namespace {
void requireLegacy(bool mpas) {
  if (mpas) {
    throw eckit::NotImplemented(
        "MPAS OOPS variable sampling requires the stacked variable-transform PR", Here());
  }
}
}  // namespace

LocalInterpolator::LocalInterpolator(const eckit::Configuration &config, const Geometry &geometry,
                                     const std::vector<double> &latitudes,
                                     const std::vector<double> &longitudes) {
  requireLegacy(geometry.isMpas());
  generic_ = std::make_unique<oops::UnstructuredInterpolator>(
      config, geometry.geometryData(), latitudes, longitudes);
}
LocalInterpolator::~LocalInterpolator() = default;

LocalTargetPartitioner LocalInterpolator::makeTargetPartitioner(const Geometry &geometry) {
  geometry.validateIterationBoundary();
  requireLegacy(geometry.isMpas());
#ifdef IJEDI_HAS_OOPS_TARGET_PARTITIONER
  return oops::makeSourceProximityPartitioner(geometry.functionSpace(), geometry.comm());
#else
  if (geometry.comm().size() != 1) {
    throw eckit::NotImplemented("installed OOPS lacks target partitioning support", Here());
  }
  return LocalTargetPartitioner();
#endif
}

void LocalInterpolator::preprocess(State &state) {
  requireLegacy(state.isMpas());
  oops::UnstructuredInterpolator::preprocess(state.fieldSet());
}
void LocalInterpolator::preprocess(Increment &increment) {
  requireLegacy(increment.isMpas());
  oops::UnstructuredInterpolator::preprocess(increment.fieldSet());
}
void LocalInterpolator::preprocessAD(Increment &increment) {
  requireLegacy(increment.isMpas());
  oops::UnstructuredInterpolator::preprocessAD(increment.fieldSet());
}
void LocalInterpolator::apply(const oops::Variables &variables, const State &state,
                              const std::vector<bool> &mask, std::vector<double> &buffer) const {
  requireLegacy(state.isMpas());
  generic_->apply(variables, state.fieldSet(), mask, buffer);
}
void LocalInterpolator::apply(const oops::Variables &variables, const Increment &increment,
                              const std::vector<bool> &mask, std::vector<double> &buffer) const {
  requireLegacy(increment.isMpas());
  generic_->apply(variables, increment.fieldSet(), mask, buffer);
}
void LocalInterpolator::applyAD(const oops::Variables &variables, Increment &increment,
                                const std::vector<bool> &mask,
                                const std::vector<double> &buffer) const {
  requireLegacy(increment.isMpas());
  generic_->applyAD(variables, increment.fieldSet(), mask, buffer);
}
void LocalInterpolator::print(std::ostream &stream) const {
  stream << "ijedi::LocalInterpolator legacy OOPS delegate; MPAS sampling requires PR 2";
}
}  // namespace ijedi
