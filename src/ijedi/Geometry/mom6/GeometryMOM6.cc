#include <netcdf.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"

#include "atlas/array.h"
#include "atlas/functionspace/NodeColumns.h"
#include "atlas/grid/Distribution.h"
#include "atlas/grid/Partitioner.h"
#include "atlas/grid/UnstructuredGrid.h"
#include "atlas/mesh.h"
#include "atlas/mesh/MeshBuilder.h"
#include "atlas/mesh/actions/BuildHalo.h"
#include "atlas/output/Gmsh.h"
#include "atlas/util/Earth.h"
#include "atlas/util/KDTree.h"
#include "atlas/util/Metadata.h"

#include "oops/util/FieldSetHelpers.h"
#include "oops/util/Logger.h"
#include "oops/util/Timer.h"
#include "oops/util/abor1_cpp.h"

#include "ijedi/Geometry/mom6/GeometryMOM6.h"
#include "ijedi/Geometry/base/AtlasMeshBuilderContract.h"
#include "ijedi/Geometry/mom6/GeometryMOM6Utils.h"

// ---------------------------------------------------------------------------
namespace ijedi
{
// ---------------------------------------------------------------------------
// Build Atlas NodeColumns for the local MOM6 compute domain.
// Global arrays are passed in; local slice is extracted here.
void GeometryMOM6::buildMom6FunctionSpace(const eckit::mpi::Comm & comm,
                                          const std::vector<double> & lonGlobal,
                                          const std::vector<double> & latGlobal)
{
  using atlas::gidx_t;
  using atlas::idx_t;

  const int rank   = static_cast<int>(comm.rank());
  const int nOwned = iCount_ * jCount_;

  std::vector<double>  lons(nOwned), lats(nOwned);
  std::vector<int>     ghosts(nOwned, 0), partitions(nOwned, rank);
  std::vector<gidx_t>  globalIdx(nOwned);
  std::vector<idx_t>   remoteIdx(nOwned);

  for (int jL = 0; jL < jCount_; ++jL) {
    for (int iL = 0; iL < iCount_; ++iL) {
      const int n  = jL * iCount_ + iL;
      const int iG = iStart_ - 1 + iL;
      const int jG = jStart_ - 1 + jL;
      lons[n]      = lonGlobal[jG * niEff_ + iG];
      lats[n]      = latGlobal[jG * niEff_ + iG];
      globalIdx[n] = static_cast<gidx_t>(jG * niEff_ + iG + 1);  // 1-based
      remoteIdx[n] = static_cast<idx_t>(n + 1);                      // 1-based
    }
  }

  eckit::LocalConfiguration meshConf;
  meshConf.set("mpi_comm", comm.name());

  const atlas::mesh::MeshBuilder meshBuilder;
  atlas::Mesh mesh = meshBuilder(
      lons, lats, ghosts,
      globalIdx, remoteIdx, /*remote_index_base=*/1, partitions,
      /*tri_boundary_nodes=*/{}, /*tri_global_indices=*/{},
      /*quad_boundary_nodes=*/{}, /*quad_global_indices=*/{},
      meshConf);

  bindMeshBuilderOwnership(mesh);
  mom6FunctionSpace_ = atlas::functionspace::NodeColumns(
      mesh, atlas::util::Config("mpi_comm", comm.name()));
}

// ---------------------------------------------------------------------------
// Build Atlas NodeColumns for the JEDI unstructured partition.
//
// Algorithm:
//   1. All ranks build the same global active-point list (ocean + configurable
//      land fringe) — no MPI needed, deterministic from the global mask.
//   2. Atlas "equal_regions" partitioner assigns each active point to a
//      rank based on its lon/lat, giving geographically compact ownership.
//   3. Ghost nodes: active-point neighbours of owned nodes that belong to
//      other JEDI ranks.
//   4. jediPoints_ records (iG, jG) for each node (owned first, then ghost).
//   5. Atlas MeshBuilder → NodeColumns.
//
// Periodicity: i wraps (east-west); j is clamped (no tripolar fold).
void GeometryMOM6::buildJediFunctionSpace(const eckit::mpi::Comm & comm,
                                          const std::vector<double> & wetGlobal,
                                          const std::vector<double> & lonGlobal,
                                          const std::vector<double> & latGlobal)
{
  using atlas::gidx_t;
  using atlas::idx_t;

  // Periodic ocean test — MOM6's wet mask (i wraps, j clamped)
  auto isOcean = [&](int i, int j) -> bool {
    i = (i % niEff_ + niEff_) % niEff_;
    if (j < 0 || j >= njEff_) return false;
    return wetGlobal[j * niEff_ + i] > 0.5;
  };

  // --- Step 1: global active-point list (row-major scan order) ---
  struct APoint { int iG, jG; };
  std::vector<APoint> active;
  active.reserve(niEff_ * njEff_);

  const int nCells = niEff_ * njEff_;
  std::vector<int> distToOcean(nCells, -1);
  std::vector<int> frontier;
  frontier.reserve(nCells);

  for (int jG = 0; jG < njEff_; ++jG) {
    for (int iG = 0; iG < niEff_; ++iG) {
      if (!isOcean(iG, jG)) continue;
      const int idx = jG * niEff_ + iG;
      distToOcean[idx] = 0;
      frontier.push_back(idx);
    }
  }

  for (size_t head = 0; head < frontier.size(); ++head) {
    const int idx = frontier[head];
    const int dist = distToOcean[idx];
    if (dist >= fringeWidth_) continue;

    const int iG = idx % niEff_;
    const int jG = idx / niEff_;
    const int nbI[4] = { (iG - 1 + niEff_) % niEff_,
                         (iG + 1) % niEff_, iG, iG };
    const int nbJ[4] = { jG, jG, jG - 1, jG + 1 };
    for (int d = 0; d < 4; ++d) {
      if (nbJ[d] < 0 || nbJ[d] >= njEff_) continue;
      const int nbIdx = nbJ[d] * niEff_ + nbI[d];
      if (distToOcean[nbIdx] != -1) continue;
      distToOcean[nbIdx] = dist + 1;
      frontier.push_back(nbIdx);
    }
  }

  for (int jG = 0; jG < njEff_; ++jG) {
    for (int iG = 0; iG < niEff_; ++iG) {
      const int idx = jG * niEff_ + iG;
      if (distToOcean[idx] >= 0 && distToOcean[idx] <= fringeWidth_)
        active.push_back({iG, jG});
    }
  }
  nActiveGlobal_ = static_cast<int>(active.size());

  // --- Step 2: Atlas geographic partition ---
  // Build an UnstructuredGrid from the active-point coordinates and let
  // the "equal_regions" partitioner assign geographically compact ownership.
  std::vector<atlas::PointXY> pts(nActiveGlobal_);
  const int npes = static_cast<int>(comm.size());
  const int rank = static_cast<int>(comm.rank());
  std::vector<int> partOf(nActiveGlobal_);
  for (int k = 0; k < nActiveGlobal_; ++k) {
    const int gIdx = active[k].jG * niEff_ + active[k].iG;
    pts[k] = atlas::PointXY(lonGlobal[gIdx], latGlobal[gIdx]);
  }
  const atlas::UnstructuredGrid ugrid(pts);
  const atlas::grid::Distribution dist =
      atlas::grid::Partitioner("equal_regions", npes).partition(ugrid);

  // partOf[k] = JEDI rank owning active point k
  for (int k = 0; k < nActiveGlobal_; ++k)
    partOf[k] = dist.partition(k);

  ownedCount_ = static_cast<int>(dist.nb_pts()[rank]);
  ownedMin_   = static_cast<int>(dist.min_pts());
  ownedMax_   = static_cast<int>(dist.max_pts());

  // Fast lookup: flat global index (jG*NI+iG) → index in active[]
  std::unordered_map<int, int> activeMap;
  activeMap.reserve(nActiveGlobal_);
  for (int k = 0; k < nActiveGlobal_; ++k)
    activeMap[active[k].jG * niEff_ + active[k].iG] = k;

  // Atlas gather/scatter addresses global fields by global_index-1. The
  // masked active space therefore needs dense IDs, distinct from file indices.
  const auto atlasNodeId = [&](int iG, int jG) -> atlas::gidx_t {
    return static_cast<atlas::gidx_t>(activeMap.at(jG * niEff_ + iG) + 1);
  };

  // --- Step 3: identify ghost nodes ---
  // Collect owned points, then scan their neighbours for cross-rank
  // active points.
  std::vector<int> ghostIdxVec;
  // active-list index → position in ghostIdxVec
  std::unordered_map<int, int> ghostSeen;

  auto addGhostActiveIndex = [&](int candidateK) {
    if (candidateK < 0 || candidateK >= nActiveGlobal_) return;
    if (partOf[candidateK] == rank) return;
    const int gpos = static_cast<int>(ghostIdxVec.size());
    if (ghostSeen.emplace(candidateK, gpos).second)
      ghostIdxVec.push_back(candidateK);
  };

  for (int k = 0; k < nActiveGlobal_; ++k) {
    if (partOf[k] != rank) continue;
    const int iG = active[k].iG;
    const int jG = active[k].jG;
    const int nbI[8] = { (iG - 1 + niEff_) % niEff_,
                         (iG + 1) % niEff_, iG, iG,
                         (iG - 1 + niEff_) % niEff_, (iG + 1) % niEff_,
                         (iG - 1 + niEff_) % niEff_, (iG + 1) % niEff_ };
    const int nbJ[8] = { jG, jG, jG - 1, jG + 1,
                         jG - 1, jG - 1,
                         jG + 1, jG + 1 };
    for (int d = 0; d < 8; ++d) {
      if (nbJ[d] < 0 || nbJ[d] >= njEff_) continue;
      auto it = activeMap.find(nbJ[d] * niEff_ + nbI[d]);
      if (it == activeMap.end()) continue;
      addGhostActiveIndex(it->second);
    }

    // Tripolar fold connectivity: northern seam quads connect to reflected
    // points on the same row that are not geometric neighbours in (i, j).
    if (hasFold_ && jG == njEff_ - 1) {
      const int iG1 = iG + 1;
      if (2 * (iG + 1) < niEff_) {
        const int foldNE = niEff_ - 2 - iG;
        const int foldNW = niEff_ - 1 - iG;
        auto itNE = activeMap.find(jG * niEff_ + foldNE);
        if (itNE != activeMap.end()) addGhostActiveIndex(itNE->second);
        auto itNW = activeMap.find(jG * niEff_ + foldNW);
        if (itNW != activeMap.end()) addGhostActiveIndex(itNW->second);
      }
      if (iG1 < niEff_ && 2 * iG1 < niEff_) {
        const int foldOfI1 = niEff_ - 1 - iG1;
        auto itFold = activeMap.find(jG * niEff_ + foldOfI1);
        if (itFold != activeMap.end()) addGhostActiveIndex(itFold->second);
      }
    }
  }

  // --- Step 4: populate jediPoints_ (owned first, then ghost) ---
  // Also compute each active point's local index on its owner rank.
  // All ranks build the same active/partOf arrays, so this is deterministic
  // with no extra MPI — needed to set remoteIdx correctly for ghost nodes.
  std::vector<int> ownerLocalIdx(nActiveGlobal_);
  {
    std::vector<int> ownerCount(npes, 0);
    for (int k = 0; k < nActiveGlobal_; ++k)
      ownerLocalIdx[k] = ownerCount[partOf[k]]++;
  }

  const int nGhost = static_cast<int>(ghostIdxVec.size());
  const int nTotal = ownedCount_ + nGhost;
  jediPoints_.clear();
  jediPoints_.reserve(nTotal);
  for (int k = 0; k < nActiveGlobal_; ++k)
    if (partOf[k] == rank)
      jediPoints_.push_back({active[k].iG, active[k].jG});
  for (int gk : ghostIdxVec)
    jediPoints_.push_back({active[gk].iG, active[gk].jG});

  // --- Step 4.5: generate quad cells for owned ocean points ---
  // SABER Diffusion (NodeColumns path) requires mesh cells so that
  // atlas::mesh::actions::build_edges() can produce the edge list used by the
  // Laplacian stencil.  Each quad covers four corners that are all ocean-active;
  // land points are excluded.  This is the same requirement as any other
  // NodeColumns user of SABER Diffusion (e.g. the cubed-sphere CS-LFR-12 grid).
  std::vector<std::array<atlas::gidx_t, 4>> quadNodes;
  std::vector<atlas::gidx_t>                quadGidx;
  for (int n = 0; n < ownedCount_; ++n) {
    const int iG  = jediPoints_[n].first;
    const int jG  = jediPoints_[n].second;
    if (jG + 1 >= njEff_) continue;                    // no northern neighbour
    const int iG1 = (iG + 1) % niEff_;                 // periodic in i
    const int jG1 = jG + 1;
    if (activeMap.find(jG  * niEff_ + iG ) == activeMap.end()) continue;
    if (activeMap.find(jG  * niEff_ + iG1) == activeMap.end()) continue;
    if (activeMap.find(jG1 * niEff_ + iG1) == activeMap.end()) continue;
    if (activeMap.find(jG1 * niEff_ + iG ) == activeMap.end()) continue;
    // SW, SE, NE, NW corners — global node indices (1-based)
    quadNodes.push_back({
        atlasNodeId(iG, jG), atlasNodeId(iG1, jG),
        atlasNodeId(iG1, jG1), atlasNodeId(iG, jG1)});
    quadGidx.push_back(0);   // placeholder; filled after allGather below
  }
  // --- Step 4.6: fold quads along the tripolar northern seam ---
  // For a tripolar grid the "virtual" row at j=njEff_ folds back onto the last
  // real row via the reflection:  ghost(i, njEff_) = real(niEff_-1-i, njEff_-1)
  // A fold quad centred at column iG on the last row has corners (all on jG):
  //   SW=(iG, jG)  SE=(iG+1, jG)  NE=(niEff_-2-iG, jG)  NW=(niEff_-1-iG, jG)
  // Only the left half (2*(iG+1) < niEff_) yields unique quads; the seam
  // column where NE==SE is degenerate and deliberately skipped.
  if (hasFold_) {
    const int jG = njEff_ - 1;
    for (int n = 0; n < ownedCount_; ++n) {
      if (jediPoints_[n].second != jG) continue;        // only last row
      const int iG    = jediPoints_[n].first;
      if (2 * (iG + 1) >= niEff_) continue;             // skip seam + right half
      const int iG1    = iG + 1;
      const int foldNE = niEff_ - 2 - iG;              // NE: reflected SE
      const int foldNW = niEff_ - 1 - iG;              // NW: reflected SW
      if (foldNE < 0) continue;                         // guard for tiny grids
      if (activeMap.find(jG * niEff_ + iG)    == activeMap.end()) continue;
      if (activeMap.find(jG * niEff_ + iG1)   == activeMap.end()) continue;
      if (activeMap.find(jG * niEff_ + foldNE) == activeMap.end()) continue;
      if (activeMap.find(jG * niEff_ + foldNW) == activeMap.end()) continue;
      quadNodes.push_back({
          atlasNodeId(iG, jG), atlasNodeId(iG1, jG),
          atlasNodeId(foldNE, jG), atlasNodeId(foldNW, jG)});
      quadGidx.push_back(0);  // filled by allGather offset below
    }
  }
  // Compute globally unique quad element indices via per-rank allGather offset
  {
    const int nQ = static_cast<int>(quadNodes.size());
    std::vector<int> nPerRank(comm.size());
    comm.allGather(nQ, nPerRank.begin(), nPerRank.end());
    int offset = 0;
    for (int r = 0; r < rank; ++r) offset += nPerRank[r];
    for (int q = 0; q < nQ; ++q)
      quadGidx[q] = static_cast<atlas::gidx_t>(offset + q + 1);  // 1-based
  }

  // --- Step 5: build Atlas mesh ---
  std::vector<double>  lons(nTotal), lats(nTotal);
  std::vector<int>     ghosts(nTotal, 0), partitions(nTotal, rank);
  std::vector<gidx_t>  globalIdx(nTotal);
  std::vector<idx_t>   remoteIdx(nTotal);

  for (int n = 0; n < ownedCount_; ++n) {
    const int iG = jediPoints_[n].first;
    const int jG = jediPoints_[n].second;
    lons[n]      = lonGlobal[jG * niEff_ + iG];
    lats[n]      = latGlobal[jG * niEff_ + iG];
    globalIdx[n] = atlasNodeId(iG, jG);  // dense active-space ID, 1-based
    remoteIdx[n] = static_cast<idx_t>(n + 1);                   // 1-based local
  }
  for (int g = 0; g < nGhost; ++g) {
    const int n  = ownedCount_ + g;
    const int gk = ghostIdxVec[g];
    const int iG = jediPoints_[n].first;
    const int jG = jediPoints_[n].second;
    lons[n]      = lonGlobal[jG * niEff_ + iG];
    lats[n]      = latGlobal[jG * niEff_ + iG];
    globalIdx[n] = atlasNodeId(iG, jG);  // dense active-space ID, 1-based
    remoteIdx[n] = static_cast<idx_t>(ownerLocalIdx[gk] + 1);  // 1-based on owner
    ghosts[n]     = 1;
    partitions[n] = partOf[gk];
  }

  eckit::LocalConfiguration meshConf;
  meshConf.set("mpi_comm", comm.name());

  const atlas::mesh::MeshBuilder meshBuilder;
  atlas::Mesh mesh = meshBuilder(
      lons, lats, ghosts,
      globalIdx, remoteIdx, /*remote_index_base=*/1, partitions,
      /*tri_boundary_nodes=*/{}, /*tri_global_indices=*/{},
      quadNodes, quadGidx,
      meshConf);
  bindMeshBuilderOwnership(mesh);
  atlas::mesh::actions::build_halo(mesh, 1);

  functionSpace_ = atlas::functionspace::NodeColumns(
      mesh, atlas::util::Config("mpi_comm", comm.name()));

  // Preserve the physical structured address separately, including every halo
  // node. Never reinterpret a dense Atlas ID as an index into a restart file.
  atlas::Field structuredIndex("mom6_structured_index",
      atlas::array::make_datatype<atlas::gidx_t>(),
      atlas::array::make_shape(mesh.nodes().size()));
  auto structured = atlas::array::make_view<atlas::gidx_t, 1>(structuredIndex);
  const auto dense = atlas::array::make_view<atlas::gidx_t, 1>(mesh.nodes().global_index());
  for (atlas::idx_t node = 0; node < mesh.nodes().size(); ++node) {
    if (dense(node) <= 0 || dense(node) > nActiveGlobal_) {
      throw eckit::BadValue("MOM6 Atlas node has an invalid active-space ID", Here());
    }
    const auto &point = active[dense(node) - 1];
    structured(node) = point.jG * niEff_ + point.iG + 1;
  }
  mesh.nodes().add(structuredIndex);

  oops::Log::info() << "GeometryMOM6 JEDI space: rank " << rank
                    << " nActiveGlobal=" << nActiveGlobal_
                    << " ownedCount="    << ownedCount_
                    << " ghostCount="    << nGhost << std::endl;
}

// ---------------------------------------------------------------------------
// Populate mom6Fields_ on mom6FunctionSpace_ with geometry data for the
// local MOM6 compute domain.  All input arrays are global (size
// njGlobal*niGlobal, row-major); local slice is extracted here.
void GeometryMOM6::buildMom6Fields(const std::vector<double> & lonGlobal,
                                    const std::vector<double> & latGlobal,
                                    const std::vector<double> & depthGlobal,
                                    const std::vector<double> & wetGlobal,
                                    const std::vector<double> & layerThicknessGlobal,
                                    const std::vector<double> & layerCenterDepthGlobal,
                                    const std::vector<double> & mask3dGlobal,
                                    const std::vector<double> & dxTGlobal,
                                    const std::vector<double> & dyTGlobal,
                                    const std::vector<double> & areaTGlobal,
                                    const std::vector<double> & lonUGlobal,
                                    const std::vector<double> & latUGlobal,
                                    const std::vector<double> & lonVGlobal,
                                    const std::vector<double> & latVGlobal)
{
  const int npts = iCount_ * jCount_;
  mom6Fields_ = atlas::FieldSet();
  const size_t nHoriz = static_cast<size_t>(njEff_) * niEff_;

  auto addField = [&](const std::string & name, int levels = 1) -> atlas::Field {
    atlas::Field f = mom6FunctionSpace_.createField<double>(
        atlas::option::name(name) | atlas::option::levels(levels));
    mom6Fields_.add(f);
    return f;
  };

  atlas::Field fOwned = mom6FunctionSpace_.createField<int>(
      atlas::option::name("owned") | atlas::option::levels(1));
  auto vOwned = atlas::array::make_view<int, 2>(fOwned);
  for (int n = 0; n < npts; ++n) vOwned(n, 0) = 1;
  mom6Fields_.add(fOwned);

  atlas::Field fLon   = addField("lon");
  atlas::Field fLat   = addField("lat");
  atlas::Field fMask  = addField("mask2d");
  atlas::Field fDepth = addField("depth");
  atlas::Field fDxT   = addField("dxT");
  atlas::Field fDyT   = addField("dyT");
  atlas::Field fAreaT = addField("areaT");
  atlas::Field fLayerThickness = addField("sea_water_cell_thickness", numLevels_);
  atlas::Field fLayerCenterDepth = addField("sea_water_depth", numLevels_);
  atlas::Field fVertCoord = addField("vert_coord", numLevels_);
  atlas::Field fMask3d = addField("mask3d", numLevels_);
  atlas::Field fLonU  = addField("lonu");
  atlas::Field fLatU  = addField("latu");
  atlas::Field fLonV  = addField("lonv");
  atlas::Field fLatV  = addField("latv");

  auto vLon   = atlas::array::make_view<double, 2>(fLon);
  auto vLat   = atlas::array::make_view<double, 2>(fLat);
  auto vMask  = atlas::array::make_view<double, 2>(fMask);
  auto vDepth = atlas::array::make_view<double, 2>(fDepth);
  auto vDxT   = atlas::array::make_view<double, 2>(fDxT);
  auto vDyT   = atlas::array::make_view<double, 2>(fDyT);
  auto vAreaT = atlas::array::make_view<double, 2>(fAreaT);
  auto vLayerThickness = atlas::array::make_view<double, 2>(fLayerThickness);
  auto vLayerCenterDepth = atlas::array::make_view<double, 2>(fLayerCenterDepth);
  auto vVertCoord = atlas::array::make_view<double, 2>(fVertCoord);
  auto vMask3d = atlas::array::make_view<double, 2>(fMask3d);
  auto vLonU  = atlas::array::make_view<double, 2>(fLonU);
  auto vLatU  = atlas::array::make_view<double, 2>(fLatU);
  auto vLonV  = atlas::array::make_view<double, 2>(fLonV);
  auto vLatV  = atlas::array::make_view<double, 2>(fLatV);

  for (int jL = 0; jL < jCount_; ++jL) {
    for (int iL = 0; iL < iCount_; ++iL) {
      const int n    = jL * iCount_ + iL;
      const int gIdx = (jStart_ - 1 + jL) * niEff_ + (iStart_ - 1 + iL);
      vLon(n, 0)   = lonGlobal[gIdx];
      vLat(n, 0)   = latGlobal[gIdx];
      vDepth(n, 0) = depthGlobal[gIdx];
      vMask(n, 0)  = wetGlobal[gIdx];
      vDxT(n, 0)   = dxTGlobal[gIdx];
      vDyT(n, 0)   = dyTGlobal[gIdx];
      vAreaT(n, 0) = areaTGlobal[gIdx];
      for (int k = 0; k < numLevels_; ++k) {
        const size_t idx3D = static_cast<size_t>(k) * nHoriz + gIdx;
        vLayerThickness(n, k) = layerThicknessGlobal[idx3D];
        vLayerCenterDepth(n, k) = layerCenterDepthGlobal[idx3D];
        vVertCoord(n, k) = layerCenterDepthGlobal[idx3D];
        vMask3d(n, k) = mask3dGlobal[idx3D];
      }
      vLonU(n, 0)  = lonUGlobal[gIdx];
      vLatU(n, 0)  = latUGlobal[gIdx];
      vLonV(n, 0)  = lonVGlobal[gIdx];
      vLatV(n, 0)  = latVGlobal[gIdx];
    }
  }
}

// ---------------------------------------------------------------------------
// Populate fields_ on the JEDI unstructured functionSpace_ from global arrays.
// Node ordering follows jediPoints_ (owned nodes first, then ghost nodes).
void GeometryMOM6::buildFields(const std::vector<double> & lonGlobal,
                                const std::vector<double> & latGlobal,
                                const std::vector<double> & depthGlobal,
                                const std::vector<double> & wetGlobal,
                                const std::vector<double> & layerThicknessGlobal,
                                const std::vector<double> & layerCenterDepthGlobal,
                                const std::vector<double> & mask3dGlobal,
                                const std::vector<double> & dxTGlobal,
                                const std::vector<double> & dyTGlobal,
                                const std::vector<double> & areaTGlobal,
                                const std::vector<double> & lonUGlobal,
                                const std::vector<double> & latUGlobal,
                                const std::vector<double> & lonVGlobal,
                                const std::vector<double> & latVGlobal,
                                bool buildVerticalGeometry)
{
  const int npts = static_cast<int>(jediPoints_.size());  // owned + ghost
  fields_ = atlas::FieldSet();
  const size_t nHoriz = static_cast<size_t>(njEff_) * niEff_;

  auto addField = [&](const std::string & name, int levels = 1) -> atlas::Field {
    atlas::Field f = functionSpace_.createField<double>(
        atlas::option::name(name) | atlas::option::levels(levels));
    fields_.add(f);
    return f;
  };

  // "owned": 1 for owned nodes, 0 for ghost nodes. The functionspace spans
  // more nodes than jediPoints_ (build_halo appends periodic / northern-fold
  // halo nodes), so initialise the whole field to 0 first - otherwise those
  // extra halo nodes carry uninitialised "owned" flags, which is both a
  // correctness hazard and a source of non-reproducible reductions.
  atlas::Field fOwned = functionSpace_.createField<int>(
      atlas::option::name("owned") | atlas::option::levels(1));
  auto vOwned = atlas::array::make_view<int, 2>(fOwned);
  const int nOwnedField = static_cast<int>(fOwned.shape(0));
  for (int n = 0; n < nOwnedField; ++n)
    vOwned(n, 0) = (n < ownedCount_) ? 1 : 0;
  fields_.add(fOwned);

  atlas::Field fLon   = addField("lon");
  atlas::Field fLat   = addField("lat");
  atlas::Field fMask  = addField("mask2d");
  atlas::Field fDepth = addField("depth");
  atlas::Field fDxT   = addField("dxT");
  atlas::Field fDyT   = addField("dyT");
  atlas::Field fArea  = addField("area");   // called "area" for SABER diffusion
  atlas::Field fLonU  = addField("lonu");
  atlas::Field fLatU  = addField("latu");
  atlas::Field fLonV  = addField("lonv");
  atlas::Field fLatV  = addField("latv");
  atlas::Field fAreaT = addField("areaT");  // MOM6-native alias for area

  atlas::Field fLayerThickness;
  atlas::Field fLayerCenterDepth;
  atlas::Field fVertCoord;
  // mask3d is always registered: the masked-cell flood fill depends on it, and
  // mask3dGlobal is computed whether or not vertical geometry is read (without
  // it, the surface wet mask is simply extruded to all levels).
  atlas::Field fMask3d = addField("mask3d", numLevels_);
  if (buildVerticalGeometry) {
    fLayerThickness = addField("sea_water_cell_thickness", numLevels_);
    fLayerCenterDepth = addField("sea_water_depth", numLevels_);
    fVertCoord = addField("vert_coord", numLevels_);
  }

  auto vLon   = atlas::array::make_view<double, 2>(fLon);
  auto vLat   = atlas::array::make_view<double, 2>(fLat);
  auto vMask  = atlas::array::make_view<double, 2>(fMask);
  auto vDepth = atlas::array::make_view<double, 2>(fDepth);
  auto vDxT   = atlas::array::make_view<double, 2>(fDxT);
  auto vDyT   = atlas::array::make_view<double, 2>(fDyT);
  auto vArea  = atlas::array::make_view<double, 2>(fArea);
  auto vLonU  = atlas::array::make_view<double, 2>(fLonU);
  auto vLatU  = atlas::array::make_view<double, 2>(fLatU);
  auto vLonV  = atlas::array::make_view<double, 2>(fLonV);
  auto vLatV  = atlas::array::make_view<double, 2>(fLatV);
  auto vAreaT = atlas::array::make_view<double, 2>(fAreaT);

  if (buildVerticalGeometry) {
    auto vLayerThickness = atlas::array::make_view<double, 2>(fLayerThickness);
    auto vLayerCenterDepth = atlas::array::make_view<double, 2>(fLayerCenterDepth);
    auto vVertCoord = atlas::array::make_view<double, 2>(fVertCoord);
    for (int n = 0; n < npts; ++n) {
      const int iG   = jediPoints_[n].first;
      const int jG   = jediPoints_[n].second;
      const int gIdx = jG * niEff_ + iG;
      vLon(n, 0)   = lonGlobal[gIdx];
      vLat(n, 0)   = latGlobal[gIdx];
      vDepth(n, 0) = depthGlobal[gIdx];
      vMask(n, 0)  = wetGlobal[gIdx];
      vDxT(n, 0)   = dxTGlobal[gIdx];
      vDyT(n, 0)   = dyTGlobal[gIdx];
      vArea(n, 0)  = areaTGlobal[gIdx];
      for (int k = 0; k < numLevels_; ++k) {
        const size_t idx3D = static_cast<size_t>(k) * nHoriz + gIdx;
        vLayerThickness(n, k) = layerThicknessGlobal[idx3D];
        vLayerCenterDepth(n, k) = layerCenterDepthGlobal[idx3D];
        vVertCoord(n, k) = layerCenterDepthGlobal[idx3D];
      }
      vAreaT(n, 0) = areaTGlobal[gIdx];
      vLonU(n, 0)  = lonUGlobal[gIdx];
      vLatU(n, 0)  = latUGlobal[gIdx];
      vLonV(n, 0)  = lonVGlobal[gIdx];
      vLatV(n, 0)  = latVGlobal[gIdx];
    }
  } else {
    for (int n = 0; n < npts; ++n) {
      const int iG   = jediPoints_[n].first;
      const int jG   = jediPoints_[n].second;
      const int gIdx = jG * niEff_ + iG;
      vLon(n, 0)   = lonGlobal[gIdx];
      vLat(n, 0)   = latGlobal[gIdx];
      vDepth(n, 0) = depthGlobal[gIdx];
      vMask(n, 0)  = wetGlobal[gIdx];
      vDxT(n, 0)   = dxTGlobal[gIdx];
      vDyT(n, 0)   = dyTGlobal[gIdx];
      vArea(n, 0)  = areaTGlobal[gIdx];
      vAreaT(n, 0) = areaTGlobal[gIdx];
      vLonU(n, 0)  = lonUGlobal[gIdx];
      vLatU(n, 0)  = latUGlobal[gIdx];
      vLonV(n, 0)  = lonVGlobal[gIdx];
      vLatV(n, 0)  = latVGlobal[gIdx];
    }
  }

  // Fill mask3d unconditionally (registered above regardless of vertical
  // geometry) so the masked-cell flood fill always has a per-level mask.
  {
    auto vMask3d = atlas::array::make_view<double, 2>(fMask3d);
    for (int n = 0; n < npts; ++n) {
      const int iG   = jediPoints_[n].first;
      const int jG   = jediPoints_[n].second;
      const int gIdx = jG * niEff_ + iG;
      for (int k = 0; k < numLevels_; ++k) {
        const size_t idx3D = static_cast<size_t>(k) * nHoriz + gIdx;
        vMask3d(n, k) = mask3dGlobal[idx3D];
      }
    }
  }

  // Halo-exchange the geometry fields so that the periodic / northern-fold halo
  // nodes added by build_halo (which the jediPoints_ loop above does not cover)
  // carry valid owner values. Without this, downstream consumers that iterate
  // every functionspace node - notably the land-mask flood fill, which seeds on
  // mask3d - read uninitialised halo memory, giving non-reproducible results
  // run to run. "owned" is excluded: it must stay 1 on owned / 0 on ghost.
  {
    atlas::functionspace::NodeColumns fsHalo(functionSpace_);
    for (auto & f : fields_) {
      if (f.name() == "owned") continue;
      fsHalo.haloExchange(f);
    }
  }

  // TODO(guillaumevernieres): buildDistFromCoast is too slow at high resolution.
  // Make it optional (config-gated) and/or optimize the distance-to-coast
  // computation before re-enabling it here.
  // if (buildVerticalGeometry) {
  //   buildDistFromCoast(lonGlobal, latGlobal, wetGlobal, mask3dGlobal);
  // }
}

// ---------------------------------------------------------------------------
// Compute great-circle distance (metres) from each JEDI point to the
// nearest land cell.  lonGlobal/latGlobal/wetGlobal are full global arrays
// (all ranks have them), so no MPI needed.
void GeometryMOM6::buildDistFromCoast(const std::vector<double> & lonGlobal,
                                       const std::vector<double> & latGlobal,
                                       const std::vector<double> & wetGlobal,
                                       const std::vector<double> & mask3dGlobal)
{
  const int nGlobal = static_cast<int>(wetGlobal.size());
  const int npts    = static_cast<int>(jediPoints_.size());
  const size_t nHoriz = static_cast<size_t>(njEff_) * niEff_;

  // --- 2-D surface distance (one KD-tree from surface land points) ---
  {
    atlas::util::IndexKDTree kdtree;
    for (int i = 0; i < nGlobal; ++i) {
      if (wetGlobal[i] == 0.0)
        kdtree.insert(atlas::PointLonLat{lonGlobal[i], latGlobal[i]},
                      static_cast<atlas::idx_t>(i));
    }
    kdtree.build();

    atlas::Field fDist = functionSpace_.createField<double>(
        atlas::option::name("dist_from_coast") | atlas::option::levels(1));
    auto vDist = atlas::array::make_view<double, 2>(fDist);
    for (int n = 0; n < npts; ++n) {
      const int gIdx = jediPoints_[n].second * niEff_ + jediPoints_[n].first;
      const atlas::PointLonLat pt{lonGlobal[gIdx], latGlobal[gIdx]};
      const auto nearest = kdtree.closestPoint(pt);
      const int landK = static_cast<int>(nearest.payload());
      vDist(n, 0) = atlas::util::Earth::distance(
          pt, atlas::PointLonLat{lonGlobal[landK], latGlobal[landK]});
    }
    fields_.add(fDist);
  }

  // --- 3-D per-layer distance (one KD-tree per level from mask3d land points) ---
  atlas::Field fDist3d = functionSpace_.createField<double>(
      atlas::option::name("dist_from_coast3d") | atlas::option::levels(numLevels_));
  auto vDist3d = atlas::array::make_view<double, 2>(fDist3d);

  for (int k = 0; k < numLevels_; ++k) {
    atlas::util::IndexKDTree kdtree;
    for (int i = 0; i < nGlobal; ++i) {
      const size_t idx = static_cast<size_t>(k) * nHoriz + i;
      if (mask3dGlobal[idx] == 0.0)
        kdtree.insert(atlas::PointLonLat{lonGlobal[i], latGlobal[i]},
                      static_cast<atlas::idx_t>(i));
    }
    kdtree.build();

    for (int n = 0; n < npts; ++n) {
      const int gIdx = jediPoints_[n].second * niEff_ + jediPoints_[n].first;
      const atlas::PointLonLat pt{lonGlobal[gIdx], latGlobal[gIdx]};
      const auto nearest = kdtree.closestPoint(pt);
      const int landK = static_cast<int>(nearest.payload());
      vDist3d(n, k) = atlas::util::Earth::distance(
          pt, atlas::PointLonLat{lonGlobal[landK], latGlobal[landK]});
    }
  }
  fields_.add(fDist3d);
}

// ---------------------------------------------------------------------------
// Build scatter/gather map: for each JEDI-owned point, record the owning
// MOM6 rank and the row-major local index on that rank.
// Deterministic — no MPI needed.
void GeometryMOM6::buildScatterMap()
{
  scatterMap_.mom6Rank.resize(ownedCount_);
  scatterMap_.mom6LocalIdx.resize(ownedCount_);

  for (int n = 0; n < ownedCount_; ++n) {
    const int iG = jediPoints_[n].first;
    const int jG = jediPoints_[n].second;

    // Find MOM6 piX (i-rank) and piY (j-rank) owning (iG, jG)
    const int baseX  = niEff_ / layoutX_;
    const int extraX = niEff_ % layoutX_;
    const int piX    = (iG < extraX * (baseX + 1))
                       ? iG / (baseX + 1)
                       : extraX + (iG - extraX * (baseX + 1)) / baseX;

    const int baseY  = njEff_ / layoutY_;
    const int extraY = njEff_ % layoutY_;
    const int piY    = (jG < extraY * (baseY + 1))
                       ? jG / (baseY + 1)
                       : extraY + (jG - extraY * (baseY + 1)) / baseY;

    scatterMap_.mom6Rank[n] = piX * layoutY_ + piY;

    // 0-based start of that tile
    int iStart0 = 0;
    for (int k = 0; k < piX; ++k) iStart0 += mom6::computeExtent(niEff_, layoutX_, k);
    int jStart0 = 0;
    for (int k = 0; k < piY; ++k) jStart0 += mom6::computeExtent(njEff_, layoutY_, k);

    scatterMap_.mom6LocalIdx[n] =
        (jG - jStart0) * mom6::computeExtent(niEff_, layoutX_, piX)
        + (iG - iStart0);
  }
}

// ---------------------------------------------------------------------------
// Precompute allToAllv metadata from ScatterMap.
// Called once after buildScatterMap.
//
// Roles (from each rank's perspective during scatter, i.e. MOM6 → JEDI):
//   recvCounts/recvDispl : values this rank wants from each MOM6 rank
//   sendCounts/sendDispl : values this rank must send to each JEDI rank
//   sendLocalIdx : local mom6FunctionSpace_ indices to pack per send
void GeometryMOM6::buildExchangePlan()
{
  const int npes = static_cast<int>(comm_.size());

  // 1. Count requests from each MOM6 rank (pure local)
  std::vector<int> recvCounts(npes, 0);
  for (int n = 0; n < ownedCount_; ++n)
    recvCounts[scatterMap_.mom6Rank[n]]++;

  // 2. Exchange counts: MOM6 ranks learn how many values each JEDI rank
  //    requests
  std::vector<int> sendCounts(npes, 0);
  comm_.allToAll(recvCounts, sendCounts);

  // 3. Build recvOrder: sort owned-JEDI indices by source MOM6 rank so that the
  //    recv buffer arrives in contiguous rank-order blocks.
  std::vector<int> recvOrder(ownedCount_);
  std::iota(recvOrder.begin(), recvOrder.end(), 0);
  std::stable_sort(recvOrder.begin(), recvOrder.end(),
    [&](int a, int b) {
      return scatterMap_.mom6Rank[a] < scatterMap_.mom6Rank[b];
    });

  // 4. Prefix-sum displacements
  std::vector<int> recvDispl(npes, 0), sendDispl(npes, 0);
  for (int r = 1; r < npes; ++r) {
    recvDispl[r] = recvDispl[r-1] + recvCounts[r-1];
    sendDispl[r] = sendDispl[r-1] + sendCounts[r-1];
  }

  // 5. Each JEDI rank sends the requested mom6LocalIdx values to the
  //    owning MOM6 ranks
  std::vector<int> reqIdxBuf(ownedCount_);
  for (int i = 0; i < ownedCount_; ++i)
    reqIdxBuf[i] = scatterMap_.mom6LocalIdx[recvOrder[i]];

  const int totalSend = sendDispl[npes-1] + sendCounts[npes-1];
  std::vector<int> sendLocalIdx(totalSend);
  comm_.allToAllv(reqIdxBuf.data(),    recvCounts.data(), recvDispl.data(),
                  sendLocalIdx.data(), sendCounts.data(), sendDispl.data());

  exchangePlan_.recvCounts   = std::move(recvCounts);
  exchangePlan_.recvDispl    = std::move(recvDispl);
  exchangePlan_.recvOrder    = std::move(recvOrder);
  exchangePlan_.sendCounts   = std::move(sendCounts);
  exchangePlan_.sendDispl    = std::move(sendDispl);
  exchangePlan_.sendLocalIdx = std::move(sendLocalIdx);
}

// ---------------------------------------------------------------------------
// Scatter a field from the MOM6 structured decomposition to the JEDI
// unstructured one.  Only owned JEDI nodes are filled; ghost nodes are
// left untouched.
void GeometryMOM6::scatterToJedi(const atlas::Field & mom6Field,
                                       atlas::Field * jediField) const
{
  const int npes  = static_cast<int>(comm_.size());
  const int nSend = exchangePlan_.sendDispl[npes-1]
                  + exchangePlan_.sendCounts[npes-1];

  // MOM6 side: pack values at the requested local indices
  auto vMom6 = atlas::array::make_view<double, 2>(mom6Field);
  std::vector<double> sendBuf(nSend);
  for (int i = 0; i < nSend; ++i)
    sendBuf[i] = vMom6(exchangePlan_.sendLocalIdx[i], 0);

  // AllToAllv: MOM6 ranks → JEDI ranks
  std::vector<double> recvBuf(ownedCount_);
  comm_.allToAllv(sendBuf.data(),
                  exchangePlan_.sendCounts.data(),
                  exchangePlan_.sendDispl.data(),
                  recvBuf.data(),
                  exchangePlan_.recvCounts.data(),
                  exchangePlan_.recvDispl.data());

  // JEDI side: unpack into owned positions using the recvOrder permutation
  auto vJedi = atlas::array::make_view<double, 2>(*jediField);
  for (int i = 0; i < ownedCount_; ++i)
    vJedi(exchangePlan_.recvOrder[i], 0) = recvBuf[i];
}

// ---------------------------------------------------------------------------
// Gather a field from the JEDI unstructured decomposition back to the
// MOM6 structured one.  Only active (JEDI) point positions in mom6Field
// are updated; interior land cells are unchanged.
void GeometryMOM6::gatherFromJedi(const atlas::Field & jediField,
                                        atlas::Field * mom6Field) const
{
  const int npes  = static_cast<int>(comm_.size());
  const int nRecv = exchangePlan_.sendDispl[npes-1]
                  + exchangePlan_.sendCounts[npes-1];

  // JEDI side: pack owned values in the same order scatter used
  auto vJedi = atlas::array::make_view<double, 2>(jediField);
  std::vector<double> sendBuf(ownedCount_);
  for (int i = 0; i < ownedCount_; ++i)
    sendBuf[i] = vJedi(exchangePlan_.recvOrder[i], 0);

  // AllToAllv: JEDI → MOM6 (send/recv descriptors transposed vs scatter)
  std::vector<double> recvBuf(nRecv);
  comm_.allToAllv(sendBuf.data(),
                  exchangePlan_.recvCounts.data(),
                  exchangePlan_.recvDispl.data(),
                  recvBuf.data(),
                  exchangePlan_.sendCounts.data(),
                  exchangePlan_.sendDispl.data());

  // MOM6 side: unpack at the originally requested local indices
  auto vMom6 = atlas::array::make_view<double, 2>(*mom6Field);
  for (int i = 0; i < nRecv; ++i)
    vMom6(exchangePlan_.sendLocalIdx[i], 0) = recvBuf[i];
}

// ---------------------------------------------------------------------------
// Self-consistency check.
// Check 1: invert the scatter map; verify it recovers jediPoints_.
// Check 2: scatter mom6Fields_["lon"] and compare with fields_["lon"].
// Check 3: gather fields_["depth"] into a temp MOM6 field and compare
//          with mom6Fields_["depth"] at all active point positions.
void GeometryMOM6::checkScatterMap()
{
  const int rank  = static_cast<int>(comm_.rank());
  const int npes  = static_cast<int>(comm_.size());
  int nErrors = 0;

  // --- Check 1: index arithmetic ---
  for (int n = 0; n < ownedCount_; ++n) {
    const int r      = scatterMap_.mom6Rank[n];
    const int locIdx = scatterMap_.mom6LocalIdx[n];

    const int piX = r / layoutY_;
    const int piY = r % layoutY_;

    int iStart0 = 0;
    for (int k = 0; k < piX; ++k) iStart0 += mom6::computeExtent(niEff_, layoutX_, k);
    int jStart0 = 0;
    for (int k = 0; k < piY; ++k) jStart0 += mom6::computeExtent(njEff_, layoutY_, k);

    const int iCount   = mom6::computeExtent(niEff_, layoutX_, piX);
    const int iG_check = iStart0 + (locIdx % iCount);
    const int jG_check = jStart0 + (locIdx / iCount);

    const int iG = jediPoints_[n].first;
    const int jG = jediPoints_[n].second;

    if (iG_check != iG || jG_check != jG) {
      oops::Log::error() << "checkScatterMap rank " << rank << " point " << n
                         << ": expected (" << iG << "," << jG
                         << ") got (" << iG_check << ","
                         << jG_check << ")" << std::endl;
      ++nErrors;
    }
  }

  // --- Check 2: scatter lon and compare with fields_["lon"] ---
  atlas::Field tempLon = functionSpace_.createField<double>(
      atlas::option::name("temp_lon") | atlas::option::levels(1));
  scatterToJedi(mom6Fields_.field("lon"), &tempLon);

  auto vScattered = atlas::array::make_view<double, 2>(tempLon);
  auto vJediLon   = atlas::array::make_view<double, 2>(fields_.field("lon"));
  for (int n = 0; n < ownedCount_; ++n) {
    if (std::abs(vScattered(n, 0) - vJediLon(n, 0)) > 1e-10) {
      oops::Log::error() << "checkScatterMap scatter lon mismatch rank " << rank
                         << " n=" << n << ": got " << vScattered(n, 0)
                         << " expected " << vJediLon(n, 0) << std::endl;
      ++nErrors;
    }
  }

  // --- Check 3: gather depth and compare with mom6Fields_["depth"] ---
  atlas::Field tempDepth = mom6FunctionSpace_.createField<double>(
      atlas::option::name("temp_depth") | atlas::option::levels(1));
  gatherFromJedi(fields_.field("depth"), &tempDepth);

  auto vGathered  = atlas::array::make_view<double, 2>(tempDepth);
  auto vMom6Depth = atlas::array::make_view<double, 2>(
                       mom6Fields_.field("depth"));
  const int nRecv = exchangePlan_.sendDispl[npes-1]
                  + exchangePlan_.sendCounts[npes-1];
  for (int i = 0; i < nRecv; ++i) {
    const int localIdx = exchangePlan_.sendLocalIdx[i];
    if (std::abs(vGathered(localIdx, 0) - vMom6Depth(localIdx, 0)) > 1e-10) {
      ++nErrors;
    }
  }

  comm_.allReduceInPlace(nErrors, eckit::mpi::sum());
  if (nErrors > 0)
    throw eckit::Exception(
        "checkScatterMap: " + std::to_string(nErrors) + " error(s)",
        Here());

  oops::Log::info() << "checkScatterMap: index + scatter + gather OK on rank "
                    << rank << std::endl;
}

// ---------------------------------------------------------------------------
GeometryMOM6::GeometryMOM6(const eckit::Configuration & conf,
                           const eckit::mpi::Comm & comm,
                           eckit::LocalConfiguration &geomVariables,
                           atlas::FunctionSpace &functionSpace,
                           atlas::FieldSet &geomFields,
                           bool &levelsAreTopDown, int &numberLevels)
  : comm_(comm)
{
  oops::Log::trace() << "GeometryMOM6 constructor starting" << std::endl;
  levelsAreTopDown = true;

  util::Timer ctorTimer("ijedi::GeometryMOM6", "GeometryMOM6");

  {
    util::Timer timer("ijedi::GeometryMOM6", "configure");

  // 1. Read grid parameters from the MOM_input sub-configuration
  const eckit::LocalConfiguration momConf(conf, "MOM_input");
  niGlobal_     = momConf.getInt("NIGLOBAL");
  njGlobal_     = momConf.getInt("NJGLOBAL");
  numLevels_    = momConf.getInt("NZ");
  minimumDepth_ = momConf.getDouble("MINIMUM_DEPTH");
  const std::vector<int> layout = momConf.getIntVector("LAYOUT");
  layoutX_ = layout[0];
  layoutY_ = layout[1];

  const int npes = static_cast<int>(comm.size());
  if (npes != layoutX_ * layoutY_)
    throw eckit::BadValue(
        "MOM6 geometry: number of MPI tasks (" + std::to_string(npes) +
        ") must equal LAYOUT[0]*LAYOUT[1] (" + std::to_string(layoutX_) +
        "*" + std::to_string(layoutY_) + "=" +
        std::to_string(layoutX_ * layoutY_) + ")", Here());

  coarsenFactor_ = conf.getInt("coarsen factor", 1);
  if (niGlobal_ % coarsenFactor_ != 0 || njGlobal_ % coarsenFactor_ != 0)
    throw eckit::BadValue("coarsen_factor=" + std::to_string(coarsenFactor_)
        + " does not divide NIGLOBAL=" + std::to_string(niGlobal_)
        + " x NJGLOBAL=" + std::to_string(njGlobal_)
        + " (need both divisible)", Here());
  niEff_   = niGlobal_ / coarsenFactor_;
  njEff_   = njGlobal_ / coarsenFactor_;
  fringeWidth_ = conf.getInt("fringe width", 2);
  if (fringeWidth_ < 0)
    throw eckit::BadValue("fringe width must be non-negative, got " +
                          std::to_string(fringeWidth_), Here());
  hasFold_ = conf.getBool("has northern fold", true);
  }

  oops::Log::debug() << "GeometryMOM6: NI=" << niGlobal_ << " NJ=" << njGlobal_
                     << " NZ=" << numLevels_
                     << " layout=(" << layoutX_ << "," << layoutY_ << ")"
                     << " northern_fold=" << std::boolalpha << hasFold_
                     << " fringe_width=" << fringeWidth_
                     << " coarsen_factor=" << coarsenFactor_ << std::endl;

  // 2. Compute local MOM6 domain extent for this rank
  //    MOM6 convention: rank = piX * layoutY_ + piY
  const int rank = static_cast<int>(comm.rank());
  {
    util::Timer timer("ijedi::GeometryMOM6", "computeLocalMom6DomainExtent");
  const int piX  = rank / layoutY_;
  const int piY  = rank % layoutY_;

  iCount_ = mom6::computeExtent(niEff_, layoutX_, piX);
  jCount_ = mom6::computeExtent(njEff_, layoutY_, piY);

  iStart_ = 1;
  for (int k = 0; k < piX; ++k) iStart_ += mom6::computeExtent(niEff_, layoutX_, k);
  jStart_ = 1;
  for (int k = 0; k < piY; ++k) jStart_ += mom6::computeExtent(njEff_, layoutY_, k);

  oops::Log::debug() << "GeometryMOM6 rank " << rank
                     << ": iStart=" << iStart_ << " iCount=" << iCount_
                     << " jStart=" << jStart_
                     << " jCount=" << jCount_ << std::endl;
  }

  // 3. Read global grid arrays (all ranks read identically)
  const std::string hgridPath = conf.getString("ocean_hgrid");
  const std::string topogPath = conf.getString("ocean_topog");
  const bool buildVerticalGeometry = conf.has("vertical geometry from");

  std::vector<double> lon, lat, dxT, dyT, areaT, lonU, latU, lonV, latV;
  {
    util::Timer timer("ijedi::GeometryMOM6", "readHgrid");
    mom6::readHgrid(hgridPath,
                    niGlobal_, njGlobal_, niEff_, njEff_, coarsenFactor_,
                    &lon, &lat, &dxT, &dyT, &areaT, &lonU, &latU, &lonV, &latV);
  }

  std::vector<double> depth, wet;
  {
    util::Timer timer("ijedi::GeometryMOM6", "readTopog");
    mom6::readTopog(topogPath,
                    niGlobal_, njGlobal_, niEff_, njEff_, coarsenFactor_, minimumDepth_,
                    &depth, &wet);
  }

  if (conf.has("minimum layer thickness")) {
    minimumThickness_ = conf.getDouble("minimum layer thickness");
  } else {
    // Backward-compatible YAML key used in existing test configs.
    minimumThickness_ = conf.getDouble("minimum_layer_thickness", 1e-6);
  }

  const size_t nHoriz3d = static_cast<size_t>(njEff_) * niEff_;
  std::vector<double> layerThickness(static_cast<size_t>(numLevels_) * nHoriz3d,
                                     minimumThickness_);
  std::vector<double> layerCenterDepth(static_cast<size_t>(numLevels_) * nHoriz3d, 0.0);
  if (buildVerticalGeometry) {
    const std::string verticalGeomPath = conf.getString("vertical geometry from");
    util::Timer timer("ijedi::GeometryMOM6", "readVerticalGeometry");
    mom6::readVerticalGeometry(verticalGeomPath,
                               niGlobal_, njGlobal_, niEff_, njEff_, numLevels_,
                               coarsenFactor_,
                               &layerThickness, &layerCenterDepth);
  } else {
    oops::Log::info() << "GeometryMOM6: skipping vertical geometry read"
                      << " (vertical geometry from not provided)" << std::endl;
  }

  // Build 3D mask: ocean (1.0) where surface is wet and layer thickness
  // is >= minimumThickness_. Once a thin layer is encountered top-down,
  // all deeper layers are also masked (sealed = land).
  std::vector<double> mask3d(static_cast<size_t>(numLevels_) * nHoriz3d, 0.0);
  {
    util::Timer timer("ijedi::GeometryMOM6", "buildMask3d");
    if (buildVerticalGeometry) {
      for (size_t n = 0; n < nHoriz3d; ++n) {
        if (wet[n] < 0.5) continue;  // surface land → all layers stay 0
        bool sealed = false;
        for (int k = 0; k < numLevels_; ++k) {
          const size_t idx = static_cast<size_t>(k) * nHoriz3d + n;
          if (!sealed && layerThickness[idx] < minimumThickness_) sealed = true;
          mask3d[idx] = sealed ? 0.0 : 1.0;
        }
      }
    } else {
      for (size_t n = 0; n < nHoriz3d; ++n) {
        if (wet[n] < 0.5) continue;
        for (int k = 0; k < numLevels_; ++k) {
          const size_t idx = static_cast<size_t>(k) * nHoriz3d + n;
          mask3d[idx] = 1.0;
        }
      }
    }
  }

  // 4. MOM6 structured function space and fields (local compute domain)
  {
    util::Timer timer("ijedi::GeometryMOM6", "buildMom6FunctionSpace");
    buildMom6FunctionSpace(comm, lon, lat);
  }
  {
    util::Timer timer("ijedi::GeometryMOM6", "buildMom6Fields");
    buildMom6Fields(lon, lat, depth, wet, layerThickness, layerCenterDepth,
                    mask3d, dxT, dyT, areaT,
                    lonU, latU, lonV, latV);
  }

  // 5. JEDI unstructured function space and fields (ocean + retained fringe)
  {
    util::Timer timer("ijedi::GeometryMOM6", "buildJediFunctionSpace");
    buildJediFunctionSpace(comm, wet, lon, lat);
  }
  {
    util::Timer timer("ijedi::GeometryMOM6", "buildFields");
    buildFields(lon, lat, depth, wet, layerThickness, layerCenterDepth,
                mask3d, dxT, dyT, areaT, lonU, latU, lonV, latV,
                buildVerticalGeometry);
  }

  // Release temporary global arrays now that local fieldsets are built.
  auto release = [](std::vector<double> & v) {
    std::vector<double>().swap(v);
  };
  release(lon);
  release(lat);
  release(dxT);
  release(dyT);
  release(areaT);
  release(lonU);
  release(latU);
  release(lonV);
  release(latV);
  release(depth);
  release(wet);
  release(layerThickness);
  release(layerCenterDepth);
  release(mask3d);

  // 6. Scatter/gather map and exchange plan: JEDI ↔ MOM6
  const bool buildScatterExchange =
      conf.getBool("JEDI/MOM6 scatter/gather exchange plan", true);
  if (buildScatterExchange) {
    {
      util::Timer timer("ijedi::GeometryMOM6", "buildScatterMap");
      buildScatterMap();
    }
    {
      util::Timer timer("ijedi::GeometryMOM6", "buildExchangePlan");
      buildExchangePlan();
    }
    if (conf.getBool("check scatter map", false)) {
      util::Timer timer("ijedi::GeometryMOM6", "checkScatterMap");
      checkScatterMap();
    }
  } else {
    oops::Log::info() << "GeometryMOM6: skipping scatter/gather map and exchange plan"
                      << " (JEDI/MOM6 scatter/gather exchange plan=false)"
                      << std::endl;
  }

  // 7. Optionally save grids to NetCDF / debug files
  if (conf.has("save unstructured grid to"))
    {
      util::Timer timer("ijedi::GeometryMOM6", "saveGrid");
      saveGrid(conf.getString("save unstructured grid to"), comm);
    }
  if (conf.has("save structured grid to"))
    {
      util::Timer timer("ijedi::GeometryMOM6", "saveStructuredGrid");
      saveStructuredGrid(conf.getString("save structured grid to"), comm);
    }
  if (conf.has("save debug mesh to"))
    {
      util::Timer timer("ijedi::GeometryMOM6", "saveDebugMesh");
      saveDebugMesh(conf.getString("save debug mesh to"), comm);
    }
  if (conf.has("save gmsh to"))
    {
      util::Timer timer("ijedi::GeometryMOM6", "saveGmsh");
      saveGmsh(conf.getString("save gmsh to"), comm);
    }

  // 8. Populate output parameters
  {
    util::Timer timer("ijedi::GeometryMOM6", "setOutputs");
    functionSpace = functionSpace_;
    geomFields    = fields_;
    numberLevels  = numLevels_;

    geomVariables.set("ni", niEff_);
    geomVariables.set("nj", njEff_);
    geomVariables.set("nz", numLevels_);
  }

  oops::Log::trace() << "GeometryMOM6 constructor done" << std::endl;
}

// ---------------------------------------------------------------------------
// Gather all MOM6-structured geometry fields from all ranks and write a
// (nj, ni) NetCDF file on rank 0.  Uses mom6Fields_ which holds the local
// MOM6 compute-domain data.
void GeometryMOM6::saveStructuredGrid(const std::string & filename,
                                       const eckit::mpi::Comm & comm) const
{
  const int npes = static_cast<int>(comm.size());
  const int root = 0;

  std::vector<std::string> fieldNames2D, fieldNames3D;
  for (const auto & field : mom6Fields_) {
    if (field.name() == "owned") continue;
    (field.shape(1) == 1 ? fieldNames2D : fieldNames3D).push_back(field.name());
  }

  // Step 1: gather per-rank local sizes on root
  const int localSize = jCount_ * iCount_;
  std::vector<int> recvcounts(npes, 0);
  comm.gather(localSize, recvcounts, root);

  std::vector<int> displs;
  if (comm.rank() == root) {
    displs.resize(npes);
    displs[0] = 0;
    for (int p = 1; p < npes; ++p)
      displs[p] = displs[p - 1] + recvcounts[p - 1];
  }

  // Step 2: on root, precompute (iStart, jStart, iCount, jCount) for every rank
  struct DomInfo { int iStart, jStart, iCount, jCount; };
  std::vector<DomInfo> allDoms;
  if (comm.rank() == root) {
    allDoms.resize(npes);
    for (int p = 0; p < npes; ++p) {
      const int px = p / layoutY_;
      const int py = p % layoutY_;
      allDoms[p].iCount = mom6::computeExtent(niEff_, layoutX_, px);
      allDoms[p].jCount = mom6::computeExtent(njEff_, layoutY_, py);
      allDoms[p].iStart = 1;
      for (int k = 0; k < px; ++k)
        allDoms[p].iStart += mom6::computeExtent(niEff_, layoutX_, k);
      allDoms[p].jStart = 1;
      for (int k = 0; k < py; ++k)
        allDoms[p].jStart += mom6::computeExtent(njEff_, layoutY_, k);
    }
  }

  // Step 3: create NetCDF file on root
  int ncid = -1;
  std::vector<int> varids2D(fieldNames2D.size(), -1);
  std::vector<int> varids3D(fieldNames3D.size(), -1);
  int part_varid = -1;
  if (comm.rank() == root) {
    if (nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid) != NC_NOERR)
      throw eckit::CantOpenFile(filename, Here());
    int nj_dim, ni_dim, nz_dim = -1;
    nc_def_dim(ncid, "nj", static_cast<size_t>(njEff_), &nj_dim);
    nc_def_dim(ncid, "ni", static_cast<size_t>(niEff_), &ni_dim);
    if (!fieldNames3D.empty())
      nc_def_dim(ncid, "nz", static_cast<size_t>(numLevels_), &nz_dim);
    const int dims2D[2] = {nj_dim, ni_dim};
    const int dims3D[3] = {nz_dim, nj_dim, ni_dim};
    for (size_t f = 0; f < fieldNames2D.size(); ++f)
      nc_def_var(ncid, fieldNames2D[f].c_str(), NC_DOUBLE, 2, dims2D, &varids2D[f]);
    for (size_t f = 0; f < fieldNames3D.size(); ++f)
      nc_def_var(ncid, fieldNames3D[f].c_str(), NC_DOUBLE, 3, dims3D, &varids3D[f]);
    nc_def_var(ncid, "partition", NC_INT, 2, dims2D, &part_varid);
    nc_enddef(ncid);
  }

