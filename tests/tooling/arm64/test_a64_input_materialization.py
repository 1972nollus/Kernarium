#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Projection, conflict, and mutation negatives for A64 input JSONL."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import csv
import gzip
import hashlib
import json
import os
import sqlite3
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from a64_aslref_state_digest import POINTER_AUTH_KEYS
from a64_input_materialization import (
    CONTRACT_SCHEMA,
    EXCLUDED_CPU_STATE_FIELDS,
    MEMORY_BLOB_SCHEMA,
    PROJECTION_INVENTORY_SCHEMA,
    PROGRAM_BLOB_SCHEMA,
    RAW_SCHEMA,
    STATE_BLOB_SCHEMA,
    STATE_CLASS_PATHS,
    WIRE_EXCLUDED_CLASSES,
    canonical_json,
    content_id,
    contract_payload,
    iter_raw_materializations,
    materialization_id,
    projection_inventory,
    raw_contract_id,
    raw_contract_tsv,
    read_callback_contract_registry,
    read_raw_materializations,
    regions_digest,
    representation_obligations,
    state_digest,
    validate_raw_materialization,
    validate_callback_contract_registry_use,
    validate_callback_registry_source_coverage,
    wire_json,
)
from aggregate_a64_raw_instruction_evidence import OUTPUT_COLUMNS
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from validate_a64_instruction_aggregate_manifest import (
    validate_checked_manifest_rows,
)
from join_a64_input_materializations import (
    CHECKPOINT_METADATA_ALLOWANCE_BYTES,
    CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
    CONTRACT_MANIFEST_SCHEMA,
    DEFAULT_SPOOL_BYTES_PER_CONTRIBUTION,
    ContributionSpool,
    SPOOL_METADATA_ALLOWANCE_BYTES,
    declaration,
    evidence_sort_key,
    evidence_sort_key_blob,
    join_materializations,
    map_raw_contract,
    mapping_context,
    materialization_paths,
    producer_storage_caps,
    require_complete_producer_coverage,
    require_checkpoint_free_space,
    reserve_bytes_from_gib,
    reusable_contribution_checkpoint,
    validate_join_output_paths,
    verify_contribution_checkpoint,
    verify_contract_manifest,
    verify_index_tsv,
    write_contribution_checkpoint,
    write_contract_manifest,
    write_tsv,
)
from a64_raw_evidence_files import filesystem_is_case_sensitive


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except ValueError:
        return
    raise AssertionError(message)


def failure_message(action) -> str:
    try:
        action()
    except ValueError as error:
        return str(error)
    raise AssertionError("expected action to fail")


def state() -> dict[str, object]:
    return {
        "x": [f"{index:016x}" for index in range(31)],
        "sp": "0000000051000800",
        "pc": "000000004f000000",
        "nzcv": "a0000000",
        "vectors": [bytes([index] * 16).hex() for index in range(32)],
        "fpcr": "00000000",
        "fpsr": "00000000",
        "fpmr": "0000000000000000",
        "tls": "0000000000000000",
        "mte": {
            "enabled": False,
            "exclude": "0000",
            "tag_choices": "00" * 256,
            "random_tags": False,
            "seed": "0001",
            "tag": "00",
            "random_state": "9e3779b97f4a7c15",
        },
        "checked_arithmetic": {
            "add_enabled": False,
            "multiply_enabled": False,
            "accdata": "00000000",
        },
        "pstate": {
            "ssbs": False,
            "dit": False,
            "tco": False,
            "sm": False,
            "za": False,
            "daif_access": False,
            "daif": "00",
        },
        "exclusive_monitor": {
            "address": "ffffffffffffffff",
            "key": "0000000000000000",
            "value": "0000000000000000",
            "value2": "0000000000000000",
            "version": "0000000000000000",
            "size": "00",
        },
        "guarded_stack": {
            "pointer": "0000000000000000",
            "implemented": False,
            "push_enabled": False,
            "store_enabled": False,
        },
        "signing_keys": [
            {"low": f"{low:016x}", "high": f"{high:016x}"}
            for low, high in POINTER_AUTH_KEYS
        ],
        "signing_enabled": True,
    }


def memory_region() -> dict[str, object]:
    initial = bytes(range(32))
    expected = bytes(reversed(range(32)))
    return {
        "address": "0000000051000000",
        "size": "0000000000000020",
        "initial_bytes": initial.hex(),
        "expected_bytes": expected.hex(),
        "initial_tags": "0001",
        "expected_tags": "0203",
        "permissions": {
            "readable": True,
            "writable": True,
            "executable": False,
            "guest_code": False,
            "guest_writable": True,
        },
    }


def program(page_size: int) -> dict[str, object]:
    image = bytearray(page_size)
    words = ((0, 0x91000400), (4, 0xD4000001))
    for offset, word in words:
        image[offset : offset + 4] = word.to_bytes(4, "little")
    return {
        "guest_page_size": f"{page_size:08x}",
        "code_base": "000000004f000000",
        "entry_offset": "0000000000000000",
        "page_encoding": "zero-fill-sparse-words/v1",
        "words": [
            {"offset": f"{offset:016x}", "word": f"{word:08x}"}
            for offset, word in words
        ],
        "reconstructed_page_sha256": hashlib.sha256(image).hexdigest(),
        "permissions": {
            "readable": True,
            "writable": False,
            "executable": True,
            "guest_code": True,
            "guest_writable": False,
        },
    }


def identify(record: dict[str, object]) -> dict[str, object]:
    record["program_id"] = content_id("program-", record["program"])
    record["initial_state_id"] = content_id("state-", record["initial_state"])
    record["expected_state_id"] = content_id("state-", record["expected_state"])
    record["memory_id"] = content_id("memory-", record["memory_regions"])
    record["input_contract_id"] = content_id(
        "input-contract-", contract_payload(record)
    )
    return record


def finish(record: dict[str, object]) -> dict[str, object]:
    initial = record["initial_state"]
    expected = record["expected_state"]
    memory = record["memory_regions"]
    assert isinstance(initial, dict) and isinstance(expected, dict)
    assert isinstance(memory, list)
    record["integrity"] = {
        "initial_state_digest": state_digest(initial),
        "initial_memory_digest": regions_digest(memory, False),
        "expected_state_digest": state_digest(expected),
        "expected_memory_digest": regions_digest(memory, True),
    }
    return identify(record)


