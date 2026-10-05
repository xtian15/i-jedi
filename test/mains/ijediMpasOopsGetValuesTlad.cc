/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <cmath>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include <nlohmann/json.hpp>
#include "eckit/config/LocalConfiguration.h"
#include "eckit/config/YAMLConfiguration.h"
#include "eckit/filesystem/PathName.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Traits.h"
#include "oops/base/Geometry.h"
#include "oops/base/GetValues.h"
#include "oops/base/Increment.h"
#include "oops/base/Model.h"
#include "oops/base/PostProcessor.h"
#include "oops/base/State.h"
#include "oops/generic/instantiateModelFactory.h"
#include "oops/interface/GeoVaLs.h"
#include "oops/interface/LinearVariableChange.h"
#include "oops/interface/LocalInterpolator.h"
#include "oops/interface/ModelAuxControl.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"
#include "oops/util/missingValues.h"
#include "oops/util/TimeWindow.h"
#include "MpasTestObservations.h"

namespace {
using Json = nlohmann::json;
using Arrays = ijedi::MpasAnalysisArrays;
using Increment = oops::Increment<ijedi::Traits>;
using State = oops::State<ijedi::Traits>;
using GeoVaLs = oops::GeoVaLs<synthetic_obs::Traits>;

void require(bool test, const std::string &message) {
  if (!test) {
    throw std::runtime_error(message);
  }
}
std::string precise(long double value) {
  std::ostringstream out;
  out << std::scientific << std::setprecision(30) << value;
  return out.str();
}
Json manifest(const Arrays &arrays) {
  Json output;
  for (const auto &[name, array] : arrays) {
    ijedi::AtlasOperatorReceipt hash;
    for (double value : array.values) {
      hash.real(value);
    }
    output[name] = {{"shape", array.shape}, {"sha256", hash.finish()}};
  }
  return output;
}
Arrays observationArrays(const GeoVaLs &values, const oops::Variables &variables) {
  Arrays result;
  for (const auto &var : variables) {
    const auto &matrix = values.geovals().values(var.name());
    ijedi::MpasAnalysisArray array{
        {static_cast<size_t>(matrix.cols()), static_cast<size_t>(matrix.rows())}, {}};
    for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
      for (Eigen::Index row = 0; row < matrix.rows(); ++row) {
        array.values.push_back(matrix(row, column));
      }
    }
    result.emplace(var.name(), std::move(array));
  }
  return result;
}

