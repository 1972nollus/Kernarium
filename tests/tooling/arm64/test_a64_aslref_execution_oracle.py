#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative and parity fixtures for A64 official-ASL execution evidence."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import csv
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from a64_aggregate_manifest_identity import read_manifest_identity  # noqa: E402
from aggregate_a64_raw_instruction_evidence import (  # noqa: E402
    OUTPUT_COLUMNS as AGGREGATE_COLUMNS,
)
from generate_a64_aslref_aggregate_execution_plan import (  # noqa: E402
    DECODE_COLUMNS,
    build_plan,
    read_tsv,
    validate_plan,
)
from generate_a64_instruction_required_variants import (  # noqa: E402
    COLUMNS as REQUIRED_COLUMNS,
)
from run_a64_aslref_execution_oracle import (  # noqa: E402
    COLUMNS as EXECUTION_COLUMNS,
    Partition,
    exact_function,
    semantic_witness,
    validate_evidence_rows,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except ValueError:
        return
    raise AssertionError(message)


def read_execution(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        require(reader.fieldnames == EXECUTION_COLUMNS, "execution columns drifted")
        return list(reader)


def execution_negative_fixtures(repository: Path) -> None:
    checked = repository / "tests/unit/arm64/a64_aslref_execution_oracle.tsv"
    if not checked.is_file():
        raise ValueError(f"checked representative evidence is missing: {checked}")
    rows = read_execution(checked)
    validate_evidence_rows(rows)
    require(len(rows) >= 20, "representative execution cross-section shrank")

    expect_failure(
        lambda: validate_evidence_rows(rows[:-1]),
        "removed execution partition was accepted",
    )
    expect_failure(
        lambda: validate_evidence_rows([*rows, copy.deepcopy(rows[0])]),
        "duplicated execution partition was accepted",
    )
    left = 0
    right = next(
        index for index, row in enumerate(rows)
        if row["allocation_status_id"] != rows[left]["allocation_status_id"]
    )
    swapped = copy.deepcopy(rows)
    first, second = swapped[left], swapped[right]
    first["allocation_status_id"], second["allocation_status_id"] = (
        second["allocation_status_id"],
        first["allocation_status_id"],
    )
    expect_failure(
        lambda: validate_evidence_rows(swapped),
        "swapped allocation identities were accepted",
    )
    perturbed = copy.deepcopy(rows)
    perturbed[0]["witness_encoding"] = (
        f"{int(perturbed[0]['witness_encoding'], 16) ^ 1:08x}"
    )
    expect_failure(
        lambda: validate_evidence_rows(perturbed),
        "perturbed semantic input was accepted without identity regeneration",
    )


def pure_mapping_fixtures() -> None:
    with tempfile.TemporaryDirectory(prefix="a64-aslref-execution-fixture-") as name:
        directory = Path(name)
        xml = directory / "alias.xml"
        xml.write_text(
            "<instructionsection><classes><iclass id='iclass_one'>"
            "<regdiagram><box name='CRm' hibit='11' width='4'/></regdiagram>"
            "<encoding name='ALIAS'/></iclass></classes>"
            "<explanations><explanation enclist='ALIAS'>"
            "<definition encodedin='CRm[2:1]'><table class='valuetable'><tbody>"
            "<row><entry class='bitfield'>00</entry>"
            "<entry class='symbol'>RESERVED</entry></row>"
            "<row><entry class='bitfield'>01</entry>"
            "<entry class='symbol'>VALID</entry></row>"
            "</tbody></table></definition></explanation></explanations>"
            "</instructionsection>",
            encoding="utf-8",
        )
        row = {
            "stable_id": "FIXTURE-1",
            "architecture_file": "alias.xml",
            "architecture_encoding": "ALIAS",
            "architecture_mask": "fffff9ff",
            "architecture_value": "d503417f",
            "architecture_constraints": "none",
            "decode_mask": "fff8f01f",
            "decode_value": "d500401f",
            "decode_constraints": "op1@16:3!=000",
        }
        word, choices = semantic_witness(row, directory)
        require(word == 0xD503437F, "reserved alias operand was not refined")
        require(choices == ("CRm[2:1]=01:VALID",), "alias choice evidence drifted")
        require(
            exact_function(xml, "ALIAS", ("instr_alias_iclass_one", "instr_other"))
            == "instr_alias_iclass_one",
            "encoding-to-iclass mapping drifted",
        )

    base_row = {"static_feature_expressions": "FEAT_FP"}
    partition = Partition("pattern-state-v1", "pattern")
    original = partition.digest(base_row, 0x1E602800)
    require(
        original != partition.digest(base_row, 0x1E602801),
        "instruction-input perturbation did not change partition digest",
    )
    changed_features = {"static_feature_expressions": "FEAT_FP16"}
    require(
        original != partition.digest(changed_features, 0x1E602800),
        "feature-input perturbation did not change partition digest",
    )


def aggregate_plan_fixtures(repository: Path) -> None:
    aggregates = read_tsv(
        repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
        AGGREGATE_COLUMNS,
    )
    required = read_tsv(
        repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
        REQUIRED_COLUMNS,
    )
    decodes = read_tsv(
        repository / "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
        DECODE_COLUMNS,
    )
    first = build_plan(aggregates, required, decodes)
    second = build_plan(aggregates, required, decodes)
    require(first == second, "aggregate execution plan regeneration drifted")
    identity = read_manifest_identity(
        repository
        / "tests/unit/arm64/a64_instruction_aggregate_manifest.identity.json"
    )
    require(
        len(first) == identity["row_count"],
        "aggregate execution plan lost frozen identities",
    )
    validate_plan(aggregates, required, decodes, first)

    expect_failure(
        lambda: validate_plan(aggregates, required, decodes, first[:-1]),
        "removed aggregate obligation was accepted",
    )
    expect_failure(
        lambda: validate_plan(
            aggregates, required, decodes, [*first, copy.deepcopy(first[0])]
        ),
        "duplicated aggregate obligation was accepted",
    )
    swapped = copy.deepcopy(first)
    allocated = [
        index for index, row in enumerate(swapped)
        if row["allocation_status_id"] != "none"
    ][:2]
    left, right = allocated
    swapped[left]["allocation_status_id"], swapped[right]["allocation_status_id"] = (
        swapped[right]["allocation_status_id"],
        swapped[left]["allocation_status_id"],
    )
    expect_failure(
        lambda: validate_plan(aggregates, required, decodes, swapped),
        "swapped aggregate allocation mapping was accepted",
    )
    perturbed = copy.deepcopy(first)
    perturbed[0]["semantic_input_id"] = "aggregate-input-00000000000000000000"
    expect_failure(
        lambda: validate_plan(aggregates, required, decodes, perturbed),
        "perturbed aggregate input identity was accepted",
    )


def main() -> int:
    repository = Path(__file__).resolve().parents[3]
    pure_mapping_fixtures()
    aggregate_plan_fixtures(repository)
    execution_negative_fixtures(repository)
    print("A64 ASLRef execution oracle negative/parity fixtures passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, AssertionError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
