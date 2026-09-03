#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Enable the appliance-owned native ARM64 xtajit module in pinned Wine."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    contents = path.read_text(encoding="utf-8")
    count = contents.count(old)
    if count != 1:
        raise RuntimeError(
            f"expected one pinned-Wine anchor in {path}, found {count}: {old!r}"
        )
    path.write_text(contents.replace(old, new), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    options = parser.parse_args()
    source = options.source.resolve(strict=True)

    configure_ac = source / "configure.ac"
    configure = source / "configure"
    replace_once(
        configure_ac,
        "    enable_wow64cpu=${enable_wow64cpu:-x86_64}\n"
        "    enable_xtajit64=${enable_xtajit64:-arm64ec}\n",
        "    enable_wow64cpu=${enable_wow64cpu:-x86_64}\n"
        "    enable_xtajit=${enable_xtajit:-aarch64}\n"
        "    enable_xtajit64=${enable_xtajit64:-arm64ec}\n",
    )
    replace_once(
        configure_ac,
        "    enable_wow64cpu=${enable_wow64cpu:-no}\n"
        "    enable_xtajit64=${enable_xtajit64:-no}\n",
        "    enable_wow64cpu=${enable_wow64cpu:-no}\n"
        "    enable_xtajit=${enable_xtajit:-no}\n"
        "    enable_xtajit64=${enable_xtajit64:-no}\n",
    )
    replace_once(
        configure_ac,
        "WINE_CONFIG_MAKEFILE(dlls/xtajit64)\n",
        "WINE_CONFIG_MAKEFILE(dlls/xtajit)\n"
        "WINE_CONFIG_MAKEFILE(dlls/xtajit64)\n",
    )

    replace_once(
        configure,
        "enable_xtajit64\nenable_fonts\n",
        "enable_xtajit64\nenable_xtajit\nenable_fonts\n",
    )
    replace_once(
        configure,
        "    enable_wow64cpu=${enable_wow64cpu:-x86_64}\n"
        "    enable_xtajit64=${enable_xtajit64:-arm64ec}\n",
        "    enable_wow64cpu=${enable_wow64cpu:-x86_64}\n"
        "    enable_xtajit=${enable_xtajit:-aarch64}\n"
        "    enable_xtajit64=${enable_xtajit64:-arm64ec}\n",
    )
    replace_once(
        configure,
        "    enable_wow64cpu=${enable_wow64cpu:-no}\n"
        "    enable_xtajit64=${enable_xtajit64:-no}\n",
        "    enable_wow64cpu=${enable_wow64cpu:-no}\n"
        "    enable_xtajit=${enable_xtajit:-no}\n"
        "    enable_xtajit64=${enable_xtajit64:-no}\n",
    )
    replace_once(
        configure,
        "wine_fn_config_makefile dlls/xtajit64 enable_xtajit64\n",
        "wine_fn_config_makefile dlls/xtajit enable_xtajit\n"
        "wine_fn_config_makefile dlls/xtajit64 enable_xtajit64\n",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        raise SystemExit(f"{Path(__file__).name}: {error}") from error
