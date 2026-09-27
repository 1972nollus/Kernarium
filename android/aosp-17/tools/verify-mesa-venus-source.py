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
    "MESA_VENUS_SOURCE_SCHEMA",
    "MESA_VENUS_UPSTREAM_REPOSITORY",
    "MESA_VENUS_UPSTREAM_RELEASE",
    "MESA_VENUS_UPSTREAM_COMMIT",
    "MESA_VENUS_UPSTREAM_TREE",
    "MESA_VENUS_GUEST_EPOCH",
    "MESA_VENUS_GUEST_SERIES",
    "MESA_VENUS_GUEST_TREE",
    "MESA_VENUS_PATCH_BRANCH",
    "MESA_VENUS_FINAL_TREE",
    "MESA_VENUS_DESTINATION",
    "MESA_VENUS_HAL_MODULE",
    "MESA_VENUS_LICENSE",
}


class VerificationError(ValueError):
    pass


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
    values = parse_assignments(path, VerificationError)
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


def verify_series(root: Path) -> int:
    """Return how many Android port patches the checked-in series holds."""
    try:
        series = patch_series.read_series(root / "third_party/mesa-venus/series")
    except patch_series.SeriesError as error:
        raise VerificationError(f"Mesa Venus series: {error}") from error
    if not series.names:
        raise VerificationError("Mesa Venus series is empty")
    return len(series.names)


def verify_checkout(
    aosp_source: Path,
    values: dict[str, str],
    guest_count: int,
    count: int,
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

    history = run("git", "rev-list", "--parents", f"{guest_head}..HEAD", cwd=source)
    if any(len(row.split()) != 2 for row in history.splitlines()) or len(
        history.splitlines()
    ) != count:
        raise VerificationError("materialized Mesa Venus topic is not a clean linear series")


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
        count = verify_series(root)
        if args.aosp_source is not None:
            verify_checkout(args.aosp_source.resolve(), values, guest_count, count)
    except (OSError, UnicodeError, VerificationError) as error:
        raise SystemExit(f"verify-mesa-venus-source.py: {error}") from error

    suffix = " and materialized checkout" if args.aosp_source is not None else ""
    print(f"verified pinned Mesa Venus source series{suffix}")


if __name__ == "__main__":
    main()
