#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Benchmark a bounded, content-factored batch of real A64 input contracts.

This is a capacity and adapter-correctness probe, not promotion evidence.  It
uses real v4 recorder contracts, exact official per-form instruction
procedures, and the checked state/memory/event adapters.  Full payloads are
emitted once per content ID; each contract carries only compact table indices.
Both the contract count and the generated payload-source bytes are hard caps.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import ctypes
import csv
import functools
import hashlib
import json
import math
import os
import re
import resource
import shutil
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

from a64_aslref_dependencies import (
    AslDependencyCatalog,
    DependencyStatus,
    interface_declarations,
)
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    GenerationLimits,
    run_bounded,
)
from a64_input_materialization import (
    iter_raw_materializations,
    representation_obligations,
)
from aggregate_a64_raw_instruction_evidence import (
    parse_patterns,
    select_allocations,
)
from generate_a64_aslref_corpus import load_interface_modules
from run_a64_aslref_execution_oracle import exact_function
from a64_raw_evidence_files import (
    path_is_ancestor,
    paths_alias,
    require_no_symlink_components,
)


FORMAT = "uml.a64.aslref-materialization-batch-probe/v1"
MAX_CONTRACTS = 1024
# This is deliberately adjusted only after the 64/256/1024 measurements have
# all passed.  It remains a hard guard during calibration.
MAX_ENCODED_BYTES = 128 * 1024 * 1024
MAX_RSS_KIB = 12 * 1024 * 1024
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
MAX_ARTIFACT_FILES = 4096
MAX_LOG_BYTES = 8 * 1024 * 1024
DEFAULT_MINIMUM_GENERATED_FREE_GIB = 1.0
COUNTER_FREQUENCY = 1_000_000_000
COUNTER_INITIAL = 0x1020_3040_5060_7080
COUNTER_STEP = 1
ARCH_MAX_MOPS_CPY_SIZE = 0x007F_FFFF_FFFF_FFFF
CPY_POST_RESERVE_BYTES = 64
CPY_POST_CALLER_COUNT = 2
CPY_POST_CALLER_SHA256 = "84d028b0b8fdc900ddf54d09050fd54dfd0c071eb4e18aff8a6ee377170ed9a0"
CPY_POST_ADAPTER_SHA256 = "13a97e26e46b3535024b50d56b2e65802d6810bcecba056fe4bb7d5a4cdaa0a5"
COMPACT_MAX_MEMORY_ADDRESS = 0x5100_0000
COMPACT_MAX_MEMORY_LENGTH = 64 * 1024
COMPACT_MAX_MEMORY_TAG_LENGTH = COMPACT_MAX_MEMORY_LENGTH // 16
COMPACT_MAX_MEMORY_FORMULA_MODULUS = 256
COMPACT_MAX_MEMORY_FORMULA_SEED = 0xD2
COMPACT_MAX_MEMORY_FORMULA_STEP = 0x35
COMPACT_MAX_MEMORY_COPY_START = 16_346
COMPACT_MAX_MEMORY_COPY_END = 32_987
COMPACT_MAX_MEMORY_COPY_SOURCE_DISPLACEMENT = 173
COMPACT_MAX_MEMORY_INITIAL_OVERRIDES: tuple[tuple[int, int], ...] = ()
COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES: tuple[tuple[int, int], ...] = ()
COMPACT_MAX_MEMORY_DESCRIPTOR_SHA256 = (
    "79ee530cd2943f4540f570e41ac4246955c404afd5b4506f910b8b19052f6f8e"
)
COMPACT_MAX_MEMORY_INITIAL_SHA256 = (
    "b13cb130b276d9b07af5c602cfdd321f536e80a018321873f7e1984ec78b9247"
)
COMPACT_MAX_MEMORY_EXPECTED_SHA256 = (
    "8430d0a6a314c800c910c1cc2a0ed1c665f6a8ca3aa702f6c8b66787ad3790ea"
)
COMPACT_MAX_MEMORY_TAGS_SHA256 = (
    "ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7"
)
COMPACT_MAX_BACKING_INPUT_CONTRACT_ID = (
    "input-contract-3caf0916db7f9185faea6633ebc50661939131eeea93e3c47ceabe9fbb924a4d"
)
COMPACT_MAX_BACKING_TRANSFER_LENGTH = 16_641
COMPACT_MAX_BACKING_SOURCE_START = 0x5100_3F2D
COMPACT_MAX_BACKING_DESTINATION_START = 0x5100_3FDA
COMPACT_MAX_BACKING_PROGRAM_WORDS = (
    0x1D04_04A3,
    0x1D44_04A3,
    0x1D84_04A3,
    0xD400_0001,
)
FULL_EXTENT_TRANSFER_CASE = "CPY full-extent 65536-byte backward overlap"
FULL_EXTENT_TRANSFER_LENGTH = 65_536
FULL_EXTENT_SOURCE_PAGE_DELTA = -211
FULL_EXTENT_OVERLAP_DELTA = 173
FULL_EXTENT_PATTERN_SEED = 0x86
FULL_EXTENT_PATTERN_STEP = 0x35
FULL_EXTENT_MIN_TIMEOUT_SECONDS = 300
FULL_EXTENT_MIN_RSS_KIB = 9 * 1024 * 1024
FULL_EXTENT_RESOURCE_LOCK = "a64-aslref-full-extent"
FULL_EXTENT_CAPTURE_PROFILES: Mapping[int, Mapping[str, object]] = {
    4096: {
        "input_contract_id": "input-contract-9f603ebaf608ef558f51ad7522bb7c4f38ca2153cbc9fab1c6656c75cda37ef5",
        "program_id": "program-35ac62bca818718a0ca5e64f6805f7386ab7758b58528bd7b149f37b2326221d",
        "initial_state_id": "state-f8a07a06d57dfdf704f75e8821693ae9d3a23bc139f8a608dc42c42c965a7131",
        "expected_state_id": "state-633623e2c4dad46f9a9699d126c0c8025cda339815bd424816e7fbfcf327366f",
        "memory_id": "memory-c442bbbd9602cd00da73f4d985a9522bc7af372c4eb75187c6392a48d46dccc3",
        "backing_length": 73_728,
        "initial_sha256": "c0f7ae8741214f60dbf0f46b678ccd1ed993fba32f3f2180ef9a96df88c3e4db",
        "expected_sha256": "f38cef7c22e67548ca2d197a6ddf9f95d230beef6767009a88ffdecad5075fca",
        "tags_sha256": "606f558e014930f9c1669f03c71c28945c4631568e39cd308c6c7f4077c7bfb9",
        "initial_state_digest": "9b17f76053f2da18",
        "expected_state_digest": "21ca5c9bc933077f",
        "initial_memory_digest": "2959678e7e507a39",
        "expected_memory_digest": "5461bde24bfc7a39",
    },
    16384: {
        "input_contract_id": "input-contract-71fafc9a2f6dfc8cfeae30070895a01b7f3be6618f58d0eef94e4dcd944a70af",
        "program_id": "program-8cc802e796a60a5a41361778f2c9f7495cd7360d964f6f16146a14f7e2659461",
        "initial_state_id": "state-5d86c7a2adf69550273ede7efe1f7541efeab1e1a11e99f83d9a46341d289419",
        "expected_state_id": "state-ce719d8632e488eae37a1916757cd7fcb777e5ca12f116e61f2c10d1cba703b7",
        "memory_id": "memory-643052834eb208fc68035193a23020dc8a4b10a35709ab398a01893a593c1afb",
        "backing_length": 98_304,
        "initial_sha256": "5d813898b3b3d1f5b50ba5a1104efc55edc2d2ef9c6d42676eec2646dda24f1e",
        "expected_sha256": "79125b717d690578081c13e5096a1b60c38d904d17865d3c8da7a7fb7670fbda",
        "tags_sha256": "fd9243e1ba57263ed469c3bdbd7ade6ec5254e7ed924a9f5737fa44749933cc0",
        "initial_state_digest": "84a416747214e9f8",
        "expected_state_digest": "617b527e0ae2d39f",
        "initial_memory_digest": "4238693173de9499",
        "expected_memory_digest": "64ecb31750869499",
    },
}

CALLBACK_EXACT = 0
CALLBACK_PERMITTED_TRANSFORMED_CODE = 1
EVENT_SYSTEM_CALL = 1
EVENT_MEMORY_FAULT = 2
FAULT_NONE = 0
FAULT_TRANSLATION = 1
FAULT_PERMISSION = 2
FEATURES_SHA256 = "6d5c19e70492db0edb7a0a51efddf78573a311289771097cc8ad6e2000129b58"
PAC_ALGORITHM = "FEAT_PACIMP"
PAC_RELATION = "permitted-transformed-code/v1"


@dataclass(frozen=True)
class Field:
    name: str
    width: int
    offset: int


@dataclass(frozen=True)
class CompactMemoryAslSpec:
    length: int
    tag_length: int
    formula_seed: int
    formula_step: int
    formula_modulus: int
    copy_start: int
    copy_end: int
    copy_source_displacement: int
    initial_overrides: tuple[tuple[int, int], ...] = ()
    expected_overrides: tuple[tuple[int, int], ...] = ()


class PackedLayout:
    def __init__(self, specifications: Iterable[tuple[str, int]]) -> None:
        offset = 0
        fields: list[Field] = []
        for name, width in specifications:
            if width <= 0:
                raise ValueError(f"{name}: nonpositive packed width")
            fields.append(Field(name, width, offset))
            offset += width
        self.fields = tuple(fields)
        self.width = offset
        self.by_name = {field.name: field for field in fields}

    def pack(self, values: Mapping[str, int]) -> int:
        if set(values) != set(self.by_name):
            raise ValueError("packed field set differs")
        result = 0
        for field in self.fields:
            value = int(values[field.name])
            if value < 0 or value >= 1 << field.width:
                raise ValueError(f"{field.name}: value exceeds bits({field.width})")
            result |= value << field.offset
        return result


STATE_LAYOUT = PackedLayout(
    [(f"x{index}", 64) for index in range(31)]
    + [("sp", 64), ("pc", 64), ("nzcv", 4)]
    + [(f"v{index}", 128) for index in range(32)]
    + [
        ("fpcr", 32),
        ("fpsr", 32),
        ("fpmr", 64),
        ("tls", 64),
        ("pstate_ssbs", 1),
        ("pstate_dit", 1),
        ("pstate_tco", 1),
        ("pstate_sm", 1),
        ("pstate_za", 1),
        ("pstate_daif_access", 1),
        ("pstate_daif", 8),
        ("mte_enabled", 1),
        ("mte_exclude", 16),
        ("mte_random_tags", 1),
        ("mte_seed", 16),
        ("mte_tag", 8),
        ("cpa_add_enabled", 1),
        ("cpa_multiply_enabled", 1),
        ("accdata", 32),
        ("gcs_pointer", 64),
        ("gcs_implemented", 1),
        ("gcs_push_enabled", 1),
        ("gcs_store_enabled", 1),
    ]
    + [
        item
        for key in range(5)
        for item in ((f"key{key}_low", 64), (f"key{key}_high", 64))
    ]
    + [
        ("signing_enabled", 1),
        ("exclusive_valid", 1),
        ("exclusive_address", 64),
        ("exclusive_key", 64),
        ("exclusive_value", 64),
        ("exclusive_value2", 64),
        ("exclusive_version", 64),
        ("exclusive_size", 8),
    ]
)

CONTRACT_LAYOUT = PackedLayout(
    (
        ("initial_state", 16),
        ("expected_state", 16),
        ("memory", 16),
        ("program", 16),
        ("callback", 8),
        ("event", 8),
        ("features", 8),
        ("evidence", 32),
        ("syndrome", 32),
        ("fault_address", 64),
        ("fault_write", 1),
        ("fault_bytes", 8),
        ("fault_code", 2),
        ("fault_throws", 1),
        ("restore_on_fault", 1),
        ("terminal_word", 8),
        ("retired_words", 8),
    )
)


@dataclass(frozen=True)
class Operation:
    word: int
    architecture_file: str
    architecture_encoding: str
    source_file: str
    function: str
    features: tuple[str, ...]


@dataclass(frozen=True)
class PreparedSources:
    instructions: tuple[Path, ...]
    shared: Path
    operations: Mapping[int, Operation]


@dataclass(frozen=True)
class ImpDefProjection:
    kind: str
    description: str
    declared_type: str
    policy_class: str
    value: int
    minimum: int | None
    maximum: int | None
    review: str
    allowed_values: tuple[int, ...] = ()


@dataclass(frozen=True)
class AslDefinitionEntry:
    name: str
    header: str
    return_types: tuple[str, ...]
    body: str
    start: int
    end: int


@dataclass(frozen=True)
class TupleForwardSite:
    source_index: int
    owner: str
    callee: str
    owner_header: str
    callee_headers: tuple[str, ...]
    return_types: tuple[str, ...]
    owner_body_sha256: str
    body_offset: int
    start: int
    end: int
    indentation: str
    call_expression: str
    statement: str


# These are the implementation-defined choices conservatively reachable from
# main().  Every descriptor is named explicitly; this is intentionally not a
# catch-all false/zero policy.  Feature-gated and overload-ambiguous branches
# remain projected because static ambiguity must never be used as an
# unreachability proof.
_REVIEWED_FALSE_IMPDEF_BOOLS = (
    "AT reports the HW update fault",
    "AT updates AF",
    "Apply effective shareability at stage 1",
    "BADDR expresses 52 bits for 64KB granule",
    "CPY in the forward direction",
    "CPY* instructions use Option A",
    "DCIMVAC generates watchpoint",
    "Device location supports unaligned access",
    "EDWAR is not valid on watchpoint debug event",
    "EL3 trap priority when SDD == '1'",
    "External Abort on HDBSS write reported in HDBSSPROD_EL2",
    "FAR is not valid on watchpoint exception",
    "FEAT_LSE2: access is atomic",
    "Fault on TxSZ value above maximum",
    "Fault on TxSZ value below minimum",
    "GPC Fault on DC operations",
    "GPC fault on HDBSSS write reported in HDBSSPROD_EL2",
    "GPF, GPC, Data SyncExternal Aborts, TagChecks prioritized over Watchpoints",
    "Generate access flag fault on IC/DC operations",
    "HCR_EL2.NV1 is implemented as RAZ",
    "Implementation option is checked with a cpysize of 0",
    "Implementation option is checked with a stage cpysize of 0",
    "Instruction SyncExternal Aborts prioritized over Breakpoints",
    "LD64B or ST64B faults to cacheable non-iWBoWB memory",
    "MPAMSM_EL1 label precedence",
    "PAN applies when S1UnprivBasePerm is reserved",
    "PAN applies when S1UnprivBasePerms are removed",
    "Permission fault on EL0 IC_IVAU execution",
    "Programming HCR_EL2.<NV,NV2> to '10' behaves as '11'",
    "Realm EL2&0 regime affects EPAN",
    "Report SPE ExtAbort as SError",
    "Report SPE ExtAbort asynchronously",
    "Report SPE ExtAbort on TTW as fault",
    "Report TRBE ExtAbort as SError",
    "Report TRBE ExtAbort asynchronously",
    "SCR_EL3.PIEn forces PIR_ELx or POR_ELx to zero",
    "SCR_EL3.SIF affects EPAN",
    "SPE 16bit counters",
    "SPE PAT for tag unchecked access zero",
    "SPE get prev br if not br",
    "SPE non-tbi tag is zero",
    "SPE prev br on eret",
    "SPE prev br on exception",
    "SPE prev br on isb",
    "Secure-only implementation",
    "Shared SMCU",
    "Translation fault on misprogrammed contiguous bit",
    "Treat Contiguous bit as 0",
    "WPTV field is valid",
    "descriptor[15:12] for 64KB granule are OA[51:48]",
    "nT bit causes Translation Fault",
)

_FALSE_REVIEW = (
    "reviewed baseline false choice; retained for conservative feature/overload closure"
)
_SPECIAL_FALSE_REVIEWS = {
    "CPY* instructions use Option A": "Option B is the recorder/native MOPS policy",
    "CPY in the forward direction": (
        "deterministic backward choice when direction is implementation-defined"
    ),
    "Implementation option is checked with a cpysize of 0": (
        "zero-size CPY uses the unchecked implementation option"
    ),
    "Implementation option is checked with a stage cpysize of 0": (
        "zero-size CPY stage uses the unchecked implementation option"
    ),
}

IMPDEF_PROJECTIONS = tuple(
    ImpDefProjection(
        "Bool",
        description,
        "boolean",
        "enumerated",
        0,
        0,
        1,
        _SPECIAL_FALSE_REVIEWS.get(description, _FALSE_REVIEW),
        (0, 1),
    )
    for description in _REVIEWED_FALSE_IMPDEF_BOOLS
) + (
    ImpDefProjection(
        "Bit", "WPF value on TRUE Watchpoint match", "bits(1)", "enumerated",
        0, 0, 1, "reviewed false WPF syndrome choice", (0, 1),
    ),
    ImpDefProjection(
        "Bits", "TG0 encoded granule size", "bits(2)", "bounded", 0, 0, 3,
        "reviewed 4KB TG0 encoding",
    ),
    ImpDefProjection(
        "Bits", "TG1 encoded granule size", "bits(2)", "bounded", 2, 0, 3,
        "reviewed 4KB TG1 encoding",
    ),
    ImpDefProjection(
        "Bits", "target_el for SPE ImpDef Fault", "bits(2)", "bounded",
        1, 0, 3,
        "reviewed EL1 target for an implementation-defined SPE fault",
    ),
    ImpDefProjection(
        "Bits", "target_el for TRBE ImpDef Fault", "bits(2)", "bounded",
        1, 0, 3,
        "reviewed EL1 target for an implementation-defined TRBE fault",
    ),
    ImpDefProjection(
        "Int", "Aligned quantity for atomic access", "integer{16..4096}",
        "bounded", 4096, 16, 4096,
        "official pseudocode bound and single-translation alignment quantity",
    ),
    ImpDefProjection(
        "Int", "Maximum Physical Address Size",
        "AddressSize (integer{3..64})", "bounded", 48, 3, 64,
        "baseline AArch64 physical-address size, bounded by AArch64_OAMax()",
    ),
    ImpDefProjection(
        "Int", "Number of BRBE Branch records", "integer in {8,16,32,64}",
        "enumerated", 8, 8, 64,
        "minimum architecturally admitted BRBE record count",
        (8, 16, 32, 64),
    ),
    ImpDefProjection(
        "Int", "Number of address-linking breakpoints", "integer (unconstrained)",
        "fixed", 0, None, None, "no FEAT_ABLE address-linking breakpoints",
    ),
    ImpDefProjection(
        "Int", "Number of breakpoints", "integer (unconstrained)",
        "fixed", 2, None, None, "baseline two debug breakpoints",
    ),
    ImpDefProjection(
        "Int", "Number of context-aware breakpoints", "integer (unconstrained)",
        "fixed", 0, None, None, "no context-aware debug breakpoints",
    ),
    ImpDefProjection(
        "Int", "Number of watchpoints", "integer (unconstrained)",
        "fixed", 2, None, None, "baseline two debug watchpoints",
    ),
    ImpDefProjection(
        "Int", "SPE Data Source packet payload size", "integer{1..2}",
        "bounded", 1, 1, 2, "minimum official SPE data-source payload size",
    ),
    ImpDefProjection(
        "Int", "SPE Events packet payload size", "integer{1..4}",
        "bounded", 1, 1, 4, "minimum official SPE event payload size",
    ),
)


IMPDEF_PROJECTION_BY_KEY = {
    (projection.kind, projection.description): projection
    for projection in IMPDEF_PROJECTIONS
}

IMPDEF_INVENTORY_SHA256 = "baa52ef386aa4cc7f7819f3999aaf90bfd6d1a19f87e26f051b00f840f56b71b"
IMPDEF_CONTEXT_SHA256 = "46be6d65d5e4aee1c8b5346a4e375ed792e09d6a4974f5f8cb05093ab2d61913"
IMPDEF_POLICY_SHA256 = "450e4ba7c64a777937e271e72fef1ad50af1a15fb824e730774d7654af3c99ac"
IMPLEMENTATION_DEFINED_ADAPTER_SHA256 = "cd5b892cd8840a3b7e13f8a2b85acc8c54c0894bee63b22b439dac4114881dfc"
SUPPLIED_INTERFACE_SIGNATURE_SHA256 = "df3df5bfb9fae1740d6999504184112ca3022a69f5f2b7534d5bcab69864c180"
SUPPLIED_INTERFACE_CALLER_SHA256 = "447dec3052cb59617a0e4d48af3edcc8afd364e5cf5e83ecd790be749cb6a8f1"
SUPPLIED_INTERFACE_REACHABLE_NAMES = 337
SUPPLIED_INTERFACE_REACHABLE_OVERLOADS = 338


