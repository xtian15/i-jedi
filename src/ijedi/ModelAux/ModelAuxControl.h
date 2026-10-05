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
class ModelAuxIncrement;

class ModelAuxControl : public util::Printable {
 public:
  ModelAuxControl(const Geometry &, const eckit::Configuration &) {}
  ModelAuxControl(const Geometry &, const ModelAuxControl &) {}
  ModelAuxControl(const ModelAuxControl &, bool) {}
  ModelAuxControl &operator+=(const ModelAuxIncrement &) { return *this; }
  void read(const eckit::Configuration &) {}
  void write(const eckit::Configuration &) const {}
  double norm() const { return 0.0; }
 private:
  void print(std::ostream &) const override {}
};
}  // namespace ijedi
