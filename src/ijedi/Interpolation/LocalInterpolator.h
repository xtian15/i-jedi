/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "oops/util/Printable.h"

#if __has_include("oops/generic/SourceProximityPartitioner.h")
#include "oops/generic/SourceProximityPartitioner.h"
#define IJEDI_HAS_OOPS_TARGET_PARTITIONER 1
#endif

namespace eckit {
class Configuration;
}

namespace oops {
class UnstructuredInterpolator;
class Variables;
}

namespace ijedi {
class Geometry;
class Increment;
class State;

#ifdef IJEDI_HAS_OOPS_TARGET_PARTITIONER
using LocalTargetPartitioner = oops::SourceProximityPartitioner;
#else
class LocalTargetPartitioner {
 public:
  int interpolatingTask(double, double) const { return 0; }
};
#endif

// Preserve existing non-MPAS interpolation. PR 1 exposes Atlas MPAS operators
// directly; typed OOPS sampling is supplied and qualified by the stacked PR 2.
class LocalInterpolator : public util::Printable {
 public:
  static const std::string classname() { return "ijedi::LocalInterpolator"; }
  LocalInterpolator(const eckit::Configuration &, const Geometry &,
                    const std::vector<double> &, const std::vector<double> &);
  ~LocalInterpolator() override;
  static LocalTargetPartitioner makeTargetPartitioner(const Geometry &);
  static void preprocess(State &);
  static void preprocess(Increment &);
  static void preprocessAD(Increment &);
  void apply(const oops::Variables &, const State &, const std::vector<bool> &,
             std::vector<double> &) const;
  void apply(const oops::Variables &, const Increment &, const std::vector<bool> &,
             std::vector<double> &) const;
  void applyAD(const oops::Variables &, Increment &, const std::vector<bool> &,
               const std::vector<double> &) const;

 private:
  void print(std::ostream &) const override;
  std::unique_ptr<oops::UnstructuredInterpolator> generic_;
};
}  // namespace ijedi
