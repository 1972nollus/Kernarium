#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the pinned external Jelly source contract and optional checkout."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path


HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
PATCH_NAME = re.compile(r"[0-9]{4}-[A-Za-z0-9._-]+\.patch")
ASSIGNMENT = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
HEADER = ("order", "filename", "stable_patch_id", "sha256", "subject")
REQUIRED_METADATA = {
    "JELLY_SOURCE_SCHEMA",
    "JELLY_UPSTREAM_REPOSITORY",
    "JELLY_UPSTREAM_BRANCH",
    "JELLY_UPSTREAM_COMMIT",
    "JELLY_UPSTREAM_TREE",
    "JELLY_PATCH_BRANCH",
    "JELLY_FINAL_COMMIT",
    "JELLY_FINAL_TREE",
    "JELLY_PATCH_COUNT",
    "JELLY_DESTINATION",
    "JELLY_LICENSE",
}


class VerificationError(ValueError):
    pass


@dataclass(frozen=True)
class PatchEntry:
    order: int
    filename: str
    patch_id: str
    digest: str
    subject: str
    commit: str


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
    values: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ASSIGNMENT.fullmatch(line)
        if match is None:
            raise VerificationError(
                f"{path}:{line_number}: malformed metadata assignment"
            )
        key, quoted, numeric = match.groups()
        if key in values:
            raise VerificationError(f"{path}:{line_number}: duplicate key {key}")
        values[key] = quoted if quoted is not None else numeric
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
        "JELLY_FINAL_COMMIT",
        "JELLY_FINAL_TREE",
    ):
        if HEX40.fullmatch(values[key]) is None:
            raise VerificationError(f"{key} is not a full lowercase Git object ID")
    count = values["JELLY_PATCH_COUNT"]
    if not count.isdecimal() or int(count) <= 0:
        raise VerificationError("JELLY_PATCH_COUNT is not positive")
    ref = subprocess.run(
        ("git", "check-ref-format", f"refs/heads/{values['JELLY_PATCH_BRANCH']}"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ref.returncode != 0:
        raise VerificationError("Jelly patch branch name is invalid")


def mail_subject(payload: bytes) -> str:
    output = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
    subjects = [
        line.removeprefix("Subject: ")
        for line in output.splitlines()
        if line.startswith("Subject: ")
    ]
    if len(subjects) != 1:
        raise VerificationError("generated Jelly patch has no unique mail subject")
    return subjects[0]


def verify_series(root: Path, values: dict[str, str]) -> tuple[PatchEntry, ...]:
    series = root / "third_party/jelly/series"
    manifest = series / "manifest.tsv"
    if not manifest.is_file():
        raise VerificationError("Jelly series manifest is missing")
    rows = [
        line
        for line in manifest.read_text().splitlines()
        if line and not line.startswith("#")
    ]
    if not rows or tuple(rows[0].split("\t")) != HEADER:
        raise VerificationError("Jelly series manifest has the wrong header")

    entries: list[PatchEntry] = []
    seen: set[str] = set()
    for expected_order, row in enumerate(rows[1:], 1):
        fields = row.split("\t")
        if len(fields) != len(HEADER):
            raise VerificationError("Jelly series manifest has a malformed row")
        order_text, filename, patch_id, digest, subject = fields
        if order_text != str(expected_order):
            raise VerificationError("Jelly patch order is not contiguous")
        if PATCH_NAME.fullmatch(filename) is None or filename in seen:
            raise VerificationError(f"unsafe or duplicate Jelly patch name: {filename}")
        if HEX40.fullmatch(patch_id) is None or HEX64.fullmatch(digest) is None:
            raise VerificationError(f"invalid Jelly patch identity: {filename}")
        if not subject.startswith("UML-ANDROID-LOCAL: Jelly "):
            raise VerificationError(f"Jelly patch lacks the local subject prefix: {filename}")
        patch = series / filename
        if patch.is_symlink() or not patch.is_file():
            raise VerificationError(f"Jelly patch is missing or a symlink: {filename}")
        payload = patch.read_bytes()
        if hashlib.sha256(payload).hexdigest() != digest:
            raise VerificationError(f"Jelly patch hash changed: {filename}")
        ids = [
            line.split()[0]
            for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
            if line.strip()
        ]
        if ids != [patch_id]:
            raise VerificationError(f"Jelly stable patch ID changed: {filename}")
        if mail_subject(payload) != subject:
            raise VerificationError(f"Jelly patch subject changed: {filename}")
        first_line = payload.splitlines()[0].decode(errors="strict")
        match = re.fullmatch(
            r"From ([0-9a-f]{40}) Mon Sep 17 00:00:00 2001", first_line
        )
        if match is None:
            raise VerificationError(f"Jelly patch is not git-format-patch output: {filename}")
        if re.search(
            rb"^index [0-9a-f]{40}\.\.[0-9a-f]{40}(?: [0-7]{6})?$",
            payload,
            re.MULTILINE,
        ) is None:
            raise VerificationError(f"Jelly patch lacks full-index identity: {filename}")
        seen.add(filename)
        entries.append(
            PatchEntry(expected_order, filename, patch_id, digest, subject, match.group(1))
        )

    if len(entries) != int(values["JELLY_PATCH_COUNT"]):
        raise VerificationError("Jelly patch count differs from metadata")
    inventory = {path.name for path in series.glob("*.patch")}
    if inventory != seen:
        raise VerificationError("Jelly patch inventory differs from the series manifest")
    if entries[-1].commit != values["JELLY_FINAL_COMMIT"]:
        raise VerificationError("Jelly final commit differs from the generated series")
    return tuple(entries)


def verify_checkout(
    aosp_source: Path, values: dict[str, str], entries: tuple[PatchEntry, ...]
) -> None:
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
    if run("git", "rev-parse", "HEAD", cwd=source) != values["JELLY_FINAL_COMMIT"]:
        raise VerificationError("materialized Jelly final commit differs")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=source) != values[
        "JELLY_FINAL_TREE"
    ]:
        raise VerificationError("materialized Jelly final tree differs")

    history = run(
        "git",
        "rev-list",
        "--reverse",
        "--parents",
        f"{values['JELLY_UPSTREAM_COMMIT']}..HEAD",
        cwd=source,
    ).splitlines()
    if len(history) != len(entries):
        raise VerificationError("materialized Jelly history has the wrong patch count")
    parent = values["JELLY_UPSTREAM_COMMIT"]
    for row, entry in zip(history, entries, strict=True):
        fields = row.split()
        if fields != [entry.commit, parent]:
            raise VerificationError("materialized Jelly topic is not the exact linear series")
        if run("git", "show", "-s", "--format=%s", entry.commit, cwd=source) != entry.subject:
            raise VerificationError("materialized Jelly commit subject differs")
        parent = entry.commit


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
        entries = verify_series(root, values)
        if args.aosp_source is not None:
            verify_checkout(args.aosp_source.resolve(), values, entries)
    except (OSError, UnicodeError, VerificationError) as error:
        raise SystemExit(f"verify-jelly-source.py: {error}") from error

    suffix = " and materialized checkout" if args.aosp_source is not None else ""
    print(f"verified pinned Jelly source series{suffix}")


if __name__ == "__main__":
    main()