def compact_events(record: dict[str, object]) -> list[dict[str, object]]:
    return [
        {
            "schema": PROGRAM_BLOB_SCHEMA,
            "id": record["program_id"],
            "payload": record["program"],
        },
        {
            "schema": STATE_BLOB_SCHEMA,
            "id": record["initial_state_id"],
            "payload": record["initial_state"],
        },
        {
            "schema": STATE_BLOB_SCHEMA,
            "id": record["expected_state_id"],
            "payload": record["expected_state"],
        },
        {
            "schema": MEMORY_BLOB_SCHEMA,
            "id": record["memory_id"],
            "payload": record["memory_regions"],
        },
        {
            "schema": CONTRACT_SCHEMA,
            "id": record["input_contract_id"],
            "payload": contract_payload(record),
            "raw_occurrences": "0000000000000001",
        },
    ]


def event_stream(
    records: list[dict[str, object]], *, repeat_contract: int = 1
) -> str:
    blobs: dict[str, dict[str, object]] = {}
    contracts: list[dict[str, object]] = []
    for record in records:
        events = compact_events(record)
        for event in events[:-1]:
            identifier = str(event["id"])
            previous = blobs.get(identifier)
            if previous is not None:
                require(previous == event, "fixture content ID collision")
            else:
                blobs[identifier] = event
        contracts.extend([events[-1]] * repeat_contract)
    return "".join(
        wire_json(event) + "\n" for event in [*blobs.values(), *contracts]
    )


def materialization(page_size: int = 4096) -> dict[str, object]:
    initial = state()
    expected = copy.deepcopy(initial)
    expected["x"][0] = "0000000000000001"
    expected["pc"] = "000000004f000008"
    return finish(
        {
            "schema": RAW_SCHEMA,
            "identity": {
                "ctest": "arm64-materialization-fixture",
                "case": "exact concrete input",
                "source": "tests/unit/arm64/materialization_fixture.cpp",
                "line": "00000011",
                "scope": "instruction",
                "evidence_word": "91000400",
            },
            "program": program(page_size),
            "feature_profile": {
                "native_required": [],
                "native_observed": "baseline=1",
                "native_missing": "none",
            },
            "initial_state": initial,
            "expected_state": expected,
            "memory_regions": [memory_region()],
            "terminal": {
                "event": "system-call",
                "retirement": "exact:2",
                "instruction_budget": "0000000000000040",
                "state_oracle": "exact",
                "fault": "none",
                "syndrome": "00000000",
                "invalid_encoding": False,
            },
            "engine_results": {
                "interpreter": "executed-retired",
                "gadgets": "executed-retired",
                "jit": "executed-retired",
            },
            "representability": {
                "status": "pending-official-model-projection",
                "obligations": [],
                "callback_contracts": {
                    "validator": None,
                    "before_engine": None,
                    "after_engine": None,
                },
                "excluded_engine_private_fields": list(WIRE_EXCLUDED_CLASSES),
            },
            "integrity": {},
            "raw_occurrences": "0000000000000001",
        }
    )


def mutate_class(record: dict[str, object], name: str) -> None:
    target = record["initial_state"]
    assert isinstance(target, dict)
    if name == "general-registers":
        target["x"][0] = "ffffffffffffffff"
    elif name == "condition-flags":
        target["nzcv"] = "b0000000"
    elif name == "fixed-vector-registers":
        target["vectors"][0] = "ff" + target["vectors"][0][2:]
    elif name == "floating-point-controls":
        target["fpmr"] = "0000000000000001"
    elif name == "thread-register":
        target["tls"] = "0000000000000001"
    elif name == "memory-tag-generator":
        target["mte"]["enabled"] = True
    elif name == "checked-arithmetic":
        target["checked_arithmetic"]["add_enabled"] = True
    elif name == "modeled-pstate":
        target["pstate"]["dit"] = True
    elif name == "exclusive-monitor":
        target["exclusive_monitor"]["address"] = "0000000000000000"
    elif name == "guarded-control-stack":
        target["guarded_stack"]["pointer"] = "0000000000000010"
    elif name == "pointer-authentication":
        target["signing_keys"][0]["low"] = "0000000000000000"
    elif name == "extended-vector-mode":
        record["feature_profile"]["native_required"] = ["sve"]
        record["feature_profile"]["native_observed"] = "sve=1"
    else:
        raise AssertionError(f"missing mutation for projection class {name}")


def projection_mutation_fixture() -> None:
    baseline = materialization()
    validate_raw_materialization(baseline)
    expected_classes = {
        "general-registers",
        "condition-flags",
        "fixed-vector-registers",
        "floating-point-controls",
        "thread-register",
        "memory-tag-generator",
        "checked-arithmetic",
        "modeled-pstate",
        "exclusive-monitor",
        "guarded-control-stack",
        "pointer-authentication",
        "extended-vector-mode",
    }
    require(set(STATE_CLASS_PATHS) == expected_classes, "projection class lacks a mutation negative")
    for name in STATE_CLASS_PATHS:
        changed = copy.deepcopy(baseline)
        mutate_class(changed, name)
        if name == "extended-vector-mode":
            identify(changed)
            validate_raw_materialization(changed)
            require(
                "unrepresentable-cpustate-sve-register-file-and-vl"
                in representation_obligations(changed),
                "missing precise SVE CpuState obligation",
            )
        else:
            expect_failure(
                lambda changed=changed: validate_raw_materialization(changed),
                f"{name} mutation survived digest validation",
            )


def program_memory_negative_fixture() -> None:
    mutations = (
        lambda row: row["program"]["words"][0].__setitem__("word", "d503201f"),
        lambda row: row["program"].__setitem__(
            "reconstructed_page_sha256", "0" * 64
        ),
        lambda row: row["memory_regions"][0].__setitem__(
            "initial_bytes", "ff" + row["memory_regions"][0]["initial_bytes"][2:]
        ),
        lambda row: row["memory_regions"][0].__setitem__("initial_tags", "0f01"),
        lambda row: row["memory_regions"][0]["permissions"].__setitem__("readable", False),
        lambda row: row["memory_regions"][0].__setitem__("address", "000000004f000010"),
    )
    for mutate in mutations:
        changed = materialization()
        mutate(changed)
        expect_failure(
            lambda changed=changed: validate_raw_materialization(changed),
            "program/memory perturbation survived validation",
        )


