/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include "atlas/field.h"
#include "ijedi/Increment/IncrementBackend.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "oops/base/Variables.h"
#include "oops/util/DateTime.h"
#include "oops/util/Duration.h"

namespace ijedi {
class Geometry;

// The Atlas FieldSet is the sole increment authority. Transfers to owner APIs
// are short-lived copies, never another stored numerical state. Its four fixed
// metric fields come exclusively from the model-owned support/geometry API.
class MpasIncrementBackend final : public IncrementBackend {
 public:
  MpasIncrementBackend(const Geometry &, const oops::Variables &, const util::DateTime &);
  bool isMpas() const override { return true; }
  MpasIncrementBackend &typed() override { return *this; }
  const MpasIncrementBackend &typed() const override { return *this; }
  const util::DateTime &validTime() const { return time_; }
  const oops::Variables &variables() const { return variables_; }
  const oops::Variables &carriedVariables() const { return carriedVariables_; }
  static oops::Variables expandVectorDependencies(const MpasBackendContext &,
                                                 const oops::Variables &);
  // Routing only; the installed owner still validates every descriptor/binding.
  static std::string namespaceFor(const MpasBackendContext &, const oops::Variables &);
  const std::string &nameSpace() const { return namespace_; }
  void updateTime(const util::Duration &);
  void zero();
  void fill(double);
  void random();
  void scale(double);
  void axpy(double, const MpasIncrementBackend &, bool checkTime = true);
  void schur(const MpasIncrementBackend &);
  void squareRoot();
  double dot(const MpasIncrementBackend &) const;
  double norm() const;
  atlas::FieldSet &fieldSet();
  const atlas::FieldSet &fieldSet() const;
  void fromFieldSet(const atlas::FieldSet &);
  void toFieldSet(atlas::FieldSet &) const;
  MpasAnalysisArrays arrays() const;
  // Inject the requested native/control subspace into the model's full
  // inventory with exact zero directions. No additional stored authority.
  MpasAnalysisArrays completeArrays() const;
  MpasAnalysisArrays measures() const;
  void setArrays(const MpasAnalysisArrays &);
  void validate() const;
  size_t serialSize() const;
  void serialize(std::vector<double> &) const;
  void deserialize(const std::vector<double> &, size_t &);

 private:
  void compatible(const MpasIncrementBackend &, bool) const;
  atlas::FieldSet allocate(const MpasAnalysisArrays &) const;
  std::string envelope() const;
  std::string serializationMetadata() const;
  size_t serializationBytes() const;
  const Geometry &geom_;
  oops::Variables variables_;
  oops::Variables carriedVariables_;
  util::DateTime time_;
  std::string namespace_;
  std::map<std::string, std::string> bindings_, metricsFor_;
  MpasAnalysisArrays metrics_;
  atlas::FieldSet fields_;
};
}  // namespace ijedi
