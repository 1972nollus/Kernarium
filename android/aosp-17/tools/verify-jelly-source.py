#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the pinned external Jelly source contract and optional checkout."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
from pathlib import Path

import pinned_source
from pinned_source import run as _run


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


def run(*command: str, cwd: Path | None = None) -> str:
    return _run(*command, cwd=cwd, error=VerificationError)


def parse_metadata(path: Path) -> dict[str, str]:
    return pinned_source.read_metadata(
        path, REQUIRED_METADATA, "Jelly", VerificationError
    )


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
    pinned_source.check_object_ids(
        values,
        ("JELLY_UPSTREAM_COMMIT", "JELLY_UPSTREAM_TREE", "JELLY_FINAL_TREE"),
        VerificationError,
    )
    pinned_source.check_branch_name(
        values["JELLY_PATCH_BRANCH"], "Jelly", VerificationError
    )


def verify_series(root: Path) -> None:
    pinned_source.read_generated_series(
        root / "third_party/jelly/series", "Jelly", VerificationError
    )


def verify_checkout(aosp_source: Path, values: dict[str, str]) -> None:
    source = pinned_source.open_checkout(
        aosp_source, values, "JELLY", "Jelly", VerificationError
    )
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
