#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import os
import re
import stat
import struct
from pathlib import Path, PurePosixPath
from typing import NoReturn


SCHEMA_VERSION = 1
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
ABI_FINGERPRINT_RE = re.compile(r"^0x[0-9a-f]{16}$")

TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "family",
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
    "patch_count",
    "patch_manifest_sha256",
}
ABI_KEYS = {"version", "fingerprint"}
INVENTORY_KEYS = {"path", "role", "format", "size"}
ACCEPTANCE_KEYS = {
    "UML_KERNEL_ARTIFACT_SCHEMA",
    "UML_KERNEL_ARTIFACT_RELEASE_ID",
    "UML_KERNEL_ARTIFACT_DIRECTORY_NAME",
    "UML_KERNEL_ARTIFACT_FAMILY",
    "UML_KERNEL_ARTIFACT_RELEASE",
    "UML_KERNEL_ARTIFACT_PAGE_SIZE",
    "UML_KERNEL_ARTIFACT_FINAL_TREE",
    "UML_KERNEL_ARTIFACT_PATCH_MANIFEST_SHA256",
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


def fail(message: str) -> NoReturn:
    raise SystemExit(f"kernel artifact verifier: {message}")


def require_exact_keys(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    keys = set(value)
    if keys != expected:
        fail(
            f"{label} keys differ "
            f"(missing={sorted(expected - keys)}, extra={sorted(keys - expected)})"
        )
    return value


def require_string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{label} must be a nonempty string")
    if any(character in value for character in ("\0", "\n", "\r")):
        fail(f"{label} contains a control character")
    return value


def require_integer(value: object, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        fail(f"{label} must be an integer at least {minimum}")
    return value


def require_hex(value: object, length: int, label: str) -> str:
    result = require_string(value, label)
    pattern = HEX40_RE if length == 40 else HEX64_RE
    if pattern.fullmatch(result) is None:
        fail(f"{label} must be {length} lowercase hexadecimal characters")
    return result


def safe_relative(value: object, label: str) -> str:
    text = require_string(value, label)
    if "\\" in text:
        fail(f"{label} contains a backslash")
    path = PurePosixPath(text)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"{label} is not a normalized relative path")
    if str(path) != text:
        fail(f"{label} is not normalized")
    return text


def parse_env(path: Path) -> dict[str, str]:
    if not path.is_file() or path.is_symlink():
        fail(f"metadata file is missing or unsafe: {path}")
    values: dict[str, str] = {}
    assignment = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = assignment.fullmatch(line)
        if match is None:
            fail(f"{path}:{line_number}: malformed metadata assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            fail(f"{path}:{line_number}: duplicate metadata key {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def sha256_path(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "artifact manifest")
    if manifest["schema_version"] != SCHEMA_VERSION:
        fail("artifact manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    family = require_string(manifest["family"], "kernel family")
    if family not in ("mainline-6.18", "android-6.18"):
        fail("kernel family is unsupported")
    page_size = require_integer(manifest["page_size"], "kernel page size")
    if page_size not in (4096, 16384):
        fail("kernel page size is unsupported")
    directory_name = require_string(manifest["directory_name"], "directory name")
    expected_directory = f"{family}-{page_size // 1024}k-{release_id}"
    if directory_name != expected_directory:
        fail("artifact directory name does not match family, page size, and release ID")
    require_string(manifest["published_at_utc"], "publication timestamp")
    require_string(manifest["release"], "kernel release")
    if manifest["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    source = require_exact_keys(manifest["source"], SOURCE_KEYS, "source")
    for key in ("repository", "branch"):
        require_string(source[key], f"source.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(source[key], 40, f"source.{key}")
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


def parse_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("CONFIG_") and "=" in raw:
            key, value = raw.split("=", 1)
            values[key] = value
        elif raw.startswith("# CONFIG_") and raw.endswith(" is not set"):
            values[raw[2:-11]] = "n"
    return values


def verify_config(path: Path, page_size: int) -> None:
    config = parse_config(path)
    expected = {
        "CONFIG_64BIT": "y",
        "CONFIG_ARM64": "y",
        "CONFIG_UML": "y",
        "CONFIG_MODULES": "n",
        "CONFIG_PAGE_SHIFT": "14" if page_size == 16384 else "12",
        "CONFIG_PAGE_SIZE_16KB": "y" if page_size == 16384 else "n",
        "CONFIG_PAGE_SIZE_4KB": "n" if page_size == 16384 else "y",
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


def expected_imports(common_root: Path, page_size: int) -> set[str]:
    declared: set[str] = set()
    declaration = re.compile(r"\b(darwin_uml_[A-Za-z0-9_]+)\s*\(")
    for path in sorted((common_root / "abi").glob("darwin_uml_*.h")):
        declared.update(declaration.findall(path.read_text(encoding="utf-8")))
    if not declared:
        fail("shared ABI declarations are missing")
    conditional: set[str] = set()
    policy = common_root / "abi/import-policy.tsv"
    rows = policy.read_text(encoding="utf-8").splitlines()
    if not rows or rows[0] != "symbol\tavailability\trationale":
        fail("ABI import policy is malformed")
    for line_number, row in enumerate(rows[1:], 2):
        fields = row.split("\t")
        if (
            len(fields) != 3
            or re.fullmatch(r"darwin_uml_[A-Za-z0-9_]+", fields[0]) is None
            or fields[1] != "4k-only"
            or not fields[2]
            or fields[0] in conditional
        ):
            fail(f"ABI import policy is malformed at line {line_number}")
        conditional.add(fields[0])
    if not conditional <= declared:
        fail("ABI import policy names undeclared bridge symbols")
    return declared - conditional if page_size == 16384 else declared


def verify_elf(path: Path, page_size: int, common_root: Path) -> None:
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
    expected = expected_imports(common_root, page_size)
    if imported != expected:
        fail(
            "kernel bridge imports differ "
            f"(missing={sorted(expected - imported)}, extra={sorted(imported - expected)})"
        )


def verify_tracked_identity(
    manifest: dict,
    kernel_metadata_path: Path,
    patch_manifest_path: Path,
    abi_metadata_path: Path,
) -> None:
    kernel = parse_env(kernel_metadata_path)
    abi = parse_env(abi_metadata_path)
    expected_source = {
        "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
        "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
        "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
        "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
        "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
        "patch_count": int(kernel["UML_KERNEL_PATCH_COUNT"]),
        "patch_manifest_sha256": sha256_path(patch_manifest_path),
    }
    for key, value in expected_source.items():
        if manifest["source"][key] != value:
            fail(f"artifact source identity differs from tracked {key}")
    expected_abi = {
        "version": int(abi["DARWIN_UML_ABI_VERSION"]),
        "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
    }
    for key, value in expected_abi.items():
        if manifest["abi"][key] != value:
            fail(f"artifact ABI identity differs from tracked {key}")


def verify_acceptance(manifest: dict, acceptance_path: Path) -> None:
    acceptance = parse_env(acceptance_path)
    if set(acceptance) != ACCEPTANCE_KEYS:
        fail("kernel artifact acceptance keys differ from the fixed contract")
    expected = {
        "UML_KERNEL_ARTIFACT_SCHEMA": str(SCHEMA_VERSION),
        "UML_KERNEL_ARTIFACT_RELEASE_ID": manifest["release_id"],
        "UML_KERNEL_ARTIFACT_DIRECTORY_NAME": manifest["directory_name"],
        "UML_KERNEL_ARTIFACT_FAMILY": manifest["family"],
        "UML_KERNEL_ARTIFACT_RELEASE": manifest["release"],
        "UML_KERNEL_ARTIFACT_PAGE_SIZE": str(manifest["page_size"]),
        "UML_KERNEL_ARTIFACT_FINAL_TREE": manifest["source"]["final_tree"],
        "UML_KERNEL_ARTIFACT_PATCH_MANIFEST_SHA256": manifest["source"][
            "patch_manifest_sha256"
        ],
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
    patch_manifest_path: Path,
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
    verify_tracked_identity(
        manifest,
        kernel_metadata_path,
        patch_manifest_path,
        abi_metadata_path,
    )
    if acceptance_path is not None:
        verify_acceptance(manifest, acceptance_path)
    verify_inventory(artifact_root, manifest)
    release = (artifact_root / "kernel/kernel.release").read_text(
        encoding="utf-8"
    ).strip()
    if release != manifest["release"]:
        fail("kernel release payload differs from its manifest")
    verify_config(artifact_root / "kernel/config", manifest["page_size"])
    verify_elf(artifact_root / "kernel/vmlinux", manifest["page_size"], common_root)
    return manifest
