#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute allocation witnesses through pinned official A64 ASL semantics.

The runner composes the frozen official dependency closure with deterministic
state adapters, enables exactly the features in each checked allocation
predicate, executes explicit input partitions, and independently projects the
complete terminal register/vector/PSTATE/event/memory observation into stable
digests.  Engine or hardware agreement is not semantic authority, and this
runner never promotes behavioral completeness.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

from a64_aslref_dependencies import (
    AslDependencyCatalog,
    interface_declarations,
)
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from a64_aslref_state_digest import (
    GUEST_PC,
    architecture_state_digest,
    exact_outcome_digest,
    memory_region_digest,
)
from generate_a64_aslref_behavior_status import status_id
from generate_a64_aslref_corpus import (
    STATE_MODULE_FILENAMES,
    allocation_witness,
    load_interface_modules,
)
from generate_a64_instruction_allocations import archive_root
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from probe_a64_aslref_batch import SUPPORT_DIGESTS, sha256
from run_a64_aslref_specimen import ASLREF_VERSION


FORMAT = "a64-aslref-execution-v1"
MEMORY_SIZE = 4096
MEMORY_TAGS = 256
PATTERN_MEMORY_BASE = 0x100000
FEATURE = re.compile(r"FEAT_[A-Za-z0-9_]+")
FUNCTION = re.compile(r"(?m)^func\s+(instr_[A-Za-z0-9_]+)\s*\(")

REPRESENTATIVE_FORMS = (
    ("add_addsub_imm.xml", "ADD_64_addsub_imm"),
    ("mov_add_addsub_imm.xml", "MOV_ADD_64_addsub_imm"),
    ("b_uncond.xml", "B_only_branch_imm"),
    ("cbz.xml", "CBZ_64_compbranch"),
    ("ldr_imm_gen.xml", "LDR_64_ldst_pos"),
    ("str_imm_gen.xml", "STR_64_ldst_pos"),
    ("fadd_float.xml", "FADD_D_floatdp2"),
    ("add_advsimd.xml", "ADD_asimdsame_only"),
    ("wfe.xml", "WFE_HI_hints"),
    ("svc.xml", "SVC_EX_exception"),
    ("mrs.xml", "MRS_RS_systemmove"),
    ("smstart_msr_imm.xml", "SMSTART_MSR_SI_pstate"),
)

COLUMNS = [
    "oracle_execution_id",
    "allocation_status_id",
    "stable_id",
    "section",
    "architectural_form",
    "architecture_file",
    "architecture_encoding",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "decode_class",
    "decode_encoding",
    "decode_mask",
    "decode_value",
    "decode_constraints",
    "allocation_kind",
    "allocation_witness_encoding",
    "witness_encoding",
    "semantic_witness_choices",
    "input_partition",
    "input_partition_digest",
    "required_partition_set",
    "observed_partition_set",
    "partition_coverage_status",
    "feature_profile",
    "official_operation_source",
    "official_operation_function",
    "official_operation_digest",
    "generated_operation_digest",
    "adapter_digest",
    "execution_eligibility",
    "execution_status",
    "observation_boundary",
    "terminal_event",
    "exception_type",
    "fault_status",
    "memory_event",
    "memory_status",
    "sysreg_outcome",
    "initial_pc",
    "raw_terminal_pc",
    "architectural_terminal_pc",
    "initial_state_digest",
    "initial_memory_digest",
    "observed_state_digest",
    "observed_memory_digest",
    "observed_memory_sha256",
    "observed_event_digest",
    "semantic_expected_join",
    "allocation_behavior_complete",
    "unresolved_requirements",
    "architecture_oracle",
    "aslref_oracle",
]


