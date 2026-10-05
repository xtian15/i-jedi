#!/usr/bin/env python3
"""Serial multi-resolution analytic/complexity checks of the production operator."""
import argparse
import gc
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import tempfile

from direct_two_step import verify_installed_wheel
from validate_atlas_topology import export_atlas_snapshot


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "cases", "locations", "executable", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--atlas-compiler-identity", required=True)
    args = parser.parse_args()
    installed = verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    if Path(importlib.util.find_spec("mpas_pytorch").origin).resolve() != installed:
        raise RuntimeError("geometry scaling would import a shadowed model")
    import torch
    from mpas_pytorch import load_config_from_namelist
    from mpas_pytorch.ijedi_contracts import load_ijedi_geometry_snapshot, build_configuration_receipt
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    cases = json.loads(args.cases.read_text())
    if [case["cells"] for case in cases] != [10242, 40962, 163842]:
        raise AssertionError("scaling inventory must contain all three ordered native meshes")
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    reports = []
    for case in cases:
        config = load_config_from_namelist(case["namelist"])
        snapshot = load_ijedi_geometry_snapshot(
            case["init"], case["grid"],
            configuration_receipt=build_configuration_receipt(case["namelist"], config))
        if snapshot.horizontal.metadata["counts"]["nCells"] != case["cells"]:
            raise AssertionError("case extent disagrees with authenticated native mesh")
        source = args.output / f"model-snapshot-{case['cells']}.json"
        source.write_text(json.dumps(export_atlas_snapshot(snapshot), allow_nan=False))
        del snapshot
        gc.collect()
        for count in (256, 10000):
            locations = args.locations / f"locations_{count}.json"
            output = args.output / f"point-{case['cells']}-{count}.json"
            subprocess.run([str(args.executable), str(source), str(locations), str(output)], check=True)
            report = json.loads(output.read_text())
            if (report["source_cells"] != case["cells"] or report["targets"] != count or
                    report["atlas_compiler_identity"] != args.atlas_compiler_identity or
                    not 0 < report["nonzeros"] <= 3 * count or report["artifact_bytes"] > 1024 * count + 8192):
                raise AssertionError("scaled operator inventory/storage violates its sparse contract")
            values = report["analytic_rms"]
            if len(values) != 8 or any(not math.isfinite(x) or x <= 0 for x in values):
                raise AssertionError("analytic error inventory is invalid")
            if report["weighted_adjoint_max"] > 2.e-14 or not math.isfinite(report["weighted_adjoint_max"]):
                raise AssertionError("scaled operator fails its weighted adjoint bar")
            trials = report["apply_trials"]
            if (report["adjoint_seeds"] != 4 or len(trials) != 12 or
                    [(row["levels"], row["seed"]) for row in trials] !=
                    [(level, seed) for level in (1, 55, 56) for seed in range(4)] or
                    any(len(row["apply_seconds"]) != 5 or
                        any(not math.isfinite(t) or t < 0 for t in row["apply_seconds"])
                        for row in trials)):
                raise AssertionError("scaled operator lacks the required four-seed/repeat inventory")
            reports.append(report)
    refinement = {}
    for count in (256, 10000):
        rows = [row for row in reports if row["targets"] == count]
        ratios = [[a / b for a, b in zip(coarse["analytic_rms"], fine["analytic_rms"])]
                  for coarse, fine in zip(rows, rows[1:])]
        if any(not math.isfinite(ratio) or ratio <= 1 for row in ratios for ratio in row):
            raise AssertionError(f"analytic fields fail monotone refinement at {count} targets: {ratios}")
        refinement[str(count)] = ratios
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    report = {"scope": "serial native Atlas point interpolation, CPU float64",
              "atlas_compiler_identity": args.atlas_compiler_identity,
              "wheel_sha256": args.wheel_sha256, "runs": reports, "refinement_ratios": refinement}
    (args.output / "scaling-manifest.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"runs": len(reports), "minimum_refinement_ratio": min(
        ratio for rows in refinement.values() for row in rows for ratio in row)}))


if __name__ == "__main__":
    main()
