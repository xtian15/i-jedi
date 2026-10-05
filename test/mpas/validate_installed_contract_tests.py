#!/usr/bin/env python3
"""Run preserved contract inventories against the authenticated installed wheel.

Source checkout paths provide test fixtures and parity/corpus guards only.
Every imported production module must remain inside the installed distribution.
Skipped, xfailed, empty, or unexpectedly small inventories fail qualification.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys

from direct_two_step import verify_installed_wheel


class Results:
    def __init__(self):
        self.collected = []
        self.passed = []
        self.nonpasses = []
        self.node_names = {}

    def pytest_collection_finish(self, session):
        for item in session.items:
            # Two authenticated test roots retain their original logical IDs.
            # Files have distinct basenames; collisions are hard failures.
            suffix = item.nodeid.partition("::")[2]
            logical = "tests/" + Path(item.path).name + "::" + suffix
            if logical in self.node_names.values():
                raise RuntimeError("duplicate retained/additive test identity: " + logical)
            self.node_names[item.nodeid] = logical
        self.collected = sorted(self.node_names.values())

    def pytest_runtest_logreport(self, report):
        if report.failed or report.skipped or hasattr(report, "wasxfail"):
            self.nonpasses.append([self.node_names.get(report.nodeid, report.nodeid),
                                   report.when, report.outcome])
        elif report.when == "call" and report.passed:
            self.passed.append(self.node_names[report.nodeid])


def clean_authority(repo, expected):
    revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"], text=True)
    if revision != expected or dirty:
        raise RuntimeError("contract test checkout is not the declared clean commit")
    return revision


def source_hashes(repo, files):
    return {str(p.relative_to(repo.resolve())): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in files}


def check_retained_sources(original, retained, hashes, corrections=None):
    if len(original["tests"]) != 56 or original["skips"] != 0:
        raise RuntimeError("qualification baseline is not the complete original 56-test profile")
    if len(retained["tests"]) != 71 or retained["skips"] != 0:
        raise RuntimeError("retained qualification is not the complete 71-test profile")
    if not set(original["tests"]).issubset(retained["tests"]):
        raise RuntimeError("retained qualification lost original tests")
    changes = {}
    if corrections is not None:
        allowed = {"dycore-torch/mpas-pytorch/tests/" + name for name in (
            "test_assimilation_variables.py", "test_ijedi_contracts_real.py",
            "test_variable_registry.py", "test_analysis_coordinates_real.py")}
        if (corrections.get("schema_version") != 1 or
                corrections.get("original_source_commit") != retained["source_commit"] or
                corrections.get("vader_source_commit") != "cb75e639ca09da1b132a777a81f6fe56209f64dd" or
                set(corrections.get("test_sources", {})) != allowed):
            raise RuntimeError("reference correction is not the scoped humidity semantics and finite-difference authority")
        changes = corrections["test_sources"]
        for name, change in changes.items():
            if (retained["test_sources"].get(name) != change["original_sha256"] or
                    hashes.get(name) != change["corrected_sha256"] or
                    change["original_sha256"] == change["corrected_sha256"]):
                raise RuntimeError("reference correction provenance differs: " + name)
    for receipt in (original, retained):
        for name, digest in receipt["test_sources"].items():
            if name in changes:
                if digest != changes[name]["original_sha256"]:
                    raise RuntimeError("reference correction lost an original source: " + name)
            elif hashes.get(name) != digest:
                raise RuntimeError(f"original contract test source changed: {name}")


def run_profile(spec_path):
    """One installed engine, one upstream test root, in a fresh interpreter.

    Upstream owns a top-level 'tests' package. Separate processes preserve both
    conftests unchanged instead of bypassing guards to resolve their namespace
    collision. This worker result alone is never a complete qualification.
    """
    spec = json.loads(spec_path.read_text())
    installed = verify_installed_wheel(Path(spec["wheel"]), spec["wheel_sha256"]).parent
    import mpas_pytorch
    import pytest
    import torch
    if Path(mpas_pytorch.__file__).resolve().parent != installed:
        raise RuntimeError("source-shadowed production import before profile execution")
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    sys.path.append(spec["package_source"])
    results = Results()
    status = pytest.main((["--collect-only"] if spec["collect_only"] else []) +
        ["--import-mode=importlib", "-q", "--maxfail=1", "--tb=short",
         "-o", "cache_dir=" + spec["cache"],
         "--junitxml=" + spec["xml"], *spec["files"]], plugins=[results])
    modules = {}
    for name, module in sorted(sys.modules.items()):
        if name == "mpas_pytorch" or name.startswith("mpas_pytorch."):
            path = Path(module.__file__).resolve()
            if not path.is_relative_to(installed):
                raise RuntimeError(f"source-shadowed production module: {name}: {path}")
            modules[name] = str(path.relative_to(installed))
    verify_installed_wheel(Path(spec["wheel"]), spec["wheel_sha256"])
    Path(spec["report"]).write_text(json.dumps(dict(
        scope="profile_worker_not_complete_qualification", status=status,
        collected=results.collected, passed=sorted(results.passed),
        nonpasses=results.nonpasses, installed_modules=modules), indent=2) + "\n")
    if status != 0 or results.nonpasses:
        raise RuntimeError("installed profile failed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--model-repo", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--expected-tests", type=int, required=True)
    parser.add_argument("--baseline-receipt", type=Path, required=True,
                        help="Retained original installed qualification; preserve all its tests and source bytes")
    parser.add_argument("--retained-receipt", type=Path, required=True,
                        help="The complete 71-test qualification being extended, never replaced")
    parser.add_argument("--test-authority-repo", type=Path, required=True,
                        help="Clean retained test checkout; separate from the current wheel source")
    parser.add_argument("--collect-only", action="store_true")
    parser.add_argument("--reference-corrections", type=Path,
                        help="Explicit scoped correction of the demonstrated humidity semantics and finite-difference reference defects")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.expected_tests != 291:
        raise RuntimeError("the complete current installed qualification has exactly 291 tests")
    runner_digest = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    for name in ("MPAS_IJEDI_INIT", "MPAS_IJEDI_GRID", "MPAS_IJEDI_NAMELIST"):
        value = os.environ.get(name)
        if not value or not Path(value).is_file():
            raise RuntimeError(f"required real-case contract input is missing: {name}")
    if os.environ.get("MPAS_RUN_SLOW") != "1":
        raise RuntimeError("installed qualification requires MPAS_RUN_SLOW=1")
    installed = verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256).parent
    revision = clean_authority(args.model_repo, args.source_commit)
    baseline_bytes = args.baseline_receipt.read_bytes()
    retained_bytes = args.retained_receipt.read_bytes()
    baseline, retained = json.loads(baseline_bytes), json.loads(retained_bytes)
    corrections_bytes = args.reference_corrections.read_bytes() if args.reference_corrections else None
    corrections = json.loads(corrections_bytes) if corrections_bytes is not None else None
    test_revision = clean_authority(args.test_authority_repo,
        corrections["corrected_source_commit"] if corrections else retained["source_commit"])

    package_source = args.model_repo.resolve() / "dycore-torch/mpas-pytorch"
    test_source = args.test_authority_repo.resolve() / "dycore-torch/mpas-pytorch"
    # Append, never prepend: the installed production package is authoritative.
    # Retain upstream conftest and its unchanged parity and corpus guards.
    test_files = [test_source / "tests" / name for name in (
        "test_assimilation_variables.py", "test_ijedi_contracts.py",
        "test_ijedi_contracts_real.py", "test_analysis_coordinates_real.py",
        "test_variable_registry.py", "test_analysis_spaces_real.py",
        "test_analysis_native_replacement_real.py")]
    additive_files = [package_source / "tests" / name for name in (
        "test_prepared_boundary_identity_real.py", "test_portable_continuation_real.py",
        "test_continuation_marker_contract.py", "test_ijedi_owner_input_hardening_real.py",
        "test_fd_validation.py")]
    ingress_files = [package_source / "tests" / name for name in (
        "test_public_runtime_ingress.py", "test_public_runtime_ingress_real.py")]
    retained_files = test_files + [test_source / "tests/conftest.py"]
    current_files = additive_files + ingress_files + [package_source / "tests/conftest.py",
                                     package_source / "tests/test_ic_loader.py"]
    retained_hashes = source_hashes(args.test_authority_repo, retained_files)
    additive_hashes = source_hashes(args.model_repo, current_files)
    check_retained_sources(baseline, retained, retained_hashes, corrections)
    results = Results()
    os.environ["MPAS_ANALYSIS_PACKAGE_IDENTITY"] = json.dumps({
        "version": importlib.metadata.version("mpas-pytorch"), "source_commit": args.source_commit,
        "wheel_sha256": args.wheel_sha256}, sort_keys=True)
    os.environ["MPAS_ANALYSIS_SPACES_RECEIPT"] = str(args.output.with_suffix(".owned-spaces"))
    fd_output = args.output.with_suffix(".native-fd.json")
    evidence_outputs = [fd_output] + [Path(os.environ["MPAS_ANALYSIS_SPACES_RECEIPT"] + suffix)
        for suffix in (".P.json", ".P-native-geoval.json")]
    if any(path.exists() for path in evidence_outputs):
        raise RuntimeError("installed qualification requires fresh, nonexisting derivative evidence paths")
    os.environ["MPAS_ANALYSIS_EVIDENCE"] = str(fd_output)
    if args.output.exists():
        raise RuntimeError("installed qualification output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    modules, profile_evidence = {}, {}
    for label, source, files, expected_count in (
            ("ingress", package_source, ingress_files, 215),
            ("additive", package_source, additive_files, 5),
            ("retained", test_source, test_files, 71)):
        spec_path = args.output.with_suffix("." + label + ".job.json")
        report_path = args.output.with_suffix("." + label + ".result.json")
        xml_path = args.output.with_suffix("." + label + ".xml")
        if any(path.exists() for path in (spec_path, report_path, xml_path)):
            raise RuntimeError("profile requires fresh evidence paths")
        spec_path.write_text(json.dumps(dict(
            wheel=str(args.wheel.resolve()), wheel_sha256=args.wheel_sha256,
            package_source=str(source), files=[str(path) for path in files],
            collect_only=args.collect_only, report=str(report_path), xml=str(xml_path),
            cache=str(args.output.with_suffix("." + label + ".pytest-cache"))), indent=2) + "\n")
        subprocess.run([sys.executable, str(Path(__file__).resolve()),
                        "--_worker-spec", str(spec_path)], check=True)
        report = json.loads(report_path.read_text())
        if (report["status"] != 0 or report["nonpasses"] or
                len(report["collected"]) != expected_count or
                len(set(report["collected"])) != expected_count or
                (not args.collect_only and report["passed"] != report["collected"])):
            raise RuntimeError("installed profile lost required tests or nonpassing checks")
        results.collected.extend(report["collected"])
        results.passed.extend(report["passed"])
        modules.update(report["installed_modules"])
        for path in (spec_path, report_path, xml_path):
            profile_evidence[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    status = 0
    results.collected.sort()
    if (source_hashes(args.test_authority_repo, retained_files) != retained_hashes or
            source_hashes(args.model_repo, current_files) != additive_hashes or
            clean_authority(args.model_repo, revision) != revision or
            clean_authority(args.test_authority_repo, test_revision) != test_revision or
            args.baseline_receipt.read_bytes() != baseline_bytes or
            args.retained_receipt.read_bytes() != retained_bytes or
            (args.reference_corrections and args.reference_corrections.read_bytes() != corrections_bytes)):
        raise RuntimeError("qualification source authority changed during the test run")
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)
    if hashlib.sha256(Path(__file__).read_bytes()).hexdigest() != runner_digest:
        raise RuntimeError("installed qualification runner changed during execution")
    if args.collect_only:
        if (status != 0 or len(results.collected) != 291 or results.nonpasses or
                not set(retained["tests"]).issubset(results.collected)):
            raise RuntimeError("collection lost the complete retained/additive profile")
        print(json.dumps(dict(scope="collection_only_not_qualification", collected=291,
            retained=71, additive=5, ingress=215, tests=results.collected)))
        return
    if (status != 0 or results.nonpasses or len(results.collected) != args.expected_tests
            or sorted(results.passed) != results.collected or len(modules) < 2
            or not set(retained["tests"]).issubset(results.collected)):
        raise RuntimeError(f"installed contract qualification failed: status={status}, "
                           f"collected={len(results.collected)}, passed={len(results.passed)}, "
                           f"nonpasses={results.nonpasses}")
    fd = json.loads(fd_output.read_text())
    if len(fd["trials"]) != 144:
        raise RuntimeError("installed qualification lost the complete native derivative seed matrix")
    for path in evidence_outputs[1:]:
        report = json.loads(path.read_text())
        if len(report["cases"]) != 12 or report["package_identity"] != json.loads(os.environ["MPAS_ANALYSIS_PACKAGE_IDENTITY"]):
            raise RuntimeError("owned analysis-space evidence lacks all authenticated boundary/seed pairs")
    receipt = {"source_commit": revision, "wheel_sha256": args.wheel_sha256,
               "test_sources": retained_hashes, "additive_test_sources": additive_hashes,
               "retained_test_source_commit": test_revision, "tests": results.collected,
               "reference_correction_sha256": hashlib.sha256(corrections_bytes).hexdigest()
                   if corrections_bytes is not None else None,
               "installed_modules": modules, "skips": 0,
               "baseline_receipt_sha256": hashlib.sha256(args.baseline_receipt.read_bytes()).hexdigest(),
               "retained_receipt_sha256": hashlib.sha256(retained_bytes).hexdigest(),
               "retained_tests": len(retained["tests"]),
               "original_tests_retained": len(baseline["tests"]),
               "runner_sha256": runner_digest,
               "profile_evidence": profile_evidence,
               "derivative_evidence": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in evidence_outputs},
               "test_tools": {name: importlib.metadata.version(name)
                              for name in ("pytest", "pluggy", "iniconfig", "pygments", "packaging")}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"installed_contract_tests": len(results.passed),
                      "installed_modules": len(modules), "skips": 0}))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--_worker-spec":
        run_profile(Path(sys.argv[2]))
    else:
        main()
