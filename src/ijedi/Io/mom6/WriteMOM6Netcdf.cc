#include "ijedi/Io/mom6/WriteMOM6Netcdf.h"

#include <netcdf.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas/array.h"
#include "atlas/field.h"
#include "atlas/functionspace/NodeColumns.h"
#include "atlas/option.h"

#include "eckit/exception/Exceptions.h"
#include "eckit/mpi/Comm.h"

#include "oops/util/Logger.h"

namespace ijedi {

// ---------------------------------------------------------------------------
void writeMOM6Netcdf(const std::string & filepath,
                     const atlas::FieldSet & x,
                     const std::vector<std::string> & fileVarNames,
                     const int ni, const int nj, const int nz,
                     const eckit::mpi::Comm & comm) {
  const bool isRoot = (comm.rank() == 0);

  // --- 1. Gather every field to rank 0 using Atlas NodeColumns::gather ---
  const auto fs =
      atlas::functionspace::NodeColumns(x.field(0).functionspace());

  // The JEDI function space is unstructured and only contains active points
  // (ocean + fringe); land points are omitted.  After fs.gather() the global
  // field has nActiveGlobal entries -- NOT ni*nj.  We therefore also gather
  // the global-index field so we know where each gathered node sits on the
  // full structured grid.
  //
  // mom6_structured_index = jG * ni + iG + 1 (1-based). Atlas global IDs
  // are dense in the active space and are not structured-file addresses.

  // 1a. Build a local field holding each node's global index as double,
  //     then gather it to root.
  auto localGidx = atlas::array::make_view<atlas::gidx_t, 1>(
      fs.nodes().field("mom6_structured_index"));
  atlas::Field gidxField = fs.createField<double>(
      atlas::option::name("__gidx") | atlas::option::levels(1));
  {
    auto gv = atlas::array::make_view<double, 2>(gidxField);
    for (atlas::idx_t n = 0; n < fs.nodes().size(); ++n)
      gv(n, 0) = static_cast<double>(localGidx(n));
  }
  atlas::Field globalGidxField = fs.createField<double>(
      atlas::option::name("__gidx") | atlas::option::levels(1)
      | atlas::option::global());
  fs.gather(gidxField, globalGidxField);

  // 1b. Gather all data fields.
  std::vector<atlas::Field> globalFields;
  globalFields.reserve(x.size());

  int fi = 0;
  for (const auto & field : x) {
    const std::string & varName = fileVarNames[fi++];
    if (varName.empty()) {
      globalFields.emplace_back();          // placeholder
      continue;
    }
    const int nLevels = field.shape(1);
    atlas::Field gf = fs.createField<double>(
        atlas::option::name(varName)
        | atlas::option::levels(nLevels)
        | atlas::option::global());
    fs.gather(field, gf);
    globalFields.push_back(gf);
  }

  // --- 2. Root creates the NetCDF file, defines dims/vars, writes data ---
  if (isRoot) {
    std::remove(filepath.c_str());
    int ncid;
    ASSERT_MSG(nc_create(filepath.c_str(), NC_CLOBBER, &ncid) == NC_NOERR,
               "writeMOM6Netcdf: cannot create " + filepath);

    // Define dimensions
    int dim_time, dim_layer, dim_lath, dim_lonh;
    nc_def_dim(ncid, "Time",  NC_UNLIMITED, &dim_time);
    nc_def_dim(ncid, "Layer", nz,           &dim_layer);
    nc_def_dim(ncid, "lath",  nj,           &dim_lath);
    nc_def_dim(ncid, "lonh",  ni,           &dim_lonh);

    // Define variables (skip duplicates, e.g. two JEDI names → same file var)
    std::unordered_map<std::string, int> varids;
    fi = 0;
    for (const auto & field : x) {
      const std::string & varName = fileVarNames[fi++];
      if (varName.empty() || varids.count(varName)) continue;
      int varid;
      if (field.shape(1) > 1) {
        int dimids[4] = {dim_time, dim_layer, dim_lath, dim_lonh};
        nc_def_var(ncid, varName.c_str(), NC_DOUBLE, 4, dimids, &varid);
      } else {
        int dimids[3] = {dim_time, dim_lath, dim_lonh};
        nc_def_var(ncid, varName.c_str(), NC_DOUBLE, 3, dimids, &varid);
      }
      varids[varName] = varid;
    }
    nc_enddef(ncid);

    // Retrieve the gathered global indices (only valid on root)
    auto gIdx = atlas::array::make_view<double, 2>(globalGidxField);
    const int nGathered = static_cast<int>(globalGidxField.shape(0));

    // --- Diagnostic: validate gathered global indices ---
    {
      int nBadIdx = 0;
      double minIdx = 1.0e30, maxIdx = -1.0e30;
      for (int n = 0; n < nGathered; ++n) {
        const double g = gIdx(n, 0);
        if (g < minIdx) minIdx = g;
        if (g > maxIdx) maxIdx = g;
        const int flatIdx = static_cast<int>(g) - 1;
        if (flatIdx < 0 || flatIdx >= nj * ni) ++nBadIdx;
      }
      oops::Log::info() << "writeMOM6Netcdf: gathered " << nGathered
                        << " nodes (full grid = " << nj * ni << ")"
                        << ", gIdx range [" << minIdx << ", " << maxIdx << "]"
                        << ", out-of-range count = " << nBadIdx << std::endl;
    }

    // Write each gathered field
    fi = 0;
    for (const auto & field : x) {
      const std::string & varName = fileVarNames[fi];
      const auto & gf = globalFields[fi];
      ++fi;
      if (varName.empty()) continue;

      auto gView = atlas::array::make_view<double, 2>(gf);
      const int nGlobal = static_cast<int>(gf.shape(0));
      const int nLevels = static_cast<int>(gf.shape(1));

      // The gathered field has nActiveGlobal entries (ocean+fringe only).
      // Use the independently retained structured index to place each value.
      if (nLevels > 1) {
        std::vector<double> buf(static_cast<size_t>(nz * nj * ni), 0.0);
        for (int n = 0; n < nGlobal; ++n) {
          const int flatIdx = static_cast<int>(gIdx(n, 0)) - 1;
          const int iG = flatIdx % ni;
          const int jG = flatIdx / ni;
          for (int k = 0; k < nLevels; ++k)
            buf[static_cast<size_t>(k * nj * ni + jG * ni + iG)] = gView(n, k);
        }
        const size_t start[4] = {0, 0, 0, 0};
        const size_t count[4] = {1, static_cast<size_t>(nz),
                                    static_cast<size_t>(nj),
                                    static_cast<size_t>(ni)};
        nc_put_vara_double(ncid, varids.at(varName), start, count, buf.data());
      } else {
        std::vector<double> buf(static_cast<size_t>(nj * ni), 0.0);
        for (int n = 0; n < nGlobal; ++n) {
          const int flatIdx = static_cast<int>(gIdx(n, 0)) - 1;
          buf[flatIdx] = gView(n, 0);
        }
        const size_t start[3] = {0, 0, 0};
        const size_t count[3] = {1, static_cast<size_t>(nj),
                                    static_cast<size_t>(ni)};
        nc_put_vara_double(ncid, varids.at(varName), start, count, buf.data());
      }

      oops::Log::info() << "writeMOM6Netcdf: " << field.name()
                        << " -> " << varName << " to " << filepath << std::endl;
    }

    nc_close(ncid);
  }

  // Ensure the file is fully written before any rank returns (and potentially
  // tries to read it back).
  comm.barrier();
}

// ---------------------------------------------------------------------------
}  // namespace ijedi