  // Step 4: for each field, gather and write
  std::vector<double> recvBuf;
  if (comm.rank() == root) {
    const int total = displs.back() + recvcounts.back();
    recvBuf.resize(total);
  }

  // Assemble one global (nj x ni) slice from per-rank contributions
  auto assembleGlobal = [&](std::vector<double> & global) {
    global.assign(static_cast<size_t>(njEff_) * niEff_, 0.0);
    for (int p = 0; p < npes; ++p) {
      const DomInfo & d = allDoms[p];
      const int off = displs[p];
      for (int jL = 0; jL < d.jCount; ++jL)
        for (int iL = 0; iL < d.iCount; ++iL) {
          const int jG = d.jStart - 1 + jL;
          const int iG = d.iStart - 1 + iL;
          global[static_cast<size_t>(jG) * niEff_ + iG] =
              recvBuf[off + jL * d.iCount + iL];
        }
    }
  };

  // 2D fields
  for (size_t f = 0; f < fieldNames2D.size(); ++f) {
    auto view = atlas::array::make_view<double, 2>(
                    mom6Fields_.field(fieldNames2D[f]));
    std::vector<double> localData(localSize);
    for (int n = 0; n < localSize; ++n) localData[n] = view(n, 0);
    comm.gatherv(localData, recvBuf, recvcounts, displs, root);
    if (comm.rank() == root) {
      std::vector<double> global;
      assembleGlobal(global);
      nc_put_var_double(ncid, varids2D[f], global.data());
    }
  }

