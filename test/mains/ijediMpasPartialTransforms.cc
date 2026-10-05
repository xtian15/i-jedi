/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Traits.h"
#include "oops/base/Geometry.h"
#include "oops/base/Increment.h"
#include "oops/base/Model.h"
#include "oops/base/PostProcessor.h"
#include "oops/base/State.h"
#include "oops/generic/instantiateModelFactory.h"
#include "oops/interface/LinearVariableChange.h"
#include "oops/interface/ModelAuxControl.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {
using Json = nlohmann::json;
using Arrays = ijedi::MpasAnalysisArrays;
using Increment = oops::Increment<ijedi::Traits>;
using State = oops::State<ijedi::Traits>;
void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
Json manifest(const Arrays &arrays) {
  Json result;
  for (const auto &[name, array] : arrays) {
    ijedi::AtlasOperatorReceipt hash;
    for (double value : array.values) {
      hash.real(value);
    }
    result[name] = {{"shape", array.shape}, {"sha256", hash.finish()}};
  }
  return result;
}
class PartialTransforms final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &config) const override {
    oops::instantiateModelFactory<ijedi::Traits>();
    const oops::Geometry<ijedi::Traits> geometry(eckit::LocalConfiguration(config, "geometry"),
                                                 getComm());
    State state(geometry, eckit::LocalConfiguration(config, "initial condition"));
    oops::Model<ijedi::Traits> model(geometry, eckit::LocalConfiguration(config, "model"));
    const oops::ModelAuxControl<ijedi::Traits> aux(
        geometry, eckit::LocalConfiguration(config, "model aux control"));
    const oops::Variables geovars(config.getStringVector("typed variables"));
    const std::vector<std::vector<std::string>> inventories{
        {"control_eastward_wind", "control_northward_wind", "control_upward_air_velocity",
         "control_moist_potential_temperature", "control_jacobian_dry_air_density",
         "control_moisture_tracers"},
        {"u", "w", "theta_m", "rho_zz", "scalars"}};
    int tlCases = 0, adCases = 0, stateCases = 0, negativeCases = 0;
    double maximumResidual = 0.;
    const bool nativeSocket = config.getBool("native P socket probe", false);
    for (int generation = 0; generation < 3; ++generation) {
      oops::LinearVariableChange<ijedi::Traits> change(geometry, eckit::LocalConfiguration());
      change.changeVarTraj(state, geovars);
      if (config.getBool("partial transform probe", false)) {
        Increment one(geometry, oops::Variables(std::vector<std::string>{"control_eastward_wind"}),
                      state.validTime());
        try {
          change.changeVarTL(one, geovars);
        } catch (const std::exception &error) {
          throw std::runtime_error(std::string("partial control TL rejected: ") + error.what());
        }
        return 0;
      }
      for (int seed = 0; seed < 4; ++seed) {
        if (nativeSocket) {
          const oops::Variables nativeVars(inventories.at(1));
          Increment nativeSeed(geometry, nativeVars, state.validTime());
          auto values = nativeSeed.increment().analysisArrays();
          const auto weights = nativeSeed.increment().analysisMeasures();
          int response = 0;
          for (auto &[name, array] : values) {
            for (size_t i = 0; i < array.values.size(); ++i) {
              array.values[i] =
                  std::ldexp(static_cast<double>((i + 11 * seed + 7 * response) % 37) - 18., -16);
            }
            ++response;
          }
          nativeSeed.increment().replaceAnalysis(nativeVars, values);
          auto euclidean = values;
          for (auto &[name, array] : euclidean) {
            const auto &mass = weights.at(name).values;
            const size_t repeat = array.values.size() / mass.size();
            for (size_t i = 0; i < array.values.size(); ++i) {
              array.values[i] *= mass.at(i / repeat);
            }
          }
          const auto expectedAdjoint = state.state().nativeCovectorsToControl(euclidean);
          for (size_t family = 0; family < inventories.front().size(); ++family) {
            const auto &name = inventories.front().at(family);
            const auto raw = name.substr(8);
            const oops::Variables oneVar(std::vector<std::string>{name});
            Increment direction(geometry, oneVar, state.validTime());
            auto arrays = direction.increment().analysisArrays();
            for (size_t i = 0; i < arrays.at(raw).values.size(); ++i) {
              arrays.at(raw).values[i] =
                  std::ldexp(static_cast<double>((i + 11 * seed + 7 * family) % 37) - 18., -16);
            }
            direction.increment().replaceAnalysis(oneVar, arrays);
            const auto expected =
                state.state().controlToNative(direction.increment().completeAnalysisArrays());
            Increment forward(direction);
            change.changeVarTL(forward, nativeVars);
            require(manifest(forward.increment().analysisArrays()) == manifest(expected),
                    "OOPS control-to-native differs from the installed owner: " + name);
            ++tlCases;
            Increment backward(nativeSeed);
            change.changeVarAD(backward, oneVar);
            require(manifest(backward.increment().analysisArrays()) ==
                        manifest(Arrays{{raw, expectedAdjoint.at(raw)}}),
                    "OOPS native-to-control covector differs from the installed owner: " + name);
            ++adCases;
            const double lhs = forward.dot_product_with(nativeSeed),
                         rhs = direction.dot_product_with(backward);
            const double residual =
                std::abs(lhs - rhs) / std::max({1., std::abs(lhs), std::abs(rhs)});
            require(std::isfinite(residual) && residual <= 2.e-14,
                    "OOPS P/P* weighted identity failed: " + name);
            maximumResidual = std::max(maximumResidual, residual);
            const std::vector<std::string> nativeKeys{"u",       "u",      "w",
                                                      "theta_m", "rho_zz", "scalars"};
            const auto &nativeKey = nativeKeys.at(family);
            const oops::Variables oneNative(std::vector<std::string>{nativeKey});
            Increment selectedForward(direction);
            change.changeVarTL(selectedForward, oneNative);
            require(manifest(selectedForward.increment().analysisArrays()) ==
                        manifest(Arrays{{nativeKey, expected.at(nativeKey)}}),
                    "OOPS partial native output differs from P: " + name);
            ++tlCases;
            Increment selectedSeed(geometry, oneNative, state.validTime());
            selectedSeed.increment().replaceAnalysis(oneNative,
                                                     Arrays{{nativeKey, values.at(nativeKey)}});
            auto sparseCovector = selectedSeed.increment().completeAnalysisArrays();
            const auto &mass = weights.at(nativeKey).values;
            auto &array = sparseCovector.at(nativeKey);
            const size_t repeat = array.values.size() / mass.size();
            for (size_t i = 0; i < array.values.size(); ++i) {
              array.values[i] *= mass.at(i / repeat);
            }
            const auto sparseExpected = state.state().nativeCovectorsToControl(sparseCovector);
            Increment selectedBackward(selectedSeed);
            change.changeVarAD(selectedBackward, oneVar);
            require(manifest(selectedBackward.increment().analysisArrays()) ==
                        manifest(Arrays{{raw, sparseExpected.at(raw)}}),
                    "OOPS partial native seed differs from zero embedding/P*: " + name);
            ++adCases;
            const double sparseLhs = selectedForward.dot_product_with(selectedSeed),
                         sparseRhs = direction.dot_product_with(selectedBackward);
            const double sparseResidual = std::abs(sparseLhs - sparseRhs) /
                                          std::max({1., std::abs(sparseLhs), std::abs(sparseRhs)});
            require(std::isfinite(sparseResidual) && sparseResidual <= 2.e-14,
                    "OOPS partial P/P* identity failed: " + name);
            maximumResidual = std::max(maximumResidual, sparseResidual);
          }
          const oops::Variables east(std::vector<std::string>{"control_eastward_wind"});
          const oops::Variables mixed(std::vector<std::string>{"control_eastward_wind", "u"});
          Increment controlSeed(geometry, east, state.validTime());
          const auto reject = [&](const Increment &source, const oops::Variables &target,
                                  bool adjoint, const std::string &reason) {
            Increment candidate(source);
            const auto before = manifest(candidate.increment().analysisArrays());
            const auto beforeVars = candidate.increment().variables();
            bool rejected = false;
            try {
              if (adjoint) {
                change.changeVarAD(candidate, target);
              } else {
                change.changeVarTL(candidate, target);
              }
            } catch (const std::exception &error) {
              rejected = std::string(error.what()).find(reason) != std::string::npos;
            }
            require(rejected && candidate.validTime() == source.validTime() &&
                        candidate.increment().variables() == beforeVars &&
                        manifest(candidate.increment().analysisArrays()) == before,
                    "unsupported-map rejection was absent, wrong, or non-atomic: " + reason);
            ++negativeCases;
          };
          reject(nativeSeed, east, false, "no native-to-control inverse map");
          reject(controlSeed, nativeVars, true, "no declared map for these spaces");
          reject(controlSeed, mixed, false, "cannot mix native/control/GeoVaL namespaces");
          reject(nativeSeed, mixed, true, "cannot mix native/control/GeoVaL namespaces");
          std::cout << "native P socket generation=" << generation << " seed=" << seed
                    << " TL=" << tlCases << " AD=" << adCases << std::endl;
          continue;
        }
        Increment outputSeed(geometry, geovars, state.validTime());
        auto outputArrays = outputSeed.increment().analysisArrays();
        int response = 0;
        for (auto &[name, array] : outputArrays) {
          for (size_t i = 0; i < array.values.size(); ++i) {
            array.values[i] =
                std::ldexp(static_cast<double>((i + 11 * seed + 7 * response) % 37) - 18., -16);
          }
          ++response;
        }
        outputSeed.increment().replaceAnalysis(geovars, outputArrays);
        for (const auto &names : inventories) {
          const bool control = names.front().rfind("control_", 0) == 0;
          const oops::Variables fullVars(names);
          Increment fullGradient(outputSeed);
          change.changeVarAD(fullGradient, fullVars);
          const auto fullCovectors = fullGradient.increment().analysisArrays();
          for (size_t family = 0; family < names.size(); ++family) {
            const std::string &name = names[family];
            const std::string raw = control ? name.substr(8) : name;
            const oops::Variables oneVar(std::vector<std::string>{name});
            Increment one(geometry, oneVar, state.validTime());
            auto direction = one.increment().analysisArrays();
            for (size_t i = 0; i < direction.at(raw).values.size(); ++i) {
              direction.at(raw).values[i] =
                  std::ldexp(static_cast<double>((i + 11 * seed + 7 * family) % 37) - 18.,
                             family < 3 ? -10 : -24);
            }
            one.increment().replaceAnalysis(oneVar, direction);
            Increment embedded(geometry, fullVars, state.validTime());
            auto fullDirections = embedded.increment().analysisArrays();
            fullDirections.at(raw) = direction.at(raw);
            embedded.increment().replaceAnalysis(fullVars, fullDirections);
            Increment reference(embedded), partial(one);
            change.changeVarTL(reference, geovars);
            change.changeVarTL(partial, geovars);
            require(manifest(reference.increment().analysisArrays()) ==
                        manifest(partial.increment().analysisArrays()),
                    "partial TL differs from exact explicit-zero embedding: " + name);
            ++tlCases;
            Increment adjoint(outputSeed);
            change.changeVarAD(adjoint, oneVar);
            require(manifest(adjoint.increment().analysisArrays()) ==
                        manifest(Arrays{{raw, fullCovectors.at(raw)}}),
                    "partial AD differs from full-covector projection: " + name);
            ++adCases;
            const double lhs = partial.dot_product_with(outputSeed),
                         rhs = one.dot_product_with(adjoint);
            const double residual =
                std::abs(lhs - rhs) / std::max({1., std::abs(lhs), std::abs(rhs)});
            require(std::isfinite(residual) && residual <= 2.e-14,
                    "partial transform weighted adjoint identity failed: " + name);
            maximumResidual = std::max(maximumResidual, residual);
            if (generation == 0 && seed == 0) {
              // An analysis update is a feasible physical state, unlike the
              // signed tangent direction at zero condensate. Keep the full
              // signed direction above for the TL/AD proof.
              if (raw == "scalars" || raw == "moisture_tracers") {
                auto feasible = direction;
                for (auto &value : feasible.at(raw).values) {
                  value = std::abs(value);
                }
                one.increment().replaceAnalysis(oneVar, feasible);
                fullDirections.at(raw) = feasible.at(raw);
                embedded.increment().replaceAnalysis(fullVars, fullDirections);
              }
              State a(state), b(state);
              a += one;
              b += embedded;
              require(a.state().continuationManifest() == b.state().continuationManifest() &&
                          a.validTime() == b.validTime(),
                      "partial State update differs from zero embedding: " + name);
              ++stateCases;
            }
          }
        }
        std::cout << "partial transform generation=" << generation << " seed=" << seed
                  << " TL=" << tlCases << " AD=" << adCases << std::endl;
      }
      if (generation < 2) {
        oops::PostProcessor<State> post;
        model.forecast(state, aux, util::Duration("PT12M"), post);
      }
    }
    require(tlCases == (nativeSocket ? 144 : 132) && adCases == (nativeSocket ? 144 : 132) &&
                stateCases == (nativeSocket ? 0 : 11) && negativeCases == (nativeSocket ? 48 : 0),
            "partial transform gate omitted an inventory, seed or authenticated state");
    std::ofstream output(config.getString("analysis bridge output") +
                         (nativeSocket ? ".native-p-socket.json" : ".partial-transforms.json"));
    output << Json{{"tl_cases", tlCases},
                   {"ad_cases", adCases},
                   {"state_updates", stateCases},
                   {"negative_controls", negativeCases},
                   {"weighted_adjoint_max", maximumResidual},
                   {"scope", nativeSocket
                                 ? "OOPS_control_native_P_Pstar_not_model_time_adjoint"
                                 : "partial_inventory_embedding_projection_not_model_time_adjoint"}}
                  .dump(2)
           << '\n';
    require(static_cast<bool>(output), "cannot write partial transform evidence");
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasPartialTransforms"; }
};
}  // namespace
int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  PartialTransforms app(oops::mpi::world());
  return run.execute(app);
}
