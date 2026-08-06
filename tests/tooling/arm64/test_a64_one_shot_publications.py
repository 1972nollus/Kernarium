#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adversarial fixtures for fail-closed one-shot A64 publishers."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Callable, Mapping, Sequence
from unittest.mock import patch

from a64_aslref_corpus_identity import (
    CORPUS_SCHEMA,
    IDENTITY_SCHEMA,
    QUEUE_SCHEMA,
    canonical_json,
    columns_sha256,
    corpus_identity_bytes,
    validate_corpus_trio,
)
from a64_bounded_generation import (
    BoundedWorkspace,
    GenerationLimits,
    run_bounded,
)
from a64_one_shot_publication import OneShotPublication, render_tsv
from a64_raw_evidence_files import (
    atomic_publish_bytes,
    filesystem_is_case_sensitive,
    publication_target_locks,
    validate_publication_topology,
)


PUBLISHERS = (
    "generate_a64_aslref_aggregate_execution_plan.py",
    "generate_a64_aslref_behavior_status.py",
    "generate_a64_aslref_corpus.py",
    "generate_a64_instruction_allocations.py",
    "generate_a64_instruction_decode_partitions.py",
    "generate_a64_instruction_required_variants.py",
    "harvest_a64_instruction_semantic_contracts.py",
    "probe_a64_aslref_batch.py",
    "run_a64_aslref_execution_oracle.py",
    "run_a64_aslref_specimen.py",
)
WORKSPACE_GENERATORS = (
    "freeze_a64_aggregate_manifest.py",
    "generate_a64_aslref_corpus.py",
    "generate_a64_instruction_allocations.py",
    "generate_a64_instruction_decode_partitions.py",
    "generate_a64_instruction_required_variants.py",
    "probe_a64_aslref_batch.py",
    "run_a64_aslref_execution_oracle.py",
    "run_a64_aslref_specimen.py",
)
PROCESS_GENERATORS = (
    "freeze_a64_aggregate_manifest.py",
    "generate_a64_aslref_corpus.py",
    "generate_a64_instruction_allocations.py",
    "probe_a64_aslref_batch.py",
    "run_a64_aslref_execution_oracle.py",
    "run_a64_aslref_specimen.py",
)


def expect_failure(
    name: str,
    action: Callable[[], object],
    text: str,
) -> None:
    try:
        action()
    except (OSError, ValueError) as error:
        if text not in str(error):
            raise AssertionError(f"{name}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{name}: accepted the negative fixture")


def row(value: str) -> list[dict[str, str]]:
    return [{"key": "fixture", "value": value}]


def single_publication(input_path: Path, output_path: Path) -> OneShotPublication:
    return OneShotPublication.prepare([output_path], [input_path])


def topology_fixtures(root: Path) -> None:
    input_path = root / "input.tsv"
    input_path.write_text("frozen\n", encoding="utf-8")

    hardlink_output = root / "hardlink-output.tsv"
    os.link(input_path, hardlink_output)
    expect_failure(
        "read-write-hardlink-alias",
        lambda: single_publication(input_path, hardlink_output),
        "aliases a read input",
    )
    if input_path.read_text(encoding="utf-8") != "frozen\n":
        raise AssertionError("hardlink alias rejection mutated the input")

    first_output = root / "first-output.tsv"
    second_output = root / "second-output.tsv"
    first_output.write_text("old\n", encoding="utf-8")
    os.link(first_output, second_output)
    expect_failure(
        "paired-hardlink-alias",
        lambda: OneShotPublication.prepare(
            [first_output, second_output], [input_path]
        ),
        "write targets alias",
    )

    directory_target = root / "publication-directory"
    expect_failure(
        "read-write-ancestry",
        lambda: validate_publication_topology(
            [],
            directory_targets=[directory_target],
            read_inputs=[directory_target / "input.tsv"],
        ),
        "ancestry",
    )

    real_parent = root / "real-parent"
    real_parent.mkdir()
    linked_parent = root / "linked-parent"
    linked_parent.symlink_to(real_parent, target_is_directory=True)
    expect_failure(
        "symlinked-output-parent",
        lambda: single_publication(input_path, linked_parent / "output.tsv"),
        "symlinked path component",
    )
    linked_input = root / "linked-input.tsv"
    linked_input.symlink_to(input_path)
    expect_failure(
        "symlinked-read-input",
        lambda: single_publication(linked_input, root / "safe-output.tsv"),
        "symlink",
    )
    expect_failure(
        "missing-output-parent",
        lambda: single_publication(
            input_path, root / "missing-parent" / "output.tsv"
        ),
        "publication target parent",
    )

    if not filesystem_is_case_sensitive(root):
        expect_failure(
            "casefold-output-alias",
            lambda: OneShotPublication.prepare(
                [root / "Case-Output.tsv", root / "case-output.tsv"],
                [input_path],
            ),
            "write targets alias",
        )


