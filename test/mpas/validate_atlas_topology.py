#!/usr/bin/env python3
"""Compare native Atlas primitives with an independent raw grid.nc oracle.

Only the installed model decodes the production snapshot. netCDF reads here
are a test oracle, never an alternate geometry or model execution path.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import netCDF4
import numpy as np

from direct_two_step import verify_installed_wheel


def export_atlas_snapshot(snapshot):
    """Serialize the model-owned snapshot for the C++ test boundary only."""
    snapshot.validate()
    fields = snapshot.export_fields()
    counts = snapshot.horizontal.metadata["counts"]
    integers = ("indexToCellID", "indexToEdgeID", "indexToVertexID", "nEdgesOnCell",
                "verticesOnCell", "cellsOnVertex", "verticesOnEdge", "cellsOnEdge")
    reals = ("lonCell", "latCell", "lonVertex", "latVertex", "lonEdge", "latEdge",
             "areaCell", "edgeNormalVectors")
    source = snapshot.horizontal.metadata["source"]
    units = source["variable_units"]
    if (any(units[f"{prefix}{suffix}"] != units["lonCell"]
            for suffix in ("Cell", "Edge", "Vertex") for prefix in ("lon", "lat")) or
            any(units[f"{prefix}{suffix}"] != units["xCell"]
                for suffix in ("Cell", "Edge", "Vertex") for prefix in ("x", "y", "z"))):
        raise AssertionError("model snapshot declares inconsistent coordinate units")
    return {"cells": counts["nCells"], "edges": counts["nEdges"],
            "sphere_radius_metres": source["global_attributes"]["sphere_radius"],
            "angle_units": units["lonCell"], "length_units": units["xCell"],
            "area_units": units["areaCell"],
            "vertices": counts["nVertices"], "cell_width": fields["verticesOnCell"].shape[1],
            "receipt": snapshot.horizontal_receipt,
            "integers": {k: fields[k].reshape(-1).tolist() for k in integers},
            "reals": {k: fields[k].reshape(-1).tolist() for k in reals}}


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "init", "grid", "namelist", "executable", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--atlas-compiler-identity", required=True)
    parser.add_argument("--gate", choices=("topology", "cache-restore", "negative-controls"),
                        default="cache-restore")
    args = parser.parse_args()
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    from mpas_pytorch import load_config_from_namelist
    from mpas_pytorch.ijedi_contracts import load_ijedi_geometry_snapshot, build_configuration_receipt

    config = load_config_from_namelist(args.namelist)
    snapshot = load_ijedi_geometry_snapshot(
        args.init, args.grid,
        configuration_receipt=build_configuration_receipt(args.namelist, config))
    counts = snapshot.horizontal.metadata["counts"]
    payload = export_atlas_snapshot(snapshot)
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = Path(tempfile.mkdtemp(prefix=args.gate+"-", dir=args.output))
    source = args.output / "model-snapshot.json"
    destination = args.output / "atlas-topology.json"
    source.write_text(json.dumps(payload, allow_nan=False))
    if args.gate == "negative-controls":
        subprocess.run([str(args.executable), str(source), str(destination),
                        "--storage-negative-controls"], check=True)
        report = json.loads(destination.read_text())
        checks = report["atlas_handle_checks"]
        if (report["atlas_compiler_identity"] != args.atlas_compiler_identity or
                checks["storage_mutations"] != 13 or checks["rejections"] != 65 or
                checks["geometry_handle_mutations"] != 14 or checks["geometry_handle_rejections"] != 70 or
                checks["operator_storage_mutations"] != 36 or checks["operator_storage_rejections"] != 252 or
                checks["cache_capacity"] != 64 or
                checks["cache_bound_enforced_without_eviction"] is not True or
                checks["operator_survives_geometry"] is not True):
            raise RuntimeError("geometry negative controls lack the protected inventory/authority")
        verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
        print("65 retained + 70 + 252 storage/action attacks rejected; "
              "bounded cache and detached lifetime passed")
        return
    subprocess.run([str(args.executable), str(source), str(destination)], check=True)
    actual = json.loads(destination.read_text())
    if actual["atlas_compiler_identity"] != args.atlas_compiler_identity:
        raise RuntimeError("topology gate loaded a different Atlas source authority")
    trusted = {"point": actual["point_checks"]["receipt"],
               "identity": actual["conservative_identity"]["coefficient_receipt"],
               "forward": actual["conservative_rotated_forward"]["coefficient_receipt"],
               "reverse": actual["conservative_rotated_reverse"]["coefficient_receipt"],
               "analysis-forward": actual["conservative_analysis_forward"]["coefficient_receipt"],
               "analysis-reverse": actual["conservative_analysis_reverse"]["coefficient_receipt"],
               "physical-forward": actual["conservative_physical_radius_forward"]["coefficient_receipt"],
               "physical-reverse": actual["conservative_physical_radius_reverse"]["coefficient_receipt"]}
    fresh_destination = args.output / "atlas-topology-fresh-restore.json"
    if args.gate == "cache-restore":
        subprocess.run([str(args.executable), str(source), str(fresh_destination),
                        str(destination), json.dumps(trusted)], check=True)
        fresh = json.loads(fresh_destination.read_text())
        for manifest in (actual, fresh):
            manifest["point_checks"].pop("wall_seconds")
        if actual != fresh:
            raise AssertionError("fresh-process Atlas geometry/cache/results differ from the compiling process")
        for name in trusted:
            if Path(str(destination) + f".{name}.cache.json").read_bytes() != Path(str(fresh_destination) + f".{name}.cache.json").read_bytes():
                raise AssertionError(f"fresh-process {name} cache coefficients differ byte-for-byte")

    def equal(label, actual_values, expected):
        a, b = np.asarray(actual_values), np.asarray(expected)
        if a.shape != b.shape or not np.array_equal(a, b):
            raise AssertionError(f"{label} differs elementwise: {a.shape} vs {b.shape}")

    with netCDF4.Dataset(args.grid) as grid:
        raw = lambda k: np.asarray(grid.variables[k][:])
        c = np.asarray(actual["atlas_to_native_cells"])
        e = np.asarray(actual["atlas_to_native_edges"])
        equal("cell bijection", np.sort(c), np.arange(counts["nCells"]))
        equal("edge bijection", np.sort(e), np.arange(counts["nEdges"]))
        equal("cell inverse", np.asarray(actual["native_to_atlas_cells"])[c], np.arange(len(c)))
        equal("edge inverse", np.asarray(actual["native_to_atlas_edges"])[e], np.arange(len(e)))
        for key, suffix, order in (("primal", "Vertex", None), ("dual", "Cell", None)):
            equal(key + " node IDs", actual[key]["node_ids"], raw("indexTo" + suffix + "ID"))
            coordinates = np.column_stack((raw("lon" + suffix), raw("lat" + suffix)))
            equal(key + " coordinates", actual[key]["coordinates"], coordinates * (180.0 / np.pi))
        equal("primal cell IDs", actual["primal"]["cell_ids"], raw("indexToCellID")[c])
        equal("dual face IDs", actual["dual"]["cell_ids"], raw("indexToVertexID"))
        equal("dual triangles", actual["dual"]["faces"], raw("cellsOnVertex") - 1)
        vertices = raw("verticesOnCell") - 1
        edge_counts = raw("nEdgesOnCell")
        for row, native in zip(actual["primal"]["faces"], c):
            equal(f"primal face {native}", row, vertices[native, :edge_counts[native]])
        equal("declared measures", actual["cell_mean_measures"], raw("areaCell")[c])
        equal("edge IDs", actual["edge_ids"], raw("indexToEdgeID")[e])
        equal("edge endpoints", np.sort(actual["edge_vertices"], axis=1),
              np.sort(raw("verticesOnEdge")[e] - 1, axis=1))
        mapped_cells = c[np.asarray(actual["edge_cells"])]
        native_cells = raw("cellsOnEdge")[e] - 1
        equal("edge adjacent cells", np.sort(mapped_cells, axis=1), np.sort(native_cells, axis=1))
        equal("edge side orientation", actual["edge_cell_orientation"],
              np.where(mapped_cells[:, 0] == native_cells[:, 0], 1, -1))
        equal("cell mean coordinates", actual["cell_mean_coordinates"],
              np.column_stack((raw("lonCell")[c], raw("latCell")[c])) * (180 / np.pi))
        equal("edge coordinates", actual["edge_coordinates"],
              np.column_stack((raw("lonEdge")[e], raw("latEdge")[e])) * (180 / np.pi))
        xyz = np.column_stack([raw(key) for key in ("xCell", "yCell", "zCell")])
        chord = xyz[native_cells[:, 1]] - xyz[native_cells[:, 0]]
        independently_derived_normals = chord / np.sqrt(np.sum(chord * chord, axis=1))[:, None]
        np.testing.assert_allclose(actual["edge_canonical_normals"], independently_derived_normals,
                                   rtol=0, atol=2.e-14, err_msg="independent primitive edge normals")
        equal("space sizes", actual["space_sizes"],
              [counts["nCells"], counts["nCells"], counts["nEdges"], counts["nVertices"]])
        equal("pentagon/hexagon counts", [np.count_nonzero(edge_counts == 5),
                                         np.count_nonzero(edge_counts == 6)], [12, counts["nCells"] - 12])
    if actual["receipt"] != snapshot.horizontal_receipt or actual["negative_controls"] != 17:
        raise AssertionError("receipt or negative-control inventory differs")
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    print("All native Atlas primitives match grid.nc; seventeen negative controls passed; gate="+args.gate)


if __name__ == "__main__":
    main()
