#!/usr/bin/env python3
"""Require the OOPS MPAS constructor to reject altered runtime authorities."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.config.read_text(encoding="utf-8")

    def current(key: str) -> str:
        match = re.search(rf"^\s*{re.escape(key)}:\s*[\"']?([^\"'\n]+)[\"']?\s*$", source, re.MULTILINE)
        if match is None:
            raise AssertionError(f"generated OOPS config lacks key {key!r}")
        return match.group(1).strip()

    digest_keys = (
        "horizontal geometry receipt",
        "configuration receipt",
        "state schema digest",
    )
    attacks: list[tuple[str, str, str, str]] = [
        ("wheel_sha256", "wheel sha256", "0" * 64, "wheel digest mismatch"),
        ("python_version", "python version", "0.0.0", "dependency graph mismatch"),
        ("torch_version", "torch version", "0.0.0", "dependency graph mismatch"),
        ("numpy_version", "numpy version", "0.0.0", "dependency graph mismatch"),
        ("netcdf4_version", "netcdf4 version", "0.0.0", "dependency graph mismatch"),
        ("mpas_version", "mpas-pytorch version", "0.0.0", "dependency graph mismatch"),
        ("atlas_compiler_identity", "atlas compiler identity", "0" * 40,
         "Atlas compiler does not match the loaded library"),
        (
            "source_commit",
            "mpas-pytorch source commit",
            "0" * 40,
            "runtime receipt does not match",
        ),
        (
            "model_timestep",
            "time step",
            "PT13M",
            "timestep differs",
        ),
    ]
    for key in digest_keys:
        value = current(key)
        attacks.append(
            (
                key.replace(" ", "_"),
                key,
                ("0" if value[0] != "0" else "1") + value[1:],
                {
                    "horizontal geometry receipt": "geometry receipt mismatch",
                    "configuration receipt": "configuration receipt mismatch",
                    "state schema digest": "state schema digest mismatch",
                }[key],
            )
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    results: dict[str, dict[str, object]] = {}
    with tempfile.TemporaryDirectory(
        prefix="mpas-runtime-attacks-", dir=args.output.parent
    ) as temporary_directory:
        temporary = Path(temporary_directory)
        for label, key, replacement, expected_fragment in attacks:
            pattern = re.compile(
                rf"^(\s*{re.escape(key)}:\s*).*$", re.MULTILINE
            )
            candidate, count = pattern.subn(
                lambda match: match.group(1) + json.dumps(replacement), source
            )
            if count != 1:
                raise AssertionError(f"attack {label} replaced {count} config entries")
            candidate = re.sub(
                r'^output manifest:\s*.*$',
                "output manifest: " + json.dumps(str(temporary / f"{label}.json")),
                candidate,
                count=1,
                flags=re.MULTILINE,
            )
            candidate_path = temporary / f"{label}.yaml"
            candidate_path.write_text(candidate, encoding="utf-8")
            environment = dict(os.environ)
            environment.pop("PYTHONPATH", None)
            completed = subprocess.run(
                [str(args.executable), str(candidate_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=environment,
                timeout=180,
                check=False,
            )
            if completed.returncode == 0:
                raise AssertionError(f"runtime identity attack was accepted: {label}")
            if expected_fragment not in completed.stdout:
                raise AssertionError(
                    f"runtime identity attack {label} failed for the wrong reason; "
                    f"expected {expected_fragment!r}, output was:\n{completed.stdout[-4000:]}"
                )
            results[label] = {
                "rejected": True,
                "expected_error_fragment": expected_fragment,
                "return_code": completed.returncode,
            }

    report = {
        "schema_version": 1,
        "attack_count": len(results),
        "attacks": results,
    }
    temporary_output = args.output.with_name(args.output.name + ".tmp")
    temporary_output.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    temporary_output.replace(args.output)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
