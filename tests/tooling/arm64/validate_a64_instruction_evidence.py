#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate exact A64 inventory-to-execution evidence bindings."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from a64_bounded_generation import (  # noqa: E402
    BoundedCommandRunner,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_llvm_oracle import (  # noqa: E402
    decoder_opcodes,
    find_llvm_mc,
    require_pinned_llvm,
)
from a64_architecture_oracle import SOURCE_ORACLE  # noqa: E402
from validate_a64_instruction_inventory import FEATURE_TOKENS  # noqa: E402
from a64_raw_evidence_files import (  # noqa: E402
    atomic_publish_bytes,
    canonical_existing_directory,
    require_no_symlink_components,
    validate_publication_topology,
)


INVENTORY_COLUMNS = [
    "stable_id",
    "section",
    "pdf_label",
    "current_suite",
    "current_variant_model",
    "native_feature_requirements",
    "three_engine_status",
]
BINDING_COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "ctest",
    "case",
    "encoding",
    "alias_group",
    "native_required_features",
]
ALLOCATION_COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "binding_encoding",
    "architecture_file",
    "architecture_encoding",
    "architecture_form_encoding_count",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "architecture_features",
    "architecture_native_feature_alternatives",
    "decoder_opcode",
    "decoder_mask",
    "decoder_value",
    "encoding_mask",
    "encoding_value",
    "architecture_oracle",
    "decoder_oracle",
]
CONTRACT_COLUMNS = [
    "stable_id",
    "variant_id",
    "section",
    "architectural_form",
    "decoder_opcode",
    "encoding_mask",
    "encoding_value",
    "validity",
    "native_required_features",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "interpreter_result",
    "gadgets_result",
    "jit_result",
    "entry_complete",
    "oracle",
]
EVIDENCE_COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "alias_group",
    "ctest",
    "case",
    "source",
    "line",
    "encoding",
    "invalid",
    "guest_page_size",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "native_required_features",
    "native_requirement_scope",
    "native_observed_features",
    "native_missing_features",
    "interpreter_result",
    "gadgets_result",
    "jit_result",
]
REPORT_COLUMNS = ["variant_id", *EVIDENCE_COLUMNS]
SEMANTIC_CONTRACT_COLUMNS = [
    "stable_id",
    "semantic_id",
    "section",
    "architectural_form",
    "ctest",
    "case",
    "encoding",
    "guest_page_size",
    "architecture_file",
    "architecture_encoding",
    "architecture_mask",
    "architecture_value",
    "decoder_opcode",
    "decoder_mask",
    "decoder_value",
    "invalid",
    "native_required_features",
    "native_requirement_scope",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "expectation_basis",
    "entry_complete",
    "completion_basis",
    "oracle",
]
PROVISIONAL_SEMANTIC_COLUMNS = [
    "invalid",
    "guest_page_size",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "native_required_features",
    "native_requirement_scope",
]
STABLE_ID = re.compile(r"(?:BASE|FPSIMD)-[0-9]{4}\Z")
ENCODING = re.compile(r"[0-9a-f]{8}\Z")
ALIAS_GROUP = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
VARIANT_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
OBSERVED_FEATURE = re.compile(r"([a-z][a-z0-9_]*)=([01])\Z")
DIGEST = re.compile(r"[0-9a-f]{16}\Z")
SYNDROME = re.compile(r"[0-9a-f]{8}\Z")
FAULT = re.compile(r"(?:read|write|execute):[0-9a-f]{16}\Z")
RETIREMENT = re.compile(
    r"(?:exact:[0-9]+|pattern:[0-9]+:[1-9][0-9]*:"
    r"(?:[0-9]+|unbounded)|engine-dependent|interpreter-equal)\Z"
)
DECODER_OPCODE = re.compile(
    r"(?:[A-Za-z][A-Za-z0-9_]*|INVALID_[a-z0-9_]+)\Z"
)
ALLOCATION_ORACLE = re.compile(r"llvm-mc-[A-Za-z0-9_.-]+\Z")
ARCHITECTURE_FILE = re.compile(r"[a-z0-9_]+\.xml\Z")
ARCHITECTURE_ENCODING = re.compile(r"[A-Za-z][A-Za-z0-9_.-]+\Z")
ARCHITECTURE_CONSTRAINT = re.compile(
    r"[A-Za-z][A-Za-z0-9_]*@([0-9]+):([1-9][0-9]*)(!=|==)([01x]+)\Z"
)
CONTRACT_ORACLE = re.compile(r"[a-z0-9][a-z0-9.+-]+\Z")
CUSTOM_STATE_ORACLE = re.compile(
    r"custom-validator@"
    r"(tests/(?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+):"
    r"([1-9][0-9]*)\Z"
)
STATE_ORACLE = re.compile(
    r"(?:exact|custom-validator@"
    r"tests/(?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+:[1-9][0-9]*)\Z"
)
EXECUTED_RESULTS = {"executed-not-retired", "executed-retired"}
JIT_RESULTS = EXECUTED_RESULTS | {"skipped-host-feature"}
AUXILIARY_HOST_FEATURES = {"afp"}
EXPECTED_EVENTS = {
    "system-call",
    "memory-fault",
    "gcs-fault",
    "illegal-instruction",
    "breakpoint",
    "floating-point-trap",
    "pc-alignment",
    "instruction-budget",
    "fatal",
}


def fail(message: str) -> None:
    raise ValueError(message)


def canonical_project_source(value: str, repository: Path) -> str | None:
    """Return one unambiguous project-relative tests/ locator."""
    source = Path(value)
    if source.is_absolute():
        try:
            source = source.resolve().relative_to(repository.resolve())
        except ValueError:
            return None
    if (
        not source.parts
        or source.parts[0] != "tests"
        or any(part in {"", ".", ".."} for part in source.parts)
    ):
        return None
    return source.as_posix()


