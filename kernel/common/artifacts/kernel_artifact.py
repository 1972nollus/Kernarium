#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import datetime
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path, PurePosixPath
from typing import NoReturn

from contract_checks import ContractChecks, parse_config


SCHEMA_VERSION = 3
# Schema 2 bundles also recorded a patch count and a digest of a series
# manifest. Both are still accepted; the final tree is the source identity.
SUPPORTED_SCHEMA_VERSIONS = (2, 3)
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
ABI_FINGERPRINT_RE = re.compile(r"^0x[0-9a-f]{16}$")
TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "family",
    "slice",
    "published_at_utc",
    "release",
    "page_size",
    "elf_role",
    "source",
    "abi",
    "inventory",
}
SOURCE_KEYS = {
    "repository",
    "branch",
    "base_commit",
    "base_tree",
    "final_tree",
}
SCHEMA_2_SOURCE_KEYS = SOURCE_KEYS | {"patch_count", "patch_manifest_sha256"}
ABI_KEYS = {"version", "fingerprint"}
INVENTORY_KEYS = {"path", "role", "format", "size"}
ACCEPTANCE_KEYS = {
    "UML_KERNEL_ARTIFACT_SCHEMA",
    "UML_KERNEL_ARTIFACT_RELEASE_ID",
    "UML_KERNEL_ARTIFACT_DIRECTORY_NAME",
    "UML_KERNEL_ARTIFACT_FAMILY",
    "UML_KERNEL_ARTIFACT_SLICE",
    "UML_KERNEL_ARTIFACT_RELEASE",
    "UML_KERNEL_ARTIFACT_PAGE_SIZE",
    "UML_KERNEL_ARTIFACT_FINAL_TREE",
    "UML_KERNEL_ARTIFACT_ABI_VERSION",
    "UML_KERNEL_ARTIFACT_ABI_FINGERPRINT",
}

PAYLOAD_SPECS = {
    "kernel/System.map": ("kernel-symbol-map", "utf8-text"),
    "kernel/config": ("kernel-config", "kconfig"),
    "kernel/kernel.release": ("kernel-release", "utf8-text"),
    "kernel/modules.builtin": ("built-in-module-list", "utf8-text"),
    "kernel/modules.builtin.modinfo": (
        "built-in-module-metadata",
        "binary-records",
    ),
    "kernel/vmlinux": ("kernel-elf", "elf64-aarch64-et-dyn"),
}

