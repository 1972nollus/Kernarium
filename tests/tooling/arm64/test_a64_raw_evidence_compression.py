#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused parity, recovery, duplicate, and corruption gzip fixtures."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import errno
import gzip
import os
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

from a64_raw_evidence_files import (
    RawEvidenceFile,
    raw_evidence_files,
    uncompressed_identity,
)
from aggregate_a64_raw_instruction_evidence import RAW_COLUMNS, raw_rows
from compress_a64_raw_evidence import (
    compress_plain_raw,
    gzip_temporary_worst_case_bytes,
    reserve_bytes,
    resolve_entry,
    verify_gzip,
)
from validate_a64_instruction_aggregate_manifest import exception_raw_rows


def expect_failure(name: str, action: Callable[[], object], text: str) -> None:
    try:
        action()
    except ValueError as error:
        if text not in str(error):
            raise AssertionError(f"{name}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{name}: accepted the negative fixture")


def fixture_row() -> dict[str, str]:
    return {
        "ctest": "fixture-producer",
        "case": "fixture-case",
        "source": "tests/unit/arm64/fixture.cpp",
        "line": "10",
        "encoding": "d503201f",
        "invalid": "0",
        "guest_page_size": "16384",
        "expected_event": "system-call",
        "expected_retirement": "exact:2",
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


def write_fixture(path: Path, row: dict[str, str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=RAW_COLUMNS, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerow(row)


def wait_for_path(path: Path, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.01)
    raise AssertionError(f"timed out waiting for fixture path: {path}")


def runner_environment(directory: Path, producer: str) -> dict[str, str]:
    environment = dict(os.environ)
    environment["UML_INSTRUCTION_EVIDENCE"] = str(
        directory / f"{producer}.tsv"
    )
    environment["UML_INSTRUCTION_EVIDENCE_TEST"] = producer
    environment.pop("UML_INSTRUCTION_INPUT_MATERIALIZATION", None)
    environment.pop("UML_INSTRUCTION_EVIDENCE_LOCKED", None)
    return environment


def cxx_runner_fixtures(runner: Path, directory: Path) -> None:
    if runner.is_symlink() or not runner.is_file():
        raise AssertionError(f"native evidence runner is unavailable: {runner}")
    ready = directory / "runner-ready"
    hold_code = (
        "import pathlib,sys,time; "
        "pathlib.Path(sys.argv[1]).write_text('ready'); time.sleep(30)"
    )
    first = subprocess.Popen(
        [str(runner), sys.executable, "-B", "-c", hold_code, str(ready)],
        env=runner_environment(directory, "runner-a"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    second: subprocess.Popen[str] | None = None
    try:
        wait_for_path(ready)
        same = subprocess.run(
            [str(runner), "/usr/bin/true"],
            env=runner_environment(directory, "runner-a"),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if same.returncode != 125 or "lock is active" not in same.stdout:
            raise AssertionError(
                f"same-producer runner was not rejected: {same.returncode}: "
                f"{same.stdout}"
            )
        second_ready = directory / "runner-b-ready"
        second = subprocess.Popen(
            [
                str(runner),
                sys.executable,
                "-B",
                "-c",
                hold_code,
                str(second_ready),
            ],
            env=runner_environment(directory, "runner-b"),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        wait_for_path(second_ready)
        if second.poll() is not None:
            raise AssertionError("distinct native producers did not run concurrently")
    finally:
        for process in (second, first):
            if process is None or process.poll() is not None:
                continue
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=5.0)

    exit_status = subprocess.run(
        [str(runner), sys.executable, "-B", "-c", "raise SystemExit(23)"],
        env=runner_environment(directory, "runner-exit"),
        check=False,
    )
    if exit_status.returncode != 23:
        raise AssertionError("native runner did not preserve child exit status")

    def staged_environment(producer: str) -> dict[str, str]:
        environment = runner_environment(directory, producer)
        environment["UML_INSTRUCTION_RAW_EVIDENCE"] = "1"
        environment["UML_INSTRUCTION_INPUT_MATERIALIZATION"] = str(
            directory / f"{producer}.tsv.inputs.jsonl.gz"
        )
        return environment

    apparently_valid_outputs = (
        "import gzip,os,pathlib,signal,sys\n"
        "base=pathlib.Path(os.environ['UML_INSTRUCTION_EVIDENCE'])\n"
        "base.write_text('stable_id\\tsection\\tfixture\\n')\n"
        "pathlib.Path(str(base)+'.raw').write_text('ctest\\tcase\\tfixture\\n')\n"
        "with gzip.open(str(base)+'.inputs.jsonl.gz','wb') as output:\n"
        " output.write(b'{\\\"schema\\\":\\\"fixture\\\"}\\n')\n"
        "pathlib.Path(str(base)+'.native').write_text("
        "'oracle_case_id\\tctest\\tfixture\\n')\n"
    )
    failed_producer = "runner-failed-publication"
    failed_finals = (
        directory / f"{failed_producer}.tsv",
        directory / f"{failed_producer}.tsv.raw",
        directory / f"{failed_producer}.tsv.inputs.jsonl.gz",
        directory / f"{failed_producer}.tsv.native",
    )
    for index, path in enumerate(failed_finals):
        path.write_bytes(f"old-{index}".encode())
    failed_publication = subprocess.run(
        [
            str(runner),
            sys.executable,
            "-B",
            "-c",
            apparently_valid_outputs + "raise SystemExit(23)\n",
        ],
        env=staged_environment(failed_producer),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if failed_publication.returncode != 23 or any(
        path.read_bytes() != f"old-{index}".encode()
        for index, path in enumerate(failed_finals)
    ):
        raise AssertionError(
            "nonzero producer published its staged output set: "
            f"{failed_publication.returncode}: {failed_publication.stdout}"
        )

    signaled_producer = "runner-signaled-publication"
    signaled_finals = (
        directory / f"{signaled_producer}.tsv",
        directory / f"{signaled_producer}.tsv.raw",
        directory / f"{signaled_producer}.tsv.inputs.jsonl.gz",
        directory / f"{signaled_producer}.tsv.native",
    )
    signaled_publication = subprocess.run(
        [
            str(runner),
            sys.executable,
            "-B",
            "-c",
            apparently_valid_outputs
            + "os.kill(os.getpid(), signal.SIGTERM)\n",
        ],
        env=staged_environment(signaled_producer),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if signaled_publication.returncode != 143 or any(
        path.exists() for path in signaled_finals
    ):
        raise AssertionError(
            "signaled producer published its staged output set: "
            f"{signaled_publication.returncode}: {signaled_publication.stdout}"
        )

    partial_producer = "runner-partial-publication"
    partial_finals = (
        directory / f"{partial_producer}.tsv",
        directory / f"{partial_producer}.tsv.raw",
        directory / f"{partial_producer}.tsv.inputs.jsonl.gz",
    )
    for index, path in enumerate(partial_finals):
        path.write_bytes(f"prior-{index}".encode())
    partial_publication = subprocess.run(
        [
            str(runner),
            sys.executable,
            "-B",
            "-c",
            "import os,pathlib\n"
            "base=pathlib.Path(os.environ['UML_INSTRUCTION_EVIDENCE'])\n"
            "base.write_text('stable_id\\tsection\\tfixture\\n')\n",
        ],
        env=staged_environment(partial_producer),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if partial_publication.returncode != 125 or any(
        path.read_bytes() != f"prior-{index}".encode()
        for index, path in enumerate(partial_finals)
    ):
        raise AssertionError(
            "partial producer output set replaced a canonical member: "
            f"{partial_publication.returncode}: {partial_publication.stdout}"
        )

    successful_producer = "runner-successful-publication"
    successful_publication = subprocess.run(
        [str(runner), sys.executable, "-B", "-c", apparently_valid_outputs],
        env=staged_environment(successful_producer),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    successful_finals = (
        directory / f"{successful_producer}.tsv",
        directory / f"{successful_producer}.tsv.raw",
        directory / f"{successful_producer}.tsv.inputs.jsonl.gz",
        directory / f"{successful_producer}.tsv.native",
    )
    if successful_publication.returncode != 0 or not all(
        path.is_file() for path in successful_finals
    ):
        raise AssertionError(
            "successful producer did not publish its complete output set: "
            f"{successful_publication.returncode}: {successful_publication.stdout}"
        )

    raw_only_producer = "runner-raw-only-publication"
    raw_only_environment = runner_environment(directory, raw_only_producer)
    raw_only_environment["UML_INSTRUCTION_RAW_EVIDENCE"] = "1"
    raw_only_finals = (
        directory / f"{raw_only_producer}.tsv",
        directory / f"{raw_only_producer}.tsv.raw",
    )
    stale_materialization = (
        directory / f"{raw_only_producer}.tsv.inputs.jsonl.gz"
    )
    stale_native = directory / f"{raw_only_producer}.tsv.native"
    stale_materialization.write_bytes(b"stale-materialization")
    stale_native.write_bytes(b"stale-native")
    raw_only_outputs = (
        "import os,pathlib\n"
        "base=pathlib.Path(os.environ['UML_INSTRUCTION_EVIDENCE'])\n"
        "base.write_text('stable_id\\tsection\\tfixture\\n')\n"
        "pathlib.Path(str(base)+'.raw').write_text("
        "'ctest\\tcase\\tfixture\\n')\n"
    )
    raw_only_publication = subprocess.run(
        [str(runner), sys.executable, "-B", "-c", raw_only_outputs],
        env=raw_only_environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if (
        raw_only_publication.returncode != 0
        or not all(path.is_file() for path in raw_only_finals)
        or stale_materialization.exists()
        or stale_native.exists()
    ):
        raise AssertionError(
            "raw-only producer did not publish an exact output set: "
            f"{raw_only_publication.returncode}: {raw_only_publication.stdout}"
        )
    if list(directory.glob(".producer.*.runner-stage.*")):
        raise AssertionError("native runner left producer staging debris")

    orphan = directory / "orphan-write"
    orphan_code = (
        "import os,pathlib,sys,time\n"
        "child=os.fork()\n"
        "if child == 0:\n"
        " time.sleep(.5)\n"
        " pathlib.Path(sys.argv[1]).write_text('orphan')\n"
        " os._exit(0)\n"
        "os._exit(0)\n"
    )
    early = subprocess.run(
        [str(runner), sys.executable, "-B", "-c", orphan_code, str(orphan)],
        env=runner_environment(directory, "runner-early"),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=5.0,
    )
    if early.returncode != 125:
        raise AssertionError(
            f"early-exit runner did not reject a live descendant: {early.stdout}"
        )
    time.sleep(0.75)
    if orphan.exists():
        raise AssertionError("early-exit runner orphan wrote after lock release")

    pid_file = directory / "supervised-pids"
    timeout_code = (
        "import os,pathlib,signal,sys\n"
        "child=os.fork()\n"
        "if child != 0:\n"
        " pathlib.Path(sys.argv[1]).write_text(f'{os.getpid()} {child}')\n"
        "signal.pause()\n"
    )
    supervised = subprocess.Popen(
        [str(runner), sys.executable, "-B", "-c", timeout_code, str(pid_file)],
        env=runner_environment(directory, "runner-timeout"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_for_path(pid_file)
        pids = [int(value) for value in pid_file.read_text().split()]
        supervised.send_signal(signal.SIGTERM)
        returncode = supervised.wait(timeout=5.0)
        if returncode != 143:
            raise AssertionError(
                f"terminated runner did not preserve signal status: {returncode}"
            )
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            alive = []
            for pid in pids:
                try:
                    os.kill(pid, 0)
                except OSError as error:
                    if error.errno != errno.ESRCH:
                        alive.append(pid)
                else:
                    alive.append(pid)
            if not alive:
                break
            time.sleep(0.01)
        else:
            raise AssertionError(f"runner left supervised processes alive: {alive}")
    finally:
        if supervised.poll() is None:
            supervised.kill()
            supervised.wait(timeout=5.0)

    ignoring_ready = directory / "ignoring-ready"
    ignoring_code = (
        "import pathlib,signal,sys\n"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "pathlib.Path(sys.argv[1]).write_text('ready')\n"
        "signal.pause()\n"
    )
    ignoring = subprocess.Popen(
        [
            str(runner),
            sys.executable,
            "-B",
            "-c",
            ignoring_code,
            str(ignoring_ready),
        ],
        env=runner_environment(directory, "runner-ignore"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_for_path(ignoring_ready)
        began = time.monotonic()
        ignoring.send_signal(signal.SIGTERM)
        returncode = ignoring.wait(timeout=4.0)
        if returncode != 137 or time.monotonic() - began > 3.0:
            raise AssertionError(
                "runner did not bound an uncooperative child: "
                f"returncode={returncode}"
            )
    finally:
        if ignoring.poll() is None:
            ignoring.kill()
            ignoring.wait(timeout=5.0)


def main() -> int:
    with tempfile.TemporaryDirectory(
        prefix="a64-raw-compression-fixture-"
    ) as temporary_name:
        directory = Path(temporary_name).resolve()
        plain = directory / "fixture-producer.tsv.raw"
        write_fixture(plain, fixture_row())
        plain.chmod(0o640)
        original_rows = list(raw_rows([directory]))
        exception = {
            "ctest": "fixture-producer",
            "case": "fixture-case",
            "source_file": "tests/unit/arm64/fixture.cpp",
            "source_line": "10",
            "encoding": "d503201f",
        }
        original_exception_rows = exception_raw_rows(directory, exception)
        if len(original_exception_rows) != 1:
            raise AssertionError("plain exception raw row was not resolved")
        original_identity = uncompressed_identity(
            RawEvidenceFile("fixture-producer", plain, False)
        )

        compressed = directory / "fixture-producer.tsv.raw.gz"
        entry = compress_plain_raw(
            "fixture-producer", plain, compressed, "16384"
        )
        if (
            plain.exists()
            or not compressed.exists()
            or not entry.compressed
        ):
            raise AssertionError("plain raw evidence was not atomically replaced")
        verify_gzip(compressed)
        if (
            uncompressed_identity(entry) != original_identity
            or list(raw_rows([directory])) != original_rows
            or exception_raw_rows(directory, exception)
            != original_exception_rows
        ):
            raise AssertionError(
                "plain/compressed aggregation and exception parsing differ"
            )

        write_fixture(plain, fixture_row())
        expect_failure(
            "duplicate-plain-compressed-reader",
            lambda: raw_evidence_files(directory),
            "duplicate plain/compressed producer",
        )
        recovered = resolve_entry(
            "fixture-producer", plain, compressed, "16384"
        )
        if plain.exists() or uncompressed_identity(recovered) != original_identity:
            raise AssertionError("matching interrupted compression did not resume")

        write_fixture(plain, {**fixture_row(), "occurrences": "2"})
        replaced = resolve_entry(
            "fixture-producer", plain, compressed, "16384"
        )
        if plain.exists() or uncompressed_identity(replaced) == original_identity:
            raise AssertionError("semantic source did not replace stale gzip")

        truncated = directory / "truncated.tsv.raw.gz"
        shutil.copyfile(compressed, truncated)
        payload = truncated.read_bytes()
        truncated.write_bytes(payload[:-8])
        expect_failure(
            "truncated-compressed-raw",
            lambda: uncompressed_identity(
                RawEvidenceFile("truncated", truncated, True)
            ),
            "cannot verify raw evidence stream",
        )

        wrong_page = directory / "wrong-page.tsv.raw"
        write_fixture(
            wrong_page, {**fixture_row(), "ctest": "wrong-page"}
        )
        expect_failure(
            "wrong-page-compression",
            lambda: compress_plain_raw(
                "wrong-page",
                wrong_page,
                directory / "wrong-page.tsv.raw.gz",
                "4096",
            ),
            "raw row differs from page profile 4096",
        )
        if not wrong_page.exists():
            raise AssertionError("invalid page fixture deleted its plain source")

        overwrite_plain = directory / "overwrite.tsv.raw"
        write_fixture(
            overwrite_plain, {**fixture_row(), "ctest": "overwrite"}
        )
        overwrite_gzip = directory / "overwrite.tsv.raw.gz"
        with overwrite_gzip.open("wb") as raw_output:
            with gzip.GzipFile(mode="wb", fileobj=raw_output) as output:
                output.write(b"existing")
        overwrite_entry = compress_plain_raw(
            "overwrite", overwrite_plain, overwrite_gzip, "16384"
        )
        if overwrite_plain.exists() or uncompressed_identity(overwrite_entry)[0] == 0:
            raise AssertionError("ordinary gzip replacement did not complete")

        extra_field = directory / "extra-field.tsv.raw"
        write_fixture(
            extra_field, {**fixture_row(), "ctest": "extra-field"}
        )
        lines = extra_field.read_text(encoding="utf-8").splitlines()
        extra_field.write_text(
            lines[0] + "\n" + lines[1] + "\textra\n", encoding="utf-8"
        )
        expect_failure(
            "extra-field-row-shape",
            lambda: compress_plain_raw(
                "extra-field",
                extra_field,
                directory / "extra-field.tsv.raw.gz",
                "16384",
            ),
            "raw evidence row shape differs",
        )

        missing_field = directory / "missing-field.tsv.raw"
        write_fixture(
            missing_field, {**fixture_row(), "ctest": "missing-field"}
        )
        lines = missing_field.read_text(encoding="utf-8").splitlines()
        missing_field.write_text(
            lines[0] + "\n" + lines[1].rsplit("\t", 1)[0] + "\n",
            encoding="utf-8",
        )
        expect_failure(
            "missing-field-row-shape",
            lambda: compress_plain_raw(
                "missing-field",
                missing_field,
                directory / "missing-field.tsv.raw.gz",
                "16384",
            ),
            "raw evidence row shape differs",
        )

        if gzip_temporary_worst_case_bytes(1024) != 1024 * 2 + 1024 * 1024:
            raise AssertionError("gzip temporary bound drifted")

        expect_failure(
            "negative-reserve",
            lambda: reserve_bytes(-1.0),
            "finite and nonnegative",
        )
        expect_failure(
            "nonfinite-reserve",
            lambda: reserve_bytes(float("nan")),
            "finite and nonnegative",
        )

        if len(sys.argv) > 1:
            runner_directory = directory / "native-runner"
            runner_directory.mkdir()
            cxx_runner_fixtures(Path(sys.argv[1]), runner_directory)

    print("A64 raw evidence compression strict fixtures passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=__import__("sys").stderr)
        raise SystemExit(1)