def validate_state_oracle_locator(
    row: dict[str, str], path: Path, stable_id: str, repository: Path
) -> None:
    oracle = row["expected_state_oracle"]
    if oracle == "exact":
        return
    match = CUSTOM_STATE_ORACLE.fullmatch(oracle)
    if match is None:
        fail(f"{path}: {stable_id} has invalid state oracle")
    source = canonical_project_source(row["source"], repository)
    if source is None or source != match.group(1):
        fail(
            f"{path}: {stable_id} state oracle is not the exact "
            "project-relative source locator"
        )
    if row["line"] != match.group(2):
        fail(f"{path}: {stable_id} state oracle has a stale source line")


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument(
        "--inventory",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_inventory.tsv",
    )
    parser.add_argument(
        "--bindings",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_evidence_bindings.tsv"
        ),
    )
    parser.add_argument(
        "--allocations",
        type=Path,
        default=(
            repository / "tests/unit/arm64/a64_instruction_allocations.tsv"
        ),
    )
    parser.add_argument(
        "--variant-contracts",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_variant_contracts.tsv"
        ),
    )
    parser.add_argument(
        "--semantic-contracts",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_semantic_contracts.tsv"
        ),
    )
    parser.add_argument("--registered-tests-file", type=Path)
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument("--expected-count", type=int, default=983)
    parser.add_argument("--require-variant-complete", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            fail(f"{path}: columns are {reader.fieldnames!r}, expected {columns!r}")
        rows = list(reader)
    if not rows:
        fail(f"{path}: contains no rows")
    return rows


def registered_tests(
    build_root: Path | None,
    registered_tests_file: Path | None,
    runner: BoundedCommandRunner,
    ctest_tool: Path | None,
) -> set[str]:
    if registered_tests_file is not None:
        tests = {
            line.strip()
            for line in registered_tests_file.read_text(encoding="utf-8").splitlines()
            if line.strip()
        }
        if not tests:
            fail(f"{registered_tests_file}: contains no registered tests")
        return tests
    if build_root is None:
        fail("--build-root is required without --registered-tests-file")
    if ctest_tool is None:
        fail("bounded CTest registration lacks its frozen tool")
    completed = runner.run(
        [str(ctest_tool), "--test-dir", str(build_root), "-N"],
        check=True,
        text=True,
        capture_output=True,
    )
    tests = set(
        re.findall(
            r"^\s*Test\s+#[0-9]+:\s*(\S.*?)\s*$",
            completed.stdout,
            re.MULTILINE,
        )
    )
    if not tests:
        fail(f"{build_root}: CTest lists no registered tests")
    return tests


def require_unique(rows: list[dict[str, str]], column: str, source: Path) -> None:
    counts = Counter(row[column] for row in rows)
    duplicates = sorted(value for value, count in counts.items() if count != 1)
    if duplicates:
        fail(f"{source}: duplicate {column}: {', '.join(duplicates)}")


def validate_inventory(
    rows: list[dict[str, str]], source: Path, expected_count: int
) -> dict[str, dict[str, str]]:
    if len(rows) != expected_count:
        fail(f"{source}: has {len(rows)} rows, expected {expected_count}")
    require_unique(rows, "stable_id", source)
    for row in rows:
        if STABLE_ID.fullmatch(row["stable_id"]) is None:
            fail(f"{source}: invalid stable ID {row['stable_id']!r}")
        if row["section"] not in {"Base", "FP&SIMD"}:
            fail(f"{source}: invalid section for {row['stable_id']}")
        if not row["pdf_label"]:
            fail(f"{source}: empty architectural form for {row['stable_id']}")
    return {row["stable_id"]: row for row in rows}


def requirement_alternatives(value: str, stable_id: str) -> set[frozenset[str]]:
    expression = value
    if value.startswith("per_variant(") and value.endswith(")"):
        expression = value[len("per_variant(") : -1]
    elif "|" in value:
        fail(f"{stable_id}: alternatives require per_variant(...)")
    terms = expression.split("|")
    if not terms or any(not term for term in terms):
        fail(f"{stable_id}: malformed native feature expression")
    alternatives: set[frozenset[str]] = set()
    for term in terms:
        if term == "baseline":
            features: frozenset[str] = frozenset()
        else:
            tokens = term.split("+")
            if len(tokens) != len(set(tokens)):
                fail(f"{stable_id}: duplicate native feature in {term!r}")
            unknown = set(tokens) - FEATURE_TOKENS
            if unknown:
                fail(f"{stable_id}: unknown native features {sorted(unknown)!r}")
            features = frozenset(tokens)
        alternatives.add(features)
    return alternatives


def exact_features(value: str, stable_id: str) -> tuple[str, ...]:
    if value == "baseline":
        return ()
    if value.startswith("per_variant(") or "|" in value:
        fail(f"{stable_id}: runtime feature evidence is not exact: {value!r}")
    tokens = tuple(value.split("+"))
    if any(not token for token in tokens) or len(tokens) != len(set(tokens)):
        fail(f"{stable_id}: malformed exact native features {value!r}")
    unknown = set(tokens) - FEATURE_TOKENS - AUXILIARY_HOST_FEATURES
    if unknown:
        fail(f"{stable_id}: unknown exact native features {sorted(unknown)!r}")
    return tokens


def validate_allocations(
    rows: list[dict[str, str]],
    source: Path,
    inventory: dict[str, dict[str, str]],
) -> dict[tuple[str, str], list[dict[str, str]]]:
    if len(rows) < len(inventory):
        fail(
            f"{source}: has {len(rows)} allocation variants, fewer than "
            f"the {len(inventory)} architectural entries"
        )
    by_form: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    identities: set[tuple[str, str]] = set()
    for row in rows:
        stable_id = row["stable_id"]
        if stable_id not in inventory:
            fail(f"{source}: allocation names unknown stable ID {stable_id}")
        entry = inventory[stable_id]
        if (
            row["section"] != entry["section"]
            or row["architectural_form"] != entry["pdf_label"]
        ):
            fail(f"{source}: {stable_id} allocation form differs from inventory")
        key = (row["section"], row["architectural_form"])
        identity = (stable_id, row["binding_encoding"])
        if identity in identities:
            fail(f"{source}: duplicate exact architectural allocation {identity!r}")
        identities.add(identity)
        by_form[key].append(row)
    expected_forms = {
        (row["section"], row["pdf_label"]) for row in inventory.values()
    }
    missing = sorted(expected_forms - set(by_form))
    extra = sorted(set(by_form) - expected_forms)
    if missing or extra:
        fail(
            f"{source}: architectural-form set differs from inventory; "
            f"missing={missing}, extra={extra}"
        )
    decoder_oracles: set[str] = set()
    architecture_oracles: set[str] = set()
    for key, variants in by_form.items():
        for row in variants:
            allocation_name = f"{key[0]} {key[1]} {row['decoder_opcode']}"
            if DECODER_OPCODE.fullmatch(row["decoder_opcode"]) is None:
                fail(f"{source}: {allocation_name} has invalid decoder opcode")
            if ALLOCATION_ORACLE.fullmatch(row["decoder_oracle"]) is None:
                fail(f"{source}: {allocation_name} has invalid oracle provenance")
            decoder_oracles.add(row["decoder_oracle"])
            architecture_oracles.add(row["architecture_oracle"])
            if row["architecture_oracle"] != SOURCE_ORACLE:
                fail(
                    f"{source}: {allocation_name} architecture provenance is "
                    f"{row['architecture_oracle']!r}, expected {SOURCE_ORACLE!r}"
                )
            if ARCHITECTURE_FILE.fullmatch(row["architecture_file"]) is None:
                fail(f"{source}: {allocation_name} has invalid architecture file")
            if ARCHITECTURE_ENCODING.fullmatch(
                row["architecture_encoding"]
            ) is None:
                fail(
                    f"{source}: {allocation_name} has invalid architecture encoding"
                )
            try:
                form_encoding_count = int(row["architecture_form_encoding_count"])
            except ValueError as error:
                raise ValueError(
                    f"{source}: {allocation_name} has invalid architecture "
                    "form encoding count"
                ) from error
            if form_encoding_count <= 0:
                fail(
                    f"{source}: {allocation_name} has non-positive architecture "
                    "form encoding count"
                )
            for column in (
                "binding_encoding",
                "architecture_mask",
                "architecture_value",
                "decoder_mask",
                "decoder_value",
                "encoding_mask",
                "encoding_value",
            ):
                if ENCODING.fullmatch(row[column]) is None:
                    fail(f"{source}: {allocation_name} has invalid {column}")
            word = int(row["binding_encoding"], 16)
            for prefix in ("architecture", "decoder", "encoding"):
                mask = int(row[f"{prefix}_mask"], 16)
                value = int(row[f"{prefix}_value"], 16)
                if mask == 0 or value & ~mask or word & mask != value:
                    fail(
                        f"{source}: {allocation_name} has non-canonical or "
                        f"non-matching {prefix} mask/value"
                    )
            constraints = row["architecture_constraints"].split(";")
            if constraints != ["none"]:
                for constraint in constraints:
                    match = ARCHITECTURE_CONSTRAINT.fullmatch(constraint)
                    if match is None:
                        fail(
                            f"{source}: {allocation_name} has malformed "
                            "architecture constraint"
                        )
                    low, width = int(match.group(1)), int(match.group(2))
                    if low + width > 32 or len(match.group(4)) != width:
                        fail(
                            f"{source}: {allocation_name} has out-of-range "
                            "architecture constraint"
                        )
                    actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
                    equal = all(
                        expected == "x" or expected == observed
                        for expected, observed in zip(match.group(4), actual)
                    )
                    matches = equal if match.group(3) == "==" else not equal
                    if not matches:
                        fail(
                            f"{source}: {allocation_name} violates its "
                            "architecture constraint"
                        )
            allocation_feature_alternatives(
                row["architecture_native_feature_alternatives"], allocation_name
            )
            if not row["architecture_features"]:
                fail(f"{source}: {allocation_name} lacks raw architecture features")
    if len(decoder_oracles) != 1:
        fail(
            f"{source}: mixes decoder oracle versions {sorted(decoder_oracles)!r}"
        )
    if architecture_oracles != {SOURCE_ORACLE}:
        fail(
            f"{source}: mixes architecture oracle versions "
            f"{sorted(architecture_oracles)!r}"
        )
    return by_form


def allocation_feature_alternatives(
    value: str, allocation_name: str
) -> set[frozenset[str]]:
    alternatives: set[frozenset[str]] = set()
    for term in value.split("|"):
        if term == "baseline":
            features = frozenset()
        else:
            tokens = term.split("+")
            if any(not token for token in tokens) or len(tokens) != len(set(tokens)):
                fail(f"{allocation_name}: malformed architecture feature alternative")
            unknown = set(tokens) - FEATURE_TOKENS
            if unknown:
                fail(
                    f"{allocation_name}: unknown architecture-derived host "
                    f"features {sorted(unknown)!r}"
                )
            features = frozenset(tokens)
        alternatives.add(features)
    if not alternatives:
        fail(f"{allocation_name}: empty architecture feature alternatives")
    return alternatives


def expected_decoder_opcode(allocation: dict[str, str]) -> str | None:
    opcode = allocation["decoder_opcode"]
    return None if opcode.startswith("INVALID_") else opcode


def allocation_variant_id(allocation: dict[str, str]) -> str:
    return ":".join(
        (
            allocation["decoder_opcode"],
            allocation["encoding_mask"],
            allocation["encoding_value"],
        )
    )


def provisional_variant_id(
    allocation_id: str, row: dict[str, str]
) -> str:
    """Give an uncontracted execution binding a stable, explicitly provisional ID."""
    return ":".join(
        (
            "provisional",
            allocation_id,
            row["ctest"],
            row["case"],
            row["encoding"],
        )
    )


def provisional_execution_variant_id(
    binding_variant_id: str, row: dict[str, str]
) -> str:
    """Separate legacy executions without pretending their oracle is reviewed."""
    semantic_key = "\0".join(row[column] for column in PROVISIONAL_SEMANTIC_COLUMNS)
    digest = hashlib.sha256(semantic_key.encode("utf-8")).hexdigest()[:16]
    return f"{binding_variant_id}:semantics:{digest}"


def validate_variant_contracts(
    rows: list[dict[str, str]],
    source: Path,
    inventory: dict[str, dict[str, str]],
    allocations: dict[tuple[str, str], list[dict[str, str]]],
) -> dict[tuple[str, str], dict[str, str]]:
    contracts: dict[tuple[str, str], dict[str, str]] = {}
    identities: set[tuple[str, str]] = set()
    for row in rows:
        stable_id = row["stable_id"]
        if stable_id not in inventory:
            fail(f"{source}: contract names unknown stable ID {stable_id}")
        if VARIANT_ID.fullmatch(row["variant_id"]) is None:
            fail(f"{source}: {stable_id} has invalid variant ID")
        identity = (stable_id, row["variant_id"])
        if identity in identities:
            fail(f"{source}: duplicate variant contract {identity!r}")
        identities.add(identity)
        entry = inventory[stable_id]
        if (
            row["section"] != entry["section"]
            or row["architectural_form"] != entry["pdf_label"]
        ):
            fail(f"{source}: {stable_id}/{row['variant_id']} form mismatch")
        allocation_identity = (
            row["decoder_opcode"],
            row["encoding_mask"],
            row["encoding_value"],
        )
        matching = [
            allocation
            for allocation in allocations[
                (row["section"], row["architectural_form"])
            ]
            if allocation["stable_id"] == stable_id
            if allocation_variant_id(allocation)
            == ":".join(allocation_identity)
        ]
        if len(matching) != 1:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} does not name "
                "one reviewed allocation partition"
            )
        if row["validity"] not in {"valid", "reserved", "invalid"}:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid validity")
        exact_features(row["native_required_features"], stable_id)
        if row["expected_event"] not in EXPECTED_EVENTS:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid event")
        if RETIREMENT.fullmatch(row["expected_retirement"]) is None:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} invalid retirement"
            )
        if STATE_ORACLE.fullmatch(row["expected_state_oracle"]) is None:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} invalid state oracle"
            )
        for column in ("expected_state_digest", "expected_memory_digest"):
            if DIGEST.fullmatch(row[column]) is None:
                fail(
                    f"{source}: {stable_id}/{row['variant_id']} invalid {column}"
                )
        if row["expected_fault"] != "none" and FAULT.fullmatch(
            row["expected_fault"]
        ) is None:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid fault")
        if SYNDROME.fullmatch(row["expected_syndrome"]) is None:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid syndrome")
        if row["interpreter_result"] not in EXECUTED_RESULTS:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} invalid interpreter result"
            )
        if row["gadgets_result"] not in EXECUTED_RESULTS:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} invalid gadget result"
            )
        if row["jit_result"] not in JIT_RESULTS:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid JIT result")
        if row["entry_complete"] not in {"0", "1"}:
            fail(
                f"{source}: {stable_id}/{row['variant_id']} invalid completeness flag"
            )
        if CONTRACT_ORACLE.fullmatch(row["oracle"]) is None:
            fail(f"{source}: {stable_id}/{row['variant_id']} invalid provenance")
        key = (stable_id, allocation_variant_id(matching[0]))
        if key in contracts:
            fail(
                f"{source}: multiple contracts claim allocation partition {key!r}"
            )
        contracts[key] = row
    return contracts


