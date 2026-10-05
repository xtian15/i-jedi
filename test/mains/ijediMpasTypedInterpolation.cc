/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

/*
 * Exercise the public OOPS LocalInterpolator wrapper over a typed MPAS State.
 */

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>

#include "Eigen/Core"
#include "atlas/array.h"
#include "atlas/functionspace/PointCloud.h"
#include "atlas/option.h"
#include <nlohmann/json.hpp>
#include "eckit/config/LocalConfiguration.h"
#include "eckit/config/YAMLConfiguration.h"
#include "eckit/exception/Exceptions.h"
#include "eckit/filesystem/PathName.h"
#include "ijedi/Interpolation/LocalInterpolator.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Traits.h"
#include "oops/base/Geometry.h"
#include "oops/base/GetValues.h"
#include "oops/base/Model.h"
#include "oops/base/Increment.h"
#include "oops/base/Locations.h"
#include "oops/base/PostProcessor.h"
#include "oops/base/State.h"
#include "oops/base/Variables.h"
#include "oops/generic/instantiateModelFactory.h"
#include "oops/interface/LocalInterpolator.h"
#include "oops/interface/GeoVaLs.h"
#include "oops/interface/ModelAuxControl.h"
#include "oops/interface/SampledLocations.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"
#include "oops/util/TimeWindow.h"
#include "MpasTestObservations.h"

namespace {

class MpasTypedInterpolation final : public oops::Application {
 public:
  using oops::Application::Application;

