#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the Kernarium Mesa port inside the materialized checkout.

The port carries two generated topics, and each has a subcommand here.

`protocol` writes the Venus protocol codec Mesa's Venus driver includes. The
guest build generates it out of tree through `-Dvenus-protocol-dir`, which
Soong cannot express, so the port carries the driver half of the codec at
`src/virtio/venus-protocol`. It runs venus-protocol's own generator from a
clean replay of the pinned protocol series against the registry of the pinned
Vulkan-Headers source, and refuses either input unless its tree is the one
`third_party/mesa-venus/metadata.env` records. The first line names both trees
instead of the replay commit, so the same pins always reproduce the same
bytes. `--check` compares instead of writing.

`soong` builds the Android.bp files. AOSP builds Mesa through
`meson2hermetic`, the upstream Meson fork that transpiles a meson build into
Android.bp files. This tool runs that converter with the checked project
configuration, repairs the converter's output where Soong needs more than
Meson does, drops every module the installed modules do not need, and then
scopes every remaining module name so the port cannot collide with the AOSP
`external/mesa3d` project, whose module names are otherwise identical and
global at the Make layer. The port builds the Venus Vulkan HAL and the Virgl
EGL and GLES libraries. It also deletes Mesa's legacy Android.mk makefiles:
Soong refuses a makefile under a denylisted directory, and the Android.bp
files alone describe the port.

The converter is a builder toolchain input pinned by `third_party/meson2hermetic`:
an upstream commit and a local series. This tool refuses a converter checkout
whose tree is not the recorded final tree. The builder-local platform
description stays outside this repository.

Run each step on the maintained topic branch and commit its result into its
own topic: `protocol` into "Mesa Venus pair the generated protocol headers",
`soong` into "Mesa Venus generated Soong port". Then regenerate the series
with `regenerate-mesa-venus-patches.py`. Never edit the generated headers or
Android.bp files by hand.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

import pinned_source
# pinned_source puts the contract checks on sys.path.
from contract_checks import parse_assignments  # noqa: E402

PREFIX = "venus_"
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


def checkout_files(root: Path, name: str) -> list[Path]:
    return [
        path
        for path in sorted(root.rglob(name))
        if ".git" not in path.parts
    ]


def bp_files(root: Path) -> list[Path]:
    return checkout_files(root, "Android.bp")


def remove_makefiles(root: Path) -> None:
    """Delete the checkout's legacy Android.mk makefiles."""
    makefiles = checkout_files(root, "Android.mk")
    for path in makefiles:
        path.unlink()
    print(f"removed {len(makefiles)} Android.mk files")


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


# The converter's output for Mesa's GL path needs repairs that Soong requires
# and Meson does not. Each is a generic rule about the converter's output,
# never a Mesa source change, and runs before the names are scoped so the
# modules it adds are scoped with the rest.


