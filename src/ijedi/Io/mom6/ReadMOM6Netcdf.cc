#include "ijedi/Io/mom6/ReadMOM6Netcdf.h"

#include <netcdf.h>

#include <string>
#include <vector>

#include "atlas/array.h"
#include "atlas/functionspace/NodeColumns.h"
#include "atlas/option.h"

#include "eckit/exception/Exceptions.h"
#include "eckit/mpi/Comm.h"

#include "oops/util/Logger.h"

namespace ijedi {

namespace {

std::string findDimName(int ncid,
                        const std::string & role,
                        const std::vector<std::string> & candidates,
                        size_t * len) {
  int dimid;
  for (const auto & name : candidates) {
    if (nc_inq_dimid(ncid, name.c_str(), &dimid) == NC_NOERR) {
      nc_inq_dimlen(ncid, dimid, len);
      return name;
    }
  }

  std::string tried;
  for (const auto & name : candidates) {
    if (!tried.empty()) tried += ", ";
    tried += name;
  }
  throw eckit::Exception(
      "readMOM6Netcdf: cannot find " + role +
          " dimension. Tried: " + tried,
      Here());
}

}  // namespace

// ---------------------------------------------------------------------------
void readMOM6Netcdf(const std::vector<std::string> & filepaths,
                    atlas::FieldSet & x,
                    const std::vector<std::string> & fileVarNames,
                    const std::vector<double> & scalings,
                    const eckit::mpi::Comm & comm) {
  const bool isRoot = (comm.rank() == 0);
  const auto fs =
      atlas::functionspace::NodeColumns(x.field(0).functionspace());

  // --- 1. Gather global indices to root (collective, done once). ---
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
  fs.gather(gidxField, globalGidxField);  // collective

  auto gIdx = atlas::array::make_view<double, 2>(globalGidxField);
  const int nGlobal = static_cast<int>(globalGidxField.shape(0));

  // --- 2. Pre-allocate one global field per output field (all ranks). ---
  std::vector<atlas::Field> globalFields;
  globalFields.reserve(x.size());
  for (const auto & field : x) {
    globalFields.push_back(fs.createField<double>(
        atlas::option::name(field.name())
        | atlas::option::levels(field.shape(1))
        | atlas::option::global()));
  }

  // --- 3. Root reads each file in order; a later file may overwrite an
  //        earlier one, but in practice variables don't overlap. ---
  if (isRoot) {
    for (const auto & filepath : filepaths) {
      int ncid = -1;
      ASSERT_MSG(nc_open(filepath.c_str(), NC_NOWRITE, &ncid) == NC_NOERR,
                 "readMOM6Netcdf: cannot open " + filepath);

      size_t dimNi = 0, dimNj = 0, dimNz = 0;
      const std::string dimX =
        findDimName(ncid, "x (longitude)", {"lonh", "xh", "xaxis_1", "nx", "ni"}, &dimNi);
      const std::string dimY =
        findDimName(ncid, "y (latitude)", {"lath", "yh", "yaxis_1", "ny", "nj"}, &dimNj);

      // z dimension is optional: 2D files (sea ice, fix) may not have one
      bool hasZDim = false;
      for (const auto & zName : std::vector<std::string>{"Layer", "z_l", "zaxis_1", "nz"}) {
        int dimid;
        if (nc_inq_dimid(ncid, zName.c_str(), &dimid) == NC_NOERR) {
          nc_inq_dimlen(ncid, dimid, &dimNz);
          hasZDim = true;
          break;
        }
      }

      const int file_ni = static_cast<int>(dimNi);
      const int file_nj = static_cast<int>(dimNj);
      const int file_nz = static_cast<int>(dimNz);
      const int nStructured = file_ni * file_nj;

      for (int n = 0; n < nGlobal; ++n) {
        const int flatIdx = static_cast<int>(gIdx(n, 0)) - 1;
        if (flatIdx < 0 || flatIdx >= nStructured) {
          nc_close(ncid);
          throw eckit::BadValue(
              "readMOM6Netcdf: global index " + std::to_string(flatIdx + 1) +
                  " is out of bounds for file horizontal dimensions " +
                  dimX + "=" + std::to_string(file_ni) +
                  ", " + dimY + "=" + std::to_string(file_nj) +
                  " in " + filepath,
              Here());
        }
      }

      int fi = 0;
      for (const auto & field : x) {
        const std::string & fileVarName = fileVarNames[fi];
        const double scale = scalings[fi];
        const int nLevels = field.shape(1);

        if (!fileVarName.empty()) {
          int varid;
          const bool found =
              (nc_inq_varid(ncid, fileVarName.c_str(), &varid) == NC_NOERR);

          if (found) {
            auto gView = atlas::array::make_view<double, 2>(globalFields[fi]);
            const bool is3D = (nLevels > 1);

            // Detect whether the variable has a leading time dimension.
            // Files like MOM6 restarts use (time, [z,] nj, ni); geometry
            // cache files written by saveStructuredGrid use ([z,] nj, ni).
            int var_ndims = 0;
            nc_inq_varndims(ncid, varid, &var_ndims);
            const bool hasTimeDim = (var_ndims == (is3D ? 4 : 3));

            if (is3D) {
              if (!hasZDim || file_nz != nLevels) {
                oops::Log::warning()
                    << "readMOM6Netcdf: skipping 3D variable '" << fileVarName
                    << "' from " << filepath
                    << " (z dimension absent or size mismatch)" << std::endl;
              } else {
                std::vector<double> buf(
                    static_cast<size_t>(file_nz) * file_nj * file_ni);
                if (hasTimeDim) {
                  const size_t start[4] = {0, 0, 0, 0};
                  const size_t count[4] = {1, static_cast<size_t>(file_nz),
                                              static_cast<size_t>(file_nj),
                                              static_cast<size_t>(file_ni)};
                  nc_get_vara_double(ncid, varid, start, count, buf.data());
                } else {
                  const size_t start[3] = {0, 0, 0};
                  const size_t count[3] = {static_cast<size_t>(file_nz),
                                              static_cast<size_t>(file_nj),
                                              static_cast<size_t>(file_ni)};
                  nc_get_vara_double(ncid, varid, start, count, buf.data());
                }

                if (scale != 0.0)
                  for (auto & v : buf) v *= scale;

                for (int n = 0; n < nGlobal; ++n) {
                  const int flatIdx = static_cast<int>(gIdx(n, 0)) - 1;
                  const int iG = flatIdx % file_ni;
                  const int jG = flatIdx / file_ni;
                  for (int k = 0; k < nLevels; ++k) {
                    gView(n, k) = buf[static_cast<size_t>(k) * file_nj * file_ni
                                    + static_cast<size_t>(jG) * file_ni
                                    + static_cast<size_t>(iG)];
                  }
                }
                oops::Log::info() << "readMOM6Netcdf: " << field.name()
                                  << " <- " << fileVarName
                                  << " from " << filepath << std::endl;
              }
            } else {
              std::vector<double> buf(
                  static_cast<size_t>(file_nj) * file_ni);
              if (hasTimeDim) {
                const size_t start[3] = {0, 0, 0};
                const size_t count[3] = {1, static_cast<size_t>(file_nj),
                                            static_cast<size_t>(file_ni)};
                nc_get_vara_double(ncid, varid, start, count, buf.data());
              } else {
                const size_t start[2] = {0, 0};
                const size_t count[2] = {static_cast<size_t>(file_nj),
                                            static_cast<size_t>(file_ni)};
                nc_get_vara_double(ncid, varid, start, count, buf.data());
              }

              if (scale != 0.0)
                for (auto & v : buf) v *= scale;

              for (int n = 0; n < nGlobal; ++n) {
                const int flatIdx = static_cast<int>(gIdx(n, 0)) - 1;
                gView(n, 0) = buf[flatIdx];
              }
              oops::Log::info() << "readMOM6Netcdf: " << field.name()
                                << " <- " << fileVarName
                                << " from " << filepath << std::endl;
            }
          }
        }
        ++fi;
      }

      nc_close(ncid);
    }
  }

  // --- 4. Scatter all global fields to distributed ranks (collective). ---
  // Zero the distributed fields first. atlas::createField leaves memory
  // uninitialised, and scatter only writes nodes whose global index is present
  // in the gathered global field. With the northern-fold / periodic mesh, some
  // owned nodes are not covered by the gather/scatter round-trip; without this
  // they would retain heap garbage, which is then summed by reductions
  // (e.g. normField over exclude_halo) and makes results non-reproducible.
  for (auto & field : x) {
    auto v = atlas::array::make_view<double, 2>(field);
    v.assign(0.0);
  }

  int fi = 0;
  for (auto & field : x) {
    fs.scatter(globalFields[fi], field);
    ++fi;
  }

  // Halo exchange to fill ghost nodes from their owning ranks
  for (auto & field : x) {
    atlas::functionspace::NodeColumns(field.functionspace()).haloExchange(field);
  }
}

// ---------------------------------------------------------------------------
}  // namespace ijedi