  int execute(const eckit::Configuration &config) const override {
    using Geometry = oops::Geometry<ijedi::Traits>;
    using State = oops::State<ijedi::Traits>;
    using Interpolator = oops::LocalInterpolator<ijedi::Traits>;
    using Model = oops::Model<ijedi::Traits>;
    using ModelAux = oops::ModelAuxControl<ijedi::Traits>;

    oops::instantiateModelFactory<ijedi::Traits>();
    const eckit::LocalConfiguration geometryConfig(config, "geometry");
    const eckit::LocalConfiguration stateConfig(config, "initial condition");
    const eckit::LocalConfiguration interpolationConfig(config, "local interpolation");
    const eckit::LocalConfiguration modelConfig(config, "model");
    const eckit::LocalConfiguration auxConfig(config, "model aux control");
    const Geometry geometry(geometryConfig, getComm());
    const auto &vertical = geometry.geometry().mpasContext()->staticVerticalSnapshot();
    const auto &nativeAtlas = geometry.geometry().mpasAtlasGeometry();
    const auto &geometryFields = geometry.geometry().fields();
    for (const std::string name : {"zz", "zgrid"}) {
      const bool interfaces = name == "zgrid";
      const size_t levels = vertical.layers + static_cast<size_t>(interfaces);
      const auto &values = interfaces ? vertical.interfaceHeights : vertical.layerMetrics;
      const auto field = geometryFields.field(name);
      const auto view = atlas::array::make_view<double, 2>(field);
      if (field.functionspace().get() != nativeAtlas.cellNodes().get() ||
          field.shape(0) != static_cast<atlas::idx_t>(vertical.cells) ||
          field.shape(1) != static_cast<atlas::idx_t>(levels) ||
          field.metadata().getString("vertical_stagger") != (interfaces ? "interface" : "layer") ||
          field.metadata().getString("units") != (interfaces ? "m MSL" : "unitless") ||
          field.metadata().getString("static_vertical_geometry_receipt") != vertical.receipt ||
          field.metadata().getString("horizontal_geometry_receipt") != vertical.horizontalReceipt ||
          field.metadata().getString("vertical_direction") != "bottom_to_top" ||
          field.metadata().getBool("writable_state")) {
        throw eckit::BadValue("static vertical Atlas field lost its typed owner contract", Here());
      }
      for (size_t cell = 0; cell < vertical.cells; ++cell) {
        for (size_t level = 0; level < levels; ++level) {
          if (view(cell, level) != values[cell * levels + level]) {
            throw eckit::BadValue("static vertical Atlas field reordered or changed owner values",
                                  Here());
          }
        }
      }
    }
    for (size_t attack = 0; attack < 9; ++attack) {
      auto wrong = vertical;
      if (attack == 0) {
        wrong.horizontalReceipt[0] = wrong.horizontalReceipt[0] == '0' ? '1' : '0';
      }
      if (attack == 1) {
        wrong.receipt = "unbound";
      }
      if (attack == 2) {
        wrong.layers = 0;
      }
      if (attack == 3) {
        wrong.interfaceHeights.pop_back();
      }
      if (attack == 4) {
        wrong.heightUnits = "km MSL";
      }
      if (attack == 5) {
        wrong.interfaceHeights[0] = std::numeric_limits<double>::quiet_NaN();
      }
      if (attack == 6) {
        wrong.layerMetrics[0] = 0.;
      }
      if (attack == 7) {
        wrong.interfaceHeights[1] = wrong.interfaceHeights[0];
      }
      if (attack == 8) {
        wrong.direction = "top_to_bottom";
      }
      bool rejected = false;
      try {
        (void)nativeAtlas.staticVerticalFields(wrong);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      if (!rejected) {
        throw eckit::BadValue("static vertical metadata/value attack was accepted", Here());
      }
    }
    auto disposableView = nativeAtlas.staticVerticalFields(vertical);
    atlas::array::make_view<double, 2>(disposableView.field("zz"))(0, 0) = -1.;
    const auto replacementView = nativeAtlas.staticVerticalFields(vertical);
    if (atlas::array::make_view<double, 2>(replacementView.field("zz"))(0, 0) !=
            vertical.layerMetrics[0] ||
        vertical.layerMetrics[0] <= 0.) {
      throw eckit::BadValue("editing a static Atlas copy corrupted model-owned support", Here());
    }
    const ModelAux aux(geometry, auxConfig);
    const Model model(geometry, modelConfig);
    State state(geometry, stateConfig);

    const eckit::YAMLConfiguration locations(eckit::PathName(config.getString("locations path")));
    const std::vector<double> latitudes = locations.getDoubleVector("latitude_degrees");
    const std::vector<double> longitudes = locations.getDoubleVector("longitude_degrees");
    if (latitudes.size() != longitudes.size() || latitudes.empty()) {
      throw eckit::BadValue("typed-interpolation location corpus is malformed", Here());
    }
    const oops::Variables variables(config.getStringVector("typed variables"));
    Interpolator interpolator(interpolationConfig, geometry, latitudes, longitudes);
    const std::vector<size_t> levels = geometry.variableSizes(variables);
    const size_t expectedBridgeValues = [&]() {
      size_t count = 0;
      for (size_t levelCount : levels) {
        count += levelCount * static_cast<size_t>(geometry.functionSpace().size());
      }
      return count;
    }();
    const auto measureMaterialization = [&]() {
      const auto started = std::chrono::steady_clock::now();
      const std::vector<ijedi::MpasTypedField> fields =
          state.state().materializeTypedFields(variables);
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      size_t values = 0;
      for (const ijedi::MpasTypedField &field : fields) {
        values += field.values.size();
      }
      if (fields.size() != variables.size() || values != expectedBridgeValues) {
        throw eckit::BadValue(
            "MPAS typed bridge measurement observed an incomplete materialization", Here());
      }
      return std::make_pair(seconds, values * sizeof(double));
    };
    const auto validateAtlasViews = [&]() {
      const auto sources = state.state().materializeTypedFields(variables);
      const auto &space = geometry.geometry().mpasAtlasGeometry().cellNodes();
      atlas::FieldSet views;
      for (const auto &source : sources) {
        views.add(space.createField<double>(atlas::option::name(source.name) |
                                            atlas::option::levels(source.levels)));
      }
      state.state().toFieldSet(views);
      for (const auto &source : sources) {
        const auto field = views.field(source.name);
        const auto value = atlas::array::make_view<double, 2>(field);
        const auto binding =
            nlohmann::json::parse(field.metadata().getString("mpas_descriptor_binding"));
        if (field.functionspace().get() != space.get() ||
            field.metadata().getString("mpas_payload_receipt") != source.payloadReceipt ||
            binding["descriptor"]["mutability"] != "read_only" ||
            binding["descriptor"]["namespace"] != "geoval" ||
            binding["descriptor"]["native_name"] != nullptr ||
            binding["context"]["vector_basis_geometry_receipt"] !=
                source.horizontalGeometryReceipt) {
          throw eckit::BadValue("Atlas GeoVaL view lost its complete model-owned binding", Here());
        }
        for (atlas::idx_t cell = 0; cell < field.shape(0); ++cell) {
          for (atlas::idx_t level = 0; level < field.shape(1); ++level) {
            if (value(cell, level) != source.values.at(cell * source.levels + level)) {
              throw eckit::BadValue("Atlas State view changed an authoritative transferred value",
                                    Here());
            }
          }
        }
      }
      return views;
    };
    auto initialViews = validateAtlasViews();
    const auto &cellSpace = geometry.geometry().mpasAtlasGeometry().cellNodes();
    const auto referenceField = initialViews.field("air_pressure");
    const auto binding =
        nlohmann::json::parse(referenceField.metadata().getString("mpas_descriptor_binding"));
    size_t bindingAttacksRejected = 0;
    const auto rejectRequest = [&](atlas::Field field) {
      auto values = atlas::array::make_view<double, 2>(field);
      values.assign(-1234567.89);
      atlas::FieldSet request;
      request.add(field);
      bool rejected = false;
      try {
        state.state().toFieldSet(request);
      } catch (const eckit::Exception &) {
        rejected = true;
      }
      if (!rejected) {
        throw eckit::BadValue("corrupt Atlas State request was accepted", Here());
      }
      for (atlas::idx_t cell = 0; cell < field.shape(0); ++cell) {
        for (atlas::idx_t level = 0; level < field.shape(1); ++level) {
          if (values(cell, level) != -1234567.89) {
            throw eckit::BadValue("rejected Atlas request partially changed destination values",
                                  Here());
          }
        }
      }
      ++bindingAttacksRejected;
    };
    // Mutate each semantic property and each private owner-context property,
    // rather than merely proving that one arbitrary bad label is rejected.
    for (const std::string group : {"descriptor", "context"}) {
      for (const auto &property : binding.at(group).items()) {
        auto wrong = binding;
        wrong[group][property.key()] = "incorrect-binding";
        auto field = cellSpace.createField<double>(atlas::option::name("air_pressure") |
                                                   atlas::option::levels(referenceField.shape(1)));
        field.metadata().set("mpas_descriptor_binding", wrong.dump());
        rejectRequest(field);
      }
    }
    for (const std::string key : {"descriptor_registry_digest"}) {
      auto wrong = binding;
      wrong[key] = std::string(64, '0');
      auto field = cellSpace.createField<double>(atlas::option::name("air_pressure") |
                                                 atlas::option::levels(referenceField.shape(1)));
      field.metadata().set("mpas_descriptor_binding", wrong.dump());
      rejectRequest(field);
    }
    atlas::functionspace::PointCloud foreignSpace(cellSpace.lonlat());
    rejectRequest(foreignSpace.createField<double>(atlas::option::name("air_pressure") |
                                                   atlas::option::levels(referenceField.shape(1))));

    // OOPS may give a rank no local targets.  The model-specific wrapper must
    // preserve that valid no-op instead of trying to construct an empty
    // PointCloud or rejecting the rank before the collective flow completes.
    const std::vector<double> noCoordinates;
    ijedi::LocalInterpolator emptyInterpolator(interpolationConfig, geometry.geometry(),
                                               noCoordinates, noCoordinates);
    std::vector<double> emptyBuffer{1.0};
    emptyInterpolator.apply(variables, state.state(), std::vector<bool>(), emptyBuffer);
    if (!emptyBuffer.empty()) {
      throw eckit::BadValue("zero-target MPAS interpolation was not a no-op", Here());
    }

    std::vector<bool> allTargets(latitudes.size(), true);
    const auto initialBridge = measureMaterialization();
    std::vector<double> initialValues;
    auto interpolationStarted = std::chrono::steady_clock::now();
    interpolator.apply(variables, state, allTargets, initialValues);
    const double initialInterpolationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - interpolationStarted)
            .count();
    size_t expectedValues = 0;
    for (size_t count : levels) {
      expectedValues += count * latitudes.size();
    }
    if (initialValues.size() != expectedValues) {
      throw eckit::BadValue("OOPS MPAS interpolation returned the wrong buffer extent", Here());
    }

    // Exercise the real OOPS GetValues composition, including its time mask,
    // MPI buffer framing, and GeoVaLs fill ordering, with a deliberately small
    // observation implementation that contributes no interpolation behavior.
    std::vector<util::DateTime> observationTimes(latitudes.size(), state.validTime());
    oops::SampledLocations<synthetic_obs::Traits> sampledLocations(
        std::make_unique<synthetic_obs::SampledLocations>(latitudes, longitudes, observationTimes));
    oops::Locations<synthetic_obs::Traits> observationLocations(std::move(sampledLocations));
    eckit::LocalConfiguration windowConfig;
    windowConfig.set("begin", "2026-07-17T05:59:00Z");
    windowConfig.set("end", "2026-07-17T06:01:00Z");
    const util::TimeWindow window(windowConfig);
    oops::GetValues<ijedi::Traits, synthetic_obs::Traits> getValues(
        interpolationConfig, geometry, window, observationLocations, variables);
    getValues.initialize(model.timeResolution());
    getValues.process(state);
    getValues.finalize();
    oops::GeoVaLs<synthetic_obs::Traits> geovals(observationLocations, variables, levels);
    getValues.fillGeoVaLs(geovals);
    size_t getValuesOffset = 0;
    size_t variableIndex = 0;
    for (const oops::Variable &variable : variables) {
      const Eigen::MatrixXd &field = geovals.geovals().values(variable.name());
      if (field.rows() != static_cast<Eigen::Index>(levels.at(variableIndex)) ||
          field.cols() != static_cast<Eigen::Index>(latitudes.size())) {
        throw eckit::BadValue("OOPS GetValues returned the wrong GeoVaL shape", Here());
      }
      for (Eigen::Index target = 0; target < field.cols(); ++target) {
        for (Eigen::Index level = 0; level < field.rows(); ++level) {
          const double direct = initialValues.at(
              getValuesOffset + static_cast<size_t>(target) * levels.at(variableIndex) +
              static_cast<size_t>(level));
          if (field(level, target) != direct) {
            throw eckit::BadValue("OOPS GetValues differs from the public LocalInterpolator output",
                                  Here());
          }
        }
      }
      getValuesOffset += latitudes.size() * levels.at(variableIndex++);
    }
    if (getValuesOffset != initialValues.size()) {
      throw eckit::BadValue("OOPS GetValues comparison did not consume the full buffer", Here());
    }

    // A second construction must reuse the Atlas cache, and masked targets
    // must leave every preexisting buffer entry untouched.
    const auto &nativeGeometry = geometry.geometry().mpasAtlasGeometry();
    const auto compiler = geometry.geometry().modelData().getString("atlas_compiler_identity");
    const auto cached = nativeGeometry.pointOperator(latitudes, longitudes, compiler);
    const auto repeated = nativeGeometry.pointOperator(latitudes, longitudes, compiler);
    if (cached != repeated || cached->targetSize() != latitudes.size()) {
      throw eckit::BadValue("native Atlas point operator cache was not reused", Here());
    }
    ijedi::MpasAtlasPointOperator restored(nativeGeometry, latitudes, longitudes, compiler,
                                           cached->serializeCache(), cached->cacheReceipt());
    if (restored.serializeCache() != cached->serializeCache()) {
      throw eckit::BadValue("Atlas cache restoration changed coefficients or ordering", Here());
    }
    std::ofstream cacheOutput(config.getString("atlas cache output"));
    cacheOutput << cached->serializeCache();
    if (!cacheOutput) {
      throw eckit::BadValue("cannot write Atlas cache regression artifact", Here());
    }
    std::vector<double> reorderedLatitudes(latitudes.rbegin(), latitudes.rend());
    std::vector<double> reorderedLongitudes(longitudes.rbegin(), longitudes.rend());
    const auto reordered =
        nativeGeometry.pointOperator(reorderedLatitudes, reorderedLongitudes, compiler);
    if (reordered->cacheKey() == cached->cacheKey()) {
      throw eckit::BadValue("target order did not invalidate the Atlas point cache", Here());
    }
    std::vector<double> changedLatitudes(latitudes);
    changedLatitudes[4] += 1.0e-9;
    const auto changed = nativeGeometry.pointOperator(changedLatitudes, longitudes, compiler);
    if (changed->cacheKey() == cached->cacheKey()) {
      throw eckit::BadValue("one target coordinate did not invalidate the Atlas point cache",
                            Here());
    }
    std::vector<double> invalidLatitudes(latitudes);
    invalidLatitudes[0] = std::numeric_limits<double>::quiet_NaN();
    bool invalidRejected = false;
    try {
      (void)nativeGeometry.pointOperator(invalidLatitudes, longitudes, compiler);
    } catch (const std::invalid_argument &) {
      invalidRejected = true;
    }
    if (!invalidRejected) {
      throw eckit::BadValue("nonfinite point target was silently accepted", Here());
    }
    for (const auto &badSetting : std::vector<std::pair<std::string, std::string>>{
             {"method", "patch"}, {"mask semantics", "masked_nodes"}}) {
      eckit::LocalConfiguration badConfig;
      badConfig.set(badSetting.first, badSetting.second);
      try {
        ijedi::LocalInterpolator rejected(badConfig, geometry.geometry(), latitudes, longitudes);
        throw eckit::BadValue("unsupported MPAS interpolation semantics were accepted", Here());
      } catch (const eckit::BadParameter &) {
      }
    }
    std::vector<bool> alternating(latitudes.size(), true);
    for (size_t target = 1; target < alternating.size(); target += 2) {
      alternating[target] = false;
    }
    constexpr double sentinel = -9.87654321012345e99;
    std::vector<double> masked(initialValues.size(), sentinel);
    interpolator.apply(variables, state, alternating, masked);
    size_t offset = 0;
    for (size_t field = 0; field < levels.size(); ++field) {
      for (size_t target = 0; target < latitudes.size(); ++target) {
        for (size_t level = 0; level < levels[field]; ++level) {
          const size_t index = offset + target * levels[field] + level;
          if (alternating[target] && masked[index] != initialValues[index]) {
            throw eckit::BadValue("masked MPAS interpolation changed an active value", Here());
          }
          if (!alternating[target] && masked[index] != sentinel) {
            throw eckit::BadValue("masked MPAS interpolation overwrote an inactive target", Here());
          }
        }
      }
      offset += latitudes.size() * levels[field];
    }

    oops::Increment<ijedi::Traits> increment(geometry, variables, state.validTime());
    std::vector<double> zeroTangent;
    interpolator.apply(variables, increment, allTargets, zeroTangent);
    if (zeroTangent.size() != initialValues.size() ||
        !std::all_of(zeroTangent.begin(), zeroTangent.end(),
                     [](double value) { return value == 0.; })) {
      throw eckit::BadValue("zero typed MPAS TL interpolation failed", Here());
    }

    oops::PostProcessor<State> post;
    model.forecast(state, aux, model.timeResolution(), post);
    (void)validateAtlasViews();
    // An earlier trajectory-bound view cannot be recycled as a current view.
    rejectRequest(initialViews.field("air_pressure"));
    const auto step1Bridge = measureMaterialization();
    std::vector<double> step1Values;
    interpolationStarted = std::chrono::steady_clock::now();
    interpolator.apply(variables, state, allTargets, step1Values);
    const double step1InterpolationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - interpolationStarted)
            .count();
    model.forecast(state, aux, model.timeResolution(), post);
    (void)validateAtlasViews();
    const auto step2Bridge = measureMaterialization();
    std::vector<double> step2Values;
    interpolationStarted = std::chrono::steady_clock::now();
    interpolator.apply(variables, state, allTargets, step2Values);
    const double step2InterpolationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - interpolationStarted)
            .count();
    if (step1Values.size() != expectedValues || step2Values.size() != expectedValues) {
      throw eckit::BadValue("evolved MPAS interpolation changed the buffer extent", Here());
    }

