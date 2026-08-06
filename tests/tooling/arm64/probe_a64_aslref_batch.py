#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Probe one full pinned A64 ASLRef composition and record its exact limit."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

from a64_architecture_oracle import ARCHIVE_SHA256, SOURCE_ORACLE
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from a64_system_register_oracle import (
    ARCHIVE_SHA256 as SYSREG_SHA256,
    SOURCE_ORACLE as SYSREG_ORACLE,
    SystemRegisterCatalog,
)
from generate_a64_instruction_allocations import archive_root
from run_a64_aslref_specimen import ASLREF_VERSION


FEATURES_SHA256 = "6d5c19e70492db0edb7a0a51efddf78573a311289771097cc8ad6e2000129b58"
SUPPORT_DIGESTS = {
    "bundler.py": "1200a9d458af57575857a0e32574d30a9e6f933271a67035753f7da87b209762",
    "build_features.py": "82209a316735cc8610dd18d2a1bea2e548b37d2db37437baf00af98660408f26",
    "type-checking-implementations.asl": "d264028baf1bfdb5e6f0f3e90b9d3b591351df545d0f6cf5cbb1ef386b6102a4",
    "trbmpam_el1.asl": "10ccad1e67974181806bc2cb776bd851a1eee316420616a1f04d2dbf498ab9bd",
}
COLUMNS = ["component", "status", "detail", "sha256", "oracle"]
OPTIONAL_FIELDS = (
    "CONTEXTIDR,EDVIDSR,MPAM0_EL1,MPAM1_EL1,MPAM2_EL2,MPAM3_EL3,"
    "MPAMHCR_EL2,MPAMSM_EL1,PAR_EL1,PMCNTENSET_EL0,PMINTENSET_EL1,"
    "PMCR_EL0,TCR2_EL2,TCR2MASK_EL2,TTBCR,TTBR0,TTBR0_EL1,TTBR0_EL2,"
    "TTBR0_EL3,TTBR1,TTBR1_EL1,TTBR1_EL2,VSESR_EL2,VSTTBR_EL2,VTTBR_EL2"
)
MOVING_FIELDS = "CPTR_EL2,CPTRMASK_EL2,TCR_EL2,TCRMASK_EL2"
INCLUDED_FIELDS = "PAR,RGSR_EL1,TRBDEVID1,MPAMIDR_EL1"
NO_VARIABLES = "BRBTGT<n>_EL1,BRBSRC<n>_EL1,BRBINF<n>_EL1,SVCR"
IGNORED_REGS = "ERRGSR<m>,CNTVOFF<n>,TRBMPAM_EL1"


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--system-register-archive", type=Path, required=True)
    parser.add_argument("--features-json", type=Path, required=True)
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--support-directory", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_batch_status.tsv",
    )
    parser.add_argument("--timeout-seconds", type=int, default=600)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_sha256(path: Path, replacements: dict[str, str]) -> str:
    digest = hashlib.sha256()
    with path.open(encoding="utf-8") as source:
        for line in source:
            for original, replacement in replacements.items():
                line = line.replace(original, replacement)
            digest.update(line.encode())
    return digest.hexdigest()


def safe_extract(
    archive_path: Path,
    destination: Path,
    workspace: BoundedWorkspace,
) -> Path:
    expected = "SysReg_xml_A_profile_FAT-2026-06"
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"unsafe system-register member {member.name!r}")
            if member.issym() or member.islnk():
                raise ValueError("system-register archive links are not accepted")
        workspace.validate_archive_members(members)
        archive.extractall(destination, filter="data")
    workspace.check()
    root = destination / expected
    if not root.is_dir():
        raise ValueError(f"system-register archive lacks {expected}")
    return root


