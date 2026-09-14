#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the Kernarium Venus Soong port inside the materialized checkout.

AOSP builds Mesa through `meson2hermetic`, the upstream Meson fork that
transpiles a meson build into Android.bp files. This tool runs that converter
with the checked project configuration, then scopes every generated module name
so the port cannot collide with the AOSP `external/mesa3d` GLES port, whose
module names are otherwise identical and global at the Make layer.

The converter and its builder-local platform and dependency descriptions stay
outside this repository, like every other builder toolchain input.

Run this tool, commit the result on the maintained topic branch, and then
regenerate the series with `regenerate-mesa-venus-patches.py`. Never edit the
generated Android.bp files by hand.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

PREFIX = "venus_"
KEEP = {"vulkan.virtio"}
REF_FIELDS = {
    "defaults",
    "header_libs",
    "static_libs",
    "whole_static_libs",
    "shared_libs",
    "generated_headers",
    "generated_sources",
    "libs",
    "required",
    "host_required",
    "target_required",
    "tools",
    "export_header_lib_headers",
    "export_static_lib_headers",
    "export_shared_lib_headers",
    "export_generated_headers",
    "default_applicable_licenses",
}
TOOL_FIELDS = {"tools", "tool_files"}
NAME_LINE = re.compile(r'^(\s*)name:\s*"([^"]+)"(,?)\s*$')
LIST_OPEN = re.compile(r"^\s*([a-z_0-9]+):\s*\[\s*$")
LIST_CLOSE = re.compile(r"^\s*\],?\s*$")
INLINE_LIST = re.compile(r'^(\s*)([a-z_0-9]+):\s*\[(.*)\](,?)\s*$')
QUOTED = re.compile(r'"([^"]*)"')
BLOCK_END = re.compile(r"^\}\s*$")
COMPAT = "venus_kernarium_compat_args"
COMPAT_TYPES = ("cc_library_shared", "cc_library_static", "cc_binary")
COMPAT_BLOCK = """
cc_defaults {
    name: "%s",
    vendor: true,
    cflags: [
%s    ],
    cppflags: [
%s    ],
}
"""
COMPAT_FLAGS = [
    "-Wno-gnu-pointer-arith",
]
BLOCK_TYPE = re.compile(r"^([a-z_0-9]+)\s*\{\s*$")


def bp_files(root: Path) -> list[Path]:
    return [
        path
        for path in sorted(root.rglob("Android.bp"))
        if ".git" not in path.parts
    ]


def collect(root: Path) -> set[str]:
    names: set[str] = set()
    for path in bp_files(root):
        for line in path.read_text().splitlines():
            match = NAME_LINE.match(line)
            if match is not None:
                names.add(match.group(2))
    return names


def block_tools(block: list[str]) -> set[str]:
    tools: set[str] = set()
    field: str | None = None
    for line in block:
        opened = LIST_OPEN.match(line)
        if opened is not None:
            field = opened.group(1)
            continue
        if LIST_CLOSE.match(line):
            field = None
            continue
        if field in TOOL_FIELDS:
            tools.update(QUOTED.findall(line))
    return {tool.lstrip(":") for tool in tools}


def apply_compat(block: list[str]) -> list[str]:
    """Append the port's AOSP-strictness defaults to every compiled module."""
    kind = None
    for line in block:
        match = BLOCK_TYPE.match(line)
        if match is not None:
            kind = match.group(1)
            break
    if kind not in COMPAT_TYPES:
        return block
    out: list[str] = []
    field = None
    inserted = False
    for line in block:
        opened = LIST_OPEN.match(line)
        if opened is not None:
            field = opened.group(1)
            out.append(line)
            continue
        if LIST_CLOSE.match(line):
            if field == "defaults" and not inserted:
                out.append(f'        "{COMPAT}",')
                inserted = True
            field = None
            out.append(line)
            continue
        out.append(line)
    return out


