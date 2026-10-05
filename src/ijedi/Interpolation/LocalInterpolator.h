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
#include "ijedi/Python/MpasBackendContext.h"

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
}  // namespace oops

namespace ijedi {

class Geometry;
class Increment;
class MpasAtlasPointOperator;
class State;

#ifdef IJEDI_HAS_OOPS_TARGET_PARTITIONER
using LocalTargetPartitioner = oops::SourceProximityPartitioner;
#else
class LocalTargetPartitioner {
 public:
  int interpolatingTask(double, double) const { return 0; }
};
#endif

/// I-JEDI interpolation socket. Existing geometries retain the OOPS generic
/// implementation. MPAS uses native Atlas dual triangles and an ordered target
/// PointCloud, with paired wind-basis transport through Atlas spherical-vector.
class LocalInterpolator : public util::Printable {
 public:
  static const std::string classname() { return "ijedi::LocalInterpolator"; }

  LocalInterpolator(const eckit::Configuration &, const Geometry &,
                    const std::vector<double> &latitudes, const std::vector<double> &longitudes);
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
  void applyArrays(const oops::Variables &, const MpasAnalysisArrays &, const std::vector<bool> &,
                   std::vector<double> &) const;

  bool mpas_ = false;
  size_t targetCount_ = 0;
  std::string cacheKey_;
  std::shared_ptr<const MpasAtlasPointOperator> mpasOperator_;
  std::vector<std::string> windNames_;
  std::string horizontalReceipt_;
  std::string bundleReceipt_, schemaDigest_;
  std::unique_ptr<oops::UnstructuredInterpolator> generic_;
};

}  // namespace ijedi
