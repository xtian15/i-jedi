/*
 * (C) Copyright 2026 IC Weather LLC
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */
#include <netcdf.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "atlas/array.h"
#include "atlas/functionspace/NodeColumns.h"
#include "atlas/library/Library.h"
#include "atlas/mesh/IsGhostNode.h"
#include "atlas/option.h"
#include "eckit/config/YAMLConfiguration.h"
#include "eckit/filesystem/PathName.h"
#include "eckit/mpi/Comm.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/base/AtlasMeshBuilderContract.h"
#include "ijedi/Io/mom6/ReadMOM6Netcdf.h"
#include "ijedi/Io/mom6/WriteMOM6Netcdf.h"

namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
void ncCheck(int code) { require(code == NC_NOERR, nc_strerror(code)); }
std::vector<double> readVariable(const std::string &path, const std::string &name) {
  int ncid, vid, ndims;
  ncCheck(nc_open(path.c_str(), NC_NOWRITE, &ncid));
  ncCheck(nc_inq_varid(ncid, name.c_str(), &vid));
  ncCheck(nc_inq_varndims(ncid, vid, &ndims));
  std::vector<int> dims(ndims);
  ncCheck(nc_inq_vardimid(ncid, vid, dims.data()));
  size_t count = 1;
  for (int dim : dims) { size_t length; ncCheck(nc_inq_dimlen(ncid, dim, &length)); count *= length; }
  std::vector<double> values(count);
  ncCheck(nc_get_var_double(ncid, vid, values.data()));
  ncCheck(nc_close(ncid));
  return values;
}
std::vector<long> allIds(const eckit::mpi::Comm &comm, const std::vector<long> &local) {
  std::vector<int> counts(comm.size()), offsets(comm.size());
  comm.allGather(static_cast<int>(local.size()), counts.begin(), counts.end());
  for (size_t p = 1; p < counts.size(); ++p) offsets[p] = offsets[p-1] + counts[p-1];
  std::vector<long> global(offsets.back() + counts.back());
  comm.allGatherv(local.begin(), local.end(), global.data(), counts.data(), offsets.data());
  return global;
}
bool sameDouble(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
}