def resolve_allocation(
    source: Path,
    stable_id: str,
    word: int,
    actual_opcode: str | None,
    variants: list[dict[str, str]],
) -> dict[str, str]:
    matches = []
    for allocation in variants:
        if allocation["stable_id"] != stable_id:
            continue
        if int(allocation["binding_encoding"], 16) != word:
            continue
        if expected_decoder_opcode(allocation) != actual_opcode:
            continue
        for prefix in ("architecture", "decoder", "encoding"):
            mask = int(allocation[f"{prefix}_mask"], 16)
            value = int(allocation[f"{prefix}_value"], 16)
            if word & mask != value:
                break
        else:
            matches.append(allocation)
    if not matches:
        decoded = actual_opcode if actual_opcode is not None else "invalid"
        fail(
            f"{source}: {stable_id} encoding does not match any independent "
            f"architectural allocation/dependent-field partition "
            f"(LLVM decoded {decoded})"
        )
    if len(matches) != 1:
        fail(
            f"{source}: {stable_id} encoding ambiguously matches independent "
            "variant partitions"
        )
    return matches[0]


def validate_bindings(
    rows: list[dict[str, str]],
    source: Path,
    inventory: dict[str, dict[str, str]],
    allocations: dict[tuple[str, str], list[dict[str, str]]],
    tests: set[str],
    decoded_words: dict[int, str | None] | None = None,
    contracts: dict[tuple[str, str], dict[str, str]] | None = None,
) -> dict[tuple[str, str, str, str], dict[str, str]]:
    if contracts is None:
        contracts = {}
    stable_ids = {row["stable_id"] for row in rows}
    missing = sorted(set(inventory) - stable_ids)
    extra = sorted(stable_ids - set(inventory))
    if missing or extra:
        fail(
            f"{source}: stable-ID set differs from inventory; "
            f"missing={missing}, extra={extra}"
        )

    if decoded_words is None:
        decoded_words = {}
        for row in rows:
            if ENCODING.fullmatch(row["encoding"]) is None:
                continue
            matching = [
                allocation
                for allocation in allocations[
                    (row["section"], row["architectural_form"])
                ]
                if allocation["stable_id"] == row["stable_id"]
                and allocation["binding_encoding"] == row["encoding"]
            ]
            if len(matching) == 1:
                decoded_words[int(row["encoding"], 16)] = (
                    expected_decoder_opcode(matching[0])
                )

    by_locator: dict[tuple[str, str, str, str], dict[str, str]] = {}
    variant_keys: set[tuple[str, str]] = set()
    bound_contracts: set[tuple[str, str]] = set()
    encoding_groups: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    alias_groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    for original in rows:
        row = dict(original)
        stable_id = row["stable_id"]
        expected = inventory[stable_id]
        if row["section"] != expected["section"]:
            fail(f"{source}: {stable_id} section does not match inventory")
        if row["architectural_form"] != expected["pdf_label"]:
            fail(
                f"{source}: {stable_id} architectural form does not match inventory"
            )
        if row["ctest"] not in tests:
            fail(f"{source}: {stable_id} names unregistered CTest {row['ctest']!r}")
        if not row["case"]:
            fail(f"{source}: {stable_id} has an empty case name")
        if ENCODING.fullmatch(row["encoding"]) is None:
            fail(f"{source}: {stable_id} has invalid encoding {row['encoding']!r}")
        encoding = int(row["encoding"], 16)
        if encoding not in decoded_words:
            fail(
                f"{source}: {stable_id} encoding lacks pinned LLVM decode"
            )
        allocation = resolve_allocation(
            source,
            stable_id,
            encoding,
            decoded_words[encoding],
            allocations[(row["section"], row["architectural_form"])],
        )
        allocation_id = allocation_variant_id(allocation)
        contract_key = (stable_id, allocation_id)
        contract = contracts.get(contract_key)
        row["_allocation_id"] = allocation_id
        row["_allocation"] = allocation
        row["_variant_id"] = (
            contract["variant_id"]
            if contract is not None
            else provisional_variant_id(allocation_id, row)
        )
        row["_contract"] = contract
        if contract is not None:
            bound_contracts.add(contract_key)
        variant_key = (stable_id, row["_variant_id"])
        if variant_key in variant_keys:
            fail(
                f"{source}: duplicate independently identified variant "
                f"{stable_id} {row['_variant_id']}"
            )
        variant_keys.add(variant_key)
        required = exact_features(row["native_required_features"], stable_id)
        architectural = frozenset(set(required) - AUXILIARY_HOST_FEATURES)
        authoritative = allocation_feature_alternatives(
            allocation["architecture_native_feature_alternatives"],
            f"{stable_id}/{allocation['architecture_encoding']}",
        )
        if architectural not in authoritative:
            fail(
                f"{source}: {stable_id} exact native features "
                f"{sorted(architectural)!r} do not match independent Arm XML "
                f"feature alternatives {sorted(map(sorted, authoritative))!r}"
            )
        allowed = requirement_alternatives(
            expected["native_feature_requirements"], stable_id
        )
        if architectural not in allowed:
            fail(
                f"{source}: {stable_id} exact native features "
                f"{sorted(architectural)!r} do not match inventory "
                f"{expected['native_feature_requirements']!r}"
            )
        alias = row["alias_group"]
        if alias != "-" and ALIAS_GROUP.fullmatch(alias) is None:
            fail(f"{source}: {stable_id} has invalid alias group {alias!r}")
        encoding_groups[(row["section"], row["encoding"])].append(row)
        if alias != "-":
            alias_groups[alias].append(row)
        locator = (stable_id, row["ctest"], row["case"], row["encoding"])
        if locator in by_locator:
            fail(f"{source}: duplicate execution binding {locator!r}")
        by_locator[locator] = row

    for (section, encoding), group in encoding_groups.items():
        stable_group = {row["stable_id"] for row in group}
        if len(stable_group) == 1:
            continue
        aliases = {row["alias_group"] for row in group}
        if len(aliases) != 1 or "-" in aliases:
            ids = ", ".join(row["stable_id"] for row in group)
            fail(
                f"{source}: undeclared duplicate {section} encoding "
                f"{encoding}: {ids}"
            )

    for alias, group in alias_groups.items():
        if len(group) < 2:
            fail(f"{source}: alias group {alias!r} has only one member")
        executions = {
            (row["section"], row["ctest"], row["case"], row["encoding"])
            for row in group
        }
        if len(executions) != 1:
            fail(f"{source}: alias group {alias!r} spans distinct executions")
    missing_contracts = sorted(set(contracts) - bound_contracts)
    if missing_contracts:
        fail(
            f"{source}: {len(missing_contracts)} required variant contracts "
            "lack execution bindings: "
            + ", ".join(
                f"{stable_id}/{contracts[key]['variant_id']}"
                for key in missing_contracts
                for stable_id in (key[0],)
            )
        )
    return by_locator


