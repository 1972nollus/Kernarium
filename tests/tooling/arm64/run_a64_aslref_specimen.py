#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the pinned ADD specimen through official generated A64 ASL.

The Arm archive remains an explicit input.  This runner verifies its pinned
digest, asks herdtools7's official bundler to generate the selected instruction
procedure, extracts the official AddWithCarry helper, strictly type-checks the
small user-state interface plus specimen, and executes it with ASLRef.

This proves only the named specimen.  It does not promote an allocation or
inventory entry to behavioral completeness.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET
from pathlib import Path

from a64_architecture_oracle import ARCHIVE_NAME, ARCHIVE_SHA256, SOURCE_ORACLE
from a64_bounded_generation import (
    BoundedCommandRunner,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from a64_aslref_state_digest import (
    ADAPTER_CONTRACT,
    EMPTY_MEMORY_DIGEST,
    add_specimen_digests,
)


ASLREF_VERSION = "aslref version 1.0~draft.2"
ARCHIVE_DIRECTORY = ARCHIVE_NAME.removesuffix(".tar.gz")
INSTRUCTION_XML = "add_addsub_imm.xml"
SHARED_XML = "shared_pseudocode.xml"
HELPER_NAME = "shared/functions/integer/AddWithCarry"
OUTPUT = re.compile(
    r"A64_ASLREF_ADD_X0=0x(?P<x0>[0-9a-f]{16})"
    r"A64_ASLREF_ADD_X5=0x(?P<x5>[0-9a-f]{16})"
    r"A64_ASLREF_ADD_NZCV=0x(?P<nzcv>[0-9a-f])"
)
EVIDENCE_COLUMNS = [
    "oracle_case_id",
    "stable_id",
    "architecture_encoding",
    "encoding",
    "candidate_input_partition",
    "asl_result_x0",
    "asl_preserved_x5",
    "asl_preserved_nzcv",
    "observation_boundary",
    "initial_state_digest",
    "independent_expected_state_digest",
    "terminal_suffix_expected_state_digest",
    "memory_digest",
    "strict_typecheck_status",
    "execution_status",
    "architecture_oracle",
    "aslref_oracle",
    "adapter_contract",
    "adapter_source_sha256",
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--bundler", type=Path, required=True)
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument(
        "--interface",
        type=Path,
        default=Path(__file__).with_name(
            "a64_aslref_user_state_interface.asl"
        ),
    )
    parser.add_argument(
        "--specimen",
        type=Path,
        default=Path(__file__).with_name(
            "a64_aslref_add_immediate_specimen.asl"
        ),
    )
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_required_variants.tsv"
        ),
        help="Reserved for the status join; checked for existence here.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=(
            repository / "tests/unit/arm64/a64_aslref_specimen_evidence.tsv"
        ),
    )
    parser.add_argument("--timeout-seconds", type=int, default=120)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def archive_member(archive: tarfile.TarFile, basename: str) -> tarfile.TarInfo:
    matches = [
        member
        for member in archive.getmembers()
        if member.isfile()
        and Path(member.name).parts[-2:] == (ARCHIVE_DIRECTORY, basename)
    ]
    if len(matches) != 1:
        raise ValueError(
            f"archive contains {len(matches)} regular {basename!r} members"
        )
    return matches[0]


def extract_member(
    archive: tarfile.TarFile, member: tarfile.TarInfo, output: Path
) -> None:
    source = archive.extractfile(member)
    if source is None:
        raise ValueError(f"cannot read archive member {member.name!r}")
    with source, output.open("wb") as destination:
        shutil.copyfileobj(source, destination)


def official_helper(shared_xml: Path) -> str:
    root = ET.parse(shared_xml).getroot()
    matches = [
        ps
        for ps in root.findall(".//ps")
        if ps.get("name") == HELPER_NAME
    ]
    if len(matches) != 1:
        raise ValueError(
            f"official XML contains {len(matches)} {HELPER_NAME!r} helpers"
        )
    text = "".join(matches[0].itertext()).strip()
    if "func AddWithCarry{N}" not in text:
        raise ValueError("official AddWithCarry helper has unexpected syntax")
    return text + "\n"


def checked_run(
    command: list[str], runner: BoundedCommandRunner
) -> subprocess.CompletedProcess[str]:
    return runner.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )


def main() -> int:
    options = arguments()
    options.aslref = options.aslref.resolve(strict=True)
    options.bundler = options.bundler.resolve(strict=True)
    digest_source = Path(__file__).with_name(
        "a64_aslref_state_digest.py"
    )
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.aslref,
            options.bundler,
            options.architecture_archive,
            options.interface,
            options.specimen,
            options.required_variants,
            digest_source,
        ],
    )
    for path in (
        options.aslref,
        options.bundler,
        options.architecture_archive,
        options.interface,
        options.specimen,
        options.required_variants,
    ):
        if not path.is_file():
            raise ValueError(f"required input is not a file: {path}")
    if options.architecture_archive.name != ARCHIVE_NAME:
        raise ValueError(
            f"architecture archive is {options.architecture_archive.name!r}, "
            f"expected {ARCHIVE_NAME!r}"
        )
    observed_digest = sha256(options.architecture_archive)
    if observed_digest != ARCHIVE_SHA256:
        raise ValueError(
            f"architecture archive SHA-256 is {observed_digest}, "
            f"expected {ARCHIVE_SHA256}"
        )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-aslref-specimen-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        version = checked_run(
            [str(options.aslref), "--version"], runner
        ).stdout.strip()
        if version != ASLREF_VERSION:
            raise ValueError(
                f"ASLRef version is {version!r}, expected {ASLREF_VERSION!r}"
            )

        temporary = workspace.require_path()
        instruction_xml = temporary / INSTRUCTION_XML
        shared_xml = temporary / SHARED_XML
        with tarfile.open(options.architecture_archive, "r:gz") as archive:
            instruction_member = archive_member(archive, INSTRUCTION_XML)
            shared_member = archive_member(archive, SHARED_XML)
            workspace.validate_archive_members(
                [instruction_member, shared_member]
            )
            extract_member(
                archive,
                instruction_member,
                instruction_xml,
            )
            extract_member(
                archive,
                shared_member,
                shared_xml,
            )
        workspace.check()
        root = ET.parse(instruction_xml).getroot()
        if root.get("id") != "ADD_addsub_imm":
            raise ValueError("official ADD XML has an unexpected instruction ID")

        generated = temporary / "generated"
        generated.mkdir()
        checked_run(
            [
                sys.executable,
                "-B",
                str(options.bundler),
                "--make-funs",
                "-j",
                "1",
                "-o",
                str(generated),
                str(instruction_xml),
            ],
            runner,
        )
        instructions = generated / "instructions.asl"
        if not instructions.is_file():
            raise ValueError("official bundler did not emit instructions.asl")
        helper = temporary / "add_with_carry.asl"
        workspace.write_text(helper, official_helper(shared_xml))

        execution = checked_run(
            [
                str(options.aslref),
                "--type-check-strict",
                "--exec",
                "-1",
                str(instructions),
                "-1",
                str(helper),
                "-1",
                str(options.interface),
                str(options.specimen),
            ],
            runner,
        )
        observed = execution.stdout.strip()
        match = OUTPUT.fullmatch(observed)
        if match is None:
            raise ValueError(
                f"ASLRef specimen output has an unexpected shape: {observed!r}"
            )
        values = {name: int(value, 16) for name, value in match.groupdict().items()}
        if values != {"x0": 0x2A, "x5": 0x25, "nzcv": 0xA}:
            raise ValueError(f"ASLRef specimen state is {values!r}")
        digests = add_specimen_digests(**values)
        row = {
            "oracle_case_id": "BASE-0005/ADD_64_addsub_imm",
            "stable_id": "BASE-0005",
            "architecture_encoding": "ADD_64_addsub_imm",
            "encoding": "910014a0",
            "candidate_input_partition": "x5=0000000000000025+nzcv=a0000000",
            "asl_result_x0": f"{values['x0']:016x}",
            "asl_preserved_x5": f"{values['x5']:016x}",
            "asl_preserved_nzcv": f"{values['nzcv']:x}",
            "observation_boundary": "single-instruction-pc+4",
            "initial_state_digest": digests["initial"],
            "independent_expected_state_digest": digests["instruction"],
            "terminal_suffix_expected_state_digest": digests["terminal_suffix"],
            "memory_digest": EMPTY_MEMORY_DIGEST,
            "strict_typecheck_status": "strict-pass",
            "execution_status": "executed-pass",
            "architecture_oracle": SOURCE_ORACLE,
            "aslref_oracle": ASLREF_VERSION.replace(" ", "-"),
            "adapter_contract": ADAPTER_CONTRACT,
            "adapter_source_sha256": sha256(digest_source),
        }
        publication.publish_tsvs(
            [(publication.targets[0], EVIDENCE_COLUMNS, [row])]
        )
        print(
            "A64 official ASLRef specimen passed: "
            "stable_id=BASE-0005 allocation=ADD_64_addsub_imm "
            "encoding=910014a0 x5=0000000000000025 "
            "x0=000000000000002a nzcv=a "
            f"instruction_digest={digests['instruction']} "
            f"suffix_digest={digests['terminal_suffix']} "
            f"oracle={SOURCE_ORACLE}+{ASLREF_VERSION.replace(' ', '-')} "
            f"generated_sha256={sha256(instructions)}; wrote {options.output}"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            if error.stdout:
                print(error.stdout, file=sys.stderr, end="")
            if error.stderr:
                print(error.stderr, file=sys.stderr, end="")
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