  // 3D fields: gather one level at a time
  for (size_t f = 0; f < fieldNames3D.size(); ++f) {
    auto view = atlas::array::make_view<double, 2>(
                    mom6Fields_.field(fieldNames3D[f]));
    for (int k = 0; k < numLevels_; ++k) {
      std::vector<double> localData(localSize);
      for (int n = 0; n < localSize; ++n) localData[n] = view(n, k);
      comm.gatherv(localData, recvBuf, recvcounts, displs, root);
      if (comm.rank() == root) {
        std::vector<double> global;
        assembleGlobal(global);
        const size_t start[3] = {static_cast<size_t>(k), 0, 0};
        const size_t count[3] = {1, static_cast<size_t>(njEff_),
                                    static_cast<size_t>(niEff_)};
        nc_put_vara_double(ncid, varids3D[f], start, count, global.data());
      }
    }
  }

  // Write partition map: partition[jG][iG] = owning MOM6 rank
  if (comm.rank() == root) {
    std::vector<int> partGlobal(static_cast<size_t>(njEff_) * niEff_, -1);
    for (int p = 0; p < npes; ++p) {
      const DomInfo & d = allDoms[p];
      for (int jL = 0; jL < d.jCount; ++jL)
        for (int iL = 0; iL < d.iCount; ++iL) {
          const int jG = d.jStart - 1 + jL;
          const int iG = d.iStart - 1 + iL;
          partGlobal[jG * niEff_ + iG] = p;
        }
    }
    nc_put_var_int(ncid, part_varid, partGlobal.data());
    nc_close(ncid);
  }
}

