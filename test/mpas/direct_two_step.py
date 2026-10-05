#!/usr/bin/env python3
"""Produce a fieldwise two-step oracle from one exactly installed MPAS wheel."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import importlib.util
import json
import platform
import sys
import zipfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/mpas_runtime"))
from runtime_identity import verify_runtime

REGRESSION_OUTPUT_TENSOR_KEYS = frozenset(
    {
        "divergence",
        "exner",
        "exner_base",
        "h_edge",
        "ke",
        "pressure_base",
        "pressure_p",
        "pv_edge",
        "rho_edge",
        "rho_p",
        "rho_zz",
        "rtheta_base",
        "rtheta_p",
        "rt_diabatic_tend",
        "ru",
        "rw",
        "scalars",
        "theta_m",
        "u",
        "uReconstructMeridional",
        "uReconstructX",
        "uReconstructY",
        "uReconstructZ",
        "uReconstructZonal",
        "ur_cell",
        "v",
        "vorticity",
        "vr_cell",
        "w",
    }
)
PROGNOSTIC_KEYS = frozenset(
    {"rho_zz", "rt_diabatic_tend", "scalars", "theta_m", "u", "w"}
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_installed_wheel(wheel: Path, expected_sha256: str) -> Path:
    actual_sha256 = sha256_file(wheel)
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"wheel digest mismatch: expected {expected_sha256}, got {actual_sha256}"
        )
    distribution = importlib.metadata.distribution("mpas-pytorch")
    install_root = Path(distribution.locate_file("")).resolve()
    expected_package_files: set[str] = set()
    with zipfile.ZipFile(wheel) as archive:
        for name in archive.namelist():
            if not name or name.endswith(("/", ".dist-info/RECORD")):
                continue
            installed = install_root / name
            if not installed.is_file():
                raise RuntimeError(
                    f"installed distribution is missing wheel member: {name}"
                )
            if archive.read(name) != installed.read_bytes():
                raise RuntimeError(
                    f"installed distribution differs from wheel member: {name}"
                )
            if name.startswith("mpas_pytorch/"):
                expected_package_files.add(name)
    package_root = install_root / "mpas_pytorch"
    installed_package_files = {
        str(path.relative_to(install_root))
        for path in package_root.rglob("*")
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc"
    }
    if installed_package_files != expected_package_files:
        extra = sorted(installed_package_files - expected_package_files)
        missing = sorted(expected_package_files - installed_package_files)
        raise RuntimeError(
            f"installed package inventory differs from wheel; extra={extra}, missing={missing}"
        )
    return (package_root / "__init__.py").resolve()


def clone_state(state: dict[str, Any], torch: Any) -> dict[str, Any]:
    import copy

    return {
        name: value.clone() if isinstance(value, torch.Tensor) else copy.deepcopy(value)
        for name, value in state.items()
    }


def boundary(state: dict[str, Any], torch: Any) -> dict[str, Any]:
    missing = sorted(
        name
        for name in REGRESSION_OUTPUT_TENSOR_KEYS
        if not isinstance(state.get(name), torch.Tensor)
    )
    if missing:
        raise RuntimeError(f"continuation boundary is missing tensors: {missing}")
    return {
        name: state[name].detach().cpu().contiguous().clone()
        for name in sorted(REGRESSION_OUTPUT_TENSOR_KEYS)
    }


def tensor_manifest(values: dict[str, Any], torch: Any) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in values.items():
        value = value.detach().cpu().contiguous()
        raw = value.reshape(-1).view(torch.uint8).numpy().tobytes(order="C")
        result[name] = {
            "dtype": str(value.dtype),
            "shape": list(value.shape),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "finite": bool(torch.isfinite(value).all().item()),
        }
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--namelist", type=Path, required=True)
    parser.add_argument("--dt", type=float, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-tensors", type=Path, required=True)
    parser.add_argument("--container-digest", required=True)
    parser.add_argument("--runtime-receipt", type=Path,
                        default=Path(__file__).resolve().parents[2] / "tools/mpas_runtime/runtime_receipt.json")
    parser.add_argument(
        "--legacy-forward-only",
        action="store_true",
        help=(
            "Run the declared wheel through two complete-state steps. "
            "This mode is only the immutable forward oracle; it deliberately does "
            "not synthesize geometry, schema, or typed-transform contracts that the "
            "release did not contain."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    package_file = verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)

    import netCDF4
    import numpy as np
    import torch
    from mpas_pytorch import __file__ as imported_package_file
    from mpas_pytorch import (
        __version__,
        load_config_from_namelist,
        load_initial_state,
        run_simulation,
    )

    if Path(imported_package_file).resolve() != package_file:
        raise RuntimeError("imported package is not the exact verified distribution")
    runtime_identity = verify_runtime(args.runtime_receipt)
    if torch.version.cuda is not None:
        raise RuntimeError(
            f"MPAS integration requires CPU-only torch, got CUDA={torch.version.cuda}"
        )
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    if torch.get_num_threads() != 1 or torch.get_num_interop_threads() != 1:
        raise RuntimeError("MPAS integration requires one Torch intraop and interop thread")

    config = load_config_from_namelist(args.namelist)
    configured_dt = float(config["config_dt"])
    if args.dt != configured_dt:
        raise RuntimeError(
            f"requested dt {args.dt!r} differs from namelist config_dt {configured_dt!r}"
        )
    initial, mesh = load_initial_state(
        args.init, args.grid, mesh_support_path=None, config=config
    )
    first_state = run_simulation(
        clone_state(initial, torch), mesh, config=config, nsteps=1
    )
    first = boundary(first_state, torch)
    geometry = None
    schema = None
    typed_transform_step1 = None
    typed_transform_step2 = None
    if args.legacy_forward_only:
        resumed_state = clone_state(first_state, torch)
        continuation_metadata_count = 0
        continuation_tensor_count = len(REGRESSION_OUTPUT_TENSOR_KEYS)
        second_step_state_source = "legacy_complete_step1_state"
    else:
        from mpas_pytorch.assimilation_variables import (
            build_transform_trajectory_receipt,
            descriptor_registry_manifest,
        )
        from mpas_pytorch.analysis_coordinates import diagnose_analysis_geovals
        from mpas_pytorch.ijedi_contracts import (
            CONTINUATION_METADATA_KEYS,
            CONTINUATION_TENSOR_KEYS,
            build_configuration_receipt,
            build_state_storage_schema,
            compose_continuation_state,
            extract_continuation_boundary,
            load_ijedi_geometry_snapshot,
        )

        configuration_receipt = build_configuration_receipt(args.namelist, config)
        geometry = load_ijedi_geometry_snapshot(
            args.init,
            args.grid,
            configuration_receipt=configuration_receipt,
        )
        schema = build_state_storage_schema(
            first_state,
            mesh,
            horizontal_geometry_receipt=geometry.horizontal_receipt,
            static_vertical_geometry_receipt=geometry.static_vertical_receipt,
            bundle_receipt=geometry.receipt,
            configuration_receipt=configuration_receipt,
        )
        package_identity = {
            "version": __version__,
            "source_commit": args.source_commit,
            "wheel_sha256": args.wheel_sha256,
        }

        def transform_builder(
            transform_state: dict[str, Any], *, valid_time: str, generation: int
        ) -> dict[str, Any]:
            receipt = build_transform_trajectory_receipt(
                transform_state,
                horizontal_geometry_receipt=geometry.horizontal_receipt,
                static_vertical_geometry_receipt=geometry.static_vertical_receipt,
                bundle_receipt=geometry.receipt,
                state_schema_digest=schema.digest,
                configuration_receipt=configuration_receipt,
                valid_time=valid_time,
                state_generation=generation,
                package_identity=package_identity,
            )
            geovals = diagnose_analysis_geovals(
                transform_state, mesh, schema=schema, config=config,
                static_support=first_state, namelist_path=args.namelist, geometry=geometry,
                requested_fields=("air_pressure", "air_pressure_levels", "air_pressure_at_surface",
                    "air_temperature", "dry_air_density", "water_vapor_mixing_ratio_wrt_dry_air",
                    "water_vapor_mixing_ratio_wrt_moist_air", "eastward_wind", "northward_wind",
                    "height_above_mean_sea_level", "height_above_mean_sea_level_levels",
                    "height_above_mean_sea_level_at_surface"),
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
            return {
                "trajectory_receipt": receipt,
                "descriptor_registry": descriptor_registry_manifest(),
                "geovals": tensor_manifest(geovals, torch),
            }

        typed_transform_step1 = transform_builder(
            first_state, valid_time="2026-07-17T06:12:00Z", generation=1
        )
        compact_boundary = extract_continuation_boundary(first_state, schema=schema)
        resumed_state = compose_continuation_state(
            first_state, compact_boundary, schema=schema
        )
        continuation_metadata_count = len(CONTINUATION_METADATA_KEYS)
        continuation_tensor_count = len(CONTINUATION_TENSOR_KEYS)
        second_step_state_source = (
            "receipt_checked_compact_boundary_plus_static_support"
        )
    second_state = run_simulation(resumed_state, mesh, config=config, nsteps=1)
    second = boundary(second_state, torch)
    if not args.legacy_forward_only:
        typed_transform_step2 = transform_builder(
            second_state, valid_time="2026-07-17T06:24:00Z", generation=2
        )

    if not all(
        torch.isfinite(value).all() for value in (*first.values(), *second.values())
    ):
        raise RuntimeError("non-finite value in continuation boundary")
    first_changed = [
        name
        for name in PROGNOSTIC_KEYS
        if isinstance(initial.get(name), torch.Tensor)
        and not torch.equal(initial[name].detach().cpu(), first[name])
    ]
    second_changed = [
        name for name in PROGNOSTIC_KEYS if not torch.equal(first[name], second[name])
    ]
    if not first_changed or not second_changed:
        raise RuntimeError(
            f"trivial model boundary: first_changed={first_changed}, second_changed={second_changed}"
        )

    payload = {
        "schema_version": 2,
        "python_executable": sys.executable,
        "package_file": str(package_file),
        "package_version": __version__,
        "package_source_commit": args.source_commit,
        "wheel_path": str(args.wheel.resolve()),
        "wheel_sha256": args.wheel_sha256,
        "runtime": {
            "receipt_sha256": runtime_identity["runtime_receipt_sha256"],
            "container_digest": args.container_digest,
            "python_version": platform.python_version(),
            "platform_machine": platform.machine(),
            "torch_version": torch.__version__,
            "torch_cuda": torch.version.cuda,
            "torch_intraop_threads": torch.get_num_threads(),
            "torch_interop_threads": torch.get_num_interop_threads(),
            "numpy_version": np.__version__,
            "netcdf4_version": netCDF4.__version__,
        },
        "input_files": {
            "init": {"name": args.init.name, "sha256": sha256_file(args.init)},
            "grid": {"name": args.grid.name, "sha256": sha256_file(args.grid)},
            "namelist": {
                "name": args.namelist.name,
                "sha256": sha256_file(args.namelist),
            },
        },
        "configuration_receipt": hashlib.sha256(
            json.dumps(
                {
                    "schema_version": 1,
                    "source_name": args.namelist.name,
                    "source_sha256": sha256_file(args.namelist),
                    "resolved_config": config,
                },
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            ).encode("utf-8")
        ).hexdigest(),
        "dt_seconds": configured_dt,
        "second_step_state_source": second_step_state_source,
        "continuation_tensor_count": continuation_tensor_count,
        "continuation_metadata_count": continuation_metadata_count,
        "regression_output_tensor_count": len(REGRESSION_OUTPUT_TENSOR_KEYS),
        "first_changed_prognostics": first_changed,
        "second_changed_prognostics": second_changed,
        "step1": tensor_manifest(first, torch),
        "step2": tensor_manifest(second, torch),
    }
    if geometry is not None and schema is not None:
        payload.update(
            {
                "horizontal_geometry_receipt": geometry.horizontal_receipt,
                "static_vertical_geometry_receipt": geometry.static_vertical_receipt,
                "geometry_bundle_receipt": geometry.receipt,
                "state_schema_digest": schema.digest,
                "typed_transform_step1": typed_transform_step1,
                "typed_transform_step2": typed_transform_step2,
            }
        )
    if sha256_file(args.runtime_receipt) != runtime_identity["runtime_receipt_sha256"]:
        raise RuntimeError("runtime receipt changed during direct qualification")
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_tensors.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n"
    )
    torch.save({"step1": first, "step2": second}, args.output_tensors)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
