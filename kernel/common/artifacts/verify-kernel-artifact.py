#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

from kernel_artifact import verify_artifact


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--common-root", type=Path, required=True)
    parser.add_argument("--kernel-metadata", type=Path, required=True)
    parser.add_argument("--patch-manifest", type=Path, required=True)
    parser.add_argument("--abi-metadata", type=Path, required=True)
    parser.add_argument("--acceptance", type=Path)
    args = parser.parse_args()

    manifest = verify_artifact(
        args.artifact,
        args.common_root,
        args.kernel_metadata,
        args.patch_manifest,
        args.abi_metadata,
        acceptance_path=args.acceptance,
    )
    print(
        f"verified {manifest['family']} {manifest['page_size'] // 1024} KiB "
        f"kernel artifact: {manifest['directory_name']}"
    )


if __name__ == "__main__":
    main()