class GetValuesTlad final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &configuration) const override {
    oops::instantiateModelFactory<ijedi::Traits>();
    const oops::Geometry<ijedi::Traits> geometry(
        eckit::LocalConfiguration(configuration, "geometry"), getComm());
    State state(geometry, eckit::LocalConfiguration(configuration, "initial condition"));
    oops::Model<ijedi::Traits> model(geometry, eckit::LocalConfiguration(configuration, "model"));
    const oops::ModelAuxControl<ijedi::Traits> aux(
        geometry, eckit::LocalConfiguration(configuration, "model aux control"));
    const oops::Variables geovars(configuration.getStringVector("typed variables"));
    const oops::Variables controls(std::vector<std::string>{
        "control_eastward_wind", "control_northward_wind", "control_upward_air_velocity",
        "control_moist_potential_temperature", "control_jacobian_dry_air_density",
        "control_moisture_tracers"});
    const auto levels = geometry.variableSizes(geovars);
    const eckit::YAMLConfiguration locations(
        eckit::PathName(configuration.getString("locations path")));
    const auto latitudes = locations.getDoubleVector("latitude_degrees"),
               longitudes = locations.getDoubleVector("longitude_degrees");
    require(latitudes.size() == 256 && longitudes.size() == 256,
            "OOPS TLAD requires the complete fixed 256-target seam/pole corpus");
    const auto interpolationConfig =
        eckit::LocalConfiguration(configuration, "local interpolation");
    const auto cached = geometry.geometry().mpasAtlasGeometry().pointOperator(
        latitudes, longitudes,
        geometry.geometry().modelData().getString("atlas_compiler_identity"));
    const std::string path = configuration.getString("analysis bridge output") + ".oops-tlad.json";
    std::ofstream cache(path + ".atlas-cache.json");
    cache << cached->serializeCache();
    require(static_cast<bool>(cache), "cannot write exact OOPS TLAD Atlas cache");
    cache.close();
    std::ofstream profile(path + ".profile.jsonl");
    require(static_cast<bool>(profile), "cannot write OOPS TLAD phase timings");
    Json output{{"scope", "real_OOPS_spatial_TLAD_not_model_time_adjoint_or_UFO"},
                {"atlas_compiler_identity",
                 geometry.geometry().modelData().getString("atlas_compiler_identity")},
                {"atlas_cache_key", cached->cacheKey()},
                {"atlas_cache_receipt", cached->cacheReceipt()},
                {"cases", Json::array()},
                {"single_wind_cases", Json::array()},
                {"adjoints", Json::array()}};
    int zeroResponses = 0;
    for (int generation = 0; generation < 3; ++generation) {
      {
        oops::LinearVariableChange<ijedi::Traits> change(geometry, eckit::LocalConfiguration());
        change.changeVarTraj(state, geovars);
        for (int seed = 0; seed < 4; ++seed) {
          Increment control(geometry, controls, state.validTime());
          auto directions = control.increment().analysisArrays();
          int family = 0;
          for (const auto &var : controls) {
            auto &array = directions.at(var.name().substr(8));
            const int exponent = family < 3 ? 10 : (family == 3 ? 12 : 24);
            for (size_t i = 0; i < array.values.size(); ++i) {
              array.values[i] = std::ldexp(
                  static_cast<double>((i + seed * 11 + family * 7) % 37) - 18., -exponent);
            }
            ++family;
          }
          control.increment().replaceAnalysis(controls, directions);
          Increment tangent(control);
          change.changeVarTL(tangent, geovars);
          const auto measures = control.increment().analysisMeasures();
          for (int maskKind = 0; maskKind < 2; ++maskKind) {
            std::vector<bool> active(latitudes.size());
            std::vector<util::DateTime> times;
            for (size_t i = 0; i < active.size(); ++i) {
              active[i] = maskKind == 0 || i % 2 == 0;
              times.push_back(state.validTime() + util::Duration(active[i] ? "PT0S" : "PT1H"));
            }
            oops::SampledLocations<synthetic_obs::Traits> sampled(
                std::make_unique<synthetic_obs::SampledLocations>(latitudes, longitudes, times));
            oops::Locations<synthetic_obs::Traits> obsLocations(std::move(sampled));
            eckit::LocalConfiguration window;
            std::ostringstream begin, end;
            begin << state.validTime() - util::Duration("PT1M");
            end << state.validTime() + util::Duration("PT1M");
            window.set("begin", begin.str());
            window.set("end", end.str());
            oops::GetValues<ijedi::Traits, synthetic_obs::Traits> getValues(
                interpolationConfig, geometry, util::TimeWindow(window), obsLocations, geovars,
                geovars);
            getValues.initializeTL(model.timeResolution());
            oops::GetValues<ijedi::Traits, synthetic_obs::Traits>::preprocess(tangent);
            getValues.processTL(tangent);
            getValues.finalizeTL();
            GeoVaLs sampledTangent(obsLocations, geovars, levels);
            getValues.fillGeoVaLsTL(sampledTangent);
            const auto observed = observationArrays(sampledTangent, geovars);
            oops::LocalInterpolator<ijedi::Traits> local(interpolationConfig, geometry, latitudes,
                                                         longitudes);
            size_t count = 0;
            for (size_t level : levels) {
              count += latitudes.size() * level;
            }
            std::vector<double> localValues(count, util::missingValue<double>());
            local.apply(geovars, tangent, active, localValues);
            size_t offset = 0;
            for (const auto &var : geovars) {
              const auto &array = observed.at(var.name());
              for (size_t i = 0; i < array.values.size(); ++i) {
                require(array.values[i] == localValues.at(offset + i),
                        "GetValues TL ordering/mask differs from its public LocalInterpolator");
              }
              offset += array.values.size();
            }
            output["cases"].push_back(
                {{"generation", generation},
                 {"seed", seed},
                 {"mask", maskKind},
                 {"control", manifest(directions)},
                 {"geoval_tangent", manifest(tangent.increment().analysisArrays())},
                 {"observation_tangent", manifest(observed)}});
            for (int response = -1; response < static_cast<int>(geovars.size()); ++response) {
              const auto started = std::chrono::steady_clock::now();
              GeoVaLs seeds(obsLocations, geovars, levels);
              int field = 0;
              long double lhs = 0, scale = 0;
              for (const auto &var : geovars) {
                auto &matrix = seeds.geovals().values(var.name());
                for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
                  for (Eigen::Index row = 0; row < matrix.rows(); ++row) {
                    const size_t index = static_cast<size_t>(column * matrix.rows() + row);
                    const double value =
                        response < 0 || response == field
                            ? std::ldexp(
                                  static_cast<double>((index + seed * 11 + field * 7) % 37) - 18.,
                                  -6)
                            : 0.;
                    matrix(row, column) =
                        active[column] ? value : std::numeric_limits<double>::quiet_NaN();
                    if (active[column]) {
                      const long double product =
                          static_cast<long double>(observed.at(var.name()).values[index]) * value;
                      lhs += product;
                      scale += std::abs(product);
                    }
                  }
                }
                ++field;
              }
              Increment gradient(geometry, geovars, state.validTime());
              const auto constructed = std::chrono::steady_clock::now();
              getValues.fillGeoVaLsAD(seeds);
              getValues.finalizeAD(model.timeResolution());
              getValues.processAD(gradient);
              oops::GetValues<ijedi::Traits, synthetic_obs::Traits>::preprocessAD(gradient);
              const auto interpolated = std::chrono::steady_clock::now();
              if (generation == 0 && seed == 0 && maskKind == 0 && response == -1) {
                const auto measure = [&](const std::string &name, const auto &operation) {
                  const auto begin = std::chrono::steady_clock::now();
                  operation();
                  profile << Json{{"diagnostic_operation", name},
                                  {"seconds", std::chrono::duration<double>(
                                                  std::chrono::steady_clock::now() - begin)
                                                  .count()}}
                                 .dump()
                          << std::endl;
                };
                Arrays values, masses, native;
                measure("carrier_array_export",
                        [&] { values = gradient.increment().analysisArrays(); });
                measure("carrier_measures",
                        [&] { masses = gradient.increment().analysisMeasures(); });
                measure("carrier_serial_size", [&] { (void)gradient.serialSize(); });
                for (auto &[name, array] : values) {
                  for (size_t i = 0; i < array.values.size(); ++i) {
                    array.values[i] *= masses.at(name).values.at(i);
                  }
                }
                measure("installed_owner_geoval_vjp", [&] {
                  native = state.state().nativeGeovalVjp(values, gradient.variables());
                });
                measure("installed_owner_control_covector",
                        [&] { native = state.state().nativeCovectorsToControl(native); });
                measure("unused_destination_construction", [&] {
                  ijedi::Increment destination(geometry.geometry(), controls, state.validTime());
                });
                std::unique_ptr<ijedi::Increment> destination;
                measure("geoval_carrier_construction", [&] {
                  destination = std::make_unique<ijedi::Increment>(geometry.geometry(), geovars,
                                                                   state.validTime());
                });
                measure("replace_control_carrier",
                        [&] { destination->replaceAnalysis(controls, native); });
                std::shared_ptr<ijedi::MpasStateHandle> handle;
                const auto context = geometry.geometry().mpasContext();
                measure("owner_initial_state_clone", [&] { handle = context->initialState(); });
                measure("owner_metric_export", [&] { (void)context->analysisMeasures(*handle); });
                measure("owner_control_bindings", [&] {
                  for (const auto &var : controls) {
                    (void)context->variableBinding(var.name(), "control", "2026-07-17T06:00:00Z");
                  }
                });
              }
              change.changeVarAD(gradient, controls);
              const auto transformed = std::chrono::steady_clock::now();
              getValues.initializeAD();
              const auto returned = gradient.increment().analysisArrays();
              long double rhs = 0;
              for (const auto &[name, array] : directions) {
                const auto &mass = measures.at(name).values;
                const size_t repeat = array.values.size() / mass.size();
                for (size_t i = 0; i < array.values.size(); ++i) {
                  rhs += static_cast<long double>(array.values[i]) * mass.at(i / repeat) *
                         returned.at(name).values.at(i);
                }
              }
              const double error =
                  scale > 0 ? static_cast<double>(std::abs(lhs - rhs) / scale) : 0.;
              if (scale == 0) {
                require(lhs == 0 && rhs == 0 && gradient.norm() == 0,
                        "zero/static response has a spurious adjoint");
                ++zeroResponses;
              }
              require(std::isfinite(error) && error <= 2.e-12,
                      "real OOPS weighted transform/interpolation adjoint failed");
              output["adjoints"].push_back({{"generation", generation},
                                            {"seed", seed},
                                            {"mask", maskKind},
                                            {"response", response},
                                            {"control_adjoint", manifest(returned)},
                                            {"lhs", precise(lhs)},
                                            {"rhs", precise(rhs)},
                                            {"absolute_product_sum", precise(scale)},
                                            {"relative_residual", error}});
              profile << Json{{"generation", generation},
                              {"seed", seed},
                              {"mask", maskKind},
                              {"response", response},
                              {"seed_and_carrier_seconds",
                               std::chrono::duration<double>(constructed - started).count()},
                              {"interpolation_ad_seconds",
                               std::chrono::duration<double>(interpolated - constructed).count()},
                              {"model_transform_ad_seconds",
                               std::chrono::duration<double>(transformed - interpolated).count()},
                              {"proof_seconds", std::chrono::duration<double>(
                                                    std::chrono::steady_clock::now() - transformed)
                                                    .count()}}
                             .dump()
                      << std::endl;
            }
            // A private companion is necessary but its presence is not proof:
            // exercise actual OOPS sampling and reverse transport for each
            // separately requested wind, against the complete response above.
            for (int windFamily : {7, 8}) {
              const std::string name = geovars[windFamily].name();
              const oops::Variables oneVariable(std::vector<std::string>{name});
              const auto oneLevels = geometry.variableSizes(oneVariable);
              Increment oneWind(control);
              change.changeVarTL(oneWind, oneVariable);
              require(oneWind.variables() == oneVariable &&
                          oneWind.increment().analysisArrays().count("eastward_wind") == 1 &&
                          oneWind.increment().analysisArrays().count("northward_wind") == 1,
                      "single wind request lost its private transport companion");
              oops::GetValues<ijedi::Traits, synthetic_obs::Traits> oneValues(
                  interpolationConfig, geometry, util::TimeWindow(window), obsLocations,
                  oneVariable, oneVariable);
              oneValues.initializeTL(model.timeResolution());
              oops::GetValues<ijedi::Traits, synthetic_obs::Traits>::preprocess(oneWind);
              oneValues.processTL(oneWind);
              oneValues.finalizeTL();
              GeoVaLs singleTangent(obsLocations, oneVariable, oneLevels);
              oneValues.fillGeoVaLsTL(singleTangent);
              const auto sampled = observationArrays(singleTangent, oneVariable);
              require(manifest(sampled).at(name) == manifest(observed).at(name),
                      "single-wind OOPS TL differs from the authenticated complete response");
              GeoVaLs singleSeed(obsLocations, oneVariable, oneLevels);
              auto &matrix = singleSeed.geovals().values(name);
              for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
                for (Eigen::Index row = 0; row < matrix.rows(); ++row) {
                  const size_t index = static_cast<size_t>(column * matrix.rows() + row);
                  matrix(row, column) =
                      active[column]
                          ? std::ldexp(
                                static_cast<double>((index + seed * 11 + windFamily * 7) % 37) -
                                    18.,
                                -6)
                          : std::numeric_limits<double>::quiet_NaN();
                }
              }
              Increment singleGradient(geometry, oneVariable, state.validTime());
              oneValues.fillGeoVaLsAD(singleSeed);
              oneValues.finalizeAD(model.timeResolution());
              oneValues.processAD(singleGradient);
              oops::GetValues<ijedi::Traits, synthetic_obs::Traits>::preprocessAD(singleGradient);
              change.changeVarAD(singleGradient, controls);
              oneValues.initializeAD();
              const auto gradientManifest = manifest(singleGradient.increment().analysisArrays());
              const size_t reference =
                  ((generation * 4 + seed) * 2 + maskKind) * 13 + windFamily + 1;
              require(gradientManifest == output["adjoints"].at(reference).at("control_adjoint"),
                      "single-wind OOPS AD differs from its authenticated isolated response");
              output["single_wind_cases"].push_back({{"generation", generation},
                                                     {"seed", seed},
                                                     {"mask", maskKind},
                                                     {"response", windFamily},
                                                     {"observation_tangent", manifest(sampled)},
                                                     {"control_adjoint", gradientManifest}});
            }
          }
        }
      }
      if (generation < 2) {
        oops::PostProcessor<State> post;
        model.forecast(state, aux, model.timeResolution(), post);
      }
    }
    require(output["cases"].size() == 24 && output["adjoints"].size() == 312 && zeroResponses == 72,
            "OOPS TLAD lost a generation/seed/mask/response family");
    output["zero_response_trials"] = zeroResponses;
    require(output["single_wind_cases"].size() == 48,
            "OOPS TLAD omitted a single-wind generation/seed/mask request");
    std::ofstream file(path);
    file << output.dump(2) << '\n';
    require(static_cast<bool>(file), "cannot write OOPS TLAD proof");
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasOopsGetValuesTlad"; }
};
}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  GetValuesTlad app(oops::mpi::world());
  return run.execute(app);
}