def ordered_identifier_digest(identifiers: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for identifier in identifiers:
        encoded = identifier.encode("ascii")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


def official_input_result_digest(identifiers: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for identifier in identifiers:
        payload = json.dumps(
            [identifier, "official-asl-passed"],
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()
TUPLE_FORWARD_INVENTORY_SHA256 = "7a4a278ad73c0fa75617e563a5d282dc640c1dc269e64dafd0870751cee89185"
TUPLE_FORWARD_INVENTORY_COUNT = 4
FAULT_DOMAIN_TRANSLATE_BODY_SHA256 = (
    "be6f7705e3583f95f16814b65f7de6cb1230b01e562184f80a8930d057537def"
)
FAULT_DOMAIN_PHYSICAL_CALLBACK_SHA256 = (
    "9e8391b3421498d3147a159202b0f67bb87afb3d252ab92af2c0bc2cea828add"
)
FAULT_DOMAIN_VIRTUAL_CALLBACK_SHA256 = (
    "ff9f7ea8046f070f405687aaf29142a6fbf48a293a6f8ccc9356d733828e5efe"
)
FAULT_DOMAIN_OFFICIAL_CONSUMER_SHA256 = (
    "78f70bb6a0d91046c05ac54b7300af97a8629cb9f6044a60beb86c675267e7a1"
)
FAULT_DOMAIN_OFFICIAL_BODY_SHA256 = {
    "instr_cpyp": "16fd9777757f2107e7320453cb7db026683b2f2c1bc4b13dac9b586d4e4f99a1",
    "instr_cpyptwn": "b16b3dc9441dcc8abaabf5aca0d0ab1736e370bc11ba340c3a304ba27ac8120d",
    "ExternalFault": "71702a5d941a80a35e668d1f5eda929fd85fdae11c36a3719dfb72d45a3c1b16",
    "HandleExternalAbort": "15fcf2e6ea6a8ccb030e7e7fd5558783cd012bc09aa35e6a0cac31f9671144ff",
    "ExternalAbort": "4985eaa2ffb771bd95d1b9c4a4f1317b6403948b11e823f67609efecea0e3bd8",
}
TUPLE_FORWARD_REWRITE_KEYS = frozenset(
    {("AArch64_S1Translate", "AArch64_S1DisabledOutput")}
)
TUPLE_FORWARD_DORMANT_PROOFS = {
    ("AArch64_S1Translate", "AArch64_S1Translate"): (
        "recursive descriptor-update restart is unreachable with stage-1 "
        "translation disabled by the projected SCTLR policy"
    ),
    ("AArch64_S2Translate", "AArch64_S2Translate"): (
        "recursive descriptor-update restart is unreachable with stage-2 "
        "translation disabled by the projected EL2 policy"
    ),
    ("GenPARTID", "MAP_vPARTID"): (
        "virtual MPAM PARTID mapping is unreachable because FEAT_MPAM is "
        "absent from every selected feature profile"
    ),
}
IMPDEF_INVENTORY_UNIQUE = 65
IMPDEF_INVENTORY_CALL_SITES = 98
IMPLEMENTATION_DEFINED_ADAPTERS = frozenset(
    {
        "ImpDefBool",
        "ImpDefInt",
        "ImpDefBits",
        "ImpDefBit",
        "ComputePACIMPDEF",
        "ComputePAC2IMPDEF",
    }
)

_ASL_DEFINITION = re.compile(
    r"(?m)^(?:(?:readonly|noreturn)\s+)?(?:func|accessor)\s+"
    r"([A-Za-z][A-Za-z0-9_]*)"
)
_ASL_CALL = re.compile(
    r"\b([A-Za-z][A-Za-z0-9_]*)\s*"
    r"(?:\{[^{}\n]*\}\s*)?(?=[(\[])"
)
_IMPDEF_CALL = re.compile(
    r"\bImpDef(?P<kind>Bool|Int|Bits|Bit)"
    r"(?:\{(?P<width>[0-9]+)\})?\s*\(\s*"
    r'"(?P<description>[^"\n]+)"\s*\)',
    re.MULTILINE,
)
_TUPLE_FORWARD_RETURN = re.compile(
    r"(?m)^(?P<indent>[ \t]*)return\s+"
    r"(?P<call>(?P<callee>[A-Za-z][A-Za-z0-9_]*)"
    r"(?:\{[^}\n]+\})?\s*\([^;]*?\))\s*;"
)


def impdef_inventory(text: str) -> tuple[tuple[str, int, str], ...]:
    calls: list[tuple[str, int, str]] = []
    for match in _IMPDEF_CALL.finditer(text):
        kind = match.group("kind")
        width_text = match.group("width")
        if kind == "Bits":
            if width_text is None or int(width_text) <= 0:
                raise ValueError("ImpDefBits call lacks a positive static width")
            width = int(width_text)
        elif width_text is not None:
            raise ValueError(f"ImpDef{kind} unexpectedly has a width argument")
        else:
            width = 1 if kind in {"Bool", "Bit"} else 0
        calls.append((kind, width, match.group("description")))
    return tuple(calls)


def impdef_inventory_digest(
    inventory: Iterable[tuple[str, int, str]],
) -> str:
    canonical = "".join(
        f"{kind}|{width}|{description}\n"
        for kind, width, description in sorted(set(inventory))
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def impdef_context_digest(text: str) -> str:
    definitions = list(_ASL_DEFINITION.finditer(text))
    call_sites: list[str] = []
    for call in _IMPDEF_CALL.finditer(text):
        owner_index = next(
            (
                index
                for index, definition in enumerate(definitions)
                if definition.start() <= call.start()
                and (
                    index + 1 == len(definitions)
                    or call.start() < definitions[index + 1].start()
                )
            ),
            None,
        )
        if owner_index is None:
            raise ValueError("ImpDef call is outside an ASL function/accessor")
        owner = definitions[owner_index]
        owner_end = (
            definitions[owner_index + 1].start()
            if owner_index + 1 < len(definitions)
            else len(text)
        )
        owner_text = text[owner.start() : owner_end]
        owner_sha256 = hashlib.sha256(owner_text.encode("utf-8")).hexdigest()
        kind = call.group("kind")
        width = int(call.group("width") or (1 if kind in {"Bool", "Bit"} else 0))
        call_sites.append(
            f"{kind}|{width}|{call.group('description')}|{owner.group(1)}|"
            f"{call.start() - owner.start()}|{owner_sha256}\n"
        )
    return hashlib.sha256("".join(sorted(call_sites)).encode("utf-8")).hexdigest()


def validate_impdef_projection(projection: ImpDefProjection) -> None:
    if projection.policy_class not in {
        "fixed", "bounded", "enumerated", "state-derived"
    }:
        raise ValueError(f"{projection.description}: unknown projected policy class")
    if not projection.review.strip():
        raise ValueError(f"{projection.description}: projected policy lacks review")
    if (projection.minimum is None) != (projection.maximum is None):
        raise ValueError(f"{projection.description}: partial projected range")
    if projection.minimum is not None and projection.maximum is not None:
        if projection.minimum > projection.maximum:
            raise ValueError(f"{projection.description}: inverted projected range")
        if not projection.minimum <= projection.value <= projection.maximum:
            raise ValueError(f"{projection.description}: projected value is out of range")
    if projection.allowed_values and projection.value not in projection.allowed_values:
        raise ValueError(
            f"{projection.description}: projected value is outside its admitted set"
        )
    if projection.policy_class == "fixed" and (
        projection.minimum is not None or projection.allowed_values
    ):
        raise ValueError(f"{projection.description}: fixed policy has a domain")
    if projection.policy_class == "bounded" and (
        projection.minimum is None or projection.allowed_values
    ):
        raise ValueError(f"{projection.description}: bounded policy domain differs")
    if projection.policy_class == "enumerated":
        if (
            not projection.allowed_values
            or len(set(projection.allowed_values)) != len(projection.allowed_values)
            or tuple(sorted(projection.allowed_values)) != projection.allowed_values
            or projection.minimum != projection.allowed_values[0]
            or projection.maximum != projection.allowed_values[-1]
        ):
            raise ValueError(
                f"{projection.description}: enumerated policy domain differs"
            )
    if projection.policy_class == "state-derived" and not projection.review:
        raise ValueError(f"{projection.description}: state-derived policy lacks proof")
    if projection.kind in {"Bool", "Bit"} and (
        projection.minimum != 0 or projection.maximum != 1
    ):
        raise ValueError(
            f"{projection.description}: projected boolean/bit range differs"
        )
    if projection.kind == "Int" and not projection.declared_type.startswith(
        ("integer", "AddressSize")
    ):
        raise ValueError(f"{projection.description}: projected integer type differs")


def impdef_policy_digest(projections: Iterable[ImpDefProjection]) -> str:
    canonical = "".join(
        f"{projection.kind}|{projection.description}|{projection.declared_type}|"
        f"{projection.policy_class}|{projection.value}|{projection.minimum}|"
        f"{projection.maximum}|{','.join(map(str, projection.allowed_values))}|"
        f"{projection.review}\n"
        for projection in sorted(
            projections, key=lambda item: (item.kind, item.description)
        )
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def impdef_adapter_values(text: str) -> dict[tuple[str, str], int]:
    definitions = asl_definition_bodies((text,))
    values: dict[tuple[str, str], int] = {}
    direct = re.compile(
        r'if description == "(?P<description>[^"\n]+)" then return '
        r"(?P<value>FALSE|TRUE|-?[0-9]+|'[01]+'); end;"
    )
    for kind, name in (
        ("Bool", "ImpDefBool"),
        ("Int", "ImpDefInt"),
        ("Bit", "ImpDefBit"),
    ):
        bodies = definitions.get(name, ())
        if len(bodies) != 1:
            raise ValueError(f"{name}: expected one deterministic adapter body")
        for match in direct.finditer(bodies[0]):
            literal = match.group("value")
            value = (
                int(literal == "TRUE")
                if literal in {"FALSE", "TRUE"}
                else int(literal.strip("'"), 2)
                if literal.startswith("'")
                else int(literal)
            )
            values[(kind, match.group("description"))] = value

    bits_bodies = definitions.get("ImpDefBits", ())
    if len(bits_bodies) != 1:
        raise ValueError("ImpDefBits: expected one deterministic adapter body")
    bits_body = bits_bodies[0]
    required_bits_fragments = {
        ("Bits", "TG0 encoded granule size"): (
            'if description == "TG0 encoded granule size" then return result; end;',
            0,
        ),
        ("Bits", "TG1 encoded granule size"): (
            'if description == "TG1 encoded granule size" then\n'
            "        result[N - 1] = '1';\n"
            "        return result;",
            2,
        ),
        ("Bits", "target_el for SPE ImpDef Fault"): (
            'if description == "target_el for SPE ImpDef Fault" ||\n'
            '       description == "target_el for TRBE ImpDef Fault" then\n'
            "        result[0] = '1';\n"
            "        return result;",
            1,
        ),
        ("Bits", "target_el for TRBE ImpDef Fault"): (
            'if description == "target_el for SPE ImpDef Fault" ||\n'
            '       description == "target_el for TRBE ImpDef Fault" then\n'
            "        result[0] = '1';\n"
            "        return result;",
            1,
        ),
    }
    if "assert N == 2;" not in bits_body or "Zeros{N}" not in bits_body:
        raise ValueError("ImpDefBits width/zero initialization policy drifted")
    for key, (fragment, value) in required_bits_fragments.items():
        if fragment not in bits_body:
            raise ValueError(f"{key[1]}: deterministic adapter branch drifted")
        values[key] = value
    return values


def verify_impdef_value_policies(
    adapter_text: str,
    projections: Mapping[tuple[str, str], ImpDefProjection],
) -> dict[str, object]:
    for projection in projections.values():
        validate_impdef_projection(projection)
    digest = impdef_policy_digest(projections.values())
    if digest != IMPDEF_POLICY_SHA256:
        raise ValueError("implementation-defined value-domain policy digest drifted")
    actual = impdef_adapter_values(adapter_text)
    if set(actual) != set(projections):
        raise ValueError("deterministic ImpDef adapter/value policy coverage differs")
    mismatches = [
        key for key, projection in projections.items()
        if actual[key] != projection.value
    ]
    if mismatches:
        raise ValueError(
            "deterministic ImpDef adapter values differ: "
            + ", ".join(f"ImpDef{kind}({description!r})" for kind, description in mismatches)
        )
    counts: dict[str, int] = {}
    for projection in projections.values():
        counts[projection.policy_class] = counts.get(projection.policy_class, 0) + 1
    return {
        "policy_sha256": digest,
        "caller_constraint_context_sha256": IMPDEF_CONTEXT_SHA256,
        "classified_returns": len(projections),
        "policy_class_counts": {
            policy: counts.get(policy, 0)
            for policy in ("fixed", "bounded", "enumerated", "state-derived")
        },
        "adapter_values_verified": len(actual),
    }


def require_impdef_context_digest(text: str, expected: str) -> None:
    if impdef_context_digest(text) != expected:
        raise ValueError("pinned ImpDef owner/context digest drifted")


def reachable_impdef_owners(
    all_texts: Iterable[str], roots: Iterable[str] = ("main",)
) -> tuple[
    frozenset[str], dict[tuple[str, int, str], frozenset[str]]
]:
    reachable, definitions = conservative_reachable_functions(all_texts, roots)
    owners_by_hook: dict[tuple[str, int, str], set[str]] = {}
    for owner in reachable:
        for body in definitions[owner]:
            for hook in impdef_inventory(body):
                owners_by_hook.setdefault(hook, set()).add(owner)
    return reachable, {
        hook: frozenset(owners) for hook, owners in owners_by_hook.items()
    }


def require_reachable_impdef_projections(
    all_texts: Iterable[str],
    projections: Mapping[tuple[str, str], ImpDefProjection],
    roots: Iterable[str] = ("main",),
) -> tuple[frozenset[str], frozenset[tuple[str, int, str]]]:
    reachable, owners_by_hook = reachable_impdef_owners(all_texts, roots)
    missing: list[str] = []
    for kind, width, description in sorted(owners_by_hook):
        projection = projections.get((kind, description))
        if projection is None:
            owners = ",".join(sorted(owners_by_hook[(kind, width, description)]))
            missing.append(f"ImpDef{kind}({description!r}) via {owners}")
            continue
        if kind == "Bits" and projection.maximum != (1 << width) - 1:
            raise ValueError(f"{description}: projected bits({width}) range differs")
        validate_impdef_projection(projection)
    if missing:
        raise ValueError(
            "reachable implementation-defined hooks lack reviewed projections: "
            + "; ".join(missing)
        )
    return reachable, frozenset(owners_by_hook)


def asl_definition_bodies(texts: Iterable[str]) -> dict[str, list[str]]:
    definitions: dict[str, list[str]] = {}
    for text in texts:
        matches = list(_ASL_DEFINITION.finditer(text))
        for index, match in enumerate(matches):
            end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
            definitions.setdefault(match.group(1), []).append(
                text[match.start() : end]
            )
    return definitions


def _return_arity(header: str) -> int:
    if re.match(r"(?:(?:readonly|noreturn)\s+)?accessor\b", header):
        return 1
    arrow = re.search(r"(?<!<)=>", header)
    if arrow is None:
        return 0
    result = header[arrow.end() :].strip()
    if not result.startswith("("):
        return 1
    depth = 0
    commas = 0
    for character in result:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return commas + 1
        elif character == "," and depth == 1:
            commas += 1
    raise ValueError("tuple return signature is unbalanced")


def _return_types(header: str) -> tuple[str, ...]:
    if re.match(r"(?:(?:readonly|noreturn)\s+)?accessor\b", header):
        return ("accessor-value",)
    arrow = re.search(r"(?<!<)=>", header)
    if arrow is None:
        return ()
    result = header[arrow.end() :].strip()
    if not result.startswith("("):
        return (re.split(r"\s+recurselimit\b", result, maxsplit=1)[0].strip(),)
    depth = 0
    end = -1
    for index, character in enumerate(result):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                end = index
                break
    if end < 0:
        raise ValueError("tuple return signature is unbalanced")
    content = result[1:end]
    components: list[str] = []
    start = 0
    depth = 0
    for index, character in enumerate(content):
        if character in "({[":
            depth += 1
        elif character in ")}]":
            depth -= 1
        elif character == "," and depth == 0:
            components.append(re.sub(r"\s+", " ", content[start:index]).strip())
            start = index + 1
    components.append(re.sub(r"\s+", " ", content[start:]).strip())
    if any(not component for component in components):
        raise ValueError("tuple return signature has an empty component")
    return tuple(components)


def asl_definition_entries(text: str) -> tuple[AslDefinitionEntry, ...]:
    matches = list(_ASL_DEFINITION.finditer(text))
    entries: list[AslDefinitionEntry] = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        body = text[match.start() : end]
        marker = re.search(r"\bbegin\b", body)
        if marker is None:
            raise ValueError(f"{match.group(1)}: ASL definition lacks a begin marker")
        header = re.sub(r"\s+", " ", body[: marker.start()]).strip()
        return_types = _return_types(header)
        if len(return_types) != _return_arity(header):
            raise ValueError(f"{match.group(1)}: return arity/type parse differs")
        entries.append(
            AslDefinitionEntry(
                match.group(1), header, return_types, body, match.start(), end
            )
        )
    return tuple(entries)


def asl_definition_signatures(
    texts: Iterable[str],
) -> dict[str, list[tuple[str, int]]]:
    result: dict[str, list[tuple[str, int]]] = {}
    for text in texts:
        for entry in asl_definition_entries(text):
            result.setdefault(entry.name, []).append(
                (entry.header, len(entry.return_types))
            )
    return result


def supplied_interface_signature_digest(
    signatures: Mapping[str, Sequence[tuple[str, int]]],
    reachable_names: Iterable[str],
) -> tuple[str, int]:
    canonical: list[str] = []
    for name in sorted(set(reachable_names)):
        for header, arity in sorted(signatures[name]):
            canonical.append(f"{name}|return-arity={arity}|{header}\n")
    return (
        hashlib.sha256("".join(canonical).encode("utf-8")).hexdigest(),
        len(canonical),
    )


def require_supplied_interface_signature_digest(
    signatures: Mapping[str, Sequence[tuple[str, int]]],
    reachable_names: Iterable[str],
    expected: str,
) -> None:
    digest, _overloads = supplied_interface_signature_digest(
        signatures, reachable_names
    )
    if digest != expected:
        raise ValueError("reachable supplied-interface signature digest drifted")


def supplied_interface_caller_digest(
    official_texts: Iterable[str], reachable_supplied: frozenset[str]
) -> tuple[str, int]:
    callers: list[str] = []
    definitions = asl_definition_bodies(official_texts)
    for owner, bodies in definitions.items():
        for body in bodies:
            called = sorted(set(_ASL_CALL.findall(body)) & reachable_supplied)
            if called:
                callers.append(
                    f"{owner}|{','.join(called)}|"
                    f"{hashlib.sha256(body.encode('utf-8')).hexdigest()}\n"
                )
    return (
        hashlib.sha256("".join(sorted(callers)).encode("utf-8")).hexdigest(),
        len(callers),
    )


def verify_supplied_interface_signature_closure(
    official_texts: Sequence[str],
    supplied_texts: Sequence[str],
    harness_text: str,
) -> dict[str, object]:
    official_definitions = asl_definition_bodies(official_texts)
    supplied_definitions = asl_definition_bodies(supplied_texts)
    collisions = sorted(official_definitions.keys() & supplied_definitions.keys())
    if collisions:
        raise ValueError(
            "official/supplied ASL definition collision: " + ", ".join(collisions)
        )
    reachable, _definitions = conservative_reachable_functions(
        (*official_texts, *supplied_texts, harness_text), ("main",)
    )
    reachable_supplied = frozenset(reachable & supplied_definitions.keys())
    signatures = asl_definition_signatures(supplied_texts)
    digest, overloads = supplied_interface_signature_digest(
        signatures, reachable_supplied
    )
    if len(reachable_supplied) != SUPPLIED_INTERFACE_REACHABLE_NAMES:
        raise ValueError("reachable supplied-interface name count drifted")
    if overloads != SUPPLIED_INTERFACE_REACHABLE_OVERLOADS:
        raise ValueError("reachable supplied-interface overload count drifted")
    require_supplied_interface_signature_digest(
        signatures, reachable_supplied, SUPPLIED_INTERFACE_SIGNATURE_SHA256
    )
    caller_digest, caller_count = supplied_interface_caller_digest(
        official_texts, reachable_supplied
    )
    if caller_digest != SUPPLIED_INTERFACE_CALLER_SHA256:
        raise ValueError("official caller/interface context digest drifted")
    arities: dict[int, int] = {}
    for name in reachable_supplied:
        for _header, arity in signatures[name]:
            arities[arity] = arities.get(arity, 0) + 1
    return {
        "signature_sha256": digest,
        "official_caller_sha256": caller_digest,
        "reachable_names": len(reachable_supplied),
        "reachable_overloads": overloads,
        "official_caller_definitions": caller_count,
        "return_arity_counts": {
            str(arity): count for arity, count in sorted(arities.items())
        },
    }


def tuple_forward_inventory(
    official_texts: Sequence[str],
    supplemental_texts: Sequence[str],
    harness_text: str,
) -> tuple[TupleForwardSite, ...]:
    all_texts = (*official_texts, *supplemental_texts, harness_text)
    reachable, _definitions = conservative_reachable_functions(
        all_texts, ("main",)
    )
    entries_by_name: dict[str, list[AslDefinitionEntry]] = {}
    for text in all_texts:
        for entry in asl_definition_entries(text):
            entries_by_name.setdefault(entry.name, []).append(entry)

    sites: list[TupleForwardSite] = []
    for source_index, text in enumerate(official_texts):
        for owner in asl_definition_entries(text):
            if owner.name not in reachable or len(owner.return_types) <= 1:
                continue
            for match in _TUPLE_FORWARD_RETURN.finditer(owner.body):
                callee = match.group("callee")
                tuple_callees = [
                    entry
                    for entry in entries_by_name.get(callee, ())
                    if len(entry.return_types) > 1
                ]
                if not tuple_callees:
                    continue
                callee_types = {entry.return_types for entry in tuple_callees}
                if len(callee_types) != 1:
                    raise ValueError(
                        f"{owner.name}: tuple-forward callee {callee} has "
                        "ambiguous return components"
                    )
                if owner.return_types != next(iter(callee_types)):
                    raise ValueError(
                        f"{owner.name}: tuple-forward return component types differ "
                        f"from {callee}"
                    )
                sites.append(
                    TupleForwardSite(
                        source_index=source_index,
                        owner=owner.name,
                        callee=callee,
                        owner_header=owner.header,
                        callee_headers=tuple(
                            sorted(entry.header for entry in tuple_callees)
                        ),
                        return_types=owner.return_types,
                        owner_body_sha256=hashlib.sha256(
                            owner.body.encode("utf-8")
                        ).hexdigest(),
                        body_offset=match.start(),
                        start=owner.start + match.start(),
                        end=owner.start + match.end(),
                        indentation=match.group("indent"),
                        call_expression=match.group("call"),
                        statement=match.group(0),
                    )
                )
    return tuple(
        sorted(
            sites,
            key=lambda site: (site.source_index, site.start, site.end),
        )
    )


def tuple_forward_inventory_digest(
    sites: Iterable[TupleForwardSite],
) -> str:
    canonical = "".join(
        f"{site.source_index}|{site.owner}|{site.owner_header}|"
        f"{site.callee}|{'||'.join(site.callee_headers)}|"
        f"{','.join(site.return_types)}|{site.owner_body_sha256}|"
        f"{site.body_offset}|{re.sub(r'\s+', ' ', site.statement).strip()}\n"
        for site in sites
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def require_tuple_forward_inventory(
    sites: Sequence[TupleForwardSite],
    expected_count: int,
    expected_digest: str,
) -> None:
    if len(sites) != expected_count:
        raise ValueError("reachable tuple-forward inventory count drifted")
    if tuple_forward_inventory_digest(sites) != expected_digest:
        raise ValueError("reachable tuple-forward owner/callee context drifted")
    if any(len(site.return_types) != 2 for site in sites):
        raise ValueError(
            "tuple-forward normalization supports exactly two return components"
        )


def normalize_reachable_tuple_forwards(
    official_texts: Sequence[str],
    supplemental_texts: Sequence[str],
    harness_text: str,
    expected_count: int = TUPLE_FORWARD_INVENTORY_COUNT,
    expected_digest: str = TUPLE_FORWARD_INVENTORY_SHA256,
    rewrite_keys: frozenset[tuple[str, str]] = TUPLE_FORWARD_REWRITE_KEYS,
    dormant_proofs: Mapping[tuple[str, str], str] = TUPLE_FORWARD_DORMANT_PROOFS,
) -> tuple[tuple[str, ...], dict[str, object]]:
    sites = tuple_forward_inventory(
        official_texts, supplemental_texts, harness_text
    )
    require_tuple_forward_inventory(sites, expected_count, expected_digest)
    keys = [(site.owner, site.callee) for site in sites]
    if len(keys) != len(set(keys)):
        raise ValueError("tuple-forward owner/callee key is not unique")
    if set(keys) != set(rewrite_keys) | set(dormant_proofs):
        raise ValueError("tuple-forward execution/dormancy partition is incomplete")
    if set(rewrite_keys) & set(dormant_proofs):
        raise ValueError("tuple-forward site is both rewritten and dormant")
    if any(not proof.strip() for proof in dormant_proofs.values()):
        raise ValueError("tuple-forward dormant site lacks a path proof")
    selected = [
        site for site in sites if (site.owner, site.callee) in rewrite_keys
    ]
    normalized = list(official_texts)
    replacements = 0
    site_indexes = {id(site): index for index, site in enumerate(sites)}
    for site in reversed(selected):
        site_index = site_indexes[id(site)]
        source = normalized[site.source_index]
        if source[site.start : site.end] != site.statement:
            raise ValueError("tuple-forward source context changed during normalization")
        first = f"A64TupleForward{site_index}First"
        second = f"A64TupleForward{site_index}Second"
        indent = site.indentation
        replacement = "\n".join(
            (
                f"{indent}var {first} : {site.return_types[0]};",
                f"{indent}var {second} : {site.return_types[1]};",
                f"{indent}({first}, {second}) = {site.call_expression};",
                f"{indent}return ({first}, {second});",
            )
        )
        normalized[site.source_index] = (
            source[: site.start] + replacement + source[site.end :]
        )
        replacements += 1
    if replacements != len(selected):
        raise ValueError("tuple-forward normalization rewrite count differs")
    return tuple(normalized), {
        "schema": "uml.a64.aslref-tuple-forward-normalization/v1",
        "inventory_sha256": tuple_forward_inventory_digest(sites),
        "reachable_sites": len(sites),
        "rewrite_allowlist": [
            {"owner": owner, "callee": callee}
            for owner, callee in sorted(rewrite_keys)
        ],
        "dormant_path_proofs": [
            {"owner": owner, "callee": callee, "proof": proof}
            for (owner, callee), proof in sorted(dormant_proofs.items())
        ],
        "rewritten_tuple_sites": replacements,
        "preserved_tuple_sites": len(sites) - replacements,
        "rewritten_non_tuple_sites": 0,
        "component_arity": 2,
        "equivalence": "narrow-typed-destructure-and-reconstruct",
        "canonical_source_sha256": hashlib.sha256(
            "".join(official_texts).encode("utf-8")
        ).hexdigest(),
        "execution_source_sha256": hashlib.sha256(
            "".join(normalized).encode("utf-8")
        ).hexdigest(),
    }


def normalize_virtual_fault_projection(
    official_texts: Sequence[str],
    expected_body_sha256: str = FAULT_DOMAIN_TRANSLATE_BODY_SHA256,
) -> tuple[tuple[str, ...], dict[str, object]]:
    owners: list[tuple[int, AslDefinitionEntry]] = []
    for source_index, text in enumerate(official_texts):
        owners.extend(
            (source_index, entry)
            for entry in asl_definition_entries(text)
            if entry.name == "AArch64_TranslateAddress"
        )
    if len(owners) != 1:
        raise ValueError("expected one official AArch64 translation boundary")
    source_index, owner = owners[0]
    canonical_digest = hashlib.sha256(owner.body.encode("utf-8")).hexdigest()
    if canonical_digest != expected_body_sha256:
        raise ValueError("official AArch64 translation boundary context drifted")
    marker = (
        "    result.vaddress = ZeroExtend{64}(va);\n\n"
        "    return result;"
    )
    if owner.body.count(marker) != 1:
        raise ValueError("official AArch64 translation return marker drifted")
    projection = "\n".join(
        (
            "    result.vaddress = ZeroExtend{64}(va);",
            "",
            "    if !IsFault(result) then",
            "        let projected_fault : FaultRecord =",
            "            A64Oracle_ProjectVirtualTranslationFault(",
            "                va, size, accdesc);",
            "        if projected_fault.statuscode != Fault_None then",
            "            result.fault = projected_fault;",
            "            A64Oracle_RecordProjectedVirtualFault(",
            "                va, size, accdesc, projected_fault);",
            "        end;",
            "    end;",
            "",
            "    return result;",
        )
    )
    replacement_start = owner.start + owner.body.index(marker)
    replacement_end = replacement_start + len(marker)
    source = official_texts[source_index]
    if source[replacement_start:replacement_end] != marker:
        raise ValueError("official AArch64 translation source offset drifted")
    normalized = list(official_texts)
    normalized[source_index] = (
        source[:replacement_start] + projection + source[replacement_end:]
    )
    return tuple(normalized), {
        "schema": "uml.a64.aslref-virtual-fault-projection/v1",
        "official_function": owner.name,
        "official_body_sha256": canonical_digest,
        "injection_sha256": hashlib.sha256(
            projection.encode("utf-8")
        ).hexdigest(),
        "rewritten_functions": 1,
        "mapping": {
            "unmapped-virtual": {
                "domain": "virtual",
                "status": "Fault_Translation",
                "consumer": "AArch64_Abort(FaultRecord)",
            },
            "denied-virtual": {
                "domain": "virtual",
                "status": "Fault_Permission",
                "consumer": "AArch64_Abort(FaultRecord)",
            },
            "physical-memory": {
                "domain": "physical",
                "status": "external-or-parity-only",
                "consumer": "HandleExternalAbort(PhysMemRetStatus)",
            },
        },
    }


def expected_virtual_fault_observation(
    *, status: str, address: int, write: bool
) -> dict[str, object]:
    if status not in {"Fault_Translation", "Fault_Permission"}:
        raise ValueError("virtual projection status is not translation/permission")
    if not 0 <= address < 1 << 64:
        raise ValueError("virtual projection address exceeds bits(64)")
    return {
        "domain": "virtual",
        "status": status,
        "address": address,
        "write": write,
        "consumer": "AArch64_Abort(FaultRecord)",
    }


def expected_physical_fault_observation(
    *, address: int, write: bool
) -> dict[str, object]:
    if not 0 <= address < 1 << 64:
        raise ValueError("physical projection address exceeds bits(64)")
    return {
        "domain": "physical",
        "status": "Fault_SyncExternal",
        "address": address,
        "write": write,
        "consumer": "HandleExternalAbort(PhysMemRetStatus)",
    }


def require_virtual_fault_observation(
    observed: Mapping[str, object], expected: Mapping[str, object]
) -> None:
    if dict(observed) != dict(expected):
        raise ValueError("virtual fault domain/status/write/address differs")


def verify_fault_domain_model(
    official_texts: Sequence[str], adapter_text: str
) -> dict[str, object]:
    adapter_definitions = asl_definition_bodies((adapter_text,))
    physical_names = (
        "A64Oracle_EffectivePhysicalMemoryStatus",
        "A64Oracle_PhysicalMemoryStatus",
        "A64Oracle_PhysicalAccessStatus",
        "PhysMemRead",
        "PhysMemTagRead",
        "PhysMemTagWrite",
        "PhysMemWrite",
    )
    virtual_names = (
        "A64Oracle_VirtualAccessStatus",
        "A64Oracle_ProjectVirtualTranslationFault",
        "A64Oracle_RecordProjectedVirtualFault",
        "AArch64_MemTag",
        "Mem",
    )

    def pinned_bodies(
        definitions: Mapping[str, Sequence[str]], names: Sequence[str]
    ) -> tuple[list[str], str]:
        lines: list[str] = []
        for name in names:
            bodies = definitions.get(name, ())
            if len(bodies) != 1:
                raise ValueError(f"fault-domain function {name} is not unique")
            lines.append(
                f"{name}|{hashlib.sha256(bodies[0].encode('utf-8')).hexdigest()}\n"
            )
        return lines, hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()

    physical_lines, physical_digest = pinned_bodies(
        adapter_definitions, physical_names
    )
    if physical_digest != FAULT_DOMAIN_PHYSICAL_CALLBACK_SHA256:
        raise ValueError("physical callback fault-domain context drifted")
    physical_text = "".join(
        adapter_definitions[name][0] for name in physical_names
    )
    forbidden_physical = {
        "Fault_Alignment",
        "Fault_Translation",
        "Fault_Permission",
    }
    if any(token in physical_text for token in forbidden_physical):
        raise ValueError(
            "physical PhysMemRetStatus can reach a non-external fault class"
        )
    constructed_physical = set(
        re.findall(r"A64Oracle_MakeMemoryStatus\((Fault_[A-Za-z0-9_]+)\)", physical_text)
    )
    if constructed_physical != {"Fault_None", "Fault_SyncExternal"}:
        raise ValueError("physical status construction set differs")
    if any(
        "A64Oracle_RecordPhysicalAccess(" not in adapter_definitions[name][0]
        for name in ("PhysMemRead", "PhysMemTagRead", "PhysMemTagWrite", "PhysMemWrite")
    ):
        raise ValueError("physical callback omitted physical-domain recording")

    virtual_lines, virtual_digest = pinned_bodies(
        adapter_definitions, virtual_names
    )
    if virtual_digest != FAULT_DOMAIN_VIRTUAL_CALLBACK_SHA256:
        raise ValueError("virtual callback fault-domain context drifted")
    projection_body = adapter_definitions[
        "A64Oracle_ProjectVirtualTranslationFault"
    ][0]
    for required in (
        "Fault_Translation",
        "Fault_Permission",
        "fault.vaddress",
        "fault.write",
        "fault.level = 0",
    ):
        if required not in projection_body:
            raise ValueError("virtual translation-fault projection is incomplete")
    record_body = adapter_definitions["A64Oracle_RecordProjectedVirtualFault"][0]
    if (
        "A64Oracle_RecordVirtualAccess(" not in record_body
        or "A64Oracle_RecordMemoryEvent(" not in record_body
    ):
        raise ValueError("projected virtual fault omitted virtual-domain recording")

    official_definitions = asl_definition_bodies(official_texts)
    mops_names = tuple(
        name
        for name in ("instr_cpyp", "instr_cpyptwn")
        if official_definitions.get(name)
    )
    if not mops_names or "instr_cpyp" not in mops_names:
        raise ValueError("official CPY fault consumer is absent")
    official_names = (
        *mops_names,
        "ExternalFault",
        "HandleExternalAbort",
        "ExternalAbort",
    )
    official_lines, official_digest = pinned_bodies(
        official_definitions, official_names
    )
    for name in official_names:
        digest = hashlib.sha256(
            official_definitions[name][0].encode("utf-8")
        ).hexdigest()
        if digest != FAULT_DOMAIN_OFFICIAL_BODY_SHA256[name]:
            raise ValueError(
                f"official fault-consumer branch context drifted for {name}"
            )
    if (
        len(official_names) == len(FAULT_DOMAIN_OFFICIAL_BODY_SHA256)
        and official_digest != FAULT_DOMAIN_OFFICIAL_CONSUMER_SHA256
    ):
        raise ValueError("complete official fault-consumer inventory drifted")
    for name in mops_names:
        body = official_definitions[name][0]
        for predicate in (
            "if IsFault(memaddrdesc) then",
            "AArch64_Abort(memaddrdesc.fault);",
            "HandleExternalAbort(memstatus, iswrite, memaddrdesc, B, accdesc);",
        ):
            if predicate not in body:
                raise ValueError("official MOPS fault branch predicate drifted")
    for name in ("ExternalFault", "HandleExternalAbort"):
        body = official_definitions[name][0]
        if "Fault_SyncExternal" not in body or "Fault_AsyncExternal" not in body:
            raise ValueError("official external-status allowlist drifted")
    return {
        "schema": "uml.a64.aslref-fault-domain-model/v1",
        "virtual_callback_sha256": virtual_digest,
        "virtual_callback_functions": len(virtual_lines),
        "physical_callback_sha256": physical_digest,
        "physical_callback_functions": len(physical_lines),
        "official_consumer_sha256": official_digest,
        "complete_official_consumer_sha256": (
            FAULT_DOMAIN_OFFICIAL_CONSUMER_SHA256
        ),
        "complete_official_consumer_inventory": (
            len(official_names) == len(FAULT_DOMAIN_OFFICIAL_BODY_SHA256)
        ),
        "official_consumer_functions": len(official_lines),
        "virtual_statuses": ["Fault_Translation", "Fault_Permission"],
        "physical_statuses": ["Fault_None", "Fault_SyncExternal"],
        "physical_callbacks": list(physical_names[-4:]),
        "official_mops_consumers": list(mops_names),
    }


def implementation_defined_adapter_digest(text: str) -> str:
    definitions = asl_definition_bodies((text,))
    missing = IMPLEMENTATION_DEFINED_ADAPTERS - definitions.keys()
    if missing:
        raise ValueError(
            "reviewed implementation-defined adapter definitions are absent: "
            + ", ".join(sorted(missing))
        )
    canonical = "".join(
        f"{name}|{hashlib.sha256(body.encode('utf-8')).hexdigest()}\n"
        for name in sorted(IMPLEMENTATION_DEFINED_ADAPTERS)
        for body in definitions[name]
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def conservative_reachable_functions(
    texts: Iterable[str], roots: Iterable[str]
) -> tuple[frozenset[str], dict[str, list[str]]]:
    definitions = asl_definition_bodies(texts)
    pending = list(roots)
    reachable: set[str] = set()
    while pending:
        name = pending.pop()
        if name in reachable:
            continue
        bodies = definitions.get(name)
        if not bodies:
            continue
        reachable.add(name)
        # Every overload is traversed.  Lexical call extraction deliberately
        # over-approximates branch/feature dispatch: ambiguity is projection,
        # never an unreachable proof.
        for body in bodies:
            for called in _ASL_CALL.findall(body):
                if called in definitions and called not in reachable:
                    pending.append(called)
    return frozenset(reachable), definitions


def verify_impdef_completeness(
    shared_text: str,
    all_texts: Iterable[str],
    roots: Iterable[str] = ("main",),
) -> dict[str, object]:
    all_sources = tuple(all_texts)
    inventory = impdef_inventory(shared_text)
    unique_inventory = frozenset(inventory)
    if len(inventory) != IMPDEF_INVENTORY_CALL_SITES:
        raise ValueError("pinned ImpDef call-site inventory count drifted")
    if len(unique_inventory) != IMPDEF_INVENTORY_UNIQUE:
        raise ValueError("pinned unique ImpDef inventory count drifted")
    digest = impdef_inventory_digest(inventory)
    if digest != IMPDEF_INVENTORY_SHA256:
        raise ValueError("pinned ImpDef inventory digest drifted")
    require_impdef_context_digest(shared_text, IMPDEF_CONTEXT_SHA256)
    context_digest = impdef_context_digest(shared_text)

    outside_calls = [
        hook
        for text in all_sources
        if text != shared_text
        for hook in impdef_inventory(text)
    ]
    if outside_calls:
        raise ValueError("ImpDef call exists outside the pinned official shared source")

    adapter_sources = [
        text
        for text in all_sources
        if IMPLEMENTATION_DEFINED_ADAPTERS
        <= asl_definition_bodies((text,)).keys()
    ]
    if len(adapter_sources) != 1:
        raise ValueError(
            "expected exactly one reviewed implementation-defined adapter source"
        )
    adapter_digest = implementation_defined_adapter_digest(adapter_sources[0])
    if adapter_digest != IMPLEMENTATION_DEFINED_ADAPTER_SHA256:
        raise ValueError("reviewed implementation-defined adapter digest drifted")
    value_policies = verify_impdef_value_policies(
        adapter_sources[0], IMPDEF_PROJECTION_BY_KEY
    )
    cpy_post_size = verify_cpy_post_size_policy(
        shared_text, adapter_sources[0]
    )

    reachable, reachable_hooks = require_reachable_impdef_projections(
        all_sources, IMPDEF_PROJECTION_BY_KEY, roots
    )

    projected_keys = {
        (kind, description) for kind, _width, description in reachable_hooks
    }
    stale = sorted(
        key for key in IMPDEF_PROJECTION_BY_KEY if key not in projected_keys
    )
    if stale:
        raise ValueError(
            "reviewed ImpDef projection is not conservatively reachable: "
            + ", ".join(f"ImpDef{kind}({description!r})" for kind, description in stale)
        )
    unreachable_hooks = unique_inventory - reachable_hooks
    return {
        "inventory_sha256": digest,
        "context_sha256": context_digest,
        "inventory_call_sites": len(inventory),
        "inventory_unique": len(unique_inventory),
        "reachable_functions": len(reachable),
        "reviewed_projections": len(reachable_hooks),
        "intentional_unreachable": len(unreachable_hooks),
        "external_impdef_call_sites": 0,
        "adapter_sha256": adapter_digest,
        "value_domain_policies": value_policies,
        "cpy_post_size_policy": cpy_post_size,
        "unreachable_proof": "absent from conservative all-overload main() call closure",
    }


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--architecture-root", type=Path, required=True)
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--bundler", type=Path, required=True)
    parser.add_argument("--features-json", type=Path, required=True)
    parser.add_argument(
        "--capture-directory",
        type=Path,
        default=(
            repository
            / ".build/presets/macos-unit-16k/instruction-evidence"
        ),
    )
    parser.add_argument(
        "--closure-capture-directory",
        type=Path,
        default=(
            repository
            / ".build/presets/macos-unit-16k/instruction-evidence"
        ),
        help=(
            "reviewed strict-four capture seed used only to retain the full "
            "official policy/interface closure in full-extent mode"
        ),
    )
    parser.add_argument(
        "--input-jsonl",
        action="append",
        type=Path,
        default=[],
        help=(
            "execute every distinct contract in explicitly frozen v4 input "
            "sidecars; the records must be unique and exactly match --count"
        ),
    )
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_required_variants.tsv"
        ),
    )
    parser.add_argument(
        "--interface",
        type=Path,
        default=Path(__file__).with_name("a64_aslref_user_state_interface.asl"),
    )
    parser.add_argument(
        "--observer",
        type=Path,
        default=Path(__file__).with_name("a64_aslref_execution_observer.asl"),
    )
    parser.add_argument(
        "--max-contracts", type=int, default=MAX_CONTRACTS
    )
    parser.add_argument(
        "--max-encoded-bytes", type=int, default=MAX_ENCODED_BYTES
    )
    parser.add_argument(
        "--max-artifact-bytes", type=int, default=MAX_ARTIFACT_BYTES
    )
    parser.add_argument(
        "--max-artifact-files", type=int, default=MAX_ARTIFACT_FILES
    )
    parser.add_argument("--max-log-bytes", type=int, default=MAX_LOG_BYTES)
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    parser.add_argument("--max-rss-kib", type=int, default=MAX_RSS_KIB)
    parser.add_argument(
        "--diagnostic-drop-maximum",
        action="store_true",
        help=(
            "execute only the three non-maximum contracts from the exact "
            "four-contract batch while retaining its official source closure"
        ),
    )
    parser.add_argument(
        "--diagnostic-maximum-phase",
        choices=(
            "full",
            "initialize-only",
            "initialize-expected-compare",
            "execute-without-memory-compare",
        ),
        default="full",
        help="isolate the maximum-backing contract's bounded ASL phase",
    )
    parser.add_argument(
        "--full-extent-page-size",
        type=int,
        choices=tuple(sorted(FULL_EXTENT_CAPTURE_PROFILES)),
        help=(
            "execute the separately gated, exactly pinned 65536-byte MOPS "
            "transfer captured for this guest page size; requires --count 1"
        ),
    )
    parser.add_argument("--generated-directory", type=Path)
    parser.add_argument(
        "--minimum-generated-free-gib",
        type=float,
        default=DEFAULT_MINIMUM_GENERATED_FREE_GIB,
    )
    return parser.parse_args()


def bits(value: int, width: int) -> str:
    if value < 0 or value >= 1 << width:
        raise ValueError(f"value {value} exceeds bits({width})")
    return "'" + f"{value:0{width}b}" + "'"


def boolean(value: object) -> int:
    if not isinstance(value, bool):
        raise ValueError("wire boolean is not boolean")
    return int(value)


def asl_boolean(value: object) -> str:
    return "TRUE" if bool(value) else "FALSE"


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def feature_expression(expression: Mapping[str, object]) -> str:
    kind = expression.get("_type")
    if kind == "AST.Identifier":
        return str(expression["value"])
    if kind == "AST.UnaryOp":
        operand = expression["expr"]
        if not isinstance(operand, Mapping):
            raise ValueError("pinned feature unary operand shape differs")
        return f"({expression['op']}{feature_expression(operand)})"
    if kind == "AST.BinaryOp":
        left = expression["left"]
        right = expression["right"]
        if not isinstance(left, Mapping) or not isinstance(right, Mapping):
            raise ValueError("pinned feature binary operand shape differs")
        return (
            f"({feature_expression(left)} {expression['op']} "
            f"{feature_expression(right)})"
        )
    return ""


def verify_pac_dependency(
    architecture_root: Path, features_json: Path
) -> dict[str, str]:
    if features_json.name != "Features.json" or file_sha256(
        features_json
    ) != FEATURES_SHA256:
        raise ValueError("pinned 2026-06 feature model name/digest drifted")
    feature_model = json.loads(features_json.read_text(encoding="utf-8"))
    metadata = feature_model.get("_meta", {}).get("version", {})
    if metadata.get("ref") != "2026-06_rel" or metadata.get("build") != "818":
        raise ValueError("pinned feature model release metadata drifted")
    parameters = feature_model.get("parameters")
    if not isinstance(parameters, Sequence):
        raise ValueError("pinned feature parameter table shape differs")
    pacimp = next(
        (
            item
            for item in parameters
            if isinstance(item, Mapping) and item.get("name") == PAC_ALGORITHM
        ),
        None,
    )
    if not isinstance(pacimp, Mapping) or not isinstance(
        pacimp.get("constraints"), Sequence
    ):
        raise ValueError("pinned FEAT_PACIMP definition is absent")
    constraints = {
        feature_expression(item)
        for item in pacimp["constraints"]
        if isinstance(item, Mapping)
    }
    required_constraints = {
        "(FEAT_PACIMP ==> FEAT_PAuth)",
        "(FEAT_PACIMP ==> (!FEAT_PACQARMA3))",
        "(FEAT_PACIMP ==> (!FEAT_PACQARMA5))",
    }
    if not required_constraints <= constraints:
        raise ValueError("pinned FEAT_PACIMP implication/exclusion rules drifted")

    shared_path = architecture_root / "shared_pseudocode.xml"
    root = ET.parse(shared_path).getroot()
    compute = next(
        (
            node
            for node in root.iter("ps")
            if node.get("name")
            == "aarch64/functions/pac/computepac/ComputePAC"
        ),
        None,
    )
    if compute is None:
        raise ValueError("pinned official ComputePAC definition is absent")
    compute_text = "".join(compute.itertext())
    required_fragments = (
        "IsFeatureImplemented(FEAT_PACIMP)",
        "IsFeatureImplemented(FEAT_PACQARMA3)",
        "IsFeatureImplemented(FEAT_PACQARMA5)",
        "unreachable;",
    )
    if any(fragment not in compute_text for fragment in required_fragments):
        raise ValueError("pinned official ComputePAC dispatch contract drifted")
    if not (
        compute_text.index(required_fragments[0])
        < compute_text.index(required_fragments[1])
        < compute_text.index(required_fragments[2])
        < compute_text.index(required_fragments[3])
    ):
        raise ValueError("pinned official ComputePAC dispatch order drifted")
    return {
        "feature_model_sha256": FEATURES_SHA256,
        "compute_pac_sha256": hashlib.sha256(
            compute_text.encode("utf-8")
        ).hexdigest(),
        "algorithm": PAC_ALGORITHM,
        "normalization_obligation": "bit3-engine-private-state-and-scratch",
        "relation": PAC_RELATION,
    }


def resolve_pac_algorithm(
    native_features: set[str], relation: str | None, algorithm: str | None
) -> str | None:
    if "pauth" not in native_features:
        return None
    if relation != PAC_RELATION:
        raise ValueError("PAuth execution lacks a permitted callback relation")
    if algorithm != PAC_ALGORITHM:
        raise ValueError(
            "PAuth execution lacks one pinned concrete PAC algorithm"
        )
    return algorithm


def state_values(state: Mapping[str, object]) -> dict[str, int]:
    nzcv = int(str(state["nzcv"]), 16)
    if nzcv & 0x0FFF_FFFF:
        raise ValueError("NZCV storage bits are not representable")
    pstate = state["pstate"]
    mte = state["mte"]
    checked = state["checked_arithmetic"]
    gcs = state["guarded_stack"]
    exclusive = state["exclusive_monitor"]
    keys = state["signing_keys"]
    if not all(
        isinstance(item, Mapping)
        for item in (pstate, mte, checked, gcs, exclusive)
    ) or not isinstance(keys, Sequence):
        raise ValueError("state object shape differs")
    result: dict[str, int] = {
        **{
            f"x{index}": int(str(value), 16)
            for index, value in enumerate(state["x"])  # type: ignore[arg-type]
        },
        "sp": int(str(state["sp"]), 16),
        "pc": int(str(state["pc"]), 16),
        "nzcv": nzcv >> 28,
        **{
            f"v{index}": int.from_bytes(bytes.fromhex(str(value)), "little")
            for index, value in enumerate(  # type: ignore[arg-type]
                state["vectors"]
            )
        },
        "fpcr": int(str(state["fpcr"]), 16),
        "fpsr": int(str(state["fpsr"]), 16),
        "fpmr": int(str(state["fpmr"]), 16),
        "tls": int(str(state["tls"]), 16),
        "pstate_ssbs": boolean(pstate["ssbs"]),
        "pstate_dit": boolean(pstate["dit"]),
        "pstate_tco": boolean(pstate["tco"]),
        "pstate_sm": boolean(pstate["sm"]),
        "pstate_za": boolean(pstate["za"]),
        "pstate_daif_access": boolean(pstate["daif_access"]),
        "pstate_daif": int(str(pstate["daif"]), 16),
        "mte_enabled": boolean(mte["enabled"]),
        "mte_exclude": int(str(mte["exclude"]), 16),
        "mte_random_tags": boolean(mte["random_tags"]),
        "mte_seed": int(str(mte["seed"]), 16),
        "mte_tag": int(str(mte["tag"]), 16),
        "cpa_add_enabled": boolean(checked["add_enabled"]),
        "cpa_multiply_enabled": boolean(checked["multiply_enabled"]),
        "accdata": int(str(checked["accdata"]), 16),
        "gcs_pointer": int(str(gcs["pointer"]), 16),
        "gcs_implemented": boolean(gcs["implemented"]),
        "gcs_push_enabled": boolean(gcs["push_enabled"]),
        "gcs_store_enabled": boolean(gcs["store_enabled"]),
        "signing_enabled": boolean(state["signing_enabled"]),
        "exclusive_address": int(str(exclusive["address"]), 16),
        "exclusive_key": int(str(exclusive["key"]), 16),
        "exclusive_value": int(str(exclusive["value"]), 16),
        "exclusive_value2": int(str(exclusive["value2"]), 16),
        "exclusive_version": int(str(exclusive["version"]), 16),
        "exclusive_size": int(str(exclusive["size"]), 16),
    }
    result["exclusive_valid"] = int(
        result["exclusive_address"] != (1 << 64) - 1
        and result["exclusive_size"] != 0
    )
    if len(keys) != 5:
        raise ValueError("signing key count differs")
    for index, key in enumerate(keys):
        if not isinstance(key, Mapping):
            raise ValueError("signing key shape differs")
        result[f"key{index}_low"] = int(str(key["low"]), 16)
        result[f"key{index}_high"] = int(str(key["high"]), 16)
    return result


def table_function(
    name: str, width: int, payloads: Sequence[int]
) -> list[str]:
    lines = [f"func {name}(index : integer) => bits({width})", "begin"]
    lines.append("    case index of")
    for index, payload in enumerate(payloads):
        lines.append(f"        when {index} => return {bits(payload, width)};")
    lines.extend(
        [
            "        otherwise => assert FALSE;",
            "    end;",
            f"    return Zeros{{{width}}};",
            "end;",
            "",
        ]
    )
    return lines


def field(name: str) -> Field:
    return STATE_LAYOUT.by_name[name]


def contract_field(name: str) -> Field:
    return CONTRACT_LAYOUT.by_name[name]


def state_source(state_payloads: Sequence[int]) -> list[str]:
    lines = table_function(
        "A64Batch_StatePayload", STATE_LAYOUT.width, state_payloads
    )
    lines.extend(
        [
            "func A64Batch_InstallState(index : integer)",
            "begin",
            "    let state : bits(%d) = A64Batch_StatePayload(index);"
            % STATE_LAYOUT.width,
        ]
    )
    x0 = field("x0").offset
    v0 = field("v0").offset
    lines.extend(
        [
            "    for register = 0 to 30 do",
            f"        A64Oracle_SetX(register, state[{x0} + register * 64 +: 64]);",
            "    end;",
            f"    SP{{64}}() = state[{field('sp').offset} +: 64];",
            f"    A64Oracle_SetPC(state[{field('pc').offset} +: 64]);",
            "    A64Oracle_SetArchitecturalNZCV("
            f"state[{field('nzcv').offset} +: 4]);",
            "    for vector = 0 to 31 do",
            f"        A64Oracle_SetV128(vector, state[{v0} + vector * 128 +: 128]);",
            "    end;",
            "    A64Oracle_InitializeProjectedFPSystemState("
            f"state[{field('fpcr').offset} +: 32], "
            f"state[{field('fpsr').offset} +: 32], "
            f"state[{field('fpmr').offset} +: 64]);",
            "    A64Oracle_InitializeProjectedSystemState("
            f"state[{field('tls').offset} +: 64], "
            f"state[{field('pstate_ssbs').offset}] == '1', "
            f"state[{field('pstate_dit').offset}] == '1', "
            f"state[{field('pstate_tco').offset}] == '1', "
            f"state[{field('pstate_sm').offset}] == '1', "
            f"state[{field('pstate_za').offset}] == '1', "
            f"state[{field('pstate_daif_access').offset}] == '1', "
            f"state[{field('pstate_daif').offset} +: 8], "
            f"state[{field('mte_enabled').offset}] == '1', "
            f"state[{field('mte_exclude').offset} +: 16], "
            f"state[{field('mte_random_tags').offset}] == '1', "
            f"state[{field('mte_seed').offset} +: 16], "
            f"state[{field('mte_tag').offset} +: 8], "
            f"state[{field('cpa_add_enabled').offset}] == '1', "
            f"state[{field('cpa_multiply_enabled').offset}] == '1', "
            f"state[{field('accdata').offset} +: 32], "
            f"state[{field('gcs_pointer').offset} +: 64], "
            f"state[{field('gcs_implemented').offset}] == '1', "
            f"state[{field('gcs_push_enabled').offset}] == '1', "
            f"state[{field('gcs_store_enabled').offset}] == '1', "
            f"state[{field('signing_enabled').offset}] == '1');",
        ]
    )
    for key in range(5):
        lines.append(
            f"    A64Oracle_SetProjectedSigningKey({key}, "
            f"state[{field(f'key{key}_low').offset} +: 64], "
            f"state[{field(f'key{key}_high').offset} +: 64]);"
        )
    lines.extend(
        [
            "    A64Oracle_SetLocalExclusiveMonitorState("
            f"state[{field('exclusive_valid').offset}] == '1', "
            "PAS_NonSecure, "
            f"state[{field('exclusive_address').offset} +: 64], 0, "
            f"UInt(state[{field('exclusive_size').offset} +: 8]), "
            f"state[{field('exclusive_key').offset} +: 64], "
            f"state[{field('exclusive_value').offset} +: 64], "
            f"state[{field('exclusive_value2').offset} +: 64], "
            f"state[{field('exclusive_version').offset} +: 64]);",
            "    A64Oracle_SetCounterPolicy("
            f"{bits(COUNTER_FREQUENCY, 64)}, "
            f"{bits(COUNTER_INITIAL, 64)}, {bits(COUNTER_STEP, 64)});",
            "end;",
            "",
        ]
    )
    return lines


def comparison(expression: str) -> str:
    return f"    matches = matches && {expression};"


def state_match_source() -> list[str]:
    x0 = field("x0").offset
    v0 = field("v0").offset
    lines = [
        "func A64Batch_StateMatches(expected_index : integer,",
        "                           initial_index : integer,",
        "                           relation : integer,",
        "                           evidence : bits(32)) => boolean",
        "begin",
        "    let expected : bits(%d) = A64Batch_StatePayload(expected_index);"
        % STATE_LAYOUT.width,
        "    let initial : bits(%d) = A64Batch_StatePayload(initial_index);"
        % STATE_LAYOUT.width,
        "    var matches : boolean = TRUE;",
        "    let destination : integer = UInt(evidence[4:0]);",
        "    for register = 0 to 30 do",
        "        if relation == 1 && register == destination then",
        "            let observed : bits(64) = A64Oracle_GetX(register);",
        f"            let before : bits(64) = initial[{x0} + register * 64 +: 64];",
        "            matches = matches && observed[46:0] == before[46:0] &&",
        "                observed[55] == before[63];",
        "        else",
        f"            matches = matches && A64Oracle_GetX(register) == expected[{x0} + register * 64 +: 64];",
        "        end;",
        "    end;",
    ]
    lines.extend(
        [
            comparison(
                f"SP{{64}}() == expected[{field('sp').offset} +: 64]"
            ),
            comparison(
                "A64Oracle_GetPC() == "
                f"expected[{field('pc').offset} +: 64]"
            ),
            comparison(
                "A64Oracle_GetArchitecturalNZCV() == "
                f"expected[{field('nzcv').offset} +: 4]"
            ),
            "    for vector = 0 to 31 do",
            f"        matches = matches && A64Oracle_GetV128(vector) == expected[{v0} + vector * 128 +: 128];",
            "    end;",
        ]
    )
    bit_fields = (
        ("A64Oracle_GetProjectedPSTATESSBS()", "pstate_ssbs"),
        ("A64Oracle_GetProjectedPSTATEDIT()", "pstate_dit"),
        ("A64Oracle_GetProjectedPSTATETCO()", "pstate_tco"),
        ("A64Oracle_GetProjectedPSTATESM()", "pstate_sm"),
        ("A64Oracle_GetProjectedPSTATEZA()", "pstate_za"),
        (
            "A64Oracle_GetProjectedPSTATEDAIFAccess()",
            "pstate_daif_access",
        ),
        ("A64Oracle_GetProjectedMTEEnabled()", "mte_enabled"),
        ("A64Oracle_GetProjectedMTERandomTags()", "mte_random_tags"),
        ("A64Oracle_GetProjectedCPAAddEnabled()", "cpa_add_enabled"),
        (
            "A64Oracle_GetProjectedCPAMultiplyEnabled()",
            "cpa_multiply_enabled",
        ),
        ("A64Oracle_GetProjectedGCSImplemented()", "gcs_implemented"),
        ("A64Oracle_GetProjectedGCSPushEnabled()", "gcs_push_enabled"),
        ("A64Oracle_GetProjectedGCSStoreEnabled()", "gcs_store_enabled"),
        ("A64Oracle_GetProjectedSigningEnabled()", "signing_enabled"),
    )
    scalar_fields = (
        ("A64Oracle_GetProjectedFPCR()", "fpcr"),
        ("A64Oracle_GetProjectedFPSR()", "fpsr"),
        ("A64Oracle_GetProjectedFPMR()", "fpmr"),
        ("A64Oracle_GetProjectedThreadPointer()", "tls"),
        ("A64Oracle_GetProjectedPSTATEDAIF()", "pstate_daif"),
        ("A64Oracle_GetProjectedMTEExclude()", "mte_exclude"),
        ("A64Oracle_GetProjectedMTESeed()", "mte_seed"),
        ("A64Oracle_GetProjectedMTETag()", "mte_tag"),
        ("A64Oracle_GetProjectedACCDATA()", "accdata"),
        ("A64Oracle_GetProjectedGCSPointer()", "gcs_pointer"),
    )
    lines.append(comparison("A64Oracle_IsProjectedSystemStateInstalled()"))
    for getter, name in bit_fields:
        expected = field(name)
        lines.append(
            comparison(f"{getter} == (expected[{expected.offset}] == '1')")
        )
    for getter, name in scalar_fields:
        expected = field(name)
        lines.append(
            comparison(
                f"{getter} == expected[{expected.offset} +: {expected.width}]"
            )
        )
    for key in range(5):
        for half in ("low", "high"):
            expected = field(f"key{key}_{half}")
            getter = "Low" if half == "low" else "High"
            lines.append(
                comparison(
                    f"A64Oracle_GetProjectedSigningKey{getter}({key}) == "
                    f"expected[{expected.offset} +: 64]"
                )
            )
    exclusive_checks = (
        (
            "A64Oracle_GetLocalExclusiveMonitorValid()",
            "exclusive_valid",
            True,
        ),
        (
            "A64Oracle_GetLocalExclusiveMonitorAddress()",
            "exclusive_address",
            False,
        ),
        (
            "A64Oracle_GetLocalExclusiveMonitorKey()",
            "exclusive_key",
            False,
        ),
        (
            "A64Oracle_GetLocalExclusiveMonitorValue()",
            "exclusive_value",
            False,
        ),
        (
            "A64Oracle_GetLocalExclusiveMonitorValue2()",
            "exclusive_value2",
            False,
        ),
        (
            "A64Oracle_GetLocalExclusiveMonitorVersion()",
            "exclusive_version",
            False,
        ),
    )
    for getter, name, is_boolean in exclusive_checks:
        expected = field(name)
        right = (
            f"expected[{expected.offset}] == '1'"
            if is_boolean
            else f"expected[{expected.offset} +: {expected.width}]"
        )
        lines.append(comparison(f"{getter} == ({right})"))
    lines.extend(
        [
            comparison(
                "A64Oracle_GetLocalExclusiveMonitorPASpace() == PAS_NonSecure"
            ),
            comparison("A64Oracle_GetLocalExclusiveMonitorProcessorID() == 0"),
            comparison(
                "A64Oracle_GetLocalExclusiveMonitorSize() == "
                f"UInt(expected[{field('exclusive_size').offset} +: 8])"
            ),
            comparison(
                f"A64Oracle_GetCounterFrequency() == {bits(COUNTER_FREQUENCY, 64)}"
            ),
            comparison(
                f"A64Oracle_GetCounterNext() == {bits(COUNTER_INITIAL, 64)}"
            ),
            comparison(
                f"A64Oracle_GetCounterStep() == {bits(COUNTER_STEP, 64)}"
            ),
            comparison("A64Oracle_GetCounterReadCount() == 0"),
            comparison(
                "A64Oracle_GetProjectedNormalizationObligations() == '1111'"
            ),
            "    return matches;",
            "end;",
            "",
        ]
    )
    return lines


def chunk_values(encoded: str, element_bits: int) -> list[int]:
    raw = bytes.fromhex(encoded)
    if element_bits == 8:
        return list(raw)
    if element_bits == 4 and any(value >= 16 for value in raw):
        raise ValueError("memory tag exceeds four bits")
    return list(raw)


def packed_chunk(values: Sequence[int], element_bits: int) -> int:
    result = 0
    for index, value in enumerate(values):
        result |= value << (index * element_bits)
    return result


def compact_max_memory_descriptor(
    *,
    formula_seed: int = COMPACT_MAX_MEMORY_FORMULA_SEED,
    formula_step: int = COMPACT_MAX_MEMORY_FORMULA_STEP,
    initial_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_INITIAL_OVERRIDES
    ),
    expected_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES
    ),
) -> dict[str, object]:
    return {
        "address": f"0x{COMPACT_MAX_MEMORY_ADDRESS:016x}",
        "copy_end": COMPACT_MAX_MEMORY_COPY_END,
        "copy_source_displacement": (
            COMPACT_MAX_MEMORY_COPY_SOURCE_DISPLACEMENT
        ),
        "copy_start": COMPACT_MAX_MEMORY_COPY_START,
        "expected_sparse_overrides": [list(item) for item in expected_overrides],
        "formula": {
            "modulus": COMPACT_MAX_MEMORY_FORMULA_MODULUS,
            "seed": formula_seed,
            "step": formula_step,
        },
        "initial_sparse_overrides": [list(item) for item in initial_overrides],
        "length": COMPACT_MAX_MEMORY_LENGTH,
        "tag_length": COMPACT_MAX_MEMORY_TAG_LENGTH,
    }


def compact_max_memory_descriptor_digest(
    *,
    formula_seed: int = COMPACT_MAX_MEMORY_FORMULA_SEED,
    formula_step: int = COMPACT_MAX_MEMORY_FORMULA_STEP,
    initial_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_INITIAL_OVERRIDES
    ),
    expected_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES
    ),
) -> str:
    descriptor = compact_max_memory_descriptor(
        formula_seed=formula_seed,
        formula_step=formula_step,
        initial_overrides=initial_overrides,
        expected_overrides=expected_overrides,
    )
    encoded = json.dumps(descriptor, sort_keys=True, separators=(",", ":")) + "\n"
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def _apply_compact_memory_overrides(
    payload: bytearray,
    overrides: Sequence[tuple[int, int]],
    label: str,
) -> None:
    seen: set[int] = set()
    for offset, value in overrides:
        if offset in seen:
            raise ValueError(f"duplicate {label} sparse override")
        if not 0 <= offset < len(payload) or not 0 <= value <= 0xFF:
            raise ValueError(f"out-of-domain {label} sparse override")
        seen.add(offset)
        payload[offset] = value


def generated_compact_max_memory(
    *,
    formula_seed: int = COMPACT_MAX_MEMORY_FORMULA_SEED,
    formula_step: int = COMPACT_MAX_MEMORY_FORMULA_STEP,
    initial_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_INITIAL_OVERRIDES
    ),
    expected_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES
    ),
) -> tuple[bytes, bytes]:
    if not 0 <= formula_seed <= 0xFF or not 0 <= formula_step <= 0xFF:
        raise ValueError("compact-memory formula constant exceeds one byte")
    initial = bytearray(
        (
            formula_seed
            + formula_step * (offset % COMPACT_MAX_MEMORY_FORMULA_MODULUS)
        )
        % COMPACT_MAX_MEMORY_FORMULA_MODULUS
        for offset in range(COMPACT_MAX_MEMORY_LENGTH)
    )
    _apply_compact_memory_overrides(initial, initial_overrides, "initial")
    expected = bytearray(initial)
    expected[
        COMPACT_MAX_MEMORY_COPY_START : COMPACT_MAX_MEMORY_COPY_END
    ] = initial[
        COMPACT_MAX_MEMORY_COPY_START
        - COMPACT_MAX_MEMORY_COPY_SOURCE_DISPLACEMENT :
        COMPACT_MAX_MEMORY_COPY_END
        - COMPACT_MAX_MEMORY_COPY_SOURCE_DISPLACEMENT
    ]
    _apply_compact_memory_overrides(expected, expected_overrides, "expected")
    return bytes(initial), bytes(expected)