@dataclass(frozen=True)
class Partition:
    name: str
    initialization: str
    memory_fault: bool = False
    stop_on_wait: bool = False
    fp_trap: bool = False

    def definition(self, row: dict[str, str], witness: int) -> dict[str, object]:
        return {
            "schema": FORMAT,
            "partition": self.name,
            "initialization": self.initialization,
            "witness": f"{witness:08x}",
            "features": feature_tokens(row),
            "memory_fault": self.memory_fault,
            "stop_on_wait": self.stop_on_wait,
            "fp_trap": self.fp_trap,
            "pc": f"{GUEST_PC:016x}",
            "memory_size": MEMORY_SIZE,
        }

    def digest(self, row: dict[str, str], witness: int) -> str:
        encoded = json.dumps(
            self.definition(row, witness),
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        return hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True)
class PreparedForm:
    instructions: Path
    shared: Path
    source_file: str
    generated_digest: str
    functions: tuple[str, ...]
    instructions_text: str


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--bundler", type=Path, required=True)
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
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
        "--state-module", type=Path, action="append", default=None
    )
    parser.add_argument(
        "--scope", choices=("representative", "all"), default="representative"
    )
    parser.add_argument("--max-allocations", type=int)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_execution_oracle.tsv",
    )
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_required(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != REQUIRED_COLUMNS:
            raise ValueError(f"{path}: unexpected columns {reader.fieldnames!r}")
        rows = list(reader)
    if len(rows) != 2396:
        raise ValueError(f"{path}: expected 2,396 allocations, found {len(rows)}")
    return rows


def selected_rows(
    rows: list[dict[str, str]], scope: str, maximum: int | None
) -> list[dict[str, str]]:
    if scope == "all":
        selected = list(rows)
    else:
        indexed = {
            (row["architecture_file"], row["architecture_encoding"]): row
            for row in rows
        }
        missing = [identity for identity in REPRESENTATIVE_FORMS if identity not in indexed]
        if missing:
            raise ValueError(f"representative allocations are missing: {missing!r}")
        selected = [indexed[identity] for identity in REPRESENTATIVE_FORMS]
    if maximum is not None:
        if maximum <= 0:
            raise ValueError("--max-allocations must be positive")
        selected = selected[:maximum]
    return selected


def feature_tokens(row: dict[str, str]) -> tuple[str, ...]:
    expression = row["static_feature_expressions"]
    if expression == "baseline":
        return ()
    tokens = tuple(sorted(set(FEATURE.findall(expression))))
    reconstructed = " && ".join(tokens)
    if not tokens or reconstructed != expression:
        raise ValueError(
            f"{row['stable_id']}: unsupported feature expression {expression!r}"
        )
    if len(tokens) > 8:
        raise ValueError(f"{row['stable_id']}: feature profile exceeds adapter bound")
    return tokens


def exact_function(
    xml_path: Path,
    architecture_encoding: str,
    functions: tuple[str, ...],
) -> str:
    """Map one XML encoding to the exact bundler entry procedure."""
    if len(functions) == 1:
        return functions[0]
    root = ET.parse(xml_path).getroot()
    matches: list[str] = []
    for iclass in root.findall(".//iclass"):
        if not any(
            encoding.get("name") == architecture_encoding
            for encoding in iclass.findall(".//encoding")
        ):
            continue
        identifier = iclass.get("id", "")
        matches.extend(
            function for function in functions if function.endswith(identifier)
        )
    unique = sorted(set(matches))
    if len(unique) != 1:
        raise ValueError(
            f"{xml_path.name}:{architecture_encoding}: maps to {unique!r} "
            f"among {functions!r}"
        )
    return unique[0]


def semantic_witness(
    row: dict[str, str], root: Path
) -> tuple[int, tuple[str, ...]]:
    """Avoid values explicitly marked RESERVED by the form's value tables.

    The allocation masks intentionally leave operand fields free.  Some alias
    forms document a reserved operand value only in an explanation table, not
    in the allocation mask (SMSTART's optional-mode field is one example).
    This deterministic refinement remains inside both architecture and decode
    predicates and records every changed field as input evidence.
    """
    word = allocation_witness(row)
    path = root / row["architecture_file"]
    xml = ET.parse(path).getroot()
    iclasses = [
        iclass
        for iclass in xml.findall(".//iclass")
        if any(
            encoding.get("name") == row["architecture_encoding"]
            for encoding in iclass.findall(".//encoding")
        )
    ]
    if len(iclasses) != 1:
        return word, ()
    boxes = {
        box.get("name", ""): (int(box.get("hibit", "-1")), int(box.get("width", "1")))
        for box in iclasses[0].findall("./regdiagram/box")
        if box.get("name")
    }
    fixed_mask = int(row["architecture_mask"], 16) | int(row["decode_mask"], 16)
    choices: list[str] = []
    for explanation in xml.findall(".//explanation"):
        encodings = {
            value.strip() for value in explanation.get("enclist", "").split(",")
        }
        if row["architecture_encoding"] not in encodings:
            continue
        for definition in explanation.findall("./definition"):
            encoded = definition.get("encodedin", "")
            match = re.fullmatch(
                r"([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)(?::(\d+))?\])?",
                encoded,
            )
            if match is None or match.group(1) not in boxes:
                continue
            entries: list[tuple[str, str]] = []
            for table_row in definition.findall(".//table[@class='valuetable']//tbody/row"):
                bitfield = table_row.find("./entry[@class='bitfield']")
                symbol = table_row.find("./entry[@class='symbol']")
                if bitfield is None or symbol is None:
                    continue
                bits = "".join(bitfield.itertext()).strip().replace(" ", "")
                label = "".join(symbol.itertext()).strip()
                if re.fullmatch(r"[01x]+", bits):
                    entries.append((bits, label))
            if not entries:
                continue
            hibit, width = boxes[match.group(1)]
            field_low = hibit - width + 1
            slice_high = int(match.group(2)) if match.group(2) else width - 1
            slice_low = int(match.group(3)) if match.group(3) else (
                slice_high if match.group(2) else 0
            )
            slice_width = slice_high - slice_low + 1
            if slice_width <= 0 or any(len(bits) != slice_width for bits, _ in entries):
                continue
            actual = "".join(
                "1" if word & (1 << (field_low + bit)) else "0"
                for bit in range(slice_high, slice_low - 1, -1)
            )
            actual_labels = [
                label
                for bits, label in entries
                if all(expected == "x" or expected == found for expected, found in zip(bits, actual))
            ]
            if not actual_labels or not all(
                re.search(r"RESERVED|UNDEFINED", label, re.IGNORECASE)
                for label in actual_labels
            ):
                continue
            candidates = [
                (bits, label)
                for bits, label in entries
                if not re.search(r"RESERVED|UNDEFINED", label, re.IGNORECASE)
            ]
            selected: tuple[str, str] | None = None
            for bits, label in candidates:
                proposed = word
                compatible = True
                for offset, value in enumerate(reversed(bits)):
                    if value == "x":
                        continue
                    bit = field_low + slice_low + offset
                    desired = value == "1"
                    if fixed_mask & (1 << bit) and bool(word & (1 << bit)) != desired:
                        compatible = False
                        break
                    if desired:
                        proposed |= 1 << bit
                    else:
                        proposed &= ~(1 << bit)
                if compatible:
                    word = proposed
                    selected = (bits, label)
                    break
            if selected is None:
                raise ValueError(
                    f"{row['stable_id']}:{encoded}: reserved witness has no compatible value"
                )
            choices.append(f"{encoded}={selected[0]}:{selected[1]}")
    return word, tuple(choices)


def function_source(text: str, function: str) -> str:
    start = text.find(f"func {function} ")
    if start < 0:
        start = text.find(f"func {function}(")
    if start < 0:
        raise ValueError(f"generated source lacks {function}")
    next_match = re.search(r"(?m)^func\s+instr_", text[start + 1 :])
    end = len(text) if next_match is None else start + 1 + next_match.start()
    return text[start:end]


def partitions_for(source: str) -> tuple[Partition, ...]:
    partitions = [Partition("zero-state-v1", "zero")]
    partitions.append(Partition("pattern-state-v1", "pattern"))
    if re.search(r"\bMem(?:A|O|S|U|U_unpriv)?\s*\{", source):
        partitions.append(
            Partition("injected-memory-fault-v1", "pattern", memory_fault=True)
        )
    if re.search(r"\bHint_WF(?:E|ET|I|IT)\s*\(", source):
        partitions.append(
            Partition("wait-terminal-policy-v1", "zero", stop_on_wait=True)
        )
    if re.search(r"\bAArch64_CheckFP(?:AdvSIMD)?Enabled\s*\(", source):
        partitions.append(
            Partition("fp-access-trap-v1", "pattern", fp_trap=True)
        )
    return tuple(partitions)


def initial_projection(partition: Partition) -> tuple[dict[str, object], bytes, list[int]]:
    if partition.initialization == "zero":
        registers = [0] * 31
        sp = 0
        nzcv = 0
        vectors = [0] * 32
        memory = bytes(MEMORY_SIZE)
        tags = [0] * MEMORY_TAGS
        base = 0
    elif partition.initialization == "pattern":
        registers = [PATTERN_MEMORY_BASE + 256 + index * 16 for index in range(31)]
        sp = PATTERN_MEMORY_BASE + 2048
        nzcv = 0xA
        vectors = [int.from_bytes(bytes([index + 1]) * 16, "little") for index in range(32)]
        memory = bytes((3 + 17 * index) & 0xFF for index in range(MEMORY_SIZE))
        tags = [index & 0xF for index in range(MEMORY_TAGS)]
        base = PATTERN_MEMORY_BASE
    else:
        raise ValueError(f"unknown initialization {partition.initialization!r}")
    state: dict[str, object] = {
        "registers": registers,
        "sp": sp,
        "pc": GUEST_PC,
        "nzcv": nzcv,
        "vectors": vectors,
        "fpcr": 0,
        "fpsr": 0,
        "fpmr": 0,
        "pstate_ssbs": False,
        "pstate_dit": False,
        "pstate_tco": False,
        "pstate_sm": False,
        "pstate_za": False,
        "pstate_daif": 0,
        "memory_base": base,
    }
    return state, memory, tags


def _print(key: str, expression: str) -> str:
    return f'    print "|{key}=", {expression};'


def harness_text(
    row: dict[str, str], witness: int, function: str, partition: Partition
) -> str:
    instruction = f"{witness:032b}"
    initializer = (
        "A64Execution_InitializeZero"
        if partition.initialization == "zero"
        else "A64Execution_InitializePattern"
    )
    lines = [
        "func main() => integer",
        "begin",
        f"    {initializer}('{instruction}');",
    ]
    lines.extend(
        f"    A64Oracle_EnableFeature({feature});"
        for feature in feature_tokens(row)
    )
    if partition.memory_fault:
        lines.append("    A64Oracle_SetMemoryFault(Fault_Translation);")
    if partition.stop_on_wait:
        lines.append("    A64Oracle_StopOnWaitHint = TRUE;")
    if partition.fp_trap:
        lines.append("    A64Oracle_FPAccessTrap = TRUE;")
    lines.extend(
        [
            "    var caught_terminal : boolean = FALSE;",
            "    try",
            f"        {function}('{instruction}');",
            "    catch",
            "        when outcome : A64Oracle_TerminalOutcome =>",
            "            caught_terminal = TRUE;",
            "        otherwise =>",
            '            print "ORACLE_UNEXPECTED_EXCEPTION";',
            "            return 3;",
            "    end;",
            f'    print "FORMAT={FORMAT}";',
            _print("CAUGHT", "caught_terminal"),
        ]
    )
    for index in range(31):
        lines.append(_print(f"X{index:02d}", f"A64Oracle_GetX({index})"))
    lines.extend(
        [
            _print("SP", "SP{64}()"),
            _print("PC", "A64Oracle_GetPC()"),
            _print("NZCV", "A64Oracle_GetArchitecturalNZCV()"),
        ]
    )
    for index in range(32):
        lines.append(_print(f"V{index:02d}", f"A64Oracle_GetV128({index})"))
    lines.extend(
        [
            _print("FPCR", "A64Oracle_FPCR[31:0]"),
            _print("FPSR", "A64Oracle_FPSR[31:0]"),
            _print("FPMR", "A64Oracle_FPMR"),
            _print("PSSBS", "PSTATE.SSBS"),
            _print("PDIT", "PSTATE.DIT"),
            _print("PTCO", "PSTATE.TCO"),
            _print("PSM", "PSTATE.SM"),
            _print("PZA", "PSTATE.ZA"),
            _print("PDAIF", "PSTATE.D :: PSTATE.A :: PSTATE.I :: PSTATE.F"),
            _print("TERMINAL", "A64Oracle_Terminal"),
            _print("EVENT", "A64Oracle_LastEvent"),
            _print("HAS_EXCEPTION", "A64Oracle_HasException"),
            '    print "|EXCEPTION=";',
            "    if A64Oracle_HasException then",
            "        print A64Oracle_LastException.exceptype;",
            "    else",
            '        print "none";',
            "    end;",
            '    print "|SYNDROME_ISS=";',
            "    if A64Oracle_HasException then",
            "        print A64Oracle_LastException.syndrome.iss;",
            "    else",
            '        print "none";',
            "    end;",
            '    print "|SYNDROME_ISS2=";',
            "    if A64Oracle_HasException then",
            "        print A64Oracle_LastException.syndrome.iss2;",
            "    else",
            '        print "none";',
            "    end;",
            _print("TARGET_EL", "A64Oracle_LastTargetEL"),
            _print("PREFERRED_RETURN", "A64Oracle_PreferredExceptionReturn"),
            _print("VECTOR_OFFSET", "A64Oracle_VectorOffset"),
            _print("HAS_FAULT", "A64Oracle_HasFault"),
            '    print "|FAULT=";',
            "    if A64Oracle_HasFault then",
            "        print A64Oracle_LastFault.statuscode;",
            "    else",
            '        print "none";',
            "    end;",
            _print("EVENT_ADDRESS", "A64Oracle_LastAddress"),
            _print("EVENT_IMMEDIATE", "A64Oracle_LastImmediate"),
            _print("EVENT_INTEGER", "A64Oracle_LastIntegerPayload"),
            _print("EVENT_BOOLEAN0", "A64Oracle_LastBoolean0"),
            _print("EVENT_BOOLEAN1", "A64Oracle_LastBoolean1"),
            _print("HINT", "A64Oracle_LastHint"),
            _print("HINT_COUNT", "A64Oracle_HintCount"),
            _print("HAS_BRANCH", "A64Oracle_HasBranchOutcome"),
            _print("BRANCH_TAKEN", "A64Oracle_BranchTaken"),
            _print("BRANCH_TARGET", "A64Oracle_BranchTarget"),
            '    print "|BRANCH_TYPE=";',
            "    if A64Oracle_HasBranchOutcome then",
            "        print A64Oracle_BranchType;",
            "    else",
            '        print "none";',
            "    end;",
            _print("BRANCH_CONDITIONAL", "A64Oracle_BranchConditional"),
            _print("EFFECT", "A64Oracle_LastEffect"),
            _print("EFFECT_COUNT", "A64Oracle_EffectCount"),
            _print("MEMORY_BASE", "A64Oracle_MemoryBase"),
            _print("MEMORY_EVENT", "A64Oracle_LastMemoryEvent"),
            _print("MEMORY_STATUS", "A64Oracle_LastMemoryStatus.statuscode"),
            _print("MEMORY_SEQUENCE", "A64Oracle_MemorySequence"),
            _print("MEMORY_ACQUIRE", "A64Oracle_LastAccessAcquire"),
            _print("MEMORY_RELEASE", "A64Oracle_LastAccessRelease"),
            _print("MEMORY_EXCLUSIVE", "A64Oracle_LastAccessExclusive"),
            _print("SYSREG_OUTCOME", "A64Oracle_LastSysRegOutcome"),
            _print("SYSREG_SELECTOR", "A64Oracle_LastSysRegSelector"),
            _print("SYSREG_TARGET", "A64Oracle_LastSysRegTarget"),
            _print("SYSREG_TARGET2", "A64Oracle_LastSysRegTarget2"),
        ]
    )
    for index in range(256):
        lines.append(_print(f"M{index:03d}", f"A64Execution_MemoryChunk({index})"))
    for index in range(16):
        lines.append(_print(f"T{index:03d}", f"A64Execution_TagChunk({index})"))
    lines.extend(["    return 0;", "end;", ""])
    return "\n".join(lines)


def parsed_output(output: str) -> dict[str, str]:
    if output == "ORACLE_UNEXPECTED_EXCEPTION":
        raise ValueError("ASL harness caught an unexpected exception type")
    parts = output.strip().split("|")
    values: dict[str, str] = {}
    for part in parts:
        if "=" not in part:
            raise ValueError(f"malformed ASL output field {part!r}")
        key, value = part.split("=", 1)
        if not key or not value or key in values:
            raise ValueError(f"invalid/duplicate ASL output field {part!r}")
        values[key] = value
    required = {
        "FORMAT", "CAUGHT", "SP", "PC", "NZCV", "FPCR", "FPSR", "FPMR",
        "PSSBS", "PDIT", "PTCO", "PSM", "PZA", "PDAIF", "TERMINAL",
        "EVENT", "HAS_EXCEPTION", "EXCEPTION", "SYNDROME_ISS", "SYNDROME_ISS2",
        "TARGET_EL", "PREFERRED_RETURN", "VECTOR_OFFSET", "HAS_FAULT", "FAULT",
        "EVENT_ADDRESS", "EVENT_IMMEDIATE", "EVENT_INTEGER", "EVENT_BOOLEAN0",
        "EVENT_BOOLEAN1", "HINT", "HINT_COUNT", "EFFECT", "EFFECT_COUNT",
        "HAS_BRANCH", "BRANCH_TAKEN", "BRANCH_TARGET", "BRANCH_TYPE",
        "BRANCH_CONDITIONAL",
        "MEMORY_BASE", "MEMORY_EVENT", "MEMORY_STATUS", "MEMORY_SEQUENCE",
        "MEMORY_ACQUIRE", "MEMORY_RELEASE", "MEMORY_EXCLUSIVE", "SYSREG_OUTCOME",
        "SYSREG_SELECTOR", "SYSREG_TARGET", "SYSREG_TARGET2",
        *(f"X{index:02d}" for index in range(31)),
        *(f"V{index:02d}" for index in range(32)),
        *(f"M{index:03d}" for index in range(256)),
        *(f"T{index:03d}" for index in range(16)),
    }
    if set(values) != required:
        missing = sorted(required - set(values))
        extra = sorted(set(values) - required)
        raise ValueError(f"ASL observation shape drift: missing={missing}, extra={extra}")
    if values["FORMAT"] != FORMAT:
        raise ValueError(f"ASL observation format is {values['FORMAT']!r}")
    return values


def hex_value(value: str, bits: int) -> int:
    if not value.startswith("0x"):
        raise ValueError(f"expected hexadecimal ASL bits, found {value!r}")
    result = int(value[2:], 16)
    if result >= 1 << bits:
        raise ValueError(f"ASL value {value!r} exceeds bits({bits})")
    return result


def boolean(value: str) -> bool:
    if value not in {"TRUE", "FALSE"}:
        raise ValueError(f"expected ASL boolean, found {value!r}")
    return value == "TRUE"


def memory_projection(values: dict[str, str]) -> tuple[bytes, list[int]]:
    memory = b"".join(
        hex_value(values[f"M{index:03d}"], 128).to_bytes(16, "little")
        for index in range(256)
    )
    tags: list[int] = []
    for index in range(16):
        chunk = hex_value(values[f"T{index:03d}"], 64)
        tags.extend((chunk >> (tag * 4)) & 0xF for tag in range(16))
    if len(memory) != MEMORY_SIZE or len(tags) != MEMORY_TAGS:
        raise ValueError("ASL memory observation has an invalid extent")
    return memory, tags


def event_fields(values: dict[str, str]) -> dict[str, str]:
    names = (
        "CAUGHT", "TERMINAL", "EVENT", "HAS_EXCEPTION", "EXCEPTION",
        "SYNDROME_ISS", "SYNDROME_ISS2", "TARGET_EL", "PREFERRED_RETURN",
        "VECTOR_OFFSET", "HAS_FAULT", "FAULT", "EVENT_ADDRESS",
        "EVENT_IMMEDIATE", "EVENT_INTEGER", "EVENT_BOOLEAN0", "EVENT_BOOLEAN1",
        "HINT", "HINT_COUNT", "HAS_BRANCH", "BRANCH_TAKEN", "BRANCH_TARGET",
        "BRANCH_TYPE", "BRANCH_CONDITIONAL", "EFFECT", "EFFECT_COUNT", "MEMORY_EVENT",
        "MEMORY_STATUS", "MEMORY_SEQUENCE", "MEMORY_ACQUIRE", "MEMORY_RELEASE",
        "MEMORY_EXCLUSIVE", "SYSREG_OUTCOME", "SYSREG_SELECTOR", "SYSREG_TARGET",
        "SYSREG_TARGET2",
    )
    return {name.lower(): values[name] for name in names}


def output_row(
    row: dict[str, str],
    witness: int,
    partition: Partition,
    function: str,
    prepared: PreparedForm,
    values: dict[str, str],
    adapter_digest: str,
    allocation_witness_word: int,
    witness_choices: tuple[str, ...],
) -> dict[str, str]:
    initial, initial_memory, initial_tags = initial_projection(partition)
    observed_memory, observed_tags = memory_projection(values)
    raw_pc = hex_value(values["PC"], 64)
    terminal = boolean(values["TERMINAL"])
    caught = boolean(values["CAUGHT"])
    if terminal != caught:
        raise ValueError("typed terminal state and caught terminal exception disagree")
    memory_fault = values["MEMORY_STATUS"] != "Fault_None"
    unsupported_sysreg = values["SYSREG_OUTCOME"].startswith(
        "A64OracleSysRegOutcome_Unsupported"
    )
    control_class = row["decode_class"] in {
        "branch_imm", "branch_reg", "compbranch", "condbranch", "testbranch"
    }
    has_branch = boolean(values["HAS_BRANCH"])
    branch_taken = boolean(values["BRANCH_TAKEN"])
    if has_branch and branch_taken:
        branch_target = hex_value(values["BRANCH_TARGET"], 64)
        if raw_pc != branch_target:
            raise ValueError("BranchTo observation target differs from terminal PC")
    if unsupported_sysreg and not terminal:
        architectural_pc = raw_pc
        boundary = "unexecutable-unsupported-system-register-selector"
        execution_status = "unexecutable-unsupported-system-register-selector"
    elif terminal:
        architectural_pc = raw_pc
        boundary = "official-terminal-event"
        execution_status = "executed-terminal"
    elif memory_fault:
        architectural_pc = raw_pc
        boundary = "terminal-memory-status"
        execution_status = "executed-memory-fault"
    elif has_branch and branch_taken:
        architectural_pc = raw_pc
        boundary = "official-control-transfer"
        execution_status = "executed-completed"
    elif has_branch:
        architectural_pc = (GUEST_PC + 4) & ((1 << 64) - 1)
        boundary = "official-branch-not-taken-pc+4"
        execution_status = "executed-completed"
    elif control_class:
        architectural_pc = raw_pc
        boundary = "control-flow-outcome-unobserved"
        execution_status = "executed-control-boundary-unobserved"
    else:
        architectural_pc = (GUEST_PC + 4) & ((1 << 64) - 1)
        boundary = "single-instruction-pc+4"
        execution_status = "executed-completed"

    registers = [hex_value(values[f"X{index:02d}"], 64) for index in range(31)]
    vectors = [hex_value(values[f"V{index:02d}"], 128) for index in range(32)]
    observed_state = architecture_state_digest(
        registers,
        sp=hex_value(values["SP"], 64),
        pc=architectural_pc,
        nzcv=hex_value(values["NZCV"], 4),
        vectors=vectors,
        fpcr=hex_value(values["FPCR"], 32),
        fpsr=hex_value(values["FPSR"], 32),
        fpmr=hex_value(values["FPMR"], 64),
        pstate_ssbs=bool(hex_value(values["PSSBS"], 1)),
        pstate_dit=bool(hex_value(values["PDIT"], 1)),
        pstate_tco=bool(hex_value(values["PTCO"], 1)),
        pstate_sm=bool(hex_value(values["PSM"], 1)),
        pstate_za=bool(hex_value(values["PZA"], 1)),
        pstate_daif=hex_value(values["PDAIF"], 4),
    )
    initial_state = architecture_state_digest(
        initial["registers"],
        sp=initial["sp"],
        pc=initial["pc"],
        nzcv=initial["nzcv"],
        vectors=initial["vectors"],
    )
    memory_base = hex_value(values["MEMORY_BASE"], 64)
    if memory_base != initial["memory_base"]:
        raise ValueError("instruction changed the adapter's memory-window base")
    initial_memory_digest = memory_region_digest(
        initial_memory, address=memory_base, tags=initial_tags
    )
    observed_memory_digest = memory_region_digest(
        observed_memory, address=memory_base, tags=observed_tags
    )
    memory_sha = hashlib.sha256(
        observed_memory + bytes(observed_tags)
    ).hexdigest()
    outcome = event_fields(values)
    outcome["observation_boundary"] = boundary
    outcome["architectural_terminal_pc"] = f"{architectural_pc:016x}"
    allocation_id = status_id(row)
    execution_identity = "\0".join(
        (allocation_id, partition.name, f"{witness:08x}")
    )
    unresolved = {
        "semantic-expected-state-event-memory-join",
        "native-profile-join",
        "remaining-encoding-and-state-partitions",
    }
    if execution_status == "executed-control-boundary-unobserved":
        unresolved.add("control-flow-boundary-observation")
    if unsupported_sysreg:
        unresolved.add("unsupported-system-register-selector")
    return {
        "oracle_execution_id": "asl-execution-" + hashlib.sha256(
            execution_identity.encode()
        ).hexdigest()[:20],
        "allocation_status_id": allocation_id,
        "stable_id": row["stable_id"],
        "section": row["section"],
        "architectural_form": row["architectural_form"],
        "architecture_file": row["architecture_file"],
        "architecture_encoding": row["architecture_encoding"],
        "architecture_mask": row["architecture_mask"],
        "architecture_value": row["architecture_value"],
        "architecture_constraints": row["architecture_constraints"],
        "decode_class": row["decode_class"],
        "decode_encoding": row["decode_encoding"],
        "decode_mask": row["decode_mask"],
        "decode_value": row["decode_value"],
        "decode_constraints": row["decode_constraints"],
        "allocation_kind": row["allocation_kind"],
        "allocation_witness_encoding": f"{allocation_witness_word:08x}",
        "witness_encoding": f"{witness:08x}",
        "semantic_witness_choices": "+".join(witness_choices) or "none",
        "input_partition": partition.name,
        "input_partition_digest": partition.digest(row, witness),
        "required_partition_set": "pending",
        "observed_partition_set": "pending",
        "partition_coverage_status": "pending",
        "feature_profile": "+".join(feature_tokens(row)) or "baseline",
        "official_operation_source": prepared.source_file,
        "official_operation_function": function,
        "official_operation_digest": row["operation_digest"],
        "generated_operation_digest": prepared.generated_digest,
        "adapter_digest": adapter_digest,
        "execution_eligibility": (
            "unexecutable-adapter-system-register-selector"
            if unsupported_sysreg
            else "eligible-aslref-deterministic-profile"
        ),
        "execution_status": execution_status,
        "observation_boundary": boundary,
        "terminal_event": values["EVENT"],
        "exception_type": values["EXCEPTION"],
        "fault_status": values["FAULT"],
        "memory_event": values["MEMORY_EVENT"],
        "memory_status": values["MEMORY_STATUS"],
        "sysreg_outcome": values["SYSREG_OUTCOME"],
        "initial_pc": f"{GUEST_PC:016x}",
        "raw_terminal_pc": f"{raw_pc:016x}",
        "architectural_terminal_pc": f"{architectural_pc:016x}",
        "initial_state_digest": initial_state,
        "initial_memory_digest": initial_memory_digest,
        "observed_state_digest": observed_state,
        "observed_memory_digest": observed_memory_digest,
        "observed_memory_sha256": memory_sha,
        "observed_event_digest": exact_outcome_digest(outcome),
        "semantic_expected_join": "not-joined",
        "allocation_behavior_complete": "0",
        "unresolved_requirements": "+".join(sorted(unresolved)),
        "architecture_oracle": row["architecture_oracle"],
        "aslref_oracle": ASLREF_VERSION.replace(" ", "-"),
    }


def prepared_form(
    directory: Path,
    index: int,
    row: dict[str, str],
    root: Path,
    catalog: AslDependencyCatalog,
    bundler: Path,
    supplied: frozenset[str],
    supplemental: tuple[str, ...],
    workspace: BoundedWorkspace,
    runner: BoundedCommandRunner,
) -> PreparedForm:
    source_file = catalog.operation_source_file(row["architecture_file"])
    form = directory / f"form-{index:04d}"
    form.mkdir()
    generated = runner.run(
        [
            sys.executable, "-B", str(bundler), "--make-funs", "-j", "1",
            "-o", str(form), str(root / source_file),
        ],
        capture_output=True,
        text=True,
        timeout=min(60, runner.timeout_seconds),
    )
    instructions = form / "instructions.asl"
    if generated.returncode != 0 or not instructions.is_file():
        raise ValueError(
            f"{source_file}: official bundler failed: "
            + (generated.stderr + generated.stdout).strip()
        )
    text = instructions.read_text(encoding="utf-8")
    functions = tuple(FUNCTION.findall(text))
    if not functions:
        raise ValueError(f"{source_file}: bundler emitted no instruction procedure")
    shared = form / "shared.asl"
    workspace.write_text(
        shared,
        catalog.shared_subset(
            catalog.for_file(row["architecture_file"]), supplied, supplemental
        ),
    )
    return PreparedForm(
        instructions=instructions,
        shared=shared,
        source_file=source_file,
        generated_digest=sha256(instructions),
        functions=functions,
        instructions_text=text,
    )


def coverage(rows: list[dict[str, str]], obligations: dict[str, tuple[str, ...]]) -> None:
    observed: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        observed[row["allocation_status_id"]].add(row["input_partition"])
    for row in rows:
        required = obligations[row["allocation_status_id"]]
        actual = tuple(sorted(observed[row["allocation_status_id"]]))
        row["required_partition_set"] = "+".join(required)
        row["observed_partition_set"] = "+".join(actual)
        successful = all(
            candidate["execution_status"].startswith("executed-")
            for candidate in rows
            if candidate["allocation_status_id"] == row["allocation_status_id"]
        )
        row["partition_coverage_status"] = (
            "local-obligations-executed"
            if actual == required and successful
            else "incomplete"
        )


def partition_named(name: str) -> Partition:
    known = {
        partition.name: partition
        for partition in (
            Partition("zero-state-v1", "zero"),
            Partition("pattern-state-v1", "pattern"),
            Partition(
                "injected-memory-fault-v1", "pattern", memory_fault=True
            ),
            Partition("wait-terminal-policy-v1", "zero", stop_on_wait=True),
            Partition("fp-access-trap-v1", "pattern", fp_trap=True),
        )
    }
    if name not in known:
        raise ValueError(f"unknown execution partition {name!r}")
    return known[name]


def validate_evidence_rows(rows: list[dict[str, str]]) -> None:
    """Reject identity, partition, duplication, and status drift."""
    if not rows:
        raise ValueError("execution evidence is empty")
    execution_ids: set[str] = set()
    group_partitions: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        if set(row) != set(COLUMNS):
            raise ValueError("execution evidence columns drifted")
        allocation_id = status_id(row)
        if row["allocation_status_id"] != allocation_id:
            raise ValueError("execution evidence allocation identity was swapped")
        identity = "\0".join(
            (
                allocation_id,
                row["input_partition"],
                row["witness_encoding"],
            )
        )
        expected_execution_id = "asl-execution-" + hashlib.sha256(
            identity.encode()
        ).hexdigest()[:20]
        if row["oracle_execution_id"] != expected_execution_id:
            raise ValueError("execution evidence ID does not bind its input")
        if expected_execution_id in execution_ids:
            raise ValueError("duplicate execution evidence ID")
        execution_ids.add(expected_execution_id)
        if row["allocation_behavior_complete"] != "0":
            raise ValueError("execution evidence inferred behavioral completion")
        if row["execution_status"].startswith("unexecutable-"):
            if not row["execution_eligibility"].startswith("unexecutable-"):
                raise ValueError("unexecutable outcome was marked eligible")
        elif not row["execution_status"].startswith("executed-"):
            raise ValueError("unknown execution status")
        feature_expression = (
            "baseline"
            if row["feature_profile"] == "baseline"
            else " && ".join(sorted(row["feature_profile"].split("+")))
        )
        digest_row = dict(row)
        digest_row["static_feature_expressions"] = feature_expression
        expected_partition_digest = partition_named(
            row["input_partition"]
        ).digest(digest_row, int(row["witness_encoding"], 16))
        if row["input_partition_digest"] != expected_partition_digest:
            raise ValueError("execution input partition digest drifted")
        group_partitions[allocation_id].add(row["input_partition"])
        for column, width in (
            ("input_partition_digest", 64),
            ("official_operation_digest", 64),
            ("generated_operation_digest", 64),
            ("adapter_digest", 64),
            ("initial_state_digest", 16),
            ("initial_memory_digest", 16),
            ("observed_state_digest", 16),
            ("observed_memory_digest", 16),
            ("observed_memory_sha256", 64),
            ("observed_event_digest", 16),
        ):
            if re.fullmatch(rf"[0-9a-f]{{{width}}}", row[column]) is None:
                raise ValueError(f"execution evidence {column} is malformed")
    for row in rows:
        actual = "+".join(sorted(group_partitions[row["allocation_status_id"]]))
        if row["observed_partition_set"] != actual:
            raise ValueError("observed execution partition set drifted")
        required = set(row["required_partition_set"].split("+"))
        if required != group_partitions[row["allocation_status_id"]]:
            raise ValueError("required execution partition was removed")


def generate_execution_evidence(
    options: argparse.Namespace,
    publication: OneShotPublication,
) -> tuple[list[dict[str, str]], list[dict[str, str]], float]:
    started = time.perf_counter()
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-aslref-execution-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        for path in (
            options.architecture_archive,
            options.aslref,
            options.bundler,
            options.required_variants,
            options.interface,
            options.observer,
        ):
            if not path.is_file():
                raise ValueError(f"required input is not a file: {path}")
        if sha256(options.bundler) != SUPPORT_DIGESTS["bundler.py"]:
            raise ValueError("bundler is not the pinned ASLRef support version")
        version = runner.run(
            [str(options.aslref), "--version"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        if version != ASLREF_VERSION:
            raise ValueError(
                f"ASLRef version is {version!r}, expected {ASLREF_VERSION!r}"
            )
        required = read_required(options.required_variants)
        selected = selected_rows(required, options.scope, options.max_allocations)
        module_paths, declarations, _owners = load_interface_modules(
            options.interface, options.state_module
        )
        missing_modules = sorted(
            name
            for name in (
                "a64_aslref_state_core.asl",
                "a64_aslref_state_environment.asl",
                "a64_aslref_state_events.asl",
                "a64_aslref_state_fp.asl",
                "a64_aslref_state_memory.asl",
                "a64_aslref_state_sve.asl",
                "a64_aslref_state_sme.asl",
                "a64_aslref_state_sysreg.asl",
            )
            if name not in module_paths
        )
        if missing_modules:
            raise ValueError(
                f"execution oracle requires all state modules: {missing_modules}"
            )
        owners = sorted(module_paths)
        observer_source = options.observer.read_text(encoding="utf-8")
        supplied = frozenset(
            symbol for owner in owners for symbol in declarations[owner]
        ) | interface_declarations(observer_source)
        supplemental = tuple(
            module_paths[owner].read_text(encoding="utf-8") for owner in owners
        ) + (observer_source,)
        adapter_input = "\0".join(
            [options.interface.read_text(encoding="utf-8"), *supplemental]
        )
        adapter_digest = hashlib.sha256(adapter_input.encode()).hexdigest()
        output_rows: list[dict[str, str]] = []
        obligations: dict[str, tuple[str, ...]] = {}
        directory = workspace.require_path()
        root = archive_root(
            options.architecture_archive,
            directory / "archive",
            workspace,
        )
        work = directory / "work"
        work.mkdir()
        catalog = AslDependencyCatalog(root)
        cache: dict[str, PreparedForm] = {}
        for allocation_index, row in enumerate(selected):
            source_file = catalog.operation_source_file(row["architecture_file"])
            prepared = cache.get(source_file)
            if prepared is None:
                prepared = prepared_form(
                    work,
                    len(cache),
                    row,
                    root,
                    catalog,
                    options.bundler,
                    supplied,
                    supplemental,
                    workspace,
                    runner,
                )
                cache[source_file] = prepared
            function = exact_function(
                root / source_file,
                row["architecture_encoding"],
                prepared.functions,
            )
            source = function_source(prepared.instructions_text, function)
            partitions = partitions_for(source)
            allocation_id = status_id(row)
            obligations[allocation_id] = tuple(
                sorted(partition.name for partition in partitions)
            )
            allocation_word = allocation_witness(row)
            witness, witness_choices = semantic_witness(row, root)
            for partition_index, partition in enumerate(partitions):
                harness = work / (
                    f"harness-{allocation_index:04d}-{partition_index:02d}.asl"
                )
                workspace.write_text(
                    harness,
                    harness_text(row, witness, function, partition),
                )
                command = [
                    str(options.aslref),
                    "--type-check-strict",
                    "--exec",
                    "-1",
                    str(prepared.instructions),
                    "-1",
                    str(prepared.shared),
                    "-1",
                    str(options.interface),
                ]
                for owner in owners:
                    command.extend(["-1", str(module_paths[owner])])
                command.extend(["-1", str(options.observer), str(harness)])
                execution = runner.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=options.timeout_seconds,
                )
                if execution.returncode != 0:
                    raise ValueError(
                        f"{row['stable_id']}:{partition.name}: ASLRef failed: "
                        + (execution.stderr + execution.stdout).strip()
                    )
                values = parsed_output(execution.stdout)
                output_rows.append(
                    output_row(
                        row,
                        witness,
                        partition,
                        function,
                        prepared,
                        values,
                        adapter_digest,
                        allocation_word,
                        witness_choices,
                    )
                )
        coverage(output_rows, obligations)
        return output_rows, selected, started


def main() -> int:
    options = arguments()
    selected_modules = options.state_module
    if selected_modules is None:
        selected_modules = [
            Path(__file__).with_name(filename)
            for filename in STATE_MODULE_FILENAMES
            if Path(__file__).with_name(filename).is_file()
        ]
    options.aslref = options.aslref.resolve(strict=True)
    options.bundler = options.bundler.resolve(strict=True)
    options.state_module = selected_modules
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.architecture_archive,
            options.aslref,
            options.bundler,
            options.required_variants,
            options.interface,
            options.observer,
            *selected_modules,
        ],
    )
    output_rows, selected, started = generate_execution_evidence(
        options, publication
    )
    if any(row["allocation_behavior_complete"] != "0" for row in output_rows):
        raise ValueError("execution evidence cannot infer behavioral completion")
    output_rows.sort(
        key=lambda row: (row["allocation_status_id"], row["input_partition"])
    )
    validate_evidence_rows(output_rows)
    publication.publish_tsvs(
        [(publication.targets[0], COLUMNS, output_rows)]
    )
    statuses = Counter(row["execution_status"] for row in output_rows)
    elapsed = time.perf_counter() - started
    print(
        f"A64 ASLRef execution oracle: {len(selected)} allocations, "
        f"{len(output_rows)} explicit partitions, "
        f"{sum(value for key, value in statuses.items() if key.startswith('executed-'))} "
        f"executed, 0 behavior-complete, {elapsed:.2f}s; wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        OSError,
        ValueError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
