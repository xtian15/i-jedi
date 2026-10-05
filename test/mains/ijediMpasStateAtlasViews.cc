/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/functionspace/PointCloud.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/State/State.h"
#include "ijedi/VariableChange/VariableChange.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

namespace {
using Json = nlohmann::json;
using Arrays = ijedi::MpasAnalysisArrays;
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
std::string nativePayload(const ijedi::Geometry &geometry, const atlas::Field &field) {
  ijedi::AtlasOperatorReceipt hash;
  const size_t stride = field.size() / field.shape(0);
  for (atlas::idx_t row = 0; row < field.shape(0); ++row) {
    const auto source =
        field.name() == "u" ? geometry.mpasAtlasGeometry().nativeToAtlasEdges().at(row) : row;
    for (size_t column = 0; column < stride; ++column) {
      hash.real(field.rank() == 2 ? atlas::array::make_view<double, 2>(field)(source, column)
                                  : atlas::array::make_view<double, 3>(field)(
                                        source, column / field.shape(2), column % field.shape(2)));
    }
  }
  return hash.finish();
}

class StateAtlasViews final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &configuration) const override {
    const ijedi::Geometry geometry(eckit::LocalConfiguration(configuration, "geometry"), getComm());
    if (configuration.getString("state probe mode", "views") == "initial-clock") {
      const auto initialConfig = eckit::LocalConfiguration(configuration, "initial condition");
      const ijedi::State correct(geometry, initialConfig);
      for (const std::string date : {"2026-07-17T06:12:00Z", "2026-07-18T06:00:00Z"}) {
        auto incorrect = initialConfig;
        incorrect.set("date", date);
        bool rejected = false;
        try {
          const ijedi::State wrong(geometry, incorrect);
        } catch (const eckit::BadParameter &error) {
          rejected = std::string(error.what()).find("initial State date differs from") !=
                     std::string::npos;
        }
        require(rejected, "initial native data can be falsely relabeled with a foreign UTC time");
      }
      return 0;
    }
    const ijedi::State initial(geometry,
                               eckit::LocalConfiguration(configuration, "initial condition"));
    const oops::Variables native(
        std::vector<std::string>{"u", "w", "theta_m", "rho_zz", "scalars"});
    ijedi::State state(native, initial);
    const auto original = state.nativeAnalysisValues();
    const auto checkNative = [&]() {
      const auto values = state.nativeAnalysisValues();
      const auto &fields = state.fieldSet();
      require(fields.size() == 5, "native State view omitted an analysis coordinate");
      for (const auto &field : fields) {
        require(nativePayload(geometry, field) == manifest(values).at(field.name()).at("sha256"),
                "native State view changed bytes/order/orientation");
        const auto binding = Json::parse(field.metadata().getString("mpas_descriptor_binding"));
        require(binding.at("descriptor").at("mutability") == "analysis_prognostic" &&
                    binding.at("context").contains("native_state_receipt"),
                "native State view has no content-bound write authority");
      }
      require(fields.field("u").functionspace().get() ==
                      geometry.mpasAtlasGeometry().edgeNormals().get() &&
                  fields.field("w").shape(1) == 56 && fields.field("scalars").shape(2) == 6,
              "native State view lost edge/interface/tracer location");
    };
    checkNative();
    atlas::FieldSet initialViews;
    state.toFieldSet(initialViews);
    int countRejections = 0;
    for (const auto &name : native.variables()) {
      for (const bool levels : {true, false}) {
        atlas::FieldSet fields;
        state.toFieldSet(fields);
        auto field = fields.field(name);
        if (levels) field.set_levels(field.levels() + 1);
        else field.set_variables(field.variables() + 1);
        const auto boundary = state.continuationManifest();
        bool rejectedCount = false;
        try { state.fromFieldSet(fields); } catch (const std::exception &error) {
          rejectedCount = std::string(error.what()).find("Atlas level/component count") !=
                          std::string::npos;
        }
        require(rejectedCount && state.continuationManifest() == boundary,
                "RB4 native write count contradiction was accepted or changed authority");
        ++countRejections;
      }
    }
    require(countRejections == 10, "RB4 native write count attacks were omitted");
    int rejected = 0;
    const auto reject = [&](const auto &action) {
      const auto before = state.continuationManifest();
      const auto time = state.validTime();
      bool caught = false;
      try {
        action();
      } catch (const std::exception &) {
        caught = true;
      }
      require(caught && state.continuationManifest() == before && state.validTime() == time,
              "rejected State operation changed full continuation/time or was accepted");
      ++rejected;
    };
    for (const std::string key :
         {"units", "horizontal_location", "vertical_stagger", "component_basis", "semantic_id",
          "trajectory_receipt", "horizontal_geometry_receipt", "mpas_descriptor_binding"}) {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      fields.field("w").metadata().set(key, "incorrect");
      reject([&] { state.fromFieldSet(fields); });
    }
    // Preserve JSON types: a syntactically invalid binding alone cannot prove
    // that semantic/context substitutions are rejected. Attack every declared
    // property of each of the five native coordinate families independently.
    const auto forged = [](const std::string &key, const Json &value) -> Json {
      if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos) {
          text[0] = text[0] == '0' ? '1' : '0';
          return text;  // Valid SHA syntax, wrong authority/content.
        }
        if (key == "valid_time") {
          return "2026-07-17T06:12:00Z";  // Valid UTC, wrong owned time.
        }
        return text + "-substituted";
      }
      if (value.is_boolean()) {
        return !value.get<bool>();
      }
      if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() + 1;
      }
      if (value.is_number_integer()) {
        return value.get<std::int64_t>() + 1;
      }
      if (value.is_number_float()) {
        return value.get<double>() + 1.;
      }
      if (value.is_null()) {
        return 0;
      }
      if (value.is_array()) {
        auto result = value;
        if (result.size() > 1) {
          std::reverse(result.begin(), result.end());  // Same extent, changed species/order.
        } else {
          result.push_back("qv");
        }
        return result;
      }
      auto result = value;
      result["wheel_sha256"] = std::string(64, '0');
      return result;
    };
    for (const auto &nativeVariable : native) {
      const auto pristine = initialViews.field(nativeVariable.name());
      const auto binding = Json::parse(pristine.metadata().getString("mpas_descriptor_binding"));
      require(binding.at("descriptor").size() == 26 && binding.at("context").size() == 14,
              "State forgery inventory differs from the complete frozen binding");
      for (const std::string section : {"descriptor", "context"}) {
        for (const auto &property : binding.at(section).items()) {
          auto changed = binding;
          changed[section][property.key()] = forged(property.key(), property.value());
          auto field = pristine.clone();
          field.metadata().set("mpas_descriptor_binding", changed.dump());
          atlas::FieldSet fields;
          fields.add(field);
          reject([&] { state.fromFieldSet(fields); });
        }
      }
      auto changed = binding;
      changed["descriptor_registry_digest"] = std::string(64, '0');
      auto field = pristine.clone();
      field.metadata().set("mpas_descriptor_binding", changed.dump());
      atlas::FieldSet fields;
      fields.add(field);
      reject([&] { state.fromFieldSet(fields); });
    }
    {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      atlas::array::make_view<double, 2>(fields.field("rho_zz"))(0, 0) =
          std::numeric_limits<double>::quiet_NaN();
      reject([&] { state.fromFieldSet(fields); });
    }
    {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      atlas::array::make_view<double, 2>(fields.field("theta_m"))(0, 0) = 0.;
      fields.field("theta_m").metadata().set("mpas_payload_receipt",
                                             nativePayload(geometry, fields.field("theta_m")));
      reject([&] { state.fromFieldSet(fields); });
    }
    {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      atlas::array::make_view<double, 2>(fields.field("w"))(0, 1) = 1.;
      reject([&] { state.fromFieldSet(fields); });
    }
    {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      atlas::functionspace::PointCloud foreign(geometry.mpasAtlasGeometry().cellNodes().lonlat());
      fields.field("w").set_functionspace(foreign);
      reject([&] { state.fromFieldSet(fields); });
    }
    const oops::Variables geovars(configuration.getStringVector("typed variables"));
    ijedi::VariableChange change(eckit::LocalConfiguration(), geometry);
    {
      ijedi::State diagnostics(state);
      change.changeVar(diagnostics, geovars);
      require(diagnostics.variables() == geovars &&
                  manifest(diagnostics.nativeAnalysisValues()) == manifest(original),
              "nonlinear variable selection replaced native authority");
      atlas::FieldSet fields;
      diagnostics.toFieldSet(fields);
      require(fields.size() == 12 && fields.field("air_pressure_levels").shape(1) == 56 &&
                  fields.field("air_pressure_at_surface").shape(1) == 1,
              "GeoVaL State views omitted pressure/height stagger");
      reject([&] { state.fromFieldSet(fields); });
    }
    {
      ijedi::State species(
          oops::Variables(std::vector<std::string>{"qv", "qc", "qr", "qi", "qs", "qg"}), state);
      const auto &fields = species.fieldSet();
      int slot = 0;
      for (const auto &var : species.variables()) {
        const auto value = atlas::array::make_view<double, 2>(fields.field(var.name()));
        for (size_t i = 0; i < original.at("scalars").values.size() / 6; ++i) {
          require(value(i / 55, i % 55) == original.at("scalars").values[i * 6 + slot],
                  "individual tracer view changed species/order");
        }
        ++slot;
      }
    }
    std::vector<double> checkpoint;
    state.serialize(checkpoint);
    {
      atlas::FieldSet fields;
      state.toFieldSet(fields);
      auto expected = original;
      for (auto &field : fields) {
        if (field.rank() == 2) {
          const auto row =
              field.name() == "u" ? geometry.mpasAtlasGeometry().nativeToAtlasEdges().at(0) : 0;
          auto value = atlas::array::make_view<double, 2>(field);
          value(row, 1) = std::nextafter(value(row, 1), std::numeric_limits<double>::infinity());
          expected.at(field.name()).values[1] = value(row, 1);
          if (field.name() == "w") {
            value(0, 0) = -0.;
            expected.at("w").values[0] = -0.;
          }
        } else {
          auto value = atlas::array::make_view<double, 3>(field);
          for (size_t slot = 0; slot < 6; ++slot) {
            value(0, 1, slot) =
                std::nextafter(value(0, 1, slot), std::numeric_limits<double>::infinity());
            expected.at("scalars").values[6 + slot] = value(0, 1, slot);
          }
        }
        field.metadata().set("mpas_payload_receipt", nativePayload(geometry, field));
      }
      state.fromFieldSet(fields);
      require(manifest(state.nativeAnalysisValues()) == manifest(expected),
              "absolute native State write rounded/dropped/oriented a field");
      checkNative();
    }
    reject([&] { state.fromFieldSet(initialViews); });
    {
      const auto &fields = state.fieldSet();
      atlas::array::make_view<double, 2>(fields.field("w"))(0, 1) += 1.;
      reject([&] { (void)state.fieldSet(); });
    }
    size_t cursor = 0;
    state.deserialize(checkpoint, cursor);
    require(
        cursor == checkpoint.size() && manifest(state.nativeAnalysisValues()) == manifest(original),
        "State restore did not invalidate edited views");
    checkNative();
    {
      atlas::FieldSet stale;
      state.toFieldSet(stale);
      state.advanceModel(util::Duration("PT12M"));
      checkNative();
      reject([&] { state.fromFieldSet(stale); });
      state = initial;
      require(state.variables() == initial.variables() &&
                  manifest(state.nativeAnalysisValues()) == manifest(original),
              "State assignment failed to invalidate/rebind views");
    }
    {
      const oops::Variables controls(std::vector<std::string>{
          "control_eastward_wind", "control_northward_wind", "control_upward_air_velocity",
          "control_moist_potential_temperature", "control_jacobian_dry_air_density",
          "control_moisture_tracers"});
      ijedi::Increment increment(geometry, controls, state.validTime());
      auto direction = increment.analysisArrays();
      for (auto &[name, array] : direction) {
        const double value = name == "jacobian_dry_air_density" || name == "moisture_tracers"
                                 ? std::ldexp(1., -30)
                                 : std::ldexp(1., -18);
        std::fill(array.values.begin(), array.values.end(), value);
      }
      increment.replaceAnalysis(controls, direction);
      ijedi::State expected(state);
      expected += increment;
      atlas::FieldSet fields;
      increment.toFieldSet(fields);
      state.fromFieldSet(fields);
      require(manifest(state.nativeAnalysisValues()) == manifest(expected.nativeAnalysisValues()),
              "control FieldSet write differs from State += owner P");
    }
    require(rejected == 221, "State view tests omitted a required failure family");
    std::ofstream output(configuration.getString("analysis bridge output") + ".state-views.json");
    output << Json{{"scope", "real_native_and_GeoVaL_State_views_not_model_time_TLAD"},
                   {"negative_controls_rejected", rejected},
                   {"rb4_native_count_rejections", countRejections},
                   {"native_fields", 5},
                   {"individual_tracers", 6},
                   {"geoval_fields", 12}}
                  .dump(2)
           << '\n';
    require(static_cast<bool>(output), "cannot write State view evidence");
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasStateAtlasViews"; }
};
}  // namespace
int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  StateAtlasViews app(oops::mpi::world());
  return run.execute(app);
}
