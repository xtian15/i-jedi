#!/usr/bin/env python3
"""Validate the installed model-owned MPAS geometry contract on a real grid."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any

import netCDF4 as nc
import numpy as np

from direct_two_step import verify_installed_wheel


def tensor_manifest(value: Any) -> dict[str, Any]:
    import torch

    contiguous = value.detach().cpu().contiguous()
    return {
        "dtype": str(contiguous.dtype),
        "shape": list(contiguous.shape),
        "sha256": hashlib.sha256(contiguous.numpy().tobytes(order="C")).hexdigest(),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--namelist", type=Path, required=True)
    parser.add_argument("--direct-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)

    import torch
    from mpas_pytorch import load_config_from_namelist, load_initial_state
    from mpas_pytorch.ijedi_contracts import (
        ComposedGeometrySnapshot,
        HorizontalGeometrySnapshot,
        StaticVerticalGeometrySnapshot,
        build_configuration_receipt,
        load_ijedi_geometry_snapshot,
        validate_geometry_topology,
        validate_static_vertical_geometry,
    )

    config = load_config_from_namelist(args.namelist)
    configuration_receipt = build_configuration_receipt(args.namelist, config)
    snapshot = load_ijedi_geometry_snapshot(
        args.init, args.grid, configuration_receipt=configuration_receipt
    )
    snapshot.validate()
    topology = validate_geometry_topology(snapshot.horizontal)
    vertical_report = validate_static_vertical_geometry(snapshot.static_vertical)
    fields = snapshot.export_fields()
    counts = snapshot.horizontal.metadata["counts"]
    expected_counts = {
        "nCells": 10242,
        "nEdges": 30720,
        "nVertices": 20480,
        "nVertLevels": 55,
    }
    if counts != expected_counts:
        raise AssertionError(f"real MPAS counts differ: {counts} != {expected_counts}")
    if counts["nCells"] - counts["nEdges"] + counts["nVertices"] != 2:
        raise AssertionError("real MPAS topology violates Euler characteristic")
    pentagons = int(torch.count_nonzero(fields["nEdgesOnCell"] == 5).item())
    if pentagons != 12:
        raise AssertionError(f"real MPAS grid contains {pentagons} pentagons")

    exact_grid_fields = (
        "indexToCellID",
        "indexToEdgeID",
        "indexToVertexID",
        "latCell",
        "lonCell",
        "xCell",
        "yCell",
        "zCell",
        "latVertex",
        "lonVertex",
        "xVertex",
        "yVertex",
        "zVertex",
        "areaCell",
    )
    with nc.Dataset(args.grid, "r") as grid, nc.Dataset(args.init, "r") as init:
        for name in exact_grid_fields:
            expected = torch.from_numpy(np.asarray(grid.variables[name][:]).copy()).to(
                fields[name].dtype
            )
            if not torch.equal(fields[name], expected):
                raise AssertionError(f"canonical {name} is not elementwise grid.nc data")
        init_latitude = torch.from_numpy(
            np.asarray(init.variables["latCell"][:]).copy()
        ).to(torch.float64)
        if torch.equal(fields["latCell"], init_latitude):
            raise AssertionError("conflicting init.nc latitude was accepted as canonical")

    radius = torch.linalg.vector_norm(
        torch.stack((fields["xCell"], fields["yCell"], fields["zCell"]), dim=1),
        dim=1,
    ).mean()
    normalized_area = float(torch.sum(fields["areaCell"]) / (radius * radius))
    area_relative_error = abs(normalized_area - 4.0 * math.pi) / (4.0 * math.pi)
    if area_relative_error > 2.0e-14:
        raise AssertionError(f"normalized MPAS area error is {area_relative_error:.17g}")

    zgrid = fields["zgrid"]
    zz = fields["zz"]
    if tuple(zgrid.shape) != (10242, 56) or tuple(zz.shape) != (10242, 55):
        raise AssertionError("real MPAS static vertical extents differ")
    if not torch.isfinite(zgrid).all() or not torch.isfinite(zz).all():
        raise AssertionError("real MPAS static vertical geometry is nonfinite")
    if not torch.all(zgrid[:, 1:] > zgrid[:, :-1]) or not torch.all(zz > 0.0):
        raise AssertionError("real MPAS static vertical direction/metric is invalid")

    # Runtime tensors carry one padding row, while the DA geometry contract is
    # deliberately unpadded.  Bind the active runtime rows elementwise to the
    # canonical snapshot, then require the complete padded runtime tensors to
    # retain the same manifests across both authenticated model steps.
    initial_state, _ = load_initial_state(
        args.init, args.grid, mesh_support_path=None, config=config
    )
    runtime_static_manifests: dict[str, dict[str, Any]] = {}
    for name, canonical in (("zgrid", zgrid), ("zz", zz)):
        runtime = initial_state.get(name)
        if not isinstance(runtime, torch.Tensor):
            raise AssertionError(f"runtime initial state lacks tensor {name}")
        if runtime.shape[0] != counts["nCells"] + 1:
            raise AssertionError(
                f"runtime {name} padding extent is {runtime.shape[0]}, expected "
                f"{counts['nCells'] + 1}"
            )
        if not torch.equal(runtime[: counts["nCells"]], canonical):
            raise AssertionError(
                f"runtime active {name} rows differ from canonical static geometry"
            )
        runtime_static_manifests[name] = tensor_manifest(runtime)

    direct = json.loads(args.direct_manifest.read_text(encoding="utf-8"))
    for step in ("typed_transform_step1", "typed_transform_step2"):
        inputs = direct[step]["trajectory_receipt"]["inputs"]
        for name, expected in runtime_static_manifests.items():
            if inputs[name] != expected:
                raise AssertionError(f"{step} changed static runtime {name}")

    # Receipt-scope attacks operate on immutable copies; no source file is
    # rewritten.  Configuration and zgrid changes must retain the horizontal
    # identity, while a horizontal coordinate change must invalidate it.
    configuration_changed = ComposedGeometrySnapshot(
        snapshot.horizontal,
        snapshot.static_vertical,
        configuration_receipt=("0" if configuration_receipt[0] != "0" else "1")
        + configuration_receipt[1:],
    )
    if (
        configuration_changed.horizontal_receipt != snapshot.horizontal_receipt
        or configuration_changed.static_vertical_receipt
        != snapshot.static_vertical_receipt
        or configuration_changed.receipt == snapshot.receipt
    ):
        raise AssertionError("configuration-only receipt invalidation has the wrong scope")

    vertical_fields = snapshot.static_vertical.export_fields()
    vertical_fields["zgrid"] = vertical_fields["zgrid"].clone()
    vertical_fields["zgrid"][0, 0] = torch.nextafter(
        vertical_fields["zgrid"][0, 0],
        torch.tensor(torch.inf, dtype=vertical_fields["zgrid"].dtype),
    )
    changed_vertical = StaticVerticalGeometrySnapshot(
        vertical_fields, dict(snapshot.static_vertical.metadata)
    )
    vertical_changed = ComposedGeometrySnapshot(
        snapshot.horizontal,
        changed_vertical,
        configuration_receipt=configuration_receipt,
    )
    if (
        vertical_changed.horizontal_receipt != snapshot.horizontal_receipt
        or vertical_changed.static_vertical_receipt
        == snapshot.static_vertical_receipt
        or vertical_changed.receipt == snapshot.receipt
    ):
        raise AssertionError("zgrid-only receipt invalidation has the wrong scope")

    horizontal_fields = snapshot.horizontal.export_fields()
    horizontal_fields["lonCell"] = horizontal_fields["lonCell"].clone()
    horizontal_fields["lonCell"][0] = torch.nextafter(
        horizontal_fields["lonCell"][0],
        torch.tensor(torch.inf, dtype=horizontal_fields["lonCell"].dtype),
    )
    changed_horizontal = HorizontalGeometrySnapshot(
        horizontal_fields, dict(snapshot.horizontal.metadata)
    )
    horizontal_changed = ComposedGeometrySnapshot(
        changed_horizontal,
        snapshot.static_vertical,
        configuration_receipt=configuration_receipt,
    )
    if (
        horizontal_changed.horizontal_receipt == snapshot.horizontal_receipt
        or horizontal_changed.static_vertical_receipt
        != snapshot.static_vertical_receipt
        or horizontal_changed.receipt == snapshot.receipt
    ):
        raise AssertionError("horizontal-coordinate invalidation has the wrong scope")

    report = {
        "schema_version": 1,
        "counts": counts,
        "euler_characteristic": 2,
        "pentagon_count": pentagons,
        "canonical_grid_fields_checked": len(exact_grid_fields),
        "normalized_area": normalized_area,
        "normalized_area_relative_error": area_relative_error,
        "horizontal_geometry_receipt": snapshot.horizontal_receipt,
        "static_vertical_geometry_receipt": snapshot.static_vertical_receipt,
        "geometry_bundle_receipt": snapshot.receipt,
        "configuration_receipt": configuration_receipt,
        "canonical_zgrid_manifest": tensor_manifest(zgrid),
        "canonical_zz_manifest": tensor_manifest(zz),
        "runtime_static_manifests": runtime_static_manifests,
        "runtime_active_rows_match_canonical": True,
        "topology": topology,
        "static_vertical": vertical_report,
        "init_horizontal_override_rejected": True,
        "receipt_scope_attacks_passed": 3,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    temporary.replace(args.output)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
