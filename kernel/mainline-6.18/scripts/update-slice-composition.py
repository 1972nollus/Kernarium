#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate x86_64 artifact provenance from shared and overlay manifests."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import sys
import tempfile


def main() -> int:
    line_root = Path(__file__).resolve().parents[1]
    outer_root = line_root.parents[1]
    common_root = outer_root / "kernel/common"
    slice_root = line_root / "slices/x86_64"
    sys.path.insert(0, str(common_root / "artifacts"))
    from kernel_artifact import (  # noqa: PLC0415
        PATCH_COMPOSITION_HEADER,
        parse_env,
        patch_provenance_count,
        sha256_path,
    )

    common_metadata = parse_env(line_root / "patches/metadata.env")
    slice_metadata = parse_env(slice_root / "patches/metadata.env")
    if slice_metadata.get("UML_KERNEL_SLICE") != "x86_64":
        raise SystemExit("x86_64 patch metadata has the wrong slice identity")
    if (
        slice_metadata.get("UML_KERNEL_PARENT_TREE")
        != common_metadata["UML_KERNEL_FINAL_TREE"]
    ):
        raise SystemExit("x86_64 patch parent tree differs from the common final tree")
    inherited_keys = (
        "UML_KERNEL_RELEASE_VERSION",
        "UML_KERNEL_UPSTREAM_REPOSITORY",
        "UML_KERNEL_UPSTREAM_BRANCH",
        "UML_KERNEL_UPSTREAM_COMMIT",
        "UML_KERNEL_UPSTREAM_TREE",
        "UML_KERNEL_BASE_REF",
        "UML_KERNEL_ABI_VERSION",
        "UML_KERNEL_ABI_FINGERPRINT",
    )
    mismatched = [
        key
        for key in inherited_keys
        if slice_metadata.get(key) != common_metadata.get(key)
    ]
    if mismatched:
        raise SystemExit(
            "x86_64 patch metadata differs from common identity: "
            + ", ".join(mismatched)
        )
    common_manifest = line_root / "patches/series/manifest.tsv"
    slice_manifest = slice_root / "patches/series/manifest.tsv"
    common_count = int(common_metadata["UML_KERNEL_PATCH_COUNT"])
    slice_count = int(slice_metadata["UML_KERNEL_PATCH_COUNT"])
    total = common_count + slice_count
    tracked_total = int(
        slice_metadata.get("UML_KERNEL_ARTIFACT_PATCH_COUNT", str(slice_count))
    )
    if tracked_total != total:
        raise SystemExit(
            "x86_64 artifact patch count "
            f"{tracked_total} does not equal common+overlay count {total}"
        )
    if patch_provenance_count(common_manifest) != common_count:
        raise SystemExit("common patch manifest count differs from metadata")
    if patch_provenance_count(slice_manifest) != slice_count:
        raise SystemExit("x86_64 patch manifest count differs from metadata")

    common_relative = common_manifest.relative_to(outer_root).as_posix()
    slice_relative = slice_manifest.relative_to(outer_root).as_posix()
    contents = "\n".join(
        (
            "# SPDX-License-Identifier: GPL-2.0-or-later",
            "# Ordered patch-series composition used by x86_64 artifacts.",
            PATCH_COMPOSITION_HEADER,
            f"1\tcommon\t{common_count}\t{common_relative}\t"
            f"{sha256_path(common_manifest)}",
            f"2\tx86_64\t{slice_count}\t{slice_relative}\t"
            f"{sha256_path(slice_manifest)}",
            "",
        )
    )
    destination = slice_root / "patches/composition.tsv"
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=".composition.",
            dir=destination.parent,
            delete=False,
        ) as output:
            descriptor = Path(output.name)
            output.write(contents)
        patch_provenance_count(
            descriptor,
            outer_root,
            (
                ("common", common_relative),
                ("x86_64", slice_relative),
            ),
        )
        os.replace(descriptor, destination)
        descriptor = None
    finally:
        if descriptor is not None:
            descriptor.unlink(missing_ok=True)
    print(f"regenerated x86_64 patch composition: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
