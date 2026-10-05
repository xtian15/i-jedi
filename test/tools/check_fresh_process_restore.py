#!/usr/bin/env python3
"""Require a separate-process MPAS restore to reproduce the producer's step two."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--consumer", type=Path, required=True)
    args = parser.parse_args()

    producer = json.loads(args.producer.read_text(encoding="utf-8"))
    consumer = json.loads(args.consumer.read_text(encoding="utf-8"))
    for key in ("step2_time", "serialized_double_count", "step2", "continuation_step2",
                "typed_transform_step2"):
        if consumer.get(key) != producer.get(key):
            raise AssertionError(f"fresh-process restore mismatch for {key}")
    if consumer.get("driver") != "fresh-process-consumer":
        raise AssertionError("consumer manifest does not identify the fresh-process path")
    if producer.get("serialization_attack_count") != 5:
        raise AssertionError("producer did not execute the frozen framing attack set")
    print("fresh-process step2, continuation and typed manifests agree exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
