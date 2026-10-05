#pragma once

#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include "atlas/field.h"
#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"
#include "oops/util/Duration.h"

#include "oops/base/ParameterTraitsVariables.h"
#include "oops/util/parameters/OptionalParameter.h"
#include "oops/util/parameters/Parameter.h"
#include "oops/util/parameters/Parameters.h"
#include "oops/util/parameters/RequiredParameter.h"

#include "ijedi/Io/IoBase.h"
#include "ijedi/State/StateBackend.h"

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
class Increment;

// -----------------------------------------------------------------------------------------------

class AnalyticICParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(AnalyticICParameters, Parameters)
 public:
  // Analytic initial condition parameters
  oops::RequiredParameter<std::string> method{"method", this};
};

// -----------------------------------------------------------------------------------------------

class StateParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(StateParameters, Parameters)
 public:
  oops::OptionalParameter<util::DateTime> datetime{"datetime", this};
  oops::OptionalParameter<oops::Variables> stateVariables{"state variables", this};
  // Analytic initial condition parameters
  oops::OptionalParameter<AnalyticICParameters> analytic{"analytic init", this};
  // Io parameters wrapper for polymorphic IO parameters (nested under "io" key)
  oops::OptionalParameter<IoParametersWrapper> io{"io", this};
};

// -----------------------------------------------------------------------------------------------

class StateWriteParameters : public oops::Parameters {
  OOPS_CONCRETE_PARAMETERS(StateWriteParameters, Parameters)
 public:
  // Io parameters for writing (nested under "io" key)
  oops::OptionalParameter<IoParametersWrapper> io{"io", this};
};

// -----------------------------------------------------------------------------------------------

class State : public util::Printable, private util::ObjectCounter<State> {
 public:
  static std::string classname() { return "ijedi::State"; }

  State(const Geometry &, const eckit::Configuration &);
  State(const Geometry &, const oops::Variables &, const util::DateTime &, bool initToZero = true);
  State(const Geometry &, const State &);
  State(const oops::Variables &, const State &);
  State(const State &);

  ~State();

  State &operator=(const State &);
  State &operator+=(const Increment &);
  const util::DateTime validTime() const { return backend_->validTime(); }
  void updateTime(const util::Duration &dt) { backend_->updateTime(dt); }
  const oops::Variables &variables() const { return backend_->variables(); }
  void zero() { backend_->zero(); }
  void accumul(double weight, const State &other) { backend_->accumul(weight, *other.backend_); }
  double norm() const { return backend_->norm(); }
  bool isMpas() const { return backend_->isMpas(); }
  const Geometry &geometry() const { return geom_; }
  bool hasFieldSet() const { return backend_->hasFieldSet(); }
  void advanceModel(const util::Duration &dt) { backend_->advanceModel(dt); }
  std::string regressionManifest() const { return backend_->regressionManifest(); }
  std::string continuationManifest() const { return backend_->continuationManifest(); }
  std::string typedTransformManifest() const { return backend_->typedTransformManifest(); }
  std::vector<MpasTypedField> materializeTypedFields(const oops::Variables &variables) const {
    return backend_->materializeTypedFields(variables);
  }

  atlas::FieldSet &fieldSet() { return backend_->fieldSet(); }
  const atlas::FieldSet &fieldSet() const { return backend_->fieldSet(); }
  void toFieldSet(atlas::FieldSet &fset) const { backend_->toFieldSet(fset); }
  void fromFieldSet(const atlas::FieldSet &fset) { backend_->fromFieldSet(fset); }

  size_t serialSize() const { return backend_->serialSize(); }
  void serialize(std::vector<double> &buffer) const { backend_->serialize(buffer); }
  void deserialize(const std::vector<double> &buffer, size_t &index) {
    backend_->deserialize(buffer, index);
  }

  void transpose(const State &, const eckit::mpi::Comm &, int ensNum, int transNum);

  void read(const eckit::Configuration &);
  void write(const eckit::Configuration &) const;

  void setAtlasFieldMetadata();

 private:
  void analytic_init(const eckit::Configuration &);

  void print(std::ostream &os) const override;

  const Geometry &geom_;
  std::unique_ptr<StateBackend> backend_;
};

// -----------------------------------------------------------------------------------------------

}  // namespace ijedi
