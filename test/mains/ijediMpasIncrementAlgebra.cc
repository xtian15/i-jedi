/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/functionspace/PointCloud.h"
#include "atlas/option.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/State/State.h"
#include "oops/mpi/mpi.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"

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

Json fieldSetManifest(const atlas::FieldSet &fields) {
  Json result = Json::array();
  for (const auto &field : fields) {
    std::ostringstream metadata;
    metadata << field.metadata();
    ijedi::AtlasOperatorReceipt hash;
    const size_t stride = field.size() / field.shape(0);
    for (atlas::idx_t row = 0; row < field.shape(0); ++row) {
      for (size_t col = 0; col < stride; ++col) {
        hash.real(field.rank() == 2 ? atlas::array::make_view<double, 2>(field)(row, col) :
            atlas::array::make_view<double, 3>(field)(row, col / field.shape(2), col % field.shape(2)));
      }
    }
    const std::vector<atlas::idx_t> shape(field.shape().begin(), field.shape().end());
    result.push_back({{"name", field.name()}, {"metadata", metadata.str()},
                      {"shape", shape}, {"sha256", hash.finish()}});
  }
  std::ostringstream metadata;
  metadata << fields.metadata();
  return {{"fields", result}, {"metadata", metadata.str()}, {"name", fields.name()}};
}

void require(bool test, const std::string &what) {
  if (!test) {
    throw std::runtime_error(what);
  }
}

