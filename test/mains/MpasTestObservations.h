/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>
#include "Eigen/Core"
#include "eckit/exception/Exceptions.h"
#include "oops/base/Locations.h"
#include "oops/base/Variables.h"
#include "oops/util/DateTime.h"
#include "oops/util/Printable.h"

namespace synthetic_obs {

class SampledLocations;
class GeoVaLs;
class ObsSpace;

struct Traits {
  using SampledLocations = synthetic_obs::SampledLocations;
  using GeoVaLs = synthetic_obs::GeoVaLs;
  using ObsSpace = synthetic_obs::ObsSpace;
};

class SampledLocations final : public util::Printable {
 public:
  SampledLocations(std::vector<double> latitudes, std::vector<double> longitudes,
                   std::vector<util::DateTime> times)
      : latitudes_(std::move(latitudes)),
        longitudes_(std::move(longitudes)),
        times_(std::move(times)) {
    if (latitudes_.size() != longitudes_.size() || latitudes_.size() != times_.size()) {
      throw eckit::BadValue("synthetic observation locations have inconsistent extents", Here());
    }
  }

  const std::vector<double> &latitudes() const { return latitudes_; }
  const std::vector<double> &longitudes() const { return longitudes_; }
  const std::vector<util::DateTime> &times() const { return times_; }

 private:
  void print(std::ostream &os) const override {
    os << "synthetic sampled locations: " << times_.size();
  }

  std::vector<double> latitudes_;
  std::vector<double> longitudes_;
  std::vector<util::DateTime> times_;
};

class ObsSpace {};

class GeoVaLs final : public util::Printable {
 public:
  GeoVaLs(const oops::Locations<Traits> &locations, const oops::Variables &variables,
          const std::vector<size_t> &sizes) {
    if (variables.size() != sizes.size()) {
      throw eckit::BadValue("synthetic GeoVaLs variable-size mismatch", Here());
    }
    const size_t nlocations = locations.samplingMethod(0).times().size();
    size_t variableIndex = 0;
    for (const oops::Variable &variable : variables) {
      Eigen::MatrixXd matrix(sizes.at(variableIndex++), nlocations);
      matrix.setConstant(std::numeric_limits<double>::quiet_NaN());
      values_.emplace(variable.name(), std::move(matrix));
    }
  }

  void fill(const oops::Variable &variable, const Eigen::Ref<const Eigen::VectorX<size_t>> &indices,
            const Eigen::Ref<const Eigen::MatrixXd> &values, bool levelsTopDown) {
    if (levelsTopDown) {
      throw eckit::BadValue("MPAS synthetic GeoVaLs unexpectedly received top-down levels", Here());
    }
    Eigen::MatrixXd &destination = values_.at(variable.name());
    if (values.rows() != destination.rows() || values.cols() != indices.size()) {
      throw eckit::BadValue("synthetic GeoVaLs fill extent mismatch", Here());
    }
    for (Eigen::Index column = 0; column < indices.size(); ++column) {
      if (indices[column] >= static_cast<size_t>(destination.cols())) {
        throw eckit::BadValue("synthetic GeoVaLs fill index is out of range", Here());
      }
      destination.col(indices[column]) = values.col(column);
    }
  }

  const Eigen::MatrixXd &values(const std::string &name) const { return values_.at(name); }
  Eigen::MatrixXd &values(const std::string &name) { return values_.at(name); }
  void fillAD(const oops::Variable &variable,
              const Eigen::Ref<const Eigen::VectorX<size_t>> &indices,
              Eigen::Ref<Eigen::MatrixXd> destination, bool levelsTopDown) const {
    const auto &source = values_.at(variable.name());
    if (levelsTopDown || source.rows() != destination.rows() ||
        indices.size() != destination.cols()) {
      throw eckit::BadValue("synthetic GeoVaL AD rank/orientation differs", Here());
    }
    for (Eigen::Index column = 0; column < indices.size(); ++column) {
      if (indices[column] >= static_cast<size_t>(source.cols())) {
        throw eckit::BadValue("synthetic GeoVaL AD index differs", Here());
      }
      destination.col(column) = source.col(indices[column]);
    }
  }

 private:
  void print(std::ostream &os) const override { os << "synthetic GeoVaLs"; }

  std::map<std::string, Eigen::MatrixXd> values_;
};

}  // namespace synthetic_obs
