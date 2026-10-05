#!/usr/bin/env python3
"""Real-grid typed GeoVaL JVP/VJP with authenticated Atlas scalar/vector replay.

Replay is an independent test oracle, not a production interpolation engine.
"""

from __future__ import annotations

import argparse
import gc
import importlib.util
import json
import math
from pathlib import Path

import netCDF4 as nc
import numpy as np
import torch

from direct_two_step import verify_installed_wheel
from direct_typed_interpolation import load_atlas_cache
from composition_fd_controls import TRACER_SCALE


OUTPUTS = (
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

INPUT_SCALES = {
    "u": 1.0,
    "theta_m": 0.1,
    "scalars": 1.0e-5,
    "exner": 1.0e-4,
    "pressure_base": 1.0,
    "pressure_p": 1.0,
    "rho_zz": 1.0e-5,
    "zz": 1.0e-5,
    "zgrid": 1.0e-1,
}

# Preregister the exact input blocks on which each typed response is allowed to
# depend.  This turns the derivative test into an input-by-output matrix: an
# omitted live block and an accidental cross-block dependency both fail.
EXPECTED_DEPENDENCIES = {
    "air_pressure": {"pressure_base", "pressure_p"},
    "air_pressure_levels": {
        "scalars", "pressure_base", "pressure_p", "rho_zz", "zz", "zgrid",
    },
    "air_pressure_at_surface": {
        "scalars", "pressure_base", "pressure_p", "rho_zz", "zz", "zgrid",
    },
    "air_temperature": {"theta_m", "scalars", "exner"},
    "dry_air_density": {"rho_zz", "zz"},
    "water_vapor_mixing_ratio_wrt_dry_air": {"scalars"},
    "water_vapor_mixing_ratio_wrt_moist_air": {"scalars"},
    "eastward_wind": {"u"},
    "northward_wind": {"u"},
    "height_above_mean_sea_level": {"zgrid"},
    "height_above_mean_sea_level_levels": {"zgrid"},
    "height_above_mean_sea_level_at_surface": {"zgrid"},
}

TRACERS = ("qv", "qc", "qr", "qi", "qs", "qg")
EXPECTED_TRACERS = {
    "air_temperature": {"qv"},
    "water_vapor_mixing_ratio_wrt_dry_air": {"qv"},
    "water_vapor_mixing_ratio_wrt_moist_air": {"qv"},
    "air_pressure_levels": {"qv"},
    "air_pressure_at_surface": {"qv"},
}


class AtlasReplay:
    def __init__(self, args, snapshot):
        locations = json.loads(args.locations.read_text())
        lat, lon = (np.asarray(locations[name], dtype=np.float64)
                    for name in ("latitude_degrees", "longitude_degrees"))
        offsets, columns, weights, vector, _ = load_atlas_cache(args, snapshot, lat, lon)
        self.source_size = int(snapshot.horizontal.metadata["counts"]["nCells"])
        self.destination_size = len(lat)
        self.row = torch.repeat_interleave(torch.arange(len(lat)), torch.from_numpy(np.diff(offsets)))
        self.col, self.weight = torch.from_numpy(columns), torch.from_numpy(weights)
        self.vrow = torch.tensor([entry[0] for entry in vector], dtype=torch.int64)
        self.vcol = torch.tensor([entry[1] for entry in vector], dtype=torch.int64)
        self.real = torch.tensor([entry[2] for entry in vector], dtype=torch.float64)
        self.imaginary = torch.tensor([entry[3] for entry in vector], dtype=torch.float64)

    @staticmethod
    def family(name):
        return ("eastward_wind", "northward_wind") if name in ("eastward_wind", "northward_wind") else (name,)

    def project(self, fields, name):
        if len(self.family(name)) == 1:
            return apply_operator(self.row, self.col, self.weight, fields[name], self.destination_size)
        east, north = fields["eastward_wind"], fields["northward_wind"]
        selected = (self.real[:, None] * east[self.vcol] - self.imaginary[:, None] * north[self.vcol]
                    if name == "eastward_wind" else
                    self.imaginary[:, None] * east[self.vcol] + self.real[:, None] * north[self.vcol])
        result = torch.zeros((self.destination_size,) + east.shape[1:], dtype=torch.float64)
        return result.index_add_(0, self.vrow, selected)

    def pull(self, seed, name):
        if len(self.family(name)) == 1:
            return {name: apply_transpose(self.row, self.col, self.weight, seed, self.source_size)}
        result = {}
        for component, coefficient in (("eastward_wind", self.real if name == "eastward_wind" else self.imaginary),
                                       ("northward_wind", -self.imaginary if name == "eastward_wind" else self.real)):
            values = torch.zeros((self.source_size,) + seed.shape[1:], dtype=torch.float64)
            result[component] = values.index_add_(0, self.vcol, coefficient[:, None] * seed[self.vrow])
        return result


def apply_operator(
    row: torch.Tensor, col: torch.Tensor, weight: torch.Tensor,
    source: torch.Tensor, destination_size: int,
) -> torch.Tensor:
    flat = source.reshape(source.shape[0], -1)
    result = torch.zeros((destination_size, flat.shape[1]), dtype=torch.float64)
    result.index_add_(0, row, weight[:, None] * flat[col])
    return result.reshape((destination_size,) + source.shape[1:])


def apply_transpose(
    row: torch.Tensor, col: torch.Tensor, weight: torch.Tensor,
    destination: torch.Tensor, source_size: int,
) -> torch.Tensor:
    flat = destination.reshape(destination.shape[0], -1)
    result = torch.zeros((source_size, flat.shape[1]), dtype=torch.float64)
    result.index_add_(0, col, weight[:, None] * flat[row])
    return result.reshape((source_size,) + destination.shape[1:])


def extended_dot(left: torch.Tensor, right: torch.Tensor) -> np.longdouble:
    a = left.detach().cpu().numpy().reshape(-1).astype(np.longdouble, copy=False)
    b = right.detach().cpu().numpy().reshape(-1).astype(np.longdouble, copy=False)
    result = np.sum(a * b, dtype=np.longdouble)
    if not np.isfinite(result):
        raise AssertionError("derivative dot product is nonfinite")
    return result


def relative_residual(left: np.longdouble, right: np.longdouble) -> float:
    if not np.isfinite(left) or not np.isfinite(right):
        raise AssertionError("derivative residual input is nonfinite")
    scale = max(abs(left), abs(right), np.longdouble(1.0e-300))
    return float(abs(left - right) / scale)


def response(
    state: dict[str, object], output_name: str, seed: torch.Tensor,
    operator: AtlasReplay,
    n_cells: int,
) -> np.longdouble:
    values = diagnose_geovals(state, n_cells=n_cells, requested_fields=operator.family(output_name))
    return extended_dot(operator.project(values, output_name), seed)


def main() -> int:
    global GEOVAL_DIFFERENTIABLE_INPUTS, diagnose_geovals
    parser = argparse.ArgumentParser()
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--namelist", type=Path, required=True)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--atlas-cache", type=Path, required=True)
    parser.add_argument("--atlas-metrics", type=Path, required=True)
    parser.add_argument("--atlas-compiler-identity", required=True)
    parser.add_argument("--locations", type=Path, required=True)
    parser.add_argument("--state-generation", type=int, choices=(0, 1, 2), required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if np.finfo(np.longdouble).nmant <= np.finfo(np.float64).nmant:
        raise AssertionError("real derivative validator requires wider-than-float64 accumulation")
    installed = verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    if Path(importlib.util.find_spec("mpas_pytorch").origin).resolve() != installed:
        raise AssertionError("derivative qualification would import a shadowed model")
    from mpas_pytorch import __version__, load_config_from_namelist, load_initial_state, prepare_initial_state, run_simulation
    from mpas_pytorch.assimilation_variables import (
        GEOVAL_DIFFERENTIABLE_INPUTS, build_transform_trajectory_receipt,
        diagnose_geovals, geoval_jvp, geoval_vjp,
    )
    from mpas_pytorch.ijedi_contracts import (
        build_configuration_receipt, load_ijedi_geometry_snapshot,
        build_state_storage_schema, clone_state, compose_continuation_state,
        extract_continuation_boundary,
    )
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    config = load_config_from_namelist(args.namelist)
    initial, mesh = load_initial_state(
        args.init, args.grid, mesh_support_path=None, config=config
    )
    state = run_simulation(clone_state(initial), mesh, config=config, nsteps=1)
    configuration_receipt = build_configuration_receipt(args.namelist, config)
    snapshot = load_ijedi_geometry_snapshot(
        args.init, args.grid,
        configuration_receipt=configuration_receipt,
    )
    operator = AtlasReplay(args, snapshot)
    source_size, destination_size = operator.source_size, operator.destination_size
    n_cells = int(snapshot.metadata["counts"]["nCells"])
    if source_size != n_cells:
        raise AssertionError("point operator source extent differs from canonical MPAS cells")
    schema = build_state_storage_schema(
        state,
        mesh,
        horizontal_geometry_receipt=snapshot.horizontal_receipt,
        static_vertical_geometry_receipt=snapshot.static_vertical_receipt,
        bundle_receipt=snapshot.receipt,
        configuration_receipt=configuration_receipt,
    )
    if args.state_generation == 0:
        state = prepare_initial_state(clone_state(initial), mesh, config=config)
    elif args.state_generation == 2:
        boundary = extract_continuation_boundary(state, schema=schema)
        state = run_simulation(compose_continuation_state(state, boundary, schema=schema),
                               mesh, config=config, nsteps=1)
    from datetime import datetime, timedelta
    valid_time = (datetime(2026, 7, 17, 6) + timedelta(seconds=float(config["config_dt"]) * args.state_generation)).strftime("%Y-%m-%dT%H:%M:%SZ")
    bindings = {
        "horizontal_geometry_receipt": snapshot.horizontal_receipt,
        "static_vertical_geometry_receipt": snapshot.static_vertical_receipt,
        "bundle_receipt": snapshot.receipt,
        "state_schema_digest": schema.digest,
        "configuration_receipt": configuration_receipt,
        "package_identity": {"version": __version__, "source_commit": args.source_commit,
                             "wheel_sha256": args.wheel_sha256},
    }
    receipt = build_transform_trajectory_receipt(
        state,
        **bindings, valid_time=valid_time, state_generation=args.state_generation,
    )

    if set(EXPECTED_DEPENDENCIES) != set(OUTPUTS):
        raise AssertionError("derivative dependency matrix does not cover every output")
    known_inputs = set(GEOVAL_DIFFERENTIABLE_INPUTS)
    if any(not dependencies <= known_inputs for dependencies in EXPECTED_DEPENDENCIES.values()):
        raise AssertionError("derivative dependency matrix names an unknown input")

    trials: list[dict[str, object]] = []
    from mpas_pytorch.utils.fd_validation import FD_STEPS, validate_fd_curve
    fd_steps = FD_STEPS
    for output_index, output_name in enumerate(OUTPUTS):
        nonlinear = diagnose_geovals(
            state, n_cells=n_cells, requested_fields=(output_name,)
        )[output_name]
        identity_trials: list[dict[str, object]] = []
        dependency_norms: dict[str, float] | None = None
        curve: list[dict[str, float]] = []
        wrong_error = math.nan
        tracer_trials = []

        # Four independent output seeds produce fresh reverse and forward graphs.
        # The corresponding directions are response-aligned, but the second has
        # an independent positive spatial modulation so cancellation cannot make
        # a live input block look inactive.
        for identity_index in range(4):
            seed_value = 2026093000 + 10 * output_index + identity_index
            generator = torch.Generator().manual_seed(seed_value)
            seed = torch.randn(
                (destination_size,) + nonlinear.shape[1:],
                dtype=torch.float64,
                generator=generator,
            )
            output_cotangent = operator.pull(seed, output_name)
            reverse_values, gradients = geoval_vjp(
                state,
                output_cotangent,
                n_cells=n_cells,
                requested_fields=operator.family(output_name),
                trajectory_receipt=receipt,
                expected_valid_time=valid_time,
                expected_state_generation=args.state_generation,
                expected_bindings=bindings,
            )
            if not torch.equal(reverse_values[output_name], nonlinear):
                raise AssertionError(f"{output_name}: VJP changed the nonlinear value")

            current_norms = {
                name: float(torch.max(torch.abs(gradients[name])))
                for name in GEOVAL_DIFFERENTIABLE_INPUTS
            }
            if not all(math.isfinite(value) for value in current_norms.values()):
                raise AssertionError(f"{output_name}: nonfinite VJP dependency matrix entry")
            expected = EXPECTED_DEPENDENCIES[output_name]
            actual = {name for name, value in current_norms.items() if value != 0.0}
            if actual != expected:
                raise AssertionError(
                    f"{output_name}: derivative dependencies {sorted(actual)} differ from "
                    f"preregistered {sorted(expected)}"
                )
            if dependency_norms is None:
                dependency_norms = current_norms
            tracer_norms = {tracer: float(gradients["scalars"][..., slot].abs().max())
                            for slot, tracer in enumerate(TRACERS)}
            if {tracer for tracer, value in tracer_norms.items() if value != 0.0} != EXPECTED_TRACERS.get(output_name, set()):
                raise AssertionError(f"{output_name}: omitted or unexpected tracer dependency: {tracer_norms}")

            # Zero condensates must not disappear from the derivative test.
            # Positive, single-slot directions stay inside their feasible cone;
            # central differences at those lower bounds would be invalid.
            for slot, tracer in enumerate(TRACERS):
                if tracer not in EXPECTED_TRACERS.get(output_name, set()):
                    continue
                tracer_direction = torch.zeros_like(state["scalars"])
                # Bind the same fixed physical stimulus as the composed FD
                # gate. The former 1e-5 ray put the affine interface-pressure
                # response below the float64 subtraction floor. Preserve RNG
                # consumption, every seed/epsilon and the exact-stencil bar.
                # The fixed 1.5 bound keeps the largest displacement <=32 g/kg.
                tracer_direction[:n_cells, :, slot] = (TRACER_SCALE / 1.5) * (0.5 + torch.rand(
                    (n_cells, state["scalars"].shape[1]), dtype=torch.float64, generator=generator))
                # The feasible cone forbids flipping zero condensates into a
                # response-aligned signed direction. Use an independent positive
                # output seed instead. This avoids a nearly cancelled pressure
                # response (measured 0.0103 Pa in the former random-seed trial)
                # without changing either numerical bar or the nonlinear oracle.
                tracer_seed = 0.5 + torch.rand(seed.shape, dtype=torch.float64, generator=generator)
                _, tracer_gradients = geoval_vjp(
                    state, operator.pull(tracer_seed, output_name), n_cells=n_cells,
                    requested_fields=operator.family(output_name), trajectory_receipt=receipt,
                    expected_valid_time=valid_time, expected_state_generation=args.state_generation,
                    expected_bindings=bindings)
                tracer_tangents = {name: (tracer_direction if name == "scalars" else torch.zeros_like(state[name]))
                                   for name in GEOVAL_DIFFERENTIABLE_INPUTS}
                _, tracer_derivatives = geoval_jvp(
                    state, tracer_tangents, n_cells=n_cells,
                    requested_fields=operator.family(output_name), trajectory_receipt=receipt,
                    expected_valid_time=valid_time, expected_state_generation=args.state_generation,
                    expected_bindings=bindings)
                tracer_lhs = extended_dot(operator.project(tracer_derivatives, output_name), tracer_seed)
                tracer_rhs = extended_dot(tracer_direction, tracer_gradients["scalars"])
                tracer_error = relative_residual(tracer_lhs, tracer_rhs)
                if tracer_error > 2.e-13 or tracer_lhs == 0:
                    raise AssertionError(f"{output_name}/{tracer}: one-slot adjoint/response failed")
                tracer_curve = []
                if identity_index == 0:
                    baseline = response(state, output_name, tracer_seed, operator, n_cells)
                    for step in fd_steps:
                        plus, twice = dict(state), dict(state)
                        plus["scalars"] = state["scalars"] + step * tracer_direction
                        twice["scalars"] = state["scalars"] + 2 * step * tracer_direction
                        estimate = (-3 * baseline + 4 * response(plus, output_name, tracer_seed, operator, n_cells)
                                    - response(twice, output_name, tracer_seed, operator, n_cells)) / np.longdouble(2 * step)
                        tracer_curve.append({"step": step, "finite_difference": float(estimate),
                                             "relative_error": relative_residual(estimate, tracer_lhs)})
                    policy = ("exact_stencil" if output_name in (
                        "water_vapor_mixing_ratio_wrt_dry_air", "air_pressure_levels",
                        "air_pressure_at_surface") else "second_order")
                    try:
                        validate_fd_curve(tracer_curve, (output_name, tracer), policy=policy)
                    except AssertionError:
                        print(json.dumps({"status": "failed", "generation": args.state_generation,
                                          "field": output_name, "tracer": tracer,
                                          "seed": seed_value, "policy": policy,
                                          "curve": tracer_curve}, allow_nan=False), flush=True)
                        raise
                    del plus, twice
                tracer_trials.append({"tracer": tracer, "seed": seed_value,
                                      "adjoint_lhs": float(tracer_lhs), "adjoint_rhs": float(tracer_rhs),
                                      "adjoint_relative_error": tracer_error,
                                      "direction_max_abs": float(tracer_direction.abs().max()),
                                      "largest_displacement": float(tracer_direction.abs().max()) * 2 * max(fd_steps),
                                      "one_sided_fd_curve": tracer_curve})
                del tracer_direction, tracer_seed, tracer_gradients, tracer_tangents, tracer_derivatives

            direction: dict[str, torch.Tensor] = {}
            for name in GEOVAL_DIFFERENTIABLE_INPUTS:
                gradient = gradients[name]
                maximum = current_norms[name]
                if maximum == 0.0:
                    direction[name] = torch.zeros_like(gradient)
                elif identity_index == 0:
                    direction[name] = gradient * (INPUT_SCALES[name] / maximum)
                else:
                    modulation = 0.5 + torch.rand(
                        gradient.shape, dtype=torch.float64, generator=generator
                    )
                    modulated = gradient * modulation
                    direction[name] = modulated * (
                        INPUT_SCALES[name] / float(torch.max(torch.abs(modulated)))
                    )
                if name == "scalars":
                    # The authenticated state contains exact-zero condensates.
                    # Stay on the smooth feasible face for both FD signs.
                    limit = 0.25 * state[name][..., 1:] / max(fd_steps)
                    direction[name][..., 1:] = torch.maximum(
                        torch.minimum(direction[name][..., 1:], limit), -limit
                    )
            values, derivatives = geoval_jvp(
                state,
                direction,
                n_cells=n_cells,
                requested_fields=operator.family(output_name),
                trajectory_receipt=receipt,
                expected_valid_time=valid_time,
                expected_state_generation=args.state_generation,
                expected_bindings=bindings,
            )
            if not torch.equal(values[output_name], nonlinear):
                raise AssertionError(f"{output_name}: JVP changed the nonlinear value")
            mapped_derivative = operator.project(derivatives, output_name)
            lhs = extended_dot(mapped_derivative, seed)
            rhs = sum(
                (extended_dot(direction[name], gradients[name])
                 for name in GEOVAL_DIFFERENTIABLE_INPUTS),
                np.longdouble(0.0),
            )
            adjoint_error = relative_residual(lhs, rhs)
            if adjoint_error > 2.0e-13:
                raise AssertionError(
                    f"{output_name}: composed JVP/VJP residual {adjoint_error:.17e}"
                )
            identity_trials.append(
                {
                    "seed": seed_value,
                    "adjoint_lhs": float(lhs),
                    "adjoint_rhs": float(rhs),
                    "adjoint_relative_error": adjoint_error,
                }
            )

            if identity_index == 0:
                wrong_seed = torch.roll(seed, shifts=1, dims=0)
                wrong_cotangent = operator.pull(wrong_seed, output_name)
                _, wrong_gradients = geoval_vjp(
                    state,
                    wrong_cotangent,
                    n_cells=n_cells,
                    requested_fields=operator.family(output_name),
                    trajectory_receipt=receipt,
                    expected_valid_time=valid_time,
                    expected_state_generation=args.state_generation,
                    expected_bindings=bindings,
                )
                wrong_rhs = sum(
                    (extended_dot(direction[name], wrong_gradients[name])
                     for name in GEOVAL_DIFFERENTIABLE_INPUTS),
                    np.longdouble(0.0),
                )
                wrong_error = relative_residual(lhs, wrong_rhs)
                if wrong_error < 1.0e-7:
                    raise AssertionError(
                        f"{output_name}: wrong sparse transpose was not detected"
                    )

                for step in fd_steps:
                    plus = dict(state)
                    minus = dict(state)
                    for name in GEOVAL_DIFFERENTIABLE_INPUTS:
                        plus[name] = state[name] + step * direction[name]
                        minus[name] = state[name] - step * direction[name]
                    plus_response = response(
                        plus, output_name, seed, operator, n_cells,
                    )
                    minus_response = response(
                        minus, output_name, seed, operator, n_cells,
                    )
                    finite_difference = (
                        plus_response - minus_response
                    ) / np.longdouble(2.0 * step)
                    error = relative_residual(finite_difference, lhs)
                    curve.append(
                        {
                            "step": step,
                            "finite_difference": float(finite_difference),
                            "relative_error": error,
                        }
                    )
                del wrong_seed, wrong_cotangent, wrong_gradients

            del seed, output_cotangent, reverse_values, gradients
            del direction, values, derivatives, mapped_derivative
            gc.collect()

        if dependency_norms is None or len(identity_trials) != 4:
            raise AssertionError(f"{output_name}: incomplete derivative seed matrix")
        policy = ("exact_stencil" if output_name in (
            "air_pressure", "dry_air_density", "water_vapor_mixing_ratio_wrt_dry_air",
            "eastward_wind", "northward_wind", "height_above_mean_sea_level",
            "height_above_mean_sea_level_levels", "height_above_mean_sea_level_at_surface")
            else "second_order")
        fd_validation = validate_fd_curve(curve, output_name, policy=policy)
        best_fd_error = fd_validation["best_error"]
        best_index = min(range(len(curve)), key=lambda index: curve[index]["relative_error"])
        if best_index == len(curve) - 1:
            raise AssertionError(
                f"{output_name}: finite-difference curve ended before its minimum: {curve}"
            )
        if curve[-1]["relative_error"] <= max(
            10.0 * curve[best_index]["relative_error"], 1.0e-10
        ):
            raise AssertionError(f"{output_name}: finite-difference curve did not show roundoff")
        trials.append(
            {
                "output": output_name,
                "expected_dependencies": sorted(EXPECTED_DEPENDENCIES[output_name]),
                "vjp_max_abs_by_input": dependency_norms,
                "identity_trials": identity_trials,
                "tracer_trials": tracer_trials,
                "wrong_transpose_relative_error": wrong_error,
                "best_fd_relative_error": best_fd_error,
                "best_fd_step": curve[best_index]["step"],
                "fd_curve": curve,
                "fd_validation": fd_validation,
            }
        )
        del nonlinear
        gc.collect()

    report = {
        "schema_version": 2,
        "scope": "x1.10242 typed-transform plus authenticated Atlas scalar/vector test replay",
        "state_generation": args.state_generation,
        "valid_time": valid_time,
        "source_cells": source_size,
        "destination_locations": destination_size,
        "nonzeros": int(operator.weight.numel()),
        "differentiable_inputs": list(GEOVAL_DIFFERENTIABLE_INPUTS),
        "output_families": list(OUTPUTS),
        "trials": trials,
        "maximum_adjoint_relative_error": max(
            float(identity["adjoint_relative_error"])
            for trial in trials for identity in trial["identity_trials"] + trial["tracer_trials"]
        ),
        "maximum_best_fd_relative_error": max(
            [float(trial["best_fd_relative_error"]) for trial in trials] +
            [min(point["relative_error"] for point in tracer["one_sided_fd_curve"])
             for trial in trials for tracer in trial["tracer_trials"]
             if tracer["one_sided_fd_curve"]]
        ),
        "tracer_probe_policy": "four independent positive output seeds and feasible positive single-slot directions",
        "minimum_wrong_transpose_relative_error": min(
            float(trial["wrong_transpose_relative_error"]) for trial in trials
        ),
    }
    if not all(math.isfinite(float(value)) for value in (
        report["maximum_adjoint_relative_error"],
        report["maximum_best_fd_relative_error"],
        report["minimum_wrong_transpose_relative_error"],
    )):
        raise AssertionError("real derivative report contains a nonfinite summary")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, sort_keys=True, indent=2, allow_nan=False) + "\n")
    print(json.dumps({key: report[key] for key in (
        "state_generation", "maximum_adjoint_relative_error", "maximum_best_fd_relative_error",
        "minimum_wrong_transpose_relative_error")}, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