def mutation_and_staging_fixtures(root: Path) -> None:
    input_path = root / "mutable-input.tsv"
    output_path = root / "stable-output.tsv"
    input_path.write_text("version-a\n", encoding="utf-8")
    output_path.write_text("old-output\n", encoding="utf-8")
    publication = single_publication(input_path, output_path)
    input_path.write_text("version-b\n", encoding="utf-8")
    expect_failure(
        "frozen-input-mutation",
        lambda: publication.publish_tsvs(
            [(publication.targets[0], ["key", "value"], row("new"))]
        ),
        "identity changed",
    )
    if output_path.read_text(encoding="utf-8") != "old-output\n":
        raise AssertionError("input mutation changed the old output")

    replaced_input = root / "replaced-input.tsv"
    replaced_output = root / "replaced-output.tsv"
    replacement = root / "replacement.tsv"
    replaced_input.write_text("same-bytes\n", encoding="utf-8")
    replaced_output.write_text("old-output\n", encoding="utf-8")
    replacement.write_text("same-bytes\n", encoding="utf-8")
    replaced_publication = single_publication(replaced_input, replaced_output)
    os.replace(replacement, replaced_input)
    expect_failure(
        "frozen-input-inode-replacement",
        lambda: replaced_publication.publish_tsvs(
            [(replaced_publication.targets[0], ["key", "value"], row("new"))]
        ),
        "identity changed",
    )
    if replaced_output.read_text(encoding="utf-8") != "old-output\n":
        raise AssertionError("input inode replacement changed the old output")

    first = root / "stage-first.tsv"
    second = root / "stage-second.tsv"
    first.write_text("old-first\n", encoding="utf-8")
    second.write_text("old-second\n", encoding="utf-8")

    def reject_staged(_paths: object) -> None:
        raise ValueError("fixture staged verifier rejected")

    with publication_target_locks((first, second)):
        expect_failure(
            "staged-set-verifier",
            lambda: atomic_publish_bytes(
                [(first, b"new-first\n"), (second, b"new-second\n")],
                validate_staged=reject_staged,
            ),
            "staged verifier rejected",
        )
    if first.read_text() != "old-first\n" or second.read_text() != "old-second\n":
        raise AssertionError("staged validation failure split the old pair")

    mode_input = root / "mode-input.tsv"
    existing_output = root / "mode-existing.tsv"
    new_output = root / "mode-new.tsv"
    mode_input.write_text("frozen\n", encoding="utf-8")
    existing_output.write_text("old\n", encoding="utf-8")
    existing_output.chmod(0o640)
    existing = single_publication(mode_input, existing_output)
    existing.publish_tsvs(
        [(existing.targets[0], ["key", "value"], row("existing-mode"))]
    )
    if stat.S_IMODE(existing_output.stat().st_mode) != 0o640:
        raise AssertionError("publication changed the existing target mode")
    created = single_publication(mode_input, new_output)
    created.publish_tsvs(
        [(created.targets[0], ["key", "value"], row("new-mode"))]
    )
    if stat.S_IMODE(new_output.stat().st_mode) != 0o644:
        raise AssertionError("new publication target does not use mode 0644")

    capped_first = root / "capped-first.bin"
    capped_second = root / "capped-second.bin"
    capped = OneShotPublication.prepare(
        [capped_first, capped_second], [mode_input]
    )
    accept = lambda _payload: None
    with patch.multiple(
        "a64_one_shot_publication",
        MAX_OUTPUT_BYTES=8,
        MAX_OUTPUT_SET_BYTES=12,
    ):
        capped.publish_payloads(
            [
                (capped.targets[0], b"a" * 6, accept),
                (capped.targets[1], b"b" * 6, accept),
            ]
        )
        expect_failure(
            "publication-artifact-cap-plus-one",
            lambda: capped.publish_payloads(
                [
                    (capped.targets[0], b"x" * 9, accept),
                    (capped.targets[1], b"y", accept),
                ]
            ),
            "publication output exceeds 8 bytes",
        )
        expect_failure(
            "publication-set-cap-plus-one",
            lambda: capped.publish_payloads(
                [
                    (capped.targets[0], b"x" * 6, accept),
                    (capped.targets[1], b"y" * 7, accept),
                ]
            ),
            "publication output set exceeds 12 bytes",
        )
    if capped_first.read_bytes() != b"a" * 6:
        raise AssertionError("publication cap failure changed the first output")
    if capped_second.read_bytes() != b"b" * 6:
        raise AssertionError("publication cap failure changed the second output")


