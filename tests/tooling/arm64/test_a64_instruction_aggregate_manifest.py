#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative fixtures for strict A64 aggregate-manifest validation."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import csv
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from threading import Event, Thread
from typing import Callable, Sequence

from aggregate_a64_raw_instruction_evidence import (
    OUTPUT_COLUMNS,
    RAW_COLUMNS,
    aggregate_id,
    semantic_features_for_word,
)
from freeze_a64_aggregate_manifest import (
    full_publication_rows,
    raw_producer_authority,
    require_exact_raw_derivation,
    require_regular_raw_file,
    validate_full_publication_paths,
    validate_producer_authority,
)
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from validate_a64_instruction_aggregate_manifest import (
    canonical_combined_rows,
    compare_manifest,
    compare_profiles,
    parse_counter,
    read_bound_manifest,
    validate_rows,
)
from a64_raw_evidence_files import (
    atomic_publish_bytes,
    filesystem_is_case_sensitive,
    freeze_read_inputs,
    publication_target_locks,
    recheck_read_inputs,
    require_no_symlink_components,
    validate_publication_topology,
)


def read(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise AssertionError(f"{path}: fixture columns drifted")
        return list(reader)


def publish_fixture(
    outputs: tuple[tuple[Path, bytes], ...],
    before_replace: Callable[[], None] | None = None,
) -> None:
    def validate_staged(staged: Sequence[tuple[Path, Path]]) -> None:
        for (_target, payload), (_published, temporary) in zip(outputs, staged):
            if temporary.read_bytes() != payload:
                raise ValueError("staged fixture publication differs")

    def validate_published() -> None:
        for target, payload in outputs:
            if target.read_bytes() != payload:
                raise ValueError("published fixture publication differs")

    atomic_publish_bytes(
        outputs,
        validate_staged=validate_staged,
        before_replace=before_replace,
        validate_published=validate_published,
    )


def expect_failure(name: str, action: Callable[[], object], text: str) -> None:
    try:
        action()
    except ValueError as error:
        if text not in str(error):
            raise AssertionError(f"{name}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{name}: validator accepted the negative fixture")


def expect_read_failure(
    name: str, action: Callable[[], object], text: str
) -> None:
    try:
        action()
    except (OSError, ValueError) as error:
        if text not in str(error):
            raise AssertionError(f"{name}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{name}: validator accepted the negative fixture")


def base_row(
    required: dict[str, str], decode: dict[str, str], page: str = "4096"
) -> dict[str, str]:
    row = {
        "aggregate_id": "",
        "stable_id": required["stable_id"],
        "section": required["section"],
        "architectural_form": required["architectural_form"],
        "architecture_file": required["architecture_file"],
        "architecture_encoding": required["architecture_encoding"],
        "architecture_mask": required["architecture_mask"],
        "architecture_value": required["architecture_value"],
        "architecture_constraints": required["architecture_constraints"],
        "operation_digest": required["operation_digest"],
        "alias_digest": required["alias_digest"],
        "decode_partition_id": decode["partition_id"],
        "decode_class": required["decode_class"],
        "allocation_kind": required["allocation_kind"],
        "producer": "fixture-test",
        "source_site": hashlib.sha256(
            b"fixture-test:tests/unit/arm64/fixture.cpp:10"
        ).hexdigest()[:20],
        "source_file": "tests/unit/arm64/fixture.cpp",
        "source_line": "10",
        "validity_partition": "allocated-valid",
        "expected_native_feature_alternatives": required[
            "static_native_feature_alternatives"
        ],
        "native_requirement_status": "exact",
        "semantic_native_features": required["semantic_native_features"],
        "native_fallback_features": required[
            "static_native_fallback_features"
        ],
        "native_required_features": required[
            "static_native_feature_alternatives"
        ].split("|", 1)[0],
        "native_requirement_scope": "instruction",
        "guest_page_size": page,
        "expected_event": "system-call",
        "expected_retirement": "exact:2",
        "expected_state_oracle": "custom-validator@fixture.cpp:10",
        "expected_fault_partition": "none",
        "declared_count": "1",
        "unique_word_count": "1",
        "interpreter_executed_count": "1",
        "gadgets_executed_count": "1",
        "jit_executed_count": "1",
        "interpreter_results": "executed-retired:1",
        "gadgets_results": "executed-retired:1",
        "jit_results": "executed-retired:1",
        "native_observed_results": "baseline=1:1",
        "native_missing_results": "none:1",
        "decoder_results": "FIXTURE:1",
        "declared_digest": "1" * 64,
        "observed_digest": "2" * 64,
        "architecture_oracle": required["architecture_oracle"],
        "decoder_oracle": "llvm-mc-22.1.8",
    }
    row["aggregate_id"] = aggregate_id(row)
    return row


def main() -> int:
    repository = Path(__file__).resolve().parents[3]
    required_rows = read(
        repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
        REQUIRED_COLUMNS,
    )
    decode_rows = read(
        repository / "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
        DECODE_COLUMNS,
    )
    required = required_rows[0]
    decode = next(
        row
        for row in decode_rows
        if row["decode_class"] == required["decode_class"]
        and row["partition_name"] == required["decode_encoding"]
        and row["architecture_file"] == required["decode_architecture_file"]
        and row["architecture_mask"] == required["decode_mask"]
        and row["architecture_value"] == required["decode_value"]
        and row["architecture_constraints"] == required["decode_constraints"]
    )
    row = base_row(required, decode)
    validate_rows([row], "4096", [required], decode_rows)
    if parse_counter("mte2=0+pops=0:32", "fixture") != {
        "mte2=0+pops=0": 32
    }:
        raise AssertionError("feature-conjunction counter parsed incorrectly")

    mrs_required = next(
        item for item in required_rows if item["stable_id"] == "BASE-0292"
    )
    mrs_decode = next(
        item
        for item in decode_rows
        if item["decode_class"] == mrs_required["decode_class"]
        and item["partition_name"] == mrs_required["decode_encoding"]
        and item["architecture_file"]
        == mrs_required["decode_architecture_file"]
        and item["architecture_mask"] == mrs_required["decode_mask"]
        and item["architecture_value"] == mrs_required["decode_value"]
        and item["architecture_constraints"]
        == mrs_required["decode_constraints"]
    )
    if semantic_features_for_word(mrs_required, 0xD53B4400) != {"floating_point"}:
        raise AssertionError("FPCR selector did not select floating-point gate")
    if semantic_features_for_word(mrs_required, 0xD53B4440) != {"fp8"}:
        raise AssertionError("FPMR selector did not select FP8 gate")
    if semantic_features_for_word(mrs_required, 0xD53B4460) != set():
        raise AssertionError("unlisted system register selected a semantic gate")
    mrs = base_row(mrs_required, mrs_decode)
    mrs.update(
        {
            "native_requirement_status": "semantic-feature-gate",
            "semantic_native_features": "floating_point",
            "native_required_features": "floating_point",
        }
    )
    mrs["aggregate_id"] = aggregate_id(mrs)
    validate_rows([mrs], "4096", [mrs_required], decode_rows)

    msr_required_rows = [
        item for item in required_rows if item["stable_id"] == "BASE-0293"
    ]
    if len(msr_required_rows) != 3:
        raise AssertionError("MSR immediate fixture no longer has three predicates")
    msr_decodes = [
        next(
            item
            for item in decode_rows
            if item["decode_class"] == required_item["decode_class"]
            and item["partition_name"] == required_item["decode_encoding"]
            and item["architecture_file"]
            == required_item["decode_architecture_file"]
            and item["architecture_mask"] == required_item["decode_mask"]
            and item["architecture_value"] == required_item["decode_value"]
            and item["architecture_constraints"]
            == required_item["decode_constraints"]
        )
        for required_item in msr_required_rows
    ]
    if len({item["partition_id"] for item in msr_decodes}) != 3:
        raise AssertionError("MSR immediate decode predicates collapsed")
    collapsed = base_row(msr_required_rows[0], msr_decodes[0])
    collapsed["decode_partition_id"] = msr_decodes[1]["partition_id"]
    collapsed["aggregate_id"] = aggregate_id(collapsed)
    expect_failure(
        "same-name-decode-predicate-collapse",
        lambda: validate_rows(
            [collapsed], "4096", [msr_required_rows[0]], decode_rows
        ),
        "unknown or swapped required allocation",
    )

    expect_failure(
        "duplicate-aggregate",
        lambda: validate_rows([row, row], "4096", [required], decode_rows),
        "duplicate aggregate ID",
    )
    drift = copy.deepcopy(row)
    drift["native_requirement_status"] = "drift"
    drift["aggregate_id"] = aggregate_id(drift)
    expect_failure(
        "unrelated-feature-skip",
        lambda: validate_rows([drift], "4096", [required], decode_rows),
        "unrelated or missing native feature gate",
    )
    sysreg_drift = copy.deepcopy(mrs)
    sysreg_drift["semantic_native_features"] = "dotprod"
    sysreg_drift["aggregate_id"] = aggregate_id(sysreg_drift)
    expect_failure(
        "unrelated-system-register-feature",
        lambda: validate_rows(
            [sysreg_drift], "4096", [mrs_required], decode_rows
        ),
        "semantic feature dependency drift",
    )
    swapped = copy.deepcopy(row)
    swapped["stable_id"] = next(
        item["stable_id"]
        for item in required_rows
        if item["stable_id"] != required["stable_id"]
    )
    swapped["aggregate_id"] = aggregate_id(swapped)
    expect_failure(
        "swapped-stable-id",
        lambda: validate_rows([swapped], "4096", [required], decode_rows),
        "unknown or swapped required allocation",
    )
    count = copy.deepcopy(row)
    count["declared_count"] = "2"
    expect_failure(
        "altered-count",
        lambda: validate_rows([count], "4096", [required], decode_rows),
        "result count drift",
    )
    digest = copy.deepcopy(row)
    digest["declared_digest"] = "0" * 64
    expect_failure(
        "altered-digest",
        lambda: compare_manifest([digest], [row]),
        "differs from checked manifest",
    )
    locator = copy.deepcopy(row)
    locator["expected_state_oracle"] = "custom-validator@fixture.cpp:11"
    locator["aggregate_id"] = aggregate_id(locator)
    expect_failure(
        "custom-oracle-locator-drift",
        lambda: compare_manifest([locator], [row]),
        "differs from checked manifest",
    )
    expect_failure(
        "missing-nonrepresentative-aggregate",
        lambda: compare_manifest([], [row]),
        "differs from checked manifest",
    )
    second = copy.deepcopy(row)
    second["source_line"] = "11"
    second["aggregate_id"] = aggregate_id(second)
    profile = copy.deepcopy(row)
    profile["guest_page_size"] = "16384"
    profile["aggregate_id"] = aggregate_id(profile)
    expect_failure(
        "profile-aggregate-removal",
        lambda: compare_profiles([row, second], [profile]),
        "aggregate semantics differ",
    )
    reserved_decode = next(
        item
        for item in decode_rows
        if item["partition_kind"] == "unallocated"
        and "!=" in item["architecture_constraints"]
    )
    reserved = base_row(required, decode)
    reserved.update(
        {
            "stable_id": "none",
            "section": "none",
            "architectural_form": "none",
            "architecture_file": reserved_decode["architecture_file"],
            "architecture_encoding": reserved_decode["partition_name"],
            "architecture_mask": reserved_decode["architecture_mask"],
            "architecture_value": reserved_decode["architecture_value"],
            "architecture_constraints": reserved_decode[
                "architecture_constraints"
            ],
            "operation_digest": "none",
            "alias_digest": "none",
            "decode_partition_id": reserved_decode["partition_id"],
            "decode_class": reserved_decode["decode_class"],
            "allocation_kind": "reserved",
            "validity_partition": "unallocated",
            "native_requirement_status": "not-applicable",
            "semantic_native_features": "baseline",
            "native_fallback_features": "baseline",
        }
    )
    reserved["aggregate_id"] = aggregate_id(reserved)
    validate_rows([reserved], "4096", [], decode_rows)
    pair_drift = copy.deepcopy(reserved)
    pair_drift["architecture_constraints"] = "none"
    pair_drift["aggregate_id"] = aggregate_id(pair_drift)
    expect_failure(
        "pair-dependent-reserved-encoding",
        lambda: validate_rows([pair_drift], "4096", [], decode_rows),
        "reserved decode predicate drift",
    )

    checked = read(
        repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
        OUTPUT_COLUMNS,
    )
    full_4k = sorted(
        (item for item in checked if item["guest_page_size"] == "4096"),
        key=lambda item: (item["stable_id"], item["aggregate_id"]),
    )
    full_16k = sorted(
        (item for item in checked if item["guest_page_size"] == "16384"),
        key=lambda item: (item["stable_id"], item["aggregate_id"]),
    )
    expected_producers = {
        "4096": {item["producer"] for item in full_4k},
        "16384": {item["producer"] for item in full_16k},
    }
    published = full_publication_rows(
        full_4k, full_16k, expected_producers
    )
    if len(published) != 32169 or published != sorted(
        published, key=lambda item: item["aggregate_id"]
    ):
        raise AssertionError("full publication fixture is not canonical")
    if canonical_combined_rows(full_4k, full_16k) != published:
        raise AssertionError("validator full-profile order differs from publication")
    reordered = list(published)
    reordered[0], reordered[1] = reordered[1], reordered[0]
    expect_failure(
        "checked-manifest-order-mutation",
        lambda: compare_manifest(reordered, published),
        "differs from checked manifest",
    )
    expect_failure(
        "full-publication-missing-row",
        lambda: full_publication_rows(
            full_4k[:-1], full_16k, expected_producers
        ),
        "expected 16084",
    )
    duplicated = sorted(
        [*full_4k, copy.deepcopy(full_4k[0])],
        key=lambda item: (item["stable_id"], item["aggregate_id"]),
    )
    expect_failure(
        "full-publication-duplicate-row",
        lambda: full_publication_rows(
            duplicated, full_16k, expected_producers
        ),
        "duplicate aggregate IDs",
    )
    cross_page = copy.deepcopy(full_4k)
    cross_page[0]["guest_page_size"] = "16384"
    expect_failure(
        "full-publication-cross-page-row",
        lambda: full_publication_rows(
            cross_page, full_16k, expected_producers
        ),
        "cross-page rows",
    )
    victim = full_4k[0]["producer"]
    substituted = copy.deepcopy(full_4k)
    for item in substituted:
        if item["producer"] == victim:
            item["producer"] = "same-count-substituted-producer"
            item["aggregate_id"] = aggregate_id(item)
    substituted.sort(key=lambda item: (item["stable_id"], item["aggregate_id"]))
    expect_failure(
        "full-publication-producer-substitution",
        lambda: full_publication_rows(
            substituted, full_16k, expected_producers
        ),
        "producer set differs from raw evidence",
    )
    substituted_authority = set(expected_producers["4096"])
    substituted_authority.remove(victim)
    substituted_authority.add("same-count-substituted-producer")
    expect_failure(
        "full-publication-authority-substitution",
        lambda: validate_producer_authority(
            substituted_authority, "4096"
        ),
        "producer authority differs",
    )
    fabricated = copy.deepcopy(full_4k)
    fabricated[0]["declared_digest"] = "0" * 64
    expect_failure(
        "full-publication-fabricated-row",
        lambda: require_exact_raw_derivation(
            fabricated, full_4k, "4096"
        ),
        "does not exactly derive from supplied raw evidence",
    )
    expect_failure(
        "full-publication-output-alias",
        lambda: validate_full_publication_paths(
            Path("4k.tsv"),
            Path("16k.tsv"),
            Path("output.tsv"),
            Path("output.tsv"),
        ),
        "write targets alias each other",
    )
    expect_failure(
        "full-publication-input-output-alias",
        lambda: validate_full_publication_paths(
            Path("4k.tsv"),
            Path("16k.tsv"),
            Path("4k.tsv"),
            Path("identity.json"),
        ),
        "target aliases a read input",
    )
    expect_failure(
        "full-publication-model-output-alias",
        lambda: validate_full_publication_paths(
            Path("4k.tsv"),
            Path("16k.tsv"),
            Path("required.tsv"),
            Path("identity.json"),
            (Path("required.tsv"),),
        ),
        "target aliases a read input",
    )
    with tempfile.TemporaryDirectory(
        prefix="a64-full-publication-fixture-"
    ) as temporary_name:
        temporary = Path(temporary_name).resolve()
        target = temporary / "producer-target.tsv.raw"
        target.write_text("fixture\n", encoding="utf-8")
        link = temporary / "producer.tsv.raw"
        link.symlink_to(target)
        expect_failure(
            "full-publication-symlink-raw",
            lambda: require_regular_raw_file(link),
            "not a nonsymlink regular file",
        )

        nested_output = temporary / "publication"
        expect_failure(
            "full-publication-output-identity-ancestry",
            lambda: validate_full_publication_paths(
                temporary / "4k.tsv",
                temporary / "16k.tsv",
                nested_output,
                nested_output / "identity.json",
            ),
            "impossible ancestry",
        )
        read_file = temporary / "read-model.tsv"
        read_file.write_text("model\n", encoding="utf-8")
        expect_failure(
            "full-publication-output-under-read-file",
            lambda: validate_full_publication_paths(
                temporary / "4k.tsv",
                temporary / "16k.tsv",
                read_file / "output.tsv",
                temporary / "identity.json",
                (read_file,),
            ),
            "nondirectory path component",
        )
        if not filesystem_is_case_sensitive(temporary):
            expect_failure(
                "full-publication-case-variant-output-alias",
                lambda: validate_full_publication_paths(
                    temporary / "4k.tsv",
                    temporary / "16k.tsv",
                    temporary / "Result.tsv",
                    temporary / "result.tsv",
                ),
                "write targets alias each other",
            )

        missing_parent = temporary / "missing-parent"
        expect_failure(
            "publication-lock-missing-parent",
            lambda: publication_target_locks(
                (missing_parent / "output.tsv",)
            ).__enter__(),
            "publication target parent is not a regular directory",
        )
        if missing_parent.exists():
            raise AssertionError("missing-parent preflight left filesystem debris")

        locked_manifest = temporary / "concurrent-manifest.tsv"
        locked_identity = temporary / "concurrent-identity.json"
        acquired = Event()

        def contend_for_pair() -> None:
            with publication_target_locks(
                (locked_manifest, locked_identity)
            ):
                acquired.set()

        with publication_target_locks((locked_manifest, locked_identity)):
            contender = Thread(target=contend_for_pair)
            contender.start()
            if acquired.wait(0.05):
                raise AssertionError("publication pair lock did not serialize")
        contender.join(timeout=2.0)
        if contender.is_alive() or not acquired.is_set():
            raise AssertionError("publication pair contender did not resume")
        if any(path.name.endswith("publication.lock") for path in temporary.iterdir()):
            raise AssertionError("publication parent locking left lock debris")

        real_parent = temporary / "real-parent"
        real_parent.mkdir()
        linked_output_parent = temporary / "linked-output-parent"
        linked_output_parent.symlink_to(real_parent, target_is_directory=True)
        expect_failure(
            "publication-existing-parent-symlink",
            lambda: require_no_symlink_components(
                linked_output_parent / "output.tsv", "fixture output"
            ),
            "symlinked path component",
        )
        broken_parent = temporary / "broken-parent"
        broken_parent.symlink_to(temporary / "absent", target_is_directory=True)
        expect_failure(
            "publication-broken-parent-symlink",
            lambda: require_no_symlink_components(
                broken_parent / "output.tsv", "fixture output"
            ),
            "symlinked path component",
        )
        switch_parent = temporary / "switch-parent"
        switch_parent.mkdir()
        switched_files, _switched_directories, _switched_reads = (
            validate_publication_topology((switch_parent / "output.tsv",))
        )
        moved_parent = temporary / "switch-parent-moved"
        switch_parent.rename(moved_parent)
        switch_parent.symlink_to(real_parent, target_is_directory=True)
        expect_failure(
            "publication-swapped-parent",
            lambda: publication_target_locks(switched_files).__enter__(),
            "symlinked path component",
        )

        raw_directory = temporary / "complete-raw"
        raw_directory.mkdir()

        def write_raw(producer: str) -> None:
            row = {column: "" for column in RAW_COLUMNS}
            row["ctest"] = producer
            row["guest_page_size"] = "4096"
            with (raw_directory / f"{producer}.tsv.raw").open(
                "w", encoding="utf-8", newline=""
            ) as output:
                writer = csv.DictWriter(
                    output,
                    fieldnames=RAW_COLUMNS,
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerow(row)

        for producer in sorted(expected_producers["4096"]):
            write_raw(producer)
        raw_producer_authority(raw_directory, "4096")
        raw_victim = sorted(expected_producers["4096"])[0]
        raw_victim_path = raw_directory / f"{raw_victim}.tsv.raw"
        raw_lines = raw_victim_path.read_text(encoding="utf-8").splitlines()
        fields = raw_lines[1].split("\t")
        fields[RAW_COLUMNS.index("ctest")] = "swapped-producer-content"
        raw_victim_path.write_text(
            raw_lines[0] + "\n" + "\t".join(fields) + "\n",
            encoding="utf-8",
        )
        expect_failure(
            "full-publication-swapped-raw-content",
            lambda: raw_producer_authority(raw_directory, "4096"),
            "raw producer field differs from filename",
        )
        write_raw(raw_victim)
        raw_victim_path.write_text(raw_lines[0] + "\n", encoding="utf-8")
        expect_failure(
            "full-publication-empty-raw-content",
            lambda: raw_producer_authority(raw_directory, "4096"),
            "raw evidence stream is empty",
        )
        write_raw(raw_victim)
        raw_lines = raw_victim_path.read_text(encoding="utf-8").splitlines()
        raw_victim_path.write_text(
            raw_lines[0] + "\n" + raw_lines[1] + "\textra\n",
            encoding="utf-8",
        )
        expect_failure(
            "full-publication-extra-raw-field",
            lambda: raw_producer_authority(raw_directory, "4096"),
            "raw evidence row shape differs",
        )

        failed_raw = temporary / "failed-aggregate-raw"
        failed_raw.mkdir()
        failed_row = {column: "" for column in RAW_COLUMNS}
        failed_row.update(
            {
                "ctest": "drift-producer",
                "case": "declared-valid-unallocated",
                "source": "tests/unit/arm64/fixture.cpp",
                "line": "1",
                "encoding": "ffffffff",
                "invalid": "0",
                "guest_page_size": "4096",
                "expected_event": "system-call",
                "expected_retirement": "exact:1",
                "expected_state_oracle": "exact",
                "initial_state_digest": "0" * 16,
                "initial_memory_digest": "1" * 16,
                "expected_state_digest": "2" * 16,
                "expected_memory_digest": "3" * 16,
                "expected_fault": "none",
                "expected_syndrome": "00000000",
                "native_required_features": "baseline",
                "native_requirement_scope": "instruction",
                "native_observed_features": "baseline=1",
                "native_missing_features": "none",
                "interpreter_result": "executed-retired",
                "gadgets_result": "executed-retired",
                "jit_result": "executed-retired",
                "occurrences": "1",
            }
        )
        failed_raw_path = failed_raw / "drift-producer.tsv.raw"
        with failed_raw_path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(
                output,
                fieldnames=RAW_COLUMNS,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerow(failed_row)
        aggregate_output = temporary / "preserved-aggregate.tsv"
        unmapped_output = temporary / "preserved-unmapped.tsv"
        aggregate_output.write_bytes(b"old-aggregate")
        unmapped_output.write_bytes(b"old-unmapped")
        aggregate_script = Path(__file__).with_name(
            "aggregate_a64_raw_instruction_evidence.py"
        )
        failed_aggregate = subprocess.run(
            [
                sys.executable,
                "-E",
                "-s",
                "-B",
                str(aggregate_script),
                "--evidence-directory",
                str(failed_raw),
                "--output",
                str(aggregate_output),
                "--unmapped-output",
                str(unmapped_output),
                "--decode-chunk-size",
                "1",
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if failed_aggregate.returncode == 0:
            raise AssertionError("semantic aggregate failure was accepted")
        if (
            aggregate_output.read_bytes() != b"old-aggregate"
            or unmapped_output.read_bytes() != b"old-unmapped"
        ):
            raise AssertionError("failed aggregate replaced prior outputs")
        raw_before = failed_raw_path.read_bytes()
        alias_aggregate = subprocess.run(
            [
                sys.executable,
                "-E",
                "-s",
                "-B",
                str(aggregate_script),
                "--evidence-directory",
                str(failed_raw),
                "--output",
                str(failed_raw_path),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if alias_aggregate.returncode == 0 or failed_raw_path.read_bytes() != raw_before:
            raise AssertionError("aggregate output/raw-input alias was not fail-closed")

        model = temporary / "model.tsv"
        model.write_text("frozen\n", encoding="utf-8")
        identities = freeze_read_inputs((model,))
        model.write_text("changed\n", encoding="utf-8")
        expect_failure(
            "full-publication-read-input-mutation",
            lambda: recheck_read_inputs(identities),
            "publication read input identity changed",
        )
        model.write_text("reversible\n", encoding="utf-8")
        reversible_identity = freeze_read_inputs((model,))
        original_status = model.stat()
        time.sleep(0.01)
        model.write_text("transient\n", encoding="utf-8")
        model.write_text("reversible\n", encoding="utf-8")
        os.utime(
            model,
            ns=(original_status.st_atime_ns, original_status.st_mtime_ns),
        )
        expect_failure(
            "full-publication-reversible-input-mutation",
            lambda: recheck_read_inputs(reversible_identity),
            "publication read input identity changed",
        )

        first_parent = temporary / "first-parent"
        first_parent.mkdir()
        (first_parent / "input.tsv").write_text("same\n", encoding="utf-8")
        linked_parent = temporary / "linked-parent"
        linked_parent.symlink_to(first_parent, target_is_directory=True)
        expect_failure(
            "full-publication-parent-symlink",
            lambda: freeze_read_inputs((linked_parent / "input.tsv",)),
            "symlinked path component",
        )

        first_output = temporary / "bound-manifest.tsv"
        second_output = temporary / "bound-identity.json"
        first_output.write_bytes(b"old-manifest")
        second_output.write_bytes(b"old-identity")
        model.write_text("before-stage\n", encoding="utf-8")
        staged_identity = freeze_read_inputs((model,))

        def mutate_before_replace() -> None:
            model.write_text("during-stage\n", encoding="utf-8")
            recheck_read_inputs(staged_identity)

        expect_failure(
            "full-publication-staged-input-mutation",
            lambda: publish_fixture(
                (
                    (first_output, b"new-manifest"),
                    (second_output, b"new-identity"),
                ),
                before_replace=mutate_before_replace,
            ),
            "publication read input identity changed",
        )
        if (
            first_output.read_bytes() != b"old-manifest"
            or second_output.read_bytes() != b"old-identity"
        ):
            raise AssertionError("failed publication replaced a bound output")
        first_output.chmod(0o640)
        second_output.chmod(0o644)
        publish_fixture(
            (
                (first_output, b"new-manifest"),
                (second_output, b"new-identity"),
            )
        )
        if (
            first_output.stat().st_mode & 0o777 != 0o640
            or second_output.stat().st_mode & 0o777 != 0o644
        ):
            raise AssertionError("publication replacement changed existing modes")
        new_output = temporary / "new-mode.tsv"
        publish_fixture(((new_output, b"new"),))
        if new_output.stat().st_mode & 0o777 != 0o644:
            raise AssertionError("new publication did not use canonical mode")

        manifest_path = (
            repository
            / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv"
        )
        manifest_identity_path = manifest_path.with_suffix(".identity.json")
        read_bound_manifest(manifest_path, manifest_identity_path)
        stale_identity = temporary / "stale.identity.json"
        stale_value = json.loads(
            manifest_identity_path.read_text(encoding="utf-8")
        )
        stale_value["row_count"] += 1
        stale_value["guest_page_size_counts"]["4096"] += 1
        stale_identity.write_text(
            json.dumps(
                stale_value,
                ensure_ascii=True,
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        expect_failure(
            "validator-stale-manifest-identity",
            lambda: read_bound_manifest(manifest_path, stale_identity),
            "differs from its frozen identity",
        )
        boolean_identity = temporary / "boolean.identity.json"
        boolean_value = json.loads(
            manifest_identity_path.read_text(encoding="utf-8")
        )
        boolean_value["row_count"] = True
        boolean_identity.write_text(
            json.dumps(
                boolean_value,
                ensure_ascii=True,
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        expect_failure(
            "validator-boolean-manifest-count",
            lambda: read_bound_manifest(manifest_path, boolean_identity),
            "invalid row_count",
        )
        expect_read_failure(
            "validator-missing-manifest-identity",
            lambda: read_bound_manifest(
                manifest_path, temporary / "missing.identity.json"
            ),
            "No such file",
        )

        cli_directory = temporary / "cli-policy"
        cli_directory.mkdir()
        environment = {
            name: value
            for name, value in os.environ.items()
            if not name.startswith("PYTHON")
        }
        for script_name, removed_option, required_arguments in (
            (
                "validate_a64_instruction_aggregate_manifest.py",
                "--write-manifest",
                (
                    "--actual-4k", "unused-4k.tsv",
                    "--actual-16k", "unused-16k.tsv",
                    "--evidence-4k", "unused-4k",
                    "--evidence-16k", "unused-16k",
                ),
            ),
            (
                "freeze_a64_aggregate_manifest.py",
                "--base-manifest",
                (
                    "--full-4k-manifest", "unused-4k.tsv",
                    "--full-16k-manifest", "unused-16k.tsv",
                    "--evidence-4k", "unused-4k",
                    "--evidence-16k", "unused-16k",
                    "--output", "unused-output.tsv",
                    "--identity-output", "unused-identity.json",
                ),
            ),
        ):
            completed = subprocess.run(
                [
                    sys.executable,
                    "-E",
                    "-s",
                    "-B",
                    str(Path(__file__).with_name(script_name)),
                    removed_option,
                    *required_arguments,
                ],
                cwd=cli_directory,
                env=environment,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if (
                completed.returncode == 0
                or f"unrecognized arguments: {removed_option}"
                not in completed.stdout
            ):
                raise AssertionError(
                    f"removed publication option remains accepted: "
                    f"{removed_option}: {completed.stdout}"
                )
        if any(cli_directory.iterdir()):
            raise AssertionError("rejected publication CLI created output files")

    print("A64 aggregate-manifest strict publication fixtures passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
