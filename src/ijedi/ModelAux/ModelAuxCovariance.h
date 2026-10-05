/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <ostream>

#include "oops/util/Printable.h"

namespace eckit { class Configuration; }

namespace ijedi {
class Geometry;
class ModelAuxControl;
class ModelAuxIncrement;

class ModelAuxCovariance : public util::Printable {
 public:
  ModelAuxCovariance(const eckit::Configuration &, const Geometry &) {}
  void linearize(const ModelAuxControl &, const Geometry &) {}
  void multiply(const ModelAuxIncrement &, ModelAuxIncrement &) const {}
  void inverseMultiply(const ModelAuxIncrement &, ModelAuxIncrement &) const {}
  void randomize(ModelAuxIncrement &) const {}
 private:
  void print(std::ostream &) const override {}
};
}  // namespace ijedi
