#include <algorithm>
#include <cmath>

#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"

#include "atlas/field.h"
#include "atlas/functionspace.h"
#include "atlas/grid.h"
#include "atlas/mesh/actions/BuildHalo.h"
#include "atlas/mesh/Mesh.h"
#include "atlas/mesh/MeshBuilder.h"
#include "atlas/output/Gmsh.h"

#include "oops/util/abor1_cpp.h"
#include "oops/util/Logger.h"

#include "ijedi/Geometry/fv3/GeometryFV3.h"
#include "ijedi/Geometry/base/AtlasMeshBuilderContract.h"
#include "ijedi/Geometry/fv3/GeometryFV3.interface.h"
#include "ijedi/Geometry/fv3/GeometryFV3Parameters.h"
#include "ijedi/Utilities/Constants.h"

namespace {

std::vector<double> fv3MidlayerPressurePhilips(const std::vector<double> & edgePressure,
                                               const double kappa) {
  const double kap1 = kappa + 1.0;
  const double kapr = 1.0 / kappa;
  std::vector<double> midPressure(edgePressure.size() - 1);
  for (size_t k = 0; k < midPressure.size(); ++k) {
    midPressure[k] = std::pow((std::pow(edgePressure[k + 1], kap1) -
                               std::pow(edgePressure[k], kap1)) /
                              (kap1 * (edgePressure[k + 1] - edgePressure[k])), kapr);
  }
  return midPressure;
}

std::vector<double> fv3LogPressureProfile(const std::vector<double> & ak,
                                          const std::vector<double> & bk,
                                          const double surfacePressure) {
  std::vector<double> edgePressure(ak.size());
  for (size_t k = 0; k < edgePressure.size(); ++k) {
    edgePressure[k] = ak[k] + bk[k] * surfacePressure;
  }

  const std::vector<double> midPressure =
      fv3MidlayerPressurePhilips(edgePressure, ijedi::getConstant("kappa"));
  std::vector<double> logPressure(midPressure.size());
  for (size_t k = 0; k < logPressure.size(); ++k) {
    logPressure[k] = -std::log(midPressure[k]);
  }
  return logPressure;
}

}  // namespace

namespace ijedi
{

  // -----------------------------------------------------------------------------------------------

