/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <ostream>

#include "eckit/config/LocalConfiguration.h"
#include "oops/base/Variables.h"
#include "oops/util/Printable.h"

namespace ijedi {
class Geometry;

class ModelData : public util::Printable {
 public:
  explicit ModelData(const Geometry &);
  static oops::Variables defaultVariables() { return oops::Variables(); }
  eckit::LocalConfiguration modelData() const { return data_; }
 private:
  void print(std::ostream &) const override;
  eckit::LocalConfiguration data_;
};
}  // namespace ijedi
