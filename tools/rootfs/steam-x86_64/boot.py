#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the immutable x86-64 Steam appliance with a retained GPT user-data disk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import launcher  # noqa: E402
from launcher import launch, parser  # noqa: E402

# The x86-64 guest has one execution engine, named explicitly so the launch
# contract never depends on the emulator's default. Virtio sound plays through
# the Apple audio output.
OPTIONS = ("--execution-engine=gadgets", "--audio")


def command(emulator: Path, rootfs: Path, userdata: Path, memory: str,
            unit: str, extra: list[str]) -> list[str]:
    return launcher.command(emulator, rootfs, userdata, memory, unit, extra, OPTIONS)


def main() -> int:
    return launch(parser(__doc__).parse_args(), OPTIONS)


if __name__ == "__main__":
    raise SystemExit(main())
