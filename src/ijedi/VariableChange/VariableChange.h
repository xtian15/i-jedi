/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#pragma once

#include <memory>
#include <ostream>

#include "oops/util/Printable.h"
#include "vader/vader.h"

namespace eckit {
class Configuration;
}  // namespace eckit

namespace oops {
class Variables;
}  // namespace oops

namespace ijedi {

class Geometry;
class State;

class VariableChange : public util::Printable {
 public:
  VariableChange(const eckit::Configuration &, const Geometry &);
  ~VariableChange() = default;

  // Perform transforms
  void changeVar(State &, const oops::Variables &) const;
  void changeVarInverse(State &, const oops::Variables &) const;

 private:
  void print(std::ostream &) const override;
  const Geometry & geom_;
  std::unique_ptr<vader::Vader> varchange_;
};

}  // namespace ijedi
