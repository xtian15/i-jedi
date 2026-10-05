/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <sys/resource.h>

#include <nlohmann/json.hpp>
#include "atlas/library/Library.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Python/MpasBackendContext.h"

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

// Independent analytic truth on the unit sphere, not another interpolator.
std::array<double, 8> harmonics(double lat, double lon) {
  const double x = std::cos(lat) * std::cos(lon), y = std::cos(lat) * std::sin(lon), z = std::sin(lat);
  return {x, y, z, x * y, x * z, y * z, x * x - y * y, 3 * z * z - 1};
}
}

int main(int argc, char **argv) {
  if (argc != 4) { std::cerr << "usage: ijedi_mpas_atlas_point_scaling SNAPSHOT LOCATIONS OUTPUT\n"; return 2; }
  try {
    atlas::initialize(argc, argv);
    Json snapshotJson, locations;
    std::ifstream(argv[1]) >> snapshotJson;
    std::ifstream(argv[2]) >> locations;
    ijedi::MpasHorizontalSnapshot snapshot;
    snapshot.cells = snapshotJson.at("cells"); snapshot.edges = snapshotJson.at("edges");
    snapshot.vertices = snapshotJson.at("vertices"); snapshot.cellWidth = snapshotJson.at("cell_width");
    snapshot.receipt = snapshotJson.at("receipt");
    snapshot.sphereRadiusMetres = snapshotJson.at("sphere_radius_metres");
    snapshot.angleUnits = snapshotJson.at("angle_units");
    snapshot.lengthUnits = snapshotJson.at("length_units");
    snapshot.areaUnits = snapshotJson.at("area_units");
    snapshot.reals = snapshotJson.at("reals").get<decltype(snapshot.reals)>();
    snapshot.integers = snapshotJson.at("integers").get<decltype(snapshot.integers)>();
    const auto construction = Clock::now();
    const ijedi::MpasAtlasGeometry geometry(snapshot);
    const double geometrySeconds = seconds(construction);
    const auto lat = locations.at("latitude_degrees").get<std::vector<double>>();
    const auto lon = locations.at("longitude_degrees").get<std::vector<double>>();
    const std::string compiler = atlas::Library::instance().gitsha1(40);
    const auto setup = Clock::now();
    const auto op = geometry.pointOperator(lat, lon, compiler);
    const double setupSeconds = seconds(setup);
    const auto hit = Clock::now();
    for (int i = 0; i < 5; ++i) if (geometry.pointOperator(lat, lon, compiler).get() != op.get()) {
      throw std::runtime_error("Atlas point cache hit recompiled or substituted the operator");
    }
    const double hitSeconds = seconds(hit) / 5;
    const auto payload = op->serializeCache();
    const auto hydration = Clock::now();
    const ijedi::MpasAtlasPointOperator restored(geometry, lat, lon, compiler, payload, op->cacheReceipt());
    const double hydrationSeconds = seconds(hydration);
    if (op->nonzeros() > 3 * lat.size()) throw std::runtime_error("point operator storage is not three-node sparse");
    std::vector<double> source(snapshot.cells * 8);
    for (size_t c = 0; c < snapshot.cells; ++c) {
      const auto value = harmonics(snapshot.reals.at("latCell")[c], snapshot.reals.at("lonCell")[c]);
      for (size_t k = 0; k < value.size(); ++k) source[c * 8 + k] = value[k];
    }
    const auto sampled = op->apply(source, 8);
    if (sampled != restored.apply(source, 8)) throw std::runtime_error("hydrated analytic interpolation differs elementwise");
    constexpr double radians = 3.141592653589793238462643383279502884 / 180.;
    std::array<long double, 8> square{};
    for (size_t t = 0; t < lat.size(); ++t) {
      const auto exact = harmonics(lat[t] * radians, lon[t] * radians);
      for (size_t k = 0; k < square.size(); ++k) {
        const long double error = static_cast<long double>(sampled[t * 8 + k]) - exact[k];
        if (!std::isfinite(error)) throw std::runtime_error("nonfinite analytic interpolation error");
        square[k] += error * error;
      }
    }
    std::array<double, 8> rms;
    for (size_t k = 0; k < rms.size(); ++k) rms[k] = std::sqrt(square[k] / lat.size());
    Json timings = Json::array();
    double maximumAdjoint = 0;
    for (size_t levels : {1, 55, 56}) {
      for (int seed = 0; seed < 4; ++seed) {
      std::vector<double> x(op->sourceSize() * levels, 1.);
      std::vector<double> y(op->targetSize() * levels);
      std::mt19937_64 generator(20260930 + seed); std::normal_distribution<double> normal;
      for (double &value : y) value = normal(generator);
      const auto constant = op->apply(x, levels);
      for (double value : constant) if (!std::isfinite(value) || std::abs(value - 1.) > 1.e-13) {
        throw std::runtime_error("scaled Atlas operator fails constant preservation");
      }
      for (double &value : x) value = normal(generator);
      Json repeats = Json::array();
      for (int repeat = 0; repeat < 5; ++repeat) {
        const auto start = Clock::now();
        const auto result = op->apply(x, levels);
        repeats.push_back(seconds(start));
        if (result != restored.apply(x, levels)) throw std::runtime_error("scaled cached application differs elementwise");
      }
      const auto wx = op->apply(x, levels), adjoint = op->applyWeightedAdjoint(y, levels);
      long double lhs = 0, rhs = 0, norm = 0;
      for (size_t i = 0; i < y.size(); ++i) {
        lhs += static_cast<long double>(wx[i]) * y[i];
        norm += std::abs(static_cast<long double>(wx[i]) * y[i]);
      }
      for (size_t i = 0; i < x.size(); ++i) {
        rhs += static_cast<long double>(x[i]) * adjoint[i] * op->sourceMeasures()[i / levels];
      }
      const double residual = std::abs(lhs - rhs) / norm;
      if (!std::isfinite(residual) || residual > 2.e-14) throw std::runtime_error("scaled weighted adjoint fails");
      maximumAdjoint = std::max(maximumAdjoint, residual);
      timings.push_back({{"levels", levels}, {"seed", seed}, {"apply_seconds", repeats}});
      }
    }
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("cannot measure process RSS");
#ifdef __APPLE__
    const auto peakRssBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    const auto peakRssBytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    const Json output{{"source_cells", snapshot.cells}, {"targets", lat.size()},
                      {"atlas_compiler_identity", compiler}, {"geometry_receipt", geometry.receipt()},
                      {"cache_receipt", op->cacheReceipt()}, {"cache_key", op->cacheKey()},
                      {"nonzeros", op->nonzeros()}, {"artifact_bytes", payload.size()},
                      {"geometry_seconds", geometrySeconds}, {"setup_seconds", setupSeconds},
                      {"cache_hit_seconds", hitSeconds}, {"cache_hydration_seconds", hydrationSeconds},
                      {"apply_trials", timings}, {"analytic_rms", rms},
                      {"weighted_adjoint_max", maximumAdjoint},
                      {"adjoint_seeds", 4},
                      {"peak_rss_bytes", peakRssBytes}};
    std::ofstream stream(argv[3]); stream << output.dump(2);
    if (!stream) throw std::runtime_error("cannot write Atlas scaling evidence");
    atlas::finalize();
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
