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
#include "oops/util/Duration.h"
#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"

#include "ijedi/Increment/Increment.h"
#include "ijedi/Geometry/Geometry.h"

#include "ijedi/ModelAux/ModelAuxControl.h"
#include "ijedi/ModelAux/ModelAuxIncrement.h"

namespace ijedi {

class Geometry;
class State;

// An identity linear model.
class LinearModel : public util::Printable, private util::ObjectCounter<LinearModel> {
 public:
  static const std::string classname() { return "ijedi::LinearModel"; }
  static std::vector<std::string> names() { return {"I-JEDI identity"}; }

  LinearModel(const Geometry &geometry, const eckit::Configuration & config):
    tstep_(config.getString("time step")) {
    if (geometry.isMpas()) {
      throw eckit::BadParameter(
          "I-JEDI identity is not the tangent-linear or adjoint of MPAS-PyTorch", Here());
    }
  }
  ~LinearModel() = default;

  /// Model trajectory computation
  void setTrajectory(const State &, State &, const ModelAuxControl &) {}

/// Run TLM and its adjoint
  void initializeTL(Increment &) const {}
  void stepTL(Increment & increment, const ModelAuxIncrement &) const {
    increment.updateTime(tstep_);
  }
  void finalizeTL(Increment &) const {}

  void initializeAD(Increment &) const {}
  void stepAD(Increment & increment, ModelAuxIncrement &) const {
    increment.updateTime(-tstep_);
  }
  void finalizeAD(Increment &) const {}

/// Other utilities
  const util::Duration & timeResolution() const {return tstep_;}
  const util::Duration & stepTrajectory() const {return tstep_;}

 private:
  void print(std::ostream &) const override {}

  util::Duration tstep_;
};

}  // namespace ijedi
