#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the pinned guest Mesa Venus source contract and optional checkout.

The Android Vulkan HAL is Mesa's Venus driver built from this project's own
guest Mesa source: the pinned upstream Mesa revision, the guest Vulkan patch
series under `tools/rootfs/vulkan/`, and the generated Android port series
beside this metadata. A guest replay assigns new commit identities every time
it is recreated, so the guest half of the contract is pinned by tree, not by
commit.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import patch_series  # noqa: E402


HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
PATCH_NAME = re.compile(r"[0-9]{4}-[A-Za-z0-9._-]+\.patch")
ASSIGNMENT = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
HEADER = ("order", "filename", "stable_patch_id", "sha256", "subject")
SUBJECT_PREFIX = "UML-ANDROID-LOCAL: Mesa Venus "
REQUIRED_METADATA = {
    "MESA_VENUS_SOURCE_SCHEMA",
    "MESA_VENUS_UPSTREAM_REPOSITORY",
    "MESA_VENUS_UPSTREAM_RELEASE",
    "MESA_VENUS_UPSTREAM_COMMIT",
    "MESA_VENUS_UPSTREAM_TREE",
    "MESA_VENUS_GUEST_EPOCH",
    "MESA_VENUS_GUEST_SERIES",
    "MESA_VENUS_GUEST_TREE",
    "MESA_VENUS_PATCH_BRANCH",
    "MESA_VENUS_PATCH_COUNT",
    "MESA_VENUS_FINAL_TREE",
    "MESA_VENUS_DESTINATION",
    "MESA_VENUS_HAL_MODULE",
    "MESA_VENUS_LICENSE",
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


def run(*command: str, cwd: Path | None = None, payload: bytes | None = None) -> str:
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
            f"Mesa Venus metadata keys differ (missing={missing}, extra={extra})"
        )
    return values