// ---------------------------------------------------------------------------
// Save the JEDI geometry fields to NetCDF using oops Atlas IO helpers.
void GeometryMOM6::saveGrid(const std::string & filename,
                             const eckit::mpi::Comm & comm) const
{
  // "lon" and "lat" are written automatically by Atlas writeFieldSet as node
  // coordinate variables; including them as fields causes a NetCDF name clash.
  // "owned" is a rank-partitioning artefact that is meaningless after reload.
  static const std::unordered_set<std::string> skip = {"owned", "lon", "lat"};
  atlas::FieldSet toWrite;
  for (const auto & field : fields_) {
    if (!skip.count(field.name())) toWrite.add(field);
  }

  std::string filepath = filename;
  const std::string ext = ".nc";
  if (filepath.size() > ext.size() &&
      filepath.compare(filepath.size() - ext.size(), ext.size(), ext) == 0)
    filepath.erase(filepath.size() - ext.size());

  eckit::LocalConfiguration conf;
  conf.set("filepath", filepath);
  util::writeFieldSet(comm, conf, toWrite);
}

// ---------------------------------------------------------------------------
// Write debug output for inspecting Atlas domain decomposition and halo.
void GeometryMOM6::saveDebugMesh(const std::string & prefix,
                                  const eckit::mpi::Comm & comm) const
{
  const int rank = static_cast<int>(comm.rank());
  const atlas::functionspace::NodeColumns fspace(functionSpace_);
  const atlas::Mesh & mesh = fspace.mesh();

  {
    const std::string ncFile =
        prefix + "_rank" + std::to_string(rank) + ".nc";

    const atlas::mesh::Nodes & nodes = mesh.nodes();
    const int nNodes = nodes.size();

    auto lonlatView = atlas::array::make_view<double,        2>(nodes.lonlat());
    auto ghostView  = atlas::array::make_view<int,           1>(nodes.ghost());
    auto partView = atlas::array::make_view<int, 1>(nodes.partition());
    auto gidxView = atlas::array::make_view<atlas::gidx_t, 1>(
                        nodes.global_index());
    auto maskView = atlas::array::make_view<double, 2>(
                        fields_.field("mask2d"));

    std::vector<double>    lons(nNodes), lats(nNodes), mask(nNodes);
    std::vector<int>         ghost(nNodes), part(nNodes);
    std::vector<int64_t>     gidx(nNodes);
    for (int n = 0; n < nNodes; ++n) {
      lons[n]  = lonlatView(n, 0);
      lats[n]  = lonlatView(n, 1);
      ghost[n] = ghostView(n);
      part[n]  = partView(n);
      gidx[n]  = static_cast<int64_t>(gidxView(n));
      mask[n]  = maskView(n, 0);
    }

    int ncid;
    if (nc_create(ncFile.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid) != NC_NOERR)
      throw eckit::CantOpenFile(ncFile, Here());

    int node_dim;
    nc_def_dim(ncid, "node", static_cast<size_t>(nNodes), &node_dim);

    int vid_lon, vid_lat, vid_ghost, vid_part, vid_gidx, vid_rank, vid_mask;
    nc_def_var(ncid, "lon",          NC_DOUBLE, 1, &node_dim, &vid_lon);
    nc_def_var(ncid, "lat",          NC_DOUBLE, 1, &node_dim, &vid_lat);
    nc_def_var(ncid, "ghost",        NC_INT,    1, &node_dim, &vid_ghost);
    nc_def_var(ncid, "partition",    NC_INT,    1, &node_dim, &vid_part);
    nc_def_var(ncid, "global_index", NC_INT64,  1, &node_dim, &vid_gidx);
    nc_def_var(ncid, "mask2d",       NC_DOUBLE, 1, &node_dim, &vid_mask);
    nc_def_var(ncid, "rank",         NC_INT,    0, nullptr,   &vid_rank);
    nc_enddef(ncid);

    nc_put_var_double(ncid, vid_lon,   lons.data());
    nc_put_var_double(ncid, vid_lat,   lats.data());
    nc_put_var_int(ncid, vid_ghost, ghost.data());
    nc_put_var_int(ncid, vid_part,  part.data());
    nc_put_var(ncid, vid_gidx, gidx.data());
    nc_put_var_double(ncid, vid_mask,  mask.data());
    nc_put_var_int(ncid, vid_rank,  &rank);
    nc_close(ncid);

    oops::Log::info() << "GeometryMOM6::saveDebugMesh: rank " << rank
                      << " -> " << ncFile << std::endl;
  }
}

