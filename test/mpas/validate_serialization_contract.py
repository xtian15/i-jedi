#!/usr/bin/env python3
"""Attack the real MPAS continuation envelope and its external bindings."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path
from typing import Any, Callable

from direct_two_step import verify_installed_wheel


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--wheel-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--init", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--namelist", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def changed_digest(value: str) -> str:
    return ("0" if value[0] != "0" else "1") + value[1:]


def main() -> int:
    args = parse_args()
    verify_installed_wheel(args.wheel.resolve(), args.wheel_sha256)

    import torch
    from mpas_pytorch import (
        __version__,
        load_config_from_namelist,
        load_initial_state,
        prepare_initial_state,
        run_simulation,
    )
    from mpas_pytorch.exceptions import MpasContractError
    import mpas_pytorch.ijedi_contracts as contracts

    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    config = load_config_from_namelist(args.namelist)
    initial, support = load_initial_state(
        args.init, args.grid, mesh_support_path=None, config=config
    )
    configuration_receipt = contracts.build_configuration_receipt(
        args.namelist, config
    )
    geometry = contracts.load_ijedi_geometry_snapshot(
        args.init, args.grid, configuration_receipt=configuration_receipt
    )
    reference = run_simulation(
        contracts.clone_state(initial),
        contracts.clone_state(support),
        nsteps=1,
        config=config,
    )
    schema = contracts.build_state_storage_schema(
        reference,
        support,
        horizontal_geometry_receipt=geometry.horizontal_receipt,
        static_vertical_geometry_receipt=geometry.static_vertical_receipt,
        bundle_receipt=geometry.receipt,
        configuration_receipt=configuration_receipt,
    )
    prepared = prepare_initial_state(
        contracts.clone_state(initial), contracts.clone_state(support), config=config
    )
    initial_boundary = contracts.extract_continuation_boundary(prepared, schema=schema)
    step_input = contracts.compose_continuation_state(
        reference, initial_boundary, schema=schema
    )
    completed = run_simulation(step_input, support, nsteps=1, config=config)
    boundary = contracts.extract_continuation_boundary(completed, schema=schema)
    identity = {
        "version": __version__,
        "source_commit": args.source_commit,
        "wheel_sha256": args.wheel_sha256,
    }
    valid_time = "2026-07-17T06:12:00Z"
    time_step = float(config["config_dt"])
    generation = 1
    envelope = contracts.serialize_continuation_envelope(
        boundary,
        schema=schema,
        package_identity=identity,
        state_mode="compact_continuation",
        valid_time=valid_time,
        time_step_seconds=time_step,
        state_generation=generation,
    )

    inspect_kwargs: dict[str, Any] = {
        "expected_package_identity": identity,
        "expected_state_mode": "compact_continuation",
        "expected_valid_time": valid_time,
        "expected_time_step_seconds": time_step,
        "expected_state_generation": generation,
        "expected_horizontal_geometry_receipt": geometry.horizontal_receipt,
        "expected_static_vertical_geometry_receipt": geometry.static_vertical_receipt,
        "expected_bundle_receipt": geometry.receipt,
        "expected_configuration_receipt": configuration_receipt,
        "expected_state_schema_digest": schema.digest,
    }
    inspected = contracts.inspect_continuation_envelope(envelope, **inspect_kwargs)
    restored = contracts.deserialize_continuation_envelope(
        envelope,
        expected_schema=inspected,
        expected_package_identity=identity,
        expected_state_mode="compact_continuation",
        expected_valid_time=valid_time,
        expected_time_step_seconds=time_step,
        expected_state_generation=generation,
    )
    contracts.validate_boundary_compatibility(restored, schema=schema)
    for name in contracts.CONTINUATION_TENSOR_KEYS:
        if not torch.equal(boundary[name], restored[name]):
            raise AssertionError(f"baseline envelope changed continuation tensor {name}")

    failures: dict[str, str] = {}

    def require_contract_failure(label: str, operation: Callable[[], Any]) -> None:
        try:
            operation()
        except MpasContractError as error:
            failures[label] = str(error)
            return
        except Exception as error:  # pragma: no cover - makes the wrong failure loud
            raise AssertionError(
                f"{label} failed with {type(error).__name__}, not MpasContractError"
            ) from error
        raise AssertionError(f"serialization attack was accepted: {label}")

    def inspect_with(**changes: Any) -> Any:
        values = copy.deepcopy(inspect_kwargs)
        values.update(changes)
        return contracts.inspect_continuation_envelope(envelope, **values)

    for label, identity_key in (
        ("wrong_package_version", "version"),
        ("wrong_source_commit", "source_commit"),
        ("wrong_wheel", "wheel_sha256"),
    ):
        altered = dict(identity)
        altered[identity_key] = (
            changed_digest(altered[identity_key])
            if identity_key != "version"
            else altered[identity_key] + ".wrong"
        )
        require_contract_failure(
            label,
            lambda altered=altered: inspect_with(
                expected_package_identity=altered
            ),
        )

    for label, key in (
        ("wrong_horizontal_geometry", "expected_horizontal_geometry_receipt"),
        ("wrong_static_vertical_geometry", "expected_static_vertical_geometry_receipt"),
        ("wrong_geometry_bundle", "expected_bundle_receipt"),
        ("wrong_configuration", "expected_configuration_receipt"),
        ("wrong_state_schema", "expected_state_schema_digest"),
    ):
        require_contract_failure(
            label, lambda key=key: inspect_with(**{key: changed_digest(inspect_kwargs[key])})
        )

    require_contract_failure(
        "wrong_state_mode",
        lambda: inspect_with(expected_state_mode="full_state"),
    )
    require_contract_failure(
        "wrong_valid_time",
        lambda: inspect_with(expected_valid_time="2026-07-17T06:24:00Z"),
    )
    require_contract_failure(
        "wrong_time_step",
        lambda: inspect_with(expected_time_step_seconds=time_step + 1.0),
    )
    require_contract_failure(
        "wrong_generation",
        lambda: inspect_with(expected_state_generation=generation + 1),
    )

    magic = contracts.CONTINUATION_ENVELOPE_MAGIC
    header_size = len(magic) + 16
    metadata_size, payload_size = struct.unpack(">QQ", envelope[len(magic):header_size])
    metadata_start = header_size
    payload_start = metadata_start + metadata_size
    original_metadata = json.loads(
        envelope[metadata_start:payload_start].decode("utf-8")
    )
    original_payload = bytes(envelope[payload_start:payload_start + payload_size])

    def repack(metadata: dict[str, Any], payload: bytes = original_payload) -> bytes:
        encoded = json.dumps(
            metadata, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode("utf-8")
        prefix = magic + struct.pack(">QQ", len(encoded), len(payload)) + encoded + payload
        return prefix + hashlib.sha256(prefix).digest()

    bad_outer_digest = bytearray(envelope)
    bad_outer_digest[-1] ^= 1
    require_contract_failure(
        "outer_digest_byte", lambda: contracts.inspect_continuation_envelope(
            bytes(bad_outer_digest), **inspect_kwargs
        )
    )
    bad_payload = bytearray(original_payload)
    bad_payload[len(bad_payload) // 2] ^= 1
    require_contract_failure(
        "payload_byte",
        lambda: contracts.inspect_continuation_envelope(
            repack(copy.deepcopy(original_metadata), bytes(bad_payload)), **inspect_kwargs
        ),
    )

    missing_field = copy.deepcopy(original_metadata)
    missing_field["fields"].pop()
    require_contract_failure(
        "field_inventory",
        lambda: contracts.inspect_continuation_envelope(
            repack(missing_field), **inspect_kwargs
        ),
    )

    wrong_shape = copy.deepcopy(original_metadata)
    wrong_shape["fields"][0]["shape"].append(1)
    require_contract_failure(
        "field_shape",
        lambda: contracts.deserialize_continuation_envelope(
            repack(wrong_shape),
            expected_schema=schema,
            expected_package_identity=identity,
            expected_state_mode="compact_continuation",
            expected_valid_time=valid_time,
            expected_time_step_seconds=time_step,
            expected_state_generation=generation,
        ),
    )

    wrong_dtype = copy.deepcopy(original_metadata)
    wrong_dtype["fields"][0]["dtype"] = "torch.int64"
    require_contract_failure(
        "field_dtype",
        lambda: contracts.deserialize_continuation_envelope(
            repack(wrong_dtype),
            expected_schema=schema,
            expected_package_identity=identity,
            expected_state_mode="compact_continuation",
            expected_valid_time=valid_time,
            expected_time_step_seconds=time_step,
            expected_state_generation=generation,
        ),
    )

    wrong_backend = copy.deepcopy(original_metadata)
    wrong_backend["backend_id"] += ".wrong"
    require_contract_failure(
        "wrong_backend",
        lambda: contracts.inspect_continuation_envelope(
            repack(wrong_backend), **inspect_kwargs
        ),
    )

    first_name = sorted(contracts.CONTINUATION_TENSOR_KEYS)[0]
    for label, mutate in (
        ("serialize_missing_field", lambda value: value.pop(first_name)),
        (
            "serialize_wrong_shape",
            lambda value: value.__setitem__(
                first_name, value[first_name].reshape(-1)[:-1]
            ),
        ),
        (
            "serialize_wrong_dtype",
            lambda value: value.__setitem__(first_name, value[first_name].to(torch.float32)),
        ),
    ):
        attacked = contracts.clone_state(boundary)
        mutate(attacked)
        require_contract_failure(
            label,
            lambda attacked=attacked: contracts.serialize_continuation_envelope(
                attacked,
                schema=schema,
                package_identity=identity,
                state_mode="compact_continuation",
                valid_time=valid_time,
                time_step_seconds=time_step,
                state_generation=generation,
            ),
        )

    expected_labels = {
        "wrong_package_version", "wrong_source_commit", "wrong_wheel",
        "wrong_horizontal_geometry", "wrong_static_vertical_geometry",
        "wrong_geometry_bundle", "wrong_configuration", "wrong_state_schema",
        "wrong_state_mode", "wrong_valid_time", "wrong_time_step",
        "wrong_generation", "outer_digest_byte", "payload_byte",
        "field_inventory", "field_shape", "field_dtype", "wrong_backend",
        "serialize_missing_field", "serialize_wrong_shape", "serialize_wrong_dtype",
    }
    if set(failures) != expected_labels:
        raise AssertionError("serialization attack inventory is incomplete")

    report = {
        "schema_version": 1,
        "baseline_tensor_count": len(contracts.CONTINUATION_TENSOR_KEYS),
        "baseline_bitwise_restore": True,
        "attack_count": len(failures),
        "attacks": dict(sorted(failures.items())),
        "state_schema_digest": schema.digest,
        "geometry_bundle_receipt": geometry.receipt,
        "wheel_sha256": args.wheel_sha256,
        "source_commit": args.source_commit,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    temporary.replace(args.output)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