def validate_metadata(values: dict[str, str]) -> None:
    if values["MESA_VENUS_SOURCE_SCHEMA"] != "1":
        raise VerificationError("unsupported Mesa Venus source schema")
    if values["MESA_VENUS_UPSTREAM_REPOSITORY"] != (
        "https://gitlab.freedesktop.org/mesa/mesa.git"
    ):
        raise VerificationError("Mesa repository is not the reviewed upstream source")
    if values["MESA_VENUS_DESTINATION"] != "external/mesa3d-venus":
        raise VerificationError("Mesa Venus destination is not external/mesa3d-venus")
    if values["MESA_VENUS_HAL_MODULE"] != "vulkan.virtio":
        raise VerificationError("the Vulkan HAL module name changed")
    if values["MESA_VENUS_LICENSE"] != "MIT":
        raise VerificationError("Mesa Venus license metadata changed")
    if values["MESA_VENUS_GUEST_SERIES"] != "tools/rootfs/vulkan/patches/mesa":
        raise VerificationError("the guest Mesa series moved")
    for key in (
        "MESA_VENUS_UPSTREAM_COMMIT",
        "MESA_VENUS_UPSTREAM_TREE",
        "MESA_VENUS_GUEST_TREE",
        "MESA_VENUS_FINAL_TREE",
    ):
        if HEX40.fullmatch(values[key]) is None:
            raise VerificationError(f"{key} is not a full lowercase Git object ID")
    if not values["MESA_VENUS_PATCH_COUNT"].isdecimal() or int(
        values["MESA_VENUS_PATCH_COUNT"]
    ) <= 0:
        raise VerificationError("MESA_VENUS_PATCH_COUNT is not positive")
    ref = subprocess.run(
        ("git", "check-ref-format", f"refs/heads/{values['MESA_VENUS_PATCH_BRANCH']}"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ref.returncode != 0:
        raise VerificationError("Mesa Venus patch branch name is invalid")


def verify_guest_pin(repository_root: Path, values: dict[str, str]) -> int:
    """The guest Vulkan series is an input this port pins by tree, never a copy.

    Return how many guest patches lie between the upstream base and the port.
    """
    try:
        series = patch_series.read_series(
            repository_root / values["MESA_VENUS_GUEST_SERIES"]
        )
    except patch_series.SeriesError as error:
        raise VerificationError(f"guest Mesa series: {error}") from error
    if series.tree != values["MESA_VENUS_GUEST_TREE"] or not series.names:
        raise VerificationError(
            "the pinned guest Mesa series changed; bump this port's guest epoch, "
            "replay the port, and regenerate its series"
        )
    return len(series.names)


def mail_subject(payload: bytes) -> str:
    output = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
    subjects = [
        line.removeprefix("Subject: ")
        for line in output.splitlines()
        if line.startswith("Subject: ")
    ]
    if len(subjects) != 1:
        raise VerificationError("generated Mesa Venus patch has no unique mail subject")
    return subjects[0]


def verify_series(root: Path, values: dict[str, str]) -> tuple[PatchEntry, ...]:
    series = root / "third_party/mesa-venus/series"
    manifest = series / "manifest.tsv"
    if not manifest.is_file():
        raise VerificationError("Mesa Venus series manifest is missing")
    rows = [
        line
        for line in manifest.read_text().splitlines()
        if line and not line.startswith("#")
    ]
    if not rows or tuple(rows[0].split("\t")) != HEADER:
        raise VerificationError("Mesa Venus series manifest has the wrong header")

    entries: list[PatchEntry] = []
    seen: set[str] = set()
    for expected_order, row in enumerate(rows[1:], 1):
        fields = row.split("\t")
        if len(fields) != len(HEADER):
            raise VerificationError("Mesa Venus series manifest has a malformed row")
        order_text, filename, patch_id, digest, subject = fields
        if order_text != str(expected_order):
            raise VerificationError("Mesa Venus patch order is not contiguous")
        if PATCH_NAME.fullmatch(filename) is None or filename in seen:
            raise VerificationError(
                f"unsafe or duplicate Mesa Venus patch name: {filename}"
            )
        if HEX40.fullmatch(patch_id) is None or HEX64.fullmatch(digest) is None:
            raise VerificationError(f"invalid Mesa Venus patch identity: {filename}")
        if not subject.startswith(SUBJECT_PREFIX):
            raise VerificationError(
                f"Mesa Venus patch lacks the local subject prefix: {filename}"
            )
        patch = series / filename
        if patch.is_symlink() or not patch.is_file():
            raise VerificationError(
                f"Mesa Venus patch is missing or a symlink: {filename}"
            )
        payload = patch.read_bytes()
        if hashlib.sha256(payload).hexdigest() != digest:
            raise VerificationError(f"Mesa Venus patch hash changed: {filename}")
        ids = [
            line.split()[0]
            for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
            if line.strip()
        ]
        if ids != [patch_id]:
            raise VerificationError(f"Mesa Venus stable patch ID changed: {filename}")
        if mail_subject(payload) != subject:
            raise VerificationError(f"Mesa Venus patch subject changed: {filename}")
        first_line = payload.splitlines()[0].decode(errors="strict")
        match = re.fullmatch(
            r"From ([0-9a-f]{40}) Mon Sep 17 00:00:00 2001", first_line
        )
        if match is None:
            raise VerificationError(
                f"Mesa Venus patch is not git-format-patch output: {filename}"
            )
        if (
            re.search(
                rb"^index [0-9a-f]{40}\.\.[0-9a-f]{40}(?: [0-7]{6})?$",
                payload,
                re.MULTILINE,
            )
            is None
        ):
            raise VerificationError(
                f"Mesa Venus patch lacks full-index identity: {filename}"
            )
        seen.add(filename)
        entries.append(
            PatchEntry(
                expected_order, filename, patch_id, digest, subject, match.group(1)
            )
        )

    if len(entries) != int(values["MESA_VENUS_PATCH_COUNT"]):
        raise VerificationError("Mesa Venus patch count differs from metadata")
    inventory = {path.name for path in series.glob("*.patch")}
    if inventory != seen:
        raise VerificationError(
            "Mesa Venus patch inventory differs from the series manifest"
        )
    return tuple(entries)


def verify_checkout(
    aosp_source: Path,
    values: dict[str, str],
    guest_count: int,
    entries: tuple[PatchEntry, ...],
) -> None:
    if not aosp_source.is_dir() or not (aosp_source / ".repo").is_dir():
        raise VerificationError("AOSP source is not a Repo workspace")
    source = aosp_source / values["MESA_VENUS_DESTINATION"]
    if source.is_symlink() or not source.is_dir():
        raise VerificationError(f"materialized Mesa Venus checkout is missing: {source}")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=source)).resolve()
    if top != source.resolve():
        raise VerificationError("Mesa Venus destination is not the Git checkout root")
    if run("git", "status", "--porcelain", "--untracked-files=all", cwd=source):
        raise VerificationError("materialized Mesa Venus checkout is dirty")
    if run("git", "remote", "get-url", "origin", cwd=source) != values[
        "MESA_VENUS_UPSTREAM_REPOSITORY"
    ]:
        raise VerificationError("materialized Mesa Venus origin differs from metadata")
    if run("git", "branch", "--show-current", cwd=source) != values[
        "MESA_VENUS_PATCH_BRANCH"
    ]:
        raise VerificationError(
            "the materialized Mesa Venus checkout is the maintained topic branch "
            "and must be on MESA_VENUS_PATCH_BRANCH"
        )
    if run("git", "rev-parse", "HEAD^{tree}", cwd=source) != values[
        "MESA_VENUS_FINAL_TREE"
    ]:
        raise VerificationError("materialized Mesa Venus final tree differs")

    count = len(entries)
    guest_head = run("git", "rev-parse", f"HEAD~{count}", cwd=source)
    if run("git", "rev-parse", f"{guest_head}^{{tree}}", cwd=source) != values[
        "MESA_VENUS_GUEST_TREE"
    ]:
        raise VerificationError(
            "the replayed guest Mesa tree below the Android port differs"
        )
    upstream = run(
        "git",
        "rev-parse",
        f"HEAD~{count + guest_count}",
        cwd=source,
    )
    if upstream != values["MESA_VENUS_UPSTREAM_COMMIT"]:
        raise VerificationError("the pinned upstream Mesa commit is not the port base")
    if run("git", "rev-parse", f"{upstream}^{{tree}}", cwd=source) != values[
        "MESA_VENUS_UPSTREAM_TREE"
    ]:
        raise VerificationError("the pinned upstream Mesa tree differs")

    history = run(
        "git", "rev-list", "--reverse", "--parents", f"{guest_head}..HEAD", cwd=source
    ).splitlines()
    if len(history) != count:
        raise VerificationError(
            "materialized Mesa Venus history has the wrong patch count"
        )
    parent = guest_head
    for row, entry in zip(history, entries, strict=True):
        fields = row.split()
        if len(fields) != 2 or fields[1] != parent:
            raise VerificationError(
                "materialized Mesa Venus topic is not a clean linear series"
            )
        if run("git", "show", "-s", "--format=%s", fields[0], cwd=source) != entry.subject:
            raise VerificationError("materialized Mesa Venus commit subject differs")
        parent = fields[0]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify pinned Mesa Venus metadata, generated patches, and checkout"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    args = parser.parse_args()

    try:
        root = args.android_root.resolve()
        metadata_path = root / "third_party/mesa-venus/metadata.env"
        if not metadata_path.is_file():
            raise VerificationError("Mesa Venus source metadata is missing")
        values = parse_metadata(metadata_path)
        validate_metadata(values)
        guest_count = verify_guest_pin(root.parents[1], values)
        entries = verify_series(root, values)
        if args.aosp_source is not None:
            verify_checkout(args.aosp_source.resolve(), values, guest_count, entries)
    except (OSError, UnicodeError, VerificationError) as error:
        raise SystemExit(f"verify-mesa-venus-source.py: {error}") from error

    suffix = " and materialized checkout" if args.aosp_source is not None else ""
    print(f"verified pinned Mesa Venus source series{suffix}")


if __name__ == "__main__":
    main()