class Blueprint:
    """One Android.bp file as top-level blocks of lines."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.original = path.read_text() if path.exists() else ""
        self.head: list[str] = []
        self.blocks: list[list[str]] = []
        current: list[str] | None = None
        for line in self.original.splitlines():
            if current is None and BLOCK_TYPE.match(line):
                current = [line]
                continue
            if current is None:
                if self.blocks:
                    self.blocks[-1].append(line)
                else:
                    self.head.append(line)
                continue
            current.append(line)
            if BLOCK_END.match(line):
                self.blocks.append(current)
                current = None
        if current is not None:
            raise SystemExit(f"generate-mesa-venus-port.py: {path} ends inside a block")

    def text(self) -> str:
        lines = list(self.head)
        for block in self.blocks:
            lines.extend(block)
        return "\n".join(lines) + "\n"

    def save(self) -> bool:
        updated = self.text()
        if updated == self.original:
            return False
        self.path.write_text(updated)
        return True


def block_kind(block: list[str]) -> str:
    match = BLOCK_TYPE.match(block[0])
    return match.group(1) if match is not None else ""


def block_name(block: list[str]) -> str | None:
    for line in block:
        match = NAME_LINE.match(line)
        if match is not None:
            return match.group(2)
    return None


def field_open(field: str) -> re.Pattern[str]:
    return re.compile(r"^    " + re.escape(field) + r":\s*\[(.*)$")


def list_items(block: list[str], field: str) -> list[str]:
    """The quoted items of a top-level list field, in order."""
    opener = field_open(field)
    items: list[str] = []
    inside = False
    for line in block:
        if not inside:
            match = opener.match(line)
            if match is None:
                continue
            rest = match.group(1)
            if "]" in rest:
                return QUOTED.findall(rest)
            inside = True
            continue
        if LIST_CLOSE.match(line):
            break
        items.extend(QUOTED.findall(line))
    return items


def set_list(block: list[str], field: str, items: list[str]) -> list[str]:
    """Replace, add or (when items is empty) remove a top-level list field."""
    opener = field_open(field)
    field_lines = [f"    {field}: ["] + [f'        "{item}",' for item in items] + ["    ],"]
    out: list[str] = []
    inside = False
    replaced = False
    for line in block:
        if inside:
            if LIST_CLOSE.match(line):
                inside = False
            continue
        match = opener.match(line)
        if match is not None:
            inside = "]" not in match.group(1)
            replaced = True
            if items:
                out.extend(field_lines)
            continue
        out.append(line)
    if not replaced and items:
        end = max(i for i, line in enumerate(out) if BLOCK_END.match(line))
        out[end:end] = field_lines
    return out


def load(root: Path) -> list[Blueprint]:
    return [Blueprint(path) for path in bp_files(root)]


def resolve_source(directory: Path, entry: str) -> str | None:
    """A source path as it exists below its module's directory.

    The converter writes some sources relative to the project instead of to
    the module; the shortest trailing part of the path that exists below the
    module's directory is the file meant."""
    if "*" in entry or (directory / entry).exists():
        return entry
    parts = entry.split("/")
    for index in range(1, len(parts)):
        candidate = "/".join(parts[index:])
        if (directory / candidate).exists():
            return candidate
    return None


def python_library_name(root: Path, directory: Path) -> str:
    relative = directory.relative_to(root).as_posix()
    return "pylib_" + re.sub(r"[^A-Za-z0-9]+", "_", relative)


