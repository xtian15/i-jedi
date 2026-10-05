#!/usr/bin/env python3
"""Response-aligned FD of the real OOPS spatial gradient, using explicit NumPy NL.

The six-control gradient must first match the C++ digest at each tested response.
No production transform or interpolation compiler is contributed by this oracle.
"""
import argparse
import importlib.metadata
import gc
import json
from pathlib import Path

import numpy as np
from direct_two_step import verify_installed_wheel
from direct_typed_interpolation import FIELDS, load_atlas_cache, independent_numpy_geovals, apply_csr, apply_vector_cache
from direct_oops_tlad import manifest, pull_control_covector
from composition_fd_controls import TRACER_SCALE, density_scale, retain_progress


THERMO = ("moist_potential_temperature", "jacobian_dry_air_density")
TRACERS = ("qv", "qc", "qr", "qi", "qs", "qg")
LIVE = {
    "air_pressure": THERMO,
    "air_pressure_levels": THERMO+("qv",),
    "air_pressure_at_surface": THERMO+("qv",),
    "air_temperature": THERMO+("qv",),
    "dry_air_density": ("jacobian_dry_air_density",),
    "water_vapor_mixing_ratio_wrt_dry_air": ("qv",),
    "water_vapor_mixing_ratio_wrt_moist_air": ("qv",),
    "eastward_wind": ("eastward_wind", "northward_wind"),
    "northward_wind": ("eastward_wind", "northward_wind"),
    "height_above_mean_sea_level": (),
    "height_above_mean_sea_level_levels": (),
    "height_above_mean_sea_level_at_surface": (),
}


def dot(a, b):
    return np.sum(np.asarray(a, dtype=np.longdouble)*np.asarray(b, dtype=np.longdouble), dtype=np.longdouble)


def assert_curve(curve, label):
    from mpas_pytorch.utils.fd_validation import native_fd_policy, validate_fd_curve
    return validate_fd_curve(curve, label, policy=native_fd_policy(label[2], label[3]))


