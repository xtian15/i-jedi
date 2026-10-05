#pragma once

#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include "atlas/field.h"
#include "oops/base/FieldSet3D.h"
#include "oops/base/LocalIncrement.h"
#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"
#include "oops/util/Duration.h"

#include "oops/base/ParameterTraitsVariables.h"
#include "oops/util/parameters/OptionalParameter.h"
#include "oops/util/parameters/Parameter.h"
#include "oops/util/parameters/Parameters.h"
#include "oops/util/parameters/RequiredParameter.h"

#include "ijedi/Io/IoBase.h"
#include "ijedi/Increment/IncrementBackend.h"
#include "ijedi/Python/MpasBackendContext.h"

namespace eckit {
class Configuration;
}  // namespace eckit

namespace oops {
class Variables;
}  // namespace oops

namespace util {
class DateTime;
}  // namespace util

namespace ijedi {

class Geometry;
class GeometryIterator;
class State;

// -----------------------------------------------------------------------------------------------

class DiracParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(DiracParameters, Parameters)
 public:
  // lon/lat dirac specification (grid-agnostic: nearest owned node on any
  // functionspace is found via a KD-tree). Levels are 1-based.
  oops::RequiredParameter<std::vector<double>> lon{"lon", this};
  oops::RequiredParameter<std::vector<double>> lat{"lat", this};
  oops::RequiredParameter<std::vector<int>> level{"level", this};
  oops::RequiredParameter<std::vector<std::string>> variable{"variable", this};
};

// -----------------------------------------------------------------------------------------------

class IncrementParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(IncrementParameters, Parameters)
 public:
  // Io parameters wrapper for polymorphic IO parameters (nested under "io" key)
  oops::OptionalParameter<IoParametersWrapper> io{"io", this};
};

// -----------------------------------------------------------------------------------------------

class IncrementWriteParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(IncrementWriteParameters, Parameters)
 public:
  // Io parameters for writing (nested under "io" key)
  oops::OptionalParameter<IoParametersWrapper> io{"io", this};
};

// -----------------------------------------------------------------------------------------------

class Increment : public util::Printable, private util::ObjectCounter<Increment> {
 public:
  static std::string classname() { return "ijedi::Increment"; }

  Increment(const Geometry &, const oops::Variables &, const util::DateTime &);
  Increment(const Geometry &, const Increment &, const bool ad = false);
  Increment(const Increment &, const bool copy = true);
  ~Increment();

  Increment &operator=(const Increment &);

  const util::DateTime validTime() const;
  void updateTime(const util::Duration &);
  const oops::Variables &variables() const;

  void diff(const State &, const State &);
  void zero();
  void zero(const util::DateTime &);
  void ones();
  void sqrt();
  Increment &operator+=(const Increment &);
  Increment &operator-=(const Increment &);
  Increment &operator*=(double);
  void axpy(double, const Increment &, bool checkTimesEqual = true);
  double dot_product_with(const Increment &) const;
  void schur_product_with(const Increment &);
  void random();
  void accumul(double, const State &);
  double norm() const;
  bool isMpas() const;
  oops::LocalIncrement getLocal(const GeometryIterator &) const;
  void setLocal(const oops::LocalIncrement &, const GeometryIterator &);

  atlas::FieldSet &fieldSet();
  const atlas::FieldSet &fieldSet() const;
  void toFieldSet(atlas::FieldSet &) const;
  void fromFieldSet(const atlas::FieldSet &);
  size_t serialSize() const;
  void serialize(std::vector<double> &) const;
  void deserialize(const std::vector<double> &, size_t &);

  void read(const eckit::Configuration &);
  void write(const eckit::Configuration &) const;
  void dirac(const eckit::Configuration &);
  const Geometry &geometry() const { return geom_; }
  MpasAnalysisArrays analysisArrays() const;
  MpasAnalysisArrays completeAnalysisArrays() const;
  MpasAnalysisArrays analysisMeasures() const;
  const std::string &analysisNamespace() const;
  const oops::Variables &analysisCarriedVariables() const;
  void replaceAnalysis(const oops::Variables &, const MpasAnalysisArrays &);

 private:
  void print(std::ostream &os) const override;

  const Geometry &geom_;
  oops::FieldSet3D &atlasFields() { return backend_->atlasFields(); }
  const oops::FieldSet3D &atlasFields() const { return backend_->atlasFields(); }
  std::unique_ptr<IncrementBackend> backend_;
};

// -----------------------------------------------------------------------------------------------

}  // namespace ijedi
