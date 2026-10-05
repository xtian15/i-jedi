#!/usr/bin/env python3
"""Installed owner graph plus independent replay of the exact Atlas matrix.

This oracle contributes no production interpolation or model implementation.
It retains the physical-Riesz division/multiplication order of the OOPS carrier.
"""
import argparse
import gc
import hashlib
import importlib.metadata
import json
from pathlib import Path

import numpy as np
from direct_two_step import verify_installed_wheel
from direct_typed_interpolation import FIELDS, load_atlas_cache, apply_csr, apply_vector_cache


def manifest(arrays):
    result = {}
    for name, value in arrays.items():
        if hasattr(value, "detach"): value = value.detach().cpu().numpy()
        value = np.asarray(value, dtype="<f8")
        result[name] = dict(shape=list(value.shape), sha256=hashlib.sha256(value.tobytes(order="C")).hexdigest())
    return result


def scalar_transpose(seed, offsets, columns, weights, cells):
    result = np.zeros((cells, seed.shape[1]), dtype=np.float64)
    for row in range(seed.shape[0]):
        for entry in range(int(offsets[row]), int(offsets[row+1])):
            result[columns[entry]] += weights[entry]*seed[row]
    return result


def vector_transpose(east, north, entries, cells):
    gx = np.zeros((cells, east.shape[1]), dtype=np.float64)
    gy = np.zeros_like(gx)
    for row, col, real, imaginary in entries:
        gx[col] += real*east[row] + imaginary*north[row]
        gy[col] += -imaginary*east[row] + real*north[row]
    return gx, gy


