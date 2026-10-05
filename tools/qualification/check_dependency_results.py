# Copyright (C) 2026 IC Weather LLC
"""Check the complete configured dependency partition, without accepting skips."""
import argparse
import json
import re
from pathlib import Path

from run_p0_p2_release_gate import check_inventory, check_results


def verify_oops_installed_headers(prefix):
    """Resolve owned includes through real installed paths, never lexical collapse."""
    prefix = Path(prefix)
    roots = (prefix / "include", prefix / "include/oops")
    required = ("assimilation/DFSCalculator.h", "interface/NormGradient.h",
                "test/TestEnvironment.h")
    for name in required:
        if not (roots[1] / name).is_file():
            raise RuntimeError("missing required installed OOPS header: " + name)
    headers = list(roots[1].rglob("*.h"))
    references = 0
    missing = []
    for header in headers:
        names = re.findall(r'^\s*#\s*include\s*"((?:oops|test)/[^"\n]+)"',
                           header.read_text(), flags=re.M)
        references += len(names)
        for name in names:
            # is_file first matters: missing/../valid is not a usable compiler
            # path even when Path.resolve(strict=False) collapses it to valid.
            if not any((root / name).is_file()
                       for root in (header.parent, *roots)):
                missing.append((str(header), name))
    if missing:
        raise RuntimeError("unresolved installed OOPS includes: " + str(missing))
    return {"headers": len(headers), "references": references}


def dependency_names(inventory):
    all_names = check_inventory(inventory, set())
    owned = {test["name"] for test in inventory["tests"]
             if any(prop["name"] == "LABELS" and "ijedi" in prop["value"]
                    for prop in test.get("properties", []))}
    prefixed = {name for name in all_names if name.startswith("ijedi_")}
    if not owned or owned != prefixed or not all_names - owned:
        raise RuntimeError("dependency partition is empty or disagrees with actual ownership")
    return all_names - owned


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--installed-oops-prefix", type=Path)
    args = parser.parse_args()
    names = dependency_names(json.loads(args.inventory.read_bytes()))
    count = check_results(args.results, names)
    if args.installed_oops_prefix:
        print(verify_oops_installed_headers(args.installed_oops_prefix))
    print(f"All {count} configured dependency tests passed; zero skips")


if __name__ == "__main__":
    main()
