#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the immutable x86-64 Steam appliance with a retained GPT user-data disk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "steam-common"))
import launcher  # noqa: E402
from launcher import NEW_DISK, disk_command, launch, parser, prepare_disk  # noqa: E402,F401

# The x86-64 guest has one execution engine; it is named explicitly so both
# appliances present the same launch contract. Virtio sound plays through the
# Apple audio output.
OPTIONS = ("--execution-engine=gadgets", "--audio")


def command(emulator: Path, rootfs: Path, userdata: Path, memory: str,
            unit: str, extra: list[str]) -> list[str]:
    return launcher.command(emulator, rootfs, userdata, memory, unit, extra, OPTIONS)


def main() -> int:
    return launch(parser(__doc__).parse_args(), OPTIONS)


if __name__ == "__main__":
    raise SystemExit(main())
