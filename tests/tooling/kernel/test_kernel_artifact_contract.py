#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the supported kernel artifact family, slice, and page matrix."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import struct
import sys
import tempfile
from pathlib import Path
from types import ModuleType


SUPPORTED_IDENTITIES = {
    ("mainline-6.18", "arm64", 4096),
    ("mainline-6.18", "arm64", 16384),
    ("mainline-6.18", "x86_64", 4096),
    ("android-6.18", "arm64", 16384),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_artifact_module(path: Path) -> ModuleType:
    # The module imports its sibling contract_checks the way its callers do.
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location("kernel_artifact_contract", path)
    require(spec is not None and spec.loader is not None, "cannot load artifact module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
    schema: int | None = None,
) -> dict:
    release_id = "contract-test"
    value = {
        "schema_version": module.SCHEMA_VERSION if schema is None else schema,
        "release_id": release_id,
        "directory_name": (
            f"{family}-{kernel_slice}-{page_size // 1024}k-{release_id}"
        ),
        "family": family,
        "slice": kernel_slice,
        "published_at_utc": "2026-01-01T00:00:00Z",
        "release": "6.18.0-contract-test",
        "page_size": page_size,
        "elf_role": "darwin-uml",
        "source": {
            "repository": "https://example.invalid/linux.git",
            "branch": "contract-test",
            "base_commit": "0" * 40,
            "base_tree": "1" * 40,
            "final_tree": "2" * 40,
        },
        "abi": {
            "version": 1,
            "fingerprint": "0x0000000000000001",
        },
        "inventory": [
            {
                "path": path,
                "role": role,
                "format": file_format,
                "size": 1,
            }
            for path, (role, file_format) in sorted(module.PAYLOAD_SPECS.items())
        ],
    }
    if value["schema_version"] == 2:
        # Schema 2 bundles also named their patch count and series manifest.
        value["source"]["patch_count"] = 1
        value["source"]["patch_manifest_sha256"] = "3" * 64
    return value


def require_accepted(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
) -> None:
    module.validate_manifest(manifest(module, family, kernel_slice, page_size))


def require_rejected(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
    diagnostic: str,
) -> None:
    try:
        module.validate_manifest(manifest(module, family, kernel_slice, page_size))
    except SystemExit as error:
        require(
            diagnostic in str(error),
            f"invalid identity was rejected for the wrong reason: {error}",
        )
    else:
        raise AssertionError(
            "invalid kernel artifact identity was accepted: "
            f"{family}, {kernel_slice}, {page_size}"
        )


def schema_identities(path: Path) -> set[tuple[str, str, int]]:
    schema = json.loads(path.read_text(encoding="utf-8"))
    identities: set[tuple[str, str, int]] = set()
    for branch in schema["oneOf"]:
        properties = branch["properties"]
        family = properties["family"]["const"]
        kernel_slice = properties["slice"]["const"]
        page_rule = properties["page_size"]
        page_sizes = (
            [page_rule["const"]]
            if "const" in page_rule
            else page_rule["enum"]
        )
        identities.update(
            (family, kernel_slice, page_size) for page_size in page_sizes
        )
    return identities


def config_values(kernel_slice: str) -> dict[str, str]:
    return {
        "CONFIG_64BIT": "y",
        "CONFIG_ARM64": "y",
        "CONFIG_UML": "y",
        "CONFIG_MODULES": "n",
        "CONFIG_PAGE_SHIFT": "12",
        "CONFIG_PAGE_SIZE_16KB": "n",
        "CONFIG_PAGE_SIZE_4KB": "y",
        "CONFIG_UML_X86_64_USERSPACE_ONLY": (
            "y" if kernel_slice == "x86_64" else "n"
        ),
        "CONFIG_BPF": "y",
        "CONFIG_BPF_SYSCALL": "y",
        "CONFIG_CGROUP_BPF": "y",
        "CONFIG_DEBUG_INFO_BTF": "y",
    }


SHT_PROGBITS = 1
SHT_STRTAB = 3
SHF_ALLOC = 2


def write_elf(path: Path, sections: list[tuple[str, int, int]]) -> None:
    """Write an ELF64 file whose named sections each hold 16 bytes."""
    names = b"\0"
    name_offsets = []
    for name, _, _ in sections:
        name_offsets.append(len(names))
        names += name.encode("ascii") + b"\0"
    names_name = len(names)
    names += b".shstrtab\0"
    data_offset = 64
    payload = b""
    headers = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for (_, kind, flags), name in zip(sections, name_offsets):
        headers.append(
            struct.pack(
                "<IIQQQQIIQQ",
                name, kind, flags, 0, data_offset + len(payload), 16, 0, 0, 1, 0,
            )
        )
        payload += b"\xa5" * 16
    headers.append(
        struct.pack(
            "<IIQQQQIIQQ",
            names_name, SHT_STRTAB, 0, 0, data_offset + len(payload),
            len(names), 0, 0, 1, 0,
        )
    )
    payload += names
    section_offset = data_offset + len(payload)
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<Q", header, 40, section_offset)
    struct.pack_into("<HHH", header, 58, 64, len(headers), len(headers) - 1)
    path.write_bytes(bytes(header) + payload + b"".join(headers))


def require_runtime_image(
    module: ModuleType,
    root: Path,
    label: str,
    sections: list[tuple[str, int, int]],
    btf: str,
    diagnostic: str | None,
) -> None:
    image = root / f"{label}.elf"
    config = root / f"{label}.config"
    write_elf(image, sections)
    write_config(config, {"CONFIG_DEBUG_INFO_BTF": btf})
    try:
        module.verify_runtime_image(image, config)
    except SystemExit as error:
        require(
            diagnostic is not None and diagnostic in str(error),
            f"runtime image {label} was rejected for the wrong reason: {error}",
        )
    else:
        require(diagnostic is None, f"runtime image {label} was accepted")


def write_config(path: Path, values: dict[str, str]) -> None:
    lines = [
        f"# {symbol} is not set" if value == "n" else f"{symbol}={value}"
        for symbol, value in sorted(values.items())
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def require_config_accepted(
    module: ModuleType,
    root: Path,
    kernel_slice: str,
    values: dict[str, str],
) -> None:
    path = root / f"{kernel_slice}-accepted.config"
    write_config(path, values)
    module.verify_config(path, 4096, kernel_slice)


def require_config_rejected(
    module: ModuleType,
    root: Path,
    kernel_slice: str,
    values: dict[str, str],
    symbol: str,
    wanted: str,
) -> None:
    observed = values[symbol]
    path = root / f"{kernel_slice}-{symbol}.config"
    write_config(path, values)
    try:
        module.verify_config(path, 4096, kernel_slice)
    except SystemExit as error:
        diagnostic = f"kernel config {symbol}={observed}, expected {wanted}"
        require(
            diagnostic in str(error),
            f"invalid kernel config was rejected for the wrong reason: {error}",
        )
    else:
        raise AssertionError(
            f"invalid {kernel_slice} kernel config accepted {symbol}={observed}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-module", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    arguments = parser.parse_args()
    module = load_artifact_module(arguments.artifact_module.resolve())

    require(
        schema_identities(arguments.schema.resolve()) == SUPPORTED_IDENTITIES,
        "artifact JSON schema exposes the wrong identity matrix",
    )
    for identity in SUPPORTED_IDENTITIES:
        require_accepted(module, *identity)
        # Accepted schema 2 bundles stay importable beside schema 3 ones.
        module.validate_manifest(manifest(module, *identity, schema=2))
    legacy = manifest(module, "mainline-6.18", "arm64", 16384)
    legacy["source"]["patch_count"] = 1
    try:
        module.validate_manifest(legacy)
    except SystemExit as error:
        require("source" in str(error), f"schema 3 patch count rejected oddly: {error}")
    else:
        raise AssertionError("a schema 3 manifest accepted a patch count")

    require_rejected(
        module,
        "android-6.18",
        "arm64",
        4096,
        "android-6.18 family requires arm64 and 16384-byte pages",
    )
    require_rejected(
        module,
        "android-6.18",
        "x86_64",
        4096,
        "x86_64 kernel slice requires the mainline-6.18 family",
    )
    require_rejected(
        module,
        "mainline-6.18",
        "x86_64",
        16384,
        "x86_64 kernel slice requires 4096-byte pages",
    )

    with tempfile.TemporaryDirectory(prefix="uml-kernel-config-contract-") as raw:
        root = Path(raw)
        for kernel_slice in ("arm64", "x86_64"):
            expected = config_values(kernel_slice)
            require_config_accepted(module, root, kernel_slice, expected)
            for symbol in (
                "CONFIG_BPF",
                "CONFIG_BPF_SYSCALL",
                "CONFIG_CGROUP_BPF",
                "CONFIG_DEBUG_INFO_BTF",
            ):
                invalid = dict(expected)
                invalid[symbol] = "n" if expected[symbol] == "y" else "y"
                require_config_rejected(
                    module,
                    root,
                    kernel_slice,
                    invalid,
                    symbol,
                    expected[symbol],
                )

        text = (".text", SHT_PROGBITS, SHF_ALLOC)
        btf = (".BTF", SHT_PROGBITS, SHF_ALLOC)
        require_runtime_image(module, root, "btf", [text, btf], "y", None)
        require_runtime_image(
            module,
            root,
            "dwarf",
            [text, btf, (".debug_info", SHT_PROGBITS, 0)],
            "y",
            "kernel image carries DWARF sections: .debug_info",
        )
        require_runtime_image(
            module,
            root,
            "missing-btf",
            [text],
            "y",
            "kernel config enables BTF but the image has no .BTF contents",
        )
        require_runtime_image(
            module,
            root,
            "unallocated-btf",
            [text, (".BTF", SHT_PROGBITS, 0)],
            "y",
            "kernel .BTF section is not allocated",
        )
        require_runtime_image(module, root, "no-btf", [text], "n", None)

    print("kernel artifact family/slice/page contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