def identity_payloads(
    corpus: Path,
    queue: Path,
    value: str,
) -> tuple[bytes, bytes, bytes]:
    corpus_columns = ["key", "value"]
    queue_columns = ["symbol", "value"]
    corpus_payload = render_tsv(corpus_columns, [{"key": "one", "value": value}])
    queue_payload = render_tsv(
        queue_columns, [{"symbol": "two", "value": value}]
    )
    identity = corpus_identity_bytes(
        corpus.name,
        corpus_payload,
        corpus_columns,
        queue.name,
        queue_payload,
        queue_columns,
    )
    return corpus_payload, queue_payload, identity


def unchecked_identity(
    corpus: Path,
    corpus_payload: bytes,
    corpus_columns: list[str],
    corpus_rows: int,
    queue: Path,
    queue_payload: bytes,
    queue_columns: list[str],
    queue_rows: int,
) -> bytes:
    members = [
        {
            "columns_sha256": columns_sha256(corpus_columns),
            "file_name": corpus.name,
            "file_schema": CORPUS_SCHEMA,
            "file_sha256": hashlib.sha256(corpus_payload).hexdigest(),
            "role": "allocation-corpus",
            "row_count": corpus_rows,
        },
        {
            "columns_sha256": columns_sha256(queue_columns),
            "file_name": queue.name,
            "file_schema": QUEUE_SCHEMA,
            "file_sha256": hashlib.sha256(queue_payload).hexdigest(),
            "role": "interface-queue",
            "row_count": queue_rows,
        },
    ]
    value = {
        "generation_sha256": hashlib.sha256(
            (IDENTITY_SCHEMA + "\0" + canonical_json(members)).encode("utf-8")
        ).hexdigest(),
        "members": members,
        "schema": IDENTITY_SCHEMA,
    }
    return (canonical_json(value) + "\n").encode("ascii")