def repair_python(root: Path, blueprints: list[Blueprint], license: str) -> int:
    """Give each converted Python generator the sources Soong accepts.

    Meson runs a generator script with its own directory, and those of the
    modules it imports, on the path, so they import as top-level modules and
    may sit anywhere in the tree next to their data. A python_binary_host
    takes only .py sources below its own directory. Its .py files in other
    directories, including those of a filegroup it lists, are therefore
    linked as one python_library_host per directory, whose modules import at
    top level, and a filegroup's .py files in its own directory become its
    sources; a
    data file among its sources is dropped, because the genrule that runs the
    generator is given the same files itself. A filegroup source that does
    not exist where the converter placed it is found below its directory, or
    dropped when it exists nowhere."""
    groups: dict[str, tuple[Path, list[str]]] = {}
    for bp in blueprints:
        for block in bp.blocks:
            if block_kind(block) == "filegroup":
                name = block_name(block)
                if name is not None:
                    groups[name] = (bp.path.parent, list_items(block, "srcs"))
    libraries: set[Path] = set()
    repaired = 0
    for bp in blueprints:
        directory = bp.path.parent
        for index, block in enumerate(bp.blocks):
            kind = block_kind(block)
            if kind not in ("python_binary_host", "filegroup"):
                continue
            srcs: list[str] = []
            links: list[str] = []
            for entry in list_items(block, "srcs"):
                if entry.startswith(":"):
                    group = groups.get(entry[1:])
                    if kind != "python_binary_host" or group is None or all(
                        source.endswith(".py") for source in group[1]
                    ):
                        srcs.append(entry)
                        continue
                    for source in group[1]:
                        found = resolve_source(group[0], source)
                        if found is None or not found.endswith(".py"):
                            continue
                        path = (group[0] / found).resolve()
                        if path.parent == directory.resolve():
                            srcs.append(path.name)
                        else:
                            libraries.add(path.parent)
                            links.append(python_library_name(root, path.parent))
                    continue
                found = resolve_source(directory, entry)
                if found is None:
                    continue
                if kind == "python_binary_host":
                    if not found.endswith(".py"):
                        continue
                    owner = (directory / found).resolve().parent
                    if owner != directory.resolve():
                        libraries.add(owner)
                        links.append(python_library_name(root, owner))
                        continue
                srcs.append(found)
            updated = set_list(block, "srcs", list(dict.fromkeys(srcs)))
            if links:
                merged = list_items(block, "libs")
                merged += [link for link in dict.fromkeys(links) if link not in merged]
                updated = set_list(updated, "libs", merged)
            if updated != block:
                bp.blocks[index] = updated
                repaired += 1
    for directory in sorted(libraries):
        path = directory / "Android.bp"
        bp = next((b for b in blueprints if b.path == path), None)
        if bp is None:
            bp = Blueprint(path)
            bp.head = [
                "package {",
                f'    default_applicable_licenses: ["{license}"],',
                "}",
            ]
            blueprints.append(bp)
        name = python_library_name(root, directory)
        if any(block_name(block) == name for block in bp.blocks):
            continue
        bp.blocks.append([
            "",
            "python_library_host {",
            f'    name: "{name}",',
            '    srcs: ["*.py"],',
            "}",
        ])
    return repaired


def repair_needed(blueprints: list[Blueprint]) -> int:
    """Enable a module the converter disabled although an enabled one links it.

    Meson can give a library and a test executable one name. The converter
    merges them into one disabled module that links itself; the library is
    what its users need, so the module is enabled and stops linking itself
    and every module that links it."""
    modules: dict[str, tuple[Blueprint, int, bool, set[str]]] = {}
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            name = block_name(block)
            if name is None:
                continue
            enabled = not any(line.strip() == "enabled: false," for line in block)
            refs = set(QUOTED.findall("\n".join(
                line for line in block if NAME_LINE.match(line) is None
            )))
            modules[name] = (bp, index, enabled, refs)
    repaired = 0
    for name, (bp, index, enabled, _refs) in modules.items():
        if enabled:
            continue
        users = {
            other for other, module in modules.items()
            if other != name and module[2] and name in module[3]
        }
        if not users:
            continue
        drop = {name} | {
            other for other, module in modules.items()
            if other != name and name in module[3]
        }
        block = []
        for line in bp.blocks[index]:
            if line.strip() == "enabled: false,":
                continue
            quoted = re.match(r'^\s*"([^"]+)",?\s*$', line)
            if quoted is not None and quoted.group(1) in drop:
                continue
            block.append(line)
        bp.blocks[index] = block
        repaired += 1
    return repaired


def repair_nativebase(blueprints: list[Blueprint]) -> int:
    """Give a module that links libnativewindow the nativebase headers.

    libnativewindow's own headers include <nativebase/nativebase.h>, and a
    vendor module sees only what the LL-NDK library exports, which does not
    include those headers."""
    repaired = 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            if "libnativewindow" not in list_items(block, "shared_libs"):
                continue
            headers = list_items(block, "header_libs")
            if "libnativebase_headers" in headers:
                continue
            bp.blocks[index] = set_list(
                block, "header_libs", headers + ["libnativebase_headers"]
            )
            repaired += 1
    return repaired


