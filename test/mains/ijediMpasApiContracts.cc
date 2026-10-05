/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <functional>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include "atlas/array.h"
#include "atlas/mesh/Nodes.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/LocalInterpolator.h"

#include "eckit/config/LocalConfiguration.h"
#include "ijedi/State/State.h"
#include "ijedi/State/MpasFieldSetOwner.h"
#include "ijedi/Traits.h"
#include "oops/base/Geometry.h"
#include "oops/base/Model.h"
#include "ijedi/Model/Model.h"
#include "ijedi/ModelAux/ModelAuxControl.h"
#include "oops/base/Variable.h"
#include "oops/base/Variables.h"
#include "oops/generic/instantiateModelFactory.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {

void reject(const std::function<void()> &action, const std::string &reason) {
  try {
    action();
  } catch (const std::exception &error) {
    if (std::string(error.what()).find(reason) != std::string::npos) return;
    throw std::runtime_error("API attack failed for the wrong reason: " + std::string(error.what()));
  }
  throw std::runtime_error("API attack was accepted: " + reason);
}

class ApiContracts final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &config) const override {
    oops::instantiateModelFactory<ijedi::Traits>();
    const oops::Geometry<ijedi::Traits> geometry(config.getSubConfiguration("geometry"), getComm());
    const auto &native = geometry.geometry();
    const auto initialConfig = config.getSubConfiguration("initial condition");
    ijedi::State initial(native, initialConfig);
    const auto time = initial.validTime();
    const auto variables = initial.variables();
    reject([&] { ijedi::State zero(native, variables, time, true); }, "cannot be initialized to zero");
    reject([&] { ijedi::State zero(native, variables, time); }, "cannot be initialized to zero");
    reject([&] { ijedi::State wrong(native, variables, time + util::Duration("PT12M"), false); },
           "differs from the receipt-bound model start time");
    auto wrongConfig = initialConfig;
    wrongConfig.set("date", "2026-07-17T06:12:00Z");
    reject([&] { ijedi::State wrong(native, wrongConfig); },
           "differs from the receipt-bound model start time");
    const auto before = initial.continuationManifest();
    // Field replacement must not leave an observer pointing at a dead
    // FieldSet when an independently retained original handle is used later.
    for (int repeat = 0; repeat < 128; ++repeat) {
      atlas::Field retained("retained", atlas::array::make_datatype<double>(),
                              atlas::array::make_shape(2, 3));
      {
        atlas::FieldSet source;
        source.add(retained);
        auto owned = ijedi::ownMpasFieldSet(source);
        source = atlas::FieldSet();
        auto alias = owned;
        owned[0] = retained.clone();
        if (repeat % 2) owned.clear();
        owned = atlas::FieldSet();
        alias = atlas::FieldSet();
      }
      retained.rename("after-last-alias");
    }
    // Track later insertions as well as canonical members. The earlier narrow
    // owner retained only the originals and missed this displaced observer.
    for (int repeat = 0; repeat < 128; ++repeat) {
      atlas::Field inserted("inserted", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(2, 3));
      {
        atlas::FieldSet source;
        source.add(atlas::Field("canonical", atlas::array::make_datatype<double>(),
                                  atlas::array::make_shape(2, 3)));
        auto owned = ijedi::ownMpasFieldSet(source);
        auto alias = owned;
        owned.add(inserted);
        owned[1] = inserted.clone();
        if (repeat % 2) owned.clear();
        owned = atlas::FieldSet();
        alias = atlas::FieldSet();
      }
      inserted.rename("after-inserted-and-last-alias");
    }
    for (const auto &name : std::vector<std::string>{"u", "w", "scalars", "air_temperature"}) {
      for (const bool cached : {true, false}) {
        atlas::Field retained;
        atlas::FieldSet alias;
        {
          ijedi::State carrier(oops::Variables(std::vector<std::string>{name}), initial);
          if (cached) alias = carrier.fieldSet();
          else carrier.toFieldSet(alias);
          retained = alias[0];
          atlas::Field bad(retained.name(), atlas::array::make_datatype<float>(), retained.shape());
          bad.set_functionspace(retained.functionspace());
          alias[0] = bad;
          if (cached) {
            reject([&] { (void)carrier.fieldSet(); }, "wrong name, dtype, rank or source space");
          }
          if (carrier.continuationManifest() != before || carrier.validTime() != time) {
            throw std::runtime_error("Field replacement changed native authority/time");
          }
        }
        alias = atlas::FieldSet();
        retained.rename("after-State-and-last-view-alias");
      }
    }
    for (const bool cached : {true, false}) {
      atlas::Field inserted("invalid-extra", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(2, 3));
      atlas::FieldSet alias;
      {
        ijedi::State carrier(variables, initial);
        if (cached) alias = carrier.fieldSet();
        else carrier.toFieldSet(alias);
        alias.add(inserted);
        alias[alias.size() - 1] = inserted.clone();
        if (cached) {
          reject([&] { (void)carrier.fieldSet(); }, "cached MPAS State view inventory changed");
        }
        if (carrier.continuationManifest() != before || carrier.validTime() != time) {
          throw std::runtime_error("Inserted State view field changed native authority/time");
        }
      }
      alias = atlas::FieldSet();
      inserted.rename("after-State-and-inserted-view-alias");
    }
    {
      const oops::Variables diagnostic(std::vector<std::string>{"air_temperature"});
      ijedi::State target(diagnostic, initial);
      atlas::FieldSet visible;
      target.toFieldSet(visible);
      const auto copied = visible.field("air_temperature").clone();
      eckit::LocalConfiguration malformed;
      malformed.set("io", "intentionally invalid configuration");
      reject([&] { target.read(malformed); }, "MPAS State generic IO read is unsupported");
      reject([&] { target.write(malformed); }, "MPAS State generic IO write is unsupported");
      const auto current = target.fieldSet().field("air_temperature");
      if (target.continuationManifest() != before || target.validTime() != time ||
          std::memcmp(current.array().data(), copied.array().data(),
                      copied.size() * sizeof(double)) != 0) {
        throw std::runtime_error("RB2 IO rejection changed native authority/time/view bytes");
      }
      int countChecks = 0;
      for (const auto &names : std::vector<std::vector<std::string>>{
               {"u", "w", "theta_m", "rho_zz", "scalars"},
               {"air_temperature", "air_pressure_levels", "air_pressure_at_surface"}}) {
        ijedi::State carrier(oops::Variables(names), initial);
        atlas::FieldSet destination;
        carrier.toFieldSet(destination);
        for (auto field : carrier.fieldSet()) {
          for (const bool levels : {true, false}) {
            const auto correct = levels ? field.levels() : field.variables();
            const auto bytes = destination.field(field.name()).clone();
            if (levels) field.set_levels(correct + 1);
            else field.set_variables(correct + 1);
            reject([&] { (void)carrier.fieldSet(); }, "Atlas level/component count");
            reject([&] { carrier.toFieldSet(destination); }, "Atlas level/component count");
            if (std::memcmp(destination.field(field.name()).array().data(), bytes.array().data(),
                            bytes.size() * sizeof(double)) != 0 ||
                carrier.continuationManifest() != before || carrier.validTime() != time) {
              throw std::runtime_error("RB4 failed State export changed authority/output");
            }
            if (levels) field.set_levels(correct);
            else field.set_variables(correct);
            countChecks += 2;
          }
        }
      }
      if (countChecks != 32) throw std::runtime_error("RB4 State count attacks were omitted");
      auto iterator = native.begin();
      auto iteratorCopy = iterator;
      auto mesh = native.mpasAtlasGeometry().dualMesh();
      auto coordinates = atlas::array::make_view<double, 2>(mesh.nodes().lonlat());
      const auto first = *iterator;
      const double longitude = coordinates(0, 0);
      coordinates(0, 0) += .01;
      reject([&] { (void)native.begin(); }, "geometry storage changed");
      reject([&] { (void)*iterator; }, "geometry storage changed");
      reject([&] { (void)*iteratorCopy; }, "geometry storage changed");
      reject([&] { (void)native.geometryData(); }, "geometry storage changed");
      reject([&] { (void)ijedi::LocalInterpolator::makeTargetPartitioner(native); },
             "geometry storage changed");
      coordinates(0, 0) = longitude;
      if ((*iterator)[0] != first[0]) throw std::runtime_error("RB3 restored iterator differs");
      auto second = iterator;
      ++second;
      const auto secondRow = native.ownedNodeIndices().at(second.nodeIndex());
      const double secondLongitude = coordinates(secondRow, 0);
      coordinates(secondRow, 0) += .01;
      reject([&] { (void)*second; }, "geometry storage changed");
      coordinates(secondRow, 0) = secondLongitude;
      auto ghost = atlas::array::make_view<int, 1>(mesh.nodes().ghost());
      const int originalGhost = ghost(0);
      auto finishing = native.begin();
      ghost(0) ^= 1;
      reject([&] {
        while (finishing != native.end()) { (void)*finishing; ++finishing; }
      }, "geometry storage changed");
      ghost(0) = originalGhost;
      native.validateIterationBoundary();
      const auto start = std::chrono::steady_clock::now();
      size_t points = 0;
      for (auto it = native.begin(); it != native.end(); ++it) {
        const auto point = *it;
        const auto row = native.ownedNodeIndices().at(points++);
        if (point[0] != coordinates(row, 0) || point[1] != coordinates(row, 1)) {
          throw std::runtime_error("RB3 traversal returned a noncanonical coordinate");
        }
      }
      if (points != native.ownedNodeIndices().size()) {
        throw std::runtime_error("RB3 traversal omitted owned points");
      }
      std::cout << "RB3 authenticated traversal points=" << points << " seconds="
                << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
                << '\n';
      auto threeDimensional = config.getSubConfiguration("geometry");
      threeDimensional.set("iterator dimension", 3);
      const ijedi::Geometry levelsGeometry(threeDimensional, getComm());
      size_t levelPoints = 0;
      for (auto it = levelsGeometry.begin(); it != levelsGeometry.end(); ++it) {
        const auto point = *it;
        if (point[2] != static_cast<double>(levelPoints % 55)) {
          throw std::runtime_error("RB3 3D traversal lost canonical level indices");
        }
        ++levelPoints;
      }
      if (levelPoints != 55 * points) throw std::runtime_error("RB3 3D traversal omitted points");
    }
    eckit::LocalConfiguration modelConfig;
    modelConfig.set("name", "MPAS-PyTorch");
    modelConfig.set("time step", "PT12M");
    for (const std::vector<std::string> names : {
             std::vector<std::string>{"does_not_exist"}, {}, {"u", "u"},
             {"air_temperature"}, {"control_eastward_wind"}, {"qv"},
             {"native_edge_normal_wind"}}) {
      auto bad = modelConfig;
      bad.set("model variables", names);
      const std::string reason = names.empty() ? "MPAS model inventory must not be empty" :
          (names.size() == 2 ? "duplicate MPAS model variable" : "unsupported MPAS model variable");
      reject([&] { oops::Model<ijedi::Traits> invalid(geometry, bad); }, reason);
      if (initial.continuationManifest() != before || initial.validTime() != time) {
        throw std::runtime_error("model declaration rejection changed State");
      }
    }
    modelConfig.set("model variables", std::vector<std::string>{"u", "w", "scalars"});
    oops::Model<ijedi::Traits> supported(geometry, modelConfig);
    auto badStep = modelConfig;
    badStep.set("time step", "PT6M");
    reject([&] { oops::Model<ijedi::Traits> invalid(geometry, badStep); }, "differs from owner support");
    const oops::Variables badMetadata(std::vector<oops::Variable>{oops::Variable(
        "u", oops::VariableMetaData(), 1)});
    reject([&] { native.mpasContext()->validateModelVariables(badMetadata); },
           "variable request metadata mismatch");
    {
      const ijedi::Geometry otherGeometry(config.getSubConfiguration("incompatible geometry"), getComm());
      ijedi::State other(otherGeometry, initialConfig);
      const auto otherBefore = other.continuationManifest();
      if (other.geometry().mpasContext()->configurationReceipt() == native.mpasContext()->configurationReceipt()) {
        throw std::runtime_error("support attack did not construct an incompatible valid owner");
      }
      const ijedi::Model model(native, modelConfig);
      const ijedi::ModelAuxControl aux(native, eckit::LocalConfiguration());
      reject([&] { model.initialize(other); }, "owner support differ");
      reject([&] { model.step(other, aux); }, "owner support differ");
      if (other.continuationManifest() != otherBefore || other.validTime() != time) {
        throw std::runtime_error("incompatible owner rejection changed State");
      }
      auto otherModelConfig = modelConfig;
      otherModelConfig.set("time step", "PT6M");
      const ijedi::Model validOther(otherGeometry, otherModelConfig);
      validOther.initialize(other);
    }
    reject([&] { initial.updateTime(util::Duration("PT12M")); }, "only with the model");
    if (initial.validTime() != time || initial.continuationManifest() != before) {
      throw std::runtime_error("rejected clock update changed the complete State");
    }
    ijedi::State explicitInitial(native, variables, time, false);
    if (explicitInitial.continuationManifest() != before) {
      throw std::runtime_error("explicit initial construction changed the owner boundary");
    }
    for (const std::string request : {"Pa", "m", "hPa", "level index"}) {
      std::string units = request;
      reject([&] { (void)geometry.verticalCoord(units); }, "supports only");
      if (units != request) throw std::runtime_error("rejected vertical units were mutated");
    }
    for (const std::string request : {"", "model_level_index_bottom_to_top"}) {
      std::string units = request;
      const auto levels = geometry.verticalCoord(units);
      if (units != "model_level_index_bottom_to_top" || levels.size() != 55) {
        throw std::runtime_error("OOPS vertical coordinate label/extent mismatch");
      }
      for (size_t level = 0; level < levels.size(); ++level) {
        if (levels[level] != static_cast<double>(level)) {
          throw std::runtime_error("OOPS level index changed");
        }
      }
    }
    const std::vector<oops::Variable> invalid{
      oops::Variable("air_temperature", oops::VariableMetaData(), 1),
      oops::Variable("air_temperature", oops::VariableMetaData(), -2),
      oops::Variable("air_temperature", oops::VariableMetaData(
          oops::VerticalStagger::CENTER, oops::ModelDataType::Real32)),
      oops::Variable("air_temperature", oops::VariableMetaData(
          oops::VerticalStagger::CENTER, oops::ModelDataType::Int32)),
      oops::Variable("air_temperature", oops::VariableMetaData(oops::VerticalStagger::INTERFACE)),
      oops::Variable("air_temperature", oops::VariableMetaData(oops::VerticalStagger::CENTER_WITH_TOP)),
      oops::Variable("air_temperature", oops::VariableMetaData(
          oops::VerticalStagger::CENTER, oops::ModelDataType::Real64, oops::ModelVariableDomain::Ocean)),
      oops::Variable("air_temperature", oops::VariableMetaData(
          oops::VerticalStagger::CENTER, oops::ModelDataType::Real64, oops::ModelVariableDomain::Land))};
    for (const auto &variable : invalid) {
      const oops::Variables requested(std::vector<oops::Variable>{variable});
      reject([&] { (void)native.variableSizes(requested); }, "variable request metadata mismatch");
      reject([&] { (void)initial.materializeTypedFields(requested); }, "variable request metadata mismatch");
      reject([&] { ijedi::State bad(native, requested, time, false); }, "variable request metadata mismatch");
      reject([&] { ijedi::State bad(requested, initial); }, "variable request metadata mismatch");
      if (initial.continuationManifest() != before) {
        throw std::runtime_error("metadata rejection mutated the authoritative State");
      }
    }
    const oops::Variables pressure(std::vector<oops::Variable>{oops::Variable(
        "air_pressure_levels", oops::VariableMetaData(oops::VerticalStagger::INTERFACE), 56)});
    if (native.variableSizes(pressure) != std::vector<size_t>{56}) {
      throw std::runtime_error("canonical explicit interface request was rejected");
    }
    const auto physical = initial.materializeTypedFields(pressure);
    if (physical.size() != 1 || physical[0].levels != 56 || physical[0].units != "Pa") {
      throw std::runtime_error("physical pressure must remain available as a typed field");
    }
    std::cout << "AR1/AR4/RA3 public API contracts pass; two-step model outputs are separately tested\n";
    return 0;
  }
 private:
  std::string appname() const override { return "ijedi::MpasApiContracts"; }
};

}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  const ApiContracts application(oops::mpi::world());
  return run.execute(application);
}
