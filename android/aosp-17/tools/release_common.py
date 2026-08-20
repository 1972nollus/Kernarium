#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import os
import re
import shutil
import stat
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from typing import NoReturn


SCHEMA_VERSION = 1
PRODUCT_FAMILY = "android-17-ack-6.18-16k"
DIRECTORY_PREFIX = "android17-ack6.18-16k-"
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")

PAYLOAD_SPECS = {
    "boot/kernel-command-line.txt": ("kernel-command-line", "utf8-text"),
    "boot/ramdisk.img": ("android-initramfs", "newc"),
    "guest/android-17-uml-arm64.img.zst": ("guest-disk-archive", "zstd-raw-gpt"),
    "guest/partition-layout.json": ("partition-layout", "json"),
    "kernel/System.map": ("kernel-symbol-map", "utf8-text"),
    "kernel/config": ("kernel-config", "kconfig"),
    "kernel/modules.builtin": ("built-in-module-list", "utf8-text"),
    "kernel/modules.builtin.modinfo": ("built-in-module-metadata", "binary-records"),
    "kernel/vmlinux": ("kernel-elf", "elf64-aarch64-et-dyn"),
    "provenance/aosp-repo-manifest.xml": ("aosp-resolved-manifest", "xml"),
    "provenance/build-receipt.tsv": ("build-receipt", "tsv"),
    "support/compatibility-matrix.xml": ("vendor-compatibility-matrix", "xml"),
    "support/fstab.uml": ("android-fstab", "utf8-text"),
    "support/vintf-manifest.xml": ("vendor-vintf-manifest", "xml"),
}

TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "product_family",
    "acceptance",
    "published_at_utc",
    "android",
    "aosp",
    "kernel",
    "outer",
    "producer",
    "inventory",
}
ANDROID_KEYS = {
    "major",
    "sdk",
    "build_id",
    "build_variant",
    "fingerprint",
    "vendor_api_level",
    "product",
    "release_config",
}
AOSP_KEYS = {
    "manifest_repository",
    "manifest_tag",
    "manifest_commit",
    "manifest_tree",
    "base_manifest_sha256",
    "projects_manifest_sha256",
    "routes",
}
ROUTE_KEYS = {
    "route_id",
    "manifest_project",
    "checkout_path",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "series_manifest_sha256",
}
KERNEL_KEYS = {
    "family",
    "repository",
    "branch",
    "tag",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "patch_manifest_sha256",
    "release",
    "page_size",
    "abi_version",
    "abi_fingerprint",
    "elf_role",
}
OUTER_KEYS = {"revision", "dirty"}
PRODUCER_KEYS = {"entry_point", "functional_options", "builder"}
BUILDER_KEYS = {
    "os",
    "architecture",
    "jdk",
    "clang",
    "linker",
    "darwin_sdk",
    "android_build_tools",
}
INVENTORY_KEYS = {"path", "role", "format", "size"}

ACCEPTANCE_KEYS = {
    "ANDROID17_ACCEPTED_SCHEMA",
    "ANDROID17_ACCEPTED_RELEASE_ID",
    "ANDROID17_ACCEPTED_DIRECTORY_NAME",
    "ANDROID17_ACCEPTED_PRODUCT_FAMILY",
    "ANDROID17_ACCEPTED_ANDROID_MAJOR",
    "ANDROID17_ACCEPTED_AOSP_MANIFEST_COMMIT",
    "ANDROID17_ACCEPTED_AOSP_BASE_MANIFEST_SHA256",
    "ANDROID17_ACCEPTED_AOSP_PROJECTS_MANIFEST_SHA256",
    "ANDROID17_ACCEPTED_KERNEL_FINAL_TREE",
    "ANDROID17_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256",
    "ANDROID17_ACCEPTED_OUTER_REVISION",
    "ANDROID17_ACCEPTED_PAGE_SIZE",
    "ANDROID17_ACCEPTED_ABI_VERSION",
    "ANDROID17_ACCEPTED_ABI_FINGERPRINT",
}


def fail(message: str) -> NoReturn:
    raise SystemExit(f"Android release verifier: {message}")