SEMANTIC_SIGNATURE_COLUMNS = [
    "invalid",
    "guest_page_size",
    "native_required_features",
    "native_requirement_scope",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
]


def semantic_contract_key(row: dict[str, str]) -> tuple[str, ...]:
    return (
        row["stable_id"],
        row["ctest"],
        row["case"],
        row["encoding"],
        *(row[column] for column in SEMANTIC_SIGNATURE_COLUMNS),
    )


def validate_semantic_contracts(
    rows: list[dict[str, str]],
    source: Path,
    inventory: dict[str, dict[str, str]],
    bindings: dict[tuple[str, str, str, str], dict[str, str]],
) -> tuple[dict[tuple[str, ...], dict[str, str]], set[str]]:
    by_key: dict[tuple[str, ...], dict[str, str]] = {}
    identities: set[tuple[str, str]] = set()
    by_id: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        stable_id = row["stable_id"]
        if stable_id not in inventory:
            fail(f"{source}: semantic contract names unknown stable ID {stable_id}")
        if VARIANT_ID.fullmatch(row["semantic_id"]) is None:
            fail(f"{source}: {stable_id} has invalid semantic ID")
        identity = (stable_id, row["semantic_id"])
        if identity in identities:
            fail(f"{source}: duplicate semantic contract {identity!r}")
        identities.add(identity)
        locator = (stable_id, row["ctest"], row["case"], row["encoding"])
        binding = bindings.get(locator)
        if binding is None:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} has no exact "
                "execution binding"
            )
        allocation = binding["_allocation"]
        for semantic_column, expected_value in (
            ("section", binding["section"]),
            ("architectural_form", binding["architectural_form"]),
            ("native_required_features", binding["native_required_features"]),
            ("architecture_file", allocation["architecture_file"]),
            ("architecture_encoding", allocation["architecture_encoding"]),
            ("architecture_mask", allocation["architecture_mask"]),
            ("architecture_value", allocation["architecture_value"]),
            ("decoder_opcode", allocation["decoder_opcode"]),
            ("decoder_mask", allocation["decoder_mask"]),
            ("decoder_value", allocation["decoder_value"]),
        ):
            if row[semantic_column] != expected_value:
                fail(
                    f"{source}: {stable_id}/{row['semantic_id']} exact "
                    f"{semantic_column} does not match its independent "
                    "allocation/binding"
                )
        if row["invalid"] not in {"0", "1"}:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid validity flag")
        if row["guest_page_size"] not in {"4096", "16384"}:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid page size")
        exact_features(row["native_required_features"], stable_id)
        if row["native_requirement_scope"] not in {"instruction", "program"}:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} invalid native "
                "requirement scope"
            )
        if row["expected_event"] not in EXPECTED_EVENTS:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid event")
        if RETIREMENT.fullmatch(row["expected_retirement"]) is None:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid retirement")
        if STATE_ORACLE.fullmatch(row["expected_state_oracle"]) is None:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid state oracle")
        for column in (
            "initial_state_digest",
            "initial_memory_digest",
            "expected_state_digest",
            "expected_memory_digest",
        ):
            if DIGEST.fullmatch(row[column]) is None:
                fail(
                    f"{source}: {stable_id}/{row['semantic_id']} invalid {column}"
                )
        if row["expected_fault"] != "none" and FAULT.fullmatch(
            row["expected_fault"]
        ) is None:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid fault")
        if SYNDROME.fullmatch(row["expected_syndrome"]) is None:
            fail(f"{source}: {stable_id}/{row['semantic_id']} invalid syndrome")
        if VARIANT_ID.fullmatch(row["expectation_basis"]) is None:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} invalid "
                "expectation basis"
            )
        if row["entry_complete"] not in {"0", "1"}:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} invalid completeness flag"
            )
        if row["completion_basis"] not in {"incomplete", "fixed-word-total"}:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} invalid completion basis"
            )
        expected_oracle = (
            "harness-declared-v1+"
            f"{allocation['architecture_oracle']}+{allocation['decoder_oracle']}"
        )
        if row["oracle"] != expected_oracle:
            fail(
                f"{source}: {stable_id}/{row['semantic_id']} provenance is "
                f"{row['oracle']!r}, expected {expected_oracle!r}"
            )
        key = semantic_contract_key(row)
        if key in by_key:
            fail(
                f"{source}: duplicate semantic expectation for {stable_id}: "
                f"{row['semantic_id']} and {by_key[key]['semantic_id']}"
            )
        by_key[key] = row
        by_id[stable_id].append(row)

    missing_ids = sorted(set(inventory) - set(by_id))
    if missing_ids:
        fail(
            f"{source}: semantic contracts omit {len(missing_ids)} inventory "
            "entries: " + ", ".join(missing_ids)
        )

    complete_ids: set[str] = set()
    bindings_by_id: dict[str, list[dict[str, str]]] = defaultdict(list)
    for binding in bindings.values():
        bindings_by_id[binding["stable_id"]].append(binding)
    for stable_id, variants in by_id.items():
        flags = {row["entry_complete"] for row in variants}
        if flags == {"0"}:
            if any(row["completion_basis"] != "incomplete" for row in variants):
                fail(
                    f"{source}: incomplete {stable_id} claims a completion basis"
                )
            continue
        if flags != {"1"}:
            fail(f"{source}: {stable_id} mixes complete and incomplete variants")
        bases = {row["completion_basis"] for row in variants}
        if bases != {"fixed-word-total"}:
            fail(f"{source}: {stable_id} lacks one reviewed completion basis")
        expectation_bases = {row["expectation_basis"] for row in variants}
        if (
            len(expectation_bases) != 1
            or next(iter(expectation_bases)) == "harness-declared-case"
        ):
            fail(
                f"{source}: complete {stable_id} lacks one explicit "
                "specification reference oracle"
            )
        if (
            len(variants) != 2
            or {row["guest_page_size"] for row in variants}
            != {"4096", "16384"}
            or len(bindings_by_id[stable_id]) != 1
        ):
            fail(
                f"{source}: fixed-word-total {stable_id} must have exactly one "
                "semantic variant per supported page size and one binding"
            )
        allocation = bindings_by_id[stable_id][0]["_allocation"]
        if (
            allocation["architecture_mask"] != "ffffffff"
            or allocation["architecture_constraints"] != "none"
            or allocation["architecture_form_encoding_count"] != "1"
        ):
            fail(
                f"{source}: fixed-word-total {stable_id} is not one "
                "constraint-free, fully fixed Arm encoding"
            )
        complete_ids.add(stable_id)
    return by_key, complete_ids