def corpus_identity_fixtures(root: Path) -> None:
    corpus = root / "corpus.tsv"
    queue = root / "queue.tsv"
    identity = root / "corpus.identity.json"
    corpus_columns = ["key", "value"]
    queue_columns = ["symbol", "value"]
    old = identity_payloads(corpus, queue, "old")
    new = identity_payloads(corpus, queue, "new")
    for path, payload in zip((corpus, queue, identity), old):
        path.write_bytes(payload)
    validate_corpus_trio(
        corpus, corpus_columns, queue, queue_columns, identity
    )

    corpus.write_bytes(new[0])
    expect_failure(
        "split-after-first-payload",
        lambda: validate_corpus_trio(
            corpus, corpus_columns, queue, queue_columns, identity
        ),
        "differ from their bound generation identity",
    )
    corpus.write_bytes(old[0])
    corpus.write_bytes(new[0])
    queue.write_bytes(new[1])
    expect_failure(
        "split-before-identity-last",
        lambda: validate_corpus_trio(
            corpus, corpus_columns, queue, queue_columns, identity
        ),
        "differ from their bound generation identity",
    )
    identity.write_bytes(new[2])
    validate_corpus_trio(
        corpus, corpus_columns, queue, queue_columns, identity
    )

    renamed_queue = root / "renamed-queue.tsv"
    renamed_queue.write_bytes(new[1])
    expect_failure(
        "renamed-bound-member",
        lambda: validate_corpus_trio(
            corpus, corpus_columns, renamed_queue, queue_columns, identity
        ),
        "differ from their bound generation identity",
    )
    identity.unlink()
    expect_failure(
        "missing-identity-last",
        lambda: validate_corpus_trio(
            corpus, corpus_columns, queue, queue_columns, identity
        ),
        "regular file",
    )

    malformed_corpus = root / "malformed-corpus.tsv"
    malformed_queue = root / "malformed-queue.tsv"
    malformed_identity = root / "malformed.identity.json"
    queue_payload = render_tsv(queue_columns, [{"symbol": "two", "value": "ok"}])
    malformed_payload = b"key\tvalue\none\n"
    malformed_corpus.write_bytes(malformed_payload)
    malformed_queue.write_bytes(queue_payload)
    malformed_identity.write_bytes(
        unchecked_identity(
            malformed_corpus,
            malformed_payload,
            corpus_columns,
            1,
            malformed_queue,
            queue_payload,
            queue_columns,
            1,
        )
    )
    expect_failure(
        "consistently-bound-malformed-row",
        lambda: validate_corpus_trio(
            malformed_corpus,
            corpus_columns,
            malformed_queue,
            queue_columns,
            malformed_identity,
        ),
        "row shape differs",
    )

    crlf_payload = b"key\tvalue\r\none\tbad\r\n"
    malformed_corpus.write_bytes(crlf_payload)
    malformed_identity.write_bytes(
        unchecked_identity(
            malformed_corpus,
            crlf_payload,
            corpus_columns,
            1,
            malformed_queue,
            queue_payload,
            queue_columns,
            1,
        )
    )
    expect_failure(
        "consistently-bound-noncanonical-tsv",
        lambda: validate_corpus_trio(
            malformed_corpus,
            corpus_columns,
            malformed_queue,
            queue_columns,
            malformed_identity,
        ),
        "canonical LF-terminated TSV",
    )

    duplicate_payload = render_tsv(
        corpus_columns,
        [
            {"key": "duplicate", "value": "one"},
            {"key": "duplicate", "value": "two"},
        ],
    )
    duplicate_identity = corpus_identity_bytes(
        malformed_corpus.name,
        duplicate_payload,
        corpus_columns,
        malformed_queue.name,
        queue_payload,
        queue_columns,
    )
    malformed_corpus.write_bytes(duplicate_payload)
    malformed_identity.write_bytes(duplicate_identity)

    def unique_keys(rows: Sequence[Mapping[str, str]]) -> None:
        values = [item["key"] for item in rows]
        if len(values) != len(set(values)):
            raise ValueError("fixture corpus semantic identity is duplicate")

    expect_failure(
        "consistently-bound-semantic-duplicate",
        lambda: validate_corpus_trio(
            malformed_corpus,
            corpus_columns,
            malformed_queue,
            queue_columns,
            malformed_identity,
            corpus_validator=unique_keys,
        ),
        "semantic identity is duplicate",
    )

    schema_columns = ["schema", "id"]
    wrong_schema_payload = render_tsv(
        schema_columns, [{"schema": "wrong/v1", "id": "one"}]
    )
    queue_schema_payload = render_tsv(
        schema_columns, [{"schema": QUEUE_SCHEMA, "id": "two"}]
    )
    malformed_corpus.write_bytes(wrong_schema_payload)
    malformed_queue.write_bytes(queue_schema_payload)
    malformed_identity.write_bytes(
        unchecked_identity(
            malformed_corpus,
            wrong_schema_payload,
            schema_columns,
            1,
            malformed_queue,
            queue_schema_payload,
            schema_columns,
            1,
        )
    )
    expect_failure(
        "consistently-bound-wrong-member-schema",
        lambda: validate_corpus_trio(
            malformed_corpus,
            schema_columns,
            malformed_queue,
            schema_columns,
            malformed_identity,
        ),
        "row schema differs",
    )


def worker(input_path: Path, output_path: Path) -> int:
    publication = single_publication(input_path, output_path)
    print("ready", flush=True)
    publication.publish_tsvs(
        [(publication.targets[0], ["key", "value"], row("worker"))]
    )
    print("published", flush=True)
    return 0