// ---------------------------------------------------------------------------
// Write the Atlas mesh to a Gmsh file for visualisation.  Each rank writes
// its own portion (owned + halo nodes/cells); ghost=true makes halos visible.
void GeometryMOM6::saveGmsh(const std::string & filename,
                             const eckit::mpi::Comm & comm) const
{
  const atlas::functionspace::NodeColumns fspace(functionSpace_);
  eckit::LocalConfiguration gmshConf;
  gmshConf.set("coordinates", "xyz");
  gmshConf.set("ghost", true);
  atlas::output::Gmsh gmsh(filename, gmshConf);
  gmsh.write(fspace.mesh());
  atlas::FieldSet toWrite;
  for (const char * name : {"mask2d", "depth", "dist_from_coast",
                            "sea_water_cell_thickness", "sea_water_depth",
                            "mask3d", "dist_from_coast3d"}) {
    if (fields_.has(name))
      toWrite.add(fields_.field(name));
  }
  gmsh.write(toWrite, functionSpace_);

  // Reduce Gmsh GUI load for 3D fields: store vertical levels as timesteps.
  std::string rewriteFile = filename;
  if (comm.size() > 1) {
    // In MPI runs Atlas writes per-rank payload files and a tiny merge file.
    // Rewrite each payload file so all 3D fields are exposed as time series.
    rewriteFile += ".p" + std::to_string(comm.rank());
  }
  mom6::rewriteLeveledNodeDataAsTime(rewriteFile,
                     {"sea_water_cell_thickness", "sea_water_depth",
                    "mask3d", "dist_from_coast3d"});

  comm.barrier();
  if (comm.rank() == 0 && comm.size() > 1) {
    // Keep merge file untouched in MPI mode; it only contains Merge statements.
    oops::Log::debug() << "GeometryMOM6::saveGmsh: rewritten per-rank NodeData files"
                       << std::endl;
  }
  comm.barrier();

  oops::Log::info() << "GeometryMOM6::saveGmsh: rank " << comm.rank()
                    << " -> " << filename << std::endl;
}