def feature_engine_schema_negative_fixture() -> None:
    mutations = (
        lambda row: row["representability"].pop("obligations"),
        lambda row: row["engine_results"].__setitem__("jit", "invalid"),
        lambda row: row["feature_profile"].__setitem__(
            "native_observed", "baseline=0"
        ),
        lambda row: (
            row["feature_profile"].__setitem__("native_required", ["sve"]),
            row["feature_profile"].__setitem__("native_observed", "sve=0"),
            row["feature_profile"].__setitem__("native_missing", "none"),
        ),
        lambda row: row["representability"]["callback_contracts"].__setitem__(
            "validator",
            {
                "identity": (
                    "custom-validator@tests/unit/arm64/"
                    "materialization_fixture.cpp:17"
                ),
                "relation_id": "not-versioned",
            },
        ),
    )
    for mutate in mutations:
        changed = materialization()
        mutate(changed)
        expect_failure(
            lambda changed=changed: validate_raw_materialization(changed),
            "wire schema/native observation corruption was accepted",
        )

    callback = materialization()
    callback_identity = (
        "custom-validator@tests/unit/arm64/materialization_fixture.cpp:17"
    )
    callback["terminal"]["state_oracle"] = callback_identity
    callback["representability"]["callback_contracts"]["validator"] = {
        "identity": callback_identity,
        "relation_id": "allowed-result-choice/v1",
    }
    callback["representability"]["callback_contracts"]["before_engine"] = {
        "identity": (
            "before-engine-hook@tests/unit/arm64/"
            "materialization_fixture.cpp:17"
        ),
        "effect_id": "install-memory-choice/v1",
    }
    identify(callback)
    validate_raw_materialization(callback)
    changed = copy.deepcopy(callback)
    changed["terminal"]["state_oracle"] = "exact"
    expect_failure(
        lambda: validate_raw_materialization(changed),
        "validator/state-oracle identity drift was accepted",
    )


def raw_projection_fixture() -> None:
    record = materialization()
    raw = raw_contract_tsv(record)
    require(list(raw) == [
        "ctest", "case", "source", "line", "encoding", "invalid",
        "guest_page_size", "expected_event", "expected_retirement",
        "expected_state_oracle", "initial_state_digest",
        "initial_memory_digest", "expected_state_digest",
        "expected_memory_digest", "expected_fault", "expected_syndrome",
        "native_required_features", "native_requirement_scope",
        "native_observed_features", "native_missing_features",
        "interpreter_result", "gadgets_result", "jit_result", "occurrences",
    ], "raw projection columns drifted")
    require(raw["occurrences"] == "1", "raw occurrence conversion is not exact")
    require(raw["source"].startswith("tests/"), "stable source path was not preserved")
    require(raw_contract_id(record).startswith("raw-contract-"), "missing raw identity")
    require(materialization_id(record).startswith("input-contract-"), "missing payload identity")


def duplicate_and_raw_instance_fixture() -> None:
    record = materialization(16384)
    with tempfile.TemporaryDirectory(prefix="a64-input-jsonl-") as temporary:
        path = Path(temporary) / "inputs.jsonl"
        path.write_text(
            event_stream([record], repeat_contract=2),
            encoding="utf-8",
        )
        require(len(read_raw_materializations([path])) == 1, "duplicate was not deduped")
        require(len(list(iter_raw_materializations([path]))) == 1, "streaming dedupe differs")
        require(
            read_raw_materializations([path])[0]["raw_occurrences"]
            == "0000000000000002",
            "duplicate contract occurrences were not summed",
        )
        distinct = copy.deepcopy(record)
        distinct["representability"]["status"] = "unrepresentable"
        distinct["representability"]["obligations"] = [
            "unrepresentable-engine-hook"
        ]
        identify(distinct)
        validate_raw_materialization(distinct)
        require(
            raw_contract_tsv(distinct) == raw_contract_tsv(record),
            "exact payload mutation changed the coarse evidence projection",
        )
        require(
            raw_contract_id(distinct) != raw_contract_id(record),
            "raw-instance identity omitted the exact input contract",
        )
        path.write_text(
            event_stream([record, distinct]),
            encoding="utf-8",
        )
        reconstructed = list(iter_raw_materializations([path]))
        require(
            len(reconstructed) == 2
            and len({raw_contract_id(item) for item in reconstructed}) == 2,
            "distinct exact inputs sharing raw evidence were collapsed",
        )