def repair_install(blueprints: list[Blueprint], gl_front_ends: set[str]) -> int:
    """Install each shared library where the loader that opens it looks.

    The converter installs every shared library in hw/, which is right for a
    HAL module such as vulkan.virtio. The Android EGL loader opens the EGL
    and GLES libraries from egl/, and the dynamic linker resolves a library
    another one links only on the namespace's search path, which holds the
    partition's library directory but not hw/ or egl/."""
    linked = {
        library
        for bp in blueprints
        for block in bp.blocks
        for library in list_items(block, "shared_libs")
    }
    repaired = 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            if block_kind(block) != "cc_library_shared":
                continue
            name = block_name(block)
            if name in gl_front_ends:
                wanted = '"egl"'
            elif name in linked:
                wanted = None
            else:
                continue
            updated: list[str] = []
            for line in block:
                match = re.match(r'^(\s*)relative_install_path:\s*"[^"]*",\s*$', line)
                if match is None:
                    updated.append(line)
                elif wanted is not None:
                    updated.append(f"{match.group(1)}relative_install_path: {wanted},")
            if updated != block:
                bp.blocks[index] = updated
                repaired += 1
    return repaired


class Port:
    """The parts of soong-port.toml this tool reads; the converter reads the rest."""

    def __init__(self, config: Path, hal_module: str) -> None:
        table = tomllib.loads(config.read_text())
        license = table.get("copyright", {}).get("license_name")
        if not isinstance(license, str) or not license:
            raise SystemExit("generate-mesa-venus-port.py: the port names no license")
        self.license = license
        # The renamed targets keep their names unscoped: the HAL module that
        # ro.hardware.vulkan names, and the EGL and GLES libraries the Android
        # EGL loader resolves through ro.hardware.egl.
        self.keep = set(table.get("target_renames", {}).values())
        if hal_module not in self.keep:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the port does not rename a target to {hal_module}"
            )
        self.gl_front_ends = self.keep - {hal_module}
        self.warning_exceptions = warning_exceptions(table)


def repair(root: Path, blueprints: list[Blueprint], port: Port) -> None:
    python = repair_python(root, blueprints, port.license)
    needed = repair_needed(blueprints)
    nativebase = repair_nativebase(blueprints)
    installs = repair_install(blueprints, port.gl_front_ends)
    warnings = repair_warnings(blueprints, port.warning_exceptions)
    print(
        f"repaired {python} Python modules, {needed} disabled libraries, "
        f"{nativebase} nativewindow users, {installs} install paths and "
        f"{warnings} warning exceptions"
    )


FLAG_FIELDS = ("cflags", "conlyflags", "cppflags")


def git(directory: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"generate-mesa-venus-port.py: git {' '.join(arguments)} failed in "
            f"{directory}: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def verify_converter(android_root: Path, converter: Path) -> None:
    """Refuse any converter but the pinned upstream commit plus the series."""
    metadata = parse_assignments(
        android_root / "third_party/meson2hermetic/metadata.env",
        lambda message: SystemExit(f"generate-mesa-venus-port.py: {message}"),
    )
    expected = metadata["MESON2HERMETIC_FINAL_TREE"]
    checkout = converter.resolve().parent
    tree = git(checkout, "rev-parse", "HEAD^{tree}")
    if tree != expected:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter at {checkout} has tree "
            f"{tree}, not the recorded {expected}; replay "
            "third_party/meson2hermetic/series onto its pinned commit"
        )
    if git(checkout, "status", "--porcelain", "--untracked-files=no"):
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter at {checkout} has local changes"
        )


def warning_exceptions(table: dict[str, object]) -> list[dict[str, object]]:
    """The port's per-module warning exceptions, which the converter ignores."""
    entries = table.get("kernarium", {}).get("warning_exception", [])
    for entry in entries:
        if not isinstance(entry.get("module"), str) or not entry.get("reason"):
            raise SystemExit(
                "generate-mesa-venus-port.py: a warning exception needs a module "
                "and a reason"
            )
        flags = [field for field in FLAG_FIELDS if field in entry]
        if not flags or set(entry) - {"module", "reason", *FLAG_FIELDS}:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the warning exception for "
                f"{entry['module']} names no flags or an unknown field"
            )
    return entries


