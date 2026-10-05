#!/usr/bin/env python3
"""Installed-wheel equations and independent replay of authenticated Atlas weights.

Replay is a test oracle only. Production execution remains exclusively Atlas.
Vector coefficients are checked independently by Cartesian parallel transport,
not by another bearing implementation or separate geometry compiler.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any

import numpy as np

from direct_two_step import clone_state, verify_installed_wheel


FIELDS = (
    "air_pressure",
    "air_pressure_levels",
    "air_pressure_at_surface",
    "air_temperature",
    "dry_air_density",
    "water_vapor_mixing_ratio_wrt_dry_air",
    "water_vapor_mixing_ratio_wrt_moist_air",
    "eastward_wind",
    "northward_wind",
    "height_above_mean_sea_level",
    "height_above_mean_sea_level_levels",
    "height_above_mean_sea_level_at_surface",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--namelist", type=Path, required=True)
    parser.add_argument("--atlas-cache", type=Path, required=True)
    parser.add_argument("--atlas-metrics", type=Path, required=True)
    parser.add_argument("--atlas-compiler-identity", required=True)
    parser.add_argument("--locations", type=Path, required=True)
    parser.add_argument("--output-values", type=Path, required=True)
    parser.add_argument("--output-manifest", type=Path, required=True)
    parser.add_argument("--runtime-receipt", type=Path,
                        default=Path(__file__).resolve().parents[2] / "tools/mpas_runtime/runtime_receipt.json")
    return parser.parse_args()


def coalesced_csr(
    factors: np.ndarray, indices: np.ndarray, source_size: int, destination_size: int
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    entries = sorted(
        (
            int(indices[index, 1] - 1),
            int(indices[index, 0] - 1),
            float(factors[index]),
            index,
        )
        for index in range(factors.size)
    )
    rows: list[list[tuple[int, float]]] = [[] for _ in range(destination_size)]
    for row, column, value, _ in entries:
        if not (0 <= row < destination_size and 0 <= column < source_size):
            raise RuntimeError("factor index is outside its declared extent")
        if rows[row] and rows[row][-1][0] == column:
            combined = rows[row][-1][1] + value
            if not np.isfinite(combined):
                raise RuntimeError("coalesced factor is nonfinite")
            rows[row][-1] = (column, combined)
        else:
            rows[row].append((column, value))
    if any(not row for row in rows):
        raise RuntimeError("factor table has an uncovered target")
    offsets = np.zeros(destination_size + 1, dtype=np.int64)
    columns: list[int] = []
    values: list[float] = []
    for row_index, row in enumerate(rows):
        columns.extend(column for column, _ in row)
        values.extend(value for _, value in row)
        offsets[row_index + 1] = len(columns)
    return offsets, np.asarray(columns, dtype=np.int64), np.asarray(values, dtype=np.float64)


def apply_csr(
    source: np.ndarray, offsets: np.ndarray, columns: np.ndarray, weights: np.ndarray
) -> np.ndarray:
    if source.ndim == 1:
        source = source[:, None]
    destination = np.zeros((offsets.size - 1, source.shape[1]), dtype=np.float64)
    # Independent test replay preserves the producer's row/entry order.
    for row in range(destination.shape[0]):
        for entry in range(int(offsets[row]), int(offsets[row + 1])):
            column = int(columns[entry])
            weight = float(weights[entry])
            # Independent levels have no reduction between them. Vectorize
            # their separate binary64 products/additions while retaining the
            # exact row/entry accumulation order authenticated by the cache.
            destination[row] += weight * source[column]
    return destination


def basis(latitude: np.ndarray, longitude: np.ndarray):
    sin_lat, cos_lat = np.sin(latitude), np.cos(latitude)
    sin_lon, cos_lon = np.sin(longitude), np.cos(longitude)
    radial = np.stack((cos_lat * cos_lon, cos_lat * sin_lon, sin_lat), axis=-1)
    east = np.stack((-sin_lon, cos_lon, np.zeros_like(latitude)), axis=-1)
    north = np.stack((-sin_lat * cos_lon, -sin_lat * sin_lon, cos_lat), axis=-1)
    return radial, east, north


def load_atlas_cache(args, geometry, latitudes, longitudes):
    document = json.loads(args.atlas_cache.read_text())
    metrics = json.loads(args.atlas_metrics.read_text())
    counts = geometry.horizontal.metadata["counts"]
    source_size = int(counts["nCells"])
    target_size = len(latitudes)
    if set(document) != {"schema", "key", "rows", "columns", "entries", "vector_entries"}:
        raise RuntimeError("Atlas cache schema has unknown or missing members")
    if (document["schema"] != "atlas-point-cache-v3" or
            document["rows"] != target_size or document["columns"] != source_size or
            metrics["atlas_compiler_identity"] != args.atlas_compiler_identity):
        raise RuntimeError("Atlas cache/compiler dimensions do not match the owned inputs")
    # Independently bind the actual native-order model measures. Point targets
    # use the identity metric; observational R weighting is outside the supported interfaces.
    metric = bytearray()
    for value in ("mpas-native-cell-area-per-index-level-v1", geometry.horizontal_receipt):
        value = value.encode()
        metric.extend(struct.pack("<Q", len(value))); metric.extend(value)
    metric.extend(struct.pack("<Q", source_size))
    areas = geometry.field("areaCell").numpy()
    if areas.shape != (source_size,) or not np.all(np.isfinite(areas)) or np.any(areas <= 0):
        raise RuntimeError("owned point source measures are invalid")
    metric.extend(np.asarray(areas, dtype="<f8").tobytes())
    measure_receipt = hashlib.sha256(metric).hexdigest()
    encoded = bytearray()
    def integer(value):
        encoded.extend(struct.pack("<Q", value))
    def text(value):
        value = value.encode(); integer(len(value)); encoded.extend(value)
    text("ijedi-atlas-mpas-dual-finite-element-vector-cell-area-obs-identity-v3")
    text(geometry.horizontal_receipt); text(args.atlas_compiler_identity)
    # Independently encode the exact adapter input, including coordinates,
    # orientation, connectivity and IDs. A quoted receipt cannot authorize a
    # substituted source geometry that remains geometrically valid.
    from validate_atlas_topology import export_atlas_snapshot
    snapshot = export_atlas_snapshot(geometry)
    content = bytearray()
    def content_integer(value):
        content.extend(struct.pack("<Q", value))
    def content_text(value):
        value = value.encode(); content_integer(len(value)); content.extend(value)
    content_text("mpas-atlas-native-geometry-inputs-radians-int64-float64-v2")
    content_text(geometry.horizontal_receipt)
    content.extend(struct.pack("<d", snapshot["sphere_radius_metres"]))
    for name in ("angle_units", "length_units", "area_units"):
        content_text(snapshot[name])
    for count in (snapshot["cells"], snapshot["edges"], snapshot["vertices"], snapshot["cell_width"]):
        content_integer(count)
    for group, format in (("integers", "<q"), ("reals", "<d")):
        content_integer(len(snapshot[group]))
        for name, values in sorted(snapshot[group].items()):
            content_text(name); content_integer(len(values))
            for value in values:
                content.extend(struct.pack(format, value))
    text(hashlib.sha256(content).hexdigest())
    text(measure_receipt)
    integer(source_size); integer(target_size)
    for lon, lat in zip(longitudes, latitudes, strict=True):
        encoded.extend(struct.pack("<dd", lon, lat))
    key = hashlib.sha256(encoded).hexdigest()
    if document["key"] != key or metrics["atlas_cache_key"] != key:
        raise RuntimeError("Atlas cache full ordered-target key differs from the independent owner key")
    encoded.clear(); text("atlas-point-cache-v3"); text(key)
    integer(source_size); integer(target_size); integer(len(document["entries"]))
    for row, col, weight in document["entries"]:
        integer(row); integer(col); encoded.extend(struct.pack("<d", weight))
    integer(len(document["vector_entries"]))
    for row, col, real, imaginary in document["vector_entries"]:
        integer(row); integer(col); encoded.extend(struct.pack("<dd", real, imaginary))
    if hashlib.sha256(encoded).hexdigest() != metrics["atlas_cache_receipt"]:
        raise RuntimeError("Atlas scalar/effective-vector coefficients differ from the owner's receipt")
    entries = document["entries"]
    vector = document["vector_entries"]
    weights_by_pair = {(row, col): weight for row, col, weight in entries}
    if len(weights_by_pair) != len(entries) or len(vector) != len(entries):
        raise RuntimeError("Atlas cache has duplicate or inconsistent support")
    sr, se, sn = basis(geometry.field("latCell").numpy(), geometry.field("lonCell").numpy())
    tr, te, tn = basis(np.deg2rad(latitudes), np.deg2rad(longitudes))
    maximum_error = 0.
    wrong_basis_separated = False
    for row, col, real, imaginary in vector:
        axis = np.cross(sr[col], tr[row])
        cosine = float(np.dot(sr[col], tr[row]))
        if cosine <= -1 + 1.e-12:
            raise RuntimeError("point support contains ambiguous antipodal transport")
        transported_east = se[col] + np.cross(axis, se[col]) + np.cross(axis, np.cross(axis, se[col])) / (1 + cosine)
        weight = weights_by_pair[row, col]
        reference = weight * np.array([np.dot(te[row], transported_east), np.dot(tn[row], transported_east)])
        np.testing.assert_allclose([real, imaginary], reference, rtol=0., atol=2.e-14,
                                   err_msg="independent Cartesian parallel transport")
        maximum_error = max(maximum_error, float(np.max(np.abs([real, imaginary] - reference))))
        wrong_basis_separated |= abs(real - weight) + abs(imaginary) > 1.e-4
    if not wrong_basis_separated:
        raise RuntimeError("scalar-wind negative control failed to separate at seam/pole targets")
    offsets = np.zeros(target_size + 1, dtype=np.int64)
    for row, col, weight in entries:
        if not (0 <= row < target_size and 0 <= col < source_size) or not np.isfinite(weight):
            raise RuntimeError("Atlas cache contains invalid scalar support")
        offsets[row + 1] += 1
    np.cumsum(offsets, out=offsets)
    if np.any(np.diff(offsets) == 0):
        raise RuntimeError("Atlas cache does not cover all targets")
    columns = np.asarray([entry[1] for entry in entries], dtype=np.int64)
    weights = np.asarray([entry[2] for entry in entries], dtype=np.float64)
    return offsets, columns, weights, vector, maximum_error


def apply_vector_cache(east, north, entries, destination_size):
    result_east = np.zeros((destination_size, east.shape[1]), dtype=np.float64)
    result_north = np.zeros_like(result_east)
    for row, col, real, imaginary in entries:
        # Explicit binary64 products keep independent replay's accumulation
        # order equal to the authenticated complex Atlas runtime coefficients.
        result_east[row] += real * east[col] - imaginary * north[col]
        result_north[row] += imaginary * east[col] + real * north[col]
    return result_east, result_north


def independent_numpy_geovals(
    state: dict[str, Any], *, n_cells: int, n_edges: int, geometry
) -> dict[str, np.ndarray]:
    """Evaluate the 12 supported columns without calling a package transform."""

    def array(name: str) -> np.ndarray:
        value = state[name]
        return value.detach().cpu().contiguous().numpy()

    theta_m = array("theta_m")[:n_cells]
    scalars = array("scalars")[:n_cells]
    rho_zz = array("rho_zz")[:n_cells]
    zz = array("zz")[:n_cells]
    zgrid = array("zgrid")[:n_cells]
    # Native analysis coordinates determine EOS pressure/Exner. Independent
    # cached pressure or Exner seeds would describe a different derivative.
    gas_constant, reference_pressure, heat_capacity = 287.0, 100000.0, 1004.5
    exner = (gas_constant / reference_pressure * rho_zz * zz * theta_m) ** (
        gas_constant / (heat_capacity - gas_constant))
    pressure = reference_pressure * exner ** (heat_capacity / gas_constant)
    qv = scalars[:, :, 0]

    # Reconstruct Cartesian cell wind with an explicit six-slot MPAS loop.
    # This intentionally shares no vector-reconstruction code with the wheel.
    edges_on_cell = array("edgesOnCell")[:n_cells].astype(np.int64, copy=False)
    n_edges_on_cell = array("nEdgesOnCell")[:n_cells].astype(np.int64, copy=False)
    coefficients = array("coeffs_reconstruct")[:n_cells]
    edge_wind = array("u")[:n_edges]
    xyz = np.zeros((n_cells, edge_wind.shape[1], 3), dtype=np.float64)
    for slot in range(edges_on_cell.shape[1]):
        active = slot < n_edges_on_cell
        edge_index = edges_on_cell[active, slot] - 1
        if np.any(edge_index < 0) or np.any(edge_index >= n_edges):
            raise RuntimeError("independent wind oracle found an invalid active edge")
        xyz[active] += (
            edge_wind[edge_index, :, None]
            * coefficients[active, slot, None, :]
        )
    # Atlas transport acts on the canonical model-snapshot basis, not the
    # separately authenticated native-runtime basis retained by the dynamics.
    latitude = geometry.field("latCell").numpy()
    longitude = geometry.field("lonCell").numpy()
    east = (
        -xyz[:, :, 0] * np.sin(longitude)[:, None]
        + xyz[:, :, 1] * np.cos(longitude)[:, None]
    )
    north = (
        -(
            xyz[:, :, 0] * np.cos(longitude)[:, None]
            + xyz[:, :, 1] * np.sin(longitude)[:, None]
        )
        * np.sin(latitude)[:, None]
        + xyz[:, :, 2] * np.cos(latitude)[:, None]
    )

    dz0 = zgrid[:, 1] - zgrid[:, 0]
    dz1 = zgrid[:, 2] - zgrid[:, 1]
    rho0 = rho_zz[:, 0] * zz[:, 0] * (1.0 + qv[:, 0])
    rho1 = rho_zz[:, 1] * zz[:, 1] * (1.0 + qv[:, 1])
    surface_pressure = (
        0.5
        * 9.80616
        * dz0
        * (rho0 - 0.5 * (rho1 - rho0) * dz0 / (dz0 + dz1))
        + pressure[:, 0]
    )
    log_pressure = np.log(pressure)
    interior_weight = (zgrid[:, 1:-1] - zgrid[:, :-2]) / (
        zgrid[:, 2:] - zgrid[:, :-2]
    )
    interior_pressure = np.exp(
        interior_weight * log_pressure[:, 1:]
        + (1.0 - interior_weight) * log_pressure[:, :-1]
    )
    top_weight = (zgrid[:, -1] - 0.5 * (zgrid[:, -2] + zgrid[:, -3])) / (
        0.5 * (zgrid[:, -1] + zgrid[:, -2])
        - 0.5 * (zgrid[:, -2] + zgrid[:, -3])
    )
    top_pressure = np.exp(
        top_weight * log_pressure[:, -1]
        + (1.0 - top_weight) * log_pressure[:, -2]
    )
    pressure_levels = np.concatenate(
        (surface_pressure[:, None], interior_pressure, top_pressure[:, None]), axis=1
    )

    return {
        "air_pressure": pressure,
        "air_pressure_levels": pressure_levels,
        "air_pressure_at_surface": surface_pressure,
        "air_temperature": theta_m / (1.0 + 1.6083623693379792 * qv) * exner,
        "dry_air_density": rho_zz * zz,
        "water_vapor_mixing_ratio_wrt_dry_air": qv,
        "water_vapor_mixing_ratio_wrt_moist_air": qv / (1.0 + qv),
        "eastward_wind": east,
        "northward_wind": north,
        "height_above_mean_sea_level": 0.5 * (zgrid[:, :-1] + zgrid[:, 1:]),
        "height_above_mean_sea_level_levels": zgrid,
        "height_above_mean_sea_level_at_surface": zgrid[:, 0],
    }


def main() -> int:
    args = parse_args()
    # A failed producer must not leave a prior successful oracle available to
    # the downstream comparison test.
    for path in (
        args.output_values,
        args.output_manifest,
        args.output_values.with_name(args.output_values.name + ".tmp"),
        args.output_manifest.with_name(args.output_manifest.name + ".tmp"),
    ):
        path.unlink(missing_ok=True)
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)

    import netCDF4 as nc
    import torch
    from mpas_pytorch import (
        __version__,
        load_config_from_namelist,
        load_initial_state,
        prepare_initial_state,
        run_simulation,
    )
    from mpas_pytorch.assimilation_variables import (
        build_transform_trajectory_receipt,
    )
    from mpas_pytorch.analysis_coordinates import diagnose_analysis_geovals
    from mpas_pytorch.ijedi_contracts import (
        build_configuration_receipt,
        build_state_storage_schema,
        compose_continuation_state,
        extract_continuation_boundary,
        load_ijedi_geometry_snapshot,
    )

    from runtime_identity import verify_runtime
    runtime_identity = verify_runtime(args.runtime_receipt)
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    config = load_config_from_namelist(args.namelist)
    initial, mesh_support = load_initial_state(
        args.init, args.grid, mesh_support_path=None, config=config
    )
    configuration_receipt = build_configuration_receipt(args.namelist, config)
    geometry = load_ijedi_geometry_snapshot(
        args.init, args.grid, configuration_receipt=configuration_receipt
    )
    reference = run_simulation(
        clone_state(initial, torch), mesh_support, nsteps=1, config=config
    )
    schema = build_state_storage_schema(
        reference,
        mesh_support,
        horizontal_geometry_receipt=geometry.horizontal_receipt,
        static_vertical_geometry_receipt=geometry.static_vertical_receipt,
        bundle_receipt=geometry.receipt,
        configuration_receipt=configuration_receipt,
    )
    prepared = prepare_initial_state(
        clone_state(initial, torch), clone_state(mesh_support, torch), config=config
    )
    states: list[dict[str, Any]] = [prepared]
    boundary = extract_continuation_boundary(prepared, schema=schema)
    for _ in range(2):
        step_input = compose_continuation_state(reference, boundary, schema=schema)
        completed = run_simulation(step_input, mesh_support, nsteps=1, config=config)
        states.append(completed)
        boundary = extract_continuation_boundary(completed, schema=schema)

    location_payload = json.loads(args.locations.read_text(encoding="utf-8"))
    latitudes = np.asarray(location_payload["latitude_degrees"], dtype=np.float64)
    longitudes = np.asarray(location_payload["longitude_degrees"], dtype=np.float64)
    source_size = int(geometry.horizontal.metadata["counts"]["nCells"])
    n_edges = int(geometry.metadata["counts"]["nEdges"])
    offsets, columns, weights, vector_entries, vector_basis_error = load_atlas_cache(
        args, geometry, latitudes, longitudes)

    package_identity = {
        "version": __version__,
        "source_commit": args.source_commit,
        "wheel_sha256": args.wheel_sha256,
    }
    valid_times = (
        "2026-07-17T06:00:00Z",
        "2026-07-17T06:12:00Z",
        "2026-07-17T06:24:00Z",
    )
    output: list[np.ndarray] = []
    field_shapes: dict[str, list[int]] = {}
    independent_values_checked = 0
    independent_maximum_absolute_difference = 0.0
    independent_maximum_relative_difference = 0.0
    wrong_moisture_denominator_rejected = False
    for generation, (state, valid_time) in enumerate(zip(states, valid_times, strict=True)):
        receipt = build_transform_trajectory_receipt(
            state,
            horizontal_geometry_receipt=geometry.horizontal_receipt,
            static_vertical_geometry_receipt=geometry.static_vertical_receipt,
            bundle_receipt=geometry.receipt,
            state_schema_digest=schema.digest,
            configuration_receipt=configuration_receipt,
            valid_time=valid_time,
            state_generation=generation,
            package_identity=package_identity,
        )
        fields = diagnose_analysis_geovals(
            state, mesh_support, schema=schema, config=config,
            static_support=reference, namelist_path=args.namelist, geometry=geometry,
            requested_fields=FIELDS,
            trajectory_receipt=receipt,
            expected_valid_time=valid_time,
            expected_state_generation=generation,
            expected_bindings={
                "horizontal_geometry_receipt": geometry.horizontal_receipt,
                "static_vertical_geometry_receipt": geometry.static_vertical_receipt,
                "bundle_receipt": geometry.receipt,
                "state_schema_digest": schema.digest,
                "configuration_receipt": configuration_receipt,
                "package_identity": package_identity,
            },
        )
        independent = independent_numpy_geovals(
            state, n_cells=source_size, n_edges=n_edges, geometry=geometry
        )
        if set(independent) != set(FIELDS) or set(fields) != set(FIELDS):
            raise RuntimeError("typed transform inventory differs from the independent oracle")
        wind_values = {name: fields[name].detach().cpu().contiguous().numpy()
                       for name in ("eastward_wind", "northward_wind")}
        wind_result = dict(zip(wind_values, apply_vector_cache(
            wind_values["eastward_wind"], wind_values["northward_wind"],
                vector_entries, latitudes.size), strict=True))
        for name in FIELDS:
            values = fields[name].detach().cpu().contiguous().numpy()
            expected = independent[name]
            if values.shape != expected.shape:
                raise RuntimeError(
                    f"independent typed shape mismatch for {name}: "
                    f"{values.shape} != {expected.shape}"
                )
            np.testing.assert_allclose(
                values,
                expected,
                rtol=2.0e-14,
                atol=2.0e-12,
                err_msg=f"independent {valid_time}/{name}",
            )
            difference = np.abs(values - expected)
            scale = np.maximum(np.abs(values), np.abs(expected))
            relative = np.divide(
                difference,
                scale,
                out=np.zeros_like(difference),
                where=scale > 0.0,
            )
            independent_values_checked += int(values.size)
            independent_maximum_absolute_difference = max(
                independent_maximum_absolute_difference, float(np.max(difference))
            )
            independent_maximum_relative_difference = max(
                independent_maximum_relative_difference, float(np.max(relative))
            )
            result = wind_result[name] if name in wind_result else apply_csr(values, offsets, columns, weights)
            output.append(result.reshape(-1))
            field_shapes[name] = list(result.shape)
        scalars = state["scalars"][:source_size].detach().cpu().numpy()
        qv = scalars[:, :, 0]
        # A wrong-sign denominator separates even on a condensate-free case.
        # Nonzero-condensate naming/dependency attacks are also mandatory in
        # the installed model semantic suite.
        wrong_specific_humidity = qv / (1.0 - np.sum(scalars, axis=2))
        correct_specific_humidity = independent[
            "water_vapor_mixing_ratio_wrt_moist_air"
        ]
        if not np.allclose(
            wrong_specific_humidity,
            correct_specific_humidity,
            rtol=2.0e-14,
            atol=2.0e-12,
        ):
            wrong_moisture_denominator_rejected = True
    if not wrong_moisture_denominator_rejected:
        raise RuntimeError("wrong moisture-denominator negative control did not separate")
    joined = np.concatenate(output).astype("<f8", copy=False)
    args.output_values.parent.mkdir(parents=True, exist_ok=True)
    temporary_values = args.output_values.with_name(args.output_values.name + ".tmp")
    joined.tofile(temporary_values)
    temporary_values.replace(args.output_values)
    manifest = {
        "schema_version": 1,
        "runtime_receipt_sha256": runtime_identity["runtime_receipt_sha256"],
        "states": 3,
        "field_order": list(FIELDS),
        "field_shapes": field_shapes,
        "target_count": int(latitudes.size),
        "source_size": source_size,
        "nonzeros": int(weights.size),
        "value_count": int(joined.size),
        "values_sha256": hashlib.sha256(joined.tobytes()).hexdigest(),
        "horizontal_geometry_receipt": geometry.horizontal_receipt,
        "state_schema_digest": schema.digest,
        "independent_numpy_values_checked": independent_values_checked,
        "independent_numpy_maximum_absolute_difference": (
            independent_maximum_absolute_difference
        ),
        "independent_numpy_maximum_relative_difference": (
            independent_maximum_relative_difference
        ),
        "independent_numpy_rtol": 2.0e-14,
        "independent_numpy_atol": 2.0e-12,
        "independent_numpy_equation_checks_passed": True,
        "independent_cartesian_vector_basis_checked": True,
        "independent_cartesian_vector_basis_max_error": vector_basis_error,
        "wrong_moisture_denominator_rejected": True,
    }
    if hashlib.sha256(args.runtime_receipt.read_bytes()).hexdigest() != runtime_identity["runtime_receipt_sha256"]:
        raise RuntimeError("runtime receipt changed during typed qualification")
    temporary_manifest = args.output_manifest.with_name(args.output_manifest.name + ".tmp")
    temporary_manifest.write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    temporary_manifest.replace(args.output_manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