def verify_compact_max_memory(
    regions: Sequence[Mapping[str, object]],
    *,
    formula_seed: int = COMPACT_MAX_MEMORY_FORMULA_SEED,
    formula_step: int = COMPACT_MAX_MEMORY_FORMULA_STEP,
    initial_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_INITIAL_OVERRIDES
    ),
    expected_overrides: Sequence[tuple[int, int]] = (
        COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES
    ),
) -> dict[str, object]:
    descriptor_digest = compact_max_memory_descriptor_digest(
        formula_seed=formula_seed,
        formula_step=formula_step,
        initial_overrides=initial_overrides,
        expected_overrides=expected_overrides,
    )
    if descriptor_digest != COMPACT_MAX_MEMORY_DESCRIPTOR_SHA256:
        raise ValueError("compact maximum-memory formula descriptor drifted")
    if len(regions) != 1:
        raise ValueError("compact maximum-memory contract is not one region")
    region = regions[0]
    if (
        int(str(region["address"]), 16) != COMPACT_MAX_MEMORY_ADDRESS
        or int(str(region["size"]), 16) != COMPACT_MAX_MEMORY_LENGTH
    ):
        raise ValueError("compact maximum-memory extent or address drifted")
    generated_initial, generated_expected = generated_compact_max_memory(
        formula_seed=formula_seed,
        formula_step=formula_step,
        initial_overrides=initial_overrides,
        expected_overrides=expected_overrides,
    )
    initial = bytes.fromhex(str(region["initial_bytes"]))
    expected = bytes.fromhex(str(region["expected_bytes"]))
    initial_tags = bytes.fromhex(str(region["initial_tags"]))
    expected_tags = bytes.fromhex(str(region["expected_tags"]))
    payloads = (
        ("initial", initial, generated_initial, COMPACT_MAX_MEMORY_INITIAL_SHA256),
        (
            "expected",
            expected,
            generated_expected,
            COMPACT_MAX_MEMORY_EXPECTED_SHA256,
        ),
    )
    for label, captured, generated, pinned_digest in payloads:
        if len(captured) != COMPACT_MAX_MEMORY_LENGTH:
            raise ValueError(f"compact maximum-memory {label} length drifted")
        captured_digest = hashlib.sha256(captured).hexdigest()
        if captured_digest != pinned_digest or generated != captured:
            raise ValueError(
                f"compact maximum-memory {label} generated byte differs"
            )
    for label, tags in (("initial", initial_tags), ("expected", expected_tags)):
        if (
            len(tags) != COMPACT_MAX_MEMORY_TAG_LENGTH
            or any(tags)
            or hashlib.sha256(tags).hexdigest()
            != COMPACT_MAX_MEMORY_TAGS_SHA256
        ):
            raise ValueError(f"compact maximum-memory {label} tags drifted")
    return {
        "schema": "uml.a64.aslref-compact-memory/v1",
        "descriptor_sha256": descriptor_digest,
        "length": COMPACT_MAX_MEMORY_LENGTH,
        "initial_sha256": COMPACT_MAX_MEMORY_INITIAL_SHA256,
        "expected_sha256": COMPACT_MAX_MEMORY_EXPECTED_SHA256,
        "tag_length": COMPACT_MAX_MEMORY_TAG_LENGTH,
        "initial_tags_sha256": COMPACT_MAX_MEMORY_TAGS_SHA256,
        "expected_tags_sha256": COMPACT_MAX_MEMORY_TAGS_SHA256,
        "initial_sparse_overrides": len(initial_overrides),
        "expected_sparse_overrides": len(expected_overrides),
        "initialized_bytes": COMPACT_MAX_MEMORY_LENGTH,
        "compared_bytes": COMPACT_MAX_MEMORY_LENGTH,
        "initialized_tags": COMPACT_MAX_MEMORY_TAG_LENGTH,
        "compared_tags": COMPACT_MAX_MEMORY_TAG_LENGTH,
    }


