#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned LLVM AArch64 decoder access shared by evidence tooling."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import shutil
import subprocess
from pathlib import Path
from typing import Callable


PINNED_LLVM_VERSION = "22.1.8"
MC_OPCODE = re.compile(r"<MCInst #[0-9]+ ([^\s>]+)")
INVALID_LINE = re.compile(
    r"<stdin>:(\d+):\d+: warning: invalid instruction encoding"
)
ALLOWED_WARNING = re.compile(
    r"<stdin>:\d+:\d+: warning: "
    r"(?:invalid|potentially undefined) instruction encoding"
)


def find_llvm_mc(explicit: Path | None = None) -> Path:
    if explicit is not None:
        if not explicit.is_file():
            raise ValueError(f"LLVM decoder does not exist: {explicit}")
        return explicit
    candidate = shutil.which("llvm-mc")
    if candidate is not None:
        return Path(candidate)
    homebrew = Path("/opt/homebrew/opt/llvm/bin/llvm-mc")
    if homebrew.is_file():
        return homebrew
    raise ValueError("LLVM llvm-mc was not found; pass --llvm-mc")


def byte_line(word: int) -> str:
    return " ".join(f"0x{(word >> (byte * 8)) & 0xff:02x}" for byte in range(4))


def decoder_opcodes(
    tool: Path,
    words: list[int],
    mattr: str = "+all",
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[str | None]:
    if not words:
        return []
    command = [
        str(tool),
        "--disassemble",
        "--show-inst",
        "--triple=aarch64",
    ]
    if mattr:
        command.append(f"--mattr={mattr}")
    completed = runner(
        command,
        input="\n".join(byte_line(word) for word in words) + "\n",
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    invalid = {
        int(match.group(1)) - 1
        for match in INVALID_LINE.finditer(completed.stderr)
    }
    unexpected = [
        line
        for line in completed.stderr.splitlines()
        if "warning:" in line and ALLOWED_WARNING.fullmatch(line) is None
    ]
    if unexpected:
        raise ValueError(
            "llvm-mc emitted unexpected warnings: " + "; ".join(unexpected)
        )

    decoded = iter(MC_OPCODE.findall(completed.stdout))
    result: list[str | None] = []
    for index in range(len(words)):
        result.append(None if index in invalid else next(decoded, None))
    if any(opcode is None for index, opcode in enumerate(result) if index not in invalid):
        raise ValueError("llvm-mc output omitted a successful decoder opcode")
    if next(decoded, None) is not None:
        raise ValueError("llvm-mc output contains extra decoder opcodes")
    return result


def llvm_version(
    tool: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    completed = runner(
        [str(tool), "--version"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    first = completed.stdout.splitlines()[0]
    match = re.search(
        r"(?:LLVM|version)\s+(?:version\s+)?([0-9]+(?:\.[0-9]+)+)", first
    )
    if match is None:
        raise ValueError(f"cannot parse LLVM version from {first!r}")
    return match.group(1)


def require_pinned_llvm(
    tool: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    version = llvm_version(tool, runner)
    if version != PINNED_LLVM_VERSION:
        raise ValueError(
            f"LLVM decoder version is {version}, expected pinned "
            f"{PINNED_LLVM_VERSION}"
        )
    return f"llvm-mc-{version}"
