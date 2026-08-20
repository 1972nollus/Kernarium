#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_common import verify_release  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--acceptance", type=Path)
    parser.add_argument("--zstd", type=Path)
    parser.add_argument("--expanded-disk", type=Path)
    args = parser.parse_args()

    android_root = Path(__file__).resolve().parents[1]
    manifest = verify_release(
        args.release,
        android_root,
        acceptance_path=args.acceptance,
        zstd=args.zstd,
        expanded_disk=args.expanded_disk,
        allow_expanded_disk=args.expanded_disk is not None,
    )
    print(f"verified Android release: {manifest['directory_name']}")


if __name__ == "__main__":
    main()