    const std::string outputPath = config.getString("output values");
    const std::string temporaryPath = outputPath + ".tmp";
    errno = 0;
    if (std::remove(temporaryPath.c_str()) != 0 && errno != ENOENT) {
      throw eckit::BadValue("cannot remove stale typed-interpolation output", Here());
    }
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    for (const std::vector<double> *values : {&initialValues, &step1Values, &step2Values}) {
      output.write(reinterpret_cast<const char *>(values->data()),
                   static_cast<std::streamsize>(values->size() * sizeof(double)));
    }
    output.close();
    if (!output || std::rename(temporaryPath.c_str(), outputPath.c_str()) != 0) {
      throw eckit::BadValue("failed to publish typed-interpolation output", Here());
    }

    const std::string metricsPath = config.getString("output metrics");
    const std::string temporaryMetricsPath = metricsPath + ".tmp";
    errno = 0;
    if (std::remove(temporaryMetricsPath.c_str()) != 0 && errno != ENOENT) {
      throw eckit::BadValue("cannot remove stale typed-interpolation metrics", Here());
    }
    std::ofstream metrics(temporaryMetricsPath, std::ios::trunc);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0) {
      throw eckit::BadValue("cannot measure interpolation process peak RSS", Here());
    }
#ifdef __APPLE__
    const std::uint64_t peakRssBytes = usage.ru_maxrss;