def read_evidence(
    directory: Path, tests: set[str], repository: Path | None = None
) -> list[tuple[Path, dict[str, str]]]:
    if repository is None:
        repository = Path(__file__).resolve().parents[3]
    rows: list[tuple[Path, dict[str, str]]] = []
    for path in sorted(directory.glob("*.tsv")):
        if path.name == "stable-id-bindings.tsv":
            continue
        with path.open(encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source, delimiter="\t")
            if reader.fieldnames != EVIDENCE_COLUMNS:
                fail(
                    f"{path}: columns are {reader.fieldnames!r}, "
                    f"expected {EVIDENCE_COLUMNS!r}"
                )
            for row in reader:
                rows.append((path, row))
    if not rows:
        fail(f"no exact execution evidence was produced below {directory}")

    for path, row in rows:
        stable_id = row["stable_id"]
        if path.stem != row["ctest"]:
            fail(f"{path}: evidence CTest does not match its file name")
        if row["ctest"] not in tests:
            fail(f"{path}: evidence names unregistered CTest {row['ctest']!r}")
        if STABLE_ID.fullmatch(stable_id) is None:
            fail(f"{path}: invalid stable ID {stable_id!r}")
        if ENCODING.fullmatch(row["encoding"]) is None:
            fail(f"{path}: {stable_id} has invalid encoding")
        if row["invalid"] not in {"0", "1"}:
            fail(f"{path}: {stable_id} has invalid invalid-encoding flag")
        if row["guest_page_size"] not in {"4096", "16384"}:
            fail(f"{path}: {stable_id} has invalid guest page size")
        if row["expected_event"] not in EXPECTED_EVENTS:
            fail(f"{path}: {stable_id} has invalid expected event")
        if RETIREMENT.fullmatch(row["expected_retirement"]) is None:
            fail(f"{path}: {stable_id} has invalid retirement expectation")
        validate_state_oracle_locator(row, path, stable_id, repository)
        for column in (
            "initial_state_digest",
            "initial_memory_digest",
            "expected_state_digest",
            "expected_memory_digest",
        ):
            if DIGEST.fullmatch(row[column]) is None:
                fail(f"{path}: {stable_id} has invalid {column}")
        if row["expected_fault"] != "none" and FAULT.fullmatch(
            row["expected_fault"]
        ) is None:
            fail(f"{path}: {stable_id} has invalid fault expectation")
        if SYNDROME.fullmatch(row["expected_syndrome"]) is None:
            fail(f"{path}: {stable_id} has invalid syndrome expectation")
        required = exact_features(row["native_required_features"], stable_id)
        if row["native_requirement_scope"] not in {"instruction", "program"}:
            fail(f"{path}: {stable_id} has invalid native requirement scope")
        observed_parts = row["native_observed_features"].split("+")
        if not required:
            if observed_parts != ["baseline=1"]:
                fail(f"{path}: {stable_id} has invalid baseline observation")
            observed = {"baseline": True}
        else:
            observed: dict[str, bool] = {}
            for part in observed_parts:
                match = OBSERVED_FEATURE.fullmatch(part)
                if match is None or match.group(1) in observed:
                    fail(f"{path}: {stable_id} has malformed host observation")
                observed[match.group(1)] = match.group(2) == "1"
            if tuple(observed) != required:
                fail(
                    f"{path}: {stable_id} observed features do not exactly "
                    "match required features"
                )
        try:
            line = int(row["line"])
        except ValueError as error:
            raise ValueError(f"{path}: {stable_id} has invalid source line") from error
        if line <= 0 or not row["source"] or not row["case"]:
            fail(f"{path}: {stable_id} lacks an exact source/case location")
        if row["interpreter_result"] not in EXECUTED_RESULTS:
            fail(f"{path}: {stable_id} lacks interpreter execution")
        if row["gadgets_result"] not in EXECUTED_RESULTS:
            fail(f"{path}: {stable_id} lacks gadget execution")
        if row["jit_result"] not in JIT_RESULTS:
            fail(f"{path}: {stable_id} lacks JIT execution/host-feature skip")
        missing_required = any(not supported for supported in observed.values())
        expected_missing = (
            tuple(
                feature
                for feature in required
                if not observed.get(feature, False)
            )
            if required
            else ()
        )
        recorded_missing = (
            ()
            if row["native_missing_features"] == "none"
            else tuple(row["native_missing_features"].split("+"))
        )
        if recorded_missing != expected_missing:
            fail(
                f"{path}: {stable_id} missing features do not exactly match "
                "required/observed features"
            )
        if row["jit_result"] == "skipped-host-feature" and not missing_required:
            fail(f"{path}: {stable_id} feature skip names no missing feature")
        if row["jit_result"] in EXECUTED_RESULTS and missing_required:
            fail(f"{path}: {stable_id} JIT executed with a missing required feature")
        if row["expected_retirement"].startswith("exact:"):
            count = int(row["expected_retirement"].split(":", 1)[1])
            expected_result = (
                "executed-not-retired" if count == 0 else "executed-retired"
            )
            for engine in ("interpreter", "gadgets", "jit"):
                result = row[f"{engine}_result"]
                if result in EXECUTED_RESULTS and result != expected_result:
                    fail(
                        f"{path}: {stable_id} {engine} result disagrees with "
                        "the exact retirement expectation"
                    )
    return rows