def main():
    global STEPS
    parser = argparse.ArgumentParser()
    for name in ("wheel", "init", "grid", "namelist", "atlas-cache", "atlas-metrics", "locations", "oops", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    for name in ("wheel-sha256", "source-commit", "atlas-compiler-identity"):
        parser.add_argument("--"+name, required=True)
    args = parser.parse_args()
    cpp = json.loads(args.oops.read_text())
    if len(cpp["cases"]) != 24 or len(cpp["adjoints"]) != 312 or cpp["zero_response_trials"] != 72:
        raise RuntimeError("coupled FD requires the complete real OOPS proof, not a partial trace")
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    from mpas_pytorch.utils.fd_validation import FD_STEPS
    STEPS = FD_STEPS
    import torch
    from mpas_pytorch import load_initial_state, load_config_from_namelist, prepare_initial_state, run_simulation
    from mpas_pytorch.ijedi_contracts import build_configuration_receipt, load_ijedi_geometry_snapshot, build_state_storage_schema
    from mpas_pytorch.assimilation_variables import build_transform_trajectory_receipt
    from mpas_pytorch.analysis_spaces import build_analysis_inner_product_snapshot, owned_control_increment_to_native
    torch.set_num_threads(1); torch.set_num_interop_threads(1)
    config = load_config_from_namelist(args.namelist)
    raw, mesh = load_initial_state(args.init, args.grid, config=config)
    geometry = load_ijedi_geometry_snapshot(args.init, args.grid, configuration_receipt=build_configuration_receipt(args.namelist, config))
    state = prepare_initial_state(raw, mesh, config=config)
    reference = run_simulation(raw, mesh, config=config, nsteps=1)
    bindings = dict(horizontal_geometry_receipt=geometry.horizontal_receipt,
        static_vertical_geometry_receipt=geometry.static_vertical_receipt,
        bundle_receipt=geometry.receipt, configuration_receipt=geometry.configuration_receipt)
    schema = build_state_storage_schema(reference, mesh, **bindings)
    names = {field.name for field in schema.fields if field.role in ("static_geometry", "static_support")}
    names.update(schema.support_metadata_keys)
    support = {name: reference[name] for name in names}
    del raw, reference; gc.collect()
    bindings.update(state_schema_digest=schema.digest, package_identity=dict(version=importlib.metadata.version("mpas-pytorch"),
        source_commit=args.source_commit, wheel_sha256=args.wheel_sha256))
    locations = json.loads(args.locations.read_text())
    offsets, columns, weights, vector, _ = load_atlas_cache(args, geometry,
        locations["latitude_degrees"], locations["longitude_degrees"])
    cells, edges, levels, targets = int(state["nCells"]), int(state["nEdges"]), int(state["nVertLevels"]), len(locations["latitude_degrees"])
    if (cells, edges, levels, targets) != (10242, 30720, 55, 256) or sum(map(len, LIVE.values())) != 18:
        raise RuntimeError("coupled FD inventory differs from its preregistered real case")
    trials = []
    zero_pairs = 0
    for generation, time in enumerate(("2026-07-17T06:00:00Z", "2026-07-17T06:12:00Z", "2026-07-17T06:24:00Z")):
        metrics = build_analysis_inner_product_snapshot(state, schema=schema, static_support=support, geometry=geometry)
        owner = dict(schema=schema, static_support=support, geometry=geometry, metrics=metrics)
        graph = dict(schema=schema, config=config, static_support=support, namelist_path=args.namelist,
            geometry=geometry, requested_fields=FIELDS,
            trajectory_receipt=build_transform_trajectory_receipt(state, **bindings, valid_time=time, state_generation=generation),
            expected_valid_time=time, expected_state_generation=generation, expected_bindings=bindings)
        masses = {name: metrics.field("cell_surface" if name.endswith("_at_surface") else
            ("cell_interface" if name in ("air_pressure_levels", "height_above_mean_sea_level_levels") else "cell_layer")).numpy() for name in FIELDS}
        nonlinear = independent_numpy_geovals(state, n_cells=cells, n_edges=edges, geometry=geometry)
        observed = {name: apply_csr(value, offsets, columns, weights) for name, value in nonlinear.items() if name not in ("eastward_wind", "northward_wind")}
        observed["eastward_wind"], observed["northward_wind"] = apply_vector_cache(nonlinear["eastward_wind"], nonlinear["northward_wind"], vector, targets)
        for seed in range(4):
            for response, output in enumerate(FIELDS):
                obs_seed = ((((np.arange(observed[output].size)+seed*11+response*7)%37).astype(np.float64)-18.)*(2.**-6)).reshape(observed[output].shape)
                seeds = {name: obs_seed if name == output else np.zeros_like(value) for name, value in observed.items()}
                gradient = pull_control_covector(state, seeds, offsets=offsets, columns=columns,
                    weights=weights, vector_entries=vector, masses=masses, cells=cells, graph=graph,
                    owner=dict(mesh=mesh, arguments=owner))
                cpp_trial = cpp["adjoints"][((generation*4+seed)*2)*13+response+1]
                expected_id = dict(generation=generation, seed=seed, mask=0, response=response)
                if any(cpp_trial[key] != value or type(cpp_trial[key]) is not int for key, value in expected_id.items()) or manifest(gradient) != cpp_trial["control_adjoint"]:
                    raise RuntimeError("FD gradient is not the exact real OOPS control adjoint")
                live = set()
                for name, value in gradient.items():
                    if name == "moisture_tracers":
                        live.update(tracer for slot, tracer in enumerate(TRACERS) if torch.count_nonzero(value[..., slot]))
                    elif torch.count_nonzero(value): live.add(name)
                if live != set(LIVE[output]):
                    raise RuntimeError(f"coupled FD lost/gained an input dependency: {output}: {live}")
                zero_pairs += 11-len(live)  # six scalar species + five other control coordinates
                for family in LIVE[output]:
                    control = {name: torch.zeros_like(value) for name, value in gradient.items()}
                    tracer = family in TRACERS
                    key = "moisture_tracers" if tracer else family
                    component = gradient[key][..., TRACERS.index(family)] if tracer else gradient[key]
                    # Preregister one physical density scale for every response,
                    # seed and state. The old 1e-5 amplitude put nonlinear
                    # interface-pressure truncation below the float64 floor.
                    # Keep the epsilon grid and scientific policy unchanged.
                    scale = (TRACER_SCALE if tracer else
                             density_scale(float(state["rho_zz"][:-1].max()))
                             if key == "jacobian_dry_air_density" else
                             .1 if key == "moist_potential_temperature" else 1.)
                    if tracer:
                        # Feasible response-aligned one-sided perturbation at
                        # zero condensates. Select one sign, never cancel a
                        # positive and negative objective block.
                        positive, negative = torch.clamp(component, min=0.), torch.clamp(-component, min=0.)
                        mass = metrics.field("cell_layer").numpy()
                        chosen = positive if abs(dot(positive.numpy(), mass*component.numpy())) >= abs(dot(negative.numpy(), mass*component.numpy())) else negative
                        direction = chosen*(scale/chosen.abs().max())
                        control[key][..., TRACERS.index(family)] = direction
                    else:
                        direction = component*(scale/component.abs().max())
                        if key == "jacobian_dry_air_density":
                            bound = .25*state["rho_zz"][:-1]/max(STEPS)
                            direction = torch.clamp(direction, -bound, bound)
                        control[key] = direction
                    mass = metrics.field("cell_layer").numpy()
                    if tracer: mass = mass[..., None]
                    target = dot(control[key].numpy(), mass*gradient[key].numpy())
                    if target == 0 or not np.isfinite(target): raise RuntimeError("response-aligned FD has no finite nonzero target")
                    native = owned_control_increment_to_native(state, control, **owner)
                    padded = {name: torch.cat((value, torch.zeros_like(value[:1]))) for name, value in native.items()}
                    baseline = dot(observed[output], obs_seed) if tracer else None
                    def objective(step):
                        candidate = dict(state)
                        for name, value in padded.items():
                            if torch.count_nonzero(value): candidate[name] = state[name]+step*value
                        values = independent_numpy_geovals(candidate, n_cells=cells, n_edges=edges, geometry=geometry)
                        if output in ("eastward_wind", "northward_wind"):
                            pair = apply_vector_cache(values["eastward_wind"], values["northward_wind"], vector, targets)
                            value = pair[int(output == "northward_wind")]
                        else: value = apply_csr(values[output], offsets, columns, weights)
                        return dot(value, obs_seed)
                    curve = []
                    for step in STEPS:
                        p, q = objective(step), objective(2*step if tracer else -step)
                        estimate = (-3*baseline+4*p-q if tracer else p-q)/np.longdouble(2*step)
                        curve.append(dict(step=step, finite_difference=str(estimate), relative_error=float(abs(estimate-target)/abs(target))))
                    trial = dict(generation=generation, seed=seed, output=output, input=family,
                        scheme="second_order_feasible_one_sided" if tracer else "central",
                        stimulus_scale=float(scale),
                        adjoint_target=str(target), curve=curve)
                    if key == "jacobian_dry_air_density":
                        trial["maximum_relative_density_displacement"] = float(
                            (max(STEPS)*direction.abs()/state["rho_zz"][:-1]).max())
                        if trial["maximum_relative_density_displacement"] > .25:
                            raise RuntimeError("FD density perturbation exceeds positivity cap")
                    try:
                        assert_curve(curve, (generation, seed, output, family))
                    except Exception as error:
                        retain_progress(args.output, trials,
                                        failed=dict(trial, error=str(error)))
                        raise
                    trials.append(trial)
                    retain_progress(args.output, trials)
                    print(json.dumps(dict(completed_curves=len(trials), **{key: trial[key] for key in ("generation", "seed", "output", "input")})), flush=True)
                    del control, native, padded; gc.collect()
                del gradient; gc.collect()
        if generation < 2: state = run_simulation(state, mesh, config=config, nsteps=1)
    if len(trials) != 216 or zero_pairs != 1368:
        raise RuntimeError("coupled FD omitted a declared live/null control-response pair")
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    args.output.write_text(json.dumps(dict(scope="real_OOPS_control_gradient_vs_independent_NumPy_NL_plus_stored_Atlas",
        curves=trials, live_curves=216, null_pairs=zero_pairs, epsilon_count=len(STEPS), seeds=4, states=3), indent=2, allow_nan=False)+"\n")


if __name__ == "__main__": main()
