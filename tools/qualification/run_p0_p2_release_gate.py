#!/usr/bin/env python3
"""Fail-closed complete I-JEDI gate; not dependency/platform qualification.

Never selects only the named tests: every configured I-JEDI test must execute
and pass. Clean source and fresh evidence are mandatory for an actual run.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import xml.etree.ElementTree as ET


def required_names(manifest, stage):
    if manifest.get("schema_version") != 1 or stage not in ("pr1", "pr2"):
        raise RuntimeError("unsupported required-name manifest/stage")
    groups = [manifest["pr1"], manifest["retained_non_mpas"]]
    if stage == "pr2":
        groups.append(manifest["pr2"])
    names = [name for group in groups for name in group]
    if not names or len(set(names)) != len(names) or any(not isinstance(x, str) or not x for x in names):
        raise RuntimeError("required-name manifest is empty or ambiguous")
    return set(names)


def check_inventory(inventory, required):
    if inventory.get("kind") != "ctestInfo" or inventory.get("version", {}).get("major") != 1:
        raise RuntimeError("unsupported CTest inventory")
    tests = inventory["tests"]
    names = [test["name"] for test in tests]
    if len(names) != len(set(names)) or not required.issubset(names):
        raise RuntimeError("duplicate or missing required CTest: " + str(sorted(required-set(names))))
    for test in tests:
        properties = {p["name"]: p["value"] for p in test.get("properties", [])}
        if (properties.get("DISABLED", False) not in (False, "FALSE", "OFF", 0) or
                not test.get("command")):
            raise RuntimeError("disabled or commandless configured test: " + test["name"])
    return set(names)


def check_results(xml_path, configured):
    root = ET.parse(xml_path).getroot()
    cases = list(root.iter("testcase"))
    if (root.tag != "testsuite" or int(root.attrib.get("tests", -1)) != len(cases) or
            any(int(root.attrib.get(key, 0)) != 0 for key in ("failures", "errors", "disabled", "skipped"))):
        raise RuntimeError("full JUnit summary is missing or records a nonpassing gate")
    names = [case.attrib["name"] for case in cases]
    if len(names) != len(set(names)) or set(names) != configured:
        raise RuntimeError("full test result omits, duplicates or adds a configured test")
    for case in cases:
        if (case.attrib.get("status") not in ("run", "passed") or
                any(child.tag in ("skipped", "failure", "error") for child in case)):
            raise RuntimeError("configured test did not pass without skips: " + case.attrib["name"])
    return len(cases)


def source_authority(source):
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
    if dirty:
        raise RuntimeError("release qualification requires a clean source worktree")
    return revision


def check_cpp_arithmetic(command):
    """The pinned CPU replay requires unfused, ordered binary64 operations."""
    tokens = shlex.split(command)
    forbidden = {"-ffast-math", "-Ofast", "-funsafe-math-optimizations",
                 "-fassociative-math", "-freciprocal-math", "-fno-signed-zeros",
                 "-ffinite-math-only"}
    modes = [token for token in tokens if token.startswith("-ffp-contract=")]
    if (not modes or modes[-1] != "-ffp-contract=off" or forbidden.intersection(tokens)
            or any(token.startswith("@") for token in tokens)):
        raise RuntimeError("compiled C++ arithmetic does not bind the unfused binary64 contract")


def build_source_binding(build, source):
    """Bind the configured/compiled I-JEDI target to the declared checkout."""
    source = source.resolve(strict=True)
    directory = build.resolve(strict=True)
    cache_path = next((parent / "CMakeCache.txt" for parent in
                       (directory, *directory.parents)
                       if (parent / "CMakeCache.txt").is_file()), None)
    if cache_path is None:
        raise RuntimeError("release build has no CMake source authority")
    cache_bytes = cache_path.read_bytes()
    values = {}
    for line in cache_bytes.decode().splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    declared = values.get("ijedi_SOURCE_DIR")
    if not declared or Path(declared).resolve(strict=True) != source:
        raise RuntimeError("build's actual I-JEDI source differs from declared source")
    # A bundle has its own CMAKE_HOME_DIRECTORY. The ijedi project entry must
    # still name the exact selected checkout, and its compiled target must use
    # that checkout rather than merely inheriting a forged cache variable.
    database = cache_path.parent / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError("release build lacks actual compilation commands")
    database_bytes = database.read_bytes()
    entries = json.loads(database_bytes)
    compiled = set()
    for entry in entries:
        command = entry.get("command", "") or " ".join(entry.get("arguments", []))
        if "CMakeFiles/ijedi.dir/" not in command.replace("\\", "/"):
            continue
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        path = path.resolve(strict=True)
        if not path.is_relative_to(source):
            raise RuntimeError("compiled I-JEDI target consumes a foreign source file")
        if path.suffix in (".cc", ".cpp", ".cxx"):
            check_cpp_arithmetic(command)
        compiled.add(str(path.relative_to(source)))
    required = {"src/ijedi/State/State.cc", "src/ijedi/Geometry/mpas/GeometryMPAS.cc",
                "src/ijedi/Python/PythonRuntime.cc"}
    if not required.issubset(compiled):
        raise RuntimeError("compiled I-JEDI target lacks required owned source inputs")
    return dict(source_path=str(source), cmake_build_root=str(cache_path.parent),
                cmake_cache_sha256=hashlib.sha256(cache_bytes).hexdigest(),
                compilation_commands_sha256=hashlib.sha256(database_bytes).hexdigest(),
                compiled_ijedi_sources=sorted(compiled))


def owned_artifacts(build, source):
    """Hash the linked outputs of every target compiled from this checkout."""
    binding = build_source_binding(build, source)
    root = Path(binding["cmake_build_root"])
    source = source.resolve(strict=True)
    target_directories = set()
    for entry in json.loads((root / "compile_commands.json").read_bytes()):
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        path = path.resolve(strict=False)
        if not (path.is_relative_to(source) and
                path.relative_to(source).parts[0] != "bundle"):
            continue
        tokens = shlex.split(entry.get("command", "") or
                             shlex.join(entry.get("arguments", [])))
        if "-o" not in tokens or tokens.index("-o") + 1 == len(tokens):
            raise RuntimeError("owned compilation has no explicit output path")
        obj = Path(tokens[tokens.index("-o") + 1])
        if not obj.is_absolute():
            obj = Path(entry["directory"]) / obj
        obj = obj.resolve(strict=False)
        target = next((p for p in obj.parents
                       if p.name.endswith(".dir") and p.parent.name == "CMakeFiles"), None)
        if target is None or not target.is_relative_to(root):
            raise RuntimeError("owned target has no supported CMake link authority")
        target_directories.add(target)
    members = {}

    def record(path):
        resolved = path.resolve(strict=True)
        if not path.is_relative_to(root) or not resolved.is_relative_to(root):
            raise RuntimeError("owned artifact escapes the build tree")
        members[path.relative_to(root).as_posix()] = dict(
            resolved_path=resolved.relative_to(root).as_posix(),
            sha256=hashlib.sha256(path.read_bytes()).hexdigest())

    for target in sorted(target_directories):
        link = target / "link.txt"
        record(link)
        outputs = []
        for line in link.read_text().splitlines():
            tokens = shlex.split(line)
            if "-o" in tokens and tokens.index("-o") + 1 < len(tokens):
                outputs.append(Path(tokens[tokens.index("-o") + 1]))
            elif len(tokens) > 2 and Path(tokens[0]).name.endswith("ar"):
                outputs.append(Path(tokens[2]))
        if len(outputs) != 1:
            raise RuntimeError("owned target lacks an unambiguous linked output: " + str(target))
        output = outputs[0]
        if not output.is_absolute():
            output = target.parent.parent / output
        output = Path(os.path.abspath(output))
        record(output)
        # Authenticate the loader aliases as well as their target bytes.
        for alias in output.parent.iterdir():
            if alias.is_symlink() and alias.resolve() == output.resolve():
                record(alias)
    if not target_directories or not members:
        raise RuntimeError("owned runtime artifact manifest is empty")
    return dict(schema_version=1, targets=len(target_directories), members=members)


def verify_qualification_receipt(receipt_path, source, build):
    """Authenticate retained results and the actual binaries before reuse."""
    receipt = json.loads(receipt_path.read_bytes())
    evidence = receipt_path.parent
    if (receipt["source_commit"] != source_authority(source) or
            receipt["source_binding"] != build_source_binding(build, source)):
        raise RuntimeError("qualification source/build binding differs")
    for name, key in (("inventory.json", "inventory_sha256"),
                      ("full.xml", "full_xml_sha256"),
                      ("owned-artifacts.json", "owned_artifacts_sha256")):
        if hashlib.sha256((evidence / name).read_bytes()).hexdigest() != receipt[key]:
            raise RuntimeError("qualification evidence differs: " + name)
    manifest = source / "docs/P0_P2_REQUIRED_TESTS.json"
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != receipt["required_manifest_sha256"]:
        raise RuntimeError("qualification required manifest differs")
    required = required_names(json.loads(manifest.read_bytes()), receipt["stage"])
    configured = check_inventory(json.loads((evidence / "inventory.json").read_bytes()), required)
    if (check_results(evidence / "full.xml", configured) != receipt["configured_tests"] or
            sorted(required) != receipt["required_tests"] or receipt["skips"] != 0):
        raise RuntimeError("qualification retained result summary differs")
    if json.loads((evidence / "owned-artifacts.json").read_bytes()) != owned_artifacts(build, source):
        raise RuntimeError("qualified owned runtime artifact bytes or paths differ")
    return receipt


def check_schedule(inventory):
    """Parallel execution requires an explicit class and success-bound producers."""
    tests = {test["name"]: test for test in inventory["tests"]}
    properties = {name: {p["name"]: p["value"] for p in test.get("properties", [])}
                  for name, test in tests.items()}
    for name, props in properties.items():
        classes = set(props.get("LABELS", [])) & {
            "ijedi_schedule_light", "ijedi_schedule_heavy", "ijedi_schedule_isolated"}
        if len(classes) != 1:
            raise RuntimeError("test lacks an unambiguous scheduling class: " + name)
        if "ijedi_schedule_heavy" in classes and "ijedi_heavy" not in props.get("RESOURCE_LOCK", []):
            raise RuntimeError("heavy test lacks the shared resource lock: " + name)
        if "ijedi_schedule_isolated" in classes and props.get("RUN_SERIAL") is not True:
            raise RuntimeError("measurement test is not isolated: " + name)
        for producer in props.get("DEPENDS", []):
            fixture = "ijedi_result_" + producer
            if (producer not in properties or
                    fixture not in properties[producer].get("FIXTURES_SETUP", []) or
                    fixture not in props.get("FIXTURES_REQUIRED", [])):
                raise RuntimeError("consumer is not bound to producer success: " + name)


@contextmanager
def execution_lock(path):
    """Protect independent release invocations, not just one CTest scheduler."""
    descriptor = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("another I-JEDI release owns the machine execution lock") from error
        yield
    finally:
        os.close(descriptor)


def job_count(value):
    count = int(value)
    if not 1 <= count <= 4:
        raise argparse.ArgumentTypeError("job count must be between one and four")
    return count


def check_fresh_products(build, source=None):
    # Empty scientific products alone do not establish a fresh I-JEDI build.
    # Independently qualified dependency installations may be reused, but owned
    # core compilation outputs must be created by this qualification invocation.
    database = build / "compile_commands.json"
    if not database.is_file():
        database = next((parent / "compile_commands.json" for parent in build.parents
                         if (parent / "CMakeCache.txt").is_file() and
                         (parent / "compile_commands.json").is_file()), database)
    if database.is_file():
        source = source.resolve(strict=True) if source is not None else None
        for entry in json.loads(database.read_bytes()):
            command = entry.get("command", "") or " ".join(entry.get("arguments", []))
            tokens = shlex.split(command)
            owned = "CMakeFiles/ijedi.dir/" in command.replace("\\", "/")
            if source is not None:
                if not entry.get("file"):
                    raise RuntimeError("compilation command lacks its source input")
                path = Path(entry["file"])
                if not path.is_absolute():
                    path = Path(entry["directory"]) / path
                # Dependency-generated sources may not exist before their first
                # build (fckit's module link is one real example). Determine
                # ownership before requiring an owned source to exist.
                path = path.resolve(strict=False)
                # Bundle dependencies have independent Git/source ownership.
                # Mains, tests, tools and Fortran owned here are fresh too,
                # not only the core C++ library.
                generated_owned = path.is_relative_to(build.resolve())
                owned = owned or generated_owned or (
                    path.is_relative_to(source) and
                    path.relative_to(source).parts[0] != "bundle")
                if owned and not generated_owned:
                    path.resolve(strict=True)
            if not owned:
                continue
            if "-o" not in tokens or tokens.index("-o") + 1 == len(tokens):
                raise RuntimeError("owned compilation has no explicit output path")
            output = Path(tokens[tokens.index("-o") + 1])
            if not output.is_absolute():
                output = Path(entry["directory"]) / output
            if output.exists() or output.is_symlink():
                raise RuntimeError("release build contains prior owned compiled products; "
                                   "use a fresh I-JEDI build tree: " + str(output))
    generated = build / "test/data_generated"
    if generated.exists() and any(path.is_file() or path.is_symlink()
                                  for path in generated.rglob("*")):
        raise RuntimeError("release build contains old generated test products; use a fresh build tree")


def execute(args):
    if args.output.exists():
        raise RuntimeError("release evidence directory already exists; use a fresh path")
    manifest_bytes = args.manifest.read_bytes()
    required = required_names(json.loads(manifest_bytes), args.stage)
    command = ["ctest", "--test-dir", str(args.build)]
    inventory = json.loads(subprocess.check_output(command+["--show-only=json-v1"], text=True))
    configured = check_inventory(inventory, required)
    if args.test_jobs > 1:
        # A legacy/unprotected graph must not become parallel accidentally.
        check_schedule(inventory)
    args.output.mkdir(parents=True)
    (args.output/"inventory.json").write_text(json.dumps(inventory, indent=2)+"\n")
    if args.check_inventory_only:
        (args.output/"inventory-only.json").write_text(json.dumps(dict(
            required=len(required), configured=len(configured), full_gate_executed=False,
            scope="registration_only_not_release_qualification"), indent=2)+"\n")
        print("Required registration present; no release qualification executed")
        return
    revision = source_authority(args.source)
    if args.manifest.resolve(strict=True) != (
            args.source / "docs/P0_P2_REQUIRED_TESTS.json").resolve(strict=True):
        raise RuntimeError("required manifest does not belong to the declared source")
    binding = build_source_binding(args.build, args.source)
    subprocess.run(["git", "-C", str(args.source), "diff", "--check"], check=True)
    check_fresh_products(args.build, args.source)
    subprocess.run(["cmake", "--build", str(args.build), "-j" + str(args.build_jobs)], check=True)
    if (source_authority(args.source) != revision or
            build_source_binding(args.build, args.source) != binding):
        raise RuntimeError("source authority changed during build")
    post_build = json.loads(subprocess.check_output(command+["--show-only=json-v1"], text=True))
    if post_build != inventory:
        raise RuntimeError("configured CTest graph changed during build")
    runtime_artifacts = owned_artifacts(args.build, args.source)
    results = args.output/"full.xml"
    with (args.output/"full.log").open("w") as log:
        run = subprocess.run(command+["--parallel", str(args.test_jobs), "--output-on-failure", "--output-junit",
                                      str(results)], stdout=log, stderr=subprocess.STDOUT)
    if run.returncode != 0:
        raise RuntimeError("complete configured gate failed; inspect fresh full.log")
    count = check_results(results, configured)
    final_inventory = json.loads(subprocess.check_output(command+["--show-only=json-v1"], text=True))
    if (final_inventory != inventory or source_authority(args.source) != revision or
            build_source_binding(args.build, args.source) != binding or
            args.manifest.read_bytes() != manifest_bytes):
        raise RuntimeError("release source, manifest or test graph changed during qualification")
    if owned_artifacts(args.build, args.source) != runtime_artifacts:
        raise RuntimeError("owned runtime artifacts changed during qualification")
    artifact_path = args.output / "owned-artifacts.json"
    artifact_path.write_text(json.dumps(runtime_artifacts, indent=2) + "\n")
    (args.output/"qualified.json").write_text(json.dumps(dict(
        source_commit=revision, stage=args.stage, configured_tests=count, skips=0,
        source_binding=binding,
        owned_artifacts_sha256=hashlib.sha256(artifact_path.read_bytes()).hexdigest(),
        execution_lock_path=str(args.lock_file),
        build_jobs=args.build_jobs, test_jobs=args.test_jobs,
        required_tests=sorted(required),
        required_manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
        inventory_sha256=hashlib.sha256((args.output/"inventory.json").read_bytes()).hexdigest(),
        full_xml_sha256=hashlib.sha256(results.read_bytes()).hexdigest(),
        scope="complete_configured_IJEDI_gate_not_dependency_platform_or_merge_readiness"), indent=2)+"\n")
    print(f"All {count} configured tests passed; zero skips; source={revision}")


def main():
    parser = argparse.ArgumentParser()
    for name in ("build", "source", "manifest", "output"):
        parser.add_argument("--"+name, type=Path, required=name in ("build", "source"))
    parser.add_argument("--stage", choices=("pr1", "pr2"))
    parser.add_argument("--verify-receipt", type=Path)
    parser.add_argument("--check-inventory-only", action="store_true")
    parser.add_argument("--build-jobs", type=job_count, default=2)
    parser.add_argument("--test-jobs", type=job_count, default=2)
    parser.add_argument("--lock-file", type=Path,
                        help="Common mounted lock inode for runs in separate containers")
    args = parser.parse_args()
    if args.verify_receipt is not None:
        verify_qualification_receipt(args.verify_receipt, args.source, args.build)
        print("Qualified source, results and owned runtime artifacts authenticate")
        return
    if any(getattr(args, name) is None for name in ("manifest", "output", "stage", "lock_file")):
        parser.error("qualification requires --manifest, --output, --stage and --lock-file")
    # Nonblocking machine-wide exclusion: no competing release run waits for
    # hours or modifies either build tree. Never unlink a live lock inode.
    # A different TMPDIR must not create another lock namespace on this host.
    # Separate containers must explicitly share a mounted inode. Manual jobs
    # still need the same lock or external scheduling coordination.
    if not args.lock_file.is_absolute():
        parser.error("--lock-file must be an absolute common lock path")
    with execution_lock(args.lock_file):
        execute(args)


if __name__ == "__main__":
    main()