def checked_support(directory: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for name, expected in SUPPORT_DIGESTS.items():
        path = directory / name
        if not path.is_file() or sha256(path) != expected:
            raise ValueError(f"ASLRef support file is missing or unpinned: {path}")
        result[name] = path
    return result


def run(
    command: list[str], runner: BoundedCommandRunner
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["PYTHONHASHSEED"] = "0"
    return runner.run(
        command,
        capture_output=True,
        text=True,
        timeout=min(600, runner.timeout_seconds),
        env=environment,
    )


def require_generated(result: subprocess.CompletedProcess[str], path: Path) -> None:
    if result.returncode != 0 or not path.is_file():
        raise ValueError(
            f"ASL generation failed for {path.name} "
            f"(exit={result.returncode}, output={path.is_file()}): "
            f"{(result.stderr or result.stdout).strip()[:1000]}"
        )


def main() -> int:
    options = arguments()
    options.aslref = options.aslref.resolve(strict=True)
    support_inputs = [
        options.support_directory / name for name in SUPPORT_DIGESTS
    ]
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.architecture_archive,
            options.system_register_archive,
            options.features_json,
            options.aslref,
            *support_inputs,
        ],
    )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-aslref-batch-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        support = checked_support(options.support_directory)
        if sha256(options.architecture_archive) != ARCHIVE_SHA256:
            raise ValueError("architecture archive digest drift")
        SystemRegisterCatalog(options.system_register_archive)
        if sha256(options.system_register_archive) != SYSREG_SHA256:
            raise ValueError("system-register archive digest drift")
        if options.features_json.name != "Features.json" or sha256(
            options.features_json
        ) != FEATURES_SHA256:
            raise ValueError("features JSON name/digest drift")
        metadata = json.loads(options.features_json.read_text(encoding="utf-8"))[
            "_meta"
        ]["version"]
        if metadata.get("ref") != "2026-06_rel" or metadata.get("build") != "818":
            raise ValueError("features JSON release metadata drift")
        version = run([str(options.aslref), "--version"], runner)
        if version.returncode != 0 or version.stdout.strip() != ASLREF_VERSION:
            raise ValueError("ASLRef version drift")

        root = workspace.require_path()
        isa = archive_root(
            options.architecture_archive,
            root / "isa",
            workspace,
        )
        sysregs = safe_extract(
            options.system_register_archive,
            root / "sysregs",
            workspace,
        )
        output = root / "generated"
        output.mkdir()
        instructions = output / "instructions.asl"
        shared = output / "shared_pseudocode.asl"
        registers = output / "system-registers.asl"
        features = output / "features.asl"
        pmu = output / "pmu_event_types.asl"

        result = run(
            [
                sys.executable, "-B", str(support["bundler.py"]),
                "--make-funs", "-j", "1", "-o", str(output), str(isa),
            ],
            runner,
        )
        require_generated(result, instructions)
        result = run(
            [
                sys.executable, "-B", str(support["bundler.py"]),
                "--make-shared-pseudocode", "-j", "1", "-o", str(output),
                str(isa / "shared_pseudocode.xml"),
            ],
            runner,
        )
        require_generated(result, shared)
        result = run(
            [
                sys.executable, "-B", str(support["build_features.py"]),
                "-o", str(features), str(options.features_json),
            ],
            runner,
        )
        require_generated(result, features)
        register_result = run(
            [
                sys.executable, "-B", str(support["bundler.py"]),
                "--make-regs", "-j", "1", "-o", str(output),
                "--allow-optional-fields-for", OPTIONAL_FIELDS,
                "--allow-moving-fields-for", MOVING_FIELDS,
                "--allow-included-fields-for", INCLUDED_FIELDS,
                "--do-not-generate-variables-for", NO_VARIABLES,
                "--ignored-regs", IGNORED_REGS, str(sysregs),
            ],
            runner,
        )
        require_generated(register_result, registers)
        names = sorted(set(re.findall(r"\bPMU_\w+", shared.read_text())))
        workspace.write_text(
            pmu,
            "".join(f"constant {name} = Zeros{{16}};\n" for name in names),
        )
        typecheck = run(
            [
                str(options.aslref), "--type-check-strict", "--no-exec",
                "-1", str(features), "-1", str(registers),
                "-1", str(instructions), "-1", str(shared),
                "-1", str(support["type-checking-implementations.asl"]),
                "-1", str(pmu), "-1", str(support["trbmpam_el1.asl"]),
            ],
            runner,
        )
        diagnostic = (typecheck.stderr + typecheck.stdout).replace(
            str(root), "<temporary>"
        )
        if typecheck.returncode == 0:
            composition_status = "composition-typecheck-pass-nonbehavioral"
            detail = (
                "uses arbitrary type-checking implementations; supplies no "
                "normative expected state"
            )
        elif "var _ERRIRQCR<m>: ERRIRQCR_Type;" in diagnostic and (
            "ASL Grammar error: Cannot parse" in diagnostic
        ):
            composition_status = "parse-failed"
            detail = (
                "system-registers.asl:ERRIRQCR<m>: unsupported indexed "
                "register declaration"
            )
        else:
            raise ValueError(
                "unexpected full-batch ASLRef failure: "
                + diagnostic.strip().replace("\n", " ")[:600]
            )

        rows = [
            {
                "component": "features",
                "status": "generated",
                "detail": "pinned 2026-06 feature enumeration",
                "sha256": sha256(features),
                "oracle": "arm-a-profile-features-2026-06-json-build-818",
            },
            {
                "component": "system-registers",
                "status": "generated-with-upstream-diagnostics",
                "detail": "official ASLRef v1.0~draft.2 extraction policy",
                "sha256": canonical_sha256(
                    registers, {str(sysregs): "<system-register-root>"}
                ),
                "oracle": SYSREG_ORACLE,
            },
            {
                "component": "instructions+shared",
                "status": "generated",
                "detail": "all instruction procedures and shared pseudocode",
                "sha256": hashlib.sha256(
                    (
                        canonical_sha256(
                            instructions, {str(isa): "<instruction-root>"}
                        )
                        + canonical_sha256(
                            shared, {str(isa): "<instruction-root>"}
                        )
                    ).encode()
                ).hexdigest(),
                "oracle": SOURCE_ORACLE,
            },
            {
                "component": "full-composition",
                "status": composition_status,
                "detail": detail,
                "sha256": hashlib.sha256(
                    (composition_status + "\0" + detail).encode()
                ).hexdigest(),
                "oracle": ASLREF_VERSION.replace(" ", "-"),
            },
        ]
    publication.publish_tsvs([(publication.targets[0], COLUMNS, rows)])
    print(
        "A64 ASLRef full-batch probe: "
        f"{composition_status}; {detail}; wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
