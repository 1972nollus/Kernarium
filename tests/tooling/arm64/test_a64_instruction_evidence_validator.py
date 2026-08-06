#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative fixtures for exact A64 instruction-evidence validation."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from a64_raw_evidence_files import producer_evidence_lock


SOURCE = Path(__file__).with_name("validate_a64_instruction_evidence.py")
SPEC = importlib.util.spec_from_file_location("a64_instruction_evidence", SOURCE)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {SOURCE}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def inventory(
    stable_id: str, form: str, native: str = "baseline"
) -> dict[str, str]:
    return {
        "stable_id": stable_id,
        "section": "Base",
        "pdf_label": form,
        "native_feature_requirements": native,
    }


def binding(
    stable_id: str,
    form: str,
    encoding: str,
    case: str | None = None,
    native: str = "baseline",
) -> dict[str, str]:
    return {
        "stable_id": stable_id,
        "section": "Base",
        "architectural_form": form,
        "ctest": "fixture-test",
        "case": case or stable_id,
        "encoding": encoding,
        "alias_group": "-",
        "native_required_features": native,
    }


def allocation(
    form: str,
    encoding_value: str,
    encoding_mask: str = "ffffffff",
    decoder_opcode: str = "FIXTURE",
    architecture_native: str = "baseline",
) -> dict[str, str]:
    return {
        "stable_id": "",
        "section": "Base",
        "architectural_form": form,
        "binding_encoding": encoding_value,
        "architecture_file": "fixture.xml",
        "architecture_encoding": "FIXTURE",
        "architecture_form_encoding_count": "1",
        "architecture_mask": encoding_mask,
        "architecture_value": encoding_value,
        "architecture_constraints": "none",
        "architecture_features": "fixture",
        "architecture_native_feature_alternatives": architecture_native,
        "decoder_opcode": decoder_opcode,
        "decoder_mask": encoding_mask,
        "decoder_value": encoding_value,
        "encoding_mask": encoding_mask,
        "encoding_value": encoding_value,
        "architecture_oracle": VALIDATOR.SOURCE_ORACLE,
        "decoder_oracle": "llvm-mc-fixture",
    }


def evidence(
    row: dict[str, str],
    *,
    invalid: str = "0",
    event: str = "system-call",
    retirement: str = "exact:2",
    initial_state_digest: str = "0011223344556677",
    initial_memory_digest: str = "7766554433221100",
    state_digest: str = "0123456789abcdef",
    page_size: str = "4096",
    interpreter_result: str = "executed-retired",
    gadgets_result: str = "executed-retired",
    jit_result: str = "executed-retired",
) -> tuple[Path, dict[str, str]]:
    return Path("fixture-test.tsv"), {
        "stable_id": row["stable_id"],
        "section": row["section"],
        "architectural_form": row["architectural_form"],
        "alias_group": row["alias_group"],
        "ctest": row["ctest"],
        "case": row["case"],
        "source": "fixture.cpp",
        "line": "1",
        "encoding": row["encoding"],
        "invalid": invalid,
        "guest_page_size": page_size,
        "expected_event": event,
        "expected_retirement": retirement,
        "expected_state_oracle": "exact",
        "initial_state_digest": initial_state_digest,
        "initial_memory_digest": initial_memory_digest,
        "expected_state_digest": state_digest,
        "expected_memory_digest": "fedcba9876543210",
        "expected_fault": "none",
        "expected_syndrome": "00000000",
        "native_required_features": row["native_required_features"],
        "native_requirement_scope": "instruction",
        "native_observed_features": (
            "baseline=1"
            if row["native_required_features"] == "baseline"
            else "+".join(
                f"{feature}=1"
                for feature in row["native_required_features"].split("+")
            )
        ),
        "native_missing_features": "none",
        "interpreter_result": interpreter_result,
        "gadgets_result": gadgets_result,
        "jit_result": jit_result,
    }


