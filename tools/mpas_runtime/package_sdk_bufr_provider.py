# Copyright (C) 2026 IC Weather LLC
"""Package an authenticated SDK provider for one locked Python environment.

No source or native binary is rebuilt. This artifact is tied to the declared
read-only SDK and is neither portable nor approved for publication.
"""
import argparse
import base64
import csv
import hashlib
import io
import json
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED


def build_provider(sdk, output, identity):
    sdk, output = Path(sdk), Path(output)
    if output.exists():
        raise RuntimeError("provider output already exists")
    source = sdk / "view/lib/python3.11/site-packages/bufr"
    payload, origins = {}, {}
    for path in sorted(source.rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        actual = path.resolve(strict=True)
        if (not actual.is_relative_to((sdk / "store").resolve(strict=True)) or
                identity["sdk_spec_component"] not in str(actual)):
            raise RuntimeError("provider source escapes the declared SDK component")
        name = "bufr/" + str(path.relative_to(source))
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if identity["members"].get(name) != digest:
            raise RuntimeError("SDK provider member differs from exact authority: " + name)
        payload[name] = data
        origins[name] = dict(source=str(actual), sha256=digest)
    if set(payload) != set(identity["members"]):
        raise RuntimeError("SDK provider member inventory differs from exact authority")
    info = "bufr_query_sdk-0.0.4.dist-info/"
    payload[info + "METADATA"] = (
        "Metadata-Version: 2.1\nName: bufr-query-sdk\nVersion: 0.0.4\n"
        "Summary: Exact native I-JEDI SDK BUFR provider, local qualification only\n"
        "Requires-Python: >=3.11,<3.12\nRequires-Dist: numpy==2.0.2\n"
        "Requires-Dist: PyYAML==6.0.2\n\n").encode()
    payload[info + "WHEEL"] = (
        "Wheel-Version: 1.0\nGenerator: qualified-sdk-provider\n"
        "Root-Is-Purelib: false\nTag: cp311-cp311-linux_aarch64\n").encode()
    manifest = dict(scope="exact_local_read_only_SDK_provider_not_portable_or_published",
                    sdk_receipt_sha256=identity["sdk_receipt_sha256"],
                    upstream_sdk_component="bufr-query-0.0.4", members=origins)
    payload[info + "SDK-PROVIDER.json"] = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    record = io.StringIO(newline="")
    writer = csv.writer(record, lineterminator="\n")
    for name, data in sorted(payload.items()):
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).decode().rstrip("=")
        writer.writerow((name, "sha256=" + digest, len(data)))
    writer.writerow((info + "RECORD", "", ""))
    payload[info + "RECORD"] = record.getvalue().encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED, compresslevel=9) as wheel:
        for name, data in sorted(payload.items()):
            entry = ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            wheel.writestr(entry, data, compresslevel=9)
    return dict(wheel=str(output), sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                provider_members=len(origins), total_members=len(payload))


def main():
    parser = argparse.ArgumentParser()
    for name in ("sdk", "sdk-integrity-receipt", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    artifacts = Path(__file__).resolve().parents[2] / "cmake/MpasDependencyArtifacts.json"
    identity = json.loads(artifacts.read_bytes())["bufr_sdk_provider"]
    if hashlib.sha256(args.sdk_integrity_receipt.read_bytes()).hexdigest() != identity["sdk_receipt_sha256"]:
        raise RuntimeError("SDK integrity receipt differs from the qualified prerequisite")
    report = build_provider(args.sdk, args.output, identity)
    if report["sha256"] != identity["wheel_sha256"]:
        raise RuntimeError("rebuilt provider wheel differs from exact artifact authority")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