def compact_memory_asl_source(
    memory_index: int, spec: CompactMemoryAslSpec
) -> list[str]:
    initial_name = f"A64Batch_CompactMemory{memory_index}InitialByte"
    expected_name = f"A64Batch_CompactMemory{memory_index}ExpectedByte"
    lines = [
        f"func {initial_name}(offset : integer) => bits(8)",
        "begin",
        f"    assert offset >= 0 && offset < {spec.length};",
    ]
    for offset, value in spec.initial_overrides:
        lines.append(
            f"    if offset == {offset} then return {bits(value, 8)}; end;"
        )
    lines.extend(
        [
            "    let generated : integer = ",
            f"        ({spec.formula_seed} + ",
            f"         {spec.formula_step} * ",
            f"         (offset MOD {spec.formula_modulus})) MOD ",
            f"        {spec.formula_modulus};",
            "    return generated[7:0];",
            "end;",
            "",
            f"func {expected_name}(offset : integer) => bits(8)",
            "begin",
            f"    assert offset >= 0 && offset < {spec.length};",
        ]
    )
    for offset, value in spec.expected_overrides:
        lines.append(
            f"    if offset == {offset} then return {bits(value, 8)}; end;"
        )
    lines.extend(
        [
            "    var source_offset : integer = offset;",
            f"    if offset >= {spec.copy_start} &&",
            f"       offset < {spec.copy_end} then",
            "        source_offset = offset - ",
            f"                        {spec.copy_source_displacement};",
            "    end;",
            f"    return {initial_name}(source_offset);",
            "end;",
            "",
        ]
    )
    for suffix, byte_function in (
        ("InitialChunk", initial_name),
        ("ExpectedChunk", expected_name),
    ):
        lines.extend(
            [
                f"func A64Batch_CompactMemory{memory_index}{suffix}(",
                "    chunk : integer) => bits(128)",
                "begin",
                f"    assert chunk >= 0 && chunk < {spec.length // 16};",
                "    var value : bits(128) = Zeros{128};",
                "    for byte = 0 to 15 do",
                f"        value[byte * 8 +: 8] = {byte_function}(chunk * 16 + byte);",
                "    end;",
                "    return value;",
                "end;",
                "",
            ]
        )
    return lines