def concurrency_fixture(root: Path) -> None:
    input_path = root / "concurrent-input.tsv"
    output_path = root / "concurrent-output.tsv"
    input_path.write_text("frozen\n", encoding="utf-8")
    output_path.write_text("old\n", encoding="utf-8")
    command = [
        sys.executable,
        "-B",
        str(Path(__file__).resolve()),
        "--worker",
        str(input_path),
        str(output_path),
    ]
    child: subprocess.Popen[str]
    with publication_target_locks((output_path,)):
        child = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert child.stdout is not None
        ready = child.stdout.readline().strip()
        if ready != "ready":
            raise AssertionError(f"concurrent worker did not become ready: {ready!r}")
        time.sleep(0.15)
        if child.poll() is not None:
            output = child.stdout.read()
            raise AssertionError(
                "concurrent publisher bypassed the parent lock: " + output
            )
        if output_path.read_text(encoding="utf-8") != "old\n":
            raise AssertionError("blocked concurrent publisher mutated output")
    output = child.communicate(timeout=5)[0]
    if child.returncode != 0 or "published" not in output:
        raise AssertionError(
            "concurrent publisher failed after lock release: "
            f"{child.returncode}: {output}"
        )
    expected = render_tsv(["key", "value"], row("worker"))
    if output_path.read_bytes() != expected:
        raise AssertionError("serialized concurrent publication bytes differ")


def bounded_workspace(
    root: Path,
    *,
    workspace_bytes: int,
    artifact_bytes: int,
    log_bytes: int,
    files: int,
) -> BoundedWorkspace:
    return BoundedWorkspace(
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=[root / "declared-output.tsv"],
        work_root=root,
        limits=GenerationLimits(
            free_space_reserve_bytes=1,
            max_workspace_bytes=workspace_bytes,
            max_artifact_bytes=artifact_bytes,
            max_log_bytes=log_bytes,
            max_workspace_files=files,
        ),
        prefix="bounded-fixture-",
    )


