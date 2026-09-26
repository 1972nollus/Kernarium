#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

from kernel_artifact import verify_artifact, verify_compatible_artifact


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--common-root", type=Path, required=True)
    parser.add_argument("--kernel-metadata", type=Path)
    parser.add_argument("--abi-metadata", type=Path, required=True)
    parser.add_argument("--acceptance", type=Path)
    parser.add_argument("--compatible", action="store_true")
    parser.add_argument("--expected-family")
    parser.add_argument("--expected-slice")
    parser.add_argument("--expected-release-series")
    parser.add_argument("--expected-page-size", type=int)
    args = parser.parse_args()

    if args.compatible:
        if (
            args.expected_family is None
            or args.expected_slice is None
            or args.expected_release_series is None
            or args.expected_page_size is None
            or args.kernel_metadata is not None
            or args.acceptance is not None
        ):
            parser.error(
                "--compatible requires expected family, slice, release series, "
                "and page size without tracked-source or acceptance inputs"
            )
        manifest = verify_compatible_artifact(
            args.artifact,
            args.common_root,
            args.abi_metadata,
            args.expected_family,
            args.expected_slice,
            args.expected_release_series,
            args.expected_page_size,
        )
    else:
        if args.kernel_metadata is None:
            parser.error("tracked verification requires --kernel-metadata")
        manifest = verify_artifact(
            args.artifact,
            args.common_root,
            args.kernel_metadata,
            args.abi_metadata,
            acceptance_path=args.acceptance,
        )
    print(
        f"verified {manifest['family']} {manifest['slice']} "
        f"{manifest['page_size'] // 1024} KiB kernel artifact: "
        f"{manifest['directory_name']}"
    )


if __name__ == "__main__":
    main()