_CHECKS = ContractChecks("kernel artifact verifier")
fail = _CHECKS.fail
require_exact_keys = _CHECKS.require_exact_keys
require_string = _CHECKS.require_string
require_integer = _CHECKS.require_integer
require_hex = _CHECKS.require_hex
safe_relative = _CHECKS.safe_relative
parse_env = _CHECKS.parse_env

ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_DYNSYM = 11
SHN_UNDEF = 0
SHF_ALLOC = 2
DWARF_SECTION_PREFIXES = (".debug_", ".zdebug_")


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "artifact manifest")
    schema = manifest["schema_version"]
    if schema not in SUPPORTED_SCHEMA_VERSIONS or isinstance(schema, bool):
        fail("artifact manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    family = require_string(manifest["family"], "kernel family")
    if family not in ("mainline-6.18", "android-6.18"):
        fail("kernel family is unsupported")
    kernel_slice = require_string(manifest["slice"], "kernel slice")
    if kernel_slice not in ("arm64", "x86_64"):
        fail("kernel slice is unsupported")
    page_size = require_integer(manifest["page_size"], "kernel page size")
    if page_size not in (4096, 16384):
        fail("kernel page size is unsupported")
    if kernel_slice == "x86_64" and family != "mainline-6.18":
        fail("the x86_64 kernel slice requires the mainline-6.18 family")
    if kernel_slice == "x86_64" and page_size != 4096:
        fail("the x86_64 kernel slice requires 4096-byte pages")
    if family == "android-6.18" and (
        kernel_slice != "arm64" or page_size != 16384
    ):
        fail("the android-6.18 family requires arm64 and 16384-byte pages")
    directory_name = require_string(manifest["directory_name"], "directory name")
    expected_directory = (
        f"{family}-{kernel_slice}-{page_size // 1024}k-{release_id}"
    )
    if directory_name != expected_directory:
        fail(
            "artifact directory name does not match family, slice, page size, "
            "and release ID"
        )
    require_string(manifest["published_at_utc"], "publication timestamp")
    require_string(manifest["release"], "kernel release")
    if manifest["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    source = require_exact_keys(
        manifest["source"],
        SCHEMA_2_SOURCE_KEYS if schema == 2 else SOURCE_KEYS,
        "source",
    )
    for key in ("repository", "branch"):
        require_string(source[key], f"source.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(source[key], 40, f"source.{key}")
    if schema == 2:
        require_integer(source["patch_count"], "source.patch_count", minimum=1)
        require_hex(
            source["patch_manifest_sha256"],
            64,
            "source.patch_manifest_sha256",
        )

    abi = require_exact_keys(manifest["abi"], ABI_KEYS, "abi")
    require_integer(abi["version"], "abi.version", minimum=1)
    fingerprint = require_string(abi["fingerprint"], "abi.fingerprint")
    if ABI_FINGERPRINT_RE.fullmatch(fingerprint) is None:
        fail("ABI fingerprint has an unsafe format")

    inventory = manifest["inventory"]
    if not isinstance(inventory, list):
        fail("artifact inventory must be an array")
    seen: set[str] = set()
    ordered: list[str] = []
    for index, raw_entry in enumerate(inventory):
        entry = require_exact_keys(raw_entry, INVENTORY_KEYS, f"inventory entry {index}")
        relative = safe_relative(entry["path"], f"inventory entry {index} path")
        if relative in seen:
            fail(f"duplicate inventory path: {relative}")
        if relative not in PAYLOAD_SPECS:
            fail(f"undeclared kernel artifact payload: {relative}")
        expected_role, expected_format = PAYLOAD_SPECS[relative]
        if entry["role"] != expected_role or entry["format"] != expected_format:
            fail(f"inventory role or format differs for {relative}")
        require_integer(entry["size"], f"inventory size for {relative}", minimum=1)
        seen.add(relative)
        ordered.append(relative)
    if seen != set(PAYLOAD_SPECS):
        fail("artifact inventory differs from its fixed payload contract")
    if ordered != sorted(ordered):
        fail("artifact inventory is not sorted by path")
    return manifest


def load_manifest(artifact_root: Path) -> dict:
    path = artifact_root / "kernel-artifact.json"
    if not path.is_file() or path.is_symlink():
        fail("kernel-artifact.json is missing or unsafe")
    try:
        return validate_manifest(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read artifact manifest: {error}")


def verify_inventory(artifact_root: Path, manifest: dict) -> None:
    expected_files = set(PAYLOAD_SPECS) | {"kernel-artifact.json"}
    expected_dirs = {".", "kernel"}
    actual_files: set[str] = set()
    actual_dirs: set[str] = {"."}
    for root, directories, files in os.walk(artifact_root, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = root_path.relative_to(artifact_root)
        for name in directories:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                fail(f"unsafe artifact directory: {path.relative_to(artifact_root)}")
            actual_dirs.add(str((relative_root / name).as_posix()))
        for name in files:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                fail(f"unsafe artifact payload: {path.relative_to(artifact_root)}")
            actual_files.add(str((relative_root / name).as_posix()))
    if actual_files != expected_files or actual_dirs != expected_dirs:
        fail(
            "artifact tree differs from its fixed contract "
            f"(missing files={sorted(expected_files - actual_files)}, "
            f"extra files={sorted(actual_files - expected_files)}, "
            f"extra directories={sorted(actual_dirs - expected_dirs)})"
        )
    entries = {entry["path"]: entry for entry in manifest["inventory"]}
    for relative in PAYLOAD_SPECS:
        if (artifact_root / relative).stat().st_size != entries[relative]["size"]:
            fail(f"artifact payload size differs for {relative}")


def verify_config(path: Path, page_size: int, kernel_slice: str) -> None:
    if kernel_slice not in ("arm64", "x86_64"):
        fail("kernel config slice is unsupported")
    if kernel_slice == "x86_64" and page_size != 4096:
        fail("the x86_64 kernel config requires 4096-byte pages")
    config = parse_config(path)
    expected = {
        "CONFIG_64BIT": "y",
        "CONFIG_ARM64": "y",
        "CONFIG_UML": "y",
        "CONFIG_MODULES": "n",
        "CONFIG_PAGE_SHIFT": "14" if page_size == 16384 else "12",
        "CONFIG_PAGE_SIZE_16KB": "y" if page_size == 16384 else "n",
        "CONFIG_PAGE_SIZE_4KB": "n" if page_size == 16384 else "y",
        "CONFIG_UML_X86_64_USERSPACE_ONLY": (
            "y" if kernel_slice == "x86_64" else "n"
        ),
        "CONFIG_BPF": "y",
        "CONFIG_BPF_SYSCALL": "y",
        "CONFIG_CGROUP_BPF": "y",
        "CONFIG_DEBUG_INFO_BTF": "y",
    }
    for symbol, wanted in expected.items():
        observed = config.get(symbol, "n")
        if observed != wanted:
            fail(f"kernel config {symbol}={observed}, expected {wanted}")
    modular = sorted(symbol for symbol, value in config.items() if value == "m")
    if modular:
        fail(f"kernel artifact contains loadable-module values: {modular[:8]}")


def bounded_slice(payload: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset > len(payload) or size > len(payload) - offset:
        fail(f"ELF {label} is outside the file")
    return payload[offset : offset + size]


def elf_imports(payload: bytes) -> set[str]:
    section_offset = struct.unpack_from("<Q", payload, 40)[0]
    section_entry_size, section_count = struct.unpack_from("<HH", payload, 58)
    if section_entry_size < 64 or section_count == 0:
        fail("ELF section-header table is missing or malformed")
    bounded_slice(
        payload,
        section_offset,
        section_entry_size * section_count,
        "section-header table",
    )
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        kind = struct.unpack_from("<I", payload, offset + 4)[0]
        file_offset, size = struct.unpack_from("<QQ", payload, offset + 24)
        link = struct.unpack_from("<I", payload, offset + 40)[0]
        entry_size = struct.unpack_from("<Q", payload, offset + 56)[0]
        if kind != SHT_NOBITS:
            bounded_slice(payload, file_offset, size, f"section {index}")
        sections.append((kind, file_offset, size, link if entry_size else -1))

    imported: set[str] = set()
    found_symbol_table = False
    for index, (kind, file_offset, size, link) in enumerate(sections):
        if kind not in (SHT_SYMTAB, SHT_DYNSYM):
            continue
        found_symbol_table = True
        entry_size = struct.unpack_from(
            "<Q", payload, section_offset + index * section_entry_size + 56
        )[0]
        if entry_size < 24 or size % entry_size != 0:
            fail(f"ELF symbol table {index} is malformed")
        if link < 0 or link >= len(sections):
            fail(f"ELF symbol table {index} has an invalid string table")
        _, string_offset, string_size, _ = sections[link]
        strings = bounded_slice(payload, string_offset, string_size, "string table")
        for symbol_offset in range(file_offset, file_offset + size, entry_size):
            name_offset = struct.unpack_from("<I", payload, symbol_offset)[0]
            section_index = struct.unpack_from("<H", payload, symbol_offset + 6)[0]
            if section_index != SHN_UNDEF or name_offset >= len(strings):
                continue
            terminator = strings.find(b"\0", name_offset)
            if terminator < 0:
                fail("ELF symbol name is unterminated")
            try:
                name = strings[name_offset:terminator].decode("ascii")
            except UnicodeDecodeError:
                continue
            if re.fullmatch(r"darwin_uml_[A-Za-z0-9_]+", name):
                imported.add(name)
    if not found_symbol_table:
        fail("ELF contains no symbol table for bridge verification")
    return imported


def expected_imports(
    common_root: Path, page_size: int, kernel_slice: str
) -> set[str]:
    if kernel_slice not in {"arm64", "x86_64"}:
        fail(f"unsupported kernel slice for bridge imports: {kernel_slice}")
    if kernel_slice == "x86_64" and page_size != 4096:
        fail("x86_64 bridge imports require 4096-byte pages")
    declared: set[str] = set()
    declaration = re.compile(r"\b(darwin_uml_[A-Za-z0-9_]+)\s*\(")
    for path in sorted((common_root / "abi").glob("darwin_uml_*.h")):
        declared.update(declaration.findall(path.read_text(encoding="utf-8")))
    if not declared:
        fail("shared ABI declarations are missing")
    conditional: dict[str, set[str]] = {
        "4k-only": set(),
        "arm64-only": set(),
    }
    policy = common_root / "abi/import-policy.tsv"
    rows = policy.read_text(encoding="utf-8").splitlines()
    if not rows or rows[0] != "symbol\tavailability\trationale":
        fail("ABI import policy is malformed")
    for line_number, row in enumerate(rows[1:], 2):
        fields = row.split("\t")
        if (
            len(fields) != 3
            or re.fullmatch(r"darwin_uml_[A-Za-z0-9_]+", fields[0]) is None
            or fields[1] not in conditional
            or not fields[2]
            or any(fields[0] in symbols for symbols in conditional.values())
        ):
            fail(f"ABI import policy is malformed at line {line_number}")
        conditional[fields[1]].add(fields[0])
    all_conditional = set().union(*conditional.values())
    if not all_conditional <= declared:
        fail("ABI import policy names undeclared bridge symbols")
    expected = set(declared)
    if page_size == 16384:
        expected -= conditional["4k-only"]
    if kernel_slice == "x86_64":
        expected -= conditional["arm64-only"]
    return expected


def verify_elf(
    path: Path, page_size: int, common_root: Path, kernel_slice: str
) -> None:
    payload = path.read_bytes()
    if len(payload) < 64 or payload[:7] != b"\x7fELF\x02\x01\x01":
        fail("kernel is not a little-endian ELF64 image")
    elf_type, machine = struct.unpack_from("<HH", payload, 16)
    if elf_type != ET_DYN or machine != EM_AARCH64:
        fail("kernel is not an AArch64 ET_DYN image")
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    program_entry_size, program_count = struct.unpack_from("<HH", payload, 54)
    if program_entry_size < 56:
        fail("ELF program-header entry size is malformed")
    bounded_slice(
        payload,
        program_offset,
        program_entry_size * program_count,
        "program-header table",
    )
    load_count = 0
    executable = False
    writable = False
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        kind, flags = struct.unpack_from("<II", payload, offset)
        file_offset, virtual_address = struct.unpack_from("<QQ", payload, offset + 8)
        file_size, memory_size, alignment = struct.unpack_from(
            "<QQQ", payload, offset + 32
        )
        if file_size > memory_size:
            fail(f"ELF program header {index} has file size above memory size")
        bounded_slice(payload, file_offset, file_size, f"segment {index}")
        if kind == PT_GNU_STACK and flags & PF_X:
            fail("kernel GNU stack is executable")
        if kind != PT_LOAD:
            continue
        load_count += 1
        if flags & PF_W and flags & PF_X:
            fail(f"ELF load segment {index} is writable and executable")
        if alignment < page_size or alignment & (alignment - 1):
            fail(f"ELF load segment {index} has invalid alignment {alignment:#x}")
        if virtual_address % alignment != file_offset % alignment:
            fail(f"ELF load segment {index} violates alignment congruence")
        executable |= bool(flags & PF_X)
        writable |= bool(flags & PF_W)
    if load_count < 2 or not executable or not writable:
        fail("kernel lacks separated executable and writable load segments")
    imported = elf_imports(payload)
    expected = expected_imports(common_root, page_size, kernel_slice)
    if imported != expected:
        fail(
            "kernel bridge imports differ "
            f"(missing={sorted(expected - imported)}, extra={sorted(imported - expected)})"
        )


def elf_sections(payload: bytes) -> list[tuple[str, int, int, int]]:
    """Return (name, type, flags, size) for every ELF64 section."""
    section_offset = struct.unpack_from("<Q", payload, 40)[0]
    section_entry_size, section_count, names_index = struct.unpack_from(
        "<HHH", payload, 58
    )
    if section_entry_size < 64 or section_count == 0 or names_index >= section_count:
        fail("ELF section-header table is missing or malformed")
    bounded_slice(
        payload,
        section_offset,
        section_entry_size * section_count,
        "section-header table",
    )
    headers = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        name, kind, flags = struct.unpack_from("<IIQ", payload, offset)
        file_offset, size = struct.unpack_from("<QQ", payload, offset + 24)
        headers.append((name, kind, flags, file_offset, size))
    _, names_kind, _, names_offset, names_size = headers[names_index]
    if names_kind == SHT_NOBITS:
        fail("ELF section-name table has no contents")
    names = bounded_slice(payload, names_offset, names_size, "section-name table")
    sections = []
    for name, kind, flags, _, size in headers:
        terminator = names.find(b"\0", name)
        if name >= len(names) or terminator < 0:
            fail("ELF section name is outside its string table")
        sections.append(
            (names[name:terminator].decode("ascii", "replace"), kind, flags, size)
        )
    return sections


def verify_runtime_image(vmlinux: Path, config: Path) -> None:
    """Check the shipped kernel carries BTF and no DWARF.

    DWARF is only pahole's input for BTF; nothing at runtime reads it.
    .BTF is allocated kernel data behind /sys/kernel/btf/vmlinux.
    """
    sections = {
        name: (kind, flags, size)
        for name, kind, flags, size in elf_sections(vmlinux.read_bytes())
    }
    dwarf = sorted(
        name for name in sections if name.startswith(DWARF_SECTION_PREFIXES)
    )
    if dwarf:
        fail(f"kernel image carries DWARF sections: {', '.join(dwarf[:8])}")
    if parse_config(config).get("CONFIG_DEBUG_INFO_BTF", "n") != "y":
        return
    btf = sections.get(".BTF")
    if btf is None or btf[0] == SHT_NOBITS or btf[2] == 0:
        fail("kernel config enables BTF but the image has no .BTF contents")
    if not btf[1] & SHF_ALLOC:
        fail("kernel .BTF section is not allocated")


def publish_runtime_image(objcopy: Path, source: Path, destination: Path) -> None:
    """Copy a built vmlinux without its DWARF.

    --strip-debug removes the debugging sections and the .symtab entries that
    only describe them: STT_FILE symbols and locals defined in .debug_*
    sections. Every other .symtab symbol, .dynsym, .BTF, and every loaded
    segment remain, so System.map and symbolization are unaffected.
    """
    result = subprocess.run(
        [str(objcopy), "--strip-debug", str(source), str(destination)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        fail(f"could not strip DWARF from the kernel image: {result.stderr.strip()}")
    shutil.copymode(source, destination)


def verify_tracked_identity(
    manifest: dict,
    kernel_metadata_path: Path,
    abi_metadata_path: Path,
) -> None:
    kernel = parse_env(kernel_metadata_path)
    expected_source = {
        "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
        "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
        "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
        "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
        "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
    }
    for key, value in expected_source.items():
        if manifest["source"][key] != value:
            fail(f"artifact source identity differs from tracked {key}")
    verify_abi_identity(manifest, abi_metadata_path)


def verify_abi_identity(manifest: dict, abi_metadata_path: Path) -> None:
    abi = parse_env(abi_metadata_path)
    expected_abi = {
        "version": int(abi["DARWIN_UML_ABI_VERSION"]),
        "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
    }
    for key, value in expected_abi.items():
        if manifest["abi"][key] != value:
            fail(f"artifact ABI identity differs from tracked {key}")


def verify_compatible_identity(
    manifest: dict,
    abi_metadata_path: Path,
    expected_family: str,
    expected_slice: str,
    expected_release_series: str,
    expected_page_size: int,
) -> None:
    verify_abi_identity(manifest, abi_metadata_path)
    if manifest["family"] != expected_family:
        fail(
            f"artifact family {manifest['family']} differs from "
            f"required {expected_family}"
        )
    if manifest["slice"] != expected_slice:
        fail(
            f"artifact slice {manifest['slice']} differs from "
            f"required {expected_slice}"
        )
    if manifest["page_size"] != expected_page_size:
        fail(
            f"artifact page size {manifest['page_size']} differs from "
            f"required {expected_page_size}"
        )
    release = manifest["release"]
    release_pattern = re.compile(
        rf"^{re.escape(expected_release_series)}(?:$|[.-].*)"
    )
    if release_pattern.fullmatch(release) is None:
        fail(
            f"kernel release {release} is outside required "
            f"{expected_release_series} series"
        )


def verify_acceptance(manifest: dict, acceptance_path: Path) -> None:
    acceptance = parse_env(acceptance_path)
    if set(acceptance) != ACCEPTANCE_KEYS:
        fail("kernel artifact acceptance keys differ from the fixed contract")
    expected = {
        "UML_KERNEL_ARTIFACT_SCHEMA": str(manifest["schema_version"]),
        "UML_KERNEL_ARTIFACT_RELEASE_ID": manifest["release_id"],
        "UML_KERNEL_ARTIFACT_DIRECTORY_NAME": manifest["directory_name"],
        "UML_KERNEL_ARTIFACT_FAMILY": manifest["family"],
        "UML_KERNEL_ARTIFACT_SLICE": manifest["slice"],
        "UML_KERNEL_ARTIFACT_RELEASE": manifest["release"],
        "UML_KERNEL_ARTIFACT_PAGE_SIZE": str(manifest["page_size"]),
        "UML_KERNEL_ARTIFACT_FINAL_TREE": manifest["source"]["final_tree"],
        "UML_KERNEL_ARTIFACT_ABI_VERSION": str(manifest["abi"]["version"]),
        "UML_KERNEL_ARTIFACT_ABI_FINGERPRINT": manifest["abi"]["fingerprint"],
    }
    for key, value in expected.items():
        if acceptance.get(key) != value:
            fail(f"artifact differs from accepted descriptor field {key}")


def verify_artifact(
    artifact_root: Path,
    common_root: Path,
    kernel_metadata_path: Path,
    abi_metadata_path: Path,
    *,
    acceptance_path: Path | None = None,
) -> dict:
    if not artifact_root.is_absolute():
        fail("artifact root must be an absolute local path")
    if not artifact_root.is_dir() or artifact_root.is_symlink():
        fail("artifact root must be a regular local directory")
    manifest = load_manifest(artifact_root)
    if artifact_root.name != manifest["directory_name"]:
        fail("artifact directory basename differs from its manifest")
    verify_tracked_identity(manifest, kernel_metadata_path, abi_metadata_path)
    if acceptance_path is not None:
        verify_acceptance(manifest, acceptance_path)
    verify_inventory(artifact_root, manifest)
    release = (artifact_root / "kernel/kernel.release").read_text(
        encoding="utf-8"
    ).strip()
    if release != manifest["release"]:
        fail("kernel release payload differs from its manifest")
    verify_config(
        artifact_root / "kernel/config",
        manifest["page_size"],
        manifest["slice"],
    )
    verify_elf(
        artifact_root / "kernel/vmlinux",
        manifest["page_size"],
        common_root,
        manifest["slice"],
    )
    verify_runtime_image(
        artifact_root / "kernel/vmlinux", artifact_root / "kernel/config"
    )
    return manifest


def verify_compatible_artifact(
    artifact_root: Path,
    common_root: Path,
    abi_metadata_path: Path,
    expected_family: str,
    expected_slice: str,
    expected_release_series: str,
    expected_page_size: int,
) -> dict:
    if not artifact_root.is_absolute():
        fail("artifact root must be an absolute local path")
    if not artifact_root.is_dir() or artifact_root.is_symlink():
        fail("artifact root must be a regular local directory")
    manifest = load_manifest(artifact_root)
    if artifact_root.name != manifest["directory_name"]:
        fail("artifact directory basename differs from its manifest")
    verify_compatible_identity(
        manifest,
        abi_metadata_path,
        expected_family,
        expected_slice,
        expected_release_series,
        expected_page_size,
    )
    verify_inventory(artifact_root, manifest)
    release = (artifact_root / "kernel/kernel.release").read_text(
        encoding="utf-8"
    ).strip()
    if release != manifest["release"]:
        fail("kernel release payload differs from its manifest")
    verify_config(
        artifact_root / "kernel/config",
        manifest["page_size"],
        manifest["slice"],
    )
    verify_elf(
        artifact_root / "kernel/vmlinux",
        manifest["page_size"],
        common_root,
        manifest["slice"],
    )
    verify_runtime_image(
        artifact_root / "kernel/vmlinux", artifact_root / "kernel/config"
    )
    return manifest


def publish_artifact(
    fail_publish: Callable[[str], NoReturn],
    *,
    family: str,
    kernel_slice: str,
    page_size: int,
    source: Path,
    source_label: str,
    build: Path,
    output_root: Path,
    release_id: str,
    objcopy: Path,
    common_root: Path,
    metadata_path: Path,
    check_release: Callable[[str, dict[str, str]], None],
    verify_staged: Callable[[Path], None] | None = None,
) -> Path:
    """Publish one built kernel as a new artifact directory in output_root.

    The source must be a clean checkout of the line's tracked final tree, and
    the checked-in series must still produce that tree. Each line's publisher
    validates its own arguments and supplies its release check and any extra
    check of the staged payloads, which runs before the manifest is written.
    """

    def git(*arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(source), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            fail_publish(f"Git failed in the {source_label}: {result.stderr.strip()}")
        return result.stdout.strip()

    metadata = parse_env(metadata_path)
    abi_metadata_path = common_root / "abi/identity.env"
    abi = parse_env(abi_metadata_path)
    if git("status", "--porcelain=v1", "--untracked-files=all"):
        fail_publish(f"{source_label} worktree is dirty")
    if git("rev-parse", "HEAD^{tree}") != metadata["UML_KERNEL_FINAL_TREE"]:
        fail_publish(f"{source_label} tree differs from the tracked final tree")
    # The recorded final tree must still be what the checked-in series
    # produce, so a line left stale by a change below it cannot publish.
    series_check = subprocess.run(
        [
            sys.executable,
            "-B",
            str(common_root / "tooling/kernel_series.py"),
            "tree",
            "--metadata",
            str(metadata_path),
            "--source",
            str(source),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
    )
    if series_check.returncode != 0:
        fail_publish("the checked-in series do not produce the tracked final tree")

    source_paths = {
        "kernel/System.map": build / "System.map",
        "kernel/config": build / ".config",
        "kernel/kernel.release": build / "include/config/kernel.release",
        "kernel/modules.builtin": build / "modules.builtin",
        "kernel/modules.builtin.modinfo": build / "modules.builtin.modinfo",
        "kernel/vmlinux": build / "vmlinux",
    }
    for relative, path in source_paths.items():
        if not path.is_file() or path.is_symlink():
            fail_publish(f"build payload is missing or unsafe for {relative}: {path}")
    for root, directories, files in os.walk(build, followlinks=False):
        directories[:] = [
            name for name in directories if not (Path(root) / name).is_symlink()
        ]
        for name in files:
            if name.endswith(".ko") or ".ko." in name:
                fail_publish(
                    f"loadable module output is forbidden: {Path(root) / name}"
                )

    # A config check is cheap, so a wrong configuration fails before the copy.
    # The image is checked once, as published, by verify_artifact below:
    # stripping DWARF changes nothing verify_elf reads, and the unstripped
    # build image carries the full DWARF.
    verify_config(source_paths["kernel/config"], page_size, kernel_slice)
    release = source_paths["kernel/kernel.release"].read_text(
        encoding="utf-8"
    ).strip()
    check_release(release, metadata)

    page_label = "16k" if page_size == 16384 else "4k"
    directory_name = f"{family}-{kernel_slice}-{page_label}-{release_id}"
    output_root.mkdir(parents=True, exist_ok=True)
    if output_root.is_symlink():
        fail_publish("output root must not be a symlink")
    destination = output_root / directory_name
    if destination.exists() or destination.is_symlink():
        fail_publish("kernel artifact destination already exists")
    staging_parent = Path(
        tempfile.mkdtemp(prefix=f".{directory_name}.", dir=output_root)
    )
    staging = staging_parent / directory_name
    staging.mkdir()
    try:
        inventory = []
        for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
            destination_path = staging / relative
            destination_path.parent.mkdir(parents=True, exist_ok=True)
            if relative == "kernel/vmlinux":
                # DWARF is only pahole's input. verify_artifact below proves
                # that the image keeps .BTF and carries no DWARF.
                publish_runtime_image(
                    objcopy, source_paths[relative], destination_path
                )
            else:
                shutil.copyfile(source_paths[relative], destination_path)
                shutil.copymode(source_paths[relative], destination_path)
            inventory.append(
                {
                    "path": relative,
                    "role": role,
                    "format": file_format,
                    "size": destination_path.stat().st_size,
                }
            )
        if verify_staged is not None:
            verify_staged(staging)
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "release_id": release_id,
            "directory_name": directory_name,
            "family": family,
            "slice": kernel_slice,
            "published_at_utc": datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat(timespec="seconds").replace("+00:00", "Z"),
            "release": release,
            "page_size": page_size,
            "elf_role": "darwin-uml",
            "source": {
                "repository": metadata["UML_KERNEL_UPSTREAM_REPOSITORY"],
                "branch": metadata["UML_KERNEL_UPSTREAM_BRANCH"],
                "base_commit": metadata["UML_KERNEL_UPSTREAM_COMMIT"],
                "base_tree": metadata["UML_KERNEL_UPSTREAM_TREE"],
                "final_tree": metadata["UML_KERNEL_FINAL_TREE"],
            },
            "abi": {
                "version": int(abi["DARWIN_UML_ABI_VERSION"]),
                "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            },
            "inventory": inventory,
        }
        (staging / "kernel-artifact.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        verify_artifact(staging, common_root, metadata_path, abi_metadata_path)
        os.replace(staging, destination)
        staging_parent.rmdir()
    except BaseException:
        shutil.rmtree(staging_parent, ignore_errors=True)
        raise
    return destination
