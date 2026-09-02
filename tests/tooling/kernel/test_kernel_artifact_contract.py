#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the supported kernel artifact family, slice, and page matrix."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
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
) -> dict:
    release_id = "contract-test"
    return {
        "schema_version": module.SCHEMA_VERSION,
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
            "patch_count": 1,
            "patch_manifest_sha256": "3" * 64,
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
    }


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

    print("kernel artifact family/slice/page contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
