#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Harvest declared A64 case expectations into semantic contracts.

The harvester deliberately ignores observed CPU state and engine output. Its
semantic payload consists only of the harness-declared initial state, expected
state/event/fault/memory/retirement, native requirement, and the independently
generated Arm XML/LLVM allocation identity for the checked-in binding.
Completeness is never inferred: existing reviewed flags are preserved and new
contracts are emitted incomplete. This legacy representative harvester is not
the unfiltered aggregate/partition closure gate.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import sys
from collections import Counter
from pathlib import Path

from a64_one_shot_publication import OneShotPublication
from a64_raw_evidence_files import (
    canonical_existing_directory,
    path_is_ancestor,
    paths_alias,
    require_no_symlink_components,
    validate_publication_topology,
)


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
SEMANTIC_COLUMNS = [
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
LEGACY_SEMANTIC_COLUMNS = [
    column
    for column in SEMANTIC_COLUMNS
    if column != "native_requirement_scope"
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--evidence-directory",
        type=Path,
        action="append",
        required=True,
        dest="evidence_directories",
    )
    parser.add_argument(
        "--bindings",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_evidence_bindings.tsv",
    )
    parser.add_argument(
        "--allocations",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_allocations.tsv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_semantic_contracts.tsv",
    )
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(
                f"{path}: columns are {reader.fieldnames!r}, expected {columns!r}"
            )
        return list(reader)


def preserved_semantics(
    path: Path,
) -> tuple[dict[tuple[str, str], tuple[str, str, str]], bool]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames == SEMANTIC_COLUMNS:
            rows = list(reader)
            return (
                {
                    (row["stable_id"], row["semantic_id"]): (
                        row["entry_complete"],
                        row["completion_basis"],
                        row["expectation_basis"],
                    )
                    for row in rows
                },
                False,
            )
        if reader.fieldnames != LEGACY_SEMANTIC_COLUMNS:
            raise ValueError(
                f"{path}: columns are {reader.fieldnames!r}, expected "
                f"{SEMANTIC_COLUMNS!r} or legacy {LEGACY_SEMANTIC_COLUMNS!r}"
            )
        rows = list(reader)
    reviewed = [
        row
        for row in rows
        if row["entry_complete"] != "0"
        or row["completion_basis"] != "incomplete"
        or row["expectation_basis"] != "harness-declared-case"
    ]
    if reviewed:
        raise ValueError(
            f"{path}: cannot infer native scope for {len(reviewed)} reviewed "
            "legacy semantic contracts"
        )
    # Native scope is part of the new semantic identity.  The legacy table is
    # accepted only because every row is explicitly unreviewed/incomplete;
    # new identities below remain incomplete rather than inheriting a claim.
    return {}, True


def evidence_file_paths(directories: tuple[Path, ...]) -> tuple[Path, ...]:
    paths: list[Path] = []
    for directory in directories:
        for candidate in sorted(directory.glob("*.tsv")):
            if candidate.name == "stable-id-bindings.tsv":
                continue
            path = require_no_symlink_components(candidate, "execution evidence")
            if not path.is_file():
                raise ValueError(f"execution evidence is not a regular file: {path}")
            paths.append(path)
    return tuple(paths)


def evidence_rows(paths: tuple[Path, ...]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in paths:
        rows.extend(read_tsv(path, EVIDENCE_COLUMNS))
    if not rows:
        raise ValueError("no execution evidence below supplied directories")
    return rows


def semantic_id(row: dict[str, str]) -> str:
    values = [
        row["stable_id"],
        row["ctest"],
        row["case"],
        row["encoding"],
        *(row[column] for column in SEMANTIC_SIGNATURE_COLUMNS),
    ]
    digest = hashlib.sha256("\0".join(values).encode("utf-8")).hexdigest()[:20]
    return f"semantic-{digest}"


def main() -> int:
    options = arguments()
    directories = tuple(
        canonical_existing_directory(path, "execution evidence directory")
        for path in options.evidence_directories
    )
    directory_identities = {
        (path.stat().st_dev, path.stat().st_ino) for path in directories
    }
    if len(directory_identities) != len(directories):
        raise ValueError("execution evidence directories alias each other")
    target = require_no_symlink_components(options.output, "publication target")
    for directory in directories:
        if (
            paths_alias(directory, target)
            or path_is_ancestor(directory, target)
            or path_is_ancestor(target, directory)
        ):
            raise ValueError(
                "semantic-contract output must be outside evidence directories"
            )
    validate_publication_topology(
        [target], read_inputs=[options.bindings, options.allocations]
    )
    snapshot = evidence_file_paths(directories)
    publication = OneShotPublication.prepare(
        [target], [options.bindings, options.allocations, *snapshot]
    )
    with __import__("contextlib").nullcontext():
        published_target = publication.targets[0]
        preserved: dict[tuple[str, str], tuple[str, str, str]] = {}
        legacy_migration = False
        if published_target.exists():
            preserved, legacy_migration = preserved_semantics(
                published_target
            )

            bindings = read_tsv(options.bindings, BINDING_COLUMNS)
            allocations = read_tsv(options.allocations, ALLOCATION_COLUMNS)
            binding_by_locator = {
                (row["stable_id"], row["ctest"], row["case"], row["encoding"]): row
                for row in bindings
            }
            if len(binding_by_locator) != len(bindings):
                raise ValueError(f"{options.bindings}: duplicate exact locator")
            allocation_by_word = {
                (row["stable_id"], row["binding_encoding"]): row
                for row in allocations
            }
            if len(allocation_by_word) != len(allocations):
                raise ValueError(f"{options.allocations}: duplicate exact allocation")

            output_rows: list[dict[str, str]] = []
            seen_semantics: set[tuple[str, str]] = set()
            seen_locators: set[tuple[str, str, str, str]] = set()
            for evidence in evidence_rows(snapshot):
                locator = (
                    evidence["stable_id"], evidence["ctest"],
                    evidence["case"], evidence["encoding"],
                )
                binding = binding_by_locator.get(locator)
                if binding is None:
                    raise ValueError(f"unbound execution evidence {locator!r}")
                for column in (
                    "section", "architectural_form", "alias_group",
                    "native_required_features",
                ):
                    if evidence[column] != binding[column]:
                        raise ValueError(f"{locator!r}: evidence differs in {column}")
                allocation = allocation_by_word.get(
                    (evidence["stable_id"], evidence["encoding"])
                )
                if allocation is None:
                    raise ValueError(f"{locator!r}: lacks exact allocation")
                identifier = semantic_id(evidence)
                semantic_identity = (evidence["stable_id"], identifier)
                if semantic_identity in seen_semantics:
                    raise ValueError(
                        f"duplicate semantic expectation {semantic_identity!r}"
                    )
                seen_semantics.add(semantic_identity)
                seen_locators.add(locator)
                complete, basis, expectation_basis = preserved.get(
                    semantic_identity,
                    ("0", "incomplete", "harness-declared-case"),
                )
                output_rows.append(
                    {
                        "stable_id": evidence["stable_id"],
                        "semantic_id": identifier,
                        "section": evidence["section"],
                        "architectural_form": evidence["architectural_form"],
                        "ctest": evidence["ctest"],
                        "case": evidence["case"],
                        "encoding": evidence["encoding"],
                        "guest_page_size": evidence["guest_page_size"],
                        "architecture_file": allocation["architecture_file"],
                        "architecture_encoding": allocation["architecture_encoding"],
                        "architecture_mask": allocation["architecture_mask"],
                        "architecture_value": allocation["architecture_value"],
                        "decoder_opcode": allocation["decoder_opcode"],
                        "decoder_mask": allocation["decoder_mask"],
                        "decoder_value": allocation["decoder_value"],
                        "invalid": evidence["invalid"],
                        "native_required_features": evidence[
                            "native_required_features"
                        ],
                        "native_requirement_scope": evidence[
                            "native_requirement_scope"
                        ],
                        "initial_state_digest": evidence["initial_state_digest"],
                        "initial_memory_digest": evidence["initial_memory_digest"],
                        "expected_event": evidence["expected_event"],
                        "expected_retirement": evidence["expected_retirement"],
                        "expected_state_oracle": evidence["expected_state_oracle"],
                        "expected_state_digest": evidence["expected_state_digest"],
                        "expected_memory_digest": evidence["expected_memory_digest"],
                        "expected_fault": evidence["expected_fault"],
                        "expected_syndrome": evidence["expected_syndrome"],
                        "expectation_basis": expectation_basis,
                        "entry_complete": complete,
                        "completion_basis": basis,
                        "oracle": (
                            "harness-declared-v1+"
                            f"{allocation['architecture_oracle']}+"
                            f"{allocation['decoder_oracle']}"
                        ),
                    }
                )

            missing_bindings = sorted(set(binding_by_locator) - seen_locators)
            if missing_bindings:
                raise ValueError(
                    f"{len(missing_bindings)} bindings emitted no execution evidence"
                )
            counts = Counter(row["stable_id"] for row in output_rows)
            if len(counts) != 983:
                raise ValueError(
                    f"semantic evidence covers {len(counts)} stable IDs, expected 983"
                )
            output_rows.sort(key=lambda row: (row["stable_id"], row["semantic_id"]))

            def recheck_evidence() -> None:
                if evidence_file_paths(locked_directories) != snapshot:
                    raise ValueError("execution evidence file set changed")

            publication.publish_tsvs_locked(
                [(published_target, SEMANTIC_COLUMNS, output_rows)],
                before_replace=recheck_evidence,
            )
    print(
        f"A64 semantic contracts harvested: {len(output_rows)} variants across "
        f"{len(counts)} stable IDs; completeness flags preserved only; "
        f"legacy_scope_migration={int(legacy_migration)}; "
        f"wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
