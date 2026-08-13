#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the shipped zswap and zram compressed-memory policy."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel-config", type=Path, required=True)
    arguments = parser.parse_args()
    config = arguments.kernel_config.resolve().read_text()

    for setting in (
        "CONFIG_SWAP=y",
        "CONFIG_ZSWAP=y",
        "CONFIG_ZSWAP_DEFAULT_ON=y",
        "CONFIG_ZSWAP_COMPRESSOR_DEFAULT_ZSTD=y",
        "CONFIG_ZRAM=y",
        "CONFIG_ZRAM_BACKEND_ZSTD=y",
        "CONFIG_ZRAM_DEF_COMP_ZSTD=y",
    ):
        require(
            re.search(rf"(?m)^{re.escape(setting)}$", config) is not None,
            f"compressed-memory kernel policy lacks {setting}",
        )
    require(
        re.search(
            r"(?m)^# CONFIG_ZSWAP_SHRINKER_DEFAULT_ON is not set$", config
        )
        is not None,
        "zswap shrinker policy is implicit",
    )
    selected = re.findall(
        r"(?m)^CONFIG_ZSWAP_COMPRESSOR_DEFAULT_([A-Z0-9]+)=y$", config
    )
    require(selected == ["ZSTD"], "zswap does not have exactly one zstd default")

    print("kernel compressed-memory policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