def repair_warnings(blueprints: list[Blueprint], exceptions: list[dict[str, object]]) -> int:
    """Give each module named by a warning exception its extra flags.

    Soong makes every warning an error in a project outside device/ and
    vendor/, which Meson's build of Mesa does not, so a warning Mesa's own
    build tolerates in one module needs an exception there."""
    repaired = 0
    for entry in exceptions:
        module = entry["module"]
        found = False
        for bp in blueprints:
            for index, block in enumerate(bp.blocks):
                if block_name(block) != module:
                    continue
                found = True
                for field in FLAG_FIELDS:
                    if field not in entry:
                        continue
                    items = list_items(bp.blocks[index], field)
                    items += [flag for flag in entry[field] if flag not in items]
                    bp.blocks[index] = set_list(bp.blocks[index], field, items)
                repaired += 1
        if not found:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the warning exception names "
                f"{module}, which the port does not define"
            )
    return repaired


def convert(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(args.converter),
        "convert",
        f"--config={args.config}",
        f"--platforms={args.platforms}",
        f"--project-dir={args.checkout}",
        f"--output-dir={args.checkout}",
        f"--dependencies={args.dependencies}",
    ]
    subprocess.run(command, cwd=args.checkout, check=True)


PROTOCOL_DIRECTORY = "src/virtio/venus-protocol"
PROTOCOL_HEADERS = "vn_protocol_driver*.h"


def pinned_checkout(path: Path, label: str, tree: str) -> Path:
    """A clean Git checkout root whose tree is the pinned one."""
    path = path.resolve()
    if not path.is_dir():
        raise SystemExit(f"generate-mesa-venus-port.py: the {label} is missing: {path}")
    if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the {label} is not a Git checkout root: {path}"
        )
    if git(path, "status", "--porcelain", "--untracked-files=all"):
        raise SystemExit(f"generate-mesa-venus-port.py: the {label} has local changes")
    actual = git(path, "rev-parse", "HEAD^{tree}")
    if actual != tree:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the {label} at {path} has tree {actual}, "
            f"not the pinned {tree}"
        )
    return path


def generate_protocol(args: argparse.Namespace, metadata: dict[str, str]) -> None:
    """Write, or with --check compare, the port's Venus protocol codec."""
    target = args.checkout / PROTOCOL_DIRECTORY
    if target.is_symlink() or not target.is_dir():
        raise SystemExit(
            f"generate-mesa-venus-port.py: the checkout has no {PROTOCOL_DIRECTORY}"
        )
    for key in ("MESA_VENUS_PROTOCOL_TREE", "MESA_VENUS_VULKAN_HEADERS_TREE"):
        if key not in metadata:
            raise SystemExit(f"generate-mesa-venus-port.py: the metadata lacks {key}")
    protocol = pinned_checkout(
        args.protocol_source, "venus-protocol source", metadata["MESA_VENUS_PROTOCOL_TREE"]
    )
    headers = pinned_checkout(
        args.vulkan_headers,
        "Vulkan-Headers source",
        metadata["MESA_VENUS_VULKAN_HEADERS_TREE"],
    )
    banner = pinned_source.venus_protocol_banner(metadata)
    with tempfile.TemporaryDirectory(prefix="venus-protocol-") as temporary:
        output = Path(temporary) / "codec"
        output.mkdir()
        banner_file = Path(temporary) / "banner"
        # venus-protocol's banner template ends in a blank line.
        banner_file.write_text(banner + "\n\n")
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(protocol / "vn_protocol.py"),
                "--outdir",
                str(output),
                "--registry",
                str(headers / "registry/vk.xml"),
                "--banner",
                str(banner_file),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            raise SystemExit(
                "generate-mesa-venus-port.py: the venus-protocol generator failed: "
                f"{(result.stderr or result.stdout).strip()}"
            )
        if git(protocol, "status", "--porcelain", "--untracked-files=all"):
            raise SystemExit(
                "generate-mesa-venus-port.py: the venus-protocol generator wrote into "
                "its source"
            )
        generated = {path.name: path.read_bytes() for path in output.glob(PROTOCOL_HEADERS)}
        main_header = generated.get("vn_protocol_driver.h", b"")
        if main_header.split(b"\n", 1)[0] != banner.encode():
            raise SystemExit(
                "generate-mesa-venus-port.py: the generator did not write the pinned "
                "banner into vn_protocol_driver.h"
            )
        existing = {path.name: path for path in target.glob(PROTOCOL_HEADERS)}
        stale = sorted(existing.keys() - generated.keys())
        changed = sorted(
            name for name, payload in generated.items()
            if name not in existing or existing[name].read_bytes() != payload
        )
        if args.check:
            if stale or changed:
                raise SystemExit(
                    "generate-mesa-venus-port.py: the checkout's protocol codec differs "
                    f"from the pinned generation: changed {changed}, stale {stale}"
                )
            print(f"the {len(generated)} protocol headers match the pinned generation")
            return
        for name in stale:
            existing[name].unlink()
        for name in changed:
            (target / name).write_bytes(generated[name])
    print(
        f"wrote {len(generated)} protocol headers: {len(changed)} changed, "
        f"{len(stale)} removed"
    )


