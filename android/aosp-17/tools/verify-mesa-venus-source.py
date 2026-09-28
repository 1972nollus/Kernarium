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
from pathlib import Path

import pinned_source
from pinned_source import run as _run
# pinned_source puts scripts/ on sys.path.
import patch_series  # noqa: E402


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


def run(*command: str, cwd: Path | None = None) -> str:
    return _run(*command, cwd=cwd, error=VerificationError)


def parse_metadata(path: Path) -> dict[str, str]:
    return pinned_source.read_metadata(
        path, REQUIRED_METADATA, "Mesa Venus", VerificationError
    )


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
    pinned_source.check_object_ids(
        values,
        (
            "MESA_VENUS_UPSTREAM_COMMIT",
            "MESA_VENUS_UPSTREAM_TREE",
            "MESA_VENUS_GUEST_TREE",
            "MESA_VENUS_FINAL_TREE",
        ),
        VerificationError,
    )
    pinned_source.check_branch_name(
        values["MESA_VENUS_PATCH_BRANCH"], "Mesa Venus", VerificationError
    )


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
    return pinned_source.read_generated_series(
        root / "third_party/mesa-venus/series", "Mesa Venus", VerificationError
    )


def verify_checkout(
    aosp_source: Path,
    values: dict[str, str],
    guest_count: int,
    count: int,
) -> None:
    source = pinned_source.open_checkout(
        aosp_source, values, "MESA_VENUS", "Mesa Venus", VerificationError
    )
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