def contract(
    stable_id: str,
    form: str,
    allocation_row: dict[str, str],
    variant_id: str,
    *,
    validity: str = "valid",
    native: str = "baseline",
    event: str = "system-call",
    retirement: str = "exact:2",
    state_digest: str = "0123456789abcdef",
    result: str = "executed-retired",
) -> dict[str, str]:
    return {
        "stable_id": stable_id,
        "variant_id": variant_id,
        "section": "Base",
        "architectural_form": form,
        "decoder_opcode": allocation_row["decoder_opcode"],
        "encoding_mask": allocation_row["encoding_mask"],
        "encoding_value": allocation_row["encoding_value"],
        "validity": validity,
        "native_required_features": native,
        "expected_event": event,
        "expected_retirement": retirement,
        "expected_state_oracle": "exact",
        "expected_state_digest": state_digest,
        "expected_memory_digest": "fedcba9876543210",
        "expected_fault": "none",
        "expected_syndrome": "00000000",
        "interpreter_result": result,
        "gadgets_result": result,
        "jit_result": result,
        "entry_complete": "0",
        "oracle": "arm-a-profile-fixture+llvm-mc-fixture",
    }


def semantic_contract(
    binding_row: dict[str, str],
    allocation_row: dict[str, str],
    evidence_row: dict[str, str],
    semantic_id: str,
) -> dict[str, str]:
    return {
        "stable_id": binding_row["stable_id"],
        "semantic_id": semantic_id,
        "section": binding_row["section"],
        "architectural_form": binding_row["architectural_form"],
        "ctest": binding_row["ctest"],
        "case": binding_row["case"],
        "encoding": binding_row["encoding"],
        "guest_page_size": evidence_row["guest_page_size"],
        "architecture_file": allocation_row["architecture_file"],
        "architecture_encoding": allocation_row["architecture_encoding"],
        "architecture_mask": allocation_row["architecture_mask"],
        "architecture_value": allocation_row["architecture_value"],
        "decoder_opcode": allocation_row["decoder_opcode"],
        "decoder_mask": allocation_row["decoder_mask"],
        "decoder_value": allocation_row["decoder_value"],
        **{
            column: evidence_row[column]
            for column in VALIDATOR.SEMANTIC_SIGNATURE_COLUMNS
            if column != "guest_page_size"
        },
        "expectation_basis": "harness-declared-case",
        "entry_complete": "0",
        "completion_basis": "incomplete",
        "oracle": (
            "harness-declared-v1+"
            f"{allocation_row['architecture_oracle']}+"
            f"{allocation_row['decoder_oracle']}"
        ),
    }


def expect_failure(name: str, action: Callable[[], object], text: str) -> None:
    try:
        action()
    except ValueError as error:
        if text not in str(error):
            raise AssertionError(f"{name}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{name}: validator accepted the negative fixture")