def require_exact_keys(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    keys = set(value)
    if keys != expected:
        missing = sorted(expected - keys)
        extra = sorted(keys - expected)
        fail(f"{label} keys differ (missing={missing}, extra={extra})")
    return value


def require_string(value: object, label: str, *, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value):
        fail(f"{label} must be a nonempty string")
    if "\0" in value or "\n" in value or "\r" in value:
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


def load_manifest(release_root: Path) -> dict:
    path = release_root / "release-manifest.json"
    if not path.is_file() or path.is_symlink():
        fail("release-manifest.json is missing or unsafe")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read release manifest: {error}")
    return validate_manifest(value)


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "release manifest")
    if manifest["schema_version"] != SCHEMA_VERSION:
        fail("release manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    directory_name = require_string(manifest["directory_name"], "directory name")
    if directory_name != DIRECTORY_PREFIX + release_id:
        fail("release directory name does not match the release ID")
    if manifest["product_family"] != PRODUCT_FAMILY:
        fail("release product family is not Android 17 ACK 6.18 16 KiB")
    if manifest["acceptance"] != "accepted":
        fail("release is not marked accepted")
    require_string(manifest["published_at_utc"], "publication timestamp")

    android = require_exact_keys(manifest["android"], ANDROID_KEYS, "android")
    if require_integer(android["major"], "Android major") != 17:
        fail("Android major is not 17")
    if require_integer(android["sdk"], "Android SDK") < 37:
        fail("Android SDK is below 37")
    for key in ("build_id", "build_variant", "fingerprint", "product", "release_config"):
        require_string(android[key], f"android.{key}")
    require_integer(android["vendor_api_level"], "vendor API level", minimum=202604)

    aosp = require_exact_keys(manifest["aosp"], AOSP_KEYS, "aosp")
    for key in ("manifest_repository", "manifest_tag"):
        require_string(aosp[key], f"aosp.{key}")
    require_hex(aosp["manifest_commit"], 40, "AOSP manifest commit")
    require_hex(aosp["manifest_tree"], 40, "AOSP manifest tree")
    require_hex(aosp["base_manifest_sha256"], 64, "base manifest SHA-256")
    require_hex(aosp["projects_manifest_sha256"], 64, "projects manifest SHA-256")
    routes = aosp["routes"]
    if not isinstance(routes, list) or not routes:
        fail("AOSP routes must be a nonempty array")
    seen_routes: set[str] = set()
    for index, raw_route in enumerate(routes):
        route = require_exact_keys(raw_route, ROUTE_KEYS, f"AOSP route {index}")
        route_id = require_string(route["route_id"], f"AOSP route {index} ID")
        if route_id in seen_routes:
            fail(f"duplicate AOSP route ID: {route_id}")
        seen_routes.add(route_id)
        for key in ("manifest_project", "checkout_path"):
            safe_relative(route[key], f"AOSP route {route_id} {key}")
        for key in ("base_commit", "base_tree", "final_tree"):
            require_hex(route[key], 40, f"AOSP route {route_id} {key}")
        require_integer(route["patch_count"], f"AOSP route {route_id} patch count", minimum=1)
        require_hex(
            route["series_manifest_sha256"], 64,
            f"AOSP route {route_id} series manifest SHA-256",
        )

    kernel = require_exact_keys(manifest["kernel"], KERNEL_KEYS, "kernel")
    if kernel["family"] != "android-6.18":
        fail("kernel family is not android-6.18")
    for key in ("repository", "branch", "tag", "release"):
        require_string(kernel[key], f"kernel.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(kernel[key], 40, f"kernel.{key}")
    require_hex(kernel["patch_manifest_sha256"], 64, "kernel patch manifest SHA-256")
    require_integer(kernel["patch_count"], "kernel patch count", minimum=1)
    if require_integer(kernel["page_size"], "kernel page size") != 16384:
        fail("kernel page size is not 16384")
    require_integer(kernel["abi_version"], "kernel ABI version", minimum=1)
    abi_fingerprint = require_string(kernel["abi_fingerprint"], "kernel ABI fingerprint")
    if re.fullmatch(r"0x[0-9a-f]{16}", abi_fingerprint) is None:
        fail("kernel ABI fingerprint has an unsafe format")
    if kernel["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    outer = require_exact_keys(manifest["outer"], OUTER_KEYS, "outer")
    require_hex(outer["revision"], 40, "outer revision")
    if outer["dirty"] is not False:
        fail("accepted release records a dirty outer project")

    producer = require_exact_keys(manifest["producer"], PRODUCER_KEYS, "producer")
    if producer["entry_point"] != "android/aosp-17/build/publish-release.sh":
        fail("release records an unexpected producer entry point")
    options = producer["functional_options"]
    if not isinstance(options, list) or not options:
        fail("producer functional options must be a nonempty array")
    for index, option in enumerate(options):
        require_string(option, f"producer option {index}")
    builder = require_exact_keys(producer["builder"], BUILDER_KEYS, "producer.builder")
    for key in BUILDER_KEYS:
        require_string(builder[key], f"producer.builder.{key}")
    if builder["architecture"] != "x86_64":
        fail("release was not produced on an x86_64 builder")

    inventory = manifest["inventory"]
    if not isinstance(inventory, list):
        fail("release inventory must be an array")
    expected_paths = set(PAYLOAD_SPECS)
    seen_paths: set[str] = set()
    ordered_paths: list[str] = []
    for index, raw_entry in enumerate(inventory):
        entry = require_exact_keys(raw_entry, INVENTORY_KEYS, f"inventory entry {index}")
        path = safe_relative(entry["path"], f"inventory entry {index} path")
        if path in seen_paths:
            fail(f"duplicate inventory path: {path}")
        seen_paths.add(path)
        ordered_paths.append(path)
        if path not in PAYLOAD_SPECS:
            fail(f"undeclared release payload: {path}")
        role, file_format = PAYLOAD_SPECS[path]
        if entry["role"] != role or entry["format"] != file_format:
            fail(f"inventory role or format differs for {path}")
        require_integer(entry["size"], f"inventory size for {path}", minimum=1)
    if seen_paths != expected_paths:
        fail(f"release inventory differs from its fixed contract: {sorted(expected_paths - seen_paths)}")
    if ordered_paths != sorted(ordered_paths):
        fail("release inventory is not sorted by path")
    return manifest


def sha256_path(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def verify_tracked_identity(manifest: dict, android_root: Path) -> None:
    aosp_metadata = parse_env(android_root / "metadata/aosp.env")
    kernel_root = android_root.parents[1] / "kernel/android-6.18"
    kernel_metadata = parse_env(kernel_root / "patches/metadata.env")
    abi_metadata = parse_env(android_root.parents[1] / "kernel/common/abi/identity.env")

    expected = {
        "manifest_repository": aosp_metadata["ANDROID17_MANIFEST_REPOSITORY"],
        "manifest_tag": aosp_metadata["ANDROID17_MANIFEST_TAG"],
        "manifest_commit": aosp_metadata["ANDROID17_MANIFEST_COMMIT"],
        "manifest_tree": aosp_metadata["ANDROID17_MANIFEST_TREE"],
        "base_manifest_sha256": aosp_metadata["ANDROID17_BASE_MANIFEST_SHA256"],
        "projects_manifest_sha256": sha256_path(android_root / "patches/projects.tsv"),
    }
    for key, value in expected.items():
        if manifest["aosp"][key] != value:
            fail(f"release AOSP identity differs from tracked {key}")

    kernel_expected = {
        "family": kernel_metadata["UML_KERNEL_FAMILY"],
        "repository": kernel_metadata["UML_KERNEL_UPSTREAM_REPOSITORY"],
        "branch": kernel_metadata["UML_KERNEL_UPSTREAM_BRANCH"],
        "tag": kernel_metadata["UML_KERNEL_UPSTREAM_TAG"],
        "base_commit": kernel_metadata["UML_KERNEL_UPSTREAM_COMMIT"],
        "base_tree": kernel_metadata["UML_KERNEL_UPSTREAM_TREE"],
        "final_tree": kernel_metadata["UML_KERNEL_FINAL_TREE"],
        "patch_count": int(kernel_metadata["UML_KERNEL_PATCH_COUNT"]),
        "patch_manifest_sha256": sha256_path(kernel_root / "patches/series/manifest.tsv"),
        "abi_version": int(abi_metadata["DARWIN_UML_ABI_VERSION"]),
        "abi_fingerprint": abi_metadata["DARWIN_UML_ABI_FINGERPRINT"],
    }
    for key, value in kernel_expected.items():
        if manifest["kernel"][key] != value:
            fail(f"release kernel identity differs from tracked {key}")


def verify_acceptance(manifest: dict, acceptance_path: Path) -> None:
    acceptance = parse_env(acceptance_path)
    if set(acceptance) != ACCEPTANCE_KEYS:
        fail("accepted-release descriptor keys differ from the fixed contract")
    expected = {
        "ANDROID17_ACCEPTED_SCHEMA": str(SCHEMA_VERSION),
        "ANDROID17_ACCEPTED_RELEASE_ID": manifest["release_id"],
        "ANDROID17_ACCEPTED_DIRECTORY_NAME": manifest["directory_name"],
        "ANDROID17_ACCEPTED_PRODUCT_FAMILY": manifest["product_family"],
        "ANDROID17_ACCEPTED_ANDROID_MAJOR": str(manifest["android"]["major"]),
        "ANDROID17_ACCEPTED_AOSP_MANIFEST_COMMIT": manifest["aosp"]["manifest_commit"],
        "ANDROID17_ACCEPTED_AOSP_BASE_MANIFEST_SHA256": manifest["aosp"]["base_manifest_sha256"],
        "ANDROID17_ACCEPTED_AOSP_PROJECTS_MANIFEST_SHA256": manifest["aosp"]["projects_manifest_sha256"],
        "ANDROID17_ACCEPTED_KERNEL_FINAL_TREE": manifest["kernel"]["final_tree"],
        "ANDROID17_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256": manifest["kernel"]["patch_manifest_sha256"],
        "ANDROID17_ACCEPTED_OUTER_REVISION": manifest["outer"]["revision"],
        "ANDROID17_ACCEPTED_PAGE_SIZE": str(manifest["kernel"]["page_size"]),
        "ANDROID17_ACCEPTED_ABI_VERSION": str(manifest["kernel"]["abi_version"]),
        "ANDROID17_ACCEPTED_ABI_FINGERPRINT": manifest["kernel"]["abi_fingerprint"],
    }
    for key, value in expected.items():
        if acceptance.get(key) != value:
            fail(f"release differs from accepted descriptor field {key}")


def inventory_map(manifest: dict) -> dict[str, dict]:
    return {entry["path"]: entry for entry in manifest["inventory"]}


def verify_inventory(release_root: Path, manifest: dict, *, allow_expanded_disk: bool = False) -> None:
    expected_files = set(PAYLOAD_SPECS) | {"release-manifest.json"}
    if allow_expanded_disk:
        expected_files.add("guest/android-17-uml-arm64.img")
    expected_dirs = {"."}
    for relative in expected_files:
        parent = PurePosixPath(relative).parent
        while str(parent) != ".":
            expected_dirs.add(str(parent))
            parent = parent.parent

    actual_files: set[str] = set()
    actual_dirs: set[str] = {"."}
    for root, directories, files in os.walk(release_root, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = root_path.relative_to(release_root)
        for name in directories:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                fail(f"unsafe release directory: {path.relative_to(release_root)}")
            actual_dirs.add(str((relative_root / name).as_posix()))
        for name in files:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                fail(f"unsafe release payload: {path.relative_to(release_root)}")
            actual_files.add(str((relative_root / name).as_posix()))
    if actual_files != expected_files:
        fail(
            "release file set differs from its manifest "
            f"(missing={sorted(expected_files - actual_files)}, "
            f"extra={sorted(actual_files - expected_files)})"
        )
    if actual_dirs != expected_dirs:
        fail(
            "release directory set differs from its manifest "
            f"(missing={sorted(expected_dirs - actual_dirs)}, "
            f"extra={sorted(actual_dirs - expected_dirs)})"
        )

    entries = inventory_map(manifest)
    for relative in PAYLOAD_SPECS:
        observed = (release_root / relative).stat().st_size
        if observed != entries[relative]["size"]:
            fail(f"release payload size differs for {relative}")


def parse_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("CONFIG_") and "=" in raw:
            key, value = raw.split("=", 1)
            values[key] = value
        elif raw.startswith("# CONFIG_") and raw.endswith(" is not set"):
            values[raw[2:-11]] = "n"
    return values


def verify_kernel_config(release_root: Path, android_root: Path) -> None:
    config = parse_config(release_root / "kernel/config")
    if config.get("CONFIG_MODULES") != "n":
        fail("release kernel does not have CONFIG_MODULES=n")
    module_values = sorted(key for key, value in config.items() if value == "m")
    if module_values:
        fail(f"release kernel contains loadable-module values: {module_values[:8]}")
    for path, expected in (
        (android_root / "support/verification/required-kernel-builtins.txt", "y"),
        (android_root / "support/verification/forbidden-kernel-options.txt", "n"),
    ):
        for raw in path.read_text(encoding="utf-8").splitlines():
            symbol = raw.strip()
            if not symbol or symbol.startswith("#"):
                continue
            observed = config.get(symbol, "n")
            if observed != expected:
                fail(f"release kernel config {symbol}={observed}, expected {expected}")


def run_checked(arguments: list[str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(arguments, env=env, check=False)
    if result.returncode != 0:
        fail(f"verification command failed ({result.returncode}): {arguments[0]}")


def resolve_zstd(value: Path | None) -> Path:
    if value is not None:
        candidate = value
    else:
        located = shutil.which("zstd")
        if located is None:
            fail("zstd is not available; pass --zstd with an absolute local path")
        candidate = Path(located)
    if not candidate.is_absolute():
        fail("zstd must be an absolute local executable")
    candidate = candidate.resolve()
    if not candidate.is_file():
        fail("zstd must be an absolute regular local executable")
    if not os.access(candidate, os.X_OK):
        fail("zstd is not executable")
    return candidate


def verify_semantics(
    release_root: Path,
    manifest: dict,
    android_root: Path,
    zstd: Path | None,
    expanded_disk: Path | None,
) -> None:
    run_checked([
        str(android_root / "build/verify-kernel-elf.py"),
        str(release_root / "kernel/vmlinux"),
    ])
    verify_kernel_config(release_root, android_root)
    run_checked([
        str(android_root / "tools/verify-initramfs.py"),
        str(release_root / "boot/ramdisk.img"),
    ])

    if (release_root / "boot/kernel-command-line.txt").read_bytes() != (
        android_root / "support/boot/kernel-command-line.txt"
    ).read_bytes():
        fail("release kernel command line differs from tracked policy")
    if (release_root / "guest/partition-layout.json").read_bytes() != (
        android_root / "support/image/partition-layout.json"
    ).read_bytes():
        fail("release partition layout differs from tracked policy")

    fstab = (release_root / "support/fstab.uml").read_text(encoding="utf-8")
    for name in ("system", "system_ext", "product", "vendor", "odm", "metadata", "userdata"):
        if f"/dev/block/by-name/{name}" not in fstab:
            fail(f"release fstab is missing fixed partition {name}")
    for forbidden in ("logical", "slotselect", "avb", "_dlkm"):
        if forbidden in fstab:
            fail(f"release fstab contains unsupported token: {forbidden}")

    for relative in (
        "support/vintf-manifest.xml",
        "support/compatibility-matrix.xml",
        "provenance/aosp-repo-manifest.xml",
    ):
        try:
            ET.parse(release_root / relative)
        except (OSError, ET.ParseError) as error:
            fail(f"invalid XML payload {relative}: {error}")

    receipt = (release_root / "provenance/build-receipt.tsv").read_text(
        encoding="utf-8"
    )
    for forbidden in ("/home/", "builder_host", "private_key", "\tssh\t", "\tscp\t"):
        if forbidden in receipt.lower():
            fail("build receipt contains builder endpoint, credential, or local path state")

    zstd_path = resolve_zstd(zstd)
    archive = release_root / "guest/android-17-uml-arm64.img.zst"
    run_checked([str(zstd_path), "-q", "-t", str(archive)])
    if expanded_disk is not None:
        if not expanded_disk.is_file() or expanded_disk.is_symlink():
            fail("expanded guest disk is missing or unsafe")
        run_checked([
            str(android_root / "tools/ubd-disk.py"),
            "verify",
            "--layout",
            str(release_root / "guest/partition-layout.json"),
            "--disk",
            str(expanded_disk),
        ])

    if manifest["kernel"]["page_size"] != 16384:
        fail("semantic verification received a non-16-KiB kernel")
    schema = android_root / "support/release/release-manifest.schema.json"
    if not schema.is_file():
        fail("tracked release-manifest schema is missing")


def verify_release(
    release_root: Path,
    android_root: Path,
    *,
    acceptance_path: Path | None = None,
    zstd: Path | None = None,
    expanded_disk: Path | None = None,
    allow_expanded_disk: bool = False,
    semantic: bool = True,
) -> dict:
    if not release_root.is_absolute():
        fail("release source must be an absolute local path")
    if not release_root.is_dir() or release_root.is_symlink():
        fail("release source must be a regular local directory")
    manifest = load_manifest(release_root)
    if release_root.name != manifest["directory_name"]:
        fail("release directory basename differs from its manifest")
    verify_tracked_identity(manifest, android_root)
    if acceptance_path is not None:
        verify_acceptance(manifest, acceptance_path)
    verify_inventory(release_root, manifest, allow_expanded_disk=allow_expanded_disk)
    if semantic:
        verify_semantics(release_root, manifest, android_root, zstd, expanded_disk)
    return manifest