def rewrite_block(
    block: list[str], names: set[str], colon: re.Pattern[str], label: re.Pattern[str]
) -> list[str]:
    tools = block_tools(block) & names
    out: list[str] = []
    field: str | None = None
    for line in block:
        match = NAME_LINE.match(line)
        if match is not None and match.group(2) in names:
            line = f'{match.group(1)}name: "{PREFIX}{match.group(2)}"{match.group(3)}'
            out.append(line)
            continue
        opened = LIST_OPEN.match(line)
        if opened is not None:
            field = opened.group(1)
            out.append(line)
            continue
        if LIST_CLOSE.match(line):
            field = None
            out.append(line)
            continue
        if field in REF_FIELDS:
            line = QUOTED.sub(
                lambda m: f'"{PREFIX}{m.group(1)}"'
                if m.group(1) in names
                else m.group(0),
                line,
            )
        else:
            inline = INLINE_LIST.match(line)
            if inline is not None and inline.group(2) in REF_FIELDS:
                body = QUOTED.sub(
                    lambda m: f'"{PREFIX}{m.group(1)}"'
                    if m.group(1) in names
                    else m.group(0),
                    inline.group(3),
                )
                line = f"{inline.group(1)}{inline.group(2)}: [{body}]{inline.group(4)}"
        line = colon.sub(lambda m: ":" + PREFIX + m.group(1), line)
        line = label.sub(
            lambda m: f"$({m.group(1)} {PREFIX}{m.group(2)})"
            if m.group(2) in tools
            else m.group(0),
            line,
        )
        out.append(line)
    return out


def rewrite(
    path: Path, names: set[str], colon: re.Pattern[str], label: re.Pattern[str]
) -> bool:
    original = path.read_text()
    lines = original.splitlines()
    result: list[str] = []
    block: list[str] = []
    for line in lines:
        block.append(line)
        if BLOCK_END.match(line):
            result.extend(apply_compat(rewrite_block(block, names, colon, label)))
            block = []
    result.extend(apply_compat(rewrite_block(block, names, colon, label)))
    updated = "\n".join(result) + "\n"
    if updated == original:
        return False
    path.write_text(updated)
    return True


def convert(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(args.converter),
        "convert",
        f"--config={args.config}",
        f"--platforms={args.platforms}",
        f"--project-dir={args.checkout}",
        f"--output-dir={args.checkout}",
    ]
    if args.dependencies is not None:
        command.append(f"--dependencies={args.dependencies}")
    subprocess.run(command, cwd=args.checkout, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate the Kernarium Venus Soong port"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument(
        "--checkout",
        type=Path,
        required=True,
        help="materialized external/mesa3d-venus checkout",
    )
    parser.add_argument(
        "--converter",
        type=Path,
        required=True,
        help="builder-local meson2hermetic meson.py",
    )
    parser.add_argument(
        "--platforms", type=Path, required=True, help="builder-local platforms toml"
    )
    parser.add_argument(
        "--dependencies", type=Path, help="builder-local dependencies toml"
    )
    args = parser.parse_args()
    args.android_root = args.android_root.resolve()
    args.checkout = args.checkout.resolve()
    args.config = args.android_root / "third_party/mesa-venus/soong-port.toml"
    if not args.config.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the port configuration is missing")
    convert(args)
    scope(args.checkout)


def scope(root: Path) -> None:
    names = {name for name in collect(root) if name not in KEEP}
    already = {name for name in names if name.startswith(PREFIX)}
    if already:
        raise SystemExit(
            f"generate-mesa-venus-port.py: module names are already scoped: "
            f"{sorted(already)[:3]}"
        )
    alternation = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    colon = re.compile(r":(" + alternation + r")(?![A-Za-z0-9_.])")
    label = re.compile(r"\$\((locations?)\s+(" + alternation + r")\)")
    changed = sum(1 for path in bp_files(root) if rewrite(path, names, colon, label))
    top = root / "Android.bp"
    text = top.read_text()
    if COMPAT not in text:
        flags = "".join(f'        "{flag}",\n' for flag in COMPAT_FLAGS)
        top.write_text(text + COMPAT_BLOCK % (COMPAT, flags, flags))
    print(f"scoped {len(names)} module names in {changed} Android.bp files")


if __name__ == "__main__":
    main()