def read_fixture_evidence(row: dict[str, str]) -> object:
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "fixture-test.tsv"
        with path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(
                output,
                fieldnames=VALIDATOR.EVIDENCE_COLUMNS,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerow(row)
        return VALIDATOR.read_evidence(Path(temporary), {"fixture-test"})


def validate(
    rows: list[dict[str, str]],
    inventory_rows: list[dict[str, str]],
    allocation_rows: list[dict[str, str]] | None = None,
    evidence_rows: list[tuple[Path, dict[str, str]]] | None = None,
    decoded_words: dict[int, str | None] | None = None,
    contract_rows: list[dict[str, str]] | None = None,
    semantic_rows: list[dict[str, str]] | None = None,
) -> list[dict[str, str]] | None:
    if allocation_rows is None:
        by_id = {row["stable_id"]: row for row in rows}
        allocation_rows = [
            allocation(
                row["pdf_label"],
                by_id[row["stable_id"]]["encoding"],
            )
            for row in inventory_rows
        ]
    inventory_by_id = {row["stable_id"]: row for row in inventory_rows}
    allocation_rows = [dict(row) for row in allocation_rows]
    for row in allocation_rows:
        if row["stable_id"]:
            continue
        candidates = [
            entry["stable_id"]
            for entry in inventory_rows
            if entry["section"] == row["section"]
            and entry["pdf_label"] == row["architectural_form"]
        ]
        if not candidates and len(inventory_rows) == 1:
            candidates = [inventory_rows[0]["stable_id"]]
        if len(candidates) != 1:
            raise AssertionError("fixture cannot infer allocation stable ID")
        row["stable_id"] = candidates[0]
    allocations = VALIDATOR.validate_allocations(
        allocation_rows, Path("allocations.tsv"), inventory_by_id
    )
    contracts = (
        VALIDATOR.validate_variant_contracts(
            contract_rows, Path("contracts.tsv"), inventory_by_id, allocations
        )
        if contract_rows is not None
        else {}
    )
    if decoded_words is None:
        decoded_words = {}
        for row in rows:
            word = int(row["encoding"], 16)
            candidates = allocation_rows
            matching = [
                candidate
                for candidate in candidates
                if int(candidate["encoding_value"], 16)
                == word & int(candidate["encoding_mask"], 16)
            ]
            if matching:
                opcode = matching[0]["decoder_opcode"]
            else:
                opcode = allocation_rows[0]["decoder_opcode"]
            decoded_words[word] = (
                None if opcode.startswith("INVALID_") else opcode
            )
    bindings = VALIDATOR.validate_bindings(
        rows,
        Path("bindings.tsv"),
        inventory_by_id,
        allocations,
        {"fixture-test"},
        decoded_words,
        contracts,
    )
    semantic_contracts = None
    if semantic_rows is not None:
        semantic_contracts, _complete_ids = VALIDATOR.validate_semantic_contracts(
            semantic_rows,
            Path("semantic-contracts.tsv"),
            inventory_by_id,
            bindings,
        )
    if evidence_rows is not None:
        return VALIDATOR.bind_exact(
            evidence_rows, bindings, semantic_contracts
        )
    return None


def main() -> int:
    if "pc-alignment" not in VALIDATOR.EXPECTED_EVENTS:
        raise AssertionError("pc-alignment event spelling is not accepted")

    shifted = [inventory("BASE-0001", "ADD (shifted register)")]
    expect_failure(
        "shifted-versus-extended",
        lambda: validate(
            [binding("BASE-0001", "ADD (extended register)", "0b220027")],
            shifted,
        ),
        "architectural form does not match inventory",
    )

    literal = [inventory("BASE-0002", "LDR (literal)")]
    expect_failure(
        "immediate-versus-literal",
        lambda: validate(
            [binding("BASE-0002", "LDR (immediate)", "f9400000")], literal
        ),
        "architectural form does not match inventory",
    )

    encoding_only = [inventory("BASE-0007", "ADD (shifted register)")]
    expect_failure(
        "encoding-only-form-mismatch",
        lambda: validate(
            [binding("BASE-0007", "ADD (shifted register)", "0b220027")],
            encoding_only,
            [
                allocation(
                    "ADD (shifted register)",
                    "0b000000",
                    "ff208000",
                    "ADDWrs",
                )
            ],
        ),
        "independent architectural allocation/dependent-field partition",
    )

    consistently_changed = binding(
        "BASE-0009", "ADD (shifted register)", "0b220027"
    )
    expect_failure(
        "self-consistent-selected-word-mutation",
        lambda: validate(
            [consistently_changed],
            [inventory("BASE-0009", "ADD (shifted register)")],
            [
                allocation(
                    "ADD (shifted register)",
                    "0b000000",
                    "ff208000",
                    "ADDWrs",
                )
            ],
            [evidence(consistently_changed)],
        ),
        "independent architectural allocation/dependent-field partition",
    )

    feature_inventory = [inventory("BASE-0008", "LDADD", "lse")]
    expect_failure(
        "unrelated-native-feature",
        lambda: validate(
            [binding("BASE-0008", "LDADD", "f8200020", native="aes")],
            feature_inventory,
            [
                allocation(
                    "LDADD",
                    "f8200020",
                    decoder_opcode="LDADDW",
                    architecture_native="lse",
                )
            ],
        ),
        "exact native features",
    )

    unreachable_inventory = {
        "BASE-0003": inventory("BASE-0003", "B"),
        "BASE-0004": inventory("BASE-0004", "RET"),
    }
    reachable = binding("BASE-0003", "B", "14000000")
    unreachable = binding("BASE-0004", "RET", "d65f03c0")
    exact_bindings = {
        (
            row["stable_id"],
            row["ctest"],
            row["case"],
            row["encoding"],
        ): {**row, "_variant_id": row["stable_id"], "_contract": None}
        for row in (reachable, unreachable)
    }
    expect_failure(
        "unreachable-program-word",
        lambda: VALIDATOR.bind_exact(
            [evidence(reachable)],
            exact_bindings,
        ),
        "BASE-0004",
    )

    duplicate_inventory = [
        inventory("BASE-0005", "ASR (register)"),
        inventory("BASE-0006", "ASRV"),
    ]
    expect_failure(
        "undeclared-encoding-alias",
        lambda: validate(
            [
                binding("BASE-0005", "ASR (register)", "1ac52860"),
                binding("BASE-0006", "ASRV", "1ac52860"),
            ],
            duplicate_inventory,
        ),
        "undeclared duplicate",
    )

    variant_inventory = [
        inventory("BASE-0010", "ADD (extended register)")
    ]
    w_allocation = allocation(
        "ADD (extended register)", "0b220027", decoder_opcode="ADDWrx"
    )
    x_allocation = allocation(
        "ADD (extended register)", "8b220027", decoder_opcode="ADDXrx"
    )
    w_binding = binding(
        "BASE-0010", "ADD (extended register)", "0b220027", case="w-valid"
    )
    x_binding = binding(
        "BASE-0010", "ADD (extended register)", "8b220027", case="x-valid"
    )
    w_contract = contract(
        "BASE-0010",
        "ADD (extended register)",
        w_allocation,
        "add-w-valid",
        state_digest="1111111111111111",
    )
    x_contract = contract(
        "BASE-0010",
        "ADD (extended register)",
        x_allocation,
        "add-x-valid",
        state_digest="2222222222222222",
    )
    variants = validate(
        [w_binding, x_binding],
        variant_inventory,
        [w_allocation, x_allocation],
        [
            evidence(w_binding, state_digest="1111111111111111"),
            evidence(x_binding, state_digest="2222222222222222"),
        ],
        {int(w_binding["encoding"], 16): "ADDWrx",
         int(x_binding["encoding"], 16): "ADDXrx"},
        [w_contract, x_contract],
    )
    if variants is None or len(variants) != 2:
        raise AssertionError("multiple independently contracted variants failed")

    expect_failure(
        "variant-removal",
        lambda: validate(
            [w_binding],
            variant_inventory,
            [w_allocation, x_allocation],
            decoded_words={int(w_binding["encoding"], 16): "ADDWrx"},
            contract_rows=[w_contract, x_contract],
        ),
        "required variant contracts lack execution bindings",
    )

    duplicate_w = binding(
        "BASE-0010",
        "ADD (extended register)",
        "0b220027",
        case="w-valid-duplicate",
    )
    expect_failure(
        "variant-duplication",
        lambda: validate(
            [w_binding, duplicate_w],
            variant_inventory,
            [w_allocation],
            decoded_words={int(w_binding["encoding"], 16): "ADDWrx"},
            contract_rows=[w_contract],
        ),
        "duplicate independently identified variant",
    )

    expect_failure(
        "cross-variant-result-swap",
        lambda: validate(
            [w_binding, x_binding],
            variant_inventory,
            [w_allocation, x_allocation],
            [
                evidence(w_binding, state_digest="2222222222222222"),
                evidence(x_binding, state_digest="1111111111111111"),
            ],
            {int(w_binding["encoding"], 16): "ADDWrx",
             int(x_binding["encoding"], 16): "ADDXrx"},
            [w_contract, x_contract],
        ),
        "expected_state_digest does not match its independent contract",
    )

    semantic_inventory = [inventory("BASE-0012", "NOP")]
    semantic_binding = binding("BASE-0012", "NOP", "d503201f")
    semantic_allocation = allocation(
        "NOP", "d503201f", decoder_opcode="NOP"
    )
    _path, first_semantic_evidence = evidence(
        semantic_binding,
        initial_state_digest="1111111111111111",
        state_digest="2222222222222222",
    )
    _path, second_semantic_evidence = evidence(
        semantic_binding,
        initial_state_digest="3333333333333333",
        state_digest="4444444444444444",
    )
    first_semantic = semantic_contract(
        semantic_binding,
        semantic_allocation,
        first_semantic_evidence,
        "semantic-first",
    )
    second_semantic = semantic_contract(
        semantic_binding,
        semantic_allocation,
        second_semantic_evidence,
        "semantic-second",
    )
    semantic_variants = validate(
        [semantic_binding],
        semantic_inventory,
        [semantic_allocation],
        [
            (Path("fixture-test.tsv"), first_semantic_evidence),
            (Path("fixture-test.tsv"), second_semantic_evidence),
        ],
        {int(semantic_binding["encoding"], 16): "NOP"},
        semantic_rows=[first_semantic, second_semantic],
    )
    if semantic_variants is None or len(semantic_variants) != 2:
        raise AssertionError("multiple harness-declared semantics failed")

    expect_failure(
        "semantic-removal",
        lambda: validate(
            [semantic_binding],
            semantic_inventory,
            [semantic_allocation],
            [(Path("fixture-test.tsv"), first_semantic_evidence)],
            {int(semantic_binding["encoding"], 16): "NOP"},
            semantic_rows=[first_semantic, second_semantic],
        ),
        "checked-in semantic contracts lack exact execution evidence",
    )

    duplicate_semantic = dict(first_semantic)
    duplicate_semantic["semantic_id"] = "semantic-duplicate"
    expect_failure(
        "semantic-duplication",
        lambda: validate(
            [semantic_binding],
            semantic_inventory,
            [semantic_allocation],
            decoded_words={int(semantic_binding["encoding"], 16): "NOP"},
            semantic_rows=[first_semantic, duplicate_semantic],
        ),
        "duplicate semantic expectation",
    )

    swapped_first = dict(first_semantic_evidence)
    swapped_first["expected_state_digest"] = second_semantic_evidence[
        "expected_state_digest"
    ]
    expect_failure(
        "semantic-cross-variant-swap",
        lambda: validate(
            [semantic_binding],
            semantic_inventory,
            [semantic_allocation],
            [(Path("fixture-test.tsv"), swapped_first)],
            {int(semantic_binding["encoding"], 16): "NOP"},
            semantic_rows=[first_semantic],
        ),
        "execution has no independent semantic contract",
    )

    custom_oracle_evidence = dict(first_semantic_evidence)
    custom_oracle_evidence["source"] = "tests/unit/arm64/fixture.cpp"
    custom_oracle_evidence["line"] = "10"
    custom_oracle_evidence["expected_state_oracle"] = (
        "custom-validator@tests/unit/arm64/fixture.cpp:10"
    )
    custom_oracle_contract = semantic_contract(
        semantic_binding,
        semantic_allocation,
        custom_oracle_evidence,
        "semantic-custom-oracle",
    )
    custom_oracle_drift = dict(custom_oracle_evidence)
    custom_oracle_drift["expected_state_oracle"] = (
        "custom-validator@tests/unit/arm64/fixture.cpp:11"
    )
    expect_failure(
        "custom-oracle-locator-drift",
        lambda: validate(
            [semantic_binding],
            semantic_inventory,
            [semantic_allocation],
            [(Path("fixture-test.tsv"), custom_oracle_drift)],
            {int(semantic_binding["encoding"], 16): "NOP"},
            semantic_rows=[custom_oracle_contract],
        ),
        "execution has no independent semantic contract",
    )

    read_fixture_evidence(custom_oracle_evidence)
    ambiguous_oracle = dict(custom_oracle_evidence)
    ambiguous_oracle["expected_state_oracle"] = (
        "custom-validator@fixture.cpp:10"
    )
    expect_failure(
        "custom-oracle-basename-ambiguity",
        lambda: read_fixture_evidence(ambiguous_oracle),
        "invalid state oracle",
    )
    stale_oracle = dict(custom_oracle_evidence)
    stale_oracle["expected_state_oracle"] = (
        "custom-validator@tests/unit/arm64/fixture.cpp:11"
    )
    expect_failure(
        "custom-oracle-stale-source-line",
        lambda: read_fixture_evidence(stale_oracle),
        "stale source line",
    )

    dependent_word = binding(
        "BASE-0010",
        "ADD (extended register)",
        "0b221427",
        case="reserved-imm3-5",
    )
    expect_failure(
        "dependent-reserved-field",
        lambda: validate(
            [dependent_word],
            variant_inventory,
            [
                allocation(
                    "ADD (extended register)",
                    "0b200000",
                    "ffe00000",
                    "ADDWrx",
                )
            ],
            decoded_words={int(dependent_word["encoding"], 16): None},
        ),
        "dependent-field partition",
    )

    expect_failure(
        "whole-pipeline-form-mutation",
        lambda: validate(
            [
                binding(
                    "BASE-0010", "ADD (extended register)", "0b220027"
                )
            ],
            [inventory("BASE-0010", "ADD (extended register)")],
            [allocation("ADD (shifted register)", "0b020027")],
        ),
        "allocation form differs from inventory",
    )

    feature_allocation = allocation(
        "LDADD",
        "f8200020",
        decoder_opcode="LDADDW",
        architecture_native="lse",
    )
    feature_binding = binding(
        "BASE-0010", "LDADD", "f8200020", native="aes"
    )
    feature_contract = contract(
        "BASE-0010",
        "LDADD",
        feature_allocation,
        "ldadd-w",
        native="lse",
    )
    expect_failure(
        "whole-pipeline-feature-mutation",
        lambda: validate(
            [feature_binding],
            [inventory("BASE-0010", "LDADD", "aes")],
            [feature_allocation],
            [evidence(feature_binding)],
            {int(feature_binding["encoding"], 16): "LDADDW"},
            [feature_contract],
        ),
        "do not match independent Arm XML feature alternatives",
    )

    skipped_feature = binding(
        "BASE-0011", "LDADD", "f8200020", native="lse"
    )
    _path, skipped_evidence = evidence(
        skipped_feature, jit_result="skipped-host-feature"
    )
    skipped_evidence["native_observed_features"] = "lse=0"
    expect_failure(
        "missing-host-feature-not-recorded",
        lambda: read_fixture_evidence(skipped_evidence),
        "missing features do not exactly match",
    )
    skipped_evidence["native_missing_features"] = "lse"
    read_fixture_evidence(skipped_evidence)

    repository = Path(__file__).resolve().parents[3]
    generator = Path(__file__).with_name(
        "generate_a64_instruction_allocations.py"
    )
    checked_in = (
        repository / "tests/unit/arm64/a64_instruction_allocations.tsv"
    )
    architecture_archive = os.environ.get("UML_A64_ARCHITECTURE_ARCHIVE")
    if architecture_archive:
        with tempfile.TemporaryDirectory() as temporary:
            regenerated = Path(temporary) / "allocations.tsv"
            subprocess.run(
                [
                    sys.executable,
                    str(generator),
                    "--architecture-archive",
                    architecture_archive,
                    "--output",
                    str(regenerated),
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if regenerated.read_bytes() != checked_in.read_bytes():
                raise AssertionError("checked-in allocation regeneration drifted")

            bindings_path = (
                repository
                / "tests/unit/arm64/a64_instruction_evidence_bindings.tsv"
            )
            with bindings_path.open(encoding="utf-8", newline="") as source:
                binding_rows = list(csv.DictReader(source, delimiter="\t"))
            swapped_bindings = [dict(row) for row in binding_rows]
            swapped_bindings[0]["stable_id"], swapped_bindings[1]["stable_id"] = (
                swapped_bindings[1]["stable_id"],
                swapped_bindings[0]["stable_id"],
            )
            swapped_path = Path(temporary) / "swapped-bindings.tsv"
            with swapped_path.open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(
                    output,
                    fieldnames=VALIDATOR.BINDING_COLUMNS,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(swapped_bindings)
            swapped = subprocess.run(
                [
                    sys.executable,
                    str(generator),
                    "--architecture-archive",
                    architecture_archive,
                    "--bindings",
                    str(swapped_path),
                    "--output",
                    str(Path(temporary) / "swapped.tsv"),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if (
                swapped.returncode == 0
                or "official Arm XML index names" not in swapped.stderr
            ):
                raise AssertionError(
                    "official index accepted a cross-ID form swap"
                )

    # Keep the unreachable fixture's inventory live so accidental fixture
    # simplification cannot erase the distinction between stored and executed.
    if len(unreachable_inventory) != 2:
        raise AssertionError("unreachable fixture is incomplete")

    with tempfile.TemporaryDirectory(
        prefix="a64-evidence-validator-publication-"
    ) as temporary_name:
        temporary = Path(temporary_name).resolve()
        evidence_directory = temporary / "instruction-evidence"
        evidence_directory.mkdir()
        malformed = evidence_directory / "fixture-test.tsv"
        malformed.write_text("malformed\n", encoding="utf-8")
        report = evidence_directory / "stable-id-bindings.tsv"
        report.write_bytes(b"old-report")
        registered = temporary / "registered-tests.txt"
        registered.write_text("fixture-test\n", encoding="utf-8")
        command = [
            sys.executable,
            "-E",
            "-s",
            "-B",
            str(SOURCE),
            "--evidence-directory",
            str(evidence_directory),
            "--registered-tests-file",
            str(registered),
        ]
        failed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if failed.returncode == 0 or report.read_bytes() != b"old-report":
            raise AssertionError(
                "failed evidence validation replaced the prior report: "
                + failed.stdout
            )
        with producer_evidence_lock(evidence_directory):
            waiting = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                import time

                time.sleep(0.05)
                if waiting.poll() is not None:
                    raise AssertionError(
                        "evidence validator bypassed an active producer gate"
                    )
            finally:
                if waiting.poll() is None:
                    waiting.terminate()
                    waiting.wait(timeout=5.0)
        if report.read_bytes() != b"old-report":
            raise AssertionError("contended validator partially wrote its report")
    print("A64 exact-evidence variant/oracle fixtures passed: 22")
    return 0


if __name__ == "__main__":
    sys.exit(main())