def bind_exact(
    evidence: list[tuple[Path, dict[str, str]]],
    bindings: dict[tuple[str, str, str, str], dict[str, str]],
    semantic_contracts: dict[tuple[str, ...], dict[str, str]] | None = None,
) -> list[dict[str, str]]:
    by_variant: dict[
        tuple[str, str], list[tuple[Path, dict[str, str]]]
    ] = defaultdict(list)
    seen_bindings: set[tuple[str, str, str, str]] = set()
    seen_semantic_contracts: set[tuple[str, ...]] = set()
    for path, row in evidence:
        stable_id = row["stable_id"]
        locator = (stable_id, row["ctest"], row["case"], row["encoding"])
        binding = bindings.get(locator)
        if binding is None:
            fail(
                f"{path}: evidence has unbound variant execution "
                f"{stable_id}/{row['case']}/{row['encoding']}"
            )
        for evidence_column, binding_column in (
            ("section", "section"),
            ("architectural_form", "architectural_form"),
            ("alias_group", "alias_group"),
            ("ctest", "ctest"),
            ("case", "case"),
            ("encoding", "encoding"),
            ("native_required_features", "native_required_features"),
        ):
            if row[evidence_column] != binding[binding_column]:
                fail(
                    f"{path}: {stable_id} exact {evidence_column} does not "
                    "match its checked-in binding"
                )
        contract = binding["_contract"]
        if contract is not None:
            expected_invalid = "0" if contract["validity"] == "valid" else "1"
            if row["invalid"] != expected_invalid:
                fail(
                    f"{path}: {stable_id}/{contract['variant_id']} validity "
                    "partition does not match its independent contract"
                )
            for column in (
                "native_required_features",
                "expected_event",
                "expected_retirement",
                "expected_state_oracle",
                "expected_state_digest",
                "expected_memory_digest",
                "expected_fault",
                "expected_syndrome",
                "interpreter_result",
                "gadgets_result",
                "jit_result",
            ):
                if row[column] != contract[column]:
                    fail(
                        f"{path}: {stable_id}/{contract['variant_id']} exact "
                        f"{column} does not match its independent contract"
                    )
        seen_bindings.add(locator)
        if semantic_contracts is not None:
            semantic_key = semantic_contract_key(row)
            semantic_contract = semantic_contracts.get(semantic_key)
            if semantic_contract is None:
                fail(
                    f"{path}: {stable_id} execution has no independent "
                    "semantic contract"
                )
            seen_semantic_contracts.add(semantic_key)
            variant_id = semantic_contract["semantic_id"]
        else:
            variant_id = binding["_variant_id"]
            if binding["_contract"] is None:
                variant_id = provisional_execution_variant_id(variant_id, row)
        by_variant[(stable_id, variant_id)].append((path, row))

    missing = sorted(set(bindings) - seen_bindings)
    if missing:
        fail(
            f"{len(missing)} checked-in execution variants lack exact "
            "execution evidence: "
            + ", ".join(
                f"{stable_id}/{case}/{encoding}"
                for stable_id, _ctest, case, encoding in missing
            )
        )
    if semantic_contracts is not None:
        observed_page_sizes = {row["guest_page_size"] for _path, row in evidence}
        missing_semantic = sorted(
            key
            for key, contract in semantic_contracts.items()
            if contract["guest_page_size"] in observed_page_sizes
            and key not in seen_semantic_contracts
        )
        if missing_semantic:
            fail(
                f"{len(missing_semantic)} checked-in semantic contracts lack "
                "exact execution evidence: "
                + ", ".join(
                    f"{semantic_contracts[key]['stable_id']}/"
                    f"{semantic_contracts[key]['semantic_id']}"
                    for key in missing_semantic
                )
            )
    duplicates = sorted(
        variant for variant, rows in by_variant.items() if len(rows) != 1
    )
    if duplicates:
        fail(
            "variants have multiple execution records: "
            + ", ".join(f"{stable_id}/{variant}" for stable_id, variant in duplicates)
        )
    result = []
    for (stable_id, variant_id), rows in sorted(by_variant.items()):
        result.append({"variant_id": variant_id, **rows[0][1]})
    return result