class IncrementAlgebra final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &configuration) const override {
    ijedi::Geometry geometry(eckit::LocalConfiguration(configuration, "geometry"), getComm());
    const util::DateTime time("2026-07-17T06:00:00Z");
    const std::vector<std::vector<std::string>> spaces{
        {"u", "w", "theta_m", "rho_zz", "scalars"},
        {"control_eastward_wind", "control_northward_wind", "control_upward_air_velocity",
         "control_moist_potential_temperature", "control_jacobian_dry_air_density",
         "control_moisture_tracers"},
        configuration.getStringVector("typed variables")};
    Json output;
    output["scope"] = "real_typed_Increment_algebra_not_OOPS_TLAD_or_covariance";
    int negativeControls = 0;
    int exportControls = 0;
    int countControls = 0;
    int nameControls = 0;
    for (const auto &names : spaces) {
      const oops::Variables variables(names);
      ijedi::Increment x(geometry, variables, time);
      require(x.norm() == 0., "new typed increment is not zero");
      auto values = x.analysisArrays();
      int family = 0;
      for (auto &[name, array] : values) {
        for (size_t i = 0; i < array.values.size(); ++i) {
          array.values[i] = std::ldexp(static_cast<double>((i + 7 * family) % 37) - 18., -12);
        }
        ++family;
      }
      x.replaceAnalysis(variables, values);
      require(manifest(x.analysisArrays()) == manifest(values),
              "typed increment changed native-order bytes");
      const auto before = manifest(values);
      ijedi::Increment copied(x), uninitialized(x, false);
      require(manifest(copied.analysisArrays()) == before && uninitialized.norm() == 0.,
              "copy/copy-false failed");
      const auto mass = x.analysisMeasures();
      long double independentDot = 0;
      for (const auto &[name, array] : values) {
        const auto &weights = mass.at(name).values;
        const size_t repeat = array.values.size() / weights.size();
        for (size_t i = 0; i < array.values.size(); ++i) {
          independentDot +=
              static_cast<long double>(array.values[i]) * weights.at(i / repeat) * array.values[i];
        }
      }
      const double dot = x.dot_product_with(copied);
      require(dot == static_cast<double>(independentDot) && dot > 0. && x.norm() == std::sqrt(dot),
              "weighted dot/norm differs");
      copied.axpy(2., x);
      auto triple = values;
      for (auto &[name, array] : triple) {
        for (double &value : array.values) {
          value *= 3.;
        }
      }
      require(manifest(copied.analysisArrays()) == manifest(triple), "typed axpy differs");
      copied -= x;
      copied *= .5;
      require(manifest(copied.analysisArrays()) == before, "typed subtract/scale differs");
      copied += x;
      copied.zero();
      require(copied.norm() == 0., "typed add/zero failed");
      copied.ones();
      copied.schur_product_with(x);
      require(manifest(copied.analysisArrays()) == before, "typed Schur failed");
      atlas::FieldSet exported;
      x.toFieldSet(exported);
      const auto outputBefore = fieldSetManifest(exported);
      auto outputAlias = exported;
      auto sourceAlias = x.fieldSet();
      for (auto field : x.fieldSet()) {
        for (const bool levelCount : {true, false}) {
          const auto label = levelCount ? field.levels() : field.variables();
          if (levelCount) field.set_levels(label + 1);
          else field.set_variables(label + 1);
          const auto destinationBefore = fieldSetManifest(exported);
          for (const auto &action : std::vector<std::function<void()>>{
                   [&] { (void)x.analysisArrays(); }, [&] { (void)x.norm(); },
                   [&] { x.toFieldSet(exported); }}) {
            bool rejected = false;
            try { action(); } catch (const std::exception &error) {
              rejected = std::string(error.what()).find("Atlas level/component count") !=
                         std::string::npos;
            }
            require(rejected && fieldSetManifest(exported) == destinationBefore,
                    "RB4 count contradiction was accepted or export changed destination");
            ++countControls;
          }
          if (levelCount) field.set_levels(label);
          else field.set_variables(label);
          require(manifest(x.analysisArrays()) == before, "RB4 count rejection changed values");
        }
      }
      // Atlas permits edits to the public name metadata and replacement of a
      // mutable Field handle. Neither must bypass the map/actual-name contract.
      const auto nameDestinationBefore = fieldSetManifest(exported);
      ijedi::Increment importReceiver(x);
      const auto importBefore = fieldSetManifest(importReceiver.fieldSet());
      auto authority = x.fieldSet();
      for (size_t index = 0; index < authority.size(); ++index) {
        const auto canonical = authority[index];
        const std::string name = canonical.name();
        for (bool replace : {false, true}) {
          auto changed = replace ? canonical.clone() : canonical;
          if (replace) authority[index] = changed;
          if (replace) changed.rename("invalid_actual_name");
          else changed.metadata().set("name", "invalid_actual_name");
          for (const auto &action : std::vector<std::function<void()>>{
                   [&] { (void)x.analysisArrays(); }, [&] { (void)x.norm(); },
                   [&] { x.toFieldSet(exported); },
                   [&] { importReceiver.fromFieldSet(authority); }}) {
            bool rejected = false;
            try { action(); } catch (const std::exception &error) {
              rejected = std::string(error.what()).find("MPAS Increment field name changed") !=
                         std::string::npos;
            }
            require(rejected && fieldSetManifest(exported) == nameDestinationBefore &&
                        fieldSetManifest(importReceiver.fieldSet()) == importBefore &&
                        x.validTime() == time && importReceiver.validTime() == time,
                    "RB4 actual-name contradiction was accepted or changed outputs/receiver/time");
            ++nameControls;
          }
          if (replace) authority[index] = canonical;
          else changed.metadata().set("name", name);
          require(manifest(x.analysisArrays()) == before,
                  "RB4 actual-name rejection changed authoritative values");
        }
      }
      x.toFieldSet(sourceAlias);
      require(manifest(x.analysisArrays()) == before && fieldSetManifest(sourceAlias) == outputBefore,
              "export through copied source handle changed authority");
      x.toFieldSet(exported);
      require(fieldSetManifest(outputAlias) == outputBefore,
              "successful export cleared another destination handle");
      ++exportControls;
      exported.metadata().set("caller_sentinel", "preserve on failure");
      const auto failureBefore = fieldSetManifest(exported);
      const auto exportReject = [&](const auto &mutate, const auto &restore,
                                     const std::string &reason) {
        mutate();
        bool failed = false;
        try { x.toFieldSet(exported); } catch (const std::exception &error) {
          failed = std::string(error.what()).find(reason) != std::string::npos;
        }
        const auto after = fieldSetManifest(exported);
        restore();
        require(failed && after == failureBefore && manifest(x.analysisArrays()) == before,
                "failed export destroyed destination or authority");
        ++exportControls;
      };
      auto exportField = x.fieldSet()[0];
      if (exportField.rank() == 2) {
        auto view = atlas::array::make_view<double, 2>(exportField);
        const double original = view(0, 0);
        exportReject([&] { view(0, 0) = std::numeric_limits<double>::quiet_NaN(); },
                     [&] { view(0, 0) = original; }, "nonfinite typed MPAS Increment");
      } else {
        auto view = atlas::array::make_view<double, 3>(exportField);
        const double original = view(0, 0, 0);
        exportReject([&] { view(0, 0, 0) = std::numeric_limits<double>::quiet_NaN(); },
                     [&] { view(0, 0, 0) = original; }, "nonfinite typed MPAS Increment");
      }
      const auto exportBinding = exportField.metadata().getString("mpas_descriptor_binding");
      exportReject([&] { exportField.metadata().set("mpas_descriptor_binding", "wrong"); },
                   [&] { exportField.metadata().set("mpas_descriptor_binding", exportBinding); },
                   "owner binding changed");
      const auto exportSpace = exportField.functionspace();
      exportReject([&] { exportField.set_functionspace(atlas::functionspace::PointCloud(exportSpace.lonlat())); },
                   [&] { exportField.set_functionspace(exportSpace); }, "owner binding changed");
      auto wrongShape = exportField.shape();
      wrongShape[0] -= 1;
      exportReject([&] { exportField.resize(wrongShape); }, [&] { x.fromFieldSet(sourceAlias); },
                   "extent changed");
      const auto correctField = x.fieldSet()[0];
      atlas::Field wrongDtype(correctField.name(), atlas::array::make_datatype<float>(),
                               correctField.shape());
      wrongDtype.set_functionspace(correctField.functionspace());
      exportReject([&] { x.fieldSet()[0] = wrongDtype; }, [&] { x.fromFieldSet(sourceAlias); },
                   "dtype");
      bool sourceRejected = false;
      try { x.toFieldSet(x.fieldSet()); } catch (const std::exception &error) {
        sourceRejected = std::string(error.what()).find("authoritative FieldSet") != std::string::npos;
      }
      require(sourceRejected && manifest(x.analysisArrays()) == before,
              "export replaced its authoritative FieldSet");
      ++exportControls;
      copied.fromFieldSet(exported);
      require(manifest(copied.analysisArrays()) == before, "typed field export/import differs");
      const auto reject = [&](const auto &action) {
        const auto unchanged = manifest(x.analysisArrays());
        const auto oldTime = x.validTime();
        bool rejected = false;
        try {
          action();
        } catch (const std::exception &) {
          rejected = true;
        }
        require(rejected && x.validTime() == oldTime && manifest(x.analysisArrays()) == unchanged,
                "typed invalid operation was accepted or changed increment");
        ++negativeControls;
      };
      copied.updateTime(util::Duration("PT12M"));
      reject([&] { x.axpy(1., copied); });
      reject([&] { (void)x.dot_product_with(copied); });
      reject([&] { x *= std::numeric_limits<double>::infinity(); });
      reject([&] { x.axpy(std::numeric_limits<double>::quiet_NaN(), x); });
      reject([&] {
        auto wrong = values;
        wrong.begin()->second.values.at(0) = std::numeric_limits<double>::quiet_NaN();
        x.replaceAnalysis(variables, wrong);
      });
      reject([&] {
        auto wrong = values;
        wrong.erase(wrong.begin());
        x.replaceAnalysis(variables, wrong);
      });
      for (const std::string key :
           {"units", "semantic_id", "horizontal_location", "vertical_stagger", "component_basis",
            "mpas_descriptor_binding"}) {
        atlas::FieldSet source;
        x.toFieldSet(source);
        source[0].metadata().set(key, "wrong");
        reject([&] { x.fromFieldSet(source); });
      }
      atlas::FieldSet changedTime;
      copied.toFieldSet(changedTime);
      reject([&] { x.fromFieldSet(changedTime); });
      // Same sizes/coordinates are not the location-correct owned FunctionSpace.
      atlas::FieldSet foreign;
      x.toFieldSet(foreign);
      const auto field = foreign[0];
      const auto foreignSpace = atlas::functionspace::PointCloud(field.functionspace().lonlat());
      foreign[0].set_functionspace(foreignSpace);
      reject([&] { x.fromFieldSet(foreign); });
      std::vector<double> serialized;
      x.serialize(serialized);
      require(serialized.size() == x.serialSize(), "typed serialization length differs");
      size_t cursor = 0;
      uninitialized.deserialize(serialized, cursor);
      require(cursor == serialized.size() && manifest(uninitialized.analysisArrays()) == before,
              "typed serialization is not exact");
      auto truncated = serialized;
      truncated.pop_back();
      cursor = 0;
      reject([&] { x.deserialize(truncated, cursor); });
      require(cursor == 0, "failed typed deserialize advanced caller cursor");
      auto corrupted = serialized;
      reinterpret_cast<unsigned char *>(corrupted.data() + 2)[0] ^= 1;
      reject([&] { x.deserialize(corrupted, cursor); });
      require(cursor == 0, "corrupt typed deserialize advanced caller cursor");
      {
        auto special = values;
        for (auto &[name, array] : special) {
          array.values[0] = -0.;
          array.values[1] = std::numeric_limits<double>::denorm_min();
          array.values[2] = std::nextafter(1., 2.);
          array.values[3] = std::numeric_limits<double>::lowest();
          array.values[4] = std::numeric_limits<double>::max();
        }
        ijedi::Increment extremes(x);
        extremes.replaceAnalysis(variables, special);
        std::vector<double> exact;
        extremes.serialize(exact);
        cursor = 0;
        uninitialized.deserialize(exact, cursor);
        require(cursor == exact.size() && exact.size() == extremes.serialSize() &&
                    manifest(uninitialized.analysisArrays()) == manifest(special),
                "binary serialization lost signed zero, subnormals, ULPs or finite extremes");
      }
      const auto rejectFrame = [&](std::vector<double> frame) {
        cursor = 0;
        reject([&] { x.deserialize(frame, cursor); });
        require(cursor == 0, "rejected binary frame advanced caller cursor");
      };
      auto badMagic = serialized;
      badMagic[0] += 1.;
      rejectFrame(std::move(badMagic));
      auto badCount = serialized;
      badCount[1] = std::numeric_limits<double>::quiet_NaN();
      rejectFrame(std::move(badCount));
      auto wrongLength = serialized;
      wrongLength[1] += 1.;
      rejectFrame(std::move(wrongLength));
      const auto mutatePayload = [&](bool nonfinite, bool rehash) {
        auto frame = serialized;
        const size_t bytes = static_cast<size_t>(frame[1]);
        auto *payload = reinterpret_cast<unsigned char *>(frame.data() + 2);
        std::uint64_t metadataBytes = 0;
        for (size_t byte = 0; byte < 8; ++byte) {
          metadataBytes |= static_cast<std::uint64_t>(payload[byte]) << (8 * byte);
        }
        if (nonfinite) {
          const std::uint64_t nan = 0x7ff8000000000000ULL;
          for (size_t byte = 0; byte < 8; ++byte) {
            payload[8 + metadataBytes + byte] = static_cast<unsigned char>(nan >> (8 * byte));
          }
        } else {
          payload[8] ^= 1;
        }
        if (rehash) {
          ijedi::AtlasOperatorReceipt hash;
          hash.string(std::string(reinterpret_cast<char *>(payload), bytes - 64));
          const auto digest = hash.finish();
          std::memcpy(payload + bytes - 64, digest.data(), digest.size());
        }
        return frame;
      };
      rejectFrame(mutatePayload(true, false));
      auto lastByte = serialized;
      reinterpret_cast<unsigned char *>(lastByte.data())[lastByte.size() * sizeof(double) - 1] ^= 1;
      rejectFrame(std::move(lastByte));
      std::vector<double> foreignTime;
      copied.serialize(foreignTime);
      rejectFrame(std::move(foreignTime));
      rejectFrame(mutatePayload(true, true));
      rejectFrame(mutatePayload(false, true));
      output["spaces"].push_back({{"namespace", x.analysisNamespace()},
                                  {"arrays", before},
                                  {"weighted_dot", dot},
                                  {"weighted_norm", x.norm()},
                                  {"serialized_words", serialized.size()}});
    }
    for (const auto &names : spaces) {
      atlas::Field inserted("invalid-extra", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(2, 3));
      atlas::FieldSet alias;
      {
        ijedi::Increment carrier(geometry, oops::Variables(names), time);
        alias = carrier.fieldSet();
        alias.add(inserted);
        alias[alias.size() - 1] = inserted.clone();
        bool rejected = false;
        try {
          (void)carrier.analysisArrays();
        } catch (const std::exception &error) {
          rejected = std::string(error.what()).find("MPAS Increment field inventory changed") !=
                     std::string::npos;
        }
        require(rejected, "Inserted Increment inventory was accepted");
      }
      alias = atlas::FieldSet();
      inserted.rename("after-Increment-and-inserted-alias");
    }
    bool mixedRejected = false;
    try {
      ijedi::Increment mixed(
          geometry, oops::Variables(std::vector<std::string>{"u", "control_eastward_wind"}), time);
    } catch (const std::exception &) {
      mixedRejected = true;
    }
    require(mixedRejected, "mixed typed namespace was accepted");
    ++negativeControls;
    ijedi::State state(geometry, eckit::LocalConfiguration(configuration, "initial condition"));
    const auto before = state.continuationManifest();
    ijedi::Increment zeroNative(geometry, oops::Variables(spaces[0]), time);
    state += zeroNative;
    require(state.continuationManifest() == before && state.validTime() == time,
            "zero State native update changed continuation/time");
    ijedi::Increment zeroControl(geometry, oops::Variables(spaces[1]), time);
    state += zeroControl;
    require(state.continuationManifest() == before,
            "zero State control update changed continuation");
    ijedi::Increment diagnostic(geometry, oops::Variables(spaces[2]), time);
    bool diagnosticRejected = false;
    try {
      state += diagnostic;
    } catch (const std::exception &) {
      diagnosticRejected = true;
    }
    require(diagnosticRejected && state.continuationManifest() == before,
            "diagnostic State write was accepted or changed continuation");
    ++negativeControls;
    output["negative_controls_rejected"] = negativeControls;
    require(negativeControls == 74, "typed algebra omitted required negative controls");
    output["ra4_export_controls"] = exportControls;
    output["rb4_count_controls"] = countControls;
    output["rb4_name_controls"] = nameControls;
    require(nameControls == 184, "RB4 actual-name/map or replacement attacks were omitted");
    require(countControls == 138, "RB4 omitted a layer/interface/surface/tracer count attack");
    require(exportControls == 21, "export atomicity controls were omitted");
    std::ofstream file(configuration.getString("analysis bridge output") + ".increment.json");
    file << output.dump(2) << '\n';
    require(static_cast<bool>(file), "cannot write typed Increment evidence");
    return 0;
  }

 private:
  std::string appname() const override { return "ijedi::MpasIncrementAlgebra"; }
};
}  // namespace

int main(int argc, char **argv) {
  oops::Run run(argc, argv);
  IncrementAlgebra app(oops::mpi::world());
  return run.execute(app);
}
