#!/usr/bin/env python3
"""Reject a retired geometry engine in active files, runtime, or linkage."""
import argparse
import json
from pathlib import Path
import subprocess


def active_source_files(source):
    """Scan owned code, including ignored scratch files in active code trees.

    A populated bundle contains independently pinned dependency checkouts.
    Generic unused discovery modules there are not I-JEDI dependencies. Scan
    all owned bundle files using Git's boundary, without a filename allowlist.
    Runtime imports and actual library linkage are checked separately below.
    """
    active = set()
    for directory in ("src", "test", "tools", "cmake", ".github"):
        active.update(p for p in (source / directory).rglob("*") if p.is_file())
    bundle = subprocess.check_output([
        "git", "-C", str(source), "ls-files", "-z", "--cached", "--others",
        "--exclude-standard", "--", "bundle"], text=True)
    active.update(source / name for name in bundle.split("\0") if name)
    active.add(source / "CMakeLists.txt")
    return sorted(p for p in active if p.is_file() and "__pycache__" not in p.parts and
                  (p.name == "CMakeLists.txt" or p.suffix in
                   (".cc", ".h", ".py", ".sh", ".cmake", ".yaml", ".yml", ".json")))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()
    markers = ("es" + "mf", "es" + "mpy", "ReceiptBound" + "SparseOperator")
    active = active_source_files(args.source)
    bad = [str(p.relative_to(args.source)) for p in active
           if any(marker.lower() in p.read_text().lower() for marker in markers)]
    if bad:
        raise RuntimeError(f"retired geometry references in active files: {sorted(set(bad))}")
    # Isolated normal installed imports, without the source checkout on sys.path.
    probe = '''import importlib.util, sys
import torch, netCDF4, mpas_pytorch
names = ("es" + "mpy", "ES" + "MF")
assert all(importlib.util.find_spec(name) is None for name in names)
assert all(not any(name.lower().startswith(prefix.lower()) for prefix in names)
           for name in sys.modules)
print("retired package absent; installed model imports successfully")
'''
    subprocess.run([str(args.python), "-I", "-c", probe], check=True)
    linkage = subprocess.check_output(["ldd", str(args.library)], text=True)
    if "not found" in linkage or any(marker.lower() in linkage.lower() for marker in markers):
        raise RuntimeError("unresolved or retired geometry linkage: " + linkage)
    print(json.dumps({"active_files_checked": len(set(active)),
                      "retired_runtime_absent": True, "retired_linkage_absent": True}))


if __name__ == "__main__":
    main()