def compact_stream_negative_fixture() -> None:
    baseline = materialization()
    with tempfile.TemporaryDirectory(prefix="a64-input-v3-") as temporary:
        path = Path(temporary) / "inputs.jsonl"
        events = compact_events(baseline)

        path.write_text(
            "".join(wire_json(event) + "\n" for event in events if event is not events[1]),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "missing state blob was accepted",
        )

        corrupted = copy.deepcopy(events)
        corrupted[0]["payload"]["reconstructed_page_sha256"] = "0" * 64
        path.write_text(
            "".join(wire_json(event) + "\n" for event in corrupted),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "corrupted blob payload was accepted",
        )

        conflicting_id = copy.deepcopy(events)
        conflicting_id[1]["id"] = "state-" + "0" * 64
        path.write_text(
            "".join(wire_json(event) + "\n" for event in conflicting_id),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "conflicting claimed content ID was accepted",
        )

        changed_budget = copy.deepcopy(events)
        changed_budget[-1]["payload"]["terminal"][
            "instruction_budget"
        ] = "000000000000003f"
        path.write_text(
            "".join(wire_json(event) + "\n" for event in changed_budget),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "instruction-budget-only mutation kept the old contract ID",
        )
        rebound_budget = copy.deepcopy(changed_budget[-1])
        rebound_budget["id"] = content_id(
            "input-contract-", rebound_budget["payload"]
        )
        require(
            rebound_budget["id"] != events[-1]["id"],
            "instruction budget is absent from the content identity",
        )

        other_program = materialization()
        other_program["program"]["words"][1]["word"] = "d503201f"
        program_image = bytearray(4096)
        for word in other_program["program"]["words"]:
            offset = int(word["offset"], 16)
            program_image[offset : offset + 4] = int(
                word["word"], 16
            ).to_bytes(4, "little")
        other_program["program"]["reconstructed_page_sha256"] = (
            hashlib.sha256(program_image).hexdigest()
        )
        identify(other_program)
        stale_contract = copy.deepcopy(events[-1])
        stale_contract["payload"]["program_id"] = other_program["program_id"]
        stale_program = [
            *events[:-1],
            compact_events(other_program)[0],
            stale_contract,
        ]
        path.write_text(
            "".join(wire_json(event) + "\n" for event in stale_program),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "cross-program swap kept the old exact contract ID",
        )

        duplicate_blob = [*events[:-1], events[0], events[-1]]
        path.write_text(
            "".join(wire_json(event) + "\n" for event in duplicate_blob),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "duplicate blob in one producer was accepted",
        )

        other = materialization()
        other["initial_state"]["x"][2] = "0000000000000042"
        finish(other)
        swapped_contract = copy.deepcopy(compact_events(baseline)[-1])
        swapped_contract["payload"]["initial_state_id"] = other[
            "initial_state_id"
        ]
        swapped_contract["id"] = content_id(
            "input-contract-", swapped_contract["payload"]
        )
        all_blobs: dict[str, dict[str, object]] = {}
        for record in (baseline, other):
            for event in compact_events(record)[:-1]:
                all_blobs.setdefault(str(event["id"]), event)
        path.write_text(
            "".join(
                wire_json(event) + "\n"
                for event in [*all_blobs.values(), swapped_contract]
            ),
            encoding="utf-8",
        )
        expect_failure(
            lambda: list(iter_raw_materializations([path])),
            "cross-swapped state blob survived integrity validation",
        )

        compressed = Path(temporary) / "inputs.jsonl.gz"
        with gzip.open(compressed, "wt", encoding="utf-8", newline="") as output:
            output.write(event_stream([baseline]))
        require(
            len(list(iter_raw_materializations([compressed]))) == 1,
            "transparent gzip reconstruction differs",
        )
        compressed_bytes = compressed.read_bytes()
        truncated = Path(temporary) / "truncated.jsonl.gz"
        truncated.write_bytes(compressed_bytes[:-8])
        expect_failure(
            lambda: list(iter_raw_materializations([truncated])),
            "truncated gzip materialization stream was accepted",
        )
        damaged_bytes = bytearray(compressed_bytes)
        damaged_bytes[len(damaged_bytes) // 2] ^= 0x80
        damaged = Path(temporary) / "damaged.jsonl.gz"
        damaged.write_bytes(damaged_bytes)
        expect_failure(
            lambda: list(iter_raw_materializations([damaged])),
            "corrupt gzip materialization stream was accepted",
        )


def large_repeated_stream_fixture() -> None:
    record = materialization(16384)
    events = compact_events(record)
    with tempfile.TemporaryDirectory(prefix="a64-input-large-") as temporary:
        path = Path(temporary) / "inputs.jsonl"
        with path.open("w", encoding="utf-8", newline="") as output:
            for event in events[:-1]:
                output.write(wire_json(event) + "\n")
            contract_line = wire_json(events[-1]) + "\n"
            for _ in range(4096):
                output.write(contract_line)
        records = list(iter_raw_materializations([path]))
        require(len(records) == 1, "repeated compact contracts were expanded")
        require(
            records[0]["raw_occurrences"] == "0000000000001000",
            "large repeated contract count drifted",
        )
        require(path.stat().st_size < 16 * 1024 * 1024, "compact stream grew unbounded")


def obligation_fixture() -> None:
    record = materialization(16384)
    record["initial_state"]["nzcv"] = "a0000001"
    record["expected_state"]["nzcv"] = "a0000001"
    record = finish(record)
    obligations = representation_obligations(record)
    require(
        obligations
        == ("unrepresentable-nzcv-storage-bits",),
        f"unexpected typed obligations: {obligations}",
    )


def inventory_fixture() -> None:
    inventory = projection_inventory()
    require(inventory["schema"] == PROJECTION_INVENTORY_SCHEMA, "inventory lacks version")
    fields = tuple(
        field
        for item in inventory["excluded_classes"]
        for field in item["fields"]
    )
    require(
        set(fields) == set(EXCLUDED_CPU_STATE_FIELDS)
        and len(fields) == len(EXCLUDED_CPU_STATE_FIELDS),
        "excluded field proof is incomplete",
    )
    require("not-compared-not-digested" in canonical_json(inventory), "private exclusion lacks proof")
    checked_path = (
        Path(__file__).resolve().parents[2]
        / "unit/arm64/a64_input_materialization_projection.json"
    )
    checked = json.loads(checked_path.read_text(encoding="utf-8"))
    require(checked == inventory, "checked projection inventory drifted")


def callback_registry_fixture() -> None:
    repository = Path(__file__).resolve().parents[3]
    registry_path = repository / "tests/unit/arm64/a64_callback_contracts.tsv"
    registry = read_callback_contract_registry(registry_path, repository)
    validate_callback_registry_source_coverage(
        registry, repository / "tests/unit/arm64"
    )

    callback = materialization()
    identity = "custom-validator@tests/unit/arm64/materialization_fixture.cpp:17"
    callback["terminal"]["state_oracle"] = identity
    callback["representability"]["callback_contracts"]["validator"] = {
        "identity": identity,
        "relation_id": "exact-increment/v1",
    }
    identify(callback)
    validate_raw_materialization(callback)
    validate_callback_contract_registry_use(callback, registry)

    unknown = copy.deepcopy(callback)
    unknown["representability"]["callback_contracts"]["validator"][
        "relation_id"
    ] = "unknown-relation/v1"
    identify(unknown)
    validate_raw_materialization(unknown)
    expect_failure(
        lambda: validate_callback_contract_registry_use(unknown, registry),
        "unknown callback descriptor was accepted",
    )
    wrong_kind = {name: dict(row) for name, row in registry.items()}
    wrong_kind["exact-increment/v1"]["kind"] = "after_engine"
    expect_failure(
        lambda: validate_callback_contract_registry_use(callback, wrong_kind),
        "wrong-kind callback descriptor was accepted",
    )
    unused = {name: dict(row) for name, row in registry.items()}
    unused["unused-reviewed-token/v1"] = {
        "descriptor_id": "unused-reviewed-token/v1",
        "kind": "validator",
        "allowed_result_constraint": "fixture-only negative",
        "independent_fixture": (
            "tests/unit/arm64/arm64_instruction_materialization_tests.cpp:"
            "emit_duplicate"
        ),
    }
    expect_failure(
        lambda: validate_callback_registry_source_coverage(
            unused, repository / "tests/unit/arm64"
        ),
        "unused callback descriptor was accepted",
    )

    with tempfile.TemporaryDirectory(prefix="a64-callback-registry-") as temporary:
        missing_fixture = Path(temporary) / "registry.tsv"
        missing_fixture.write_text(
            "descriptor_id\tkind\tallowed_result_constraint\t"
            "independent_fixture\n"
            "negative-fixture/v1\tvalidator\treviewed constraint\t"
            "tests/unit/arm64/does_not_exist.cpp:fixture\n",
            encoding="utf-8",
        )
        expect_failure(
            lambda: read_callback_contract_registry(
                missing_fixture, repository
            ),
            "missing independent fixture path was accepted",
        )


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        require(reader.fieldnames == columns, f"{path}: fixture columns drifted")
        return list(reader)


def aggregate_manifest_validation_fixture() -> None:
    repository = Path(__file__).resolve().parents[3]
    manifest_path = (
        repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv"
    )
    rows = read_tsv(manifest_path, OUTPUT_COLUMNS)
    validate_checked_manifest_rows(rows)

    full_extent_ids = {
        "aggregate-fdf1b81297fb9e6783e1",
        "aggregate-2074219a783c75b695e3",
    }
    require(
        full_extent_ids <= {row["aggregate_id"] for row in rows},
        "checked manifest lacks the two full-extent aggregate identities",
    )
    stale = [row for row in rows if row["aggregate_id"] not in full_extent_ids]
    require(len(stale) == 32167, "stale-manifest negative is not 32,167 rows")
    expect_failure(
        lambda: validate_checked_manifest_rows(stale),
        "stale 32,167-row aggregate manifest was accepted",
    )

    missing = [
        row
        for row in rows
        if row["aggregate_id"] != "aggregate-fdf1b81297fb9e6783e1"
    ]
    expect_failure(
        lambda: validate_checked_manifest_rows(missing),
        "aggregate manifest missing one new full-extent row was accepted",
    )

    cross_page = [dict(row) for row in rows]
    by_id = {row["aggregate_id"]: row for row in cross_page}
    first = by_id["aggregate-fdf1b81297fb9e6783e1"]
    second = by_id["aggregate-2074219a783c75b695e3"]
    first["guest_page_size"], second["guest_page_size"] = (
        second["guest_page_size"],
        first["guest_page_size"],
    )
    expect_failure(
        lambda: validate_checked_manifest_rows(cross_page),
        "cross-page-swapped full-extent rows were accepted",
    )

    duplicate = sorted(
        [*rows, dict(rows[0])], key=lambda row: row["aggregate_id"]
    )
    expect_failure(
        lambda: validate_checked_manifest_rows(duplicate),
        "duplicate aggregate row was accepted",
    )

    unrelated = dict(rows[-1])
    unrelated["aggregate_id"] = "aggregate-ffffffffffffffffffff"
    unrelated_extra = sorted(
        [*rows, unrelated], key=lambda row: row["aggregate_id"]
    )
    expect_failure(
        lambda: validate_checked_manifest_rows(unrelated_extra),
        "unrelated extra aggregate row was accepted",
    )


def aggregate_join_fixture() -> None:
    repository = Path(__file__).resolve().parents[3]
    aggregate_path = (
        repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv"
    )
    required_path = (
        repository / "tests/unit/arm64/a64_instruction_required_variants.tsv"
    )
    decode_path = (
        repository / "tests/unit/arm64/a64_instruction_decode_partitions.tsv"
    )
    aggregates = read_tsv(aggregate_path, OUTPUT_COLUMNS)
    required = read_tsv(required_path, REQUIRED_COLUMNS)
    decodes = read_tsv(decode_path, DECODE_COLUMNS)
    registry = read_callback_contract_registry(
        repository / "tests/unit/arm64/a64_callback_contracts.tsv", repository
    )
    context = mapping_context(required, decodes)

    first = materialization()
    second = materialization()
    second["program"]["words"][1]["word"] = "d503201f"
    image = bytearray(4096)
    for word in second["program"]["words"]:
        offset = int(word["offset"], 16)
        image[offset : offset + 4] = int(word["word"], 16).to_bytes(4, "little")
    second["program"]["reconstructed_page_sha256"] = hashlib.sha256(
        image
    ).hexdigest()
    identify(second)
    records = [first, second]
    raws = [raw_contract_tsv(record) for record in records]
    require(
        raws[0] == raws[1]
        and raw_contract_id(first) != raw_contract_id(second),
        "cross-program fixture is not one coarse row/two exact inputs",
    )
    mapped = map_raw_contract(raws[0], context)
    require(len(mapped) == 1, "join fixture has ambiguous official allocation")
    require(
        map_raw_contract(raws[1], context)[0].aggregate_id
        == mapped[0].aggregate_id,
        "join fixture records did not coalesce",
    )
    identity = mapped[0]
    template_index = next(
        index
        for index, row in enumerate(aggregates)
        if row["stable_id"] == identity.fields["stable_id"]
        and row["architecture_encoding"]
        == identity.fields["architecture_encoding"]
        and row["architecture_mask"] == identity.fields["architecture_mask"]
        and row["architecture_value"] == identity.fields["architecture_value"]
        and row["architecture_constraints"]
        == identity.fields["architecture_constraints"]
        and row["decode_partition_id"]
        == identity.fields["decode_partition_id"]
        and row["guest_page_size"] == "4096"
    )
    synthetic = dict(aggregates[template_index])
    synthetic.update(identity.fields)
    synthetic["aggregate_id"] = identity.aggregate_id
    digest = hashlib.sha256()
    native_raw = dict(raws[0])
    native_raw["occurrences"] = "2"
    digest.update(declaration(native_raw))
    synthetic["declared_count"] = "2"
    synthetic["declared_digest"] = digest.hexdigest()
    fixture_aggregates = list(aggregates)
    fixture_aggregates[template_index] = synthetic

    with tempfile.TemporaryDirectory(prefix="a64-aggregate-join-") as temporary:
        path = Path(temporary) / "inputs.jsonl"
        contract_index = Path(temporary) / "contracts.tsv.gz"
        checkpoint_directory = Path(temporary) / "checkpoints"
        # Both exact programs share one native EvidenceKey.  The trace keeps
        # both input IDs while the aggregate declaration re-coalesces count 2.
        path.write_text(
            event_stream(records),
            encoding="utf-8",
        )
        checkpoint_directory.mkdir()
        expect_failure(
            lambda: require_checkpoint_free_space(
                checkpoint_directory,
                path,
                2,
                reserve_bytes=10**30,
                spool_bytes_per_contribution=1024,
                checkpoint_bytes_per_contribution=128,
            ),
            "checkpoint free-space exhaustion was accepted",
        )
        joined = join_materializations(
            [path],
            fixture_aggregates,
            required,
            decodes,
            registry,
            guest_page_size=4096,
            allow_partial=True,
            contract_index_output=contract_index,
            checkpoint_directory=checkpoint_directory,
        )
        require(len(joined) == 1, "partial exact join produced extra rows")
        require(joined[0]["aggregate_id"] == identity.aggregate_id, "join ID drift")
        require(joined[0]["declared_digest"] == digest.hexdigest(), "join order drift")
        require(joined[0]["input_contract_count"] == "2", "join lost a contract")
        with gzip.open(
            contract_index, "rt", encoding="utf-8", newline=""
        ) as source:
            manifest = list(csv.DictReader(source, delimiter="\t"))
        require(
            len(manifest) == 1
            and manifest[0]["schema"] == CONTRACT_MANIFEST_SCHEMA,
            "compressed contract shard manifest drifted",
        )
        checkpoint = contract_index.parent / manifest[0]["checkpoint_file"]
        trace: list[dict[str, object]] = []
        info = verify_contribution_checkpoint(
            checkpoint, consume=lambda row: trace.append(dict(row))
        )
        require(
            len(trace) == 2 and info.contribution_count == 2,
            "streamed contract traceability shard drifted",
        )
        verify_contract_manifest(contract_index, [info])

        original_source = path.read_text(encoding="utf-8")

        def mutate_during_checkpoint_verify(*args, **kwargs):
            checked = verify_contribution_checkpoint(*args, **kwargs)
            path.write_text(original_source + " ", encoding="utf-8")
            return checked

        with patch(
            "join_a64_input_materializations.verify_contribution_checkpoint",
            side_effect=mutate_during_checkpoint_verify,
        ):
            expect_failure(
                lambda: reusable_contribution_checkpoint(
                    path,
                    checkpoint_directory,
                    model_digest=info.join_model_digest,
                    guest_page_size=4096,
                ),
                "sidecar mutation during checkpoint verification was accepted",
            )
        path.write_text(original_source, encoding="utf-8")

        index_path = Path(temporary) / "joined.tsv"
        write_tsv(index_path, joined)
        verify_index_tsv(index_path, joined)
        index_path.write_text("corrupt\n", encoding="utf-8")
        write_tsv(index_path, joined)
        verify_index_tsv(index_path, joined)

        contract_index.write_bytes(b"corrupt")
        write_contract_manifest(contract_index, [info])
        verify_contract_manifest(contract_index, [info])

        truncated = Path(temporary) / "truncated-checkpoint.jsonl.gz"
        payload = checkpoint.read_bytes()
        truncated.write_bytes(payload[:-11])
        expect_failure(
            lambda: verify_contribution_checkpoint(truncated),
            "truncated contribution checkpoint was accepted",
        )
        corrupt = Path(temporary) / "corrupt-checkpoint.jsonl.gz"
        changed = bytearray(payload)
        changed[len(changed) // 2] ^= 0x40
        corrupt.write_bytes(changed)
        expect_failure(
            lambda: verify_contribution_checkpoint(corrupt),
            "bit-corrupt contribution checkpoint was accepted",
        )
        wrong_header = {
            "schema": CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
            "producer": info.producer,
            "source_name": "cross-swapped.inputs.jsonl.gz",
            "source_sha256": "f" * 64,
            "source_size": str(info.source_size),
            "join_model_digest": info.join_model_digest,
            "guest_page_size": str(info.guest_page_size),
        }
        expect_failure(
            lambda: verify_contribution_checkpoint(
                checkpoint, expected_header=wrong_header
            ),
            "cross-swapped contribution checkpoint binding was accepted",
        )
        missing = Path(temporary) / "removed-checkpoint.jsonl.gz"
        expect_failure(
            lambda: verify_contribution_checkpoint(missing),
            "removed contribution checkpoint was accepted",
        )

        checkpoint.write_bytes(payload[:-11])
        resumed = join_materializations(
            [path],
            fixture_aggregates,
            required,
            decodes,
            registry,
            guest_page_size=4096,
            allow_partial=True,
            contract_index_output=contract_index,
            checkpoint_directory=checkpoint_directory,
        )
        require(resumed == joined, "corrupt checkpoint recovery changed join")
        verify_contribution_checkpoint(checkpoint)
        with patch(
            "join_a64_input_materializations.shutil.disk_usage",
            return_value=SimpleNamespace(free=0),
        ):
            low_space_resume = join_materializations(
                [path],
                fixture_aggregates,
                required,
                decodes,
                registry,
                guest_page_size=4096,
                allow_partial=True,
                contract_index_output=contract_index,
                checkpoint_directory=checkpoint_directory,
                free_space_reserve_bytes=1,
            )
        require(
            low_space_resume == joined,
            "verified checkpoint reuse incorrectly required rebuild headroom",
        )
        stale_checkpoint = (
            checkpoint_directory / "stale.contributions.jsonl.gz"
        )
        stale_checkpoint.write_bytes(b"stale")
        pruned_resume = join_materializations(
            [path],
            fixture_aggregates,
            required,
            decodes,
            registry,
            guest_page_size=4096,
            allow_partial=True,
            contract_index_output=contract_index,
            checkpoint_directory=checkpoint_directory,
        )
        require(
            pruned_resume == joined and not stale_checkpoint.exists(),
            "stale resumable checkpoint storage was not pruned",
        )
        checkpoint.write_bytes(b"corrupt")
        with patch(
            "join_a64_input_materializations.shutil.disk_usage",
            return_value=SimpleNamespace(free=0),
        ):
            expect_failure(
                lambda: join_materializations(
                    [path],
                    fixture_aggregates,
                    required,
                    decodes,
                    registry,
                    guest_page_size=4096,
                    allow_partial=True,
                    contract_index_output=contract_index,
                    checkpoint_directory=checkpoint_directory,
                    free_space_reserve_bytes=1,
                ),
                "corrupt checkpoint rebuild bypassed free-space reserve",
            )

        corrupted = [dict(row) for row in fixture_aggregates]
        corrupted[template_index]["declared_digest"] = "0" * 64
        expect_failure(
            lambda: join_materializations(
                [path],
                corrupted,
                required,
                decodes,
                registry,
                guest_page_size=4096,
                allow_partial=True,
            ),
            "aggregate declared-digest corruption was accepted",
        )
        expect_failure(
            lambda: join_materializations(
                [path],
                fixture_aggregates,
                required,
                decodes,
                registry,
                guest_page_size=4096,
                allow_partial=False,
            ),
            "incomplete profile coverage passed complete mode",
        )


def absent_producer_fixture() -> None:
    expect_failure(
        lambda: materialization_paths([], []),
        "a join with no producer sidecars was accepted",
    )
    require_complete_producer_coverage(
        {"producer-a", "producer-b"}, {"producer-a", "producer-b"}
    )
    expect_failure(
        lambda: require_complete_producer_coverage(
            {"producer-a", "producer-b"}, {"producer-a"}
        ),
        "a complete join with an absent producer was accepted",
    )
    for name, plain_payload, gzip_payload, truncate in (
        ("matching", b"same\n", b"same\n", False),
        ("mismatched", b"plain\n", b"gzip\n", False),
        ("interrupted", b"same\n", b"same\n", True),
    ):
        with tempfile.TemporaryDirectory(
            prefix=f"a64-duplicate-sidecar-{name}-"
        ) as temporary_name:
            directory = Path(temporary_name).resolve()
            plain = directory / "producer.tsv.inputs.jsonl"
            compressed = directory / "producer.tsv.inputs.jsonl.gz"
            plain.write_bytes(plain_payload)
            with gzip.open(compressed, "wb") as output:
                output.write(gzip_payload)
            if truncate:
                compressed.write_bytes(compressed.read_bytes()[:-4])
            expect_failure(
                lambda: materialization_paths((directory,), ()),
                f"{name} duplicate plain/gzip sidecars were accepted",
            )


def high_cardinality_spool_fixture() -> None:
    raw = raw_contract_tsv(materialization())
    variants: list[dict[str, str]] = []
    for case in ("a", "aa", "a\0", "b"):
        changed = dict(raw)
        changed["case"] = case
        variants.append(changed)
    changed = dict(raw)
    changed["line"] = "18"
    variants.append(changed)
    require(
        sorted(variants, key=evidence_sort_key)
        == sorted(variants, key=evidence_sort_key_blob),
        "external sort-key encoding differs from native tuple order",
    )

    with tempfile.TemporaryDirectory(
        prefix="a64-spool-root-"
    ) as spool_root_name, ContributionSpool(
        Path(spool_root_name),
        maximum_bytes=20000 * 1024 + SPOOL_METADATA_ALLOWANCE_BYTES,
    ) as spool:
        require(
            spool.path.is_relative_to(Path(spool_root_name)),
            "contribution spool escaped its checked storage root",
        )
        for index in range(20000):
            changed = dict(raw)
            changed["case"] = f"case-{index:08d}"
            spool.add(
                "aggregate-fixture",
                changed,
                f"raw-{index:08d}",
                f"input-{index:08d}",
                (),
            )
        require(
            not any(
                isinstance(value, (dict, list, set, tuple))
                for value in vars(spool).values()
            ),
            "contribution spool retained an in-memory high-cardinality container",
        )
        spool.finish()
        for sql, parameters in (
            (
                "SELECT DISTINCT aggregate_id FROM contribution",
                (),
            ),
            (
                "SELECT sort_key, declaration, occurrences, raw_id, input_id, "
                "obligations FROM contribution WHERE aggregate_id = ? "
                "ORDER BY sort_key, raw_id",
                ("aggregate-fixture",),
            ),
            (
                "SELECT aggregate_id, sort_key, declaration, occurrences, "
                "raw_id, input_id, obligations FROM contribution "
                "ORDER BY sort_key, aggregate_id, raw_id",
                (),
            ),
        ):
            plan = spool.query_plan(sql, parameters)
            require(
                not any("TEMP B-TREE" in step for step in plan),
                f"rooted spool query escaped to a temporary sort: {plan}",
            )
        database_paths = [
            Path(str(row[2]))
            for row in spool.database.execute("PRAGMA database_list")
            if str(row[1]) == "main"
        ]
        require(
            [path.resolve() for path in database_paths] == [spool.path.resolve()]
            and spool.path.resolve().is_relative_to(
                Path(spool_root_name).resolve()
            ),
            "contribution database is not rooted on the checked volume",
        )
        count = spool.database.execute(
            "SELECT count(*) FROM contribution"
        ).fetchone()
        require(count == (20000,), "disk-backed contribution count drifted")
        summary = spool.summary("aggregate-fixture")
        require(
            summary[0] == 20000
            and summary[3] == 20000
            and summary[5] == 20000,
            "high-cardinality compact identity counts drifted",
        )
        require(
            len(summary[4]) == 64 and len(summary[6]) == 64,
            "high-cardinality identity digests are not compact",
        )
        with tempfile.TemporaryDirectory(
            prefix="a64-high-cardinality-checkpoint-"
        ) as temporary:
            checkpoint = Path(temporary) / "contributions.jsonl.gz"
            header = {
                "schema": CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
                "producer": raw["ctest"],
                "source_name": "high-cardinality.inputs.jsonl.gz",
                "source_sha256": "0" * 64,
                "source_size": "1",
                "join_model_digest": "1" * 64,
                "guest_page_size": raw["guest_page_size"],
            }
            too_small = Path(temporary) / "too-small.jsonl.gz"
            expect_failure(
                lambda: write_contribution_checkpoint(
                    too_small, header, spool, maximum_bytes=64
                ),
                "checkpoint hard byte cap was not enforced",
            )
            require(
                not too_small.exists(),
                "failed checkpoint cap left a published artifact",
            )
            write_contribution_checkpoint(
                checkpoint,
                header,
                spool,
                maximum_bytes=(
                    20000 * 128 + CHECKPOINT_METADATA_ALLOWANCE_BYTES
                ),
            )
            info = verify_contribution_checkpoint(
                checkpoint, expected_header=header
            )
            require(
                info.contribution_count == 20000
                and info.checkpoint_size < 8 * 1024 * 1024,
                "high-cardinality checkpoint is not bounded/compressed",
            )

    with tempfile.TemporaryDirectory(
        prefix="a64-capped-spool-"
    ) as capped_root_name:
        capped_root = Path(capped_root_name)
        cap = 128 * 1024
        hit_cap = False
        cap_message = ""
        with ContributionSpool(capped_root, maximum_bytes=cap) as spool:
            for index in range(100000):
                changed = dict(raw)
                changed["case"] = f"bounded-{index:08d}"
                try:
                    spool.add(
                        "aggregate-fixture",
                        changed,
                        f"bounded-raw-{index:08d}",
                        f"bounded-input-{index:08d}",
                        (),
                    )
                except ValueError as error:
                    hit_cap = True
                    cap_message = str(error)
                    break
            require(hit_cap, "SQLite contribution spool hard cap was not hit")
            require(
                "hard byte cap" in cap_message,
                "a real max-page-count failure lost its cap diagnostic",
            )
            require(
                spool.maximum_bytes is not None
                and spool.path.stat().st_size <= spool.maximum_bytes <= cap,
                "SQLite contribution spool exceeded its hard byte cap",
            )

    require(
        DEFAULT_SPOOL_BYTES_PER_CONTRIBUTION == 2048,
        "the production spool default regressed below the measured profile",
    )


class FailingConnection:
    def __init__(self, path: Path, phase: str, *, full: bool = False) -> None:
        self.connection = sqlite3.connect(path)
        self.phase = phase
        self.full = full

    def failure(self) -> sqlite3.OperationalError:
        error = sqlite3.OperationalError(f"fixture {self.phase} failure")
        if self.full:
            error.sqlite_errorcode = sqlite3.SQLITE_FULL
        return error

    def execute(self, sql: str, parameters=()):
        if self.phase == "initialization" and "CREATE TABLE" in sql:
            raise self.failure()
        if self.phase == "insert" and sql.startswith("INSERT INTO"):
            raise self.failure()
        return self.connection.execute(sql, parameters)

    def commit(self) -> None:
        if self.phase == "commit":
            raise self.failure()
        self.connection.commit()

    def close(self) -> None:
        self.connection.close()


def spool_failure_fixture() -> None:
    raw = raw_contract_tsv(materialization())

    def add_one(spool: ContributionSpool) -> None:
        spool.add("aggregate-fixture", raw, "raw-fixture", "input-fixture", ())

    with tempfile.TemporaryDirectory(
        prefix="a64-spool-open-"
    ) as root_name:
        root = Path(root_name)

        def fail_open(_path: Path):
            raise sqlite3.OperationalError("fixture open failure")

        message = failure_message(
            lambda: ContributionSpool(
                root, connection_factory=fail_open
            ).__enter__()
        )
        require(
            "SQLite initialization failed" in message,
            "SQLite connection failure lost its initialization diagnostic",
        )
        require(
            list(root.iterdir()) == [],
            "SQLite connection failure left spool debris",
        )

    for phase in ("initialization", "insert", "commit"):
        with tempfile.TemporaryDirectory(
            prefix=f"a64-spool-{phase}-"
        ) as root_name:
            root = Path(root_name)

            def factory(path: Path, phase=phase):
                return FailingConnection(path, phase)

            def run() -> None:
                with ContributionSpool(
                    root, maximum_bytes=16 * 1024 * 1024,
                    connection_factory=factory,
                ) as spool:
                    add_one(spool)
                    spool.finish()

            message = failure_message(run)
            require(
                f"SQLite {phase} failed" in message
                and "hard byte cap" not in message,
                f"unrelated SQLite {phase} failure was misclassified",
            )
            require(
                list(root.iterdir()) == [],
                f"SQLite {phase} failure left spool debris",
            )

    with tempfile.TemporaryDirectory(
        prefix="a64-spool-filesystem-full-"
    ) as root_name:
        root = Path(root_name)

        def full_factory(path: Path):
            return FailingConnection(path, "insert", full=True)

        def run_full() -> None:
            with ContributionSpool(
                root, maximum_bytes=64 * 1024 * 1024,
                connection_factory=full_factory,
            ) as spool:
                add_one(spool)

        with patch(
            "join_a64_input_materializations.shutil.disk_usage",
            return_value=SimpleNamespace(free=0),
        ):
            message = failure_message(run_full)
        require(
            "backing filesystem is full" in message,
            "filesystem exhaustion was misreported as a spool cap hit",
        )
        require(
            list(root.iterdir()) == [],
            "filesystem exhaustion left spool debris",
        )


def storage_and_output_policy_fixture() -> None:
    with tempfile.TemporaryDirectory(
        prefix="a64-storage-policy-"
    ) as temporary_name:
        temporary = Path(temporary_name).resolve()
        low = temporary / "low"
        high = temporary / "high"
        low.mkdir()
        high.mkdir()
        source = temporary / "source.jsonl"
        source.write_text("fixture\n", encoding="utf-8")

        def capacity(path: Path) -> SimpleNamespace:
            free = 1 if Path(path).resolve() == low.resolve() else 10**9
            return SimpleNamespace(free=free)

        with patch(
            "join_a64_input_materializations.shutil.disk_usage",
            side_effect=capacity,
        ):
            expect_failure(
                lambda: require_checkpoint_free_space(
                    low,
                    source,
                    1,
                    reserve_bytes=0,
                    spool_bytes_per_contribution=1024,
                    checkpoint_bytes_per_contribution=128,
                ),
                "low-capacity checked spool volume was accepted",
            )
            expected_caps = producer_storage_caps(
                source,
                1,
                spool_bytes_per_contribution=1024,
                checkpoint_bytes_per_contribution=128,
            )
            require(
                require_checkpoint_free_space(
                    high,
                    source,
                    1,
                    reserve_bytes=0,
                    spool_bytes_per_contribution=1024,
                    checkpoint_bytes_per_contribution=128,
                )
                == sum(expected_caps)
                == 1152
                + SPOOL_METADATA_ALLOWANCE_BYTES
                + CHECKPOINT_METADATA_ALLOWANCE_BYTES,
                "capacity check did not use the selected spool volume",
            )

        expect_failure(
            lambda: reserve_bytes_from_gib(float("inf")),
            "infinite free-space reserve was accepted",
        )
        expect_failure(
            lambda: reserve_bytes_from_gib(float("nan")),
            "NaN free-space reserve was accepted",
        )
        expect_failure(
            lambda: reserve_bytes_from_gib(-1.0),
            "negative free-space reserve was accepted",
        )

        output = temporary / "output.tsv"
        contract = temporary / "contracts.tsv.gz"
        checkpoints = temporary / "checkpoints"
        expect_failure(
            lambda: validate_join_output_paths(
                output, output, checkpoints, (source,)
            ),
            "identical output and contract-index targets were accepted",
        )
        hardlink = temporary / "hardlink-output.tsv"
        os.link(source, hardlink)
        expect_failure(
            lambda: validate_join_output_paths(
                hardlink, contract, checkpoints, (source,)
            ),
            "hardlinked output/read-input alias was accepted",
        )
        expect_failure(
            lambda: validate_join_output_paths(
                output, contract, source, (source,)
            ),
            "checkpoint/read-input alias was accepted",
        )
        expect_failure(
            lambda: validate_join_output_paths(
                temporary / "result",
                contract,
                temporary / "result/checkpoints",
                (source,),
            ),
            "file-output/checkpoint ancestry was accepted",
        )
        expect_failure(
            lambda: validate_join_output_paths(
                temporary / "contract/index.tsv",
                temporary / "contract",
                checkpoints,
                (source,),
            ),
            "contract-index/output ancestry was accepted",
        )
        expect_failure(
            lambda: validate_join_output_paths(
                source / "output.tsv", contract, checkpoints, (source,)
            ),
            "output nested beneath a read file was accepted",
        )
        if not filesystem_is_case_sensitive(temporary):
            expect_failure(
                lambda: validate_join_output_paths(
                    temporary / "Result.tsv",
                    temporary / "result.tsv",
                    checkpoints,
                    (source,),
                ),
                "case-variant output aliases were accepted",
            )


def main() -> int:
    projection_mutation_fixture()
    program_memory_negative_fixture()
    feature_engine_schema_negative_fixture()
    raw_projection_fixture()
    duplicate_and_raw_instance_fixture()
    compact_stream_negative_fixture()
    large_repeated_stream_fixture()
    obligation_fixture()
    inventory_fixture()
    callback_registry_fixture()
    aggregate_manifest_validation_fixture()
    aggregate_join_fixture()
    absent_producer_fixture()
    high_cardinality_spool_fixture()
    spool_failure_fixture()
    storage_and_output_policy_fixture()
    print("A64 concrete input materialization schema/streaming negatives passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
