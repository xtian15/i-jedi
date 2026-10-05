/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include "atlas/library/Library.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"

#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Increment/MpasIncrementBackend.h"
#include "ijedi/Interpolation/LocalInterpolator.h"
#include "ijedi/LinearVariableChange/LinearVariableChange.h"
#include "ijedi/State/State.h"
#include "oops/base/Variable.h"
#include "oops/base/Variables.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {

void require(bool valid, const std::string &message) {
  if (!valid) throw std::runtime_error(message);
}

bool sameBytes(const std::vector<double> &a, const std::vector<double> &b) {
  return a.size() == b.size() &&
      (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

void rejectedFor(const std::function<void()> &action, const std::string &reason) {
  try { action(); }
  catch (const std::exception &error) {
    if (std::string(error.what()).find(reason) != std::string::npos) return;
    throw std::runtime_error("request failed for the wrong reason: " + std::string(error.what()));
  }
  throw std::runtime_error("request was silently accepted: " + reason);
}

void rejected(const std::function<void()> &action) {
  rejectedFor(action, "variable request metadata mismatch");
}

class VariableRequests final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &config) const override {
    const ijedi::Geometry geometry(config.getSubConfiguration("geometry"), getComm());
    const ijedi::State state(geometry, config.getSubConfiguration("initial condition"));
    const auto time = state.validTime();
    const auto boundary = state.continuationManifest();
    const eckit::LocalConfiguration empty;
    const ijedi::LocalInterpolator interpolator(empty, geometry, {0.}, {0.});
    const ijedi::LocalInterpolator noTargets(empty, geometry, {}, {});
    const oops::Variables valid(std::vector<std::string>{"air_temperature", "eastward_wind"});
    ijedi::Increment carrier(geometry, valid, time);
    ijedi::LinearVariableChange change(geometry, empty);
    change.changeVarTraj(state, valid);
    std::vector<double> carrierBefore;
    carrier.serialize(carrierBefore);
    const std::vector<double> untouched{-0., std::numeric_limits<double>::quiet_NaN(), 17.};
    size_t attacks = 0;
    size_t inverseMetadataAttacks = 0;
    for (const std::string name : {"air_temperature", "eastward_wind", "northward_wind"}) {
      for (const auto &variable : std::vector<oops::Variable>{
               oops::Variable(name, oops::VariableMetaData(), 1),
               oops::Variable(name, oops::VariableMetaData(), -2),
               oops::Variable(name, oops::VariableMetaData(
                   oops::VerticalStagger::CENTER, oops::ModelDataType::Real32)),
               oops::Variable(name, oops::VariableMetaData(oops::VerticalStagger::INTERFACE)),
               oops::Variable(name, oops::VariableMetaData(
                   oops::VerticalStagger::CENTER, oops::ModelDataType::Real64,
                   oops::ModelVariableDomain::Ocean))}) {
        const oops::Variables requested(std::vector<oops::Variable>{variable});
        const auto expanded = ijedi::MpasIncrementBackend::expandVectorDependencies(*geometry.mpasContext(), requested);
        require(expanded[0] == variable && expanded[0].getLevels() == variable.getLevels(),
                "dependency expansion discarded requested metadata");
        rejected([&] { (void)geometry.variableSizes(requested); });
        rejected([&] { (void)state.materializeTypedFields(requested); });
        rejected([&] { ijedi::State invalid(geometry, requested, time, false); });
        rejected([&] { ijedi::State invalid(requested, state); });
        rejected([&] { ijedi::Increment invalid(geometry, requested, time); });
        auto buffer = untouched;
        rejected([&] { interpolator.apply(requested, state, {true}, buffer); });
        require(sameBytes(buffer, untouched), "rejected NL request changed output bytes");
        rejected([&] { interpolator.apply(requested, state, {false}, buffer); });
        require(sameBytes(buffer, untouched), "masked rejected NL request changed output bytes");
        rejected([&] { noTargets.apply(requested, state, {}, buffer); });
        require(sameBytes(buffer, untouched), "empty-target rejected request cleared output");
        rejected([&] { interpolator.apply(requested, carrier, {true}, buffer); });
        require(sameBytes(buffer, untouched), "rejected TL request changed output bytes");
        rejected([&] { interpolator.applyAD(requested, carrier, {true}, untouched); });
        rejected([&] { change.changeVarTraj(state, requested); });
        rejected([&] { change.changeVarTL(carrier, requested); });
        rejected([&] { change.changeVarAD(carrier, requested); });
        rejected([&] { (void)state.nativeGeovalJvp({}, requested); });
        rejected([&] { (void)state.nativeGeovalVjp({}, requested); });
        rejected([&] { carrier.replaceAnalysis(requested, {}); });
        rejected([&] { change.changeVarInverseTL(carrier, requested); });
        rejected([&] { change.changeVarInverseAD(carrier, requested); });
        inverseMetadataAttacks += 2;
        std::vector<double> after;
        carrier.serialize(after);
        require(sameBytes(carrierBefore, after), "rejected metadata changed carrier values/time");
        require(carrier.variables() == valid, "rejected metadata changed requested inventory");
        require(state.continuationManifest() == boundary, "metadata rejection changed native authority");
        attacks += 16;
      }
    }
    // Positive name-only and explicit canonical requests retain every bit and
    // preserve requested order. The hidden wind companion remains a carrier,
    // never a silently added observation variable.
    const oops::Variables explicitVars(std::vector<oops::Variable>{
        oops::Variable("air_temperature", oops::VariableMetaData(), 55),
        oops::Variable("eastward_wind", oops::VariableMetaData(), 55)});
    std::vector<double> named, explicitBuffer;
    interpolator.apply(valid, state, {true}, named);
    interpolator.apply(explicitVars, state, {true}, explicitBuffer);
    require(sameBytes(named, explicitBuffer) && named.size() == 110,
            "canonical explicit metadata changed sampling/order");
    ijedi::Increment explicitCarrier(geometry, explicitVars, time);
    require(explicitCarrier.analysisCarriedVariables().size() == 3 &&
            explicitCarrier.variables().size() == 2 &&
            explicitCarrier.variables()[0].getLevels() == 55,
            "valid wind dependency expansion lost metadata or changed visible inventory");
    // Census every GeoVaL descriptor ID, not just the two wind spellings that
    // exposed the defect. IDs must fail consistently before masked/empty work.
    const auto registry = nlohmann::json::parse(geometry.mpasContext()->variableRegistryManifest());
    size_t rejectedNames = 0;
    for (const auto &[name, descriptor] : registry.at("descriptors").items()) {
      if (descriptor.at("namespace") != "geoval") continue;
      const oops::Variables requested(std::vector<std::string>{name});
      const auto rejectName = [&](const auto &action) {
        rejectedFor(action, "unsupported MPAS public GeoVaL name");
      };
      rejectName([&] { (void)geometry.variableSizes(requested); });
      rejectName([&] { (void)state.materializeTypedFields(requested); });
      rejectName([&] { ijedi::State bad(geometry, requested, time, false); });
      rejectName([&] { ijedi::State bad(requested, state); });
      rejectName([&] { ijedi::Increment bad(geometry, requested, time); });
      auto buffer = untouched;
      for (bool active : {true, false}) {
        rejectName([&] { interpolator.apply(requested, state, {active}, buffer); });
        rejectName([&] { interpolator.apply(requested, carrier, {active}, buffer); });
        rejectName([&] { interpolator.applyAD(requested, carrier, {active}, untouched); });
        require(sameBytes(buffer, untouched), "rejected name changed output bytes");
      }
      rejectName([&] { noTargets.apply(requested, state, {}, buffer); });
      rejectName([&] { noTargets.apply(requested, carrier, {}, buffer); });
      rejectName([&] { noTargets.applyAD(requested, carrier, {}, {}); });
      rejectName([&] { change.changeVarTraj(state, requested); });
      rejectName([&] { change.changeVarTL(carrier, requested); });
      rejectName([&] { change.changeVarAD(carrier, requested); });
      rejectName([&] { change.changeVarInverseTL(carrier, requested); });
      rejectName([&] { change.changeVarInverseAD(carrier, requested); });
      rejectName([&] { carrier.replaceAnalysis(requested, {}); });
      rejectName([&] { (void)state.nativeGeovalJvp({}, requested); });
      rejectName([&] { (void)state.nativeGeovalVjp({}, requested); });
      require(sameBytes(buffer, untouched) && state.continuationManifest() == boundary,
              "name census changed output or native authority");
      std::vector<double> after;
      carrier.serialize(after);
      require(sameBytes(after, carrierBefore), "name census changed tangent carrier");
      ++rejectedNames;
    }
    const auto aliases = geometry.mpasContext()->geovalPublicNames();
    require(aliases.size() == 12 && rejectedNames >= aliases.size(), "GeoVaL census omitted a family");
    const oops::Variables allAliases(aliases);
    require(state.materializeTypedFields(allAliases).size() == aliases.size(),
            "owner public GeoVaL inventory cannot materialize");
    ijedi::Increment aliasCarrier(geometry, allAliases, time);
    aliasCarrier.ones();
    int countAttacks = 0;
    const auto counts = geometry.variableSizes(allAliases);
    const std::vector<double> countSentinel(
        std::accumulate(counts.begin(), counts.end(), size_t{0}), -319.);
    for (auto field : aliasCarrier.fieldSet()) {
      for (const bool levels : {true, false}) {
        const auto original = levels ? field.levels() : field.variables();
        if (levels) field.set_levels(original + 1);
        else field.set_variables(original + 1);
        for (const bool active : {true, false}) {
          auto output = countSentinel;
          rejectedFor([&] { interpolator.apply(allAliases, aliasCarrier, {active}, output); },
                      "Atlas level/component count");
          rejectedFor([&] { interpolator.applyAD(allAliases, aliasCarrier, {active}, countSentinel); },
                      "Atlas level/component count");
          require(sameBytes(output, countSentinel), "RB4 rejected TL count changed output");
          countAttacks += 2;
        }
        auto output = countSentinel;
        rejectedFor([&] { noTargets.apply(allAliases, aliasCarrier, {}, output); },
                    "Atlas level/component count");
        rejectedFor([&] { noTargets.applyAD(allAliases, aliasCarrier, {}, {}); },
                    "Atlas level/component count");
        require(sameBytes(output, countSentinel), "RB4 empty count attack changed output");
        countAttacks += 2;
        if (levels) field.set_levels(original);
        else field.set_variables(original);
      }
    }
    require(countAttacks == 144, "RB4 TL/AD count attacks were omitted");
    int nameAttacks = 0;
    std::vector<double> aliasBefore;
    aliasCarrier.serialize(aliasBefore);
    auto aliasFields = aliasCarrier.fieldSet();
    for (size_t index = 0; index < aliasFields.size(); ++index) {
      const auto canonical = aliasFields[index];
      const std::string name = canonical.name();
      for (bool replace : {false, true}) {
        auto changed = replace ? canonical.clone() : canonical;
        if (replace) aliasFields[index] = changed;
        if (replace) changed.rename("invalid_actual_name");
        else changed.metadata().set("name", "invalid_actual_name");
        for (const bool active : {true, false}) {
          auto output = countSentinel;
          rejectedFor([&] { interpolator.apply(allAliases, aliasCarrier, {active}, output); },
                      "MPAS Increment field name changed");
          rejectedFor([&] { interpolator.applyAD(allAliases, aliasCarrier, {active}, countSentinel); },
                      "MPAS Increment field name changed");
          require(sameBytes(output, countSentinel), "RB4 rejected TL name changed output");
          nameAttacks += 2;
        }
        auto output = countSentinel;
        rejectedFor([&] { noTargets.apply(allAliases, aliasCarrier, {}, output); },
                    "MPAS Increment field name changed");
        rejectedFor([&] { noTargets.applyAD(allAliases, aliasCarrier, {}, {}); },
                    "MPAS Increment field name changed");
        require(sameBytes(output, countSentinel), "RB4 empty name attack changed output");
        nameAttacks += 2;
        if (replace) aliasFields[index] = canonical;
        else changed.metadata().set("name", name);
        std::vector<double> after;
        aliasCarrier.serialize(after);
        require(sameBytes(after, aliasBefore), "RB4 rejected names changed carrier bytes");
      }
    }
    require(nameAttacks == 144, "RB4 TL/AD actual-name attacks were omitted");
    std::cout << "RB4: " << nameAttacks << " actual-name TL/AD rejections pass\n";
    std::vector<double> aliasNl, aliasTl;
    interpolator.apply(allAliases, state, {true}, aliasNl);
    interpolator.apply(allAliases, aliasCarrier, {true}, aliasTl);
    interpolator.applyAD(allAliases, aliasCarrier, {true}, aliasTl);
    noTargets.apply(allAliases, state, {}, aliasNl);
    noTargets.apply(allAliases, aliasCarrier, {}, aliasTl);
    noTargets.applyAD(allAliases, aliasCarrier, {}, {});
    require(aliasNl.empty() && aliasTl.empty(), "valid empty-target sampling changed");

    // Real pole/seam vector replay: scalar application is an explicit wrong
    // oracle. Exercise paired and single-component requests and masked seeds.
    const std::vector<double> lat{89.8, -89.8, 0., 0.}, lon{40., 140., 179.999, -179.999};
    const ijedi::LocalInterpolator polar(empty, geometry, lat, lon);
    const auto op = geometry.mpasAtlasGeometry().pointOperator(lat, lon, atlas::Library::instance().gitsha1(40));
    const auto wind = geometry.mpasContext()->geovalWindNames();
    const oops::Variables paired(wind);
    const auto fields = state.materializeTypedFields(paired);
    const auto nlWind = op->applyVector(fields[0].values, fields[1].values, 55);
    ijedi::Increment unitWind(geometry, paired, time);
    unitWind.ones();
    const auto arrays = unitWind.analysisArrays();
    const auto vector = op->applyVector(arrays.at(wind[0]).values, arrays.at(wind[1]).values, 55);
    const auto scalar = op->apply(arrays.at(wind[0]).values, 55);
    double wrongOperator = 0.;
    for (size_t i = 0; i < scalar.size(); ++i) wrongOperator = std::max(wrongOperator, std::abs(scalar[i] - vector[0][i]));
    require(wrongOperator > 1., "wind oracle negative control is degenerate");
    size_t windCases = 0;
    for (const auto &names : std::vector<std::vector<std::string>>{wind, {wind[0]}, {wind[1]}, {wind[1], wind[0]}}) {
      const oops::Variables requested(names);
      for (const auto &mask : std::vector<std::vector<bool>>{{true, true, true, true}, {true, false, false, true}, {false, false, false, false}}) {
        std::vector<double> nl(names.size() * lat.size() * 55, -17.), tl(nl), expectedNl(nl), expectedTl(nl);
        for (size_t field = 0; field < names.size(); ++field) {
          const size_t component = names[field] == wind[0] ? 0 : 1;
          for (size_t target = 0; target < lat.size(); ++target) {
            if (!mask[target]) continue;
            for (size_t level = 0; level < 55; ++level) {
              const size_t row = target * 55 + level, slot = field * lat.size() * 55 + row;
              expectedNl[slot] = nlWind[component][row];
              expectedTl[slot] = vector[component][row];
            }
          }
        }
        polar.apply(requested, state, mask, nl);
        polar.apply(requested, unitWind, mask, tl);
        require(sameBytes(nl, expectedNl) && sameBytes(tl, expectedTl), "wind NL/TL differs from Atlas vector replay");
        ijedi::Increment gradient(geometry, requested, time);
        const auto masses = gradient.analysisMeasures();
        std::vector<double> eastSeed(lat.size() * 55, 0.), northSeed(eastSeed), seeds(tl.size(), 1.);
        for (const auto &name : names) {
          auto &seed = name == wind[0] ? eastSeed : northSeed;
          for (size_t target = 0; target < lat.size(); ++target) {
            if (mask[target]) std::fill(seed.begin() + target * 55, seed.begin() + (target + 1) * 55, 1.);
          }
        }
        const auto transpose = op->applyVectorTranspose(eastSeed, northSeed, 55);
        polar.applyAD(requested, gradient, mask, seeds);
        const auto actual = gradient.analysisArrays();
        for (size_t component = 0; component < 2; ++component) {
          auto expected = transpose[component];
          for (size_t i = 0; i < expected.size(); ++i) expected[i] /= masses.at(wind[component]).values[i];
          require(sameBytes(actual.at(wind[component]).values, expected), "wind AD differs from Atlas vector transpose");
        }
        ++windCases;
      }
    }
    // RA5: only exact ordered typed identity is an MPAS inverse. Every entry
    // point has the same trajectory/time/schema preflight; rectangular maps reject.
    size_t inverseControls = 0;
    for (bool adjoint : {false, true}) {
      const auto invoke = [&](ijedi::LinearVariableChange &operator_, ijedi::Increment &input,
                               const oops::Variables &output) {
        if (adjoint) operator_.changeVarInverseAD(input, output);
        else operator_.changeVarInverseTL(input, output);
      };
      invoke(change, carrier, valid);
      std::vector<double> after;
      carrier.serialize(after);
      require(sameBytes(after, carrierBefore), "inverse identity changed carrier bytes");
      ijedi::LinearVariableChange unbound(geometry, empty);
      rejectedFor([&] { invoke(unbound, carrier, valid); }, "matching owned trajectory/time/support");
      ijedi::Increment future(geometry, valid, time + util::Duration("PT12M"));
      std::vector<double> futureBefore;
      future.serialize(futureBefore);
      rejectedFor([&] { invoke(change, future, valid); }, "matching owned trajectory/time/support");
      after.clear();
      future.serialize(after);
      require(sameBytes(after, futureBefore), "inverse wrong-time rejection changed carrier");
      for (const auto &names : std::vector<std::vector<std::string>>{{"air_temperature"}, {"eastward_wind", "air_temperature"}, {"control_eastward_wind"}, {"u"}}) {
        rejectedFor([&] { invoke(change, carrier, oops::Variables(names)); }, "exact typed identity only");
        after.clear();
        carrier.serialize(after);
        require(sameBytes(after, carrierBefore), "inverse rejection changed carrier bytes");
      }
      inverseControls += 7;
    }
    {
      const ijedi::Geometry otherGeometry(config.getSubConfiguration("incompatible geometry"), getComm());
      ijedi::Increment foreign(otherGeometry, valid, time);
      foreign.ones();
      std::vector<double> before, after;
      foreign.serialize(before);
      rejectedFor([&] { change.changeVarInverseTL(foreign, valid); }, "matching owned trajectory/time/support");
      rejectedFor([&] { change.changeVarInverseAD(foreign, valid); }, "matching owned trajectory/time/support");
      rejectedFor([&] { change.changeVarTL(foreign, valid); }, "matching owned trajectory/time/support");
      rejectedFor([&] { change.changeVarAD(foreign, valid); }, "matching owned trajectory/time/support");
      foreign.serialize(after);
      require(sameBytes(before, after), "wrong-owner transform rejection changed carrier");
      inverseControls += 4;
    }
    for (const std::string space : {"native", "control"}) {
      auto names = geometry.mpasContext()->analysisInventory(space);
      if (space == "control") for (auto &name : names) name = "control_" + name;
      const oops::Variables inventory(names);
      ijedi::Increment identity(geometry, inventory, time);
      identity.ones();
      std::vector<double> before, after;
      identity.serialize(before);
      change.changeVarInverseTL(identity, inventory);
      change.changeVarInverseAD(identity, inventory);
      identity.serialize(after);
      require(sameBytes(before, after), "native/control inverse identity changed bytes");
      const oops::Variables subset(std::vector<std::string>{names[0]});
      rejectedFor([&] { change.changeVarInverseTL(identity, subset); }, "exact typed identity only");
      rejectedFor([&] { change.changeVarInverseAD(identity, subset); }, "exact typed identity only");
      after.clear();
      identity.serialize(after);
      require(sameBytes(before, after), "native/control inverse selection changed bytes");
      inverseControls += 4;
    }
    require(attacks == 240 && inverseMetadataAttacks == 30 && windCases == 12 && inverseControls == 26,
            "required old/new variable attacks were omitted");
    std::cout << "RA1/RA5: " << rejectedNames << " descriptor IDs reject consistently; "
              << aliases.size() << " public names; " << windCases << " wind replay cases; "
              << inverseMetadataAttacks << " inverse metadata and " << inverseControls << " inverse controls pass\n";
    std::cout << "AR2: " << attacks << " public-ingress rejections; exact positive sampling passed\n";
    return 0;
  }
 private:
  std::string appname() const override { return "ijedi::MpasVariableRequests"; }
};

}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  const VariableRequests application(oops::mpi::world());
  return run.execute(application);
}
