#!/usr/bin/env python3
"""Require an isolated MPAS/OOPS scenario to reproduce the producer's step 2."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--producer", type=Path, required=True)
    args = parser.parse_args()
    candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
    producer = json.loads(args.producer.read_text(encoding="utf-8"))
    for key in ("step2", "continuation_step2", "typed_transform_step2"):
        if candidate[key] != producer[key]:
            raise AssertionError(f"isolated scenario differs from producer: {key}")
    if len(candidate["step2"]) != 29:
        raise AssertionError("isolated scenario did not expose all 29 regression tensors")
    if len(candidate["continuation_step2"]) != 16:
        raise AssertionError("isolated scenario did not preserve all 16 continuation tensors")
    if len(candidate["typed_transform_step2"]["geovals"]) != 12:
        raise AssertionError("isolated scenario did not reproduce all 12 typed GeoVaLs")
    print(
        json.dumps(
            {
                "candidate_driver": candidate["driver"],
                "step2_bitwise_equal": True,
                "continuation_bitwise_equal": True,
                "typed_transform_exact": True,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