  GeometryFV3::GeometryFV3(const eckit::Configuration &geomConfig, const eckit::mpi::Comm &comm,
                           eckit::LocalConfiguration &geomVariables,
                           atlas::FunctionSpace &functionSpace,
                           atlas::FieldSet &geomFields, bool &levelsAreTopDown, int &numberLevels)
  {
    oops::Log::trace() << "GeometryFV3 constructor starting" << std::endl;

    // Deserialize the parameters
    // --------------------------
    GeometryParameters params;
    params.deserialize(geomConfig);

    // Call the fms initialize, done only once
    // ---------------------------------------
    static bool initialized = false;
    if (!initialized)
    {
      f_fv3_geom_initialize((*params.fmsInit.value()).toConfiguration(), &comm);
      initialized = true;
    }

    // Call the setup routine
    f_fv3_geom_create(geomConfig, geomVariables, &comm);

    // Extract things from GeomVariables
    int ngrid;
    ngrid = geomVariables.getInt("ngrid");

    int npx = geomVariables.getInt("npx");
    int npy = geomVariables.getInt("npy");
    numberLevels = geomVariables.getInt("nLevels");
    int ntiles = geomVariables.getInt("ntiles");

    int layout_x = geomVariables.getInt("layout_x");
    int layout_y = geomVariables.getInt("layout_y");

    // Set whether levels are top-down or bottom-up
    levelsAreTopDown = true;

    std::string globalOrRegional = ntiles == 6 ? "Global" : "Regional";

    // Message:
    // Cubed sphere geometry on <global/regional> grid.
    //
    // Number of (full) model levels: <numberLevels>
    // Number of grids: <ntiles>
    // Grid dimensions: npx x npy: C<npx=1> x C<npy=1>

    // Create print message
    printMessage_ = " Cubed Sphere Geometry for " + globalOrRegional + " Grid.\n" +
                    " Number of tiles (cube faces): " + std::to_string(ntiles) + "\n" +
                    " Grid dimensions (per tile): c" + std::to_string(npx) + " x c" +
                    std::to_string(npy) + "\n" +
                    " Number of (full) model levels: " + std::to_string(numberLevels) + "\n" +
                    " Processor layout per tile: " +
                    std::to_string(layout_x) + " x " + std::to_string(layout_y);

    // Extract variables from geomVariables that were set in Fortran
    int num_nodes;
    size_t num_tri_elements;
    size_t num_quad_elements;
    geomVariables.get("num_nodes", num_nodes);
    geomVariables.get("num_tri_elements", num_tri_elements);
    geomVariables.get("num_quad_elements", num_quad_elements);

    std::vector<double> area_owned;
    std::vector<double> lons;
    std::vector<double> lats;
    std::vector<int> ghosts;
    std::vector<int> global_indices;
    std::vector<int> remote_indices;
    std::vector<int> partitions;
    std::vector<int> raw_tri_boundary_nodes;
    std::vector<int> raw_quad_boundary_nodes;

    geomVariables.get("area", area_owned);
    geomVariables.get("lons", lons);
    geomVariables.get("lats", lats);
    geomVariables.get("ghosts", ghosts);
    geomVariables.get("global_indices", global_indices);
    geomVariables.get("remote_indices", remote_indices);
    geomVariables.get("partition", partitions);
    geomVariables.get("raw_tri_boundary_nodes", raw_tri_boundary_nodes);
    geomVariables.get("raw_quad_boundary_nodes", raw_quad_boundary_nodes);

    if (area_owned.size() != static_cast<size_t>(ngrid)) {
      throw eckit::BadValue("FV3 geometry area field size does not match ngrid", Here());
    }

    // Atlas connection
    {
      const int num_elements = num_tri_elements + num_quad_elements;
      std::vector<int> num_elements_per_rank(comm.size());
      comm.allGather(num_elements, num_elements_per_rank.begin(), num_elements_per_rank.end());
      int global_element_index = 1;  // 1-based global index
      for (size_t i = 0; i < comm.rank(); ++i)
      {
        global_element_index += num_elements_per_rank[i];
      }

      using atlas::gidx_t;
      using atlas::idx_t;

      std::vector<std::array<gidx_t, 3>> tri_boundary_nodes(num_tri_elements);
      std::vector<gidx_t> tri_global_indices(num_tri_elements);
      for (size_t tri = 0; tri < num_tri_elements; ++tri)
      {
        for (size_t i = 0; i < 3; ++i)
        {
          tri_boundary_nodes[tri][i] = raw_tri_boundary_nodes[3 * tri + i];
        }
        tri_global_indices[tri] = global_element_index;
        ++global_element_index;
      }
      std::vector<std::array<gidx_t, 4>> quad_boundary_nodes(num_quad_elements);
      std::vector<gidx_t> quad_global_indices(num_quad_elements);
      for (size_t quad = 0; quad < num_quad_elements; ++quad)
      {
        for (size_t i = 0; i < 4; ++i)
        {
          quad_boundary_nodes[quad][i] = raw_quad_boundary_nodes[4 * quad + i];
        }
        quad_global_indices[quad] = global_element_index;
        ++global_element_index;
      }

      std::vector<atlas::gidx_t> atlas_global_indices(num_nodes);
      std::transform(global_indices.begin(), global_indices.end(), atlas_global_indices.begin(),
                     [](const int index)
                     { return atlas::gidx_t{index}; });

      const atlas::idx_t remote_index_base = 1;  // 1-based indexing from Fortran
      std::vector<atlas::idx_t> atlas_remote_indices(num_nodes);
      std::transform(remote_indices.begin(), remote_indices.end(), atlas_remote_indices.begin(),
                     [](const int index)
                     { return atlas::idx_t{index}; });

      eckit::LocalConfiguration atlas_config{};
      atlas_config.set("mpi_comm", comm.name());

      // establish connectivity
      const atlas::mesh::MeshBuilder mesh_builder{};
      atlas::Mesh mesh = mesh_builder(
          lons,
          lats,
          ghosts,
          atlas_global_indices,
          atlas_remote_indices,
          remote_index_base,
          partitions,
          tri_boundary_nodes,
          tri_global_indices,
          quad_boundary_nodes,
          quad_global_indices,
          atlas_config);

      bindMeshBuilderOwnership(mesh);
      atlas::mesh::actions::build_halo(mesh, 1);
      functionSpace = atlas::functionspace::NodeColumns(mesh, atlas_config);

      // Optionally write atlas mesh for viewing with gmsh
      if (params.writeGmsh)
      {
        const std::string filename = params.writeGmshFilename;
        eckit::LocalConfiguration gmsh_config{};
        gmsh_config.set("coordinates", "xyz");
        gmsh_config.set("ghost", true);  // enables viewing halos per task
        atlas::output::Gmsh gmsh(filename, gmsh_config);
        gmsh.write(mesh);
      }
    }

    // Atlas fields for geometry variables
    geomFields = atlas::FieldSet();

    // Create fields needed in geomFields
    atlas::Field area = functionSpace.createField<double>(atlas::option::name("area") |
                                                          atlas::option::levels(1));
    atlas::Field owned = functionSpace.createField<int>(atlas::option::name("owned") |
                                                        atlas::option::levels(1));

    // Start with area being -1 everywhere
    auto areaView = atlas::array::make_view<double, 2>(area);
    auto ownedView = atlas::array::make_view<int, 2>(owned);

    // 1. initialize all local entries, including halo, to -1
    for (atlas::idx_t j = 0; j < functionSpace.size(); ++j)
    {
      areaView(j, 0) = -1.0;
      ownedView(j, 0) = 0;
    }
    // 2. overwrite owned points with your data
    for (atlas::idx_t j = 0; j < ngrid; ++j)
    {
      areaView(j, 0) = area_owned[j];
      ownedView(j, 0) = 1;
    }

    // Add area to geomFields
    geomFields.add(area);
    geomFields.add(owned);

    std::vector<double> ak;
    std::vector<double> bk;
    geomVariables.get("sigma_pressure_hybrid_coordinate_a_coefficient", ak);
    geomVariables.get("sigma_pressure_hybrid_coordinate_b_coefficient", bk);

    std::vector<double> surfacePressure;
    std::vector<double> surfaceGeopotential;
    geomVariables.get("surface_pressure", surfacePressure);
    geomVariables.get("surface_geopotential", surfaceGeopotential);

    if (functionSpace.size() < ngrid) {
      throw eckit::BadValue("FV3 functionSpace size is smaller than ngrid", Here());
    }
    if (ak.size() != static_cast<size_t>(numberLevels + 1) ||
        bk.size() != static_cast<size_t>(numberLevels + 1)) {
      throw eckit::BadValue("FV3 ak/bk sizes do not match nLevels + 1", Here());
    }
    if (surfacePressure.size() != static_cast<size_t>(ngrid)) {
      throw eckit::BadValue("FV3 surface_pressure size does not match ngrid", Here());
    }
    if (surfaceGeopotential.size() != static_cast<size_t>(ngrid)) {
      throw eckit::BadValue("FV3 surface_geopotential size does not match ngrid", Here());
    }

    // Reference pressure column for vertical localization, using FV3's own Philips
    // mid-layer convention at psurf = 1e5 to match fv3-jedi's verticalCoord("logp").
    // A generic arithmetic mid-layer instead perturbs GETKF increments by ~0.2%.
    {
      constexpr double referenceSurfacePressure = 1.0e5;
      std::vector<double> edgePressure(ak.size());
      for (size_t k = 0; k < edgePressure.size(); ++k) {
        edgePressure[k] = ak[k] + bk[k] * referenceSurfacePressure;
      }
      geomVariables.set("vertical_coordinate_reference_pressure",
                        fv3MidlayerPressurePhilips(edgePressure, getConstant("kappa")));
    }

    const std::string vertCoordType = params.vertCoord;
    if (vertCoordType != "sigma" && vertCoordType != "logp" && vertCoordType != "orography") {
      throw eckit::BadValue("Unsupported FV3 vertical coordinate type for vert_coord: "
                            + vertCoordType, Here());
    }

    const int vertCoordLevels = vertCoordType == "orography" ? 1 : numberLevels;
    atlas::Field vertCoord = functionSpace.createField<double>(
        atlas::option::name("vert_coord") | atlas::option::levels(vertCoordLevels));
    auto vertCoordView = atlas::array::make_view<double, 2>(vertCoord);
    for (atlas::idx_t j = 0; j < functionSpace.size(); ++j) {
      for (atlas::idx_t k = 0; k < vertCoord.shape(1); ++k) {
        vertCoordView(j, k) = -1.0;
      }
    }

    if (vertCoordType == "sigma") {
      for (atlas::idx_t j = 0; j < ngrid; ++j) {
        const double psLocal = surfacePressure[j];
        for (int k = 0; k < numberLevels; ++k) {
          const double sigmaUp = ak[k + 1] / psLocal + bk[k + 1];
          const double sigmaDn = ak[k] / psLocal + bk[k];
          vertCoordView(j, k) = 0.5 * (sigmaUp + sigmaDn);
        }
      }
    } else if (vertCoordType == "logp") {
      for (atlas::idx_t j = 0; j < ngrid; ++j) {
        const std::vector<double> logPressure =
            fv3LogPressureProfile(ak, bk, surfacePressure[j]);
        for (int k = 0; k < numberLevels; ++k) {
          vertCoordView(j, k) = logPressure[k];
        }
      }
    } else if (vertCoordType == "orography") {
      const double grav = getConstant("grav");
      for (atlas::idx_t j = 0; j < ngrid; ++j) {
        vertCoordView(j, 0) = surfaceGeopotential[j] / grav;
      }
    }
    geomFields.add(vertCoord);

    oops::Log::trace() << "GeometryFV3 constructor done" << std::endl;
  }

  // -----------------------------------------------------------------------------------------------

  void GeometryFV3::print(std::ostream &os) const
  {
    os << printMessage_ << std::endl;
  }

  // -----------------------------------------------------------------------------------------------

  std::vector<double> GeometryFV3::verticalCoord(std::string &vcUnits) const
  {
    // Not implemented, abort --- IGNORE ---
    std::stringstream errorMsg;
    errorMsg << "GeometryFV3::verticalCoord is not implemented" << std::endl;
    ABORT(errorMsg.str());
    return std::vector<double>();  // Never reached, but silences compiler warning
  }

  // -----------------------------------------------------------------------------------------------

}  // namespace ijedi