def report_payload(rows: list[dict[str, str]]) -> bytes:
    import io

    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=REPORT_COLUMNS,
        delimiter="\t",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode("utf-8")


def run(options: argparse.Namespace) -> int:
    tool = find_llvm_mc(options.llvm_mc).resolve()
    options.llvm_mc = tool
    registration_inputs: list[Path] = []
    registration_files: list[Path] = []
    ctest_tool: Path | None = None
    if options.registered_tests_file is not None:
        registration_inputs.append(options.registered_tests_file)
    elif options.build_root is not None:
        registration_files = sorted(
            options.build_root.rglob("CTestTestfile.cmake")
        )
        registration_inputs.extend(registration_files)
        ctest = shutil.which("ctest")
        if ctest is None:
            fail("ctest is unavailable")
        ctest_tool = Path(ctest).resolve()
        registration_inputs.append(ctest_tool)
    evidence_inputs = sorted(
        path
        for path in options.evidence_directory.glob("*.tsv")
        if path.name != "stable-id-bindings.tsv"
    )
    catalog_inputs = (
        options.inventory,
        options.bindings,
        options.allocations,
        options.variant_contracts,
        options.semantic_contracts,
        tool,
        *registration_inputs,
        *evidence_inputs,
    )
    report = options.report
    validate_publication_topology((report,), read_inputs=catalog_inputs)
    tests = registered_tests(
        options.build_root,
        options.registered_tests_file,
        options.command_runner,
        ctest_tool,
    )
    inventory_rows = read_tsv(options.inventory, INVENTORY_COLUMNS)
    inventory = validate_inventory(
        inventory_rows, options.inventory, options.expected_count
    )
    binding_rows = read_tsv(options.bindings, BINDING_COLUMNS)
    allocation_rows = read_tsv(options.allocations, ALLOCATION_COLUMNS)
    allocations = validate_allocations(
        allocation_rows, options.allocations, inventory
    )
    contract_rows = read_tsv(options.variant_contracts, CONTRACT_COLUMNS)
    contracts = validate_variant_contracts(
        contract_rows, options.variant_contracts, inventory, allocations
    )
    oracle = require_pinned_llvm(tool, options.command_runner.run)
    if {row["decoder_oracle"] for row in allocation_rows} != {oracle}:
        fail(
            f"{options.allocations}: oracle provenance does not match "
            f"the pinned decoder {oracle}"
        )
    encoded_words = sorted(
        {
            int(row["encoding"], 16)
            for row in binding_rows
            if ENCODING.fullmatch(row["encoding"]) is not None
        }
    )
    decoded_words = dict(
        zip(
            encoded_words,
            decoder_opcodes(
                tool, encoded_words, runner=options.command_runner.run
            ),
        )
    )
    bindings = validate_bindings(
        binding_rows,
        options.bindings,
        inventory,
        allocations,
        tests,
        decoded_words,
        contracts,
    )
    semantic_rows = read_tsv(
        options.semantic_contracts, SEMANTIC_CONTRACT_COLUMNS
    )
    semantic_contracts, complete_ids = validate_semantic_contracts(
        semantic_rows,
        options.semantic_contracts,
        inventory,
        bindings,
    )
    if options.require_variant_complete and complete_ids != set(inventory):
        missing_complete = sorted(set(inventory) - complete_ids)
        fail(
            f"variant-complete qualification is absent for "
            f"{len(missing_complete)} entries: " + ", ".join(missing_complete)
        )
    evidence = read_evidence(options.evidence_directory, tests)
    exact = bind_exact(evidence, bindings, semantic_contracts)
    payload = report_payload(exact)

    def validate_staged(staged: list[tuple[Path, Path]]) -> None:
        if len(staged) != 1 or staged[0][1].read_bytes() != payload:
            fail("staged evidence report differs")

    def validate_published() -> None:
        if report.read_bytes() != payload:
            fail("published evidence report differs")

    atomic_publish_bytes(
        ((report, payload),),
        validate_staged=validate_staged,
        validate_published=validate_published,
    )
    jit_executed = sum(row["jit_result"] in EXECUTED_RESULTS for row in exact)
    stable_ids = {row["stable_id"] for row in exact}
    print(
        f"A64 exact evidence valid: {len(exact)} exact execution bindings "
        f"across {len(stable_ids)} stable IDs, "
        f"{len(semantic_contracts)} representative semantic contracts, "
        f"{len(contracts)} independently reviewed allocation partitions, "
        f"{len(complete_ids)}/{len(inventory)} variant-complete entries, "
        f"{jit_executed} JIT executions, "
        f"{len(exact) - jit_executed} exact host-feature skips"
    )
    return 0


def main() -> int:
    options = arguments()
    options.evidence_directory = canonical_existing_directory(
        options.evidence_directory, "evidence directory"
    )
    report = options.evidence_directory / "stable-id-bindings.tsv"
    outputs = (report,)
    options.report = report
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=outputs,
        prefix="a64-evidence-validator-",
    ) as workspace:
        options.command_runner = BoundedCommandRunner(
            workspace, options.timeout_seconds
        )
        return run(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
