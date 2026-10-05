/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <ostream>

#include "ijedi/Increment/Increment.h"
#include "oops/base/Variables.h"
#include "oops/util/Printable.h"

namespace ijedi {
class Geometry;
class State;

/// Identity covariance retained only for interface tests.  Production DA uses SABER.
class ErrorCovariance : public util::Printable {
 public:
  ErrorCovariance(const Geometry &, const oops::Variables &, const eckit::Configuration &,
                  const State &, const State &) {}
  void multiply(const Increment &input, Increment &output) const { output = input; }
  void inverseMultiply(const Increment &input, Increment &output) const { output = input; }
  void randomize(Increment &increment) const { increment.random(); }
 private:
  void print(std::ostream &) const override {}
};
}  // namespace ijedi