def bounded_generation_fixtures(root: Path) -> None:
    storage_root = root / "bounded-storage"
    storage_root.mkdir()
    with bounded_workspace(
        storage_root,
        workspace_bytes=16,
        artifact_bytes=8,
        log_bytes=64,
        files=2,
    ) as workspace:
        workspace.write_bytes(workspace.require_path() / "first", b"a" * 8)
        workspace.write_bytes(workspace.require_path() / "second", b"b" * 8)
        if workspace.usage() != (16, 2):
            raise AssertionError("exact workspace byte/file boundary drifted")
        expect_failure(
            "workspace-file-cap-plus-one",
            lambda: workspace.write_bytes(
                workspace.require_path() / "third", b""
            ),
            "file count exceeds cap",
        )
        expect_failure(
            "workspace-byte-cap-plus-one",
            lambda: workspace.reserve(1, 0),
            "workspace exceeds cap",
        )
        expect_failure(
            "artifact-cap-plus-one",
            lambda: workspace.write_bytes(
                workspace.require_path() / "large", b"c" * 9
            ),
            "artifact exceeds cap",
        )

    directory_root = root / "bounded-directories"
    directory_root.mkdir()
    with bounded_workspace(
        directory_root,
        workspace_bytes=1,
        artifact_bytes=1,
        log_bytes=1,
        files=1,
    ) as workspace:
        first_directory = workspace.require_path() / "first-directory"
        second_directory = workspace.require_path() / "second-directory"
        first_directory.mkdir()
        if workspace.usage() != (0, 1):
            raise AssertionError("exact workspace directory boundary drifted")
        second_directory.mkdir()
        expect_failure(
            "workspace-directory-cap-plus-one",
            workspace.check,
            "file-count cap",
        )
        second_directory.rmdir()

    shape_root = root / "bounded-shapes"
    shape_root.mkdir()
    with bounded_workspace(
        shape_root,
        workspace_bytes=16,
        artifact_bytes=8,
        log_bytes=8,
        files=4,
    ) as workspace:
        symlink = workspace.require_path() / "link"
        symlink.symlink_to(root / "outside")
        expect_failure(
            "workspace-symlink",
            workspace.check,
            "workspace contains a symlink",
        )
        symlink.unlink()
        fifo = workspace.require_path() / "fifo"
        os.mkfifo(fifo)
        expect_failure(
            "workspace-special-file",
            workspace.check,
            "workspace contains a special file",
        )
        fifo.unlink()

    archive_root = root / "bounded-archive"
    archive_root.mkdir()
    with bounded_workspace(
        archive_root,
        workspace_bytes=16,
        artifact_bytes=8,
        log_bytes=1,
        files=3,
    ) as workspace:
        first_member = tarfile.TarInfo("first")
        first_member.size = 8
        second_member = tarfile.TarInfo("second")
        second_member.size = 8
        workspace.validate_archive_members([first_member, second_member])
        extra_member = tarfile.TarInfo("extra")
        extra_member.size = 1
        expect_failure(
            "archive-workspace-cap-plus-one",
            lambda: workspace.validate_archive_members(
                [first_member, second_member, extra_member]
            ),
            "archive expansion exceeds generator workspace cap",
        )
        oversized_member = tarfile.TarInfo("oversized")
        oversized_member.size = 9
        expect_failure(
            "archive-artifact-cap-plus-one",
            lambda: workspace.validate_archive_members([oversized_member]),
            "archive member exceeds generator artifact cap",
        )
        directories = []
        for index in range(4):
            member = tarfile.TarInfo(f"directory-{index}")
            member.type = tarfile.DIRTYPE
            directories.append(member)
        workspace.validate_archive_members(directories[:3])
        expect_failure(
            "archive-file-cap-plus-one",
            lambda: workspace.validate_archive_members(directories),
            "archive expansion exceeds generator file-count cap",
        )

    reserve_root = root / "bounded-reserve"
    reserve_root.mkdir()
    with patch(
        "a64_bounded_generation.shutil.disk_usage",
        return_value=SimpleNamespace(free=17),
    ):
        with BoundedWorkspace(
            source_root=Path(__file__).resolve().parents[3],
            publication_targets=[root / "reserve-output.tsv"],
            work_root=reserve_root,
            limits=GenerationLimits(16, 8, 8, 1, 1),
            prefix="bounded-reserve-fixture-",
        ) as workspace:
            workspace.reserve(1, 0)
            expect_failure(
                "free-space-reserve-plus-one",
                lambda: workspace.reserve(2, 0),
                "below required 18 bytes",
            )

    adopted_root = root / "bounded-adopted"
    adopted_root.mkdir()
    adopted_marker = adopted_root / "preserved"
    adopted_marker.write_bytes(b"p")
    adopted_limits = GenerationLimits(1, 16, 8, 64, 4)
    with BoundedWorkspace.adopt(
        adopted_root,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=[root / "adopted-output.tsv"],
        limits=adopted_limits,
    ) as workspace:
        if workspace.require_path() != adopted_root.resolve(strict=True):
            raise AssertionError("adopted workspace identity differs")
        if workspace.usage() != (1, 1):
            raise AssertionError("adopted workspace usage differs")
    if not adopted_marker.is_file():
        raise AssertionError("adopted workspace was deleted on release")
    expect_failure(
        "adopted-publication-overlap",
        lambda: BoundedWorkspace.adopt(
            adopted_root,
            source_root=Path(__file__).resolve().parents[3],
            publication_targets=[adopted_root / "output.tsv"],
            limits=adopted_limits,
        ).__enter__(),
        "overlaps the publication target",
    )
    adopted_source = adopted_root / "source"
    adopted_source.mkdir()
    expect_failure(
        "adopted-source-ancestor",
        lambda: BoundedWorkspace.adopt(
            adopted_root,
            source_root=adopted_source,
            publication_targets=[root / "source-overlap-output.tsv"],
            limits=adopted_limits,
        ).__enter__(),
        "overlaps the source tree",
    )

    identity_root = root / "bounded-adopted-identity"
    identity_root.mkdir()
    adopted = BoundedWorkspace.adopt(
        identity_root,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=[root / "identity-output.tsv"],
        limits=adopted_limits,
    )
    adopted.__enter__()
    moved_identity = root / "bounded-adopted-identity-moved"
    identity_root.rename(moved_identity)
    identity_root.mkdir()
    try:
        expect_failure(
            "adopted-workspace-identity-swap",
            adopted.check,
            "workspace identity changed",
        )
    finally:
        adopted.__exit__(None, None, None)

    process_root = root / "bounded-process"
    process_root.mkdir()
    with bounded_workspace(
        process_root,
        workspace_bytes=4096,
        artifact_bytes=128,
        log_bytes=64,
        files=16,
    ) as workspace:
        exact_log = run_bounded(
            [
                sys.executable,
                "-B",
                "-c",
                "import os;os.write(1,b'x'*64)",
            ],
            workspace=workspace,
            timeout_seconds=2,
        )
        if exact_log.stdout != "x" * 64 or exact_log.stderr:
            raise AssertionError("exact bounded log capture differs")
        split_log = run_bounded(
            [
                sys.executable,
                "-B",
                "-c",
                "import os;os.write(1,b'o'*32);os.write(2,b'e'*32)",
            ],
            workspace=workspace,
            timeout_seconds=2,
        )
        if split_log.stdout != "o" * 32 or split_log.stderr != "e" * 32:
            raise AssertionError("exact combined log boundary drifted")
        expect_failure(
            "combined-log-cap-plus-one",
            lambda: run_bounded(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    "import os;os.write(1,b'o'*32);os.write(2,b'e'*33)",
                ],
                workspace=workspace,
                timeout_seconds=2,
            ),
            "stdout/stderr exceeds 64 bytes",
        )
        exact_input = run_bounded(
            [sys.executable, "-B", "-c", "import sys;sys.stdin.buffer.read()"],
            workspace=workspace,
            timeout_seconds=2,
            input_bytes=b"i" * 64,
        )
        if exact_input.returncode != 0:
            raise AssertionError("exact bounded command input failed")
        expect_failure(
            "command-input-cap-plus-one",
            lambda: run_bounded(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    "import sys;sys.stdin.buffer.read()",
                ],
                workspace=workspace,
                timeout_seconds=2,
                input_bytes=b"i" * 65,
            ),
            "command input exceeds log cap",
        )
        expect_failure(
            "bounded-command-timeout",
            lambda: run_bounded(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    "import time;time.sleep(5)",
                ],
                workspace=workspace,
                timeout_seconds=0.1,
            ),
            "exceeded 0.1 seconds",
        )
        cancelled_marker = root / "cancelled-marker"
        cancel_event = threading.Event()
        timer = threading.Timer(0.1, cancel_event.set)
        timer.start()
        try:
            expect_failure(
                "bounded-command-cancellation",
                lambda: run_bounded(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        (
                            "import time;from pathlib import Path;"
                            "time.sleep(0.5);"
                            f"Path({str(cancelled_marker)!r}).write_text('bad')"
                        ),
                    ],
                    workspace=workspace,
                    timeout_seconds=2,
                    cancel_event=cancel_event,
                ),
                "command was cancelled",
            )
        finally:
            timer.cancel()
            timer.join()
        time.sleep(0.6)
        if cancelled_marker.exists():
            raise AssertionError("cancelled command escaped process-group cleanup")

        def stop_observer(_process: subprocess.Popen[bytes]) -> str:
            return "fixture observer requested cancellation"

        expect_failure(
            "bounded-command-observer",
            lambda: run_bounded(
                [sys.executable, "-B", "-c", "import time;time.sleep(5)"],
                workspace=workspace,
                timeout_seconds=2,
                process_observer=stop_observer,
            ),
            "fixture observer requested cancellation",
        )

        def broken_observer(_process: subprocess.Popen[bytes]) -> str:
            raise RuntimeError("fixture observer broke")

        expect_failure(
            "bounded-command-observer-error",
            lambda: run_bounded(
                [sys.executable, "-B", "-c", "import time;time.sleep(5)"],
                workspace=workspace,
                timeout_seconds=2,
                process_observer=broken_observer,
            ),
            "process observer failed: fixture observer broke",
        )
        exact_artifact = workspace.require_path() / "exact-artifact"
        completed = run_bounded(
            [
                sys.executable,
                "-B",
                "-c",
                (
                    "from pathlib import Path;"
                    f"Path({str(exact_artifact)!r}).write_bytes(b'a'*128)"
                ),
            ],
            workspace=workspace,
            timeout_seconds=2,
        )
        if completed.returncode != 0 or exact_artifact.stat().st_size != 128:
            raise AssertionError("exact bounded artifact command failed")

    overflow_root = root / "bounded-overflow"
    overflow_root.mkdir()
    with bounded_workspace(
        overflow_root,
        workspace_bytes=4096,
        artifact_bytes=128,
        log_bytes=1024,
        files=16,
    ) as workspace:
        oversized = workspace.require_path() / "oversized-artifact"
        expect_failure(
            "subprocess-artifact-cap-plus-one",
            lambda: run_bounded(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    (
                        "from pathlib import Path;"
                        f"Path({str(oversized)!r}).write_bytes(b'a'*129)"
                    ),
                ],
                workspace=workspace,
                timeout_seconds=2,
            ),
            "artifact exceeds cap",
        )

    descendants_root = root / "bounded-descendants"
    descendants_root.mkdir()
    with bounded_workspace(
        descendants_root,
        workspace_bytes=4096,
        artifact_bytes=512,
        log_bytes=1024,
        files=16,
    ) as workspace:
        escaped = root / "escaped-descendant"
        child_code = (
            "import time;from pathlib import Path;time.sleep(0.5);"
            f"Path({str(escaped)!r}).write_text('escaped')"
        )
        leader_code = (
            "import subprocess,sys;"
            "subprocess.Popen([sys.executable,'-B','-c',sys.argv[1]]);"
            "sys.exit(0)"
        )
        expect_failure(
            "early-leader-descendant",
            lambda: run_bounded(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    leader_code,
                    child_code,
                ],
                workspace=workspace,
                timeout_seconds=2,
            ),
            "descendants remained",
        )
        time.sleep(0.7)
        if escaped.exists():
            raise AssertionError("early-leader descendant escaped cancellation")

    expect_failure(
        "source-tree-workspace",
        lambda: BoundedWorkspace(
            source_root=Path(__file__).resolve().parents[3],
            publication_targets=[root / "declared-output.tsv"],
            work_root=Path(__file__).resolve().parents[3],
            limits=GenerationLimits(1, 4096, 512, 512, 16),
            prefix="forbidden-source-work-",
        ).__enter__(),
        "inside the source tree",
    )