def pull_control_covector(state, seeds, *, offsets, columns, weights, vector_entries,
                          masses, cells, graph, owner):
    """Independent stored-matrix transpose followed by the installed owner VJP/P*."""
    import torch
    from mpas_pytorch.analysis_coordinates import analysis_geoval_vjp
    from mpas_pytorch.analysis_spaces import owned_native_covectors_to_control
    covectors = {name: scalar_transpose(seeds[name], offsets, columns, weights, cells)
                 for name in FIELDS if name not in ("eastward_wind", "northward_wind")}
    covectors["eastward_wind"], covectors["northward_wind"] = vector_transpose(
        seeds["eastward_wind"], seeds["northward_wind"], vector_entries, cells)
    native_seeds = {}
    for name, value in covectors.items():
        mass = masses[name]
        if mass.ndim == 1: value = value[:, 0]
        # Preserve the OOPS physical-Riesz division and LVC multiplication.
        native_seeds[name] = torch.from_numpy(((value/mass)*mass).copy())
    _, native_gradient = analysis_geoval_vjp(state, owner["mesh"], native_seeds, **graph)
    native_gradient = {name: value[:-1] for name, value in native_gradient.items()}
    return owned_native_covectors_to_control(state, native_gradient, **owner["arguments"])


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "init", "grid", "namelist", "atlas-cache", "atlas-metrics", "locations", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    for name in ("wheel-sha256", "source-commit", "atlas-compiler-identity"):
        parser.add_argument("--"+name, required=True)
    args = parser.parse_args()
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    import torch
    from mpas_pytorch import load_initial_state, load_config_from_namelist, prepare_initial_state, run_simulation
    from mpas_pytorch.ijedi_contracts import build_configuration_receipt, load_ijedi_geometry_snapshot, build_state_storage_schema
    from mpas_pytorch.assimilation_variables import build_transform_trajectory_receipt
    from mpas_pytorch.analysis_coordinates import analysis_geoval_jvp, analysis_geoval_vjp
    from mpas_pytorch.analysis_spaces import build_analysis_inner_product_snapshot, owned_control_increment_to_native, owned_native_covectors_to_control
    torch.set_num_threads(1); torch.set_num_interop_threads(1)
    config = load_config_from_namelist(args.namelist)
    raw, mesh = load_initial_state(args.init, args.grid, config=config)
    geometry = load_ijedi_geometry_snapshot(args.init, args.grid,
        configuration_receipt=build_configuration_receipt(args.namelist, config))
    state = prepare_initial_state(raw, mesh, config=config)
    reference = run_simulation(raw, mesh, config=config, nsteps=1)
    bindings = dict(horizontal_geometry_receipt=geometry.horizontal_receipt,
        static_vertical_geometry_receipt=geometry.static_vertical_receipt,
        bundle_receipt=geometry.receipt, configuration_receipt=geometry.configuration_receipt)
    schema = build_state_storage_schema(reference, mesh, **bindings)
    support_names = {field.name for field in schema.fields if field.role in ("static_geometry", "static_support")}
    support_names.update(schema.support_metadata_keys)
    support = {name: reference[name] for name in support_names}
    del reference, raw; gc.collect()
    bindings.update(state_schema_digest=schema.digest, package_identity=dict(version=importlib.metadata.version("mpas-pytorch"),
        source_commit=args.source_commit, wheel_sha256=args.wheel_sha256))
    locations = json.loads(args.locations.read_text())
    latitudes, longitudes = locations["latitude_degrees"], locations["longitude_degrees"]
    offsets, columns, weights, vector_entries, basis_error = load_atlas_cache(args, geometry, latitudes, longitudes)
    cells, levels, targets = int(state["nCells"]), int(state["nVertLevels"]), len(latitudes)
    if (cells, levels, targets) != (10242, 55, 256):
        raise RuntimeError("OOPS TLAD oracle requires the exact preregistered real-grid inventory")
    metric_for = {name: ("cell_surface" if name.endswith("_at_surface") else
        ("cell_interface" if name in ("air_pressure_levels", "height_above_mean_sea_level_levels") else "cell_layer")) for name in FIELDS}
    control_names = ("eastward_wind", "northward_wind", "upward_air_velocity",
        "moist_potential_temperature", "jacobian_dry_air_density", "moisture_tracers")
    missing = float(np.float32(float(np.finfo(np.float32).min)*.98))
    def direction(shape, seed, family, exponent):
        index = torch.arange(int(np.prod(shape)), dtype=torch.int64)
        return ((((index+seed*11+family*7)%37).to(torch.float64)-18.)*(2.**-exponent)).reshape(shape)
    output = dict(scope="independent_installed_owner_plus_stored_Atlas_replay_not_model_time_adjoint",
        atlas_vector_cartesian_error=basis_error, cases=[], adjoints=[])
    for generation, valid_time in enumerate(("2026-07-17T06:00:00Z", "2026-07-17T06:12:00Z", "2026-07-17T06:24:00Z")):
        metrics = build_analysis_inner_product_snapshot(state, schema=schema, static_support=support, geometry=geometry)
        kwargs = dict(schema=schema, static_support=support, geometry=geometry, metrics=metrics)
        graph = dict(schema=schema, config=config, static_support=support, namelist_path=args.namelist,
            geometry=geometry, requested_fields=FIELDS,
            trajectory_receipt=build_transform_trajectory_receipt(state, **bindings, valid_time=valid_time, state_generation=generation),
            expected_valid_time=valid_time, expected_state_generation=generation, expected_bindings=bindings)
        masses = {name: metrics.field(metric_for[name]).numpy() for name in FIELDS}
        for seed in range(4):
            control = {name: direction((cells, levels+int(family == 2)) + ((6,) if family == 5 else ()),
                seed, family, 10 if family < 3 else (12 if family == 3 else 24)) for family, name in enumerate(control_names)}
            native = owned_control_increment_to_native(state, control, **kwargs)
            padded = {name: torch.cat((value, torch.zeros_like(value[:1]))) for name, value in native.items()}
            _, tangent = analysis_geoval_jvp(state, mesh, padded, **graph)
            observed = {name: apply_csr(value.detach().numpy(), offsets, columns, weights) for name, value in tangent.items() if name not in ("eastward_wind", "northward_wind")}
            observed["eastward_wind"], observed["northward_wind"] = apply_vector_cache(
                tangent["eastward_wind"].detach().numpy(), tangent["northward_wind"].detach().numpy(), vector_entries, targets)
            for mask_kind in range(2):
                active = np.ones(targets, dtype=bool) if mask_kind == 0 else np.arange(targets)%2 == 0
                masked = {name: np.where(active[:, None], value, missing) for name, value in observed.items()}
                output["cases"].append(dict(generation=generation, seed=seed, mask=mask_kind,
                    control=manifest(control), geoval_tangent=manifest(tangent), observation_tangent=manifest(masked)))
                for response in range(-1, len(FIELDS)):
                    obs_seeds = {name: (direction(value.shape, seed, family, 6).numpy() if response < 0 or response == family else np.zeros_like(value)) for family, (name, value) in enumerate((name, observed[name]) for name in FIELDS)}
                    obs_seeds = {name: np.where(active[:, None], value, 0.) for name, value in obs_seeds.items()}
                    gradient = pull_control_covector(state, obs_seeds, offsets=offsets,
                        columns=columns, weights=weights, vector_entries=vector_entries,
                        masses=masses, cells=cells, graph=graph, owner=dict(mesh=mesh, arguments=kwargs))
                    output["adjoints"].append(dict(generation=generation, seed=seed, mask=mask_kind,
                        response=response, control_adjoint=manifest(gradient)))
                    del gradient; gc.collect()
            del control, native, padded, tangent, observed; gc.collect()
        if generation < 2: state = run_simulation(state, mesh, config=config, nsteps=1)
    if len(output["cases"]) != 24 or len(output["adjoints"]) != 312:
        raise RuntimeError("OOPS TLAD independent oracle lost a required case")
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    args.output.write_text(json.dumps(output, indent=2, allow_nan=False)+"\n")


if __name__ == "__main__": main()