def compact_memory_asl_spec(
    regions: Sequence[Mapping[str, object]],
) -> tuple[CompactMemoryAslSpec, dict[str, object]] | None:
    if len(regions) != 1:
        return None
    length = int(str(regions[0]["size"]), 16)
    if length == COMPACT_MAX_MEMORY_LENGTH:
        return (
            CompactMemoryAslSpec(
                length=COMPACT_MAX_MEMORY_LENGTH,
                tag_length=COMPACT_MAX_MEMORY_TAG_LENGTH,
                formula_seed=COMPACT_MAX_MEMORY_FORMULA_SEED,
                formula_step=COMPACT_MAX_MEMORY_FORMULA_STEP,
                formula_modulus=COMPACT_MAX_MEMORY_FORMULA_MODULUS,
                copy_start=COMPACT_MAX_MEMORY_COPY_START,
                copy_end=COMPACT_MAX_MEMORY_COPY_END,
                copy_source_displacement=(
                    COMPACT_MAX_MEMORY_COPY_SOURCE_DISPLACEMENT
                ),
                initial_overrides=COMPACT_MAX_MEMORY_INITIAL_OVERRIDES,
                expected_overrides=COMPACT_MAX_MEMORY_EXPECTED_OVERRIDES,
            ),
            verify_compact_max_memory(regions),
        )
    guest_page_sizes = [
        page_size
        for page_size, profile in FULL_EXTENT_CAPTURE_PROFILES.items()
        if int(profile["backing_length"]) == length
    ]
    if not guest_page_sizes:
        return None
    if len(guest_page_sizes) != 1:
        raise ValueError("full-extent compact backing length is ambiguous")
    guest_page_size = guest_page_sizes[0]
    source_offset = guest_page_size + FULL_EXTENT_SOURCE_PAGE_DELTA
    destination_offset = source_offset + FULL_EXTENT_OVERLAP_DELTA
    return (
        CompactMemoryAslSpec(
            length=length,
            tag_length=length // 16,
            formula_seed=FULL_EXTENT_PATTERN_SEED,
            formula_step=FULL_EXTENT_PATTERN_STEP,
            formula_modulus=256,
            copy_start=destination_offset,
            copy_end=destination_offset + FULL_EXTENT_TRANSFER_LENGTH,
            copy_source_displacement=FULL_EXTENT_OVERLAP_DELTA,
        ),
        verify_full_extent_transfer_memory(regions, guest_page_size),
    )


def memory_source(
    memories: Sequence[Sequence[Mapping[str, object]]],
) -> tuple[list[str], list[dict[str, object]]]:
    lines: list[str] = []
    compact_reports: list[dict[str, object]] = []
    for memory_index, regions in enumerate(memories):
        compact = compact_memory_asl_spec(regions)
        compact_spec: CompactMemoryAslSpec | None = None
        compact_report: dict[str, object] | None = None
        if compact is not None:
            compact_spec, compact_report = compact
            compact_report["memory_index"] = memory_index
            compact_reports.append(compact_report)
            lines.extend(compact_memory_asl_source(memory_index, compact_spec))
        lines.extend(
            [
                f"func A64Batch_InitializeMemory{memory_index}()",
                "begin",
                "    A64Oracle_BeginMemoryRegions();",
            ]
        )
        for region_index, region in enumerate(regions):
            permissions = region["permissions"]
            if not isinstance(permissions, Mapping):
                raise ValueError("memory permissions shape differs")
            size = int(str(region["size"]), 16)
            lines.append(
                "    A64Oracle_ConfigureMemoryRegion("
                f"{region_index}, {bits(int(str(region['address']), 16), 64)}, "
                f"{size}, PAS_NonSecure, "
                f"{asl_boolean(permissions['readable'])}, "
                f"{asl_boolean(permissions['writable'])}, "
                f"{asl_boolean(permissions['executable'])}, "
                f"{asl_boolean(permissions['guest_code'])}, "
                f"{asl_boolean(permissions['guest_writable'])});"
            )
            initial_bytes = chunk_values(str(region["initial_bytes"]), 8)
            initial_tags = chunk_values(str(region["initial_tags"]), 4)
            if len(initial_bytes) != size:
                raise ValueError("memory byte extent differs")
            if compact_report is not None:
                assert compact_spec is not None
                if region_index != 0:
                    raise ValueError("compact memory region index drifted")
                lines.extend(
                    [
                        f"    for chunk = 0 to {compact_spec.length // 16 - 1} do",
                        "        A64Oracle_SetMemoryRegionByteChunk(",
                        f"            0, chunk * 16, 16, A64Batch_CompactMemory{memory_index}InitialChunk(chunk));",
                        "    end;",
                        f"    for chunk = 0 to {compact_spec.tag_length // 16 - 1} do",
                        "        A64Oracle_SetMemoryRegionTagChunk(",
                        "            0, chunk * 16, 16, Zeros{64});",
                        "    end;",
                    ]
                )
            else:
                for offset in range(0, len(initial_bytes), 16):
                    chunk = initial_bytes[offset : offset + 16]
                    lines.append(
                        "    A64Oracle_SetMemoryRegionByteChunk("
                        f"{region_index}, {offset}, {len(chunk)}, "
                        f"{bits(packed_chunk(chunk, 8), 128)});"
                    )
                for offset in range(0, len(initial_tags), 16):
                    chunk = initial_tags[offset : offset + 16]
                    lines.append(
                        "    A64Oracle_SetMemoryRegionTagChunk("
                        f"{region_index}, {offset}, {len(chunk)}, "
                        f"{bits(packed_chunk(chunk, 4), 64)});"
                    )
        lines.extend(["    A64Oracle_FinalizeMemoryRegions();", "end;", ""])

        lines.extend(
            [
                f"func A64Batch_MemoryMatches{memory_index}(compare_initial : boolean) => boolean",
                "begin",
                "    var matches : boolean = TRUE;",
                comparison(
                    f"A64Oracle_GetMemoryRegionCount() == {len(regions)}"
                ),
            ]
        )
        for region_index, region in enumerate(regions):
            permissions = region["permissions"]
            assert isinstance(permissions, Mapping)
            size = int(str(region["size"]), 16)
            lines.extend(
                [
                    comparison(
                        f"A64Oracle_GetMemoryRegionAddress({region_index}) == "
                        f"{bits(int(str(region['address']), 16), 64)}"
                    ),
                    comparison(
                        f"A64Oracle_GetMemoryRegionSize({region_index}) == {size}"
                    ),
                    comparison(
                        f"A64Oracle_GetMemoryRegionPASpace({region_index}) == PAS_NonSecure"
                    ),
                ]
            )
            for suffix, key in (
                ("Readable", "readable"),
                ("Writable", "writable"),
                ("Executable", "executable"),
                ("GuestCode", "guest_code"),
                ("GuestWritable", "guest_writable"),
            ):
                lines.append(
                    comparison(
                        f"A64Oracle_GetMemoryRegion{suffix}({region_index}) == "
                        f"{asl_boolean(permissions[key])}"
                    )
                )
            expected_bytes = chunk_values(str(region["expected_bytes"]), 8)
            expected_tags = chunk_values(str(region["expected_tags"]), 4)
            if compact_report is not None:
                assert compact_spec is not None
                lines.extend(
                    [
                        f"    for chunk = 0 to {compact_spec.length // 16 - 1} do",
                        "        let observed : bits(128) =",
                        "            A64Oracle_GetMemoryRegionByteChunk(0, chunk * 16, 16);",
                        "        let expected : bits(128) =",
                        "            if compare_initial then",
                        f"                A64Batch_CompactMemory{memory_index}InitialChunk(chunk)",
                        "            else",
                        f"                A64Batch_CompactMemory{memory_index}ExpectedChunk(chunk);",
                        "        if observed != expected then matches = FALSE; end;",
                        "    end;",
                        f"    for chunk = 0 to {compact_spec.tag_length // 16 - 1} do",
                        "        let observed : bits(64) =",
                        "            A64Oracle_GetMemoryRegionTagChunk(0, chunk * 16, 16);",
                        "        if observed != Zeros{64} then matches = FALSE; end;",
                        "    end;",
                    ]
                )
            else:
                for offset in range(0, len(expected_bytes), 16):
                    chunk = expected_bytes[offset : offset + 16]
                    lines.append(
                        comparison(
                            "A64Oracle_GetMemoryRegionByteChunk("
                            f"{region_index}, {offset}, {len(chunk)}) == "
                            f"{bits(packed_chunk(chunk, 8), 128)}"
                        )
                    )
                for offset in range(0, len(expected_tags), 16):
                    chunk = expected_tags[offset : offset + 16]
                    lines.append(
                        comparison(
                            "A64Oracle_GetMemoryRegionTagChunk("
                            f"{region_index}, {offset}, {len(chunk)}) == "
                            f"{bits(packed_chunk(chunk, 4), 64)}"
                        )
                    )
        lines.extend(["    return matches;", "end;", ""])
    lines.extend(
        [
            "func A64Batch_InitializeMemory(index : integer)",
            "begin",
            "    case index of",
        ]
    )
    for index in range(len(memories)):
        lines.append(f"        when {index} => A64Batch_InitializeMemory{index}();")
    lines.extend(["        otherwise => assert FALSE;", "    end;", "end;", ""])
    lines.extend(
        [
            "func A64Batch_MemoryMatches(index : integer, compare_initial : boolean) => boolean",
            "begin",
            "    case index of",
        ]
    )
    for index in range(len(memories)):
        lines.append(
            f"        when {index} => return A64Batch_MemoryMatches{index}(compare_initial);"
        )
    lines.extend(
        [
            "        otherwise => assert FALSE;",
            "    end;",
            "    return FALSE;",
            "end;",
            "",
        ]
    )
    return lines, compact_reports