def structural_fixture() -> None:
    directory = Path(__file__).resolve().parent
    for name in PUBLISHERS:
        source = (directory / name).read_text(encoding="utf-8")
        if "OneShotPublication.prepare(" not in source:
            raise AssertionError(f"{name}: lacks frozen one-shot preparation")
        if "publish_tsvs" not in source and "publish_payloads" not in source:
            raise AssertionError(f"{name}: lacks atomic one-shot publication")
        if "options.output.parent.mkdir" in source:
            raise AssertionError(f"{name}: recreates an unlocked output parent")
        if 'options.output.open("w"' in source:
            raise AssertionError(f"{name}: directly truncates its output")
    for name in WORKSPACE_GENERATORS:
        source = (directory / name).read_text(encoding="utf-8")
        if "workspace_from_options(" not in source:
            raise AssertionError(f"{name}: lacks a bounded owned workspace")
    for name in PROCESS_GENERATORS:
        source = (directory / name).read_text(encoding="utf-8")
        if "BoundedCommandRunner(" not in source:
            raise AssertionError(f"{name}: does not inject the bounded runner")
        if "subprocess.run(" in source or "subprocess.Popen(" in source:
            raise AssertionError(f"{name}: bypasses the bounded process supervisor")
    corpus = (directory / "generate_a64_aslref_corpus.py").read_text(
        encoding="utf-8"
    )
    ordered = tuple(
        corpus.find(f"(publication.targets[{index}],") for index in range(3)
    )
    if min(ordered) < 0 or ordered != tuple(sorted(ordered)):
        raise AssertionError("corpus identity is not published after both payloads")


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--worker":
        return worker(Path(sys.argv[2]), Path(sys.argv[3]))
    if len(sys.argv) != 1:
        raise ValueError("unexpected fixture arguments")
    with tempfile.TemporaryDirectory(prefix="a64-one-shot-publication-") as name:
        root = Path(name).resolve(strict=True)
        topology_fixtures(root)
        mutation_and_staging_fixtures(root)
        corpus_identity_fixtures(root)
        concurrency_fixture(root)
        bounded_generation_fixtures(root)
        structural_fixture()
    print("A64 one-shot publication adversarial fixtures passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
