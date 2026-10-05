/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

// Real embedded owner calls precede State/Increment and TL/AD abstractions.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include <nlohmann/json.hpp>
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"
#include "oops/mpi/mpi.h"

namespace {
using Json = nlohmann::json;
using Arrays = ijedi::MpasAnalysisArrays;

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

ijedi::MpasAnalysisArray direction(std::vector<size_t> shape, int seed, int family, int exponent) {
  size_t size = 1;
  for (size_t extent : shape) {
    size *= extent;
  }
  ijedi::MpasAnalysisArray result{std::move(shape), std::vector<double>(size)};
  for (size_t i = 0; i < size; ++i) {
    result.values[i] =
        std::ldexp(static_cast<double>((i + seed * 11 + family * 7) % 37) - 18., -exponent);
  }
  return result;
}

std::string precise(long double value) {
  std::ostringstream result;
  result << std::scientific << std::setprecision(30) << value;
  return result.str();
}

class AnalysisBridge final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &configuration) const override {
    const eckit::LocalConfiguration geometry(configuration, "geometry");
    ijedi::MpasBackendContext context(geometry);
    auto state = context.initialState();
    const std::vector<std::string> names = configuration.getStringVector("typed variables");
    const std::vector<std::string> times{"2026-07-17T06:00:00Z", "2026-07-17T06:12:00Z",
                                         "2026-07-17T06:24:00Z"};
    Json output;
    output["scope"] = "embedded_model_owned_analysis_not_OOPS_TLAD_or_model_time_adjoint";
    output["cases"] = Json::array();
    for (int generation = 0; generation < 3; ++generation) {
      const auto nativeValues = context.nativeAnalysisValues(*state);
      const auto measures = context.analysisMeasures(*state);
      const size_t cells = context.horizontalSnapshot().cells;
      const size_t levels = context.numberLevels();
      const std::vector<std::string> controlNames{"eastward_wind",
                                                  "northward_wind",
                                                  "upward_air_velocity",
                                                  "moist_potential_temperature",
                                                  "jacobian_dry_air_density",
                                                  "moisture_tracers"};
      const std::vector<std::string> metricNames{"cell_layer", "cell_layer", "cell_interface",
                                                 "cell_layer", "cell_layer", "cell_layer"};
      for (int seed = 0; seed < 4; ++seed) {
        Arrays control;
        for (size_t family = 0; family < controlNames.size(); ++family) {
          const int exponent = family < 3 ? 10 : (family == 3 ? 12 : 24);
          auto shape = std::vector<size_t>{cells, levels + static_cast<size_t>(family == 2)};
          if (family == 5) {
            shape.push_back(6);
          }
          control.emplace(controlNames[family], direction(shape, seed, family, exponent));
        }
        const auto native = context.controlToNative(*state, control);
        const auto tangent = context.nativeGeovalJvp(*state, native, names, times[generation]);
        Arrays seeds;
        for (size_t family = 0; family < names.size(); ++family) {
          seeds.emplace(names[family], direction(tangent.at(names[family]).shape, seed, family, 6));
        }
        const auto covectors = context.nativeGeovalVjp(*state, seeds, names, times[generation]);
        const auto adjoint = context.nativeCovectorsToControl(*state, covectors);
        long double lhs = 0, rhs = 0, norm = 0;
        for (const auto &name : names) {
          const auto &x = tangent.at(name).values, &y = seeds.at(name).values;
          for (size_t i = 0; i < x.size(); ++i) {
            const long double product = static_cast<long double>(x[i]) * y[i];
            lhs += product;
            norm += std::abs(product);
          }
        }
        for (size_t family = 0; family < controlNames.size(); ++family) {
          const auto &x = control.at(controlNames[family]).values;
          const auto &y = adjoint.at(controlNames[family]).values;
          const auto &mass = measures.at(metricNames[family]).values;
          for (size_t i = 0; i < x.size(); ++i) {
            rhs += static_cast<long double>(x[i]) * mass[family == 5 ? i / 6 : i] * y[i];
          }
        }
        const double error = static_cast<double>(std::abs(lhs - rhs) / norm);
        if (!std::isfinite(error) || norm <= 0 || error > 2.e-12) {
          throw std::runtime_error("embedded owner weighted P/native/GeoVaL adjoint failed");
        }
        output["cases"].push_back({{"generation", generation},
                                   {"seed", seed},
                                   {"native_values", manifest(nativeValues)},
                                   {"measures", manifest(measures)},
                                   {"control", manifest(control)},
                                   {"native_directions", manifest(native)},
                                   {"geoval_tangents", manifest(tangent)},
                                   {"native_covectors", manifest(covectors)},
                                   {"control_adjoint", manifest(adjoint)},
                                   {"lhs", precise(lhs)},
                                   {"rhs", precise(rhs)},
                                   {"absolute_product_sum", precise(norm)},
                                   {"adjoint_relative_residual", error}});
      }
      const std::string before = context.continuationManifest(*state);
      auto replaced = context.clone(*state);
      context.replaceNativeAnalysis(*replaced, nativeValues);
      if (context.continuationManifest(*replaced) != before ||
          context.stateGeneration(*replaced) != context.stateGeneration(*state) + 1) {
        throw std::runtime_error(
            "embedded identity replacement changed continuation or lost invalidation");
      }
      auto exactValues = nativeValues;
      for (auto &[name, array] : exactValues) {
        const size_t slot = name == "w" ? 1 : 0;
        array.values.at(slot) =
            std::nextafter(array.values.at(slot), std::numeric_limits<double>::infinity());
      }
      exactValues.at("w").values.at(0) = -0.;
      context.replaceNativeAnalysis(*replaced, exactValues);
      if (manifest(context.nativeAnalysisValues(*replaced)) != manifest(exactValues) ||
          context.stateGeneration(*replaced) != context.stateGeneration(*state) + 2) {
        throw std::runtime_error("embedded absolute assignment rounded or lost native bytes");
      }
      auto zero = nativeValues;
      for (auto &[name, array] : zero) {
        std::fill(array.values.begin(), array.values.end(), 0.);
      }
      auto incremented = context.clone(*state);
      context.addNativeAnalysis(*incremented, zero);
      if (context.continuationManifest(*incremented) != before) {
        throw std::runtime_error("embedded zero increment changed exact continuation");
      }
      int rejections = 0;
      auto invalid = nativeValues;
      invalid.at("theta_m").values[0] = 0.;
      try {
        context.replaceNativeAnalysis(*state, invalid);
      } catch (const std::exception &) {
        ++rejections;
      }
      invalid = nativeValues;
      invalid.erase("w");
      try {
        context.addNativeAnalysis(*state, invalid);
      } catch (const std::exception &) {
        ++rejections;
      }
      invalid = nativeValues;
      invalid.at("scalars").shape.back() = 5;
      try {
        context.replaceNativeAnalysis(*state, invalid);
      } catch (const std::exception &) {
        ++rejections;
      }
      invalid = nativeValues;
      invalid.at("rho_zz").values.at(0) = std::numeric_limits<double>::quiet_NaN();
      try {
        context.replaceNativeAnalysis(*state, invalid);
      } catch (const std::exception &) {
        ++rejections;
      }
      if (rejections != 4 || context.continuationManifest(*state) != before ||
          context.stateGeneration(*state) != static_cast<std::uint64_t>(generation)) {
        throw std::runtime_error("embedded native rejection was not atomic");
      }
      output["native_update_checks"].push_back({{"generation", generation},
                                                {"exact_absolute_assignment", true},
                                                {"identity_continuation", true},
                                                {"zero_increment_continuation", true},
                                                {"atomic_rejections", rejections}});
      if (generation < 2) {
        context.advance(*state, context.timeStepSeconds());
      }
    }
    std::ofstream file(configuration.getString("analysis bridge output"));
    file << output.dump(2) << '\n';
    if (!file) {
      throw std::runtime_error("cannot write embedded analysis bridge evidence");
    }
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasAnalysisBridge"; }
};
}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  AnalysisBridge app(oops::mpi::world());
  return run.execute(app);
}
