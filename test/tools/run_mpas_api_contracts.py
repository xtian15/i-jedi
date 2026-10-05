#!/usr/bin/env python3
# Copyright (C) 2026 IC Weather LLC. Apache-2.0.
"""Prepare a valid, incompatible owner context for the real C++ API attack.

Only test-fixture construction happens here. The C++ consumer still owns its
in-process installed model; this is not an alternate production model path.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "test/mpas"))
from direct_two_step import verify_installed_wheel
from runtime_identity import verify_runtime


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    for name in ("configuration", "executable", "case", "runtime-receipt", "wheel",
                 "atlas-identity", "output-directory"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--kind", choices=("geometry-api", "variables"), required=True)
    parser.add_argument("--traversal-executable")
    args = parser.parse_args(argv)
    if args.kind == "geometry-api" and not args.traversal_executable:
        parser.error("geometry-api requires --traversal-executable")
    if args.kind == "variables" and args.traversal_executable:
        parser.error("variables does not run the geometry traversal benchmark")
    return args


def main():
    args = parse_args()
    receipt = json.loads(Path(args.runtime_receipt).read_text())
    verify_installed_wheel(Path(args.wheel), receipt["mpas_pytorch"]["wheel_sha256"])
    verify_runtime(Path(args.runtime_receipt))
    import torch
    from mpas_pytorch import load_config_from_namelist, load_initial_state, run_simulation
    from mpas_pytorch.ijedi_contracts import (
        build_configuration_receipt, build_state_storage_schema, load_ijedi_geometry_snapshot)
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    output = Path(args.output_directory)
    output.mkdir(parents=True, exist_ok=True)
    owned = Path(tempfile.mkdtemp(prefix="owner-support-", dir=output))
    case = Path(args.case)
    original = case / "namelist.atmosphere"
    config = load_config_from_namelist(original)
    dt = float(config["config_dt"])
    if dt != 720.:
        raise RuntimeError("support attack must start from the retained 720-second case")
    altered, count = re.subn(r"(?im)^(\s*config_dt\s*=\s*)[^,\n/]+",
                             r"\g<1>360.0", original.read_text())
    if count != 1:
        raise RuntimeError("support attack must change exactly one namelist timestep")
    namelist = owned / "namelist.atmosphere"
    namelist.write_text(altered)
    config = load_config_from_namelist(namelist)
    if float(config["config_dt"]) != 360.:
        raise RuntimeError("alternate model configuration did not select 360 seconds")
    init, grid = case / "x1.10242.init.nc", case / "x1.10242.grid.nc"
    initial, mesh = load_initial_state(init, grid, mesh_support_path=None, config=config)
    reference = run_simulation(initial, mesh, nsteps=1, config=config)
    configuration = build_configuration_receipt(namelist, config)
    geometry = load_ijedi_geometry_snapshot(init, grid, configuration_receipt=configuration)
    schema = build_state_storage_schema(
        reference, mesh, horizontal_geometry_receipt=geometry.horizontal_receipt,
        static_vertical_geometry_receipt=geometry.static_vertical_receipt,
        bundle_receipt=geometry.receipt, configuration_receipt=configuration)
    fixture = {
        "geometry_type": "mpas", "atlas compiler identity": args.atlas_identity,
        "python executable": sys.executable, "runtime receipt path": args.runtime_receipt,
        "wheel path": args.wheel, "wheel sha256": receipt["mpas_pytorch"]["wheel_sha256"],
        "init path": str(init), "grid path": str(grid), "namelist path": str(namelist),
        "horizontal geometry receipt": geometry.horizontal_receipt,
        "static vertical geometry receipt": geometry.static_vertical_receipt,
        "geometry bundle receipt": geometry.receipt, "configuration receipt": configuration,
        "state schema digest": schema.digest, "python version": receipt["runtime"]["python"],
        "torch version": receipt["runtime"]["torch"], "numpy version": receipt["runtime"]["numpy"],
        "netcdf4 version": receipt["runtime"]["netcdf4"],
        "mpas-pytorch version": receipt["mpas_pytorch"]["version"],
        "mpas-pytorch source commit": receipt["mpas_pytorch"]["source_commit"],
        "torch intraop threads": 1, "torch interop threads": 1,
        "vertical coordinate source": "level index"}
    config_path = owned / "api-contracts.yaml"
    text = Path(args.configuration).read_text()
    if "incompatible geometry:" in text:
        raise RuntimeError("support fixture must not overwrite an existing declaration")
    text += "\nincompatible geometry:\n" + "".join(
        "  " + json.dumps(key) + ": " + json.dumps(value) + "\n"
        for key, value in fixture.items())
    config_path.write_text(text)
    subprocess.run([args.executable, str(config_path)], check=True)

    if args.kind == "variables":
        return

    # Keep the public caller's complete traversal in the same required test.
    # The forced whole-mesh scan must trip the cost bound with identical points.
    cpu = min(os.sched_getaffinity(0))
    reports = []
    for forced in (False, True):
        label = "quadratic" if forced else "linear"
        report = output / ("traversal-" + label + ".json")
        configuration = output / ("traversal-" + label + ".yaml")
        configuration.write_text(Path(args.configuration).read_text() +
            "\nforced scan: " + str(forced).lower() +
            "\nprobe output: " + json.dumps(str(report)) + "\n")
        run = subprocess.run(["taskset", "-c", str(cpu), args.traversal_executable,
                              str(configuration)], capture_output=True, text=True, timeout=120)
        (output / ("traversal-" + label + ".log")).write_text(run.stdout + run.stderr)
        if not report.is_file():
            raise RuntimeError("public traversal failed before producing measurements: " +
                               run.stdout + run.stderr)
        data = json.loads(report.read_text())
        if (data["points"] != 10242 or data["mpi_ranks"] != 1 or
                data["threads"] != 1 or data["maximum_ratio"] != 256):
            raise RuntimeError("public traversal lost its declared measurement conditions")
        if forced:
            if (run.returncode != 1 or
                    "public traversal cost indicates per-point full-mesh scans" not in (run.stdout + run.stderr) or
                    data["complete_to_single_guard_ratio"] < 256):
                raise RuntimeError("quadratic traversal negative control did not reject by cost")
        elif run.returncode != 0 or not data["complete_to_single_guard_ratio"] < 256:
            raise RuntimeError("complete public traversal failed its retained cost bound")
        reports.append(data)
    if reports[0]["coordinate_receipt"] != reports[1]["coordinate_receipt"]:
        raise RuntimeError("traversal negative control changed its coordinate oracle")


if __name__ == "__main__":
    main()
