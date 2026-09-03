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


SCHEMA_VERSION = 2
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
ABI_FINGERPRINT_RE = re.compile(r"^0x[0-9a-f]{16}$")
PATCH_SERIES_HEADER = "order\tfilename\tstable_patch_id\tsha256\tsubject"
PATCH_COMPOSITION_HEADER = (
    "order\tname\tpatch_count\tmanifest_path\tmanifest_sha256"
)
PATCH_SERIES_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")

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
    "UML_KERNEL_ARTIFACT_SLICE",
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


def read_patch_provenance(path: Path) -> list[str]:
    if not path.is_file() or path.is_symlink():
        fail(f"patch provenance is missing or unsafe: {path}")
    try:
        rows = [
            raw
            for raw in path.read_text(encoding="utf-8").splitlines()
            if raw and not raw.startswith("#")
        ]
    except (OSError, UnicodeError) as error:
        fail(f"cannot read patch provenance: {error}")
    if not rows:
        fail("patch provenance is empty")
    return rows


def patch_series_count(rows: list[str], label: str) -> int:
    if rows[0] != PATCH_SERIES_HEADER:
        fail(f"{label} is not a patch-series manifest")
    expected_order = 1
    for line_number, row in enumerate(rows[1:], 2):
        fields = row.split("\t", 4)
        if len(fields) != 5:
            fail(f"patch series provenance is malformed at line {line_number}")
        order, filename, patch_id, digest, subject = fields
        if order != str(expected_order):
            fail(
                f"patch series provenance order {order} is not "
                f"{expected_order}"
            )
        safe_relative(filename, f"patch series filename at line {line_number}")
        require_hex(patch_id, 40, f"stable patch ID at line {line_number}")
        require_hex(digest, 64, f"patch digest at line {line_number}")
        require_string(subject, f"patch subject at line {line_number}")
        expected_order += 1
    count = expected_order - 1
    if count < 1:
        fail("patch series provenance contains no patches")
    return count


def patch_provenance_count(
    path: Path,
    source_root: Path | None = None,
    expected_composition: tuple[tuple[str, str], ...] | None = None,
) -> int:
    rows = read_patch_provenance(path)
    header = rows[0]
    if header == PATCH_SERIES_HEADER:
        if expected_composition is not None:
            fail("expected ordered patch composition, found one patch series")
        return patch_series_count(rows, "patch provenance")

    if header == PATCH_COMPOSITION_HEADER:
        if source_root is None or not source_root.is_absolute():
            fail("patch composition requires an absolute source root")
        total = 0
        seen_names: set[str] = set()
        seen_manifests: set[str] = set()
        expected_order = 1
        actual_composition: list[tuple[str, str]] = []
        for line_number, row in enumerate(rows[1:], 2):
            fields = row.split("\t")
            if len(fields) != 5:
                fail(f"patch composition is malformed at line {line_number}")
            order, name, raw_count, manifest_relative, digest = fields
            if order != str(expected_order):
                fail(
                    f"patch composition order {order} is not "
                    f"{expected_order}"
                )
            if PATCH_SERIES_NAME_RE.fullmatch(name) is None:
                fail(f"patch composition name is unsafe at line {line_number}")
            if name in seen_names:
                fail(f"patch composition repeats series {name}")
            if re.fullmatch(r"[1-9][0-9]*", raw_count) is None:
                fail(f"patch composition count is invalid at line {line_number}")
            require_hex(digest, 64, f"patch manifest digest at line {line_number}")
            manifest_relative = safe_relative(
                manifest_relative,
                f"patch manifest path at line {line_number}",
            )
            if manifest_relative in seen_manifests:
                fail(f"patch composition repeats manifest {manifest_relative}")
            manifest_path = source_root / manifest_relative
            referenced_rows = read_patch_provenance(manifest_path)
            referenced_count = patch_series_count(
                referenced_rows,
                f"patch composition series {name}",
            )
            if referenced_count != int(raw_count):
                fail(
                    f"patch composition series {name} contains "
                    f"{referenced_count} patches, expected {raw_count}"
                )
            if sha256_path(manifest_path) != digest:
                fail(f"patch composition series {name} digest differs")
            seen_names.add(name)
            seen_manifests.add(manifest_relative)
            actual_composition.append((name, manifest_relative))
            total += referenced_count
            expected_order += 1
        if len(seen_names) < 2:
            fail("patch composition must contain at least two ordered series")
        if (
            expected_composition is not None
            and tuple(actual_composition) != expected_composition
        ):
            fail("patch composition differs from its required ordered series")
        return total

    fail("patch provenance has an unsupported header")


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


def verify_config_fragment(path: Path, fragment: Path) -> None:
    config = parse_config(path)
    requested = parse_config(fragment)
    if not requested:
        fail(f"kernel profile fragment has no configuration: {fragment}")
    for symbol, wanted in requested.items():
        observed = config.get(symbol, "n")
        if observed != wanted:
            fail(
                f"kernel profile config {symbol}={observed}, expected {wanted}"
            )


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


def verify_tracked_identity(
    manifest: dict,
    source_root: Path,
    kernel_metadata_path: Path,
    patch_manifest_path: Path,
    abi_metadata_path: Path,
) -> None:
    kernel = parse_env(kernel_metadata_path)
    raw_patch_count = kernel.get(
        "UML_KERNEL_ARTIFACT_PATCH_COUNT",
        kernel["UML_KERNEL_PATCH_COUNT"],
    )
    if re.fullmatch(r"[1-9][0-9]*", raw_patch_count) is None:
        fail("tracked artifact patch count must be a positive integer")
    patch_count = int(raw_patch_count)
    expected_composition = None
    if (
        manifest["family"] == "mainline-6.18"
        and manifest["slice"] == "x86_64"
    ):
        expected_composition = (
            (
                "common",
                "kernel/mainline-6.18/patches/series/manifest.tsv",
            ),
            (
                "x86_64",
                "kernel/mainline-6.18/slices/x86_64/patches/series/manifest.tsv",
            ),
        )
    provenance_count = patch_provenance_count(
        patch_manifest_path,
        source_root,
        expected_composition,
    )
    if provenance_count != patch_count:
        fail(
            f"patch provenance contains {provenance_count} patches, "
            f"tracked artifact identity requires {patch_count}"
        )
    expected_source = {
        "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
        "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
        "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
        "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
        "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
        "patch_count": patch_count,
        "patch_manifest_sha256": sha256_path(patch_manifest_path),
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
        "UML_KERNEL_ARTIFACT_SCHEMA": str(SCHEMA_VERSION),
        "UML_KERNEL_ARTIFACT_RELEASE_ID": manifest["release_id"],
        "UML_KERNEL_ARTIFACT_DIRECTORY_NAME": manifest["directory_name"],
        "UML_KERNEL_ARTIFACT_FAMILY": manifest["family"],
        "UML_KERNEL_ARTIFACT_SLICE": manifest["slice"],
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
        common_root.parents[1],
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
    return manifest


def verify_compatible_artifact(
    artifact_root: Path,
    common_root: Path,
    abi_metadata_path: Path,
    expected_family: str,
    expected_slice: str,
    expected_release_series: str,
    expected_page_size: int,
    expected_config_fragment: Path | None = None,
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
    if expected_config_fragment is not None:
        verify_config_fragment(
            artifact_root / "kernel/config", expected_config_fragment
        )
    verify_elf(
        artifact_root / "kernel/vmlinux",
        manifest["page_size"],
        common_root,
        manifest["slice"],
    )
    return manifest
