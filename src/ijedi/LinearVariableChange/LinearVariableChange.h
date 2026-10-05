/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#pragma once

#include <memory>

#include "oops/util/Printable.h"
#include "vader/vader.h"

namespace oops {
class Variables;
}  // namespace oops

namespace ijedi {

class Geometry;
class State;

class Increment;

class LinearVariableChange : public util::Printable {
 public:
  LinearVariableChange(const Geometry &, const eckit::Configuration &);
  ~LinearVariableChange();

  /// Inject geometry-sourced trajectory ingredients (latitude, longitude,
  /// sea_area_fraction) required by SeaWaterTemperature_B's Jacobian before
  /// handing the trajectory to the base class.
  void changeVarTraj(const State &, const oops::Variables &);
  void changeVarTL(Increment &, const oops::Variables &) const;
  void changeVarInverseTL(Increment &, const oops::Variables &) const;
  void changeVarAD(Increment &, const oops::Variables &) const;
  void changeVarInverseAD(Increment &, const oops::Variables &) const;

 private:
  void selectVariables(Increment &, const oops::Variables &) const;
  void ensureLinearPlan(oops::Variables &) const;
  void requireMpasTrajectory(const Increment &, const oops::Variables &) const;
  void mpasInverseIdentity(Increment &, const oops::Variables &) const;
  void print(std::ostream &) const override {}
  const Geometry &geom_;
  std::unique_ptr<vader::Vader> vader_;
  std::unique_ptr<State> mpasTrajectory_;
  mutable oops::Variables varsVaderPopulates_;
};

}  // namespace ijedi
