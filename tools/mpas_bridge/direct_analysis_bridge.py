#!/usr/bin/env python3
"""Authenticated installed-model oracle for the narrow embedded analysis calls."""
import argparse
import gc
import importlib.metadata
import json
from pathlib import Path

from direct_two_step import verify_installed_wheel, tensor_manifest
from direct_typed_interpolation import FIELDS


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "init", "grid", "namelist", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    import torch
    from mpas_pytorch import load_initial_state, load_config_from_namelist, prepare_initial_state, run_simulation
    from mpas_pytorch.ijedi_contracts import (
        build_configuration_receipt, load_ijedi_geometry_snapshot, build_state_storage_schema,
    )
    from mpas_pytorch.assimilation_variables import build_transform_trajectory_receipt
    from mpas_pytorch.analysis_spaces import (
        build_analysis_inner_product_snapshot, owned_control_increment_to_native,
        owned_native_covectors_to_control,
    )
    from mpas_pytorch.analysis_coordinates import NATIVE_ANALYSIS_KEYS, analysis_geoval_jvp, analysis_geoval_vjp
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
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
    del raw, reference
    gc.collect()
    bindings.update(state_schema_digest=schema.digest, package_identity={"version": importlib.metadata.version("mpas-pytorch"),
        "source_commit": args.source_commit, "wheel_sha256": args.wheel_sha256})
    control_names = ("eastward_wind", "northward_wind", "upward_air_velocity",
                     "moist_potential_temperature", "jacobian_dry_air_density", "moisture_tracers")

    def direction(shape, seed, family, exponent):
        size = 1
        for extent in shape: size *= extent
        values = ((torch.arange(size, dtype=torch.int64) + seed*11 + family*7) % 37).to(torch.float64) - 18.
        return (values * (2. ** -exponent)).reshape(shape)

    def manifest(arrays):
        return {name: {"shape": item["shape"], "sha256": item["sha256"]}
                for name, item in tensor_manifest(arrays, torch).items()}

    output = {"scope": "direct_installed_model_analysis_not_OOPS_TLAD_or_model_time_adjoint", "cases": []}
    for generation in range(3):
        valid_time = ("2026-07-17T06:00:00Z", "2026-07-17T06:12:00Z", "2026-07-17T06:24:00Z")[generation]
        metrics = build_analysis_inner_product_snapshot(state, schema=schema, static_support=support, geometry=geometry)
        kwargs = dict(schema=schema, static_support=support, geometry=geometry, metrics=metrics)
        graph = dict(schema=schema, config=config, static_support=support, namelist_path=args.namelist,
            geometry=geometry, requested_fields=FIELDS,
            trajectory_receipt=build_transform_trajectory_receipt(state, **bindings,
                valid_time=valid_time, state_generation=generation),
            expected_valid_time=valid_time, expected_state_generation=generation, expected_bindings=bindings)
        for seed in range(4):
            control = {}
            for family, name in enumerate(control_names):
                exponent = 10 if family < 3 else (12 if family == 3 else 24)
                shape = (int(state["nCells"]), int(state["nVertLevels"]) + int(family == 2))
                if family == 5: shape += (6,)
                control[name] = direction(shape, seed, family, exponent)
            native = owned_control_increment_to_native(state, control, **kwargs)
            padded = {name: torch.cat((value, torch.zeros_like(value[:1]))) for name, value in native.items()}
            values, tangent = analysis_geoval_jvp(state, mesh, padded, **graph)
            seeds = {name: direction(tangent[name].shape, seed, family, 6) for family, name in enumerate(FIELDS)}
            _, covectors = analysis_geoval_vjp(state, mesh, seeds, **graph)
            covectors = {name: value[:-1] for name, value in covectors.items()}
            adjoint = owned_native_covectors_to_control(state, covectors, **kwargs)
            output["cases"].append(dict(generation=generation, seed=seed,
                native_values=manifest({name: state[name][:-1] for name in NATIVE_ANALYSIS_KEYS}),
                measures=manifest({name: metrics.field(name) for name in ("cell_layer", "cell_interface", "cell_surface", "edge_layer")}),
                control=manifest(control), native_directions=manifest(native), geoval_tangents=manifest(tangent),
                native_covectors=manifest(covectors), control_adjoint=manifest(adjoint)))
            del control, native, padded, values, tangent, seeds, covectors, adjoint
            gc.collect()
        if generation < 2: state = run_simulation(state, mesh, config=config, nsteps=1)
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    args.output.write_text(json.dumps(output, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
