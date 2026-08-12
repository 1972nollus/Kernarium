#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate and identify concrete A64 harness input materializations.

The native recorder emits one canonical JSON object for every raw evidence
contract.  This module independently verifies the complete CpuState, program,
memory, terminal, and evidence-digest projections.  Stable official aggregate
identities are added only by the separate all-aggregate join; native selector
representatives are deliberately not part of this format.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import gzip
import hashlib
import json
import re
import zlib
from collections import defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO

from a64_aslref_state_digest import (
    architecture_state_digest,
    memory_regions_digest,
)
from aggregate_a64_raw_instruction_evidence import RAW_COLUMNS


RAW_SCHEMA = "uml.a64.input-materialization/v4"
PROGRAM_BLOB_SCHEMA = "uml.a64.program-blob/v2"
STATE_BLOB_SCHEMA = "uml.a64.state-blob/v2"
MEMORY_BLOB_SCHEMA = "uml.a64.memory-blob/v2"
CONTRACT_SCHEMA = "uml.a64.input-contract/v4"
AGGREGATE_SCHEMA = "uml.a64.aggregate-input-materialization/v4"
PROJECTION_INVENTORY_SCHEMA = "uml.a64.cpustate-projection-inventory/v4"

HEX2 = re.compile(r"[0-9a-f]{2}\Z")
HEX4 = re.compile(r"[0-9a-f]{4}\Z")
HEX8 = re.compile(r"[0-9a-f]{8}\Z")
HEX16 = re.compile(r"[0-9a-f]{16}\Z")
HEX32 = re.compile(r"[0-9a-f]{32}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
TOKEN = re.compile(r"[a-z0-9][a-z0-9_]*\Z")
CALLBACK_DESCRIPTOR = re.compile(
    r"[a-z0-9]+(?:-[a-z0-9]+)*/v[1-9][0-9]*\Z"
)
CALLBACK_FIXTURE = re.compile(r"(tests/[A-Za-z0-9_./+-]+):([A-Za-z_][A-Za-z0-9_]*)\Z")
CALLBACK_REGISTRY_COLUMNS = [
    "descriptor_id",
    "kind",
    "allowed_result_constraint",
    "independent_fixture",
]
CALLBACK_KINDS = {"validator", "before_engine", "after_engine"}
ENGINE_RESULTS = {
    "unavailable",
    "skipped-host-feature",
    "executed-not-retired",
    "executed-retired",
}

WIRE_EXCLUDED_CLASSES = (
    "deferred_fp_representation",
    "gadget_host_environment",
    "gadget_indirect_miss",
    "gadget_zero",
)
EXCLUDED_CPU_STATE_FIELDS = (
    "deferred_vectors",
    "deferred_fpcr",
    "deferred_fpsr",
    "fp_loaded",
    "fp_dirty",
    "gadget_zero",
    "gadget_host_fpcr",
    "gadget_host_fpsr",
    "gadget_fp_active",
    "gadget_indirect_miss",
    "gadget_host_fpmr",
    "gadget_fpmr_active",
)
STATE_CLASS_PATHS = {
    "general-registers": ("x", "sp", "pc"),
    "condition-flags": ("nzcv",),
    "fixed-vector-registers": ("vectors",),
    "floating-point-controls": ("fpcr", "fpsr", "fpmr"),
    "thread-register": ("tls",),
    "memory-tag-generator": ("mte",),
    "checked-arithmetic": ("checked_arithmetic",),
    "modeled-pstate": ("pstate",),
    "exclusive-monitor": ("exclusive_monitor",),
    "guarded-control-stack": ("guarded_stack",),
    "pointer-authentication": ("signing_keys", "signing_enabled"),
    "extended-vector-mode": (),
}


def projection_inventory() -> dict[str, object]:
    """Return the schema-versioned proof for included and excluded fields."""
    excluded = {
        "deferred_fp_representation": (
            "deferred_vectors",
            "deferred_fpcr",
            "deferred_fpsr",
            "fp_loaded",
            "fp_dirty",
        ),
        "gadget_host_environment": (
            "gadget_host_fpcr",
            "gadget_host_fpsr",
            "gadget_fp_active",
            "gadget_host_fpmr",
            "gadget_fpmr_active",
        ),
        "gadget_indirect_miss": ("gadget_indirect_miss",),
        "gadget_zero": ("gadget_zero",),
    }
    return {
        "schema": PROJECTION_INVENTORY_SCHEMA,
        "wire_schema": CONTRACT_SCHEMA,
        "source_type": "uml::cpu::arm64::CpuState",
        "included_classes": [
            {
                "name": name,
                "wire_paths": list(paths),
                "status": (
                    "unrepresented-by-cpustate"
                    if name == "extended-vector-mode"
                    else "lossless"
                ),
            }
            for name, paths in STATE_CLASS_PATHS.items()
        ],
        "excluded_classes": [
            {
                "wire_name": wire,
                "fields": list(fields),
                "reason": (
                    "copy-then-access_fp_state-materializes-architectural-image"
                    if wire == "deferred_fp_representation"
                    else "engine-private-not-compared-not-digested"
                ),
            }
            for wire, fields in excluded.items()
        ],
        "proof": (
            "The wire state contains every field used by both "
            "require_architecture_equal and architecture_state_digest. "
            "Deferred FP state is serialized only after access_fp_state() on "
            "a copy; the remaining exclusions are absent from both consumers."
        ),
    }


def canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def wire_json(value: object) -> str:
    """Encode a schema-ordered payload exactly as the native v4 recorder."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def sha256_json(value: object) -> str:
    return hashlib.sha256(canonical_json(value).encode()).hexdigest()


def content_id(prefix: str, payload: object) -> str:
    return prefix + hashlib.sha256(wire_json(payload).encode()).hexdigest()


def _object(value: object, where: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{where}: expected an object")
    return value


def _array(value: object, where: str) -> Sequence[object]:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes)):
        raise ValueError(f"{where}: expected an array")
    return value


def _keys(value: Mapping[str, object], expected: set[str], where: str) -> None:
    actual = set(value)
    if actual != expected:
        raise ValueError(
            f"{where}: keys differ; missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def _ordered_keys(
    value: Mapping[str, object], expected: Sequence[str], where: str
) -> None:
    """Require the native recorder's one canonical member order."""
    _keys(value, set(expected), where)
    if list(value) != list(expected):
        raise ValueError(f"{where}: object members are not in canonical order")


def _text(value: object, where: str, *, empty: bool = False) -> str:
    if not isinstance(value, str) or (not empty and not value):
        raise ValueError(f"{where}: expected a canonical string")
    return value


def _hex(value: object, pattern: re.Pattern[str], where: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ValueError(f"{where}: expected fixed-width lowercase hexadecimal")
    return value


def _bool(value: object, where: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{where}: expected a boolean")
    return value


def _tokens(value: object, where: str) -> list[str]:
    result = [
        _text(item, f"{where}[{index}]")
        for index, item in enumerate(_array(value, where))
    ]
    if result != sorted(set(result)) or any(
        TOKEN.fullmatch(item) is None for item in result
    ):
        raise ValueError(f"{where}: expected sorted unique feature tokens")
    return result


def _callback_contract(
    value: object,
    *,
    where: str,
    kind: str,
    descriptor_key: str,
    source: str,
    line: int,
) -> Mapping[str, object] | None:
    if value is None:
        return None
    contract = _object(value, where)
    _ordered_keys(contract, ("identity", descriptor_key), where)
    identity = _text(contract["identity"], f"{where}.identity")
    expected_identity = f"{kind}@{source}:{line}"
    if identity != expected_identity:
        raise ValueError(f"{where}.identity: callback call-site identity differs")
    descriptor = _text(contract[descriptor_key], f"{where}.{descriptor_key}")
    if CALLBACK_DESCRIPTOR.fullmatch(descriptor) is None:
        raise ValueError(f"{where}.{descriptor_key}: invalid versioned descriptor")
    return contract


def callback_contract_ids(
    record: Mapping[str, object],
) -> tuple[tuple[str, str], ...]:
    representability = _object(record["representability"], "representability")
    contracts = _object(
        representability["callback_contracts"],
        "representability.callback_contracts",
    )
    result: list[tuple[str, str]] = []
    for kind, descriptor_key in (
        ("validator", "relation_id"),
        ("before_engine", "effect_id"),
        ("after_engine", "effect_id"),
    ):
        value = contracts[kind]
        if value is not None:
            contract = _object(value, f"callback_contracts.{kind}")
            result.append((kind, str(contract[descriptor_key])))
    return tuple(result)


def read_callback_contract_registry(
    path: Path, repository: Path
) -> dict[str, dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != CALLBACK_REGISTRY_COLUMNS:
            raise ValueError(f"{path}: unexpected callback registry columns")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: empty callback registry")
    result: dict[str, dict[str, str]] = {}
    for line_number, row in enumerate(rows, 2):
        descriptor = row["descriptor_id"]
        if CALLBACK_DESCRIPTOR.fullmatch(descriptor) is None:
            raise ValueError(
                f"{path}:{line_number}: invalid versioned descriptor ID"
            )
        if descriptor in result:
            raise ValueError(f"{path}:{line_number}: duplicate descriptor ID")
        if row["kind"] not in CALLBACK_KINDS:
            raise ValueError(f"{path}:{line_number}: invalid callback kind")
        if not row["allowed_result_constraint"].strip():
            raise ValueError(f"{path}:{line_number}: empty result constraint")
        fixture = CALLBACK_FIXTURE.fullmatch(row["independent_fixture"])
        if fixture is None:
            raise ValueError(f"{path}:{line_number}: invalid fixture identity")
        fixture_path = repository / fixture.group(1)
        if not fixture_path.is_file():
            raise ValueError(f"{path}:{line_number}: fixture path does not exist")
        if fixture.group(2) not in fixture_path.read_text(encoding="utf-8"):
            raise ValueError(f"{path}:{line_number}: fixture symbol is absent")
        result[descriptor] = row
    return result


def validate_callback_registry_source_coverage(
    registry: Mapping[str, Mapping[str, str]], source_directory: Path
) -> None:
    field_kind = {
        "validator_relation_id": "validator",
        "before_engine_effect_id": "before_engine",
        "after_engine_effect_id": "after_engine",
    }
    assignment = re.compile(
        r"\.(validator_relation_id|before_engine_effect_id|"
        r"after_engine_effect_id)\s*=\s*\"([^\"]+)\""
    )
    used: dict[str, set[str]] = defaultdict(set)
    for path in sorted(source_directory.glob("**/*")):
        if path.suffix not in {".cpp", ".hpp"}:
            continue
        for field, descriptor in assignment.findall(
            path.read_text(encoding="utf-8")
        ):
            used[descriptor].add(field_kind[field])
    unknown = sorted(set(used) - set(registry))
    unused = sorted(set(registry) - set(used))
    wrong_kind = sorted(
        descriptor
        for descriptor in set(used) & set(registry)
        if used[descriptor] != {registry[descriptor]["kind"]}
    )
    if unknown or unused or wrong_kind:
        raise ValueError(
            "callback registry/source coverage differs: "
            f"unknown={unknown}, unused={unused}, wrong_kind={wrong_kind}"
        )


def validate_callback_contract_registry_use(
    record: Mapping[str, object],
    registry: Mapping[str, Mapping[str, str]],
) -> None:
    for kind, descriptor in callback_contract_ids(record):
        row = registry.get(descriptor)
        if row is None:
            raise ValueError(f"callback contract uses unknown {descriptor}")
        if row["kind"] != kind:
            raise ValueError(
                f"callback contract {descriptor} has kind {kind}, "
                f"registry declares {row['kind']}"
            )


STATE_KEY_ORDER = (
    "x",
    "sp",
    "pc",
    "nzcv",
    "vectors",
    "fpcr",
    "fpsr",
    "fpmr",
    "tls",
    "mte",
    "checked_arithmetic",
    "pstate",
    "exclusive_monitor",
    "guarded_stack",
    "signing_keys",
    "signing_enabled",
)
STATE_KEYS = set(STATE_KEY_ORDER)


def validate_state(value: object, where: str = "state") -> Mapping[str, object]:
    state = _object(value, where)
    _ordered_keys(state, STATE_KEY_ORDER, where)
    x = _array(state["x"], f"{where}.x")
    if len(x) != 31:
        raise ValueError(f"{where}.x: expected 31 registers")
    for index, item in enumerate(x):
        _hex(item, HEX16, f"{where}.x[{index}]")
    for name in ("sp", "pc", "tls", "fpmr"):
        _hex(state[name], HEX16, f"{where}.{name}")
    for name in ("nzcv", "fpcr", "fpsr"):
        _hex(state[name], HEX8, f"{where}.{name}")
    vectors = _array(state["vectors"], f"{where}.vectors")
    if len(vectors) != 32:
        raise ValueError(f"{where}.vectors: expected 32 registers")
    for index, item in enumerate(vectors):
        _hex(item, HEX32, f"{where}.vectors[{index}]")

    mte = _object(state["mte"], f"{where}.mte")
    _ordered_keys(
        mte,
        (
            "enabled",
            "exclude",
            "tag_choices",
            "random_tags",
            "seed",
            "tag",
            "random_state",
        ),
        f"{where}.mte",
    )
    _bool(mte["enabled"], f"{where}.mte.enabled")
    _hex(mte["exclude"], HEX4, f"{where}.mte.exclude")
    choices = _text(mte["tag_choices"], f"{where}.mte.tag_choices")
    if re.fullmatch(r"[0-9a-f]{512}", choices) is None:
        raise ValueError(f"{where}.mte.tag_choices: expected 256 bytes")
    _bool(mte["random_tags"], f"{where}.mte.random_tags")
    _hex(mte["seed"], HEX4, f"{where}.mte.seed")
    _hex(mte["tag"], HEX2, f"{where}.mte.tag")
    _hex(mte["random_state"], HEX16, f"{where}.mte.random_state")

    checked = _object(state["checked_arithmetic"], f"{where}.checked_arithmetic")
    _ordered_keys(
        checked,
        ("add_enabled", "multiply_enabled", "accdata"),
        f"{where}.checked_arithmetic",
    )
    _bool(checked["add_enabled"], f"{where}.checked_arithmetic.add_enabled")
    _bool(
        checked["multiply_enabled"],
        f"{where}.checked_arithmetic.multiply_enabled",
    )
    _hex(checked["accdata"], HEX8, f"{where}.checked_arithmetic.accdata")

    pstate = _object(state["pstate"], f"{where}.pstate")
    _ordered_keys(
        pstate,
        ("ssbs", "dit", "tco", "sm", "za", "daif_access", "daif"),
        f"{where}.pstate",
    )
    for name in ("ssbs", "dit", "tco", "sm", "za", "daif_access"):
        _bool(pstate[name], f"{where}.pstate.{name}")
    _hex(pstate["daif"], HEX2, f"{where}.pstate.daif")

    exclusive = _object(state["exclusive_monitor"], f"{where}.exclusive_monitor")
    _ordered_keys(
        exclusive,
        ("address", "key", "value", "value2", "version", "size"),
        f"{where}.exclusive_monitor",
    )
    for name in ("address", "key", "value", "value2", "version"):
        _hex(exclusive[name], HEX16, f"{where}.exclusive_monitor.{name}")
    _hex(exclusive["size"], HEX2, f"{where}.exclusive_monitor.size")

    guarded = _object(state["guarded_stack"], f"{where}.guarded_stack")
    _ordered_keys(
        guarded,
        ("pointer", "implemented", "push_enabled", "store_enabled"),
        f"{where}.guarded_stack",
    )
    _hex(guarded["pointer"], HEX16, f"{where}.guarded_stack.pointer")
    for name in ("implemented", "push_enabled", "store_enabled"):
        _bool(guarded[name], f"{where}.guarded_stack.{name}")

    signing = _array(state["signing_keys"], f"{where}.signing_keys")
    if len(signing) != 5:
        raise ValueError(f"{where}.signing_keys: expected five keys")
    for index, item in enumerate(signing):
        key = _object(item, f"{where}.signing_keys[{index}]")
        _ordered_keys(key, ("low", "high"), f"{where}.signing_keys[{index}]")
        _hex(key["low"], HEX16, f"{where}.signing_keys[{index}].low")
        _hex(key["high"], HEX16, f"{where}.signing_keys[{index}].high")
    _bool(state["signing_enabled"], f"{where}.signing_enabled")
    return state


def state_digest(value: Mapping[str, object]) -> str:
    state = validate_state(value)
    mte = _object(state["mte"], "state.mte")
    checked = _object(state["checked_arithmetic"], "state.checked_arithmetic")
    pstate = _object(state["pstate"], "state.pstate")
    exclusive = _object(state["exclusive_monitor"], "state.exclusive_monitor")
    guarded = _object(state["guarded_stack"], "state.guarded_stack")
    vector_bytes = [bytes.fromhex(str(item)) for item in state["vectors"]]  # type: ignore[index]
    return architecture_state_digest(
        [int(str(item), 16) for item in state["x"]],  # type: ignore[index]
        sp=int(str(state["sp"]), 16),
        pc=int(str(state["pc"]), 16),
        nzcv=int(str(state["nzcv"]), 16) >> 28,
        nzcv_raw=int(str(state["nzcv"]), 16),
        vectors=[int.from_bytes(item, "little") for item in vector_bytes],
        fpcr=int(str(state["fpcr"]), 16),
        fpsr=int(str(state["fpsr"]), 16),
        fpmr=int(str(state["fpmr"]), 16),
        tls=int(str(state["tls"]), 16),
        mte_enabled=bool(mte["enabled"]),
        mte_exclude=int(str(mte["exclude"]), 16),
        mte_tag_choices=bytes.fromhex(str(mte["tag_choices"])),
        mte_random_tags=bool(mte["random_tags"]),
        mte_seed=int(str(mte["seed"]), 16),
        mte_tag=int(str(mte["tag"]), 16),
        mte_random_state=int(str(mte["random_state"]), 16),
        cpa_enabled=bool(checked["add_enabled"]),
        cpa_multiply_enabled=bool(checked["multiply_enabled"]),
        accdata=int(str(checked["accdata"]), 16),
        pstate_ssbs=bool(pstate["ssbs"]),
        pstate_dit=bool(pstate["dit"]),
        pstate_tco=bool(pstate["tco"]),
        pstate_sm=bool(pstate["sm"]),
        pstate_za=bool(pstate["za"]),
        pstate_daif_access=bool(pstate["daif_access"]),
        pstate_daif=int(str(pstate["daif"]), 16),
        exclusive_address=int(str(exclusive["address"]), 16),
        exclusive_key=int(str(exclusive["key"]), 16),
        exclusive_value=int(str(exclusive["value"]), 16),
        exclusive_value2=int(str(exclusive["value2"]), 16),
        exclusive_version=int(str(exclusive["version"]), 16),
        exclusive_size=int(str(exclusive["size"]), 16),
        gcs_pointer=int(str(guarded["pointer"]), 16),
        gcs_implemented=bool(guarded["implemented"]),
        gcs_push_enabled=bool(guarded["push_enabled"]),
        gcs_store_enabled=bool(guarded["store_enabled"]),
        pointer_auth_keys=[
            (int(str(item["low"]), 16), int(str(item["high"]), 16))
            for item in state["signing_keys"]  # type: ignore[index]
        ],
        pointer_authentication_enabled=bool(state["signing_enabled"]),
    )


PERMISSION_KEY_ORDER = (
    "readable",
    "writable",
    "executable",
    "guest_code",
    "guest_writable",
)
PERMISSION_KEYS = set(PERMISSION_KEY_ORDER)
REGION_KEY_ORDER = (
    "address",
    "size",
    "initial_bytes",
    "expected_bytes",
    "initial_tags",
    "expected_tags",
    "permissions",
)
REGION_KEYS = set(REGION_KEY_ORDER)


def _permissions(value: object, where: str) -> Mapping[str, object]:
    result = _object(value, where)
    _ordered_keys(result, PERMISSION_KEY_ORDER, where)
    for name in PERMISSION_KEY_ORDER:
        _bool(result[name], f"{where}.{name}")
    return result


def validate_region(value: object, where: str) -> Mapping[str, object]:
    region = _object(value, where)
    _ordered_keys(region, REGION_KEY_ORDER, where)
    address = int(_hex(region["address"], HEX16, f"{where}.address"), 16)
    size = int(_hex(region["size"], HEX16, f"{where}.size"), 16)
    if size == 0 or address + size > 1 << 64:
        raise ValueError(f"{where}: empty or wrapping range")
    for name in ("initial_bytes", "expected_bytes"):
        data = _text(region[name], f"{where}.{name}")
        if len(data) != size * 2 or re.fullmatch(r"[0-9a-f]+", data) is None:
            raise ValueError(f"{where}.{name}: byte count mismatch")
    tag_count = (size + 15) // 16
    for name in ("initial_tags", "expected_tags"):
        tags = _text(region[name], f"{where}.{name}", empty=tag_count == 0)
        if len(tags) != tag_count * 2 or re.fullmatch(r"[0-9a-f]*", tags) is None:
            raise ValueError(f"{where}.{name}: tag count mismatch")
        if any(tag > 15 for tag in bytes.fromhex(tags)):
            raise ValueError(f"{where}.{name}: tag is outside bits(4)")
    _permissions(region["permissions"], f"{where}.permissions")
    return region


def regions_digest(values: Sequence[object], expected: bool) -> str:
    projected: list[dict[str, object]] = []
    for index, value in enumerate(values):
        region = validate_region(value, f"memory_regions[{index}]")
        permissions = _object(region["permissions"], "permissions")
        projected.append(
            {
                "address": int(str(region["address"]), 16),
                "memory": bytes.fromhex(
                    str(
                        region["expected_bytes"]
                        if expected
                        else region["initial_bytes"]
                    )
                ),
                "tags": list(
                    bytes.fromhex(
                        str(
                            region["expected_tags"]
                            if expected
                            else region["initial_tags"]
                        )
                    )
                ),
                **{name: permissions[name] for name in PERMISSION_KEY_ORDER},
            }
        )
    return memory_regions_digest(projected)


def validate_program(value: object) -> Mapping[str, object]:
    program = _object(value, "program")
    _ordered_keys(
        program,
        (
            "guest_page_size",
            "code_base",
            "entry_offset",
            "page_encoding",
            "words",
            "reconstructed_page_sha256",
            "permissions",
        ),
        "program",
    )
    page_size = int(
        _hex(program["guest_page_size"], HEX8, "program.guest_page_size"), 16
    )
    if page_size < 4096 or page_size & (page_size - 1):
        raise ValueError("program.guest_page_size: invalid page size")
    _hex(program["code_base"], HEX16, "program.code_base")
    entry = int(_hex(program["entry_offset"], HEX16, "program.entry_offset"), 16)
    if entry % 4 or entry > page_size - 4:
        raise ValueError("program.entry_offset: invalid offset")
    if program["page_encoding"] != "zero-fill-sparse-words/v1":
        raise ValueError("program.page_encoding: unsupported sparse encoding")
    words = _array(program["words"], "program.words")
    if not words:
        raise ValueError("program.words: empty program")
    image = bytearray(page_size)
    offsets: list[int] = []
    for index, item in enumerate(words):
        word = _object(item, f"program.words[{index}]")
        _ordered_keys(word, ("offset", "word"), f"program.words[{index}]")
        offset = int(
            _hex(word["offset"], HEX16, f"program.words[{index}].offset"), 16
        )
        encoding = int(
            _hex(word["word"], HEX8, f"program.words[{index}].word"), 16
        )
        if offset % 4 or offset > page_size - 4:
            raise ValueError(f"program.words[{index}]: invalid offset")
        if offsets and offset <= offsets[-1]:
            raise ValueError(
                f"program.words[{index}]: offsets are not strictly increasing"
            )
        offsets.append(offset)
        image[offset : offset + 4] = encoding.to_bytes(4, "little")
    if entry not in offsets:
        raise ValueError("program.entry_offset: entry word absent")
    expected_page_digest = _hex(
        program["reconstructed_page_sha256"],
        HEX64,
        "program.reconstructed_page_sha256",
    )
    if hashlib.sha256(image).hexdigest() != expected_page_digest:
        raise ValueError(
            "program.reconstructed_page_sha256: sparse page digest mismatch"
        )
    permissions = _permissions(program["permissions"], "program.permissions")
    if permissions != {
        "readable": True,
        "writable": False,
        "executable": True,
        "guest_code": True,
        "guest_writable": False,
    }:
        raise ValueError("program.permissions: not the harness RX mapping")
    return program


def raw_contract_tsv(record: Mapping[str, object]) -> dict[str, str]:
    """Reconstruct the exact RAW_COLUMNS row consumed by the aggregator."""
    identity = _object(record["identity"], "identity")
    program = _object(record["program"], "program")
    terminal = _object(record["terminal"], "terminal")
    features = _object(record["feature_profile"], "feature_profile")
    engines = _object(record["engine_results"], "engine_results")
    integrity = _object(record["integrity"], "integrity")
    required = _tokens(features["native_required"], "feature_profile.native_required")
    result = {
        "ctest": str(identity["ctest"]),
        "case": str(identity["case"]),
        "source": str(identity["source"]),
        "line": str(int(str(identity["line"]), 16)),
        "encoding": str(identity["evidence_word"]),
        "invalid": "1" if terminal["invalid_encoding"] else "0",
        "guest_page_size": str(int(str(program["guest_page_size"]), 16)),
        "expected_event": str(terminal["event"]),
        "expected_retirement": str(terminal["retirement"]),
        "expected_state_oracle": str(terminal["state_oracle"]),
        "initial_state_digest": str(integrity["initial_state_digest"]),
        "initial_memory_digest": str(integrity["initial_memory_digest"]),
        "expected_state_digest": str(integrity["expected_state_digest"]),
        "expected_memory_digest": str(integrity["expected_memory_digest"]),
        "expected_fault": str(terminal["fault"]),
        "expected_syndrome": str(terminal["syndrome"]),
        "native_required_features": "+".join(required) or "baseline",
        "native_requirement_scope": str(identity["scope"]),
        "native_observed_features": str(features["native_observed"]),
        "native_missing_features": str(features["native_missing"]),
        "interpreter_result": str(engines["interpreter"]),
        "gadgets_result": str(engines["gadgets"]),
        "jit_result": str(engines["jit"]),
        "occurrences": str(int(str(record["raw_occurrences"]), 16)),
    }
    if list(result) != RAW_COLUMNS:
        raise AssertionError("raw contract projection drifted from RAW_COLUMNS")
    return result


ROOT_KEYS = {
    "schema",
    "input_contract_id",
    "identity",
    "program_id",
    "program",
    "initial_state_id",
    "initial_state",
    "expected_state_id",
    "expected_state",
    "memory_id",
    "memory_regions",
    "feature_profile",
    "terminal",
    "engine_results",
    "representability",
    "integrity",
    "raw_occurrences",
}


def contract_payload(record: Mapping[str, object]) -> dict[str, object]:
    """Project an expanded record back to the exact native contract payload."""
    return {
        "identity": record["identity"],
        "program_id": record["program_id"],
        "initial_state_id": record["initial_state_id"],
        "expected_state_id": record["expected_state_id"],
        "memory_id": record["memory_id"],
        "feature_profile": record["feature_profile"],
        "terminal": record["terminal"],
        "engine_results": record["engine_results"],
        "representability": record["representability"],
        "integrity": record["integrity"],
    }


def validate_raw_materialization(value: object) -> Mapping[str, object]:
    record = _object(value, "record")
    _keys(record, ROOT_KEYS, "record")
    if record["schema"] != RAW_SCHEMA:
        raise ValueError("record.schema: unsupported materialization schema")
    identity = _object(record["identity"], "identity")
    _ordered_keys(
        identity,
        ("ctest", "case", "source", "line", "scope", "evidence_word"),
        "identity",
    )
    for name in ("ctest", "case", "source"):
        _text(identity[name], f"identity.{name}")
    _hex(identity["line"], HEX8, "identity.line")
    if int(str(identity["line"]), 16) == 0:
        raise ValueError("identity.line: zero source line")
    if identity["scope"] not in {"instruction", "program"}:
        raise ValueError("identity.scope: invalid scope")
    evidence_word = _hex(identity["evidence_word"], HEX8, "identity.evidence_word")

    program_id = _text(record["program_id"], "program_id")
    if program_id != content_id("program-", record["program"]):
        raise ValueError("program_id: concrete program payload differs")
    initial_state_id = _text(record["initial_state_id"], "initial_state_id")
    if initial_state_id != content_id("state-", record["initial_state"]):
        raise ValueError("initial_state_id: concrete state payload differs")
    expected_state_id = _text(record["expected_state_id"], "expected_state_id")
    if expected_state_id != content_id("state-", record["expected_state"]):
        raise ValueError("expected_state_id: concrete state payload differs")
    memory_id = _text(record["memory_id"], "memory_id")
    if memory_id != content_id("memory-", record["memory_regions"]):
        raise ValueError("memory_id: concrete memory payload differs")
    contract_id = _text(record["input_contract_id"], "input_contract_id")
    if contract_id != content_id("input-contract-", contract_payload(record)):
        raise ValueError("input_contract_id: compact contract payload differs")

    program = validate_program(record["program"])
    if evidence_word not in {
        str(_object(item, "word")["word"])
        for item in _array(program["words"], "program.words")
    }:
        raise ValueError("identity.evidence_word: absent from program words")
    features = _object(record["feature_profile"], "feature_profile")
    _ordered_keys(
        features,
        ("native_required", "native_observed", "native_missing"),
        "feature_profile",
    )
    required_features = _tokens(
        features["native_required"], "feature_profile.native_required"
    )
    observed = _text(features["native_observed"], "feature_profile.native_observed")
    missing = _text(features["native_missing"], "feature_profile.native_missing")
    if not required_features:
        if observed != "baseline=1" or missing != "none":
            raise ValueError(
                "feature_profile: baseline observation/missing fields disagree"
            )
    else:
        observation_parts = observed.split("+")
        if len(observation_parts) != len(required_features):
            raise ValueError("feature_profile.native_observed: feature count differs")
        observed_values: list[bool] = []
        for required, part in zip(required_features, observation_parts):
            if part not in {f"{required}=0", f"{required}=1"}:
                raise ValueError(
                    "feature_profile.native_observed: required feature/order differs"
                )
            observed_values.append(part.endswith("=1"))
        expected_missing = "+".join(
            required
            for required, is_observed in zip(required_features, observed_values)
            if not is_observed
        ) or "none"
        if missing != expected_missing:
            raise ValueError(
                "feature_profile.native_missing: disagrees with native_observed"
            )

    initial = validate_state(record["initial_state"], "initial_state")
    expected = validate_state(record["expected_state"], "expected_state")
    memory = _array(record["memory_regions"], "memory_regions")
    ranges: list[tuple[int, int]] = []
    code_base = int(str(program["code_base"]), 16)
    page_size = int(str(program["guest_page_size"]), 16)
    ranges.append((code_base, page_size))
    for index, item in enumerate(memory):
        region = validate_region(item, f"memory_regions[{index}]")
        ranges.append(
            (int(str(region["address"]), 16), int(str(region["size"]), 16))
        )
    ordered = sorted(ranges)
    for (left, size), (right, _right_size) in zip(ordered, ordered[1:]):
        if left + size > right:
            raise ValueError("memory_regions: mapped ranges overlap")

    terminal = _object(record["terminal"], "terminal")
    _ordered_keys(
        terminal,
        (
            "event",
            "retirement",
            "instruction_budget",
            "state_oracle",
            "fault",
            "syndrome",
            "invalid_encoding",
        ),
        "terminal",
    )
    for name in ("event", "retirement", "state_oracle", "fault"):
        _text(terminal[name], f"terminal.{name}")
    _hex(terminal["syndrome"], HEX8, "terminal.syndrome")
    budget = int(
        _hex(
            terminal["instruction_budget"],
            HEX16,
            "terminal.instruction_budget",
        ),
        16,
    )
    if budget == 0:
        raise ValueError("terminal.instruction_budget: zero")
    _bool(terminal["invalid_encoding"], "terminal.invalid_encoding")
    engines = _object(record["engine_results"], "engine_results")
    _ordered_keys(
        engines, ("interpreter", "gadgets", "jit"), "engine_results"
    )
    for name in ("interpreter", "gadgets", "jit"):
        result = _text(engines[name], f"engine_results.{name}")
        if result not in ENGINE_RESULTS:
            raise ValueError(f"engine_results.{name}: unsupported result token")

    representability = _object(record["representability"], "representability")
    _ordered_keys(
        representability,
        (
            "status",
            "obligations",
            "callback_contracts",
            "excluded_engine_private_fields",
        ),
        "representability",
    )
    obligations = [
        _text(item, f"representability.obligations[{index}]")
        for index, item in enumerate(
            _array(representability["obligations"], "representability.obligations")
        )
    ]
    if obligations != sorted(set(obligations)):
        raise ValueError("representability.obligations: not sorted unique")
    expected_status = (
        "unrepresentable"
        if obligations
        else "pending-official-model-projection"
    )
    if representability["status"] != expected_status:
        raise ValueError("representability.status: inconsistent with obligations")
    callback_contracts = _object(
        representability["callback_contracts"],
        "representability.callback_contracts",
    )
    _ordered_keys(
        callback_contracts,
        ("validator", "before_engine", "after_engine"),
        "representability.callback_contracts",
    )
    source = str(identity["source"])
    line = int(str(identity["line"]), 16)
    validator = _callback_contract(
        callback_contracts["validator"],
        where="representability.callback_contracts.validator",
        kind="custom-validator",
        descriptor_key="relation_id",
        source=source,
        line=line,
    )
    _callback_contract(
        callback_contracts["before_engine"],
        where="representability.callback_contracts.before_engine",
        kind="before-engine-hook",
        descriptor_key="effect_id",
        source=source,
        line=line,
    )
    _callback_contract(
        callback_contracts["after_engine"],
        where="representability.callback_contracts.after_engine",
        kind="after-engine-hook",
        descriptor_key="effect_id",
        source=source,
        line=line,
    )
    expected_oracle = "exact" if validator is None else str(validator["identity"])
    if terminal["state_oracle"] != expected_oracle:
        raise ValueError(
            "terminal.state_oracle: differs from validator callback identity"
        )
    excluded = tuple(
        _text(item, f"representability.excluded_engine_private_fields[{index}]")
        for index, item in enumerate(
            _array(
                representability["excluded_engine_private_fields"],
                "representability.excluded_engine_private_fields",
            )
        )
    )
    if excluded != WIRE_EXCLUDED_CLASSES:
        raise ValueError("representability: projection exclusion list drifted")

    integrity = _object(record["integrity"], "integrity")
    _ordered_keys(
        integrity,
        (
            "initial_state_digest",
            "initial_memory_digest",
            "expected_state_digest",
            "expected_memory_digest",
        ),
        "integrity",
    )
    calculated = {
        "initial_state_digest": state_digest(initial),
        "initial_memory_digest": regions_digest(memory, False),
        "expected_state_digest": state_digest(expected),
        "expected_memory_digest": regions_digest(memory, True),
    }
    for name, digest in calculated.items():
        _hex(integrity[name], HEX16, f"integrity.{name}")
        if integrity[name] != digest:
            raise ValueError(f"integrity.{name}: concrete payload digest mismatch")
    occurrences = int(_hex(record["raw_occurrences"], HEX16, "raw_occurrences"), 16)
    if occurrences == 0:
        raise ValueError("raw_occurrences: zero occurrences")
    raw_contract_tsv(record)
    return record


def raw_contract_id(record: Mapping[str, object]) -> str:
    raw = raw_contract_tsv(record)
    projection = {
        column: raw[column] for column in RAW_COLUMNS if column != "occurrences"
    }
    key = {
        "raw_projection": projection,
        "input_contract_id": materialization_id(record),
    }
    return "raw-contract-" + sha256_json(key)


def materialization_id(record: Mapping[str, object]) -> str:
    return _text(record["input_contract_id"], "input_contract_id")


def recorder_obligations(record: Mapping[str, object]) -> tuple[str, ...]:
    representability = _object(record["representability"], "representability")
    return tuple(str(item) for item in representability["obligations"])  # type: ignore[index]


def representation_obligations(
    record: Mapping[str, object],
    architecture_features: Iterable[str] = (),
) -> tuple[str, ...]:
    """Derive current ASL adapter limitations without assuming defaults."""
    validate_raw_materialization(record)
    result = set(recorder_obligations(record))
    program = _object(record["program"], "program")
    for prefix in ("initial_state", "expected_state"):
        state = _object(record[prefix], prefix)
        if int(str(state["nzcv"]), 16) & 0x0FFFFFFF:
            result.add("unrepresentable-nzcv-storage-bits")
    native = set(
        _object(record["feature_profile"], "feature_profile")["native_required"]  # type: ignore[arg-type]
    )
    official = set(architecture_features)
    if "sve" in native or any(item.startswith("FEAT_SVE") for item in official):
        result.add("unrepresentable-cpustate-sve-register-file-and-vl")
    if "sme" in native or any(item.startswith("FEAT_SME") for item in official):
        result.add("unrepresentable-cpustate-sme-streaming-za-zt0-state")
    return tuple(sorted(result))


def read_raw_materializations(paths: Iterable[Path]) -> list[Mapping[str, object]]:
    """Materialize the streaming reader; intended only for bounded fixtures."""
    return list(iter_raw_materializations(paths))


@dataclass(frozen=True)
class _RecordLocation:
    path: Path
    offset: int
    length: int
    line_number: int


@dataclass(frozen=True)
class _BlobIndexEntry:
    schema: str
    payload_length: int
    first_path: Path


@dataclass
class _ContractIndexEntry:
    references: tuple[str, str, str, str]
    occurrences: int = 1


_BLOB_SCHEMAS = {
    PROGRAM_BLOB_SCHEMA: ("program-", validate_program),
    STATE_BLOB_SCHEMA: ("state-", validate_state),
    MEMORY_BLOB_SCHEMA: ("memory-", None),
}
_CONTRACT_PAYLOAD_ORDER = (
    "identity",
    "program_id",
    "initial_state_id",
    "expected_state_id",
    "memory_id",
    "feature_profile",
    "terminal",
    "engine_results",
    "representability",
    "integrity",
)


def _identifier(value: object, prefix: str, where: str) -> str:
    identifier = _text(value, where)
    suffix = identifier.removeprefix(prefix)
    if suffix == identifier or HEX64.fullmatch(suffix) is None:
        raise ValueError(f"{where}: invalid content identifier")
    return identifier


def _validate_memory_blob(value: object, where: str) -> Sequence[object]:
    regions = _array(value, where)
    for index, region in enumerate(regions):
        validate_region(region, f"{where}[{index}]")
    return regions


def _parse_canonical_event(
    raw_line: bytes, location: _RecordLocation
) -> Mapping[str, object]:
    if not raw_line.endswith(b"\n"):
        raise ValueError(
            f"{location.path}:{location.line_number}: unterminated JSONL"
        )
    if not raw_line[:-1]:
        raise ValueError(f"{location.path}:{location.line_number}: empty JSONL record")
    try:
        value: Any = json.loads(raw_line[:-1])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(
            f"{location.path}:{location.line_number}: invalid JSON: {error}"
        ) from error
    event = _object(value, "event")
    try:
        canonical = wire_json(event).encode("utf-8")
    except (TypeError, UnicodeEncodeError) as error:
        raise ValueError(
            f"{location.path}:{location.line_number}: invalid JSON value"
        ) from error
    if canonical != raw_line[:-1]:
        raise ValueError(
            f"{location.path}:{location.line_number}: noncanonical JSONL event"
        )
    return event


def _open_input(path: Path) -> BinaryIO:
    with path.open("rb") as probe:
        gzip_magic = probe.read(2) == b"\x1f\x8b"
    if path.suffix == ".gz" or gzip_magic:
        return gzip.open(path, "rb")
    return path.open("rb")


def _iter_events(path: Path) -> Iterable[tuple[Mapping[str, object], _RecordLocation]]:
    try:
        with _open_input(path) as source:
            line_number = 0
            while True:
                offset = source.tell()
                raw_line = source.readline()
                if not raw_line:
                    break
                line_number += 1
                location = _RecordLocation(path, offset, len(raw_line), line_number)
                yield _parse_canonical_event(raw_line, location), location
    except (EOFError, gzip.BadGzipFile, OSError, zlib.error) as error:
        raise ValueError(f"{path}: corrupt or truncated compressed JSONL: {error}") from error


def iter_raw_materializations(
    paths: Iterable[Path],
) -> Iterable[Mapping[str, object]]:
    """Index compact v3 events, then reconstruct each distinct raw contract.

    The first pass retains only content IDs, references, and occurrence
    counts.  The second pass is sequential (including for gzip inputs) and
    evicts each expanded blob immediately after its final distinct contract.
    """
    path_list = sorted({Path(path).resolve() for path in paths})
    if not path_list:
        raise ValueError("no input-materialization JSONL paths")
    blobs: dict[str, _BlobIndexEntry] = {}
    contracts: dict[str, _ContractIndexEntry] = {}
    reference_counts: dict[str, int] = defaultdict(int)

    for path in path_list:
        local_blobs: set[str] = set()
        for event, location in _iter_events(path):
                line_number = location.line_number
                schema = _text(event.get("schema"), "event.schema")
                if schema in _BLOB_SCHEMAS:
                    _ordered_keys(event, ("schema", "id", "payload"), "event")
                    prefix, validator = _BLOB_SCHEMAS[schema]
                    identifier = _identifier(event["id"], prefix, "event.id")
                    payload = event["payload"]
                    if identifier != content_id(prefix, payload):
                        raise ValueError(
                            f"{path}:{line_number}: blob content ID differs"
                        )
                    if schema == MEMORY_BLOB_SCHEMA:
                        _validate_memory_blob(payload, "memory blob payload")
                    elif validator is not None:
                        validator(payload)
                    if identifier in local_blobs:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate blob {identifier}"
                        )
                    local_blobs.add(identifier)
                    payload_length = len(wire_json(payload).encode("utf-8"))
                    previous = blobs.get(identifier)
                    if previous is None:
                        blobs[identifier] = _BlobIndexEntry(
                            schema, payload_length, path
                        )
                    elif (
                        previous.schema != schema
                        or previous.payload_length != payload_length
                    ):
                        raise ValueError(
                            f"{path}:{line_number}: conflicting blob {identifier}"
                        )
                elif schema == CONTRACT_SCHEMA:
                    _ordered_keys(
                        event,
                        ("schema", "id", "payload", "raw_occurrences"),
                        "event",
                    )
                    identifier = _identifier(
                        event["id"], "input-contract-", "event.id"
                    )
                    payload = _object(event["payload"], "contract payload")
                    _ordered_keys(payload, _CONTRACT_PAYLOAD_ORDER, "contract payload")
                    for name, prefix in (
                        ("program_id", "program-"),
                        ("initial_state_id", "state-"),
                        ("expected_state_id", "state-"),
                        ("memory_id", "memory-"),
                    ):
                        _identifier(payload[name], prefix, f"contract payload.{name}")
                    references = tuple(
                        str(payload[name])
                        for name in (
                            "program_id",
                            "initial_state_id",
                            "expected_state_id",
                            "memory_id",
                        )
                    )
                    if identifier != content_id("input-contract-", payload):
                        raise ValueError(
                            f"{path}:{line_number}: contract content ID differs"
                        )
                    if event["raw_occurrences"] != "0000000000000001":
                        raise ValueError(
                            f"{path}:{line_number}: contract event occurrence is not one"
                        )
                    previous_contract = contracts.get(identifier)
                    if previous_contract is None:
                        contracts[identifier] = _ContractIndexEntry(references)
                        for blob_id in references:
                            reference_counts[blob_id] += 1
                    else:
                        if previous_contract.references != references:
                            raise ValueError(
                                f"{path}:{line_number}: conflicting contract "
                                f"references for {identifier}"
                            )
                        previous_contract.occurrences += 1
                        if previous_contract.occurrences >= 1 << 64:
                            raise ValueError(
                                f"{path}:{line_number}: contract occurrence overflow"
                            )
                else:
                    raise ValueError(
                        f"{path}:{line_number}: unsupported event schema {schema!r}"
                    )

    if not contracts:
        raise ValueError("input-materialization stream has no contracts")
    for contract_id, indexed in contracts.items():
        for blob_id, expected_schema in zip(
            indexed.references,
            (
                PROGRAM_BLOB_SCHEMA,
                STATE_BLOB_SCHEMA,
                STATE_BLOB_SCHEMA,
                MEMORY_BLOB_SCHEMA,
            ),
        ):
            blob = blobs.get(blob_id)
            if blob is None:
                raise ValueError(f"{contract_id}: missing referenced blob {blob_id}")
            if blob.schema != expected_schema:
                raise ValueError(f"{contract_id}: wrong blob schema for {blob_id}")

    seen_contracts: set[str] = set()
    live_blobs: dict[str, object] = {}
    remaining = dict(reference_counts)
    for path in path_list:
        for event, location in _iter_events(path):
            schema = str(event["schema"])
            if schema in _BLOB_SCHEMAS:
                identifier = str(event["id"])
                if remaining.get(identifier, 0) != 0:
                    live_blobs[identifier] = event["payload"]
                continue
            identifier = str(event["id"])
            if identifier in seen_contracts:
                continue
            seen_contracts.add(identifier)
            indexed = contracts[identifier]
            payload = _object(event["payload"], "contract payload")
            referenced: dict[str, object] = {}
            for field, expected_schema in (
                ("program_id", PROGRAM_BLOB_SCHEMA),
                ("initial_state_id", STATE_BLOB_SCHEMA),
                ("expected_state_id", STATE_BLOB_SCHEMA),
                ("memory_id", MEMORY_BLOB_SCHEMA),
            ):
                blob_id = str(payload[field])
                blob = live_blobs.get(blob_id)
                if blob is None:
                    raise ValueError(
                        f"{path}:{location.line_number}: referenced blob "
                        f"{blob_id} was not emitted before its contract"
                    )
                referenced[field] = blob
            record: dict[str, object] = {
                "schema": RAW_SCHEMA,
                "input_contract_id": identifier,
                "identity": payload["identity"],
                "program_id": payload["program_id"],
                "program": referenced["program_id"],
                "initial_state_id": payload["initial_state_id"],
                "initial_state": referenced["initial_state_id"],
                "expected_state_id": payload["expected_state_id"],
                "expected_state": referenced["expected_state_id"],
                "memory_id": payload["memory_id"],
                "memory_regions": referenced["memory_id"],
                "feature_profile": payload["feature_profile"],
                "terminal": payload["terminal"],
                "engine_results": payload["engine_results"],
                "representability": payload["representability"],
                "integrity": payload["integrity"],
                "raw_occurrences": f"{indexed.occurrences:016x}",
            }
            validate_raw_materialization(record)
            yield record
            for blob_id in indexed.references:
                remaining[blob_id] -= 1
                if remaining[blob_id] == 0:
                    live_blobs.pop(blob_id, None)
    if seen_contracts != set(contracts) or any(remaining.values()):
        raise ValueError("second-pass materialization reconstruction drifted")


__all__ = [
    "AGGREGATE_SCHEMA",
    "CALLBACK_REGISTRY_COLUMNS",
    "CONTRACT_SCHEMA",
    "ENGINE_RESULTS",
    "EXCLUDED_CPU_STATE_FIELDS",
    "MEMORY_BLOB_SCHEMA",
    "PROGRAM_BLOB_SCHEMA",
    "PROJECTION_INVENTORY_SCHEMA",
    "RAW_SCHEMA",
    "STATE_BLOB_SCHEMA",
    "STATE_CLASS_PATHS",
    "WIRE_EXCLUDED_CLASSES",
    "canonical_json",
    "callback_contract_ids",
    "content_id",
    "contract_payload",
    "iter_raw_materializations",
    "materialization_id",
    "projection_inventory",
    "raw_contract_id",
    "raw_contract_tsv",
    "read_raw_materializations",
    "read_callback_contract_registry",
    "recorder_obligations",
    "regions_digest",
    "representation_obligations",
    "sha256_json",
    "state_digest",
    "validate_raw_materialization",
    "validate_callback_contract_registry_use",
    "validate_callback_registry_source_coverage",
    "validate_state",
    "wire_json",
]
