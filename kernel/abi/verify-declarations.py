#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bind the independent kernel bridge and installed API declarations."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True


KERNEL_ABI_FILES = (
    "kernel/abi/darwin_uml_abi.h",
    "kernel/abi/darwin_uml_cpu.h",
    "kernel/abi/darwin_uml_host.h",
    "kernel/abi/darwin_uml_memory.h",
    "kernel/abi/darwin_uml_network.h",
    "kernel/abi/darwin_uml_bridge.h",
    "kernel/abi/uml_tlb_sync_ranges.h",
)
PUBLIC_ABI_FILES = (
    "darwin/api/include/uml/abi_identity.h",
    "darwin/api/include/uml/emulator.h",
)

DOMAINS = {
    "kernel": {
        "files": KERNEL_ABI_FILES,
        "identity": "kernel/abi/identity.env",
        "history": "kernel/abi/declaration-history.tsv",
        "prefix": "DARWIN_UML_ABI",
    },
    "public": {
        "files": PUBLIC_ABI_FILES,
        "identity": "darwin/api/abi/identity.env",
        "history": "darwin/api/abi/declaration-history.tsv",
        "prefix": "UML_EMULATOR_ABI",
    },
}

TOKEN = re.compile(
    r"""
    [A-Za-z_][A-Za-z_0-9]*
    |0[xX][0-9A-Fa-f]+[uUlL]*
    |[0-9]+[uUlL]*
    |"(?:\\.|[^"\\])*"
    |'(?:\\.|[^'\\])*'
    |\.\.\.|<<=|>>=|->|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\#\#
    |[^\s]
    """,
    re.VERBOSE,
)


def fail(message: str) -> None:
    raise SystemExit(f"kernel/abi/verify-declarations.py: {message}")


def without_comments(source: str) -> str:
    result: list[str] = []
    index = 0
    quote: str | None = None
    while index < len(source):
        character = source[index]
        if quote is not None:
            result.append(character)
            if character == "\\" and index + 1 < len(source):
                index += 1
                result.append(source[index])
            elif character == quote:
                quote = None
            index += 1
            continue
        if character in {'"', "'"}:
            quote = character
            result.append(character)
            index += 1
            continue
        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            if newline < 0:
                break
            result.append("\n")
            index = newline + 1
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                fail("unterminated block comment in ABI input")
            result.append("\n" * source[index : end + 2].count("\n"))
            index = end + 2
            continue
        result.append(character)
        index += 1
    if quote is not None:
        fail("unterminated string or character literal in ABI input")
    return "".join(result)


def declaration_digest(root: Path, domain: str) -> str:
    normalized: list[str] = []
    for relative in DOMAINS[domain]["files"]:
        path = root / relative
        if not path.is_file():
            fail(f"missing ABI input: {path}")
        source = without_comments(path.read_text(encoding="utf-8"))
        if relative == "kernel/abi/darwin_uml_abi.h":
            # These checks mirror identity.env. Hashing their right-hand
            # literals would make a derived fingerprint depend on itself.
            source = re.sub(
                r"static_assert\s*\(\s*DARWIN_UML_ABI_"
                r"(?:VERSION|FINGERPRINT)\s*==.*?\);",
                "",
                source,
                flags=re.DOTALL,
            )
        elif relative == "darwin/api/include/uml/abi_identity.h":
            # The public identity header is installed, but its selected
            # version and derived fingerprint must not hash themselves.
            source = re.sub(
                r"#\s*define\s+UML_EMULATOR_ABI_"
                r"(?:VERSION|FINGERPRINT)\s+[^\n]+",
                "",
                source,
            )
        tokens = TOKEN.findall(source)
        normalized.append(f"FILE {relative}\n")
        normalized.extend(f"{token}\n" for token in tokens)
    return hashlib.sha256("".join(normalized).encode()).hexdigest()


def derived_fingerprint(digest: str) -> str:
    return f"0x{digest[:16]}"


def identity(root: Path, domain: str) -> tuple[int, str]:
    configuration = DOMAINS[domain]
    path = root / configuration["identity"]
    source = path.read_text(encoding="utf-8")
    prefix = configuration["prefix"]
    version = re.search(rf"^{prefix}_VERSION=([0-9]+)$", source, re.MULTILINE)
    fingerprint = re.search(
        rf"^{prefix}_FINGERPRINT='(0x[0-9A-Fa-f]+)'$",
        source,
        re.MULTILINE,
    )
    if version is None or fingerprint is None:
        fail(f"cannot parse {path}")
    return int(version.group(1)), fingerprint.group(1).lower()


def history(root: Path, domain: str) -> list[tuple[int, str, str]]:
    path = root / DOMAINS[domain]["history"]
    if not path.is_file():
        fail(f"missing declaration history: {path}")
    rows: list[tuple[int, str, str]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#") or line.startswith("version\t"):
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            fail(f"{path}:{number}: expected four tab-separated fields")
        try:
            version = int(fields[0])
        except ValueError:
            fail(f"{path}:{number}: invalid ABI version")
        fingerprint = fields[1].lower()
        digest = fields[2].lower()
        if not re.fullmatch(r"0x[0-9a-f]{16}", fingerprint):
            fail(f"{path}:{number}: invalid ABI fingerprint")
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(f"{path}:{number}: invalid declaration SHA-256")
        if not fields[3].strip():
            fail(f"{path}:{number}: missing change rationale")
        rows.append((version, fingerprint, digest))
    if not rows:
        fail("declaration history is empty")
    for previous, current in zip(rows, rows[1:]):
        if current[0] != previous[0] + 1:
            fail("declaration history versions must be consecutive")
    if len({row[0] for row in rows}) != len(rows):
        fail("declaration history repeats an ABI version")
    if len({row[1] for row in rows}) != len(rows):
        fail("declaration history repeats an ABI fingerprint")
    if len({row[2] for row in rows}) != len(rows):
        fail("declaration history repeats a declaration digest")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--compute", action="store_true")
    parser.add_argument(
        "--domain", choices=("all", *DOMAINS), default="all"
    )
    options = parser.parse_args()
    root = options.root.resolve()
    domains = tuple(DOMAINS) if options.domain == "all" else (options.domain,)
    for domain in domains:
        digest = declaration_digest(root, domain)
        fingerprint = derived_fingerprint(digest)
        if options.compute:
            print(f"{domain}_declaration_sha256={digest}")
            print(f"{domain}_derived_fingerprint={fingerprint}")
            continue

        version, configured_fingerprint = identity(root, domain)
        rows = history(root, domain)
        recorded_version, recorded_fingerprint, recorded_digest = rows[-1]
        if (version != recorded_version or
                configured_fingerprint != recorded_fingerprint):
            fail(
                f"{domain} identity does not select its final history row"
            )
        if digest != recorded_digest:
            fail(
                f"{domain} ABI declarations changed without appending a new "
                f"identity row (computed {digest}, recorded {recorded_digest})"
            )
        if fingerprint != recorded_fingerprint:
            fail(
                f"{domain} ABI fingerprint is not derived from its normalized "
                f"declarations (expected {fingerprint})"
            )
        print(
            f"verified {domain} ABI declarations version {version}, "
            f"fingerprint {configured_fingerprint}, SHA-256 {digest}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        fail(str(error))
