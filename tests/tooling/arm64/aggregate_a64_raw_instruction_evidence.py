#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Map unfiltered harness evidence to official Arm/LLVM aggregates."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import io
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from a64_architecture_oracle import feature_alternatives
from a64_bounded_generation import (
    BoundedCommandRunner,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_llvm_oracle import decoder_opcodes, find_llvm_mc, require_pinned_llvm
from a64_raw_evidence_files import (
    RAW_COLUMNS,
    atomic_publish_bytes,
    canonical_existing_directory,
    open_raw_text,
    paths_alias,
    raw_evidence_files,
    require_no_symlink_components,
    validate_publication_topology,
)
from generate_a64_instruction_allocations import exact_features
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS


OUTPUT_COLUMNS = [
    "aggregate_id", "stable_id", "section", "architectural_form",
    "architecture_file", "architecture_encoding", "architecture_mask",
    "architecture_value", "architecture_constraints", "operation_digest",
    "alias_digest", "decode_partition_id", "decode_class", "allocation_kind",
    "producer", "source_site", "source_file", "source_line", "validity_partition",
    "expected_native_feature_alternatives", "native_requirement_status",
    "semantic_native_features", "native_fallback_features",
    "native_required_features", "native_requirement_scope", "guest_page_size",
    "expected_event", "expected_retirement", "expected_state_oracle",
    "expected_fault_partition", "declared_count", "unique_word_count",
    "interpreter_executed_count", "gadgets_executed_count",
    "jit_executed_count", "interpreter_results", "gadgets_results",
    "jit_results", "native_observed_results", "native_missing_results",
    "decoder_results", "declared_digest", "observed_digest",
    "architecture_oracle", "decoder_oracle",
]
CONSTRAINT = re.compile(
    r"[^@]+@([0-9]+):([1-9][0-9]*)(!=|==)([01x]+)\Z"
)
EXECUTED = {"executed-not-retired", "executed-retired"}
AGGREGATE_ID_COLUMNS = [
    "stable_id", "producer", "source_site", "source_file", "source_line",
    "architecture_encoding", "architecture_mask", "architecture_value",
    "architecture_constraints", "decode_partition_id", "decode_class",
    "allocation_kind", "validity_partition",
    "expected_native_feature_alternatives", "native_requirement_status",
    "semantic_native_features", "native_fallback_features",
    "native_required_features", "native_requirement_scope", "guest_page_size", "expected_event",
    "expected_retirement", "expected_state_oracle", "expected_fault_partition",
]


def aggregate_id(row: dict[str, str]) -> str:
    identity = "\0".join(row[column] for column in AGGREGATE_ID_COLUMNS)
    return "aggregate-" + hashlib.sha256(identity.encode()).hexdigest()[:20]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", action="append", type=Path,
                        required=True, dest="evidence_directories")
    parser.add_argument(
        "--required-variants", type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
    )
    parser.add_argument(
        "--decode-partitions", type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unmapped-output", type=Path)
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument("--decode-chunk-size", type=int, default=20000)
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(f"{path}: unexpected columns {reader.fieldnames!r}")
        return list(reader)


@dataclass(frozen=True)
class Pattern:
    row: dict[str, str] = field(compare=False, hash=False)
    mask: int
    value: int
    constraints: tuple[tuple[int, int, str, str], ...]

    def matches(self, word: int) -> bool:
        if word & self.mask != self.value:
            return False
        for low, width, operator, expected in self.constraints:
            actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
            equal = all(a == "x" or a == b for a, b in zip(expected, actual))
            if (operator == "==") != equal:
                return False
        return True


def parse_constraints(*values: str) -> tuple[tuple[int, int, str, str], ...]:
    constraints = []
    for value in values:
        if value == "none":
            continue
        for item in value.split(";"):
            match = CONSTRAINT.fullmatch(item)
            if match is None:
                raise ValueError(f"unsupported required constraint {item!r}")
            parsed = (
                int(match.group(1)), int(match.group(2)),
                match.group(3), match.group(4),
            )
            if parsed not in constraints:
                constraints.append(parsed)
    return tuple(constraints)


def parse_patterns(
    rows: list[dict[str, str]],
    constraint_columns: tuple[str, ...] = ("architecture_constraints",),
    additional_predicates: tuple[tuple[str, str], ...] = (),
) -> dict[int, list[Pattern]]:
    by_prefix: dict[int, list[Pattern]] = defaultdict(list)
    for row in rows:
        constraints = parse_constraints(
            *(row[column] for column in constraint_columns)
        )
        mask = int(row["architecture_mask"], 16)
        value = int(row["architecture_value"], 16)
        for mask_column, value_column in additional_predicates:
            additional_mask = int(row[mask_column], 16)
            additional_value = int(row[value_column], 16)
            if (value ^ additional_value) & mask & additional_mask:
                raise ValueError("required predicates contain conflicting bits")
            value = (value & mask) | (additional_value & additional_mask)
            mask |= additional_mask
        pattern = Pattern(row, mask, value, constraints)
        prefix_mask = pattern.mask >> 20
        prefix_value = pattern.value >> 20
        for prefix in range(1 << 12):
            if prefix & prefix_mask == prefix_value:
                by_prefix[prefix].append(pattern)
    return by_prefix


RULE_CONDITION = re.compile(r"[^@]+@([0-9]+):([1-9][0-9]*)=([01x]+)\Z")


def matching_rule_expressions(
    required: dict[str, str], word: int, column: str
) -> tuple[str, ...]:
    expressions: list[str] = []
    if required[column] != "none":
        for rule in required[column].split(";"):
            condition_text, expression = rule.split("=>", 1)
            matched = True
            for condition in condition_text.split("&"):
                parsed = RULE_CONDITION.fullmatch(condition)
                if parsed is None:
                    raise ValueError(f"unsupported word feature rule {rule!r}")
                low, width = int(parsed.group(1)), int(parsed.group(2))
                pattern = parsed.group(3)
                actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
                if any(a != "x" and a != b for a, b in zip(pattern, actual)):
                    matched = False
                    break
            if matched:
                expressions.append(expression)
    return tuple(dict.fromkeys(expressions))


def matching_feature_expressions(
    required: dict[str, str], word: int
) -> tuple[str, ...]:
    expressions = (
        []
        if required["static_feature_expressions"] == "baseline"
        else required["static_feature_expressions"].split(";")
    )
    expressions.extend(
        matching_rule_expressions(required, word, "word_feature_rules")
    )
    return tuple(dict.fromkeys(expressions))


def alternatives_text(required: dict[str, str], word: int) -> str:
    alternatives = feature_alternatives(
        matching_feature_expressions(required, word),
        required["architecture_file"],
    )
    return "|".join(sorted(
        "baseline" if not item else "+".join(sorted(item))
        for item in alternatives
    ))


def fallback_features(required: dict[str, str], word: int) -> frozenset[str]:
    result = set(exact_features(required["static_native_fallback_features"]))
    expressions = matching_rule_expressions(
        required, word, "word_feature_fallback_rules"
    )
    for alternative in feature_alternatives(
        expressions, required["architecture_file"]
    ):
        result.update(alternative)
    return frozenset(result)


def semantic_features_for_word(
    required: dict[str, str], word: int
) -> frozenset[str]:
    """Return static plus exact word-selected semantic runtime features."""
    result = set(exact_features(required["semantic_native_features"]))
    expressions = matching_rule_expressions(
        required, word, "word_semantic_feature_rules"
    )
    for alternative in feature_alternatives(
        expressions, required["architecture_file"]
    ):
        result.update(alternative)
    return frozenset(result)


def possible_semantic_feature_sets(
    required: dict[str, str],
) -> set[frozenset[str]]:
    """Enumerate semantic sets licensed by a required allocation's rules."""
    rules = required["word_semantic_feature_rules"]
    if rules == "none":
        return {exact_features(required["semantic_native_features"])}
    bits: set[int] = set()
    for rule in rules.split(";"):
        condition_text, _expression = rule.split("=>", 1)
        for condition in condition_text.split("&"):
            parsed = RULE_CONDITION.fullmatch(condition)
            if parsed is None:
                raise ValueError(f"unsupported word semantic rule {rule!r}")
            low, width = int(parsed.group(1)), int(parsed.group(2))
            bits.update(range(low, low + width))
    free_bits = sorted(
        bit for bit in bits
        if not int(required["architecture_mask"], 16) & (1 << bit)
    )
    if len(free_bits) > 20:
        raise ValueError("word semantic rules expose too many free bits")
    base = int(required["architecture_value"], 16)
    result: set[frozenset[str]] = set()
    for settings in range(1 << len(free_bits)):
        word = base
        for index, bit in enumerate(free_bits):
            if settings & (1 << index):
                word |= 1 << bit
        result.add(semantic_features_for_word(required, word))
    return result


def counter_text(counter: Counter[str]) -> str:
    return "+".join(f"{name}:{counter[name]}" for name in sorted(counter))


@dataclass
class Aggregate:
    key: tuple[str, ...]
    required: dict[str, str]
    declared_count: int = 0
    words: set[int] = field(default_factory=set)
    engine_results: dict[str, Counter[str]] = field(
        default_factory=lambda: {
            "interpreter": Counter(), "gadgets": Counter(), "jit": Counter()
        }
    )
    observed: Counter[str] = field(default_factory=Counter)
    missing: Counter[str] = field(default_factory=Counter)
    decoders: Counter[str] = field(default_factory=Counter)
    declared_hash: object = field(default_factory=hashlib.sha256)
    observed_hash: object = field(default_factory=hashlib.sha256)

    def add(self, raw: dict[str, str], word: int, opcode: str | None) -> None:
        count = int(raw["occurrences"])
        self.declared_count += count
        self.words.add(word)
        decoder = opcode or "invalid"
        self.decoders[decoder] += count
        self.observed[raw["native_observed_features"]] += count
        self.missing[raw["native_missing_features"]] += count
        for engine in ("interpreter", "gadgets", "jit"):
            self.engine_results[engine][raw[f"{engine}_result"]] += count
        declared = "\0".join(
            raw[column] for column in (
                "case", "encoding", "invalid", "guest_page_size",
                "native_required_features", "native_requirement_scope",
                "initial_state_digest",
                "initial_memory_digest", "expected_event",
                "expected_retirement", "expected_state_oracle",
                "expected_state_digest", "expected_memory_digest",
                "expected_fault", "expected_syndrome", "occurrences",
            )
        )
        observed = "\0".join(
            raw[column] for column in (
                "native_observed_features", "native_missing_features",
                "interpreter_result", "gadgets_result", "jit_result",
            )
        ) + "\0" + decoder
        self.declared_hash.update(declared.encode() + b"\n")
        self.observed_hash.update(observed.encode() + b"\n")


def raw_rows(directories: list[Path]):
    for directory in directories:
        for entry in raw_evidence_files(directory):
            with open_raw_text(entry) as source:
                reader = csv.DictReader(source, delimiter="\t")
                if reader.fieldnames != RAW_COLUMNS:
                    raise ValueError(f"{entry.path}: unexpected raw columns")
                for row in reader:
                    if row["native_requirement_scope"] not in {
                        "instruction", "program"
                    }:
                        raise ValueError(
                            f"{entry.path}: invalid native requirement scope"
                        )
                    yield row


def fault_partition(value: str) -> str:
    return "none" if value == "none" else value.split(":", 1)[0]


def source_identity(raw: dict[str, str]) -> tuple[str, str, str, str]:
    source = raw["source"]
    marker = "/tests/"
    source_file = "tests/" + source.split(marker, 1)[1] if marker in source else source
    source_site = hashlib.sha256(
        f"{raw['ctest']}:{source_file}:{raw['line']}".encode()
    ).hexdigest()[:20]
    return raw["ctest"], source_site, source_file, raw["line"]


def select_allocations(candidates: list[Pattern], word: int) -> list[Pattern]:
    by_stable_id: dict[str, list[Pattern]] = defaultdict(list)
    for pattern in candidates:
        if pattern.matches(word):
            by_stable_id[pattern.row["stable_id"]].append(pattern)
    result = []
    for stable_id, matches in by_stable_id.items():
        specificity = max(pattern.mask.bit_count() for pattern in matches)
        matches = [
            pattern for pattern in matches
            if pattern.mask.bit_count() == specificity
        ]
        identities = {
            (
                pattern.row["architecture_encoding"],
                pattern.row["architecture_mask"],
                pattern.row["architecture_value"],
                pattern.row["architecture_constraints"],
                pattern.row["decode_mask"],
                pattern.row["decode_value"],
                pattern.row["decode_constraints"],
                pattern.row["decode_encoding"],
            )
            for pattern in matches
        }
        if len(identities) != 1:
            raise ValueError(
                f"{stable_id} {word:08x}: ambiguous official allocations"
            )
        result.append(matches[0])
    return sorted(result, key=lambda pattern: pattern.row["stable_id"])


def select_decode_partition(
    hierarchy: dict[int, list[Pattern]],
    tables: dict[str, list[Pattern]],
    word: int,
) -> Pattern:
    leaves = [
        pattern for pattern in hierarchy[word >> 20] if pattern.matches(word)
    ]
    if len(leaves) != 1:
        raise ValueError(
            f"{word:08x}: official hierarchy matches {len(leaves)} leaves"
        )
    leaf = leaves[0]
    if leaf.row["partition_kind"] == "unallocated":
        return leaf
    rows = [
        pattern
        for pattern in tables.get(leaf.row["decode_class"], [])
        if pattern.matches(word)
    ]
    if len(rows) != 1:
        raise ValueError(
            f"{word:08x}: decode class {leaf.row['decode_class']} matches "
            f"{len(rows)} terminal rows"
        )
    return rows[0]


def run(options: argparse.Namespace) -> int:
    if options.decode_chunk_size <= 0:
        raise ValueError("decode chunk size must be positive")
    required_rows = read_tsv(options.required_variants, REQUIRED_COLUMNS)
    patterns = parse_patterns(
        required_rows,
        ("architecture_constraints", "decode_constraints"),
        (("decode_mask", "decode_value"),),
    )
    decode_rows = read_tsv(options.decode_partitions, DECODE_COLUMNS)
    decode_patterns = parse_patterns(decode_rows)
    hierarchy = defaultdict(list)
    tables: dict[str, list[Pattern]] = defaultdict(list)
    for prefix, items in decode_patterns.items():
        hierarchy[prefix].extend(
            item for item in items if item.row["decode_layer"] == "hierarchy"
        )
    for row in decode_rows:
        if row["decode_layer"] != "instruction-table":
            continue
        pattern = Pattern(
            row,
            int(row["architecture_mask"], 16),
            int(row["architecture_value"], 16),
            parse_constraints(row["architecture_constraints"]),
        )
        tables[row["decode_class"]].append(pattern)
    decode_id_by_identity = {
        (
            row["decode_class"], row["partition_name"],
            row["architecture_file"], row["architecture_mask"],
            row["architecture_value"], row["architecture_constraints"],
        ):
        row["partition_id"]
        for row in decode_rows
        if row["decode_layer"] == "instruction-table"
    }
    architecture_oracles = {row["architecture_oracle"] for row in required_rows}
    if len(architecture_oracles) != 1:
        raise ValueError("required model mixes architecture oracles")
    tool = options.llvm_mc
    if tool is None:
        raise ValueError("bounded aggregate lacks its LLVM tool")
    decoder_oracle = require_pinned_llvm(tool, options.command_runner.run)
    aggregates: dict[tuple[str, ...], Aggregate] = {}
    unmapped: Counter[tuple[str, str, str, str]] = Counter()
    contract_drifts: Counter[str] = Counter()
    total = 0

    iterator = iter(raw_rows(options.evidence_directories))
    while True:
        chunk = []
        try:
            for _ in range(options.decode_chunk_size):
                chunk.append(next(iterator))
        except StopIteration:
            pass
        if not chunk:
            break
        words = [int(row["encoding"], 16) for row in chunk]
        opcodes = decoder_opcodes(tool, words, runner=options.command_runner.run)
        for raw, word, opcode in zip(chunk, words, opcodes):
            total += int(raw["occurrences"])
            matches = select_allocations(patterns[word >> 20], word)
            allocation_features: dict[
                int,
                tuple[
                    str,
                    set[frozenset[str]],
                    frozenset[str],
                    frozenset[str],
                ],
            ] = {}
            declared_features = exact_features(raw["native_required_features"])
            for allocation in matches:
                text_value = alternatives_text(allocation.row, word)
                allocation_features[id(allocation)] = (
                    text_value,
                    {
                        exact_features(item)
                        for item in text_value.split("|")
                    },
                    semantic_features_for_word(allocation.row, word),
                    fallback_features(allocation.row, word),
                )
            some_allocation_matches_features = any(
                any(
                    (
                        required.issubset(declared_features)
                        and declared_features - required <= semantic
                    )
                    or (
                        required - declared_features <= fallback
                        and declared_features - required <= semantic
                    )
                    for required in alternatives
                )
                for _text, alternatives, semantic, fallback
                in allocation_features.values()
            )
            if matches:
                mapped: list[tuple[Pattern, str]] = [
                    (pattern, "allocated") for pattern in matches
                ]
            else:
                decoded = select_decode_partition(hierarchy, tables, word)
                mapped = [(decoded, "decode")]
            producer, source_site, source_file, source_line = source_identity(raw)
            for pattern, mapping_kind in mapped:
                required = pattern.row
                if mapping_kind == "allocated":
                    validity = (
                        "allocated-valid" if raw["invalid"] == "0"
                        else "allocated-profile-illegal"
                    )
                    (
                        expected_alternatives,
                        expected_sets,
                        semantic_features,
                        native_fallback_features,
                    ) = allocation_features[id(pattern)]
                    semantically_related = any(
                        required.issubset(declared_features)
                        and declared_features - required <= semantic_features
                        for required in expected_sets
                    )
                    feature_fallback = any(
                        required - declared_features
                        <= native_fallback_features
                        and declared_features - required
                        <= semantic_features
                        for required in expected_sets
                    )
                    native_status = (
                        "not-applicable-invalid"
                        if raw["invalid"] == "1"
                        else (
                            "exact"
                            if declared_features in expected_sets
                            else (
                                "semantic-feature-gate"
                                if semantically_related
                                else (
                                    "architectural-feature-fallback"
                                    if feature_fallback
                                    else "drift"
                                )
                            )
                        )
                    )
                    if (
                        native_status == "drift"
                        and some_allocation_matches_features
                    ):
                        native_status = "overlapping-allocation"
                    if (
                        native_status == "drift"
                        and raw["native_requirement_scope"] == "program"
                    ):
                        native_status = "program-level-gate"
                    if raw["invalid"] == "0" and opcode is None:
                        contract_drifts["llvm-rejects-declared-valid"] += int(raw["occurrences"])
                    decode_partition_id = decode_id_by_identity.get(
                        (
                            required["decode_class"],
                            required["decode_encoding"],
                            required["decode_architecture_file"],
                            required["decode_mask"],
                            required["decode_value"],
                            required["decode_constraints"],
                        )
                    )
                    if decode_partition_id is None:
                        raise ValueError("required allocation lacks decode partition")
                    stable_ids = [required["stable_id"]]
                    allocation_kind = required["allocation_kind"]
                else:
                    validity = (
                        required["partition_kind"]
                        if raw["invalid"] == "1"
                        else required["partition_kind"] + "-declared-valid"
                    )
                    if raw["invalid"] == "0":
                        contract_drifts["official-unallocated-declared-valid"] += int(raw["occurrences"])
                    expected_alternatives = (
                        required["feature_condition"]
                        if required["feature_condition"] != "baseline"
                        else "baseline"
                    )
                    semantic_features = frozenset()
                    native_fallback_features = frozenset()
                    native_status = "not-applicable"
                    decode_partition_id = required["partition_id"]
                    stable_ids = (
                        required["related_stable_ids"].split("+")
                        if required["related_stable_ids"] != "none"
                        else ["none"]
                    )
                    allocation_kind = "reserved"
                for stable_id in stable_ids:
                    section = required.get("section", "none")
                    form = required.get("architectural_form", "none")
                    architecture_file = required["architecture_file"]
                    architecture_encoding = required.get(
                        "architecture_encoding", required.get("partition_name", "none")
                    )
                    architecture_mask = required["architecture_mask"]
                    architecture_value = required["architecture_value"]
                    architecture_constraints = required["architecture_constraints"]
                    operation_digest = required.get("operation_digest", "none")
                    alias_digest = required.get("alias_digest", "none")
                    decode_class = required["decode_class"]
                    key = (
                        stable_id, producer, source_site, source_file, source_line,
                        architecture_encoding, architecture_mask,
                        architecture_value, architecture_constraints,
                        decode_partition_id, decode_class, allocation_kind,
                        validity, expected_alternatives, native_status,
                        "+".join(sorted(semantic_features)) or "baseline",
                        "+".join(sorted(native_fallback_features)) or "baseline",
                        raw["native_required_features"],
                        raw["native_requirement_scope"],
                        raw["guest_page_size"],
                        raw["expected_event"], raw["expected_retirement"],
                        raw["expected_state_oracle"],
                        fault_partition(raw["expected_fault"]),
                    )
                    aggregate = aggregates.get(key)
                    if aggregate is None:
                        normalized = dict(required)
                        normalized.update({
                            "stable_id": stable_id,
                            "section": section,
                            "architectural_form": form,
                            "architecture_file": architecture_file,
                            "architecture_encoding": architecture_encoding,
                            "architecture_mask": architecture_mask,
                            "architecture_value": architecture_value,
                            "architecture_constraints": architecture_constraints,
                            "operation_digest": operation_digest,
                            "alias_digest": alias_digest,
                        })
                        aggregate = Aggregate(key, normalized)
                        aggregates[key] = aggregate
                    aggregate.add(raw, word, opcode)

    output_rows = []
    for key, aggregate in aggregates.items():
        (stable_id, producer, source_site, source_file, source_line,
         architecture_encoding, architecture_mask, architecture_value,
         constraints, decode_partition_id, decode_class, allocation_kind,
         validity, expected_alternatives, native_status, semantic, fallback,
         native, native_scope, page, event,
         retirement, state_oracle, fault) = key
        required = aggregate.required
        if native_status == "drift":
            contract_drifts["native-feature"] += aggregate.declared_count
        output_row = {
            "aggregate_id": "",
            "stable_id": stable_id, "section": required["section"],
            "architectural_form": required["architectural_form"],
            "architecture_file": required["architecture_file"],
            "architecture_encoding": architecture_encoding,
            "architecture_mask": architecture_mask,
            "architecture_value": architecture_value,
            "architecture_constraints": constraints,
            "operation_digest": required["operation_digest"],
            "alias_digest": required["alias_digest"],
            "decode_partition_id": decode_partition_id,
            "decode_class": decode_class,
            "allocation_kind": allocation_kind,
            "producer": producer,
            "source_site": source_site,
            "source_file": source_file, "source_line": source_line,
            "validity_partition": validity,
            "expected_native_feature_alternatives": expected_alternatives,
            "native_requirement_status": native_status,
            "semantic_native_features": semantic,
            "native_fallback_features": fallback,
            "native_required_features": native,
            "native_requirement_scope": native_scope,
            "guest_page_size": page,
            "expected_event": event, "expected_retirement": retirement,
            "expected_state_oracle": state_oracle,
            "expected_fault_partition": fault,
            "declared_count": str(aggregate.declared_count),
            "unique_word_count": str(len(aggregate.words)),
            "interpreter_executed_count": str(sum(v for k, v in aggregate.engine_results["interpreter"].items() if k in EXECUTED)),
            "gadgets_executed_count": str(sum(v for k, v in aggregate.engine_results["gadgets"].items() if k in EXECUTED)),
            "jit_executed_count": str(sum(v for k, v in aggregate.engine_results["jit"].items() if k in EXECUTED)),
            "interpreter_results": counter_text(aggregate.engine_results["interpreter"]),
            "gadgets_results": counter_text(aggregate.engine_results["gadgets"]),
            "jit_results": counter_text(aggregate.engine_results["jit"]),
            "native_observed_results": counter_text(aggregate.observed),
            "native_missing_results": counter_text(aggregate.missing),
            "decoder_results": counter_text(aggregate.decoders),
            "declared_digest": aggregate.declared_hash.hexdigest(),
            "observed_digest": aggregate.observed_hash.hexdigest(),
            "architecture_oracle": next(iter(architecture_oracles)),
            "decoder_oracle": decoder_oracle,
        }
        output_row["aggregate_id"] = aggregate_id(output_row)
        output_rows.append(output_row)
    output_rows.sort(key=lambda row: (row["stable_id"], row["aggregate_id"]))
    print(f"A64 raw aggregate map: {total} invocations, {len(output_rows)} aggregates, "
          f"{sum(unmapped.values())} unmapped invocations, "
          f"{sum(contract_drifts.values())} contract drifts")
    if unmapped:
        raise ValueError("raw evidence contains official-decode-unmapped words")
    if contract_drifts:
        raise ValueError(
            "raw evidence disagrees with official/LLVM contracts: "
            + counter_text(contract_drifts)
        )
    aggregate_text = io.StringIO(newline="")
    aggregate_writer = csv.DictWriter(
        aggregate_text,
        fieldnames=OUTPUT_COLUMNS,
        delimiter="\t",
        lineterminator="\n",
    )
    aggregate_writer.writeheader()
    aggregate_writer.writerows(output_rows)
    outputs: list[tuple[Path, bytes]] = [
        (options.output, aggregate_text.getvalue().encode("utf-8"))
    ]
    if options.unmapped_output is not None:
        unmapped_text = io.StringIO(newline="")
        unmapped_writer = csv.writer(
            unmapped_text, delimiter="\t", lineterminator="\n"
        )
        unmapped_writer.writerow(
            ("source", "line", "encoding", "invalid", "count")
        )
        for key, count in sorted(unmapped.items()):
            unmapped_writer.writerow((*key, count))
        outputs.append(
            (options.unmapped_output, unmapped_text.getvalue().encode("utf-8"))
        )

    def validate_files(files: list[tuple[Path, Path]]) -> None:
        expected = {path: payload for path, payload in outputs}
        for target, observed in files:
            if observed.read_bytes() != expected[target]:
                raise ValueError(f"staged aggregate output differs: {target}")

    def validate_published() -> None:
        for target, payload in outputs:
            if target.read_bytes() != payload:
                raise ValueError(f"published aggregate output differs: {target}")

    atomic_publish_bytes(
        outputs,
        validate_staged=validate_files,
        validate_published=validate_published,
    )
    return 0


def publish(options: argparse.Namespace) -> int:
    tool = find_llvm_mc(options.llvm_mc)
    options.llvm_mc = tool.resolve()
    raw_paths = [
        entry.path
        for directory in options.evidence_directories
        for entry in raw_evidence_files(directory)
    ]
    output_targets = [options.output]
    if options.unmapped_output is not None:
        output_targets.append(options.unmapped_output)
    read_inputs = (
        options.required_variants,
        options.decode_partitions,
        options.llvm_mc,
        *raw_paths,
    )
    files, _directories, _reads = validate_publication_topology(
        output_targets, read_inputs=read_inputs
    )
    if any(
        paths_alias(output.parent, evidence)
        for output in files
        for evidence in options.evidence_directories
    ):
        raise ValueError(
            "aggregate output parent aliases a raw evidence directory"
        )
    with __import__("contextlib").nullcontext(files) as outputs:
        options.output = outputs[0]
        if options.unmapped_output is not None:
            options.unmapped_output = outputs[1]
        with workspace_from_options(
            options,
            source_root=Path(__file__).resolve().parents[3],
            publication_targets=outputs,
            prefix="a64-raw-aggregate-",
        ) as workspace:
            options.command_runner = BoundedCommandRunner(
                workspace, options.timeout_seconds
            )
            return run(options)


def main() -> int:
    options = arguments()
    options.evidence_directories = [
        canonical_existing_directory(path, "raw evidence path")
        for path in options.evidence_directories
    ]
    return publish(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
