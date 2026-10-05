#!/usr/bin/env python3
"""Run the six unchanged registry tests against the exact installed owner.

The clean dependency checkout supplies tests only, never production imports.
Assertions, skips, xfails and missing/extra tests all fail this qualification.
"""
import argparse
import hashlib
import importlib.util
import inspect
import json
from pathlib import Path
import subprocess
import sys

from direct_two_step import verify_installed_wheel


TESTS = (
    "test_registry_species_domains_and_namespace_permissions",
    "test_registry_all_descriptors_and_aliases_are_closed_and_immutable",
    "test_every_descriptor_has_exact_atlas_shape_and_complete_binding",
    "test_all_bound_descriptor_properties_fail_on_mutation",
    "test_static_vertical_and_surface_semantics_are_distinct",
    "test_all_geoval_names_are_readonly_and_mass_definitions_are_explicit",
)


def authority(repo, commit):
    revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"], text=True)
    if revision != commit or dirty:
        raise RuntimeError("variable schema requires its declared clean owner release")


def main():
    parser = argparse.ArgumentParser()
    for name in ("wheel", "model-repo", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    for name in ("wheel-sha256", "source-commit"):
        parser.add_argument("--"+name, required=True)
    args = parser.parse_args()
    installed = verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256).parent
    authority(args.model_repo, args.source_commit)
    source = args.model_repo / "dycore-torch/mpas-pytorch/tests/test_variable_registry.py"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    spec = importlib.util.spec_from_file_location("installed_owner_registry_tests", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    found = {name for name in vars(module) if name.startswith("test_")}
    if found != set(TESTS):
        raise RuntimeError("variable-schema test inventory differs from the six frozen owner tests")
    for name in TESTS:
        test = getattr(module, name)
        if inspect.signature(test).parameters:
            raise RuntimeError("variable-schema test unexpectedly requires a fixture")
        test()
    modules = {}
    for name, imported in sorted(sys.modules.items()):
        if name == "mpas_pytorch" or name.startswith("mpas_pytorch."):
            path = Path(imported.__file__).resolve()
            if not path.is_relative_to(installed):
                raise RuntimeError("source-shadowed owner module: " + name)
            modules[name] = str(path.relative_to(installed))
    if not modules or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
        raise RuntimeError("schema source authority changed or no installed module was verified")
    authority(args.model_repo, args.source_commit)
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    report = dict(source_commit=args.source_commit, wheel_sha256=args.wheel_sha256,
                  test_source_sha256=digest, tests=list(TESTS), skips=0,
                  installed_modules=modules,
                  scope="complete_immutable_owner_registry_not_real_model_derivative_proof")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+"\n")
    print("Six unchanged installed-owner registry tests passed; zero skips")


if __name__ == "__main__":
    main()