int main(int argc, char **argv) {
  require(argc == 3, "usage: mesh contract CONFIG NEW_OUTPUT_FILE");
  atlas::initialize(argc, argv);
  const auto &comm = eckit::mpi::comm();
  // Unit controls: ownership binding must preserve all unrelated topology bits
  // and remain idempotent. An invalid ghost value must reject.
  {
    atlas::Mesh mesh;
    mesh.nodes().resize(2);
    auto g = atlas::array::make_view<int, 1>(mesh.nodes().ghost());
    auto f = atlas::array::make_view<int, 1>(mesh.nodes().flags());
    using T = atlas::mesh::Nodes::Topology;
    g(0) = 0; g(1) = 1;
    f(0) = T::LAND | T::GHOST; f(1) = T::BC | T::PERIODIC;
    ijedi::bindMeshBuilderOwnership(mesh);
    require(f(0) == T::LAND && f(1) == (T::BC | T::PERIODIC | T::GHOST), "ownership binding changed other flags");
    ijedi::bindMeshBuilderOwnership(mesh);
    require(f(0) == T::LAND && f(1) == (T::BC | T::PERIODIC | T::GHOST), "ownership binding is not idempotent");
    g(1) = 2;
    bool rejected = false;
    try { ijedi::bindMeshBuilderOwnership(mesh); } catch (const eckit::BadValue &) { rejected = true; }
    require(rejected, "nonbinary ghost value was accepted");
  }
  eckit::YAMLConfiguration config{eckit::PathName(argv[1])};
  const auto geometryConfig = config.getSubConfiguration("geometry");
  const auto begin = std::chrono::steady_clock::now();
  ijedi::Geometry geometry(geometryConfig, comm);
  const double setup = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  atlas::functionspace::NodeColumns fs(geometry.functionSpace());
  // Explicit test-only corruptions. They must fail the independent checks below.
  const std::string fault = std::getenv("IJEDI_CONTRACT_FAULT") ? std::getenv("IJEDI_CONTRACT_FAULT") : "";
  if (fault == "clear_ghost_flags") {
    auto flags = atlas::array::make_view<int, 1>(fs.nodes().flags());
    for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n) flags(n) &= ~atlas::mesh::Nodes::Topology::GHOST;
  } else if (fault == "structured_as_global") {
    auto dense = atlas::array::make_view<atlas::gidx_t, 1>(fs.nodes().global_index());
    dense.assign(atlas::array::make_view<atlas::gidx_t, 1>(fs.nodes().field("mom6_structured_index")));
  } else if (fault == "shift_structured") {
    auto structured = atlas::array::make_view<atlas::gidx_t, 1>(fs.nodes().field("mom6_structured_index"));
    for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n) structured(n) = structured(n) % 2520 + 1;
  } else {
    require(fault.empty(), "unknown contract fault");
  }
  const auto ids = atlas::array::make_view<atlas::gidx_t, 1>(fs.nodes().global_index());
  const auto ghost = atlas::array::make_view<int, 1>(fs.nodes().ghost());
  atlas::mesh::IsGhostNode topologyGhost(fs.nodes());
  std::vector<long> localOwned;
  int ownershipErrors = 0;
  for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n) {
    ownershipErrors += (ghost(n) != 0) != topologyGhost(n);
    if (!ghost(n)) localOwned.push_back(ids(n));
  }
  int globalErrors;
  comm.allReduce(ownershipErrors, globalErrors, eckit::mpi::sum());
  require(globalErrors == 0, "ghost array and topology ownership disagree");
  auto globalIds = allIds(comm, localOwned);
  std::sort(globalIds.begin(), globalIds.end());
  for (size_t i = 0; i < globalIds.size(); ++i)
    require(globalIds[i] == static_cast<long>(i+1), "owned IDs are not a unique dense permutation");
  require(fs.nb_nodes_global() == globalIds.size(), "Atlas allocation differs from unique ownership");
  if (geometry.type() == "mom6") require(globalIds.size() == 2221, "fixture must retain all 2221 active nodes");
  atlas::Field local = fs.createField<double>(atlas::option::name("ordered") | atlas::option::levels(3));
  auto values = atlas::array::make_view<double, 2>(local);
  for (atlas::idx_t n = 0; n < values.shape(0); ++n)
    for (int k = 0; k < 3; ++k) values(n,k) = 1000. * ids(n) + k;
  atlas::Field global = fs.createField<double>(atlas::option::name("ordered") |
      atlas::option::levels(3) | atlas::option::global());
  fs.gather(local, global);
  if (comm.rank() == 0) {
    const auto gathered = atlas::array::make_view<double, 2>(global);
    for (atlas::idx_t n = 0; n < gathered.shape(0); ++n)
      for (int k = 0; k < 3; ++k)
        require(gathered(n,k) == 1000. * (n+1) + k, "gather changed global ordering");
  }
  atlas::Field restored = fs.createField<double>(atlas::option::name("restored") | atlas::option::levels(3));
  fs.scatter(global, restored); fs.haloExchange(restored);
  const auto recovered = atlas::array::make_view<double, 2>(restored);
  for (atlas::idx_t n = 0; n < recovered.shape(0); ++n)
    for (int k = 0; k < 3; ++k) require(recovered(n,k) == values(n,k), "scatter/halo changed values");

  size_t fileValues = 0;
  if (geometry.type() == "mom6") {
    const auto input = geometryConfig.getSubConfiguration("MOM_input");
    const int ni = input.getInt("NIGLOBAL"), nj = input.getInt("NJGLOBAL"), nz = input.getInt("NZ");
    const auto structured = atlas::array::make_view<atlas::gidx_t, 1>(fs.nodes().field("mom6_structured_index"));
    const auto lonlat = atlas::array::make_view<double, 2>(fs.nodes().lonlat());
    const auto x = readVariable(geometryConfig.getString("ocean_hgrid"), "x");
    const auto y = readVariable(geometryConfig.getString("ocean_hgrid"), "y");
    require(x.size() == static_cast<size_t>((2*ni+1)*(2*nj+1)) && x.size() == y.size(), "grid oracle shape");
    std::vector<long> localStructured;
    for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n) {
      require(structured(n) > 0 && structured(n) <= ni*nj, "structured address out of range");
      const long flat = structured(n)-1;
      const size_t index = (2*(flat/ni)+1)*(2*ni+1) + 2*(flat%ni)+1;
      require(std::remainder(lonlat(n,0)-x[index],360.) == 0. && lonlat(n,1) == y[index],
              "physical node shifted relative to independent hgrid read");
      if (!ghost(n)) localStructured.push_back(structured(n));
    }
    const auto allStructured = allIds(comm, localStructured);
    std::set<long> active(allStructured.begin(), allStructured.end());
    require(active.size() == 2221 && *active.begin() == 17 && *active.rbegin() == 2518,
            "physical structured active set changed");
    const std::string restart = geometryConfig.getString("vertical geometry from");
    const auto temperature = readVariable(restart, "Temp");
    const auto surface = readVariable(restart, "ave_ssh");
    require(temperature.size() == static_cast<size_t>(ni*nj*nz) && surface.size() == static_cast<size_t>(ni*nj),
            "restart oracle shape");
    atlas::FieldSet fields;
    fields.add(fs.createField<double>(atlas::option::name("temperature") | atlas::option::levels(nz)));
    fields.add(fs.createField<double>(atlas::option::name("surface") | atlas::option::levels(1)));
    ijedi::readMOM6Netcdf({restart}, fields, {"Temp", "ave_ssh"}, {0., 0.}, comm);
    const auto t = atlas::array::make_view<double, 2>(fields.field("temperature"));
    const auto s = atlas::array::make_view<double, 2>(fields.field("surface"));
    for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n) {
      const size_t flat = structured(n)-1;
      for (int k = 0; k < nz; ++k) require(sameDouble(t(n,k),temperature[k*ni*nj+flat]), "raw read shifted a level/node");
      require(sameDouble(s(n,0),surface[flat]), "raw surface read shifted a node");
    }
    ijedi::writeMOM6Netcdf(argv[2], fields, {"Temp", "ave_ssh"}, ni, nj, nz, comm);
    if (comm.rank() == 0) {
      const auto writtenT = readVariable(argv[2], "Temp"), writtenS = readVariable(argv[2], "ave_ssh");
      require(writtenT.size() == temperature.size() && writtenS.size() == surface.size(), "output shape changed");
      for (size_t flat = 0; flat < surface.size(); ++flat) {
        const bool retained = active.count(flat+1);
        require(sameDouble(writtenS[flat], retained ? surface[flat] : 0.), "surface output moved a physical node");
        for (int k = 0; k < nz; ++k) {
          const size_t index = k*ni*nj+flat;
          require(sameDouble(writtenT[index], retained ? temperature[index] : 0.), "output moved a physical node/level");
        }
      }
    }
    fileValues = temperature.size()+surface.size();
  }
  if (comm.rank() == 0) std::cout << "contract_pass model=" << geometry.type()
      << " ranks=" << comm.size() << " unique_owned=" << globalIds.size()
      << " file_values=" << fileValues << " setup_seconds=" << setup << std::endl;
}