def generate_soong(args: argparse.Namespace, metadata: dict[str, str]) -> None:
    args.config = args.android_root / "third_party/mesa-venus/soong-port.toml"
    if not args.config.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the port configuration is missing")
    if args.dependencies is None:
        args.dependencies = (
            args.android_root / "third_party/mesa-venus/soong-dependencies.toml"
        )
    if not args.dependencies.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the dependency map is missing")
    port = Port(args.config, metadata.get("MESA_VENUS_HAL_MODULE", ""))
    verify_converter(args.android_root, args.converter)
    convert(args)
    remove_makefiles(args.checkout)
    blueprints = load(args.checkout)
    repair(args.checkout, blueprints, port)
    blueprints = prune(blueprints, port.keep)
    scope(args.checkout, blueprints, port.keep)
    saved = sum(1 for bp in blueprints if bp.save())
    print(f"wrote {saved} Android.bp files")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the Kernarium Mesa port")
    steps = parser.add_subparsers(dest="step", required=True)
    protocol = steps.add_parser(
        "protocol", help="write the paired Venus protocol codec"
    )
    protocol.add_argument(
        "--protocol-source",
        type=Path,
        required=True,
        help="clean venus-protocol replay at MESA_VENUS_PROTOCOL_TREE",
    )
    protocol.add_argument(
        "--vulkan-headers",
        type=Path,
        required=True,
        help="clean Vulkan-Headers checkout at MESA_VENUS_VULKAN_HEADERS_TREE",
    )
    protocol.add_argument(
        "--check",
        action="store_true",
        help="compare the checkout's codec with the generation; write nothing",
    )
    soong = steps.add_parser("soong", help="write the Android.bp files")
    soong.add_argument(
        "--converter",
        type=Path,
        required=True,
        help="builder-local meson2hermetic meson.py",
    )
    soong.add_argument(
        "--platforms", type=Path, required=True, help="builder-local platforms toml"
    )
    soong.add_argument(
        "--dependencies",
        type=Path,
        help="dependency map override; defaults to the port's own map",
    )
    for step in (protocol, soong):
        step.add_argument("--android-root", type=Path, required=True)
        step.add_argument(
            "--checkout",
            type=Path,
            required=True,
            help="materialized external/mesa3d-venus checkout",
        )
    args = parser.parse_args()
    args.android_root = args.android_root.resolve()
    args.checkout = args.checkout.resolve()
    metadata = parse_assignments(
        args.android_root / "third_party/mesa-venus/metadata.env",
        lambda message: SystemExit(f"generate-mesa-venus-port.py: {message}"),
    )
    if args.step == "protocol":
        generate_protocol(args, metadata)
    else:
        generate_soong(args, metadata)


