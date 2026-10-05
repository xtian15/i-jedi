/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <fstream>
#include <exception>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "pybind11/embed.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {
using Json = nlohmann::json;
void requireCompletedPythonTeardown() {
  if (Py_IsInitialized() != 0) {
    throw std::runtime_error("final MPAS owner destruction did not finalize embedded Python");
  }
  // This executable's retained CTests run under one actual MPI rank. The
  // reader may import mpi4py, but Python teardown must not finalize the MPI
  // environment initialized and still owned by OOPS/eckit.
  oops::mpi::world().barrier();
}
class HandleOwnership final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &configuration) const override {
    const auto config = eckit::LocalConfiguration(configuration, "geometry");
    auto first = std::make_unique<ijedi::MpasBackendContext>(config);
    {
      pybind11::gil_scoped_acquire acquire;
      if (!pybind11::module_::import("sys").attr("dont_write_bytecode").cast<bool>()) {
        throw std::runtime_error("embedded Python may modify immutable SDK bytecode caches");
      }
    }
    auto handle = first->initialState();
    const std::string time = "2026-07-17T06:00:00Z";
    if (configuration.getString("handle probe mode", "ownership") == "restart") {
      first.reset();
      handle.reset();
      bool rejected = false;
      try {
        const ijedi::MpasBackendContext unsupported(config);
      } catch (const std::logic_error &error) {
        rejected = std::string(error.what()) ==
                   "MPAS embedded runtime cannot restart after finalization";
      }
      if (!rejected) {
        throw std::runtime_error("MPAS runtime restart lacks its fail-closed capability preflight");
      }
      requireCompletedPythonTeardown();
      std::cout << "finalized MPAS runtime rejects restart before Python initialization\n";
      return 0;
    }
    if (configuration.getString("handle probe mode", "ownership") == "initial-boundary") {
      const auto pristine = first->continuationManifest(*handle);
      const auto sealed = first->serializeState(*handle, time);
      const auto fixed = first->analysisMeasures();
      if (fixed.size() != 4) {
        throw std::runtime_error("initial boundary omitted fixed analysis measures");
      }
      const auto fromHandle = first->analysisMeasures(*handle);
      for (const auto &[name, array] : fixed) {
        if (array.shape != fromHandle.at(name).shape ||
            array.values != fromHandle.at(name).values || array.values.empty()) {
          throw std::runtime_error("fixed and initial-handle analysis measures differ");
        }
        for (const auto value : array.values) {
          if (!std::isfinite(value) || value <= 0.) {
            throw std::runtime_error("initial analysis measures must be finite and positive");
          }
        }
      }
      auto sibling = first->initialState();
      if (first->stateGeneration(*sibling) != 0 ||
          first->serializeState(*sibling, time) != sealed) {
        throw std::runtime_error("repeated initial State changed its portable boundary");
      }
      auto replacement = first->nativeAnalysisValues(*sibling);
      replacement.at("u").values.at(0) += 1.e-7;
      first->replaceNativeAnalysis(*sibling, replacement);
      auto fresh = first->initialState();
      if (first->stateGeneration(*sibling) != 1 ||
          first->continuationManifest(*sibling) == pristine ||
          first->stateGeneration(*handle) != 0 || first->stateGeneration(*fresh) != 0 ||
          first->serializeState(*handle, time) != sealed ||
          first->serializeState(*fresh, time) != sealed) {
        throw std::runtime_error("initial State handles alias the owned portable boundary");
      }
      first.reset();
      handle.reset();
      sibling.reset();
      fresh.reset();
      requireCompletedPythonTeardown();
      std::cout << "three initial handles retain exact independent portable boundaries\n";
      return 0;
    }
    if (configuration.getString("handle probe mode", "ownership") == "generation") {
      const std::uint64_t maximum = std::uint64_t{1} << 53;
      const auto pristine = first->continuationManifest(*handle);
      const auto bytes = first->serializeState(*handle, time);
      std::string nearLimit;
      {
        pybind11::gil_scoped_acquire acquire;
        try {
          const auto contracts = pybind11::module_::import("mpas_pytorch.ijedi_contracts");
          pybind11::dict package;
          package["version"] = config.getString("mpas-pytorch version");
          package["source_commit"] = config.getString("mpas-pytorch source commit");
          package["wheel_sha256"] = config.getString("wheel sha256");
          const pybind11::bytes envelope(bytes);
          const auto schema = contracts.attr("inspect_continuation_envelope")(
              envelope, pybind11::arg("expected_package_identity") = package,
              pybind11::arg("expected_state_mode") = "compact_continuation",
              pybind11::arg("expected_valid_time") = time,
              pybind11::arg("expected_time_step_seconds") = first->timeStepSeconds(),
              pybind11::arg("expected_state_generation") = 0,
              pybind11::arg("expected_horizontal_geometry_receipt") =
                  first->horizontalGeometryReceipt(),
              pybind11::arg("expected_static_vertical_geometry_receipt") =
                  first->staticVerticalGeometryReceipt(),
              pybind11::arg("expected_bundle_receipt") = first->geometryReceipt(),
              pybind11::arg("expected_configuration_receipt") = first->configurationReceipt(),
              pybind11::arg("expected_state_schema_digest") = first->stateSchemaDigest());
          const auto boundary = contracts.attr("deserialize_continuation_envelope")(
              envelope, pybind11::arg("expected_schema") = schema,
              pybind11::arg("expected_package_identity") = package,
              pybind11::arg("expected_state_mode") = "compact_continuation",
              pybind11::arg("expected_valid_time") = time,
              pybind11::arg("expected_time_step_seconds") = first->timeStepSeconds(),
              pybind11::arg("expected_state_generation") = 0);
          nearLimit = contracts
                          .attr("serialize_continuation_envelope")(
                              boundary, pybind11::arg("schema") = schema,
                              pybind11::arg("package_identity") = package,
                              pybind11::arg("state_mode") = "compact_continuation",
                              pybind11::arg("valid_time") = time,
                              pybind11::arg("time_step_seconds") = first->timeStepSeconds(),
                              pybind11::arg("state_generation") = maximum - 1)
                          .cast<std::string>();
        } catch (const pybind11::error_already_set &error) {
          throw std::runtime_error(std::string("generation fixture owner error: ") + error.what());
        }
      }
      auto exhausted = first->deserializeState(nearLimit, time, maximum - 1);
      if (first->stateGeneration(*exhausted) != maximum - 1 ||
          first->continuationManifest(*exhausted) != pristine) {
        throw std::runtime_error("near-limit generation restore changed continuation/framing");
      }
      // One legal identity update reaches the exact framing maximum.
      auto zero = first->nativeAnalysisValues(*exhausted);
      for (auto &[name, array] : zero) {
        for (auto &value : array.values) {
          value = 0.;
        }
      }
      first->addNativeAnalysis(*exhausted, zero);
      if (first->stateGeneration(*exhausted) != maximum ||
          first->continuationManifest(*exhausted) != pristine) {
        throw std::runtime_error("last legal generation update lost exact framing/continuation");
      }
      const auto sealed = first->serializeState(*exhausted, time);
      auto roundtrip = first->deserializeState(sealed, time, maximum);
      if (first->stateGeneration(*roundtrip) != maximum ||
          first->continuationManifest(*roundtrip) != pristine) {
        throw std::runtime_error("maximum generation did not roundtrip exactly");
      }
      int rejected = 0;
      const auto reject = [&](const auto &call) {
        bool caught = false;
        try {
          call();
        } catch (const std::overflow_error &) {
          caught = true;
        }
        if (!caught || first->stateGeneration(*exhausted) != maximum ||
            first->continuationManifest(*exhausted) != pristine) {
          throw std::runtime_error("exhausted generation mutates state or admits overflow");
        }
        ++rejected;
      };
      reject([&] { first->advance(*exhausted, first->timeStepSeconds()); });
      reject([&] { first->addNativeAnalysis(*exhausted, {}); });
      reject([&] { first->replaceNativeAnalysis(*exhausted, {}); });
      reject([&] { (void)first->deserializeState("malformed", time, maximum + 1); });
      if (rejected != 4) {
        throw std::runtime_error("generation boundary gate omitted an attack");
      }
      std::cout << "generation 2^53-1 and 2^53 are exact; four overflow attacks are atomic\n";
      return 0;
    }
    if (configuration.getString("handle probe mode", "ownership") == "errors") {
      const auto before = first->continuationManifest(*handle);
      const auto generation = first->stateGeneration(*handle);
      int rejected = 0;
      std::exception_ptr retained;
      const auto attack = [&](const std::string &operation, const auto &call) {
        bool translated = false;
        try {
          call();
        } catch (const std::runtime_error &error) {
          translated = std::string(error.what()).find("MPAS " + operation + " failed:\n") == 0;
          retained = std::current_exception();
        }
        if (!translated || first->continuationManifest(*handle) != before ||
            first->stateGeneration(*handle) != generation) {
          throw std::runtime_error("owner error escaped untranslated or changed continuation");
        }
        ++rejected;
      };
      attack("control-to-native", [&] { (void)first->controlToNative(*handle, {}); });
      attack("native-to-control adjoint",
             [&] { (void)first->nativeCovectorsToControl(*handle, {}); });
      attack("native GeoVaL VJP",
             [&] { (void)first->nativeGeovalVjp(*handle, {}, {"air_pressure"}, time); });
      attack("native analysis replacement", [&] { first->replaceNativeAnalysis(*handle, {}); });
      attack("native analysis addition", [&] { first->addNativeAnalysis(*handle, {}); });
      attack("variable binding",
             [&] { (void)first->variableBinding("air_pressure", "native", time); });
      attack("variable binding", [&] { (void)first->variableBinding("unknown", "native", time); });
      // Retain an actual owner exception across destruction of the final
      // handle and Context. It must contain no surviving Python error object.
      first.reset();
      handle.reset();
      bool survived = false;
      try {
        std::rethrow_exception(retained);
      } catch (const std::runtime_error &error) {
        survived = std::string(error.what()).find("MPAS variable binding failed:\n") == 0;
      }
      if (!survived || rejected != 7) {
        throw std::runtime_error("translated owner exception did not survive interpreter teardown");
      }
      requireCompletedPythonTeardown();
      std::cout
          << "seven model rejections were atomic C++ errors; retained error survived teardown\n";
      return 0;
    }
    if (configuration.getString("handle probe mode", "ownership") != "lifetime") {
      auto second = std::make_unique<ijedi::MpasBackendContext>(config);
      const auto before = first->continuationManifest(*handle);
      std::vector<std::string> accepted, rejected;
      const auto attack = [&](const std::string &name, const auto &operation) {
        try {
          operation();
          accepted.push_back(name);
        } catch (const std::invalid_argument &error) {
          if (std::string(error.what()) !=
              "MPAS opaque State handle belongs to a different Context") {
            throw;
          }
          rejected.push_back(name);
        }
      };
      attack("clone", [&] { (void)second->clone(*handle); });
      attack("generation", [&] { (void)second->stateGeneration(*handle); });
      attack("norm", [&] { (void)second->norm(*handle, {"u"}); });
      attack("continuation", [&] { (void)second->continuationManifest(*handle); });
      attack("native_values", [&] { (void)second->nativeAnalysisValues(*handle); });
      attack("measures", [&] { (void)second->analysisMeasures(*handle); });
      attack("serialize", [&] { (void)second->serializeState(*handle, time); });
      attack("typed_manifest", [&] { (void)second->typedTransformManifest(*handle, time); });
      attack("typed_values",
             [&] { (void)second->materializeTypedFields(*handle, {"air_pressure"}, time); });
      attack("regression", [&] { (void)second->regressionManifest(*handle); });
      attack("control_to_native", [&] { (void)second->controlToNative(*handle, {}); });
      attack("native_to_control", [&] { (void)second->nativeCovectorsToControl(*handle, {}); });
      attack("native_jvp",
             [&] { (void)second->nativeGeovalJvp(*handle, {}, {"air_pressure"}, time); });
      attack("native_vjp",
             [&] { (void)second->nativeGeovalVjp(*handle, {}, {"air_pressure"}, time); });
      attack("native_replace", [&] { second->replaceNativeAnalysis(*handle, {}); });
      attack("native_add", [&] { second->addNativeAnalysis(*handle, {}); });
      attack("advance", [&] { second->advance(*handle, second->timeStepSeconds()); });
      const Json evidence{{"accepted_foreign_operations", accepted},
                          {"rejected_foreign_operations", rejected}};
      std::cout << evidence.dump() << std::endl;
      std::ofstream output(configuration.getString("analysis bridge output") +
                           ".handle-ownership.json");
      output << evidence.dump(2) << '\n';
      if (!output || !accepted.empty() || rejected.size() != 17 ||
          first->continuationManifest(*handle) != before) {
        throw std::runtime_error(
            "public MPAS handle boundary accepts a foreign owner or changes its state");
      }
      // Explicit sealed-envelope adoption remains legal for a compatible
      // independently authenticated context; it is not an opaque-handle bypass.
      auto adopted = second->deserializeState(first->serializeState(*handle, time), time,
                                              first->stateGeneration(*handle));
      if (second->continuationManifest(*adopted) != before) {
        throw std::runtime_error("sealed cross-context State adoption changes continuation");
      }
    }
    std::cout << "destroying Context while its sole opaque State handle remains alive" << std::endl;
    first.reset();
    handle.reset();
    requireCompletedPythonTeardown();
    std::cout << "orphan-handle Python teardown survived" << std::endl;
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasHandleOwnership"; }
};
}  // namespace
int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  HandleOwnership application(oops::mpi::world());
  return run.execute(application);
}