def read_required(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def official_features(row: Mapping[str, str]) -> tuple[str, ...]:
    expression = row["static_feature_expressions"]
    if expression == "baseline":
        return ()
    features = tuple(sorted(set(re.findall(r"FEAT_[A-Za-z0-9_]+", expression))))
    if " && ".join(features) != expression:
        raise ValueError(f"unsupported feature expression {expression!r}")
    return features


def mops_stage(word: int) -> int:
    return (word >> 22) & 3


def cpy_post_size_policy(cpysize: int) -> int:
    if not 0 <= cpysize <= ARCH_MAX_MOPS_CPY_SIZE:
        raise ValueError("Option-B CPY remaining size is outside the official range")
    return min(cpysize, CPY_POST_RESERVE_BYTES)


def validate_cpy_post_size_policy(
    policy: Callable[[int], int],
) -> None:
    boundaries = (
        0,
        1,
        CPY_POST_RESERVE_BYTES - 1,
        CPY_POST_RESERVE_BYTES,
        CPY_POST_RESERVE_BYTES + 1,
        ARCH_MAX_MOPS_CPY_SIZE - 1,
        ARCH_MAX_MOPS_CPY_SIZE,
    )
    for cpysize in boundaries:
        reserved = policy(cpysize)
        if type(reserved) is not int:
            raise ValueError("CPY post-size policy did not return an integer")
        if reserved != min(cpysize, CPY_POST_RESERVE_BYTES):
            raise ValueError("CPY post-size policy differs from the pinned reserve")
        if not 0 <= reserved <= cpysize:
            raise ValueError("CPY post-size policy exceeds the remaining size")
        if reserved % 1 != 0:
            raise ValueError("CPY post-size policy violates byte alignment")
        if cpysize > 0 and reserved == 0:
            raise ValueError("CPY post-size policy prevents epilogue forward progress")
        if policy(reserved) != reserved:
            raise ValueError("CPY post-size policy is not stable at epilogue entry")
        main_work = cpysize - reserved
        if cpysize > 0 and main_work == 0 and reserved == 0:
            raise ValueError("CPY stage policy makes no forward progress")
    for invalid in (-1, ARCH_MAX_MOPS_CPY_SIZE + 1):
        try:
            policy(invalid)
        except ValueError:
            continue
        raise ValueError("CPY post-size policy accepted an out-of-domain length")


def cpy_post_size_caller_digest(shared_text: str) -> tuple[str, int]:
    callers: list[str] = []
    for name, bodies in asl_definition_bodies((shared_text,)).items():
        for body in bodies:
            if name != "CPYPostSizeChoice" and "CPYPostSizeChoice(" in body:
                callers.append(
                    f"{name}|{hashlib.sha256(body.encode('utf-8')).hexdigest()}\n"
                )
    return (
        hashlib.sha256("".join(sorted(callers)).encode("utf-8")).hexdigest(),
        len(callers),
    )


def verify_cpy_post_size_policy(
    shared_text: str, adapter_text: str
) -> dict[str, object]:
    digest, count = cpy_post_size_caller_digest(shared_text)
    if count != CPY_POST_CALLER_COUNT or digest != CPY_POST_CALLER_SHA256:
        raise ValueError("official CPY post-size caller constraints drifted")
    definitions = asl_definition_bodies((adapter_text,))
    bodies = definitions.get("CPYPostSizeChoice", ())
    if len(bodies) != 1:
        raise ValueError("expected one deterministic CPY post-size adapter")
    adapter_digest = hashlib.sha256(bodies[0].encode("utf-8")).hexdigest()
    if adapter_digest != CPY_POST_ADAPTER_SHA256:
        raise ValueError("deterministic CPY post-size adapter drifted")
    validate_cpy_post_size_policy(cpy_post_size_policy)
    return {
        "schema": "uml.a64.aslref-cpy-post-size-policy/v1",
        "official_caller_sha256": digest,
        "official_callers": count,
        "adapter_sha256": adapter_digest,
        "option": "B",
        "remaining_size_domain": [0, ARCH_MAX_MOPS_CPY_SIZE],
        "reserve_cap_bytes": CPY_POST_RESERVE_BYTES,
        "alignment_bytes": 1,
        "constraints": [
            "same-sign",
            "zero-to-remaining-size",
            "epilogue-fixed-point",
            "positive-remaining-implies-positive-reserve",
        ],
        "boundary_cases_verified": 7,
        "invalid_domain_cases_rejected": 2,
    }


def require_mops_whole_sequence(words: Sequence[int]) -> None:
    if tuple(mops_stage(word) for word in words) != (0, 1, 2):
        raise ValueError(
            "a standalone MOPS stage cannot be credited as a whole variant"
        )


def mops_contract_kind(record: Mapping[str, object]) -> str | None:
    native = set(record["feature_profile"]["native_required"])  # type: ignore[index]
    if "mops" not in native:
        return None
    program_words = [
        int(str(item["word"]), 16)
        for item in record["program"]["words"]  # type: ignore[index]
    ]
    operation_words = [word for word in program_words if word != 0xD400_0001]
    if len(operation_words) == 3:
        require_mops_whole_sequence(operation_words)
        terminal = record["terminal"]["event"]  # type: ignore[index]
        if terminal == "system-call":
            return "prologue-main-epilogue"
        if terminal == "memory-fault":
            evidence = int(str(record["identity"]["evidence_word"]), 16)  # type: ignore[index]
            if evidence not in operation_words:
                raise ValueError("faulted MOPS sequence does not name a program stage")
            return (
                "fault-sequence-"
                + ("prologue", "main", "epilogue")[mops_stage(evidence)]
            )
        raise ValueError("whole MOPS program has an unsupported terminal")
    if len(operation_words) != 1:
        raise ValueError("MOPS contract has an unsupported stage topology")
    stage = mops_stage(operation_words[0])
    if stage not in {1, 2} or record["terminal"]["event"] != "memory-fault":  # type: ignore[index]
        return "standalone-stage"
    evidence = int(str(record["identity"]["evidence_word"]), 16)  # type: ignore[index]
    if evidence != operation_words[0]:
        raise ValueError("MOPS restart evidence does not name its faulting stage")
    count_register = (evidence >> 5) & 31
    if count_register == 31:
        raise ValueError("MOPS restart uses the zero register as count")
    count = int(str(record["initial_state"]["x"][count_register]), 16)  # type: ignore[index]
    nzcv = int(str(record["initial_state"]["nzcv"]), 16) >> 28  # type: ignore[index]
    if count == 0 or nzcv not in {0x2, 0xA}:
        raise ValueError("MOPS restart state lacks an Option-B direction marker")
    return "restart-main" if stage == 1 else "restart-epilogue"


def prepare_sources(
    directory: Path,
    records: Sequence[Mapping[str, object]],
    required: Sequence[dict[str, str]],
    architecture_root: Path,
    bundler: Path,
    supplied: frozenset[str],
    supplemental: tuple[str, ...],
    runner: BoundedCommandRunner,
) -> PreparedSources:
    patterns = parse_patterns(
        required,
        ("architecture_constraints", "decode_constraints"),
        (("decode_mask", "decode_value"),),
    )
    words = sorted(
        {
            int(str(word["word"]), 16)
            for record in records
            for word in record["program"]["words"]  # type: ignore[index]
        }
    )
    rows_by_word: dict[int, dict[str, str]] = {}
    for word in words:
        matches = select_allocations(patterns[word >> 20], word)
        if len(matches) != 1:
            raise ValueError(f"{word:08x}: expected one official operation, got {len(matches)}")
        rows_by_word[word] = matches[0].row

    catalog = AslDependencyCatalog(architecture_root)
    by_source: dict[str, list[int]] = {}
    for word, row in rows_by_word.items():
        source = catalog.operation_source_file(row["architecture_file"])
        by_source.setdefault(source, []).append(word)
    instruction_paths: list[Path] = []
    source_functions: dict[str, tuple[str, ...]] = {}
    statuses: list[DependencyStatus] = []
    function_pattern = re.compile(r"(?m)^func\s+(instr_[A-Za-z0-9_]+)\s*\(")
    for index, source_file in enumerate(sorted(by_source)):
        output = directory / f"official-{index:03d}"
        output.mkdir()
        generated = runner.run(
            [
                sys.executable,
                "-E",
                "-s",
                "-S",
                "-B",
                str(bundler),
                "--make-funs",
                "-j",
                "1",
                "-o",
                str(output),
                str(architecture_root / source_file),
            ],
            capture_output=True,
            text=True,
            timeout=min(120, runner.timeout_seconds),
        )
        instructions = output / "instructions.asl"
        if generated.returncode != 0 or not instructions.is_file():
            raise ValueError(
                f"{source_file}: bundler failed: "
                + (generated.stderr + generated.stdout).strip()[:1000]
            )
        text = instructions.read_text(encoding="utf-8")
        functions = tuple(function_pattern.findall(text))
        if not functions:
            raise ValueError(f"{source_file}: no generated instruction functions")
        instruction_paths.append(instructions)
        source_functions[source_file] = functions
    operations: dict[int, Operation] = {}
    for word, row in rows_by_word.items():
        source = catalog.operation_source_file(row["architecture_file"])
        function = exact_function(
            architecture_root / source,
            row["architecture_encoding"],
            source_functions[source],
        )
        operations[word] = Operation(
            word=word,
            architecture_file=row["architecture_file"],
            architecture_encoding=row["architecture_encoding"],
            source_file=source,
            function=function,
            features=official_features(row),
        )
        statuses.append(catalog.for_file(row["architecture_file"]))
    direct = frozenset(
        definition for status in statuses for definition in status.direct_definitions
    )
    union = DependencyStatus(
        direct_definitions=direct,
        transitive_definitions=frozenset(),
        dependency_digest="",
        impdef_symbols=frozenset(),
        external_symbols=frozenset(),
        missing_interfaces=(),
        official_operation_present=True,
    )
    shared = directory / "official-shared.asl"
    runner.workspace.write_text(
        shared, catalog.shared_subset(union, supplied, supplemental)
    )
    return PreparedSources(tuple(instruction_paths), shared, operations)


def verify_compact_max_backing_contract(
    record: Mapping[str, object],
) -> dict[str, object]:
    if str(record["input_contract_id"]) != COMPACT_MAX_BACKING_INPUT_CONTRACT_ID:
        raise ValueError("maximum-backing contract identity drifted")
    initial_x = record["initial_state"]["x"]  # type: ignore[index]
    expected_x = record["expected_state"]["x"]  # type: ignore[index]
    destination = int(str(initial_x[3]), 16)  # type: ignore[index]
    source = int(str(initial_x[4]), 16)  # type: ignore[index]
    transferred = int(str(initial_x[5]), 16)  # type: ignore[index]
    if (
        destination != COMPACT_MAX_BACKING_DESTINATION_START
        or source != COMPACT_MAX_BACKING_SOURCE_START
        or transferred != COMPACT_MAX_BACKING_TRANSFER_LENGTH
    ):
        raise ValueError("maximum-backing transfer registers drifted")
    if (
        int(str(expected_x[3]), 16) != destination  # type: ignore[index]
        or int(str(expected_x[4]), 16) != source  # type: ignore[index]
        or int(str(expected_x[5]), 16) != 0  # type: ignore[index]
    ):
        raise ValueError("maximum-backing terminal copy registers drifted")
    program = record["program"]  # type: ignore[assignment]
    words_with_offsets = tuple(
        (int(str(item["offset"]), 16), int(str(item["word"]), 16))
        for item in program["words"]  # type: ignore[index]
    )
    expected_program = tuple(
        (index * 4, word)
        for index, word in enumerate(COMPACT_MAX_BACKING_PROGRAM_WORDS)
    )
    if words_with_offsets != expected_program:
        raise ValueError("maximum-backing P/M/E/SVC program drifted")
    require_mops_whole_sequence(COMPACT_MAX_BACKING_PROGRAM_WORDS[:3])
    regions = record["memory_regions"]  # type: ignore[assignment]
    verify_compact_max_memory(regions)
    source_span = (source, source + transferred)
    destination_span = (destination, destination + transferred)
    backing_span = (
        COMPACT_MAX_MEMORY_ADDRESS,
        COMPACT_MAX_MEMORY_ADDRESS + COMPACT_MAX_MEMORY_LENGTH,
    )
    if not (
        backing_span[0] <= source_span[0] < source_span[1] <= backing_span[1]
        and backing_span[0]
        <= destination_span[0]
        < destination_span[1]
        <= backing_span[1]
        and destination > source
        and destination < source_span[1]
    ):
        raise ValueError("maximum-backing overlapping transfer span drifted")
    terminal = record["terminal"]  # type: ignore[assignment]
    if (
        terminal["event"] != "system-call"  # type: ignore[index]
        or terminal["fault"] != "none"  # type: ignore[index]
        or terminal["retirement"] != "interpreter-equal"  # type: ignore[index]
    ):
        raise ValueError("maximum-backing terminal evidence drifted")
    return {
        "schema": "uml.a64.aslref-maximum-backing-contract/v1",
        "input_contract_id": COMPACT_MAX_BACKING_INPUT_CONTRACT_ID,
        "backing_span": [f"0x{value:016x}" for value in backing_span],
        "backing_bytes": COMPACT_MAX_MEMORY_LENGTH,
        "transferred_bytes": transferred,
        "source_span": [f"0x{value:016x}" for value in source_span],
        "destination_span": [
            f"0x{value:016x}" for value in destination_span
        ],
        "overlap_bytes": source_span[1] - destination_span[0],
        "direction": "backward",
        "program_words": [f"{word:08x}" for word in COMPACT_MAX_BACKING_PROGRAM_WORDS],
        "terminal_event": "system-call",
        "full_extent_65536_transfer": False,
    }


def generated_full_extent_transfer_memory(
    guest_page_size: int,
) -> tuple[bytes, bytes]:
    profile = FULL_EXTENT_CAPTURE_PROFILES.get(guest_page_size)
    if profile is None:
        raise ValueError("full-extent transfer uses an unreviewed page size")
    backing_length = int(profile["backing_length"])
    initial = bytes(
        (FULL_EXTENT_PATTERN_SEED + FULL_EXTENT_PATTERN_STEP * offset) & 0xFF
        for offset in range(backing_length)
    )
    expected = bytearray(initial)
    source_offset = guest_page_size + FULL_EXTENT_SOURCE_PAGE_DELTA
    destination_offset = source_offset + FULL_EXTENT_OVERLAP_DELTA
    expected[
        destination_offset : destination_offset + FULL_EXTENT_TRANSFER_LENGTH
    ] = initial[source_offset : source_offset + FULL_EXTENT_TRANSFER_LENGTH]
    return initial, bytes(expected)


def verify_full_extent_transfer_memory(
    regions: Sequence[Mapping[str, object]], guest_page_size: int
) -> dict[str, object]:
    profile = FULL_EXTENT_CAPTURE_PROFILES.get(guest_page_size)
    if profile is None:
        raise ValueError("full-extent transfer uses an unreviewed page size")
    if len(regions) != 1:
        raise ValueError("full-extent capture is not one mapped region")
    region = regions[0]
    backing_length = int(profile["backing_length"])
    if (
        int(str(region["address"]), 16) != COMPACT_MAX_MEMORY_ADDRESS
        or int(str(region["size"]), 16) != backing_length
    ):
        raise ValueError("full-extent backing address/length drifted")
    generated_initial, generated_expected = (
        generated_full_extent_transfer_memory(guest_page_size)
    )
    for label, generated in (
        ("initial", generated_initial),
        ("expected", generated_expected),
    ):
        captured = bytes.fromhex(str(region[f"{label}_bytes"]))
        if (
            len(captured) != backing_length
            or hashlib.sha256(captured).hexdigest()
            != profile[f"{label}_sha256"]
            or captured != generated
        ):
            raise ValueError(f"full-extent {label} memory evidence drifted")
    tag_length = backing_length // 16
    for label in ("initial_tags", "expected_tags"):
        tags = bytes.fromhex(str(region[label]))
        if (
            len(tags) != tag_length
            or any(tags)
            or hashlib.sha256(tags).hexdigest() != profile["tags_sha256"]
        ):
            raise ValueError(f"full-extent {label} evidence drifted")
    return {
        "schema": "uml.a64.aslref-compact-full-extent-memory/v1",
        "guest_page_size": guest_page_size,
        "length": backing_length,
        "tag_length": tag_length,
        "initial_sha256": profile["initial_sha256"],
        "expected_sha256": profile["expected_sha256"],
        "tags_sha256": profile["tags_sha256"],
        "initialized_bytes": backing_length,
        "compared_bytes": backing_length,
        "initialized_tags": tag_length,
        "compared_tags": tag_length,
    }


def verify_full_extent_transfer_contract(
    record: Mapping[str, object], guest_page_size: int
) -> dict[str, object]:
    profile = FULL_EXTENT_CAPTURE_PROFILES.get(guest_page_size)
    if profile is None:
        raise ValueError("full-extent transfer uses an unreviewed page size")
    exact_identifiers = {
        "input_contract_id": record["input_contract_id"],
        "program_id": record["program_id"],
        "initial_state_id": record["initial_state_id"],
        "expected_state_id": record["expected_state_id"],
        "memory_id": record["memory_id"],
    }
    if any(
        str(exact_identifiers[name]) != str(profile[name])
        for name in exact_identifiers
    ):
        raise ValueError("full-extent captured content identity drifted")
    identity = record["identity"]  # type: ignore[assignment]
    if (
        identity["case"] != FULL_EXTENT_TRANSFER_CASE  # type: ignore[index]
        or identity["ctest"] != "arm64-base-mops-copy"  # type: ignore[index]
        or identity["scope"] != "program"  # type: ignore[index]
        or int(str(identity["evidence_word"]), 16)  # type: ignore[index]
        != COMPACT_MAX_BACKING_PROGRAM_WORDS[0]
    ):
        raise ValueError("full-extent capture identity metadata drifted")
    program = record["program"]  # type: ignore[assignment]
    if int(str(program["guest_page_size"]), 16) != guest_page_size:  # type: ignore[index]
        raise ValueError("full-extent captured guest page size drifted")
    words_with_offsets = tuple(
        (int(str(item["offset"]), 16), int(str(item["word"]), 16))
        for item in program["words"]  # type: ignore[index]
    )
    if words_with_offsets != tuple(
        (index * 4, word)
        for index, word in enumerate(COMPACT_MAX_BACKING_PROGRAM_WORDS)
    ):
        raise ValueError("full-extent captured P/M/E/SVC program drifted")
    require_mops_whole_sequence(COMPACT_MAX_BACKING_PROGRAM_WORDS[:3])
    source = (
        COMPACT_MAX_MEMORY_ADDRESS
        + guest_page_size
        + FULL_EXTENT_SOURCE_PAGE_DELTA
    )
    destination = source + FULL_EXTENT_OVERLAP_DELTA
    initial_x = record["initial_state"]["x"]  # type: ignore[index]
    expected_x = record["expected_state"]["x"]  # type: ignore[index]
    if tuple(int(str(initial_x[index]), 16) for index in (3, 4, 5)) != (  # type: ignore[index]
        destination,
        source,
        FULL_EXTENT_TRANSFER_LENGTH,
    ):
        raise ValueError("full-extent initial source/destination/count drifted")
    if tuple(int(str(expected_x[index]), 16) for index in (3, 4, 5)) != (  # type: ignore[index]
        destination,
        source,
        0,
    ):
        raise ValueError("full-extent terminal source/destination/count drifted")
    expected_state = record["expected_state"]  # type: ignore[assignment]
    if (
        int(str(expected_state["pc"]), 16) != 0x4F00_0010  # type: ignore[index]
        or int(str(expected_state["nzcv"]), 16) != 0xA000_0000  # type: ignore[index]
    ):
        raise ValueError("full-extent terminal PC/NZCV drifted")
    regions = record["memory_regions"]  # type: ignore[assignment]
    memory_report = verify_full_extent_transfer_memory(regions, guest_page_size)
    backing_length = int(profile["backing_length"])
    terminal = record["terminal"]  # type: ignore[assignment]
    if terminal != {
        "event": "system-call",
        "retirement": "interpreter-equal",
        "instruction_budget": "0000000000000040",
        "state_oracle": "exact",
        "fault": "none",
        "syndrome": "00000000",
        "invalid_encoding": False,
    }:
        raise ValueError("full-extent terminal event evidence drifted")
    if record["engine_results"] != {  # type: ignore[comparison-overlap]
        "interpreter": "executed-retired",
        "gadgets": "executed-retired",
        "jit": "skipped-host-feature",
    }:
        raise ValueError("full-extent three-engine evidence drifted")
    if record["integrity"] != {  # type: ignore[comparison-overlap]
        key: profile[key]
        for key in (
            "initial_state_digest",
            "expected_state_digest",
            "initial_memory_digest",
            "expected_memory_digest",
        )
    }:
        raise ValueError("full-extent recorder integrity evidence drifted")
    source_span = (source, source + FULL_EXTENT_TRANSFER_LENGTH)
    destination_span = (
        destination,
        destination + FULL_EXTENT_TRANSFER_LENGTH,
    )
    return {
        "schema": "uml.a64.full-extent-transfer-capture/v1",
        "guest_page_size": guest_page_size,
        **{name: str(value) for name, value in exact_identifiers.items()},
        "transfer_bytes": FULL_EXTENT_TRANSFER_LENGTH,
        "backing_bytes": backing_length,
        "source_span": [f"0x{value:016x}" for value in source_span],
        "destination_span": [
            f"0x{value:016x}" for value in destination_span
        ],
        "overlap_bytes": source_span[1] - destination_span[0],
        "crossed_guest_pages": (
            (destination_span[1] - 1) // guest_page_size
            - destination_span[0] // guest_page_size
        ),
        "terminal_event": "system-call",
        "state_oracle": "exact",
        "memory_bytes_compared": backing_length,
        "compact_memory": memory_report,
        "engine_results": dict(record["engine_results"]),  # type: ignore[arg-type]
    }


def verify_full_extent_transfer_capture(
    path: Path, guest_page_size: int
) -> dict[str, object]:
    matches = [
        record
        for record in iter_raw_materializations([path])
        if record["identity"]["case"] == FULL_EXTENT_TRANSFER_CASE  # type: ignore[index]
    ]
    if len(matches) != 1:
        raise ValueError("capture does not contain exactly one full-extent contract")
    return verify_full_extent_transfer_contract(matches[0], guest_page_size)


def selected_full_extent_transfer_record(
    directory: Path, guest_page_size: int
) -> Mapping[str, object]:
    path = directory / "arm64-base-mops-copy.tsv.inputs.jsonl.gz"
    if not path.is_file():
        raise ValueError(f"missing real v4 full-extent capture {path}")
    matches = [
        record
        for record in iter_raw_materializations([path])
        if record["identity"]["case"] == FULL_EXTENT_TRANSFER_CASE  # type: ignore[index]
    ]
    if len(matches) != 1:
        raise ValueError("capture does not contain exactly one full-extent contract")
    verify_full_extent_transfer_contract(matches[0], guest_page_size)
    return matches[0]


def selected_records(directory: Path, count: int) -> list[Mapping[str, object]]:
    if count < 4 or count > MAX_CONTRACTS:
        raise ValueError(f"count must be in 4..{MAX_CONTRACTS}")
    if count > 4:
        strict_four = selected_records(directory, 4)
        return [
            *strict_four,
            *(
                strict_four[1 + ((index - 4) % 3)]
                for index in range(4, count)
            ),
        ]
    mops_path = directory / "arm64-base-mops-copy.tsv.inputs.jsonl.gz"
    fault_path = directory / "arm64-base-immediate-memory-fault.tsv.inputs.jsonl.gz"
    callback_path = directory / "arm64-base-pauth-code.tsv.inputs.jsonl.gz"
    for path in (mops_path, fault_path, callback_path):
        if not path.is_file():
            raise ValueError(f"missing real v4 capture {path}")
    maximum_memory: Mapping[str, object] | None = None
    multiple_regions: Mapping[str, object] | None = None
    for record in iter_raw_materializations([mops_path]):
        kind = mops_contract_kind(record)
        total = sum(
            int(str(region["size"]), 16)
            for region in record["memory_regions"]  # type: ignore[index]
        )
        if (
            str(record["input_contract_id"])
            == COMPACT_MAX_BACKING_INPUT_CONTRACT_ID
            and total == COMPACT_MAX_MEMORY_LENGTH
            and kind == "prologue-main-epilogue"
            and maximum_memory is None
        ):
            verify_compact_max_backing_contract(record)
            maximum_memory = record
        if (
            len(record["memory_regions"]) >= 3  # type: ignore[arg-type]
            and isinstance(kind, str)
            and kind.startswith("restart-")
            and multiple_regions is None
        ):
            multiple_regions = record
        if maximum_memory is not None and multiple_regions is not None:
            break
    if maximum_memory is None or multiple_regions is None:
        raise ValueError(
            "MOPS capture lacks pinned maximum-backing/multi-region contracts"
        )
    # The capacity probe uses fault contracts whose raw terminal address is
    # also the architectural request address.  Cross-page fixtures report the
    # first failing byte while the current bounded Mem interface exposes the
    # request start, and SP-alignment fixtures need a projected SCTLR.SA0
    # policy.  Those remain promotion obligations rather than being silently
    # weakened in this benchmark.
    faults = [
        record
        for record in iter_raw_materializations([fault_path])
        if record["terminal"]["event"] == "memory-fault"  # type: ignore[index]
        and "second-page" not in str(record["identity"]["case"])  # type: ignore[index]
        and "SP alignment" not in str(record["identity"]["case"])  # type: ignore[index]
    ]
    fault_count = min(len(faults), (count - 2) // 2)
    callback_count = count - 2 - fault_count
    callbacks: list[Mapping[str, object]] = []
    for record in iter_raw_materializations([callback_path]):
        contracts = record["representability"]["callback_contracts"]  # type: ignore[index]
        validator = contracts["validator"]
        if (
            isinstance(validator, Mapping)
            and validator.get("relation_id") == "permitted-transformed-code/v1"
            and (int(str(record["identity"]["evidence_word"]), 16) & 0xFFFF_C000)  # type: ignore[index]
            == 0xDAC1_0000
            and ((int(str(record["identity"]["evidence_word"]), 16) >> 10) & 15)  # type: ignore[index]
            == 0
        ):
            callbacks.append(record)
            if len(callbacks) == callback_count:
                break
    if len(callbacks) != callback_count:
        raise ValueError("callback capture lacks enough bounded relation contracts")
    selected = [maximum_memory, multiple_regions, *faults[:fault_count], *callbacks]
    if len(selected) != count or len(
        {str(record["input_contract_id"]) for record in selected}
    ) != count:
        raise ValueError("mixed selection count/identity differs")
    return selected


def factored_payloads(
    records: Sequence[Mapping[str, object]],
) -> tuple[
    list[int],
    list[Sequence[Mapping[str, object]]],
    list[Mapping[str, object]],
    list[int],
]:
    states: dict[str, int] = {}
    memories: dict[str, int] = {}
    programs: dict[str, int] = {}
    state_payloads: list[int] = []
    memory_payloads: list[Sequence[Mapping[str, object]]] = []
    program_payloads: list[Mapping[str, object]] = []
    contract_payloads: list[int] = []
    for record in records:
        for identifier_name, payload_name in (
            ("initial_state_id", "initial_state"),
            ("expected_state_id", "expected_state"),
        ):
            identifier = str(record[identifier_name])
            if identifier not in states:
                states[identifier] = len(state_payloads)
                state_payloads.append(
                    STATE_LAYOUT.pack(state_values(record[payload_name]))  # type: ignore[arg-type]
                )
        memory_id = str(record["memory_id"])
        if memory_id not in memories:
            memories[memory_id] = len(memory_payloads)
            memory_payloads.append(record["memory_regions"])  # type: ignore[arg-type]
        program_id = str(record["program_id"])
        if program_id not in programs:
            programs[program_id] = len(program_payloads)
            program_payloads.append(record["program"])  # type: ignore[arg-type]
        callback_contracts = record["representability"]["callback_contracts"]  # type: ignore[index]
        validator = callback_contracts["validator"]
        callback = CALLBACK_EXACT
        relation_id: str | None = None
        if isinstance(validator, Mapping):
            relation_id = str(validator.get("relation_id"))
            if relation_id != PAC_RELATION:
                raise ValueError("selected callback relation is unsupported")
            callback = CALLBACK_PERMITTED_TRANSFORMED_CODE
        event_name = str(record["terminal"]["event"])  # type: ignore[index]
        event = {
            "system-call": EVENT_SYSTEM_CALL,
            "memory-fault": EVENT_MEMORY_FAULT,
        }.get(event_name)
        if event is None:
            raise ValueError(f"selected event {event_name!r} is unsupported")
        syndrome = int(str(record["terminal"]["syndrome"]), 16)  # type: ignore[index]
        fault_address = 0
        fault_write = 0
        fault_bytes = 0
        fault_code = FAULT_NONE
        raw_fault = str(record["terminal"]["fault"])  # type: ignore[index]
        if event == EVENT_MEMORY_FAULT:
            match = re.fullmatch(r"(read|write):([0-9a-f]{16})", raw_fault)
            if match is None:
                raise ValueError("selected memory-fault terminal is malformed")
            fault_write = int(match.group(1) == "write")
            fault_address = int(match.group(2), 16)
            evidence_word = int(str(record["identity"]["evidence_word"]), 16)  # type: ignore[index]
            fault_bytes = 1 << ((evidence_word >> 30) & 3)
            containing_region: Mapping[str, object] | None = None
            for region in record["memory_regions"]:  # type: ignore[index]
                first = int(str(region["address"]), 16)
                size = int(str(region["size"]), 16)
                if (
                    fault_address >= first
                    and fault_address + fault_bytes <= first + size
                ):
                    containing_region = region
                    break
            if containing_region is None:
                fault_code = FAULT_TRANSLATION
            else:
                permissions = containing_region["permissions"]
                if not isinstance(permissions, Mapping):
                    raise ValueError("memory permissions shape differs")
                permitted = bool(
                    permissions["writable" if fault_write else "readable"]
                )
                if permitted:
                    raise ValueError(
                        "selected fault lacks an exact translation/permission policy"
                    )
                fault_code = FAULT_PERMISSION
        elif raw_fault != "none":
            raise ValueError("non-fault terminal carries fault metadata")
        ordered_words = sorted(
            (
                int(str(item["offset"]), 16),
                int(str(item["word"]), 16),
            )
            for item in record["program"]["words"]  # type: ignore[index]
            if int(str(item["offset"]), 16)
            >= int(str(record["program"]["entry_offset"]), 16)  # type: ignore[index]
        )
        if event == EVENT_SYSTEM_CALL:
            terminals = [
                index
                for index, (_offset, word) in enumerate(ordered_words)
                if word == 0xD400_0001
            ]
        else:
            evidence_word = int(str(record["identity"]["evidence_word"]), 16)  # type: ignore[index]
            terminals = [
                index
                for index, (_offset, word) in enumerate(ordered_words)
                if word == evidence_word
            ]
        if len(terminals) != 1:
            raise ValueError("terminal instruction position is not unique")
        native = set(record["feature_profile"]["native_required"])  # type: ignore[index]
        mops_kind = mops_contract_kind(record)
        resolve_pac_algorithm(
            native,
            relation_id,
            PAC_ALGORITHM if callback == CALLBACK_PERMITTED_TRANSFORMED_CODE else None,
        )
        feature_bits = (
            (1 if "mops" in native else 0)
            | (2 if "pauth" in native else 0)
            | (4 if "advanced_simd" in native else 0)
        )
        values = {
            "initial_state": states[str(record["initial_state_id"])],
            "expected_state": states[str(record["expected_state_id"])],
            "memory": memories[memory_id],
            "program": programs[program_id],
            "callback": callback,
            "event": event,
            "features": feature_bits,
            "evidence": int(str(record["identity"]["evidence_word"]), 16),  # type: ignore[index]
            "syndrome": syndrome,
            "fault_address": fault_address,
            "fault_write": fault_write,
            "fault_bytes": fault_bytes,
            "fault_code": fault_code,
            "fault_throws": int(
                event == EVENT_MEMORY_FAULT
                and mops_kind is not None
                and mops_kind.startswith("restart-")
            ),
            "restore_on_fault": int(
                event == EVENT_MEMORY_FAULT
                and str(record["terminal"]["retirement"]) == "exact:0"  # type: ignore[index]
            ),
            "terminal_word": terminals[0],
            "retired_words": terminals[0],
        }
        contract_payloads.append(CONTRACT_LAYOUT.pack(values))
    return state_payloads, memory_payloads, program_payloads, contract_payloads


def program_source(
    programs: Sequence[Mapping[str, object]], operations: Mapping[int, Operation]
) -> list[str]:
    lines: list[str] = [
        "var A64Batch_CurrentProgram : integer;",
        "var A64Batch_CurrentWord : integer;",
        "var A64Batch_RetiredWords : integer;",
        "",
    ]
    for index, program in enumerate(programs):
        entry = int(str(program["entry_offset"]), 16)
        words = [
            (int(str(item["offset"]), 16), int(str(item["word"]), 16))
            for item in program["words"]  # type: ignore[index]
            if int(str(item["offset"]), 16) >= entry
        ]
        words.sort()
        if not words or words[0][0] != entry or any(
            words[position][0] != entry + position * 4
            for position in range(len(words))
        ):
            raise ValueError("probe supports only contiguous straight-line programs")
        lines.extend(
            [
                f"func A64Batch_ExecuteProgram{index}()",
                "begin",
                "    var running : boolean = TRUE;",
            ]
        )
        for word_index, (_offset, word) in enumerate(words):
            operation = operations[word]
            lines.append("    if running then")
            lines.append(f"        A64Batch_CurrentProgram = {index};")
            lines.append(f"        A64Batch_CurrentWord = {word_index};")
            lines.append(
                f"        A64Oracle_SetCurrentInstruction({bits(word, 32)});"
            )
            if operation.architecture_encoding == "SVC_EX_exception":
                lines.append(
                    "        A64Oracle_SetPC(A64Oracle_GetPC() + 4);"
                )
                lines.append(f"        {operation.function}({bits(word, 32)});")
            else:
                lines.append(f"        {operation.function}({bits(word, 32)});")
                lines.append(
                    "        if A64Oracle_LastMemoryStatus.statuscode != Fault_None then"
                )
                lines.append("            running = FALSE;")
                lines.append("        else")
                lines.append(
                    "            A64Oracle_SetPC(A64Oracle_GetPC() + 4);"
                )
                lines.append(
                    "            A64Batch_RetiredWords = A64Batch_RetiredWords + 1;"
                )
                lines.append("        end;")
            lines.append("    end;")
        lines.extend(["end;", ""])
    lines.extend(
        [
            "func A64Batch_ExecuteProgram(index : integer)",
            "begin",
            "    case index of",
        ]
    )
    for index in range(len(programs)):
        lines.append(f"        when {index} => A64Batch_ExecuteProgram{index}();")
    lines.extend(["        otherwise => assert FALSE;", "    end;", "end;", ""])
    return lines


def event_match_source() -> list[str]:
    return [
        "func A64Batch_EventMatches(event : integer, caught : boolean,",
        "                           syndrome : bits(32),",
        "                           fault_address : bits(64),",
        "                           fault_write : boolean,",
        "                           fault_bytes : integer,",
        "                           fault_code : integer,",
        "                           fault_throws : boolean,",
        "                           expected_program : integer,",
        "                           terminal_word : integer,",
        "                           retired_words : integer) => boolean",
        "begin",
        "    if event == 1 then",
        "        return caught && A64Oracle_Terminal && A64Oracle_HasException &&",
        "            A64Batch_CurrentProgram == expected_program &&",
        "            A64Batch_CurrentWord == terminal_word &&",
        "            A64Batch_RetiredWords == retired_words &&",
        "            A64Oracle_LastEvent == A64Oracle_Event_ExceptionTaken &&",
        "            A64Oracle_LastException.exceptype == Exception_SupervisorCall &&",
        "            syndrome[31:25] == Zeros{7} &&",
        "            A64Oracle_LastException.syndrome.iss == syndrome[24:0] &&",
        "            A64Oracle_LastMemoryStatus.statuscode == Fault_None &&",
        "            !A64Oracle_GetLastFaultPresent() &&",
        "            A64Oracle_GetLastFaultDomain() == A64OracleMemoryAddressDomain_None &&",
        "            A64Oracle_GetLastFaultPASpace() == PAS_NonSecure &&",
        "            A64Oracle_GetLastFaultAddress() == Zeros{64} &&",
        "            A64Oracle_GetLastFaultStatusCode() == Fault_None &&",
        "            A64Oracle_GetLastFaultExtFlag() == '0' &&",
        "            A64Oracle_GetLastFaultErrorState() == ErrorState_CE &&",
        "            A64Oracle_GetLastFaultStore64BStatus() == Zeros{64};",
        "    elsif event == 2 then",
        "        var expected_status : Fault = Fault_None;",
        "        if fault_code == 1 then",
        "            expected_status = Fault_Translation;",
        "        elsif fault_code == 2 then",
        "            expected_status = Fault_Permission;",
        "        else",
        "            return FALSE;",
        "        end;",
        "        let expected_class : A64OracleMemoryAccessClass =",
        "            if fault_write then A64OracleMemoryAccessClass_DataWrite",
        "            else A64OracleMemoryAccessClass_DataRead;",
        "        let expected_event : A64OracleMemoryEvent =",
        "            if fault_write then A64OracleMemoryEvent_Store",
        "            else A64OracleMemoryEvent_Load;",
        "        let terminal_matches : boolean =",
        "            if fault_throws then",
        "                caught && A64Oracle_Terminal && !A64Oracle_HasException &&",
        "                A64Oracle_HasFault &&",
        "                A64Oracle_LastEvent == A64Oracle_Event_AArch64Abort &&",
        "                A64Oracle_LastFault.statuscode == expected_status &&",
        "                A64Oracle_LastFault.vaddress == fault_address &&",
        "                A64Oracle_LastFault.write == fault_write &&",
        "                A64Oracle_LastFault.extflag == '0' &&",
        "                A64Oracle_LastFault.merrorstate == ErrorState_CE &&",
        "                A64Oracle_LastFault.accessdesc.acctype == AccessType_GPR &&",
        "                A64Oracle_LastFault.accessdesc.el == EL0 &&",
        "                A64Oracle_LastFault.accessdesc.read == !fault_write &&",
        "                A64Oracle_LastFault.accessdesc.write == fault_write &&",
        "                A64Oracle_LastFault.accessdesc.mops",
        "            else",
        "                !caught && !A64Oracle_Terminal && !A64Oracle_HasException &&",
        "                !A64Oracle_HasFault &&",
        "                A64Oracle_LastEvent == A64Oracle_Event_None;",
        "        return terminal_matches &&",
        "            A64Batch_CurrentProgram == expected_program &&",
        "            A64Batch_CurrentWord == terminal_word &&",
        "            A64Batch_RetiredWords == retired_words &&",
        "            syndrome == Zeros{32} &&",
        "            A64Oracle_LastMemoryStatus.statuscode == expected_status &&",
        "            A64Oracle_LastMemoryEvent == expected_event &&",
        "            A64Oracle_GetLastAccessPresent() &&",
        "            A64Oracle_GetLastAccessClass() == expected_class &&",
        "            A64Oracle_GetLastAccessDomain() == A64OracleMemoryAddressDomain_Virtual &&",
        "            A64Oracle_GetLastAccessPASpace() == PAS_NonSecure &&",
        "            A64Oracle_GetLastAccessAddress() == fault_address &&",
        "            A64Oracle_GetLastAccessWrite() == fault_write &&",
        "            A64Oracle_GetLastAccessByteCount() == fault_bytes &&",
        "            A64Oracle_GetLastAccessStatusCode() == expected_status &&",
        "            A64Oracle_GetLastAccessExtFlag() == '0' &&",
        "            A64Oracle_GetLastAccessErrorState() == ErrorState_CE &&",
        "            A64Oracle_GetLastAccessStore64BStatus() == Zeros{64} &&",
        "            A64Oracle_GetLastFaultPresent() &&",
        "            A64Oracle_GetLastFaultDomain() == A64OracleMemoryAddressDomain_Virtual &&",
        "            A64Oracle_GetLastFaultPASpace() == PAS_NonSecure &&",
        "            A64Oracle_GetLastFaultAddress() == fault_address &&",
        "            A64Oracle_GetLastFaultStatusCode() == expected_status &&",
        "            A64Oracle_GetLastFaultExtFlag() == '0' &&",
        "            A64Oracle_GetLastFaultErrorState() == ErrorState_CE &&",
        "            A64Oracle_GetLastFaultStore64BStatus() == Zeros{64};",
        "    else",
        "        return FALSE;",
        "    end;",
        "end;",
        "",
    ]


def main_source(count: int, diagnostic_maximum_phase: str = "full") -> list[str]:
    phases = {
        "full",
        "initialize-only",
        "initialize-expected-compare",
        "execute-without-memory-compare",
    }
    if diagnostic_maximum_phase not in phases:
        raise ValueError("unknown maximum-backing diagnostic phase")
    skip_execution = diagnostic_maximum_phase in {
        "initialize-only",
        "initialize-expected-compare",
    }
    bypass_event_state = skip_execution
    bypass_memory = diagnostic_maximum_phase in {
        "initialize-only",
        "execute-without-memory-compare",
    }
    compare_initial = (
        diagnostic_maximum_phase == "initialize-expected-compare"
    )
    skip_execution_condition = "contract == 0" if skip_execution else "FALSE"
    bypass_event_condition = "contract == 0" if bypass_event_state else "FALSE"
    bypass_memory_condition = "contract == 0" if bypass_memory else "FALSE"
    compare_initial_condition = "contract == 0" if compare_initial else "FALSE"
    get = lambda name: contract_field(name)  # noqa: E731
    return [
        "func main() => integer",
        "begin",
        "    var failures : integer = 0;",
        f"    for contract = 0 to {count - 1} do",
        f"        let descriptor : bits({CONTRACT_LAYOUT.width}) = A64Batch_ContractPayload(contract);",
        f"        let initial_index : integer = UInt(descriptor[{get('initial_state').offset} +: 16]);",
        f"        let expected_index : integer = UInt(descriptor[{get('expected_state').offset} +: 16]);",
        f"        let memory_index : integer = UInt(descriptor[{get('memory').offset} +: 16]);",
        f"        let program_index : integer = UInt(descriptor[{get('program').offset} +: 16]);",
        f"        let relation : integer = UInt(descriptor[{get('callback').offset} +: 8]);",
        f"        let event : integer = UInt(descriptor[{get('event').offset} +: 8]);",
        f"        let features : bits(8) = descriptor[{get('features').offset} +: 8];",
        f"        let evidence : bits(32) = descriptor[{get('evidence').offset} +: 32];",
        f"        let syndrome : bits(32) = descriptor[{get('syndrome').offset} +: 32];",
        f"        let fault_address : bits(64) = descriptor[{get('fault_address').offset} +: 64];",
        f"        let fault_write : boolean = descriptor[{get('fault_write').offset}] == '1';",
        f"        let fault_bytes : integer = UInt(descriptor[{get('fault_bytes').offset} +: 8]);",
        f"        let fault_code : integer = UInt(descriptor[{get('fault_code').offset} +: 2]);",
        f"        let fault_throws : boolean = descriptor[{get('fault_throws').offset}] == '1';",
        f"        let restore_on_fault : boolean = descriptor[{get('restore_on_fault').offset}] == '1';",
        f"        let terminal_word : integer = UInt(descriptor[{get('terminal_word').offset} +: 8]);",
        f"        let retired_words : integer = UInt(descriptor[{get('retired_words').offset} +: 8]);",
        "        A64Execution_Reset(evidence);",
        "        A64Batch_InstallState(initial_index);",
        "        A64Batch_InitializeMemory(memory_index);",
        "        A64Batch_RetiredWords = 0;",
        "        if features[0] == '1' then A64Oracle_EnableFeature(FEAT_MOPS); end;",
        "        if features[1] == '1' then A64Oracle_EnableFeature(FEAT_PAuth); end;",
        "        if relation == 1 then",
        "            // The callback contract permits any architecturally valid",
        "            // transformed PAC code; select the adapter's deterministic",
        "            // implementation-defined PAC function under normalization",
        "            // obligation bit 3 (engine-private state and scratch).",
        "            A64Oracle_EnableFeature(FEAT_PACIMP);",
        "        end;",
        "        if features[2] == '1' then A64Oracle_EnableFeature(FEAT_AdvSIMD); end;",
        "        var caught : boolean = FALSE;",
        "        var unexpected : boolean = FALSE;",
        f"        if !({skip_execution_condition}) then",
        "            try",
        "                A64Batch_ExecuteProgram(program_index);",
        "            catch",
        "                when outcome : A64Oracle_TerminalOutcome =>",
        "                    caught = TRUE;",
        "                otherwise =>",
        "                    unexpected = TRUE;",
        "            end;",
        "        end;",
        "        if event == 2 && restore_on_fault &&",
        "           A64Oracle_LastMemoryStatus.statuscode != Fault_None then",
        "            A64Batch_InstallState(initial_index);",
        "        end;",
        "        let event_matches : boolean =",
        f"            if {bypass_event_condition} then TRUE",
        "            else A64Batch_EventMatches(",
        "                event, caught, syndrome, fault_address, fault_write,",
        "                fault_bytes, fault_code, fault_throws, program_index,",
        "                terminal_word, retired_words);",
        "        let state_matches : boolean =",
        f"            if {bypass_event_condition} then TRUE",
        "            else A64Batch_StateMatches(",
        "                expected_index, initial_index, relation, evidence);",
        "        let memory_matches : boolean =",
        f"            if {bypass_memory_condition} then TRUE",
        "            else A64Batch_MemoryMatches(",
        f"                memory_index, {compare_initial_condition});",
        "        if unexpected || !event_matches || !state_matches ||",
        "           !memory_matches then",
        "            var failure_bits : integer = 0;",
        "            if unexpected then failure_bits = failure_bits + 1; end;",
        "            if !event_matches then failure_bits = failure_bits + 2; end;",
        "            if !state_matches then failure_bits = failure_bits + 4; end;",
        "            if !memory_matches then failure_bits = failure_bits + 8; end;",
        "            print \"A64_BATCH_FAILURE=\", contract, \"|BITS=\", failure_bits, \"\\n\";",
        "            failures = failures + 1;",
        "        end;",
        "    end;",
        f'    print "FORMAT={FORMAT}|COUNT={count}|FAILURES=", failures, "\\n";',
        "    return if failures == 0 then 0 else 2;",
        "end;",
        "",
    ]


def render_harness(
    records: Sequence[Mapping[str, object]],
    prepared: PreparedSources,
    diagnostic_maximum_phase: str = "full",
) -> tuple[str, dict[str, object]]:
    states, memories, programs, contracts = factored_payloads(records)
    lines = [
        "/* Content-factored real materialization batch; generated by probe. */",
        f"constant A64Batch_StatePayloadWidth : integer{{}} = {STATE_LAYOUT.width};",
        f"constant A64Batch_ContractPayloadWidth : integer{{}} = {CONTRACT_LAYOUT.width};",
        "",
    ]
    lines.extend(state_source(states))
    lines.extend(state_match_source())
    memory_lines, compact_memories = memory_source(memories)
    lines.extend(memory_lines)
    lines.extend(program_source(programs, prepared.operations))
    lines.extend(
        table_function(
            "A64Batch_ContractPayload", CONTRACT_LAYOUT.width, contracts
        )
    )
    lines.extend(event_match_source())
    lines.extend(main_source(len(records), diagnostic_maximum_phase))
    return "\n".join(lines), {
        "unique_states": len(states),
        "unique_memories": len(memories),
        "unique_programs": len(programs),
        "contracts": len(contracts),
        "compact_memories": compact_memories,
        "diagnostic_maximum_phase": diagnostic_maximum_phase,
    }


def require_generated_artifact_limit(
    directory: Path,
    maximum_bytes: int,
    maximum_files: int = MAX_ARTIFACT_FILES,
) -> int:
    if maximum_bytes <= 0 or maximum_files <= 0:
        raise ValueError("generated artifact caps must be positive")
    files = [path for path in directory.rglob("*") if path.is_file()]
    if len(files) > maximum_files:
        raise ValueError(
            f"generated artifact files {len(files)} exceed hard cap "
            f"{maximum_files}"
        )
    byte_count = sum(path.stat().st_size for path in files)
    if byte_count > maximum_bytes:
        raise ValueError(
            f"generated artifact bytes {byte_count} exceed hard cap "
            f"{maximum_bytes}"
        )
    return byte_count


class _DarwinRusageInfoV2(ctypes.Structure):
    _fields_ = [
        ("ri_uuid", ctypes.c_uint8 * 16),
        ("ri_user_time", ctypes.c_uint64),
        ("ri_system_time", ctypes.c_uint64),
        ("ri_pkg_idle_wkups", ctypes.c_uint64),
        ("ri_interrupt_wkups", ctypes.c_uint64),
        ("ri_pageins", ctypes.c_uint64),
        ("ri_wired_size", ctypes.c_uint64),
        ("ri_resident_size", ctypes.c_uint64),
        ("ri_phys_footprint", ctypes.c_uint64),
        ("ri_proc_start_abstime", ctypes.c_uint64),
        ("ri_proc_exit_abstime", ctypes.c_uint64),
        ("ri_child_user_time", ctypes.c_uint64),
        ("ri_child_system_time", ctypes.c_uint64),
        ("ri_child_pkg_idle_wkups", ctypes.c_uint64),
        ("ri_child_interrupt_wkups", ctypes.c_uint64),
        ("ri_child_pageins", ctypes.c_uint64),
        ("ri_child_elapsed_abstime", ctypes.c_uint64),
        ("ri_diskio_bytesread", ctypes.c_uint64),
        ("ri_diskio_byteswritten", ctypes.c_uint64),
    ]


@functools.lru_cache(maxsize=1)
def _darwin_proc_rusage() -> Callable[..., int] | None:
    try:
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        function = library.proc_pid_rusage
        function.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(_DarwinRusageInfoV2),
        ]
        function.restype = ctypes.c_int
        return function
    except (AttributeError, OSError):
        return None


def _resident_size_kib(process_id: int) -> int | None:
    """Sample one process without launching an unbounded helper process."""
    if sys.platform == "darwin":
        function = _darwin_proc_rusage()
        if function is None:
            return None
        usage = _DarwinRusageInfoV2()
        if function(process_id, 2, ctypes.byref(usage)) != 0:
            return None
        return int(usage.ri_resident_size) // 1024
    status = Path(f"/proc/{process_id}/status")
    try:
        for line in status.read_text(encoding="ascii").splitlines():
            if line.startswith("VmRSS:"):
                fields = line.split()
                return int(fields[1])
    except (OSError, UnicodeError, ValueError):
        pass
    return None


def execute_measured(
    command: list[str],
    timeout: int,
    max_rss_kib: int,
    max_log_bytes: int,
    directory: Path,
    workspace: BoundedWorkspace,
) -> tuple[int, str, str, float, int, str, str]:
    if timeout <= 0 or max_rss_kib <= 0 or max_log_bytes <= 0:
        raise ValueError("ASLRef process caps must be positive")
    if directory != workspace.require_path():
        raise ValueError("ASLRef execution directory differs from bounded workspace")
    stdout_path = directory / "aslref.stdout"
    stderr_path = directory / "aslref.stderr"
    environment = os.environ.copy()
    environment["PYTHONHASHSEED"] = "0"
    started = time.perf_counter()
    peak_kib = 0
    sampling_supported = True

    def observe(process: Any) -> str | None:
        nonlocal peak_kib, sampling_supported
        sample = _resident_size_kib(process.pid)
        if sample is None:
            sampling_supported = False
            return None
        peak_kib = max(peak_kib, sample)
        if peak_kib > max_rss_kib:
            return f"ASLRef RSS {peak_kib} KiB exceeds {max_rss_kib} KiB"
        return None

    completed = run_bounded(
        command,
        workspace=workspace,
        timeout_seconds=timeout,
        environment=environment,
        process_observer=observe,
    )
    workspace.write_text(stdout_path, completed.stdout)
    workspace.write_text(stderr_path, completed.stderr)
    child_peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    child_peak_kib = (
        int(child_peak) // 1024 if sys.platform == "darwin" else int(child_peak)
    )
    peak_kib = max(peak_kib, child_peak_kib)
    if peak_kib > max_rss_kib:
        raise ValueError(
            f"ASLRef completed above RSS cap: {peak_kib} > {max_rss_kib} KiB"
        )
    elapsed = time.perf_counter() - started
    return (
        int(completed.returncode),
        completed.stdout,
        completed.stderr,
        elapsed,
        peak_kib,
        (
            "native-rss+rusage"
            if sampling_supported
            else "rusage-children-fallback"
        ),
        "exit",
    )


def run(
    options: argparse.Namespace,
    directory: Path,
    workspace: BoundedWorkspace,
) -> dict[str, object]:
    if directory != workspace.require_path():
        raise ValueError("materialization directory differs from bounded workspace")
    if options.max_contracts <= 0 or options.count > options.max_contracts:
        raise ValueError("contract count exceeds the configured hard cap")
    maximum_artifact_bytes = int(
        getattr(options, "max_artifact_bytes", MAX_ARTIFACT_BYTES)
    )
    maximum_log_bytes = int(getattr(options, "max_log_bytes", MAX_LOG_BYTES))
    maximum_artifact_files = int(
        getattr(options, "max_artifact_files", MAX_ARTIFACT_FILES)
    )
    if (
        maximum_artifact_bytes <= 0
        or maximum_artifact_files <= 0
        or maximum_log_bytes <= 0
    ):
        raise ValueError("artifact/log caps must be positive")
    runner = BoundedCommandRunner(workspace, options.timeout_seconds)
    if options.diagnostic_drop_maximum and options.count != 3:
        raise ValueError(
            "the drop-maximum diagnostic executes exactly three contracts"
        )
    full_extent_mode = options.full_extent_page_size is not None
    campaign_mode = bool(options.input_jsonl)
    scale_mode = not full_extent_mode and not campaign_mode and options.count > 4
    if campaign_mode and (
        full_extent_mode
        or options.diagnostic_drop_maximum
        or options.diagnostic_maximum_phase != "full"
    ):
        raise ValueError(
            "explicit campaign inputs cannot be combined with full-extent or "
            "maximum-phase diagnostics"
        )
    if full_extent_mode and (
        options.count != 1
        or options.diagnostic_drop_maximum
        or options.diagnostic_maximum_phase != "full"
    ):
        raise ValueError(
            "full-extent execution requires count 1 and no diagnostics"
        )
    if full_extent_mode and (
        options.timeout_seconds < FULL_EXTENT_MIN_TIMEOUT_SECONDS
        or options.max_rss_kib < FULL_EXTENT_MIN_RSS_KIB
    ):
        raise ValueError(
            "full-extent execution requires at least 300 seconds and 9 GiB "
            "of measured RSS allowance"
        )
    if options.diagnostic_maximum_phase != "full" and (
        options.count != 4 or options.diagnostic_drop_maximum
    ):
        raise ValueError(
            "maximum-phase diagnostics require the exact four-contract batch"
        )
    promotable = (
        not options.diagnostic_drop_maximum
        and options.diagnostic_maximum_phase == "full"
        and not scale_mode
        and not campaign_mode
    )
    pac_dependency = verify_pac_dependency(
        options.architecture_root, options.features_json
    )
    if campaign_mode:
        source_records = list(iter_raw_materializations(options.input_jsonl))
        identifiers = [str(record["input_contract_id"]) for record in source_records]
        if len(source_records) != options.count:
            raise ValueError(
                "explicit campaign input count differs from --count"
            )
        if len(identifiers) != len(set(identifiers)):
            raise ValueError("explicit campaign inputs are not unique")
        closure_seed_records = selected_records(
            options.closure_capture_directory, 4
        )
    elif full_extent_mode:
        source_records = [
            selected_full_extent_transfer_record(
                options.capture_directory, options.full_extent_page_size
            )
        ]
        closure_seed_records = selected_records(
            options.closure_capture_directory, 4
        )
    else:
        source_records = selected_records(
            options.capture_directory,
            options.count + (1 if options.diagnostic_drop_maximum else 0),
        )
        closure_seed_records = []
    preparation_records = [*source_records, *closure_seed_records]
    records = source_records[1:] if options.diagnostic_drop_maximum else source_records
    if len(records) != options.count:
        raise ValueError("executed contract selection count differs")
    for record in preparation_records:
        obligations = representation_obligations(record)
        if obligations:
            raise ValueError(
                f"{record['input_contract_id']}: pre-execution obligations "
                + "+".join(obligations)
            )
    module_paths, declarations, _owners = load_interface_modules(
        options.interface, None
    )
    owners = sorted(module_paths)
    observer_text = options.observer.read_text(encoding="utf-8")
    supplied = frozenset(
        symbol for owner in owners for symbol in declarations[owner]
    ) | interface_declarations(observer_text)
    supplemental = tuple(
        module_paths[owner].read_text(encoding="utf-8") for owner in owners
    ) + (observer_text,)
    prepared = prepare_sources(
        directory,
        preparation_records,
        read_required(options.required_variants),
        options.architecture_root,
        options.bundler,
        supplied,
        supplemental,
        runner,
    )
    require_generated_artifact_limit(
        directory, maximum_artifact_bytes, maximum_artifact_files
    )
    workspace.check()
    harness_text, factoring = render_harness(
        records, prepared, options.diagnostic_maximum_phase
    )
    encoded_bytes = len(harness_text.encode("utf-8"))
    if encoded_bytes > options.max_encoded_bytes:
        raise ValueError(
            f"encoded batch bytes {encoded_bytes} exceed hard cap "
            f"{options.max_encoded_bytes}"
        )
    harness = directory / "batch-harness.asl"
    workspace.write_text(harness, harness_text)
    require_generated_artifact_limit(
        directory, maximum_artifact_bytes, maximum_artifact_files
    )
    workspace.check()
    official_texts = [
        *(path.read_text(encoding="utf-8") for path in prepared.instructions),
        prepared.shared.read_text(encoding="utf-8"),
    ]
    supplied_texts = [
        options.interface.read_text(encoding="utf-8"),
        *(module_paths[owner].read_text(encoding="utf-8") for owner in owners),
        observer_text,
    ]
    memory_module_texts = [
        module_paths[owner].read_text(encoding="utf-8")
        for owner in owners
        if module_paths[owner].name == "a64_aslref_state_memory.asl"
    ]
    if len(memory_module_texts) != 1:
        raise ValueError("expected one supplied memory-domain module")
    fault_domain_model = verify_fault_domain_model(
        official_texts, memory_module_texts[0]
    )
    source_texts = [*official_texts, *supplied_texts, harness_text]
    interface_signatures = verify_supplied_interface_signature_closure(
        official_texts, supplied_texts, harness_text
    )
    impdef_completeness = verify_impdef_completeness(
        source_texts[len(prepared.instructions)], source_texts
    )
    reachable, _definitions = conservative_reachable_functions(
        source_texts, ("main",)
    )
    reachable_external_impdef = sorted(
        name for name in reachable if "IMPDEF" in name
    )
    if reachable_external_impdef != ["ComputePAC2IMPDEF", "ComputePACIMPDEF"]:
        raise ValueError(
            "reachable external implementation-defined projection set drifted"
        )
    impdef_completeness["external_implementation_defined_projections"] = (
        reachable_external_impdef
    )
    tuple_official_texts, tuple_normalization = (
        normalize_reachable_tuple_forwards(
            official_texts, supplied_texts, harness_text
        )
    )
    execution_official_texts, virtual_fault_projection = (
        normalize_virtual_fault_projection(tuple_official_texts)
    )
    execution_official_paths: list[Path] = []
    for index, text in enumerate(execution_official_texts):
        path = directory / f"official-execution-{index:03d}.asl"
        workspace.write_text(path, text)
        execution_official_paths.append(path)
    execution_instructions = execution_official_paths[
        : len(prepared.instructions)
    ]
    execution_shared = execution_official_paths[len(prepared.instructions)]
    command = [str(options.aslref), "--type-check-strict", "--exec"]
    for path in execution_instructions:
        command.extend(["-1", str(path)])
    command.extend(
        [
            "-1",
            str(execution_shared),
            "-1",
            str(options.interface),
        ]
    )
    for owner in owners:
        command.extend(["-1", str(module_paths[owner])])
    command.extend(["-1", str(options.observer), str(harness)])
    invocation_path = directory / "aslref-invocation.json"
    invocation_sources = [
        *execution_official_paths,
        options.interface,
        *(module_paths[owner] for owner in owners),
        options.observer,
        harness,
    ]
    workspace.write_text(
        invocation_path,
        json.dumps(
            {
                "schema": "uml.a64.aslref-materialization-batch-invocation/v1",
                "command": command,
                "timeout_seconds": options.timeout_seconds,
                "max_rss_kib": options.max_rss_kib,
                "diagnostic_maximum_phase": (
                    options.diagnostic_maximum_phase
                ),
                "promotable": promotable,
                "tuple_forward_normalization": tuple_normalization,
                "virtual_fault_projection": virtual_fault_projection,
                "fault_domain_model": fault_domain_model,
                "input_sha256": {
                    str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in invocation_sources
                },
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
    )
    require_generated_artifact_limit(
        directory, maximum_artifact_bytes, maximum_artifact_files
    )
    workspace.check()
    (
        returncode,
        stdout,
        stderr,
        elapsed,
        peak_kib,
        rss_measurement,
        termination_reason,
    ) = execute_measured(
        command,
        options.timeout_seconds,
        options.max_rss_kib,
        maximum_log_bytes,
        directory,
        workspace,
    )
    require_generated_artifact_limit(
        directory, maximum_artifact_bytes, maximum_artifact_files
    )
    workspace.check()
    result_path = directory / "aslref-result.json"
    result = {
        "schema": "uml.a64.aslref-materialization-batch-result/v1",
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "elapsed_seconds": round(elapsed, 6),
        "peak_rss_kib": peak_kib,
        "rss_measurement": rss_measurement,
        "termination_reason": termination_reason,
        "accepted": False,
        "passed": False,
        "promotable": promotable,
        "diagnostic": not promotable,
        "diagnostic_maximum_phase": options.diagnostic_maximum_phase,
    }
    expected_output = (
        f"FORMAT={FORMAT}|COUNT={options.count}|FAILURES=0"
    )
    if (
        termination_reason != "exit"
        or returncode != 0
        or stdout.strip() != expected_output
    ):
        raise ValueError(
            f"ASLRef batch failed reason={termination_reason}, exit={returncode}, "
            f"elapsed={elapsed:.6f}s, "
            f"peak_rss_kib={peak_kib}, stdout={stdout.strip()!r}: "
            + stderr.strip()[-3000:]
    )
    result["passed"] = True
    result["accepted"] = promotable
    workspace.write_text(
        result_path, json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    require_generated_artifact_limit(
        directory, maximum_artifact_bytes, maximum_artifact_files
    )
    workspace.check()
    fixed_sources = [
        *execution_instructions,
        execution_shared,
        options.interface,
        *(module_paths[owner] for owner in owners),
        options.observer,
    ]
    total_source_bytes = encoded_bytes + sum(
        path.stat().st_size for path in fixed_sources
    )
    categories = {
        "maximum_backing_memory": sum(
            sum(
                int(str(region["size"]), 16)
                for region in record["memory_regions"]  # type: ignore[index]
            )
            == COMPACT_MAX_MEMORY_LENGTH
            for record in records
        ),
        "full_extent_65536_transfer": sum(
            int(str(record["initial_state"]["x"][5]), 16)  # type: ignore[index]
            == COMPACT_MAX_MEMORY_LENGTH
            and mops_contract_kind(record) == "prologue-main-epilogue"
            for record in records
        ),
        "multi_region": sum(
            len(record["memory_regions"]) >= 3  # type: ignore[arg-type]
            for record in records
        ),
        "fault": sum(
            record["terminal"]["event"] == "memory-fault"  # type: ignore[index]
            for record in records
        ),
        "callback": sum(
            record["representability"]["callback_contracts"]["validator"]  # type: ignore[index]
            is not None
            for record in records
        ),
        "multiword": sum(
            len(record["program"]["words"]) > 1  # type: ignore[index]
            for record in records
        ),
        "mops_full_sequence": sum(
            mops_contract_kind(record) == "prologue-main-epilogue"
            for record in records
        ),
        "mops_restart_stage": sum(
            str(mops_contract_kind(record)).startswith("restart-")
            for record in records
        ),
    }
    full_extent_report = (
        verify_full_extent_transfer_contract(
            source_records[0], options.full_extent_page_size
        )
        if full_extent_mode
        else None
    )
    generated_artifact_bytes = sum(
        path.stat().st_size for path in directory.rglob("*") if path.is_file()
    )
    executed_identifiers = [str(record["input_contract_id"]) for record in records]
    page_sizes = {
        int(str(record["program"]["guest_page_size"]), 16)  # type: ignore[index]
        for record in records
    }
    if campaign_mode and len(page_sizes) != 1:
        raise ValueError("explicit campaign shard mixes guest page profiles")
    return {
        "schema": FORMAT,
        "contracts": options.count,
        "selection": (
            f"full-extent-65536-transfer-{options.full_extent_page_size}"
            if full_extent_mode
            else (
                "explicit-unique-campaign-shard"
                if campaign_mode
                else (
                    "exact-four-minus-maximum-diagnostic"
                    if options.diagnostic_drop_maximum
                    else (
                        "strict-four-plus-nonmaximum-cycle"
                        if scale_mode
                        else "bounded-representative"
                    )
                )
            )
        ),
        "promotable": promotable,
        "diagnostic": not promotable,
        "diagnostic_maximum_phase": options.diagnostic_maximum_phase,
        "official_source_contracts": len(preparation_records),
        "official_closure_seed_contracts": len(closure_seed_records),
        "executed_source_contracts": len(source_records),
        "unique_input_contracts": len(
            {str(record["input_contract_id"]) for record in source_records}
        ),
        "scale_workflow": (
            {
                "purpose": "workflow-resource-scalability-only",
                "semantic_coverage_promotion": "forbidden",
                "strict_contracts": 4,
                "repeated_nonmaximum_contracts": options.count - 4,
                "unique_input_contracts": 4,
                "failures": 0,
            }
            if scale_mode
            else None
        ),
        "campaign_workflow": (
            {
                "purpose": "official-execution-shard-pending-campaign-join",
                "semantic_coverage_promotion": "requires-exact-join",
                "unique_input_contracts": len(source_records),
                "failures": 0,
                "guest_page_size": next(iter(page_sizes)),
                "input_contract_ids_sha256": ordered_identifier_digest(
                    executed_identifiers
                ),
                "input_result_count": len(executed_identifiers),
                "input_results_sha256": official_input_result_digest(
                    executed_identifiers
                ),
                "input_results": [
                    {
                        "input_contract_id": identifier,
                        "official_asl_result": "official-asl-passed",
                    }
                    for identifier in executed_identifiers
                ],
                "input_sources": [
                    {
                        "path": str(path.resolve()),
                    }
                    for path in options.input_jsonl
                ],
            }
            if campaign_mode
            else None
        ),
        "encoded_payload_source_bytes": encoded_bytes,
        "generated_artifact_bytes": generated_artifact_bytes,
        "total_asl_source_bytes": total_source_bytes,
        "official_instruction_source_bytes": sum(
            path.stat().st_size for path in execution_instructions
        ),
        "official_shared_source_bytes": execution_shared.stat().st_size,
        "canonical_official_source_bytes": sum(
            len(text.encode("utf-8")) for text in official_texts
        ),
        "execution_official_source_bytes": sum(
            len(text.encode("utf-8")) for text in execution_official_texts
        ),
        "peak_rss_kib": peak_kib,
        "rss_measurement": rss_measurement,
        "termination_reason": termination_reason,
        "elapsed_seconds": round(elapsed, 6),
        "contracts_per_second": round(options.count / elapsed, 6),
        "count_cap": options.max_contracts,
        "encoded_byte_cap": options.max_encoded_bytes,
        "normalization_relation_bits": "1111",
        "pac_dependency": pac_dependency,
        "implementation_defined": impdef_completeness,
        "supplied_interface_signatures": interface_signatures,
        "tuple_forward_normalization": tuple_normalization,
        "virtual_fault_projection": virtual_fault_projection,
        "fault_domain_model": fault_domain_model,
        "factoring": factoring,
        "maximum_backing_contract": (
            None
            if full_extent_mode or campaign_mode
            else verify_compact_max_backing_contract(source_records[0])
        ),
        "full_extent_transfer_contract": full_extent_report,
        "full_extent_execution_policy": (
            {
                "mode": "explicit-standalone-opt-in",
                "ordinary_ctest_registered": False,
                "minimum_timeout_seconds": FULL_EXTENT_MIN_TIMEOUT_SECONDS,
                "minimum_max_rss_kib": FULL_EXTENT_MIN_RSS_KIB,
                "resource_lock": FULL_EXTENT_RESOURCE_LOCK,
                "success_only_acceptance": True,
            }
            if full_extent_mode
            else None
        ),
        "maximum_transfer_65536_gate": {
            "status": "open",
            "captured_in_strict_batch": False,
            "official_asl_execution": full_extent_mode and promotable,
            "official_asl_guest_page_sizes": (
                [options.full_extent_page_size] if full_extent_mode else []
            ),
            "required_guest_page_sizes": [4096, 16384],
            "required_engines": ["interpreter", "gadgets", "jit"],
            "independent_native_oracle": "required-when-host-supports-mops",
            "current_host_limitation": "jit-and-native-oracle-lack-mops",
        },
        "categories": categories,
        "source_sha256": hashlib.sha256(harness_text.encode()).hexdigest(),
    }


def _generated_directory_sources(
    options: argparse.Namespace,
) -> tuple[tuple[Path, ...], tuple[Path, ...]]:
    source_files = tuple(
        require_no_symlink_components(path, "generated-directory source file")
        for path in (
            options.aslref,
            options.bundler,
            options.features_json,
            options.required_variants,
            options.interface,
            options.observer,
            *options.input_jsonl,
        )
    )
    source_directories = tuple(
        sorted(
            {
                require_no_symlink_components(
                    path, "generated-directory source tree"
                )
                for path in (
                    options.architecture_root,
                    options.capture_directory,
                    options.closure_capture_directory,
                    *(path.parent for path in source_files),
                )
            },
            key=str,
        )
    )
    return source_directories, source_files


def canonical_generated_directory_target(options: argparse.Namespace) -> Path:
    target = require_no_symlink_components(
        options.generated_directory, "generated directory"
    )
    if target.exists():
        raise ValueError("generated directory already exists")
    parent = require_no_symlink_components(
        target.parent, "generated directory parent"
    )
    if not parent.is_dir():
        raise ValueError("generated directory parent is not an existing directory")
    source_directories, source_files = _generated_directory_sources(options)
    for source in (*source_directories, *source_files):
        if (
            paths_alias(target, source)
            or path_is_ancestor(target, source)
            or path_is_ancestor(source, target)
        ):
            raise ValueError(
                "generated directory aliases a source/input/capture tree: "
                f"{source}"
            )
    return target


def require_generated_directory_capacity(
    parent: Path, minimum_free_gib: object, maximum_artifact_bytes: object
) -> int:
    if (
        isinstance(minimum_free_gib, bool)
        or not isinstance(minimum_free_gib, (int, float))
        or not math.isfinite(minimum_free_gib)
        or minimum_free_gib < 0
    ):
        raise ValueError("generated-directory free-space reserve is invalid")
    if (
        isinstance(maximum_artifact_bytes, bool)
        or not isinstance(maximum_artifact_bytes, int)
        or maximum_artifact_bytes <= 0
    ):
        raise ValueError("generated artifact byte cap must be positive")
    required = (
        int(minimum_free_gib * 1024 * 1024 * 1024)
        + maximum_artifact_bytes
    )
    free = shutil.disk_usage(parent).free
    if free < required:
        raise ValueError(
            f"generated-directory free space {free} is below reserve {required}"
        )
    return required


def prepare_generated_directory(options: argparse.Namespace) -> Path:
    target = canonical_generated_directory_target(options)
    require_generated_directory_capacity(
        target.parent,
        options.minimum_generated_free_gib,
        options.max_artifact_bytes,
    )
    target.mkdir(exist_ok=False)
    return target


def probe_generation_limits(options: argparse.Namespace) -> GenerationLimits:
    reserve = options.minimum_generated_free_gib
    for value, label in (
        (options.max_artifact_bytes, "generated artifact byte cap"),
        (options.max_artifact_files, "generated artifact file cap"),
        (options.max_log_bytes, "generated log byte cap"),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError(f"{label} must be positive")
    if (
        isinstance(reserve, bool)
        or not isinstance(reserve, (int, float))
        or not math.isfinite(reserve)
        or reserve < 0
    ):
        raise ValueError("generated-directory free-space reserve is invalid")
    return GenerationLimits(
        # The shared workspace contract requires a positive reserve; retain
        # the CLI's historical zero-reserve spelling as a one-byte floor.
        free_space_reserve_bytes=max(1, int(reserve * 1024 * 1024 * 1024)),
        max_workspace_bytes=options.max_artifact_bytes,
        max_artifact_bytes=options.max_artifact_bytes,
        max_log_bytes=options.max_log_bytes,
        max_workspace_files=options.max_artifact_files,
    )


def main() -> int:
    options = arguments()
    required = (
        options.architecture_root,
        options.aslref,
        options.bundler,
        options.features_json,
        options.required_variants,
        options.interface,
        options.observer,
    )
    if not options.architecture_root.is_dir() or any(
        not path.is_file() for path in required[1:]
    ):
        raise ValueError("one or more pinned probe inputs are absent")
    limits = probe_generation_limits(options)
    repository = Path(__file__).resolve().parents[3]
    if options.generated_directory is None:
        workspace_context = BoundedWorkspace(
            source_root=repository,
            publication_targets=(),
            work_root=None,
            limits=limits,
            prefix="a64-materialization-batch-probe-",
        )
    else:
        options.generated_directory = prepare_generated_directory(options)
        workspace_context = BoundedWorkspace.adopt(
            options.generated_directory,
            source_root=repository,
            publication_targets=(),
            limits=limits,
        )
    with workspace_context as workspace:
        directory = workspace.require_path()
        report = run(options, directory, workspace)
        if options.generated_directory is not None:
            workspace.write_text(
                directory / "batch-report.json",
                json.dumps(
                    report,
                    ensure_ascii=True,
                    sort_keys=True,
                    separators=(",", ":"),
                ) + "\n",
            )
        require_generated_artifact_limit(
            directory,
            int(getattr(options, "max_artifact_bytes", MAX_ARTIFACT_BYTES)),
            int(getattr(options, "max_artifact_files", MAX_ARTIFACT_FILES)),
        )
        workspace.check()
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