// ---------------------------------------------------------------------------
void GeometryMOM6::print(std::ostream & os) const
{
  const int    npes     = layoutX_ * layoutY_;
  const double avg      = static_cast<double>(nActiveGlobal_) / npes;
  const double imbalPct = (ownedMax_ > 0)
                          ? 100.0 * (ownedMax_ - ownedMin_) / avg
                          : 0.0;
  os << "\n"
     << "  +----- MOM6 Geometry ------------------------------------------+\n"
     << "  |  Grid      : " << niEff_ << " x " << njEff_ << " x " << numLevels_
                            << "  (NI x NJ x NZ)"
                            << (coarsenFactor_ > 1
                                ? "  [coarsen_factor="
                                  + std::to_string(coarsenFactor_) + "]"
                                : "") << "\n"
     << "  |  Tripolar fold: " << (hasFold_ ? "yes" : "no") << "\n"
     << "  |  MOM6 layout : " << layoutX_ << " x " << layoutY_
                              << "  (" << npes << " MPI tasks)\n"
     << "  |  Min depth   : " << minimumDepth_ << " m\n"
    << "  |  Min layer h : " << minimumThickness_ << " m\n"
    << "  |  Fringe width: " << fringeWidth_
              << " land layer"
              << (fringeWidth_ == 1 ? "" : "s") << "\n"
     << "  |  ---- JEDI unstructured partition (equal_regions) ----------\n"
     << "  |  Active pts  : " << nActiveGlobal_
                              << "  (ocean + land fringe)\n"
     << "  |  Land removed: " << (niEff_ * njEff_ - nActiveGlobal_)
                              << "  (" << std::fixed << std::setprecision(1)
                              << 100.0 * (niEff_ * njEff_ - nActiveGlobal_)
                                       / (niEff_ * njEff_)
                              << "% of full grid)\n"
     << "  |  Per rank    : avg " << static_cast<int>(avg)
                                  << "  min " << ownedMin_
                                  << "  max " << ownedMax_
                                  << "  imbalance "
                                  << std::fixed << std::setprecision(1)
                                  << imbalPct << "%\n"
     << "  |  This rank   : " << ownedCount_ << " owned pts\n"
     << "  +--------------------------------------------------------------+\n";
}

// ---------------------------------------------------------------------------
std::vector<double> GeometryMOM6::verticalCoord(std::string &) const
{
  // Not implemented, abort --- IGNORE ---
  std::stringstream errorMsg;
  errorMsg << "GeometryMOM6::verticalCoord is not implemented" << std::endl;
  ABORT(errorMsg.str());
  return {};
}

// ---------------------------------------------------------------------------
void GeometryMOM6::addModelVaderIngredients(const atlas::FieldSet & geomFields,
                                            atlas::FieldSet & fset, int nlevels) const
{
  // Ingredients for the SeaWaterTemperature_A/_B recipes:
  //  - latitude/longitude, from the fieldset's own function space;
  //  - sea_area_fraction, indexed per level by the _B Jacobian, so broadcast
  //    the 2D land/sea mask across all levels.
  addLonLatIngredients(fset);
  addBroadcastIngredient(geomFields, "mask2d", "sea_area_fraction", nlevels, fset);
}

// ---------------------------------------------------------------------------
}  // namespace ijedi
