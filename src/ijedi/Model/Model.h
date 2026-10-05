/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "oops/base/Variables.h"
#include "eckit/exception/Exceptions.h"
#include "oops/util/Duration.h"
#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"

#include "ijedi/State/State.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/ModelAux/ModelAuxControl.h"

namespace ijedi {

class Geometry;
// A persistence model.
class Model : public util::Printable, private util::ObjectCounter<Model> {
 public:
  static const std::string classname() { return "ijedi::Model"; }
  static std::vector<std::string> names() {
    return {"I-JEDI persistence", "MPAS-PyTorch"};
  }

  Model(const Geometry &geometry, const eckit::Configuration & config):
    tstep_(config.getString("time step")), vars_(config, "model variables"),
    mpas_(config.getString("name") == "MPAS-PyTorch") {
    if (mpas_ != geometry.isMpas()) {
      throw eckit::BadParameter(
          "Model name and Geometry backend disagree: MPAS-PyTorch requires geometry_type=mpas",
          Here());
    }
    if (mpas_) {
      const auto context = geometry.mpasContext();
      context->validateModelVariables(vars_);
      if (tstep_.toSeconds() != context->timeStepSeconds()) {
        throw eckit::BadParameter("MPAS model timestep differs from owner support", Here());
      }
      geometryReceipt_ = context->geometryReceipt();
      configurationReceipt_ = context->configurationReceipt();
      schemaDigest_ = context->stateSchemaDigest();
    }
  }
  ~Model() = default;

  void initialize(State &state) const { validateState(state); }
  void step(State & state, const ModelAuxControl &) const {
    validateState(state);
    if (mpas_) {
      state.advanceModel(tstep_);
    } else {
      state.updateTime(tstep_);
    }
  }
  void finalize(State &) const {}

  const util::Duration & timeResolution() const { return tstep_; }
  const oops::Variables & variables() const { return vars_; }

 private:
  void validateState(const State &state) const {
    if (state.isMpas() != mpas_) {
      throw eckit::BadParameter("Model and State backends disagree", Here());
    }
    if (mpas_) {
      const auto context = state.geometry().mpasContext();
      if (context->geometryReceipt() != geometryReceipt_ ||
          context->configurationReceipt() != configurationReceipt_ ||
          context->stateSchemaDigest() != schemaDigest_) {
        throw eckit::BadParameter("MPAS Model and State owner support differ", Here());
      }
    }
  }
  void print(std::ostream &) const override {}

  util::Duration tstep_;
  const oops::Variables vars_;
  bool mpas_;
  std::string geometryReceipt_, configurationReceipt_, schemaDigest_;
};

}  // namespace ijedi
