/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <ostream>
#include <vector>

#include "oops/util/Printable.h"

namespace eckit { class Configuration; }

namespace ijedi {
class Geometry;
class ModelAuxControl;

class ModelAuxIncrement : public util::Printable {
 public:
  ModelAuxIncrement(const Geometry &, const eckit::Configuration &) {}
  ModelAuxIncrement(const ModelAuxIncrement &, bool) {}
  ModelAuxIncrement(const ModelAuxIncrement &, const eckit::Configuration &) {}
  void diff(const ModelAuxControl &, const ModelAuxControl &) {}
  void zero() {}
  ModelAuxIncrement &operator=(const ModelAuxIncrement &) { return *this; }
  ModelAuxIncrement &operator+=(const ModelAuxIncrement &) { return *this; }
  ModelAuxIncrement &operator-=(const ModelAuxIncrement &) { return *this; }
  ModelAuxIncrement &operator*=(double) { return *this; }
  void axpy(double, const ModelAuxIncrement &) {}
  double dot_product_with(const ModelAuxIncrement &) const { return 0.0; }
  size_t serialSize() const { return 0; }
  void serialize(std::vector<double> &) const {}
  void deserialize(const std::vector<double> &, size_t &) {}
  void read(const eckit::Configuration &) {}
  void write(const eckit::Configuration &) const {}
  double norm() const { return 0.0; }
 private:
  void print(std::ostream &) const override {}
};
}  // namespace ijedi
