#!/usr/bin/env python3
"""Both physical native/Atlas directions at all three native resolutions.

The production model decoder owns geometry; the existing production Atlas
operator owns coefficients. Independent integrals/adjoints run in the C++ gate.
"""
import argparse
import copy
import gc
import json
import math
from pathlib import Path
import subprocess
import tempfile

from direct_two_step import verify_installed_wheel
from validate_atlas_topology import export_atlas_snapshot


def validate(report, cells, compiler, restoring):
    if (report["source_cells"] != cells or report["atlas_compiler_identity"] != compiler or
            report["restoring"] is not restoring or report["peak_rss_bytes"] <= 0):
        raise RuntimeError("conservative scaling context differs from its declared native case")
    for direction, source, target in (("forward", cells, 3116), ("reverse", 3116, cells)):
        row = report[direction]
        if (row["source_cells"] != source or row["target_cells"] != target or
                row["analytic_fields"] != 8 or row["seeds"] != 4 or row["levels"] != [1, 55, 56] or
                row["cache_negative_controls"] != 6 or not 0 < row["nnz"] <= 32*(source+target)):
            raise RuntimeError("conservative scaling omits a physical direction or mandatory case")
        for key in ("row_max", "column_max", "analytic_integral_max", "weighted_adjoint_max"):
            if not math.isfinite(row[key]) or not 0 <= row[key] <= 2.e-14:
                raise RuntimeError("conservative scaling fails the frozen conservation/adjoint bar")
        trials = row["apply_trials"]
        if (len(trials) != 12 or [(trial["levels"], trial["seed"]) for trial in trials] !=
                [(level, seed) for level in (1, 55, 56) for seed in range(4)] or
                any(len(trial["apply_seconds"]) != 5 or any(not math.isfinite(value) or value < 0
                    for value in trial["apply_seconds"]) for trial in trials) or
                not math.isfinite(row["setup_seconds"]) or row["setup_seconds"] < 0):
            raise RuntimeError("conservative scaling lacks its four-seed/five-repeat timings")


def science(report):
    result = copy.deepcopy(report)
    for key in ("restoring", "peak_rss_bytes"): result.pop(key)
    for direction in ("forward", "reverse"):
        for key in ("setup_seconds", "apply_trials"): result[direction].pop(key)
    return result


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "cases", "executable", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    for name in ("wheel-sha256", "atlas-compiler-identity"):
        parser.add_argument("--"+name, required=True)
    args = parser.parse_args()
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    import torch
    from mpas_pytorch import load_config_from_namelist
    from mpas_pytorch.ijedi_contracts import load_ijedi_geometry_snapshot, build_configuration_receipt
    torch.set_num_threads(1); torch.set_num_interop_threads(1)
    cases = json.loads(args.cases.read_text())
    if [case["cells"] for case in cases] != [10242, 40962, 163842]:
        raise RuntimeError("conservative scaling requires all three ordered native resolutions")
    # Fresh directories prevent old evidence from satisfying a failed child.
    args.output.mkdir(parents=True, exist_ok=True)
    campaign = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    reports = []
    for case in cases:
        config = load_config_from_namelist(case["namelist"])
        geometry = load_ijedi_geometry_snapshot(case["init"], case["grid"],
            configuration_receipt=build_configuration_receipt(case["namelist"], config))
        if geometry.horizontal.metadata["counts"]["nCells"] != case["cells"]:
            raise RuntimeError("declared scaling extent differs from the owned native mesh")
        source = campaign/f"model-snapshot-{case['cells']}.json"
        source.write_text(json.dumps(export_atlas_snapshot(geometry), allow_nan=False))
        del geometry; gc.collect()
        output = campaign/f"conservative-{case['cells']}.json"
        subprocess.run([str(args.executable), str(source), str(output), "--conservative-scaling"],
                       check=True, timeout=14400)
        compiled = json.loads(output.read_text())
        validate(compiled, case["cells"], args.atlas_compiler_identity, False)
        trusted = {"analysis-"+direction: compiled[direction]["coefficient_receipt"]
                   for direction in ("forward", "reverse")}
        fresh = campaign/f"conservative-{case['cells']}-fresh.json"
        subprocess.run([str(args.executable), str(source), str(fresh), "--conservative-scaling",
            str(output), json.dumps(trusted)], check=True, timeout=14400)
        restored = json.loads(fresh.read_text())
        validate(restored, case["cells"], args.atlas_compiler_identity, True)
        if science(compiled) != science(restored):
            raise RuntimeError("fresh-process conservative results differ from the compiling process")
        for direction in ("forward", "reverse"):
            a = Path(str(output)+f".analysis-{direction}.cache.json")
            b = Path(str(fresh)+f".analysis-{direction}.cache.json")
            if a.read_bytes() != b.read_bytes() or a.stat().st_size > 1000*(case["cells"]+3116)+8192:
                raise RuntimeError("conservative cache differs across processes or violates sparse storage")
            compiled[direction]["artifact_bytes"] = a.stat().st_size
        reports.append(compiled)
        print(json.dumps(dict(completed_resolutions=len(reports), native_cells=case["cells"])), flush=True)
    verify_installed_wheel(args.wheel, args.wheel_sha256)
    manifest = campaign/"scaling-manifest.json"
    manifest.write_text(json.dumps(dict(
        scope="serial_global_native_Atlas_conservative_geometry_not_model_forecasts",
        wheel_sha256=args.wheel_sha256, atlas_compiler_identity=args.atlas_compiler_identity,
        native_resolutions=3, physical_maps=6, fresh_process_maps=6, runs=reports), indent=2, allow_nan=False)+"\n")
    print(json.dumps(dict(manifest=str(manifest), native_resolutions=3, physical_maps=6,
        fresh_process_maps=6)), flush=True)


if __name__ == "__main__": main()