#else
    const std::uint64_t peakRssBytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    metrics << std::setprecision(17)
            << "{\"schema_version\":1,\"authoritative_backend\":\"mpas-pytorch\""
            << ",\"bridge_copy_values\":" << expectedBridgeValues
            << ",\"typed_binding_attacks_rejected\":" << bindingAttacksRejected
            << ",\"typed_atlas_views_generations_checked\":3"
            << ",\"bridge_copy_bytes\":" << initialBridge.second
            << ",\"initial_materialization_seconds\":" << initialBridge.first
            << ",\"step1_materialization_seconds\":" << step1Bridge.first
            << ",\"step2_materialization_seconds\":" << step2Bridge.first
            << ",\"initial_interpolation_seconds\":" << initialInterpolationSeconds
            << ",\"step1_interpolation_seconds\":" << step1InterpolationSeconds
            << ",\"step2_interpolation_seconds\":" << step2InterpolationSeconds
            << ",\"atlas_compiler_identity\":\"" << compiler << "\""
            << ",\"atlas_cache_key\":\"" << cached->cacheKey() << "\""
            << ",\"atlas_cache_receipt\":\"" << cached->cacheReceipt() << "\""
            << ",\"process_peak_rss_bytes\":" << peakRssBytes << "}\n";
    metrics.close();
    if (!metrics || std::rename(temporaryMetricsPath.c_str(), metricsPath.c_str()) != 0) {
      throw eckit::BadValue("failed to publish typed-interpolation metrics", Here());
    }
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasTypedInterpolation"; }
};

}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  MpasTypedInterpolation app(oops::mpi::world());
  return run.execute(app);
}
