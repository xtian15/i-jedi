/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

/*
 * Drive two MPAS-PyTorch steps through the public OOPS Geometry/State/Model
 * wrappers and emit fieldwise manifests for comparison with an independent
 * direct-Python oracle.
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "pybind11/embed.h"

#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"
#include "ijedi/State/State.h"
#include "ijedi/LinearModel/LinearModel.h"
#include "ijedi/Traits.h"
#include "oops/base/Geometry.h"
#include "oops/base/Model.h"
#include "oops/base/PostProcessor.h"
#include "oops/base/State.h"
#include "oops/generic/instantiateModelFactory.h"
#include "oops/interface/ModelAuxControl.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {

namespace py = pybind11;

void publishSerializedState(const std::string &path, const std::vector<double> &serialized) {
  const std::string temporaryPath = path + ".tmp";
  errno = 0;
  if (std::remove(temporaryPath.c_str()) != 0 && errno != ENOENT) {
    throw eckit::BadValue("cannot remove stale serialized State temporary file", Here());
  }
  std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
  const std::uint64_t count = serialized.size();
  output.write(reinterpret_cast<const char *>(&count), sizeof(count));
  output.write(reinterpret_cast<const char *>(serialized.data()),
               static_cast<std::streamsize>(serialized.size() * sizeof(double)));
  output.close();
  if (!output || std::rename(temporaryPath.c_str(), path.c_str()) != 0) {
    throw eckit::BadValue("failed to publish serialized MPAS State checkpoint", Here());
  }
}

std::vector<double> readSerializedState(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw eckit::BadValue("cannot open serialized MPAS State checkpoint", Here());
  const auto fileBytes = input.tellg();
  if (fileBytes < static_cast<std::streamoff>(sizeof(std::uint64_t))) {
    throw eckit::BadValue("serialized MPAS State checkpoint has a truncated header", Here());
  }
  input.seekg(0);
  std::uint64_t count = 0;
  input.read(reinterpret_cast<char *>(&count), sizeof(count));
  // Authenticate the framing against the file extent before allocating. A
  // claimed huge count in an eight-byte file must not trigger an allocation.
  const auto payloadBytes = static_cast<std::uint64_t>(fileBytes) - sizeof(count);
  if (!input || count == 0 || payloadBytes % sizeof(double) != 0 ||
      count != payloadBytes / sizeof(double) ||
      count > std::numeric_limits<size_t>::max() / sizeof(double)) {
    throw eckit::BadValue("serialized MPAS State checkpoint has an invalid length", Here());
  }
  std::vector<double> serialized(static_cast<size_t>(count));
  input.read(reinterpret_cast<char *>(serialized.data()),
             static_cast<std::streamsize>(serialized.size() * sizeof(double)));
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    throw eckit::BadValue("serialized MPAS State checkpoint is truncated or has trailing data",
                          Here());
  }
  return serialized;
}

class MpasOopsStep final : public oops::Application {
 public:
  using oops::Application::Application;

  int execute(const eckit::Configuration &config) const override {
    using Geometry = oops::Geometry<ijedi::Traits>;
    using State = oops::State<ijedi::Traits>;
    using Model = oops::Model<ijedi::Traits>;
    using ModelAux = oops::ModelAuxControl<ijedi::Traits>;

    const std::string outputPath = config.getString("output manifest");
    const std::string executionMode = config.getString("execution mode", "producer");
    const std::string checkpointPath = config.getString("checkpoint path");
    const std::string temporaryOutputPath = outputPath + ".current.tmp";
    for (const std::string &path : {outputPath, temporaryOutputPath}) {
      errno = 0;
      if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        throw eckit::BadValue("cannot remove stale output manifest: " + path, Here());
      }
    }

    oops::instantiateModelFactory<ijedi::Traits>();
    const eckit::LocalConfiguration geometryConfig(config, "geometry");
    const eckit::LocalConfiguration stateConfig(config, "initial condition");
    const eckit::LocalConfiguration modelConfig(config, "model");
    const eckit::LocalConfiguration auxConfig(config, "model aux control");
    const Geometry geometry(geometryConfig, getComm());
    eckit::LocalConfiguration identityConfig;
    identityConfig.set("time step", "PT12M");
    bool identityRejected = false;
    try {
      const ijedi::LinearModel invalidIdentity(geometry.geometry(), identityConfig);
    } catch (const eckit::BadParameter &error) {
      if (std::string(error.what()).find("not the tangent-linear") == std::string::npos) throw;
      identityRejected = true;
    }
    if (!identityRejected) {
      throw eckit::BadValue("MPAS accepted an identity model as its tangent-linear model", Here());
    }
    const ModelAux aux(geometry, auxConfig);
    const Model model(geometry, modelConfig);
    oops::PostProcessor<State> post;

    if (executionMode != "producer" && executionMode != "bundle-attack" &&
        executionMode != "generation-zero" && executionMode != "detached-clone" &&
        executionMode != "consumer") {
      throw eckit::BadParameter("unknown MPAS OOPS step execution mode", Here());
    }

    if (executionMode == "detached-clone") {
      // Spell the same authenticated init path with a redundant /./ so the
      // cache creates a distinct backend context while Python resolves the
      // same file bytes, receipts, schema, and package.  Clone and assignment
      // run sequentially so this lifecycle test does not retain three large
      // continuation copies in addition to both backend contexts.
      eckit::LocalConfiguration distinctGeometryConfig(geometryConfig);
      const std::string initPath = geometryConfig.getString("init path");
      const size_t slash = initPath.find_last_of('/');
      if (slash == std::string::npos) {
        throw eckit::BadValue("MPAS detached-clone test requires an absolute init path",
                              Here());
      }
      distinctGeometryConfig.set(
          "init path", initPath.substr(0, slash) + "/./" + initPath.substr(slash + 1));

      std::string cloneRegression;
      std::string cloneContinuation;
      std::string cloneTransform;
      {
        std::unique_ptr<State> detachedClone;
        {
          const Geometry sourceGeometry(distinctGeometryConfig, getComm());
          if (sourceGeometry.geometry().mpasContext() ==
              geometry.geometry().mpasContext()) {
            throw eckit::BadValue(
                "distinct cache identities unexpectedly shared an MPAS backend context",
                Here());
          }
          State sourceState(sourceGeometry, stateConfig);
          detachedClone = std::make_unique<State>(geometry, sourceState);
        }
        model.forecast(*detachedClone, aux, model.timeResolution(), post);
        model.forecast(*detachedClone, aux, model.timeResolution(), post);
        cloneRegression = detachedClone->state().regressionManifest();
        cloneContinuation = detachedClone->state().continuationManifest();
        cloneTransform = detachedClone->state().typedTransformManifest();
      }

      std::unique_ptr<ijedi::State> detachedAssignment;
      {
        const Geometry sourceGeometry(distinctGeometryConfig, getComm());
        if (sourceGeometry.geometry().mpasContext() ==
            geometry.geometry().mpasContext()) {
          throw eckit::BadValue(
              "distinct assignment source shared the target MPAS backend context", Here());
        }
        State sourceState(sourceGeometry, stateConfig);
        detachedAssignment = std::make_unique<ijedi::State>(
            geometry.geometry(), sourceState.variables(), sourceState.validTime(), false);
        *detachedAssignment = sourceState.state();
      }
      detachedAssignment->advanceModel(model.timeResolution());
      detachedAssignment->advanceModel(model.timeResolution());
      if (detachedAssignment->regressionManifest() != cloneRegression ||
          detachedAssignment->continuationManifest() != cloneContinuation ||
          detachedAssignment->typedTransformManifest() != cloneTransform) {
        throw eckit::BadValue(
            "cross-context MPAS assignment differs from the sealed clone path", Here());
      }
      std::ofstream output(temporaryOutputPath, std::ios::out | std::ios::trunc);
      output << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"driver\": \"detached-source-geometry-clone\",\n"
             << "  \"step2\": " << cloneRegression << ",\n"
             << "  \"continuation_step2\": " << cloneContinuation << ",\n"
             << "  \"typed_transform_step2\": " << cloneTransform << "\n"
             << "}\n";
      output.close();
      if (!output || std::rename(temporaryOutputPath.c_str(), outputPath.c_str()) != 0) {
        throw eckit::BadValue("failed to publish detached-clone manifest", Here());
      }
      return 0;
    }

    State state(geometry, stateConfig);

    if (executionMode == "consumer") {
      const std::vector<double> serialized = readSerializedState(checkpointPath);
      State restored(state);
      size_t deserializeIndex = 0;
      restored.deserialize(serialized, deserializeIndex);
      if (deserializeIndex != serialized.size()) {
        throw eckit::BadValue("fresh-process restore did not consume the checkpoint exactly",
                              Here());
      }
      model.forecast(restored, aux, model.timeResolution(), post);
      std::ofstream output(temporaryOutputPath, std::ios::out | std::ios::trunc);
      output << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"driver\": \"fresh-process-consumer\",\n"
             << "  \"step2_time\": \"" << restored.validTime() << "\",\n"
             << "  \"serialized_double_count\": " << serialized.size() << ",\n"
             << "  \"step2\": " << restored.state().regressionManifest() << ",\n"
             << "  \"continuation_step2\": " << restored.state().continuationManifest()
             << ",\n"
             << "  \"typed_transform_step2\": "
             << restored.state().typedTransformManifest() << "\n"
             << "}\n";
      output.close();
      if (!output || std::rename(temporaryOutputPath.c_str(), outputPath.c_str()) != 0) {
        throw eckit::BadValue("failed to publish fresh-process restore manifest", Here());
      }
      return 0;
    }
    if (executionMode == "bundle-attack") {
      bool differentBundleCloneRejected = false;
      const std::string changedNamelistPath = checkpointPath + ".different-namelist";
      std::ifstream sourceNamelist(geometryConfig.getString("namelist path"),
                                  std::ios::binary);
      std::ofstream changedNamelist(changedNamelistPath,
                                    std::ios::binary | std::ios::trunc);
      changedNamelist << sourceNamelist.rdbuf()
                      << "\n! I-JEDI clone compatibility negative control\n";
      changedNamelist.close();
      if (!sourceNamelist || !changedNamelist) {
        throw eckit::BadValue("failed to construct the different-bundle namelist", Here());
      }

      std::string changedConfigurationReceipt;
      std::string changedBundleReceipt;
      std::string changedStateSchemaDigest;
      {
        py::gil_scoped_acquire acquire;
        const py::object mpas = py::module_::import("mpas_pytorch");
        const py::object contracts =
            py::module_::import("mpas_pytorch.ijedi_contracts");
        const py::object changedConfig =
            mpas.attr("load_config_from_namelist")(changedNamelistPath);
        const py::object configurationReceipt =
            contracts.attr("build_configuration_receipt")(changedNamelistPath,
                                                            changedConfig);
        changedConfigurationReceipt = configurationReceipt.cast<std::string>();
        const py::object snapshot = contracts.attr("load_ijedi_geometry_snapshot")(
            geometryConfig.getString("init path"), geometryConfig.getString("grid path"),
            py::arg("configuration_receipt") = configurationReceipt);
        changedBundleReceipt = snapshot.attr("receipt").cast<std::string>();
        const py::tuple loaded = mpas.attr("load_initial_state")(
            geometryConfig.getString("init path"), geometryConfig.getString("grid path"),
            py::arg("mesh_support_path") = py::none(),
            py::arg("config") = changedConfig);
        const py::object reference = mpas.attr("run_simulation")(
            contracts.attr("clone_state")(loaded[0]),
            contracts.attr("clone_state")(loaded[1]), py::arg("nsteps") = 1,
            py::arg("config") = changedConfig);
        const py::object changedSchema = contracts.attr("build_state_storage_schema")(
            reference, loaded[1],
            py::arg("horizontal_geometry_receipt") = snapshot.attr("horizontal_receipt"),
            py::arg("static_vertical_geometry_receipt") =
                snapshot.attr("static_vertical_receipt"),
            py::arg("bundle_receipt") = snapshot.attr("receipt"),
            py::arg("configuration_receipt") = configurationReceipt);
        changedStateSchemaDigest = changedSchema.attr("digest").cast<std::string>();
      }
      if (changedConfigurationReceipt ==
              geometryConfig.getString("configuration receipt") ||
          changedBundleReceipt == geometryConfig.getString("geometry bundle receipt")) {
        throw eckit::BadValue("different-bundle negative control did not change its receipts",
                              Here());
      }
      eckit::LocalConfiguration changedGeometryConfig(geometryConfig);
      changedGeometryConfig.set("namelist path", changedNamelistPath);
      changedGeometryConfig.set("configuration receipt", changedConfigurationReceipt);
      changedGeometryConfig.set("geometry bundle receipt", changedBundleReceipt);
      changedGeometryConfig.set("state schema digest", changedStateSchemaDigest);
      const Geometry changedGeometry(changedGeometryConfig, getComm());
      if (changedGeometry.geometry().mpasContext() == geometry.geometry().mpasContext()) {
        throw eckit::BadValue(
            "backend cache aliased different authenticated MPAS bundles", Here());
      }
      try {
        const State rejectedClone(changedGeometry, state);
      } catch (const std::exception &error) {
        if (std::string(error.what()).find("different geometry/configuration bundle") !=
            std::string::npos) {
          differentBundleCloneRejected = true;
        } else {
          throw eckit::BadValue("different-bundle clone failed for the wrong reason: " +
                                std::string(error.what()), Here());
        }
      }
      if (!differentBundleCloneRejected) {
        throw eckit::BadValue("MPAS State clone accepted a different bundle receipt", Here());
      }
      std::ofstream output(temporaryOutputPath, std::ios::out | std::ios::trunc);
      output << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"driver\": \"valid-different-bundle-negative-control\",\n"
             << "  \"different_bundle_clone_rejected\": true\n"
             << "}\n";
      output.close();
      if (!output || std::rename(temporaryOutputPath.c_str(), outputPath.c_str()) != 0) {
        throw eckit::BadValue("failed to publish different-bundle attack manifest", Here());
      }
      return 0;
    }

    const double initialNorm = state.norm();
    const util::DateTime initialTime = state.validTime();

    std::vector<double> initialSerialized;
    state.serialize(initialSerialized);
    const size_t initialSerializedCount = initialSerialized.size();

    const size_t timeWords = state.validTime().serialSize();
    // A real complete boundary at a different time, not a relabeled initial
    // state. Retain the original time/cursor failure-atomicity attack.
    State laterBoundary(state);
    model.forecast(laterBoundary, aux, model.timeResolution(), post);
    auto requireDeserializeFailure = [&](std::vector<double> attacked,
                                         const std::string &expected) {
      State rejected(laterBoundary);
      const auto beforeTime = rejected.validTime();
      const auto beforeBoundary = rejected.state().continuationManifest();
      const auto beforeTransform = rejected.state().typedTransformManifest();
      const double beforeNorm = rejected.norm();
      // Nonzero framing cursor and deliberately different destination time
      // make time/cursor mutation observable even on a framing-only failure.
      attacked.insert(attacked.begin(), 3, 0.0);
      size_t attackedIndex = 3;
      bool failed = false;
      try {
        rejected.deserialize(attacked, attackedIndex);
      } catch (const std::exception &error) {
        if (std::string(error.what()).find(expected) == std::string::npos) {
          throw eckit::BadValue("serialized-State attack failed for the wrong reason: " +
                                std::string(error.what()), Here());
        }
        failed = true;
      }
      if (!failed) throw eckit::BadValue("serialized-State attack was accepted: " + expected, Here());
      if (attackedIndex != 3 || rejected.validTime() != beforeTime || rejected.norm() != beforeNorm ||
          rejected.state().continuationManifest() != beforeBoundary ||
          rejected.state().typedTransformManifest() != beforeTransform) {
        throw eckit::BadValue("rejected MPAS restore mutated its destination or cursor", Here());
      }
    };
    requireDeserializeFailure({}, "truncated time");
    {
      std::vector<double> attacked(initialSerialized);
      attacked.at(timeWords) += 1.0;
      requireDeserializeFailure(std::move(attacked), "framing marker");
    }
    {
      std::vector<double> attacked(initialSerialized);
      attacked.at(timeWords + 1) = std::numeric_limits<double>::quiet_NaN();
      requireDeserializeFailure(std::move(attacked), "invalid generation");
    }
    {
      std::vector<double> attacked(initialSerialized);
      attacked.at(timeWords + 2) =
          static_cast<double>(initialSerialized.size() * sizeof(double));
      requireDeserializeFailure(std::move(attacked), "truncated");
    }
    {
      std::vector<double> attacked(initialSerialized);
      attacked.pop_back();
      requireDeserializeFailure(std::move(attacked), "truncated");
    }
    {
      std::vector<double> attacked(initialSerialized);
      const size_t payloadWord = timeWords + 3;
      const size_t envelopeBytes = static_cast<size_t>(initialSerialized.at(timeWords + 2));
      unsigned char *payloadBytes = reinterpret_cast<unsigned char *>(
          attacked.data() + payloadWord);
      payloadBytes[envelopeBytes / 2] ^= 1U;
      requireDeserializeFailure(std::move(attacked), "digest");
    }

    if (executionMode == "generation-zero") {
      State initialRestored(state);
      size_t initialDeserializeIndex = 0;
      initialRestored.deserialize(initialSerialized, initialDeserializeIndex);
      if (initialDeserializeIndex != initialSerialized.size() ||
          initialRestored.state().typedTransformManifest() !=
              state.state().typedTransformManifest()) {
        throw eckit::BadValue("generation-zero MPAS State did not restore exactly", Here());
      }
      model.forecast(initialRestored, aux, model.timeResolution(), post);
      model.forecast(initialRestored, aux, model.timeResolution(), post);
      std::ofstream output(temporaryOutputPath, std::ios::out | std::ios::trunc);
      output << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"driver\": \"generation-zero-serialized-replay\",\n"
             << "  \"step2\": " << initialRestored.state().regressionManifest() << ",\n"
             << "  \"continuation_step2\": "
             << initialRestored.state().continuationManifest() << ",\n"
             << "  \"typed_transform_step2\": "
             << initialRestored.state().typedTransformManifest() << "\n"
             << "}\n";
      output.close();
      if (!output || std::rename(temporaryOutputPath.c_str(), outputPath.c_str()) != 0) {
        throw eckit::BadValue("failed to publish generation-zero replay manifest", Here());
      }
      return 0;
    }

    initialSerialized.clear();
    initialSerialized.shrink_to_fit();
    const State untouched(state);

    model.forecast(state, aux, model.timeResolution(), post);
    const std::string step1 = state.state().regressionManifest();
    const std::string continuation1 = state.state().continuationManifest();
    const std::string transform1 = state.state().typedTransformManifest();
    const util::DateTime step1Time = state.validTime();

    std::vector<double> serialized;
    state.serialize(serialized);
    publishSerializedState(checkpointPath, serialized);
    State restored(state);
    size_t deserializeIndex = 0;
    restored.deserialize(serialized, deserializeIndex);
    if (deserializeIndex != serialized.size() || restored.validTime() != step1Time) {
      throw eckit::BadValue("MPAS OOPS State serialization did not consume/restore exactly",
                            Here());
    }
    model.forecast(state, aux, model.timeResolution(), post);
    const std::string step2 = state.state().regressionManifest();
    const std::string continuation2 = state.state().continuationManifest();
    const std::string transform2 = state.state().typedTransformManifest();
    const util::DateTime step2Time = state.validTime();
    model.forecast(restored, aux, model.timeResolution(), post);
    if (restored.state().regressionManifest() != step2 ||
        restored.state().continuationManifest() != continuation2 ||
        restored.state().typedTransformManifest() != transform2) {
      throw eckit::BadValue(
          "serialized/restored MPAS State did not reproduce the next OOPS step bitwise", Here());
    }
    if (untouched.validTime() != initialTime || untouched.norm() != initialNorm) {
      throw eckit::BadValue("advancing an OOPS clone mutated its source MPAS State", Here());
    }
    std::ofstream output(temporaryOutputPath, std::ios::out | std::ios::trunc);
    if (!output) {
      throw eckit::BadValue("cannot open temporary output manifest: " + temporaryOutputPath,
                            Here());
    }
    output << std::setprecision(17)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"driver\": \"oops::Model<ijedi::Traits>::forecast\",\n"
           << "  \"initial_time\": \"" << initialTime << "\",\n"
           << "  \"step1_time\": \"" << step1Time << "\",\n"
           << "  \"step2_time\": \"" << step2Time << "\",\n"
           << "  \"initial_norm\": " << initialNorm << ",\n"
           << "  \"untouched_norm\": " << untouched.norm() << ",\n"
           << "  \"serialized_double_count\": " << serialized.size() << ",\n"
           << "  \"generation_zero_serialized_double_count\": "
           << initialSerializedCount << ",\n"
           << "  \"serialization_attack_count\": 5,\n"
           << "  \"serialized_restore_step2_bitwise_equal\": true,\n"
           << "  \"step1\": " << step1 << ",\n"
           << "  \"step2\": " << step2 << ",\n"
           << "  \"continuation_step1\": " << continuation1 << ",\n"
           << "  \"continuation_step2\": " << continuation2 << ",\n"
           << "  \"typed_transform_step1\": " << transform1 << ",\n"
           << "  \"typed_transform_step2\": " << transform2 << "\n"
           << "}\n";
    output.close();
    if (!output) {
      throw eckit::BadValue("failed to write output manifest: " + temporaryOutputPath, Here());
    }
    if (std::rename(temporaryOutputPath.c_str(), outputPath.c_str()) != 0) {
      throw eckit::BadValue("failed to publish output manifest: " + outputPath, Here());
    }
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasOopsStep"; }
};

}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  MpasOopsStep app(oops::mpi::world());
  return run.execute(app);
}
