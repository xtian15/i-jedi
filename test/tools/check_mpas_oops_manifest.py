#!/usr/bin/env python3
"""Compare the OOPS-driven MPAS result with the frozen direct-Python oracle."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--oops", type=Path, required=True)
    parser.add_argument("--direct", type=Path, required=True)
    args = parser.parse_args()
    oops = json.loads(args.oops.read_text(encoding="utf-8"))
    direct = json.loads(args.direct.read_text(encoding="utf-8"))
    if oops["driver"] != "oops::Model<ijedi::Traits>::forecast":
        raise AssertionError("result was not emitted by the OOPS Model driver")
    if oops["initial_time"] != "2026-07-17T06:00:00Z":
        raise AssertionError(oops["initial_time"])
    if oops["step1_time"] != "2026-07-17T06:12:00Z":
        raise AssertionError(oops["step1_time"])
    if oops["step2_time"] != "2026-07-17T06:24:00Z":
        raise AssertionError(oops["step2_time"])
    if oops["initial_norm"] != oops["untouched_norm"]:
        raise AssertionError("OOPS forecast mutated its source clone")
    if not oops["serialized_restore_step2_bitwise_equal"]:
        raise AssertionError("serialized state did not reproduce the next step")
    if oops["serialized_double_count"] <= 0:
        raise AssertionError("serialized state payload was empty")
    expected_names = set(direct["step1"])
    if len(expected_names) != 29 or set(oops["step1"]) != expected_names:
        raise AssertionError("step-1 regression tensor inventory differs")
    if set(oops["step2"]) != set(direct["step2"]):
        raise AssertionError("step-2 regression tensor inventory differs")
    comparisons = 0
    for step in ("step1", "step2"):
        for name, expected in direct[step].items():
            actual = oops[step][name]
            for key in ("dtype", "shape", "sha256", "finite"):
                if actual[key] != expected[key]:
                    raise AssertionError(
                        f"{step}/{name}/{key}: {actual[key]!r} != {expected[key]!r}"
                    )
            comparisons += 1
    for key in ("continuation_step1", "continuation_step2"):
        if len(oops[key]) != 16:
            raise AssertionError(f"{key} has {len(oops[key])} tensors, expected 16")
    for key in ("typed_transform_step1", "typed_transform_step2"):
        if oops[key] != direct[key]:
            raise AssertionError(f"{key} differs between OOPS and direct Python")
        transform = oops[key]
        if len(transform["geovals"]) != 12:
            raise AssertionError(f"{key} does not contain all 12 typed GeoVaLs")
        if len(transform["descriptor_registry"]["digest"]) != 64:
            raise AssertionError(f"{key} descriptor registry is not receipted")
        if len(transform["trajectory_receipt"]["digest"]) != 64:
            raise AssertionError(f"{key} trajectory is not receipted")
    if oops["typed_transform_step1"]["trajectory_receipt"]["state_generation"] != 1:
        raise AssertionError("step-1 trajectory generation is not one")
    if oops["typed_transform_step2"]["trajectory_receipt"]["state_generation"] != 2:
        raise AssertionError("step-2 trajectory generation is not two")
    print(
        json.dumps(
            {
                "all_elementwise_bitwise_equal": True,
                "field_step_comparisons": comparisons,
                "continuation_tensor_count_each_step": 16,
                "source_clone_unchanged": True,
                "serialized_restore_step2_bitwise_equal": True,
                "valid_times_exact": True,
                "typed_transform_manifests_exact": True,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