LABEL = re.compile(r":([A-Za-z0-9_.+-]+)")
LOCATION = re.compile(r"\$\(locations?\s+([^)\s]+)\)")


def block_refs(block: list[str]) -> set[str]:
    """Every name a block could refer to: a quoted string, a `:name` label or a
    `$(location name)` in a genrule command. Any other string that happens to
    equal a module name only keeps that module."""
    text = "\n".join(line for line in block if NAME_LINE.match(line) is None)
    return (
        set(QUOTED.findall(text))
        | set(LABEL.findall(text))
        | set(LOCATION.findall(text))
    )


def prune(blueprints: list[Blueprint], keep: set[str]) -> list[Blueprint]:
    """Delete every module that no installed module needs.

    The converter describes each Meson target it meets, among them tools,
    tests, Windows module definitions and GLX tables the product never builds
    or links. A module stays when a kept module or a package declaration
    refers to it, directly or through another module that stays; a file left
    without modules is deleted."""
    modules: dict[str, set[str]] = {}
    roots = set(keep)
    for bp in blueprints:
        for block in bp.blocks:
            name = block_name(block)
            if name is None:
                roots |= block_refs(block)
            else:
                modules[name] = block_refs(block)
    missing = keep - modules.keys()
    if missing:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter defined no {sorted(missing)[0]}"
        )
    reached: set[str] = set()
    pending = [name for name in roots if name in modules]
    while pending:
        name = pending.pop()
        if name not in reached:
            reached.add(name)
            pending.extend(ref for ref in modules[name] if ref in modules)
    kept: list[Blueprint] = []
    removed = 0
    for bp in blueprints:
        bp.blocks = [
            block for block in bp.blocks
            if (name := block_name(block)) is None or name in reached
        ]
        if any(block_name(block) is not None for block in bp.blocks):
            # A block carries the blank lines that follow it.
            while not bp.blocks[-1][-1].strip():
                bp.blocks[-1].pop()
            kept.append(bp)
        else:
            bp.path.unlink(missing_ok=True)
            removed += 1
    print(
        f"pruned {len(modules) - len(reached)} of {len(modules)} modules and "
        f"{removed} Android.bp files"
    )
    return kept


def scope(root: Path, blueprints: list[Blueprint], keep: set[str]) -> None:
    """Prefix every generated module name, and every reference to one.

    Each compiled module also gains the port's AOSP-strictness defaults."""
    names = {
        name
        for bp in blueprints
        for block in bp.blocks
        if (name := block_name(block)) is not None and name not in keep
    }
    already = {name for name in names if name.startswith(PREFIX)}
    if already:
        raise SystemExit(
            f"generate-mesa-venus-port.py: module names are already scoped: "
            f"{sorted(already)[:3]}"
        )
    alternation = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    colon = re.compile(r":(" + alternation + r")(?![A-Za-z0-9_.])")
    label = re.compile(r"\$\((locations?)\s+(" + alternation + r")\)")
    changed = 0
    for bp in blueprints:
        before = bp.text()
        bp.head = rewrite_block(bp.head, names, colon, label)
        for index, block in enumerate(bp.blocks):
            block = rewrite_block(block, names, colon, label)
            if block_kind(block) in COMPAT_TYPES:
                block = set_list(block, "defaults", list_items(block, "defaults") + [COMPAT])
            bp.blocks[index] = block
        changed += bp.text() != before
    top = next((bp for bp in blueprints if bp.path == root / "Android.bp"), None)
    if top is None:
        raise SystemExit("generate-mesa-venus-port.py: the port has no top-level Android.bp")
    if not any(block_name(block) == COMPAT for block in top.blocks):
        flags = "".join(f'        "{flag}",\n' for flag in COMPAT_FLAGS)
        top.blocks.append((COMPAT_BLOCK % (COMPAT, flags, flags)).split("\n")[:-1])
    print(f"scoped {len(names)} module names in {changed} Android.bp files")


if __name__ == "__main__":
    main()
