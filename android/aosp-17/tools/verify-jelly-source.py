#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the pinned external Jelly source contract and optional checkout."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts"))
from contract_checks import parse_assignments  # noqa: E402
import patch_series  # noqa: E402


HEX40 = re.compile(r"[0-9a-f]{40}")
REQUIRED_METADATA = {
    "JELLY_SOURCE_SCHEMA",
    "JELLY_UPSTREAM_REPOSITORY",
    "JELLY_UPSTREAM_BRANCH",
    "JELLY_UPSTREAM_COMMIT",
    "JELLY_UPSTREAM_TREE",
    "JELLY_PATCH_BRANCH",
    "JELLY_FINAL_TREE",
    "JELLY_DESTINATION",
    "JELLY_LICENSE",
}


class VerificationError(ValueError):
    pass


def run(
    *command: str, cwd: Path | None = None, payload: bytes | None = None
) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        raise VerificationError(f"command failed: {' '.join(command)}: {detail}")
    return result.stdout.decode(errors="strict").strip()


def parse_metadata(path: Path) -> dict[str, str]:
    values = parse_assignments(path, VerificationError)
    if set(values) != REQUIRED_METADATA:
        missing = sorted(REQUIRED_METADATA - set(values))
        extra = sorted(set(values) - REQUIRED_METADATA)
        raise VerificationError(
            f"Jelly metadata keys differ (missing={missing}, extra={extra})"
        )
    return values


def validate_metadata(values: dict[str, str]) -> None:
    if values["JELLY_SOURCE_SCHEMA"] != "1":
        raise VerificationError("unsupported Jelly source schema")
    if values["JELLY_UPSTREAM_REPOSITORY"] != (
        "https://github.com/LineageOS/android_packages_apps_Jelly"
    ):
        raise VerificationError("Jelly repository is not the reviewed LineageOS source")
    if values["JELLY_UPSTREAM_BRANCH"] != "lineage-23.2":
        raise VerificationError("Jelly upstream branch is not lineage-23.2")
    if values["JELLY_DESTINATION"] != "packages/apps/Jelly":
        raise VerificationError("Jelly destination is not packages/apps/Jelly")
    if values["JELLY_LICENSE"] != "Apache-2.0":
        raise VerificationError("Jelly license metadata changed")
    for key in (
        "JELLY_UPSTREAM_COMMIT",
        "JELLY_UPSTREAM_TREE",
        "JELLY_FINAL_TREE",
    ):
        if HEX40.fullmatch(values[key]) is None:
            raise VerificationError(f"{key} is not a full lowercase Git object ID")
    ref = subprocess.run(
        ("git", "check-ref-format", f"refs/heads/{values['JELLY_PATCH_BRANCH']}"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ref.returncode != 0:
        raise VerificationError("Jelly patch branch name is invalid")


def verify_series(root: Path) -> None:
    try:
        series = patch_series.read_series(root / "third_party/jelly/series")
    except patch_series.SeriesError as error:
        raise VerificationError(f"Jelly series: {error}") from error
    if not series.names:
        raise VerificationError("Jelly series is empty")


def verify_checkout(aosp_source: Path, values: dict[str, str]) -> None:
    if not aosp_source.is_dir() or not (aosp_source / ".repo").is_dir():
        raise VerificationError("AOSP source is not a Repo workspace")
    source = aosp_source / values["JELLY_DESTINATION"]
    if source.is_symlink() or not source.is_dir():
        raise VerificationError(f"materialized Jelly checkout is missing: {source}")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=source)).resolve()
    if top != source.resolve():
        raise VerificationError("Jelly destination is not the Git checkout root")
    if run("git", "status", "--porcelain", "--untracked-files=all", cwd=source):
        raise VerificationError("materialized Jelly checkout is dirty")
    if run("git", "remote", "get-url", "origin", cwd=source) != values[
        "JELLY_UPSTREAM_REPOSITORY"
    ]:
        raise VerificationError("materialized Jelly origin differs from metadata")
    branch = subprocess.run(
        ("git", "symbolic-ref", "--quiet", "HEAD"),
        cwd=source,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if branch.returncode == 0:
        raise VerificationError("materialized Jelly checkout must have detached HEAD")
    if run("git", "rev-parse", f"{values['JELLY_UPSTREAM_COMMIT']}^{{commit}}", cwd=source) != values[
        "JELLY_UPSTREAM_COMMIT"
    ]:
        raise VerificationError("materialized Jelly upstream commit is absent")
    if run("git", "rev-parse", f"{values['JELLY_UPSTREAM_COMMIT']}^{{tree}}", cwd=source) != values[
        "JELLY_UPSTREAM_TREE"
    ]:
        raise VerificationError("materialized Jelly upstream tree differs")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=source) != values[
        "JELLY_FINAL_TREE"
    ]:
        raise VerificationError("materialized Jelly final tree differs")
    ancestor = subprocess.run(
        ("git", "merge-base", "--is-ancestor", values["JELLY_UPSTREAM_COMMIT"], "HEAD"),
        cwd=source,
    )
    if ancestor.returncode != 0:
        raise VerificationError("materialized Jelly topic is not based on its upstream commit")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify pinned Jelly metadata, generated patches, and checkout"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    args = parser.parse_args()

    try:
        root = args.android_root.resolve()
        metadata_path = root / "third_party/jelly/metadata.env"
        if not metadata_path.is_file():
            raise VerificationError("Jelly source metadata is missing")
        values = parse_metadata(metadata_path)
        validate_metadata(values)
        verify_series(root)
        if args.aosp_source is not None:
            verify_checkout(args.aosp_source.resolve(), values)
    except (OSError, UnicodeError, VerificationError) as error:
        raise SystemExit(f"verify-jelly-source.py: {error}") from error

    suffix = " and materialized checkout" if args.aosp_source is not None else ""
    print(f"verified pinned Jelly source series{suffix}")


if __name__ == "__main__":
    main()
