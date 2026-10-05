#!/usr/bin/env python3
"""Attack the registration/result gate; no scientific qualification implied."""
import copy
import importlib.util
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("release_gate", ROOT / "tools/qualification/run_p0_p2_release_gate.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
SCAN_SPEC = importlib.util.spec_from_file_location(
    "retired_geometry_scan", ROOT / "test/tools/check_retired_geometry_absent.py")
SCAN = importlib.util.module_from_spec(SCAN_SPEC)
SCAN_SPEC.loader.exec_module(SCAN)
ARTIFACT_SPEC = importlib.util.spec_from_file_location(
    "runtime_artifacts", ROOT / "tools/mpas_bridge/verify_runtime_artifacts.py")
ARTIFACTS = importlib.util.module_from_spec(ARTIFACT_SPEC)
ARTIFACT_SPEC.loader.exec_module(ARTIFACTS)
PROVIDER_SPEC = importlib.util.spec_from_file_location(
    "sdk_provider", ROOT / "tools/mpas_bridge/package_sdk_bufr_provider.py")
PROVIDER = importlib.util.module_from_spec(PROVIDER_SPEC)
PROVIDER_SPEC.loader.exec_module(PROVIDER)
sys.path.insert(0, str(ROOT / "tools/qualification"))
from check_dependency_results import dependency_names, verify_oops_installed_headers


class InstalledOOPSHeaderTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.prefix = Path(directory.name)
        self.headers = self.prefix / "include/oops"
        for name in ("assimilation/DFSCalculator.h", "interface/NormGradient.h",
                     "test/TestEnvironment.h"):
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('// fixture\n')
        self.probe = self.headers / "test/Probe.h"
        self.probe.write_text('#include "test/TestEnvironment.h"\n')

    def test_exported_roots(self):
        result = verify_oops_installed_headers(self.prefix)
        self.assertEqual(result, {"headers": 4, "references": 1})

    def test_nonexistent_intermediate_is_not_collapsed(self):
        self.probe.write_text('#include "oops/../test/TestEnvironment.h"\n')
        with self.assertRaisesRegex(RuntimeError, "unresolved installed"):
            verify_oops_installed_headers(self.prefix)

    def test_required_header_removal(self):
        for name in ("assimilation/DFSCalculator.h", "interface/NormGradient.h",
                     "test/TestEnvironment.h"):
            path = self.headers / name
            original = path.read_bytes()
            path.unlink()
            with self.assertRaisesRegex(RuntimeError, "missing required"):
                verify_oops_installed_headers(self.prefix)
            path.write_bytes(original)


class SDKProviderTests(unittest.TestCase):
    """Packaging identity attacks; these fake bytes are not ABI qualification."""
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.sdk = self.root / "sdk"
        self.source = self.sdk / "store/bufr-query-0.0.4-fixture/lib/python3.11/site-packages/bufr"
        self.source.mkdir(parents=True)
        view = self.sdk / "view/lib/python3.11/site-packages"
        view.mkdir(parents=True)
        (view / "bufr").symlink_to(self.source, target_is_directory=True)
        self.members = {"__init__.py": b"preserved source notice\n",
                        "bufr_python.cpython-311-aarch64-linux-gnu.so": b"non-scientific fixture"}
        for name, value in self.members.items():
            (self.source / name).write_bytes(value)
        self.identity = dict(sdk_spec_component="bufr-query-0.0.4-fixture",
            sdk_receipt_sha256="0" * 64,
            members={"bufr/" + name: hashlib.sha256(value).hexdigest()
                     for name, value in self.members.items()})

    def test_deterministic_bytes_and_preserved_provider_members(self):
        a = PROVIDER.build_provider(self.sdk, self.root / "a.whl", self.identity)
        b = PROVIDER.build_provider(self.sdk, self.root / "b.whl", self.identity)
        self.assertEqual(a["sha256"], b["sha256"])
        with ZipFile(self.root / "a.whl") as wheel:
            for name, value in self.members.items():
                self.assertEqual(wheel.read("bufr/" + name), value)
            self.assertEqual(len(wheel.namelist()), len(self.members) + 4)

    def test_modified_missing_extra_members_reject(self):
        member = self.source / "__init__.py"
        member.write_bytes(self.members["__init__.py"] + b"changed")
        with self.assertRaisesRegex(RuntimeError, "differs from exact authority"):
            PROVIDER.build_provider(self.sdk, self.root / "changed.whl", self.identity)
        member.unlink()
        with self.assertRaisesRegex(RuntimeError, "inventory differs"):
            PROVIDER.build_provider(self.sdk, self.root / "missing.whl", self.identity)
        member.write_bytes(self.members["__init__.py"])
        (self.source / "extra.py").write_bytes(b"undeclared")
        with self.assertRaisesRegex(RuntimeError, "differs from exact authority"):
            PROVIDER.build_provider(self.sdk, self.root / "extra.whl", self.identity)
        for name in ("changed", "missing", "extra"):
            self.assertFalse((self.root / (name + ".whl")).exists())

    def test_provider_symlink_escape_rejects_identical_bytes(self):
        member = self.source / "__init__.py"
        outside = self.root / "outside.py"
        outside.write_bytes(member.read_bytes())
        member.unlink()
        member.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            PROVIDER.build_provider(self.sdk, self.root / "escape.whl", self.identity)
        self.assertFalse((self.root / "escape.whl").exists())

    def test_existing_output_is_preserved_and_live_pin_is_consistent(self):
        output = self.root / "existing.whl"
        output.write_bytes(b"only copy")
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            PROVIDER.build_provider(self.sdk, output, self.identity)
        self.assertEqual(output.read_bytes(), b"only copy")
        receipt = json.loads((ROOT / "tools/mpas_bridge/p0_runtime_receipt.json").read_bytes())
        provider = json.loads((ROOT / "cmake/MpasDependencyArtifacts.json").read_bytes())["bufr_sdk_provider"]
        self.assertEqual(receipt["sdk_python_provider"]["wheel_sha256"], provider["wheel_sha256"])
        self.assertEqual(receipt["requirements_sha256"], hashlib.sha256(
            (ROOT / "tools/mpas_bridge/p0-runtime-linux-aarch64.requirements.txt").read_bytes()).hexdigest())
        self.assertEqual(len(ARTIFACTS.read_lock(
            ROOT / "tools/mpas_bridge/p0-runtime-linux-aarch64.requirements.txt")), 28)


class PocketfftInputTests(unittest.TestCase):
    """Exercise real CMake input binding with controlled non-scientific artifacts."""
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.input = self.root / "input"
        self.input.mkdir()
        self.members = {"pocketfft_hdronly.h": b"controlled header fixture",
                        "LICENSE.md": b"controlled license fixture"}
        for name, content in self.members.items():
            (self.input / name).write_bytes(content)
        shutil.copyfile(ROOT / "cmake/BindPocketfft.cmake", self.root / "BindPocketfft.cmake")
        (self.root / "MpasDependencyArtifacts.json").write_text(json.dumps(dict(
            pocketfft=dict(members={name: hashlib.sha256(content).hexdigest()
                                    for name, content in self.members.items()}))))
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.23)\nproject(pocketfft_input_probe NONE)\n'
            'include("${CMAKE_CURRENT_SOURCE_DIR}/BindPocketfft.cmake")\n')

    def configure(self, extra=()):
        return subprocess.run(["cmake", "-S", str(self.root), "-B", str(self.root / "build"),
            "-DIJEDI_POCKETFFT_SOURCE_DIR=" + str(self.input), *extra],
            capture_output=True, text=True, timeout=15)

    def test_verified_input_prevents_cached_path_and_feature_drift(self):
        result = self.configure(("-Dpocketfft_INCLUDE_DIR=/foreign/header",
                                 "-DENABLE_POCKETFFT=OFF"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cache = (self.root / "build/CMakeCache.txt").read_text()
        self.assertIn("pocketfft_INCLUDE_DIR:PATH=" + str(self.input), cache)
        self.assertIn("ENABLE_POCKETFFT:BOOL=ON", cache)

    def test_missing_and_changed_header_and_license_reject(self):
        for name, content in self.members.items():
            member = self.input / name
            member.write_bytes(content + b"changed")
            result = self.configure()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("differs from exact dependency receipt", result.stderr)
            member.unlink()
            result = self.configure()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Missing pinned PocketFFT member", result.stderr)
            member.write_bytes(content)
        result = self.configure(("-DIJEDI_POCKETFFT_SOURCE_DIR=",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires the pinned PocketFFT", result.stderr)

    def test_production_receipt_and_bundle_binding_are_declared(self):
        receipt = json.loads((ROOT / "cmake/MpasDependencyArtifacts.json").read_bytes())
        self.assertEqual(receipt["schema_version"], 1)
        self.assertEqual(receipt["pocketfft"]["source_commit"],
                         "0fa0ef591e38c2758e3184c6c23e497b9f732ffa")
        self.assertEqual(set(receipt["pocketfft"]["members"]),
                         {"pocketfft_hdronly.h", "LICENSE.md"})
        self.assertIn("include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BindPocketfft.cmake)",
                      (ROOT / "bundle/CMakeLists.txt").read_text())


class RuntimeArtifactTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.wheels, self.install = self.root / "wheels", self.root / "install"
        self.wheels.mkdir()
        self.install.mkdir()
        self.lock = self.root / "requirements.txt"
        self.wheel = self.wheels / "sample.whl"
        self.members = [
            ("sample/__init__.py", b"immutable package\n"),
            ("sample-1.0.dist-info/METADATA", b"Name: sample\nVersion: 1.0\n"),
            ("sample-1.0.dist-info/RECORD", b"installer-regenerated record\n"),
            ("sample-1.0.data/data/share/sample.txt", b"immutable relocated data\n")]
        self.distribution = SimpleNamespace(metadata={"Name": "sample"}, version="1.0",
                                            locate_file=lambda name: self.install / name)
        self.write_wheel(self.members)
        for member, content in self.members:
            relative = member.split(".data/data/", 1)[-1] if ".data/" in member else member
            target = self.install / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)

    def write_wheel(self, members):
        with ZipFile(self.wheel, "w") as archive:
            for name, content in members:
                archive.writestr(name, content)
        self.lock.write_text("sample==1.0 --hash=sha256:" +
                             hashlib.sha256(self.wheel.read_bytes()).hexdigest() + "\n")

    def verify(self, distributions=None):
        with patch.object(ARTIFACTS.metadata, "distributions",
                          return_value=[self.distribution] if distributions is None else distributions), \
                patch.object(ARTIFACTS.sysconfig, "get_path", return_value=str(self.install)):
            return ARTIFACTS.verify_artifacts(self.lock, self.wheels)

    def test_all_members_and_relocated_data_are_authenticated(self):
        result = self.verify()
        self.assertEqual(result["distributions"]["sample"]["installed_members_checked"], 3)
        # RECORD is generated by pip. It cannot replace retained member bytes.
        (self.install / "sample-1.0.dist-info/RECORD").write_text("different installer RECORD")
        self.verify()

    def test_dependency_bytes_and_missing_files_fail(self):
        for relative in ("sample/__init__.py", "share/sample.txt"):
            path = self.install / relative
            content = path.read_bytes()
            path.write_bytes(content + b"tampered")
            with self.assertRaises(RuntimeError): self.verify()
            path.unlink()
            with self.assertRaises(RuntimeError): self.verify()
            path.write_bytes(content)

    def test_inventory_duplicates_foreign_packages_and_versions_fail(self):
        foreign = SimpleNamespace(metadata={"Name": "foreign"}, version="1.0")
        for distributions in ([], [foreign], [self.distribution, foreign],
                              [self.distribution, self.distribution]):
            with self.assertRaises(RuntimeError): self.verify(distributions)
        self.distribution.version = "2.0"
        with self.assertRaises(RuntimeError): self.verify()

    def test_lock_is_exact_nonempty_and_unique(self):
        good = self.lock.read_text()
        for malformed in ("", good + good, good.replace("sample==", "sample>="),
                          good.replace("--hash=sha256:", "--hash=md5:"), good + "# hidden input\n"):
            self.lock.write_text(malformed)
            with self.assertRaises(RuntimeError): self.verify()

    def test_corrupted_or_absent_wheel_fails(self):
        self.wheel.write_bytes(b"unbound wheel content")
        with self.assertRaises(RuntimeError): self.verify()
        self.wheel.unlink()
        with self.assertRaises(RuntimeError): self.verify()

    def test_wheel_metadata_is_bound_to_lock(self):
        for payload in (b"Name: foreign\nVersion: 1.0\n", b"Name: sample\nVersion: 2.0\n"):
            members = [(name, payload if name.endswith("/METADATA") else content)
                       for name, content in self.members]
            self.write_wheel(members)
            with self.assertRaises(RuntimeError): self.verify()

    def test_unsafe_duplicate_or_ambiguous_members_fail(self):
        for name in ("../escape.py", "/absolute.py", "sample\\escape.py",
                     "sample/__init__.py", "other-1.0.dist-info/METADATA"):
            self.write_wheel(self.members + [(name, b"Name: sample\nVersion: 1.0\n")])
            with self.assertRaises(RuntimeError): self.verify()

    def test_undeclared_installer_transformation_fails(self):
        self.write_wheel(self.members + [("sample-1.0.data/scripts/tool", b"#!python\n")])
        with self.assertRaises(RuntimeError): self.verify()


class ReleaseGateTests(unittest.TestCase):
    def test_dependency_partition_and_single_ci_build_order(self):
        inventory = dict(kind="ctestInfo", version=dict(major=1), tests=[
            dict(name="ijedi_one", command=["owned"],
                 properties=[dict(name="LABELS", value=["ijedi"])]),
            dict(name="atlas_one", command=["dependency"], properties=[])])
        self.assertEqual(dependency_names(inventory), {"atlas_one"})
        for attack in ("unlabeled", "foreign-prefix", "empty", "disabled"):
            changed = copy.deepcopy(inventory)
            if attack == "unlabeled": changed["tests"][0]["properties"] = []
            elif attack == "foreign-prefix": changed["tests"][0]["name"] = "foreign_one"
            elif attack == "empty": changed["tests"].pop()
            else: changed["tests"][1]["properties"] = [dict(name="DISABLED", value=True)]
            with self.assertRaises(RuntimeError): dependency_names(changed)
        workflow = (ROOT / ".github/workflows/build.yaml").read_text()
        self.assertIn("runs-on: ubuntu-24.04-arm", workflow)
        self.assertIn("vars.IJEDI_MPAS_QUALIFICATION_IMAGE", workflow)
        self.assertNotIn("make -j", workflow)
        self.assertNotIn("check-container", workflow)
        self.assertLess(workflow.index("run_p0_p2_release_gate.py"),
                        workflow.index('cmake --build "$GITHUB_WORKSPACE/Build"'))
        self.assertIn("check_dependency_results.py", workflow)

    def test_ci_stage_matches_the_implemented_source_surface(self):
        workflow = (ROOT / ".github/workflows/build.yaml").read_text()
        analysis = (ROOT / "src/ijedi/Increment/MpasIncrementBackend.cc").exists()
        stage = "pr2" if analysis else "pr1"
        self.assertIn("-DIJEDI_MPAS_RELEASE_STAGE=" + stage, workflow)
        self.assertIn("--stage " + stage, workflow)
        other = "pr1" if analysis else "pr2"
        self.assertNotIn("-DIJEDI_MPAS_RELEASE_STAGE=" + other, workflow)
        self.assertNotIn("--stage " + other, workflow)
        self.assertLess(workflow.index('cmake --build "$GITHUB_WORKSPACE/Build"'),
                        workflow.index("--verify-receipt"))
        self.assertIn("actions/upload-artifact@", workflow)

    def setUp(self):
        self.inventory = dict(kind="ctestInfo", version=dict(major=1), tests=[
            dict(name="one", command=["/test", "one"], properties=[]),
            dict(name="two", command=["/test", "two"], properties=[])])
        self.directory = tempfile.TemporaryDirectory()
        self.xml = Path(self.directory.name)/"result.xml"
        self.addCleanup(self.directory.cleanup)

    def test_retained_names_and_added_api_control(self):
        manifest = json.loads((ROOT/"docs/P0_P2_REQUIRED_TESTS.json").read_text())
        self.assertEqual(set(manifest["pr1"]), {
            "ijedi_mpas_api_contracts", "ijedi_mpas_oops_two_step",
            "ijedi_mpas_atlas_topology", "ijedi_mpas_atlas_point_operator",
            "ijedi_mpas_atlas_conservative_operator", "ijedi_mpas_atlas_cache_restore",
            "ijedi_mpas_geometry_negative_controls", "ijedi_mpas_retired_geometry_absent",
        })
        self.assertEqual(set(manifest["pr2"]), {
            "ijedi_mpas_variable_schema", "ijedi_mpas_state_atlas_views",
            "ijedi_mpas_increment_algebra", "ijedi_mpas_transforms_nonlinear",
            "ijedi_mpas_transforms_tlad", "ijedi_mpas_oops_getvalues_tlad",
            "ijedi_mpas_control_native_tlad", "ijedi_mpas_variable_negative_controls",
        })
        self.assertEqual(len(GATE.required_names(manifest, "pr1")), 48)
        self.assertEqual(len(GATE.required_names(manifest, "pr2")), 56)
        manifest["pr2"].append(manifest["pr1"][0])
        with self.assertRaises(RuntimeError): GATE.required_names(manifest, "pr2")

    def test_registered_inventory(self):
        self.assertEqual(GATE.check_inventory(self.inventory, {"one"}), {"one", "two"})
        attacks = []
        missing = copy.deepcopy(self.inventory); missing["tests"].pop(); attacks.append(missing)
        duplicate = copy.deepcopy(self.inventory); duplicate["tests"][1]["name"] = "one"; attacks.append(duplicate)
        disabled = copy.deepcopy(self.inventory); disabled["tests"][0]["properties"] = [dict(name="DISABLED", value=True)]; attacks.append(disabled)
        empty = copy.deepcopy(self.inventory); empty["tests"][0]["command"] = []; attacks.append(empty)
        wrong = copy.deepcopy(self.inventory); wrong["kind"] = "other"; attacks.append(wrong)
        for attack in attacks:
            with self.subTest(attack=attack):
                with self.assertRaises(RuntimeError): GATE.check_inventory(attack, {"one", "two"})

    def test_owned_geometry_scan_with_populated_dependency_bundle(self):
        source = Path(self.directory.name) / "source"
        source.mkdir()
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        (source / ".gitignore").write_text("bundle/dependency/\nsrc/ignored.py\n")
        for name in ("CMakeLists.txt", "bundle/CMakeLists.txt", "src/ignored.py",
                     "test/probe.py", "tools/probe.sh", "cmake/owned.cmake", "bundle/owned.cmake",
                     "bundle/dependency/Modules/FindUnused.cmake"):
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("owned or external fixture\n")
        scanned = {str(p.relative_to(source)) for p in SCAN.active_source_files(source)}
        self.assertEqual(scanned, {"CMakeLists.txt", "bundle/CMakeLists.txt",
                                  "src/ignored.py", "test/probe.py", "tools/probe.sh", "cmake/owned.cmake",
                                  "bundle/owned.cmake"})
        # Tracked ownership wins even when the broad dependency ignore matches.
        subprocess.run(["git", "-C", str(source), "add", "-f",
                        "bundle/dependency/Modules/FindUnused.cmake"], check=True)
        self.assertIn(source / "bundle/dependency/Modules/FindUnused.cmake",
                      SCAN.active_source_files(source))

    def test_full_results_and_negative_controls(self):
        good = '<testsuite tests="2" failures="0" disabled="0"><testcase name="one" status="run"/><testcase name="two" status="run"/></testsuite>'
        self.xml.write_text(good)
        self.assertEqual(GATE.check_results(self.xml, {"one", "two"}), 2)
        attacks = [
            good.replace('name="two"', 'name="one"'),
            good.replace('name="two"', 'name="three"'),
            good.replace('status="run"', 'status="notrun"', 1),
            good.replace('name="one" status="run"/>', 'name="one" status="run"><skipped/></testcase>'),
            good.replace('name="one" status="run"/>', 'name="one" status="run"><failure/></testcase>'),
            good.replace('name="one" status="run"/>', 'name="one" status="run"><error/></testcase>'),
            good.replace('failures="0"', 'failures="1"'),
            good.replace('disabled="0"', 'disabled="1"'),
            good.replace('tests="2"', 'tests="1"'),
            good.replace('<testcase name="two" status="run"/>', ''),
        ]
        for attack in attacks:
            self.xml.write_text(attack)
            with self.subTest(attack=attack):
                with self.assertRaises(RuntimeError): GATE.check_results(self.xml, {"one", "two"})

    def test_schedule_rejects_unsafe_graphs(self):
        inventory = copy.deepcopy(self.inventory)
        inventory["tests"][0]["properties"] = [
            dict(name="LABELS", value=["ijedi_schedule_heavy"]),
            dict(name="RESOURCE_LOCK", value=["ijedi_heavy"]),
            dict(name="FIXTURES_SETUP", value=["ijedi_result_one"])]
        inventory["tests"][1]["properties"] = [
            dict(name="LABELS", value=["ijedi_schedule_isolated"]),
            dict(name="RUN_SERIAL", value=True), dict(name="DEPENDS", value=["one"]),
            dict(name="FIXTURES_REQUIRED", value=["ijedi_result_one"])]
        GATE.check_schedule(inventory)
        for test_index, property_name in ((0, "LABELS"), (0, "RESOURCE_LOCK"),
                                          (0, "FIXTURES_SETUP"), (1, "RUN_SERIAL"),
                                          (1, "FIXTURES_REQUIRED")):
            attacked = copy.deepcopy(inventory)
            attacked["tests"][test_index]["properties"] = [
                p for p in attacked["tests"][test_index]["properties"]
                if p["name"] != property_name]
            with self.subTest(property=property_name):
                with self.assertRaises(RuntimeError): GATE.check_schedule(attacked)

    def test_machine_lock_and_job_bounds(self):
        path = Path(self.directory.name) / "execution.lock"
        command = [sys.executable, "-c",
                   "import importlib.util,pathlib,sys; "
                   "s=importlib.util.spec_from_file_location('gate',sys.argv[1]); "
                   "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
                   "m.execution_lock(pathlib.Path(sys.argv[2])).__enter__()",
                   str(ROOT / "tools/qualification/run_p0_p2_release_gate.py"), str(path)]
        with GATE.execution_lock(path):
            result = subprocess.run(command, text=True, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("machine execution lock", result.stderr)
        subprocess.run(command, check=True, capture_output=True, timeout=10)
        for count in (1, 2, 4): self.assertEqual(GATE.job_count(str(count)), count)
        for count in (0, -1, 5):
            with self.assertRaises(Exception): GATE.job_count(str(count))

    def test_build_source_binding_rejects_foreign_sources_and_stale_commands(self):
        root = Path(self.directory.name)
        source, foreign, build = (root / name for name in ("source", "foreign", "build"))
        source.mkdir(); foreign.mkdir(); build.mkdir()
        relative = ("src/ijedi/State/State.cc", "src/ijedi/Geometry/mpas/GeometryMPAS.cc",
                    "src/ijedi/Python/PythonRuntime.cc")
        for name in relative:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// source-binding fixture, not numerical code\n")
        cache = build / "CMakeCache.txt"
        cache.write_text(f"ijedi_SOURCE_DIR:STATIC={source}\nCMAKE_HOME_DIRECTORY:INTERNAL={source}\n")
        database = build / "compile_commands.json"
        rows = [dict(directory=str(build), file=str(source / name),
                     command=f"c++ -ffp-contract=off -c {source / name} -o CMakeFiles/ijedi.dir/{name}.o")
                for name in relative]
        database.write_text(json.dumps(rows))
        good = GATE.build_source_binding(build, source)
        self.assertEqual(set(good["compiled_ijedi_sources"]), set(relative))
        with self.assertRaisesRegex(RuntimeError, "actual I-JEDI source differs"):
            GATE.build_source_binding(build, foreign)
        # An outer bundle home is legitimate; its compiled target still binds
        # the exact source. A cache name alone is never sufficient.
        cache.write_text(f"ijedi_SOURCE_DIR:STATIC={source}\nCMAKE_HOME_DIRECTORY:INTERNAL={root}\n")
        GATE.build_source_binding(build, source)
        escaped = foreign / "State.cc"
        escaped.write_text("// deliberately foreign fixture\n")
        attacked = copy.deepcopy(rows); attacked[0]["file"] = str(escaped)
        database.write_text(json.dumps(attacked))
        with self.assertRaisesRegex(RuntimeError, "foreign source file"):
            GATE.build_source_binding(build, source)
        database.write_text(json.dumps(rows[1:]))
        with self.assertRaisesRegex(RuntimeError, "lacks required owned source"):
            GATE.build_source_binding(build, source)
        database.rename(build / "retained-commands.json")
        with self.assertRaisesRegex(RuntimeError, "lacks actual compilation"):
            GATE.build_source_binding(build, source)

    def test_cpp_arithmetic_rejects_contraction_and_hidden_overrides(self):
        GATE.check_cpp_arithmetic("c++ -O2 -ffp-contract=off -c source.cc")
        GATE.check_cpp_arithmetic("c++ -ffp-contract=fast -ffp-contract=off -c source.cc")
        for command in (
            "c++ -O2 -c source.cc",
            "c++ -ffp-contract=off -ffp-contract=fast -c source.cc",
            "c++ -ffp-contract=on -c source.cc",
            "c++ -ffast-math -ffp-contract=off -c source.cc",
            "c++ -Ofast -ffp-contract=off -c source.cc",
            "c++ -fassociative-math -ffp-contract=off -c source.cc",
            "c++ -freciprocal-math -ffp-contract=off -c source.cc",
            "c++ -fno-signed-zeros -ffp-contract=off -c source.cc",
            "c++ -ffinite-math-only -ffp-contract=off -c source.cc",
            "c++ -funsafe-math-optimizations -ffp-contract=off -c source.cc",
            "c++ -ffp-contract=off @uninspected-flags -c source.cc",
        ):
            with self.subTest(command=command):
                with self.assertRaisesRegex(RuntimeError, "unfused binary64 contract"):
                    GATE.check_cpp_arithmetic(command)

    def test_stale_generated_products_rejected_without_deleting(self):
        build = Path(self.directory.name) / "build"
        GATE.check_fresh_products(build)
        generated = build / "test/data_generated/nested"
        generated.mkdir(parents=True)
        GATE.check_fresh_products(build)
        retained = generated / "prior.json"
        retained.write_text("prior result must survive")
        with self.assertRaisesRegex(RuntimeError, "old generated test products"):
            GATE.check_fresh_products(build)
        self.assertEqual(retained.read_text(), "prior result must survive")
        # Preserve the prior result outside the build; an unresolved link must
        # not evade freshness validation by failing is_file().
        retained.rename(build / "retained-prior.json")
        retained.symlink_to(generated / "missing-target")
        with self.assertRaisesRegex(RuntimeError, "old generated test products"):
            GATE.check_fresh_products(build)
        self.assertTrue(retained.is_symlink())

    def test_prior_owned_compilation_rejected_with_empty_science_products(self):
        build = Path(self.directory.name) / "freshness"
        build.mkdir()
        database = build / "compile_commands.json"
        output = build / "CMakeFiles/ijedi.dir/prior.cc.o"
        output.parent.mkdir(parents=True)
        database.write_text(json.dumps([dict(directory=str(build),
            command="c++ -ffp-contract=off -c prior.cc -o CMakeFiles/ijedi.dir/prior.cc.o")]))
        GATE.check_fresh_products(build)
        output.write_bytes(b"prior owned compilation fixture")
        with self.assertRaisesRegex(RuntimeError, "prior owned compiled products"):
            GATE.check_fresh_products(build)
        self.assertEqual(output.read_bytes(), b"prior owned compilation fixture")
        output.unlink()
        output.symlink_to(build / "absent")
        with self.assertRaisesRegex(RuntimeError, "prior owned compiled products"):
            GATE.check_fresh_products(build)
        self.assertTrue(output.is_symlink())

    def test_all_owned_targets_are_fresh_but_dependencies_may_be_reused(self):
        root = Path(self.directory.name)
        source, build = root / "owned-source", root / "owned-build"
        source.mkdir(); build.mkdir()
        rows = []
        for relative in ("src/mains/main.cc", "test/mains/science.cc",
                         "src/ijedi/fv3/interface.F90", "bundle/atlas/mesh.cc"):
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("controlled compilation fixture\n")
            output = build / (relative + ".o")
            output.parent.mkdir(parents=True, exist_ok=True)
            rows.append(dict(file=str(path), directory=str(build),
                             command=f"c++ -c {path} -o {output}"))
        (build / "compile_commands.json").write_text(json.dumps(rows))
        dependency = build / "bundle/atlas/mesh.cc.o"
        dependency.write_bytes(b"qualified dependency output")
        GATE.check_fresh_products(build, source)
        for relative in ("src/mains/main.cc", "test/mains/science.cc",
                         "src/ijedi/fv3/interface.F90"):
            output = build / (relative + ".o")
            output.write_bytes(b"prior owned output")
            with self.assertRaisesRegex(RuntimeError, "prior owned compiled products"):
                GATE.check_fresh_products(build, source)
            self.assertEqual(output.read_bytes(), b"prior owned output")
            output.unlink()
        self.assertEqual(dependency.read_bytes(), b"qualified dependency output")

    def test_unbuilt_dependency_sources_do_not_block_owned_freshness(self):
        root = Path(self.directory.name) / "unbuilt-bundle"
        source, outer = root / "source", root / "build"
        build = outer / "ijedi"
        source.mkdir(parents=True); build.mkdir(parents=True)
        (outer / "CMakeCache.txt").write_text("// configured bundle fixture\n")
        module_link = source / "bundle/fckit/src/fckit/module"
        module_link.parent.mkdir(parents=True)
        module_link.symlink_to(outer / "fckit/src/fckit/module", target_is_directory=True)
        future_dependency = module_link / "fckit_array.F90"
        future_owned = build / "src/generated.F90"
        output = build / "src/CMakeFiles/ijedi_probe.dir/generated.F90.o"
        rows = [dict(file=str(future_dependency), directory=str(outer / "fckit/src/fckit"),
                     command="gfortran -c " + str(future_dependency) + " -o fckit_array.F90.o"),
                dict(file=str(future_owned), directory=str(build / "src"),
                     command="gfortran -c " + str(future_owned) + " -o " + str(output))]
        (outer / "compile_commands.json").write_text(json.dumps(rows))
        GATE.check_fresh_products(build, source)
        self.assertFalse(future_dependency.exists())
        self.assertFalse(future_owned.exists())
        output.parent.mkdir(parents=True)
        output.write_bytes(b"prior owned generated-source compilation")
        with self.assertRaisesRegex(RuntimeError, "prior owned compiled products"):
            GATE.check_fresh_products(build, source)
        output.unlink()
        output.symlink_to(build / "missing-output")
        with self.assertRaisesRegex(RuntimeError, "prior owned compiled products"):
            GATE.check_fresh_products(build, source)
        self.assertTrue(output.is_symlink())

    def test_missing_owned_source_is_not_an_unbuilt_dependency(self):
        source, build = (Path(self.directory.name) / name for name in ("source", "build"))
        source.mkdir(); build.mkdir()
        missing = source / "src/mains/missing.cc"
        (build / "compile_commands.json").write_text(json.dumps([
            dict(file=str(missing), directory=str(build),
                 command="c++ -c " + str(missing) + " -o missing.cc.o")]))
        with self.assertRaises(FileNotFoundError):
            GATE.check_fresh_products(build, source)

    def test_full_wrapper_rejects_a_foreign_build_before_running_green_tests(self):
        # Reproduce the former failure with real Git/CMake/CTest inputs: clean
        # checkout A and 51 successful dummy tests configured from B. These are
        # adversarial gate fixtures, never a scientific qualification.
        root = Path(self.directory.name)
        declared, actual, build, output = (root / name for name in
                                          ("declared", "actual", "build", "output"))
        declared.mkdir(); actual.mkdir()
        manifest = declared / "docs/P0_P2_REQUIRED_TESTS.json"
        manifest.parent.mkdir()
        manifest.write_bytes((ROOT / "docs/P0_P2_REQUIRED_TESTS.json").read_bytes())
        subprocess.run(["git", "init", "-q", str(declared)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(declared), "add", "docs"], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(declared), "-c", "user.name=Gate Fixture",
                        "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
                       check=True, capture_output=True)
        names = GATE.required_names(json.loads(manifest.read_text()), "pr2")
        (actual / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.23)\nproject(ijedi NONE)\nenable_testing()\n" +
            "\n".join(f'add_test(NAME {name} COMMAND "${{CMAKE_COMMAND}}" -E true)'
                      for name in sorted(names)) + "\n")
        subprocess.run(["cmake", "-S", str(actual), "-B", str(build)],
                       check=True, capture_output=True, timeout=15)
        args = SimpleNamespace(source=declared, build=build, output=output,
                               manifest=manifest, stage="pr2", build_jobs=1, test_jobs=1,
                               check_inventory_only=False)
        with self.assertRaisesRegex(RuntimeError, "actual I-JEDI source differs"):
            GATE.execute(args)
        self.assertFalse((output / "qualified.json").exists())
        self.assertFalse((output / "full.xml").exists())


class OwnedArtifactTests(unittest.TestCase):
    """Compile real artifacts and attack same-path replacement and loader aliases."""
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source, self.build = self.root / "source", self.root / "build"
        self.source.mkdir()
        names = ("src/ijedi/State/State.cc", "src/ijedi/Geometry/mpas/GeometryMPAS.cc",
                 "src/ijedi/Python/PythonRuntime.cc")
        for i, name in enumerate(names):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('extern "C" int probe' + str(i) + '() { return 1; }\n')
        (self.source / "main.cc").write_text('extern "C" int probe0(); int main() { return probe0() == 1 ? 0 : 1; }\n')
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.23)\nproject(ijedi LANGUAGES CXX)\n'
            'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
            'add_compile_options(-ffp-contract=off)\n'
            'add_library(ijedi SHARED ' + ' '.join(names) + ')\n'
            'set_target_properties(ijedi PROPERTIES VERSION 1.0 SOVERSION 1)\n'
            'add_executable(probe main.cc)\ntarget_link_libraries(probe PRIVATE ijedi)\n'
            'enable_testing()\nadd_test(NAME one COMMAND probe)\nadd_test(NAME two COMMAND probe)\n')
        subprocess.run(["cmake", "-G", "Unix Makefiles", "-S", str(self.source), "-B", str(self.build)],
                       check=True, capture_output=True, timeout=20)
        subprocess.run(["cmake", "--build", str(self.build), "-j2"],
                       check=True, capture_output=True, timeout=30)

    def test_real_binary_and_alias_replacement_changes_seal(self):
        subprocess.run([str(self.build / "probe")], check=True, timeout=5)
        seal = GATE.owned_artifacts(self.build, self.source)
        self.assertEqual(seal["targets"], 2)
        self.assertIn("probe", seal["members"])
        library = next(self.build.glob("*ijedi*1.0*"))
        old = library.read_bytes()
        library.write_bytes(old + b"same-path replacement")
        self.assertNotEqual(GATE.owned_artifacts(self.build, self.source), seal)
        library.write_bytes(old)
        self.assertEqual(GATE.owned_artifacts(self.build, self.source), seal)
        aliases = [p for p in self.build.glob("*ijedi*") if p.is_symlink()]
        self.assertTrue(aliases)
        alias = aliases[0]
        original = alias.readlink()
        alias.unlink()
        alias.symlink_to("probe")
        self.assertNotEqual(GATE.owned_artifacts(self.build, self.source), seal)
        alias.unlink()
        alias.symlink_to(original)
        self.assertEqual(GATE.owned_artifacts(self.build, self.source), seal)

    def test_receipt_verifier_rejects_same_path_replacement_and_result_tampering(self):
        manifest = self.source / "docs/P0_P2_REQUIRED_TESTS.json"
        manifest.parent.mkdir()
        manifest.write_text(json.dumps(dict(schema_version=1, pr1=["one"],
                                            pr2=["future"], retained_non_mpas=["two"])))
        subprocess.run(["git", "init", "-q", str(self.source)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=Gate Fixture",
                        "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
                       check=True, capture_output=True)
        evidence = self.root / "evidence"
        evidence.mkdir()
        configured = subprocess.check_output(["ctest", "--test-dir", str(self.build),
                                              "--show-only=json-v1"])
        (evidence / "inventory.json").write_bytes(configured)
        subprocess.run(["ctest", "--test-dir", str(self.build), "--output-junit",
                        str(evidence / "full.xml")], check=True, capture_output=True, timeout=10)
        artifacts = evidence / "owned-artifacts.json"
        artifacts.write_text(json.dumps(GATE.owned_artifacts(self.build, self.source)))
        receipt = dict(source_commit=GATE.source_authority(self.source), stage="pr1",
                       configured_tests=2, skips=0, required_tests=["one", "two"],
                       source_binding=GATE.build_source_binding(self.build, self.source))
        for path, key in ((manifest, "required_manifest_sha256"),
                          (evidence / "inventory.json", "inventory_sha256"),
                          (evidence / "full.xml", "full_xml_sha256"),
                          (artifacts, "owned_artifacts_sha256")):
            receipt[key] = hashlib.sha256(path.read_bytes()).hexdigest()
        receipt_path = evidence / "qualified.json"
        receipt_path.write_text(json.dumps(receipt))
        GATE.verify_qualification_receipt(receipt_path, self.source, self.build)
        executable = self.build / "probe"
        original = executable.read_bytes()
        executable.write_bytes(original + b"replacement")
        with self.assertRaisesRegex(RuntimeError, "owned runtime artifact bytes or paths differ"):
            GATE.verify_qualification_receipt(receipt_path, self.source, self.build)
        executable.write_bytes(original)
        GATE.verify_qualification_receipt(receipt_path, self.source, self.build)
        result = evidence / "full.xml"
        result.write_bytes(result.read_bytes() + b"changed")
        with self.assertRaisesRegex(RuntimeError, "qualification evidence differs"):
            GATE.verify_qualification_receipt(receipt_path, self.source, self.build)

    def test_missing_output_and_external_link_authority_reject(self):
        executable = self.build / "probe"
        executable.rename(self.root / "retained-probe")
        with self.assertRaises(FileNotFoundError):
            GATE.owned_artifacts(self.build, self.source)
        (self.root / "retained-probe").rename(executable)
        link = self.build / "CMakeFiles/probe.dir/link.txt"
        link.write_text("c++ -o " + str(self.root / "retained-probe") + "\n")
        (self.root / "retained-probe").write_bytes(b"foreign artifact")
        with self.assertRaisesRegex(RuntimeError, "escapes the build tree"):
            GATE.owned_artifacts(self.build, self.source)


class MpasInputBindingTests(unittest.TestCase):
    """Actual CMake input binding with explicit non-numerical byte fixtures."""
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.wheel = self.root / "fixture.whl"
        self.wheel.write_bytes(b"input-binding fixture, not a model wheel")
        self.runtime = dict(mpas_pytorch=dict(version="0.1.5", source_commit="a" * 40,
            wheel_sha256=hashlib.sha256(self.wheel.read_bytes()).hexdigest()),
            runtime=dict(python="3.11.7", torch="2.8.0+cpu", numpy="2.0.2",
                         netcdf4="1.7.4.1", machine="aarch64"))
        self.oracle = dict(wheel_sha256=self.runtime["mpas_pytorch"]["wheel_sha256"],
                           package_source_commit="a" * 40,
                           runtime=dict(python_version="3.11.7", torch_version="2.8.0+cpu",
                             numpy_version="2.0.2", netcdf4_version="1.7.4.1",
                             platform_machine="aarch64"), input_files={})
        for receipt in ("horizontal_geometry_receipt", "static_vertical_geometry_receipt",
                        "geometry_bundle_receipt", "configuration_receipt", "state_schema_digest"):
            self.oracle[receipt] = "5" * 64
        for name in ("init", "grid", "namelist"):
            path = self.root / name
            path.write_bytes(("fixture " + name).encode())
            self.oracle["input_files"][name] = dict(name=name,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.23)\nproject(binding_fixture NONE)\n' +
            f'include("{ROOT / "test/BindMpasInputs.cmake"}")\n' +
            f'configure_file("{ROOT / "test/testinput/tier0_integration_mpas-oops-step.yaml.in"}" '
            '"${CMAKE_BINARY_DIR}/bound.yaml" @ONLY)\n' +
            ''.join(f'configure_file("{ROOT / "test/testinput" / (name + ".yaml.in")}" '
                    f'"${{CMAKE_BINARY_DIR}}/{name}.yaml" @ONLY)\n' for name in (
                        "tier0_integration_mpas-oops-restore",
                        "tier0_integration_mpas-typed-interpolation")
                    if (ROOT / "test/testinput" / (name + ".yaml.in")).is_file()))

    def configure(self, extra=()):
        (self.root / "runtime.json").write_text(json.dumps(self.runtime))
        (self.root / "oracle.json").write_text(json.dumps(self.oracle))
        return subprocess.run(["cmake", "-S", str(self.root), "-B", str(self.root / "build"),
            "-DIJEDI_MPAS_P0_WHEEL=" + str(self.wheel),
            "-DIJEDI_MPAS_RUNTIME_RECEIPT=" + str(self.root / "runtime.json"),
            "-DIJEDI_MPAS_P0_DIRECT_ORACLE=" + str(self.root / "oracle.json"),
            "-DIJEDI_MPAS_P0_CASE_DIR=" + str(self.root),
            "-DIJEDI_MPAS_P0_PYTHON=/fixture/python", *extra],
            capture_output=True, text=True, timeout=15)

    def test_partial_p2_inputs_fail_before_generating_unbound_receipts(self):
        result = self.configure(("-DIJEDI_MPAS_P2_LOCATIONS=/fixture/locations.json",
                                 "-DIJEDI_MPAS_P0_DIRECT_ORACLE="))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires the complete authenticated P0 input graph", result.stderr)
        self.assertFalse((self.root / "build/bound.yaml").exists())

    def test_bound_yaml_uses_one_exact_runtime_and_oracle(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        yaml = (self.root / "build/bound.yaml").read_text()
        for value in ("netcdf4 version: 1.7.4.1", "mpas-pytorch version: 0.1.5",
                      "mpas-pytorch source commit: " + "a" * 40,
                      "state schema digest: " + "5" * 64):
            self.assertIn(value, yaml)
        self.assertNotIn("@IJEDI_MPAS_", yaml)

    def test_all_atlas_consumers_bind_one_pin_and_reject_overrides(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        pin = "463944947b5ac17ea4bdcc8da1004a30f4602920"
        for name in ("bound", "tier0_integration_mpas-oops-restore",
                     "tier0_integration_mpas-typed-interpolation"):
            if name.endswith("typed-interpolation") and not (
                    ROOT / "test/testinput" / (name + ".yaml.in")).is_file():
                # This P2 surface is deliberately absent on the geometry-only tip.
                continue
            yaml = (self.root / "build" / (name + ".yaml")).read_text()
            self.assertIn("atlas compiler identity: " + pin, yaml)
            self.assertNotIn("@IJEDI_MPAS_ATLAS_", yaml)
        bundle = (ROOT / "bundle/CMakeLists.txt").read_text()
        self.assertIn("TAG ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}", bundle)
        consumers = (ROOT / "test/CMakeLists.txt").read_text().splitlines()
        commands = [line.strip() for line in consumers if "--atlas-compiler-identity" in line]
        self.assertTrue(commands)
        self.assertEqual(set(commands), {
            "--atlas-compiler-identity ${IJEDI_MPAS_ATLAS_COMPILER_IDENTITY}"})
        result = self.configure(("-DIJEDI_MPAS_ATLAS_COMPILER_IDENTITY=" + "0" * 40,))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differs from the dependency pin", result.stderr)

    def test_release_stage_requires_real_inputs_before_registering_mpas(self):
        result = self.configure(("-DIJEDI_MPAS_RELEASE_STAGE=unknown",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release stage must be pr1 or pr2", result.stderr)
        result = self.configure(("-DIJEDI_MPAS_RELEASE_STAGE=pr1",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Mandatory MPAS release input is missing: IJEDI_MPAS_P0_PYTHON",
                      result.stderr)
        self.assertFalse((self.root / "build/bound.yaml").exists())
        python = self.root / "fixture-python"
        python.write_bytes(b"existence fixture; not executed")
        cases = self.root / "geometry-cases.json"
        cases.write_text("{}")
        locations = self.root / "locations"
        locations.mkdir()
        inputs = ("-DIJEDI_MPAS_P0_PYTHON=" + str(python),
                  "-DIJEDI_MPAS_GEOMETRY_CASES=" + str(cases),
                  "-DIJEDI_MPAS_GEOMETRY_LOCATIONS=" + str(locations))
        for name in ("locations_256.json", "locations_10000.json"):
            result = self.configure((*inputs, "-DIJEDI_MPAS_RELEASE_STAGE=pr1"))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Mandatory MPAS geometry corpus is missing: " + name,
                          result.stderr)
            (locations / name).write_text("{}")
        result = self.configure((*inputs, "-DIJEDI_MPAS_RELEASE_STAGE=pr1"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.configure((*inputs, "-DIJEDI_MPAS_RELEASE_STAGE=pr2"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Mandatory MPAS P2 input is missing: IJEDI_MPAS_P2_LOCATIONS",
                      result.stderr)
        result = self.configure((*inputs, "-DIJEDI_MPAS_RELEASE_STAGE=pr2",
                                "-DIJEDI_MPAS_P2_LOCATIONS=" + str(locations / name)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Mandatory MPAS P2 input is missing: IJEDI_MPAS_CONTRACT_SOURCE_DIR",
                      result.stderr)
        result = self.configure((*inputs, "-DIJEDI_MPAS_RELEASE_STAGE=pr2",
                                "-DIJEDI_MPAS_P2_LOCATIONS=" + str(locations / name),
                                "-DIJEDI_MPAS_CONTRACT_SOURCE_DIR=" + str(self.root)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_foreign_wheel_source_runtime_and_inputs_fail_at_configuration(self):
        good = copy.deepcopy(self.oracle)
        for key, value, message in (("wheel_sha256", "b" * 64, "foreign package identity"),
                                   ("package_source_commit", "b" * 40, "foreign package identity")):
            self.oracle = dict(good, **{key: value})
            result = self.configure()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)
        self.oracle = copy.deepcopy(good)
        self.oracle["runtime"]["netcdf4_version"] = "1.7.4"
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("foreign runtime", result.stderr)
        self.oracle = copy.deepcopy(good)
        self.oracle["runtime"]["platform_machine"] = "x86_64"
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("foreign architecture", result.stderr)
        self.oracle = copy.deepcopy(good)
        (self.root / "grid").write_bytes(b"edited input")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("input differs from direct oracle: grid", result.stderr)
        self.wheel.write_bytes(b"edited wheel")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wheel differs", result.stderr)


class SchedulerExecutionTests(unittest.TestCase):
    """Use real CTest and the production policy, never numerical test doubles."""
    def setUp(self):
        if not shutil.which("cmake") or not shutil.which("ctest"):
            self.fail("scheduler execution regression requires cmake and ctest")
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.events = self.root / "events.jsonl"
        self.worker = self.root / "worker.py"
        self.worker.write_text('''import fcntl,json,sys,time
from pathlib import Path
name,events,mode=sys.argv[1:]
def emit(phase):
    with open(events,"a") as out:
        fcntl.flock(out,fcntl.LOCK_EX)
        # Apple Python 3.9's monotonic() has a per-process epoch. The POSIX
        # clock is shared across processes, as required by overlap assertions.
        out.write(json.dumps(dict(name=name,phase=phase,
                                 time=time.clock_gettime(time.CLOCK_MONOTONIC)))+"\\n")
emit("start")
time.sleep(.3)
emit("end")
if mode=="fail": sys.exit(7)
''')

    def run_graph(self, names, *, failed=None, unsafe=False, selected=None, parallel=4):
        source = self.root / ("unsafe" if unsafe else "source")
        source.mkdir()
        lines = ["cmake_minimum_required(VERSION 3.23)",
                 "project(scheduler_probe NONE)", "enable_testing()"]
        for name in names:
            lines.append(f'add_test(NAME {name} COMMAND "{sys.executable}" '
                         f'"{self.worker}" {name} "{self.events}" '
                         f'{"fail" if name == failed else "pass"})')
        if "ijedi_mpas_oops_vs_direct" in names and "producer" in names:
            lines.append("set_tests_properties(ijedi_mpas_oops_vs_direct PROPERTIES DEPENDS producer)")
        lines += [f'include("{ROOT / "test/ProtectTestSchedule.cmake"}")',
                  "ijedi_protect_test_schedule()"]
        if unsafe:
            lines.append('set_tests_properties(heavy_a heavy_b PROPERTIES RESOURCE_LOCK "")')
        (source / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
        build = source / "build"
        subprocess.run(["cmake", "-S", str(source), "-B", str(build)],
                       check=True, capture_output=True, timeout=15)
        inventory = json.loads(subprocess.check_output(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
        if not unsafe: GATE.check_schedule(inventory)
        command = ["ctest", "--test-dir", str(build), "--parallel", str(parallel),
                   "--output-junit", str(build / "results.xml")]
        if selected: command += ["-R", selected]
        result = subprocess.run(command, text=True, capture_output=True, timeout=15)
        if not failed:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            GATE.check_results(build / "results.xml", {t["name"] for t in inventory["tests"]})
        rows = [json.loads(line) for line in self.events.read_text().splitlines()]
        intervals = {name: {r["phase"]: r["time"] for r in rows if r["name"] == name}
                     for name in {r["name"] for r in rows}}
        return result, intervals

    @staticmethod
    def overlaps(a, b):
        return max(a["start"], b["start"]) < min(a["end"], b["end"])

    def test_heavy_exclusion_light_overlap_and_isolation(self):
        _, intervals = self.run_graph([
            "heavy_a", "unknown_compare", "ijedi_mpas_oops_vs_direct",
            "ijedi_mpas_atlas_point_operator"])
        self.assertFalse(self.overlaps(intervals["heavy_a"], intervals["unknown_compare"]), intervals)
        self.assertTrue(any(self.overlaps(intervals["ijedi_mpas_oops_vs_direct"], intervals[n])
                            for n in ("heavy_a", "unknown_compare")))
        isolated = intervals["ijedi_mpas_atlas_point_operator"]
        self.assertFalse(any(self.overlaps(isolated, row) for name, row in intervals.items()
                             if name != "ijedi_mpas_atlas_point_operator"))
        print(json.dumps(dict(scope="scheduler_probe_not_scientific_qualification",
                              heavy_overlap=False, light_overlap=True,
                              measurement_overlap=False,
                              parallel_span_seconds=max(r["end"] for r in intervals.values()) -
                              min(r["start"] for r in intervals.values()))), flush=True)

    def test_unlocked_negative_control_really_overlaps(self):
        _, intervals = self.run_graph(["heavy_a", "heavy_b"], unsafe=True)
        self.assertTrue(self.overlaps(intervals["heavy_a"], intervals["heavy_b"]))

    def test_failed_producer_blocks_consumer_even_when_selected_alone(self):
        result, intervals = self.run_graph(["producer", "ijedi_mpas_oops_vs_direct"],
                                          failed="producer", selected="ijedi_mpas_oops_vs_direct")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(set(intervals), {"producer"})
        self.assertIn("Not Run", result.stdout)

    def test_policy_covers_each_cmake_registration_directory(self):
        source = self.root / "directory-scope"
        source.mkdir()
        policy = ROOT / "test/ProtectTestSchedule.cmake"
        for child, name in (("src", "ijedi_coding_norms"), ("test", "heavy_a")):
            directory = source / child
            directory.mkdir()
            (directory / "CMakeLists.txt").write_text(
                f'add_test(NAME {name} COMMAND "{sys.executable}" -c "pass")\n'
                f'include("{policy}")\nijedi_protect_test_schedule()\n')
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.23)\nproject(directory_probe NONE)\n'
            'enable_testing()\nadd_subdirectory(src)\nadd_subdirectory(test)\n'
            f'add_test(NAME root_check COMMAND "{sys.executable}" -c "pass")\n'
            f'include("{policy}")\nijedi_protect_test_schedule()\n')
        build = source / "build"
        subprocess.run(["cmake", "-S", str(source), "-B", str(build)],
                       check=True, capture_output=True, timeout=15)
        inventory = json.loads(subprocess.check_output(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
        self.assertEqual({t["name"] for t in inventory["tests"]},
                         {"ijedi_coding_norms", "heavy_a", "root_check"})
        GATE.check_schedule(inventory)
        coding = next(t for t in inventory["tests"] if t["name"] == "ijedi_coding_norms")
        labels = next(p["value"] for p in coding["properties"] if p["name"] == "LABELS")
        self.assertIn("ijedi_schedule_light", labels)


if __name__ == "__main__": unittest.main()
