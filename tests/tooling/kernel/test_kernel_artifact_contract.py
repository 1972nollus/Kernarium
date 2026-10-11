#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the supported kernel artifact family, slice, and page matrix."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from types import ModuleType


SUPPORTED_IDENTITIES = {
    ("mainline-6.18", "arm64", 4096),
    ("mainline-6.18", "arm64", 16384),
    ("mainline-6.18", "x86_64", 4096),
    ("android-6.18", "arm64", 4096),
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
    diagnostic: str | None,
) -> None:
    image = root / f"{label}.elf"
    write_elf(image, sections)
    try:
        module.verify_runtime_image(image.read_bytes())
    except SystemExit as error:
        require(
            diagnostic is not None and diagnostic in str(error),
            f"runtime image {label} was rejected for the wrong reason: {error}",
        )
    else:
        require(diagnostic is None, f"runtime image {label} was accepted")


PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PF_R = 4
SHT_SYMTAB = 2
# The fixture's program headers: an R+X load, an R+W load and the GNU stack.
PROGRAM_HEADERS = (64, 120, 176)


def kernel_elf(imports: list[str]) -> bytearray:
    """Return a 16 KiB AArch64 ET_DYN kernel image that imports these names.

    Its .symtab also defines darwin_uml_defined_here, which is not an import.
    """
    names = bytearray(b"\0")
    symbols = bytearray(24)
    for name, section in [("darwin_uml_defined_here", 1)] + [
        (name, 0) for name in imports
    ]:
        symbols += struct.pack("<IBBHQQ", len(names), 0x10, 0, section, 0, 0)
        names += name.encode("ascii") + b"\0"
    section_names = b"\0.symtab\0.strtab\0.shstrtab\0"
    symtab = 64 + 56 * len(PROGRAM_HEADERS)
    strtab = symtab + len(symbols)
    shstrtab = strtab + len(names)
    section_table = shstrtab + len(section_names)
    image = bytearray(section_table + 64 * 4)
    image[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into(
        "<HHIQQQIHHHHHH", image, 16,
        3, 183, 1, 0, 64, section_table, 0, 64, 56, len(PROGRAM_HEADERS), 64, 4, 3,
    )
    for offset, (kind, flags, address, size, alignment) in zip(
        PROGRAM_HEADERS,
        (
            (PT_LOAD, PF_R | PF_X, 0, symtab, 0x4000),
            (PT_LOAD, PF_R | PF_W, 0x10000, symtab, 0x4000),
            (PT_GNU_STACK, PF_R | PF_W, 0, 0, 16),
        ),
    ):
        struct.pack_into(
            "<IIQQQQQQ", image, offset,
            kind, flags, 0, address, address, size, size, alignment,
        )
    image[symtab:section_table] = symbols + names + section_names
    for index, (name, kind, offset, size, link, entry_size) in enumerate(
        (
            (0, 0, 0, 0, 0, 0),
            (1, SHT_SYMTAB, symtab, len(symbols), 2, 24),
            (9, SHT_STRTAB, strtab, len(names), 0, 0),
            (17, SHT_STRTAB, shstrtab, len(section_names), 0, 0),
        )
    ):
        struct.pack_into(
            "<IIQQQQIIQQ", image, section_table + 64 * index,
            name, kind, 0, 0, offset, size, link, 0, 1, entry_size,
        )
    return image


def section_header(image: bytearray, index: int) -> int:
    return struct.unpack_from("<Q", image, 40)[0] + 64 * index


def write_bridge_policy(abi: Path, rows: list[str]) -> None:
    (abi / "import-policy.tsv").write_text(
        "\n".join(["symbol\tavailability\trationale", *rows]) + "\n",
        encoding="utf-8",
    )


def require_kernel_elf(
    module: ModuleType,
    path: Path,
    image: bytes,
    common_root: Path,
    page_size: int,
    kernel_slice: str,
    diagnostic: str | None,
) -> None:
    path.write_bytes(image)
    try:
        module.verify_elf(path, page_size, common_root, kernel_slice)
    except SystemExit as error:
        require(
            diagnostic is not None and diagnostic in str(error),
            f"kernel ELF was rejected for the wrong reason: {error}",
        )
    else:
        require(diagnostic is None, f"kernel ELF was accepted, expected: {diagnostic}")


def verify_kernel_elf_contract(module: ModuleType, root: Path) -> None:
    """Pin the kernel ELF's geometry checks and its bridge import policy."""
    abi = root / "common/abi"
    abi.mkdir(parents=True)
    (abi / "darwin_uml_fixture.h").write_text(
        "int darwin_uml_everywhere(void);\n"
        "int darwin_uml_small_pages(int value);\n"
        "void darwin_uml_arm64_only(void);\n",
        encoding="utf-8",
    )
    policy = [
        "darwin_uml_small_pages\t4k-only\tfixture",
        "darwin_uml_arm64_only\tarm64-only\tfixture",
    ]
    write_bridge_policy(abi, policy)
    common = root / "common"
    path = root / "vmlinux"
    everywhere, small, arm64 = (
        "darwin_uml_everywhere",
        "darwin_uml_small_pages",
        "darwin_uml_arm64_only",
    )
    active_16k = [everywhere, arm64]

    def check(
        image: bytes,
        diagnostic: str | None,
        page_size: int = 16384,
        kernel_slice: str = "arm64",
    ) -> None:
        require_kernel_elf(
            module, path, image, common, page_size, kernel_slice, diagnostic
        )

    # Each page profile and slice imports exactly its active symbols.
    check(kernel_elf(active_16k), None)
    check(kernel_elf([everywhere, small, arm64]), None, 4096)
    check(kernel_elf([everywhere, small]), None, 4096, "x86_64")
    for imports, page_size, kernel_slice, missing, extra in (
        ([everywhere, small, arm64], 16384, "arm64", [], [small]),
        ([everywhere, small, arm64], 4096, "x86_64", [], [arm64]),
        ([everywhere, small], 4096, "arm64", [arm64], []),
        ([everywhere], 16384, "arm64", [arm64], []),
        ([*active_16k, "darwin_uml_undeclared"], 16384, "arm64", [],
         ["darwin_uml_undeclared"]),
    ):
        check(
            kernel_elf(imports),
            f"kernel bridge imports differ (missing={missing}, extra={extra})",
            page_size,
            kernel_slice,
        )
    check(kernel_elf(active_16k), "unsupported kernel slice", 16384, "riscv64")
    check(
        kernel_elf([everywhere, small]),
        "x86_64 bridge imports require 4096-byte pages",
        16384,
        "x86_64",
    )

    # A 4 KiB image may align its loads to 4 KiB; a 16 KiB image may not.
    small_alignment = kernel_elf([everywhere, small, arm64])
    for offset in PROGRAM_HEADERS[:2]:
        struct.pack_into("<Q", small_alignment, offset + 48, 0x1000)
    check(small_alignment, None, 4096)

    def mutated(layout: str, offset: int, value: int) -> bytearray:
        image = kernel_elf(active_16k)
        struct.pack_into(layout, image, offset, value)
        return image

    load, data, stack = PROGRAM_HEADERS
    not_elf64 = "kernel is not a little-endian ELF64 image"
    not_et_dyn = "kernel is not an AArch64 ET_DYN image"
    unseparated = "kernel lacks separated executable and writable load segments"
    for layout, offset, value, diagnostic in (
        ("<B", 4, 1, not_elf64),
        ("<B", 5, 2, not_elf64),
        ("<B", 6, 0, not_elf64),
        ("<H", 16, 2, not_et_dyn),
        ("<H", 18, 62, not_et_dyn),
        ("<H", 54, 32, "ELF program-header entry size is malformed"),
        ("<H", 56, 1000, "ELF program-header table is outside the file"),
        ("<Q", load + 40, 1, "ELF program header 0 has file size above memory size"),
        ("<Q", data + 8, 0x40000, "ELF segment 1 is outside the file"),
        ("<I", stack + 4, PF_R | PF_W | PF_X, "kernel GNU stack is executable"),
        ("<I", data + 4, PF_R | PF_W | PF_X,
         "ELF load segment 1 is writable and executable"),
        ("<Q", load + 48, 0x1000, "ELF load segment 0 has invalid alignment 0x1000"),
        ("<Q", load + 48, 0x6000, "ELF load segment 0 has invalid alignment 0x6000"),
        ("<Q", data + 16, 0x10010, "ELF load segment 1 violates alignment congruence"),
        ("<I", data, 0, unseparated),
        ("<I", data + 4, PF_R, unseparated),
    ):
        check(mutated(layout, offset, value), diagnostic)

    symbols = section_header(kernel_elf(active_16k), 1)
    strings = section_header(kernel_elf(active_16k), 2)
    unterminated = kernel_elf(active_16k)
    string_offset, string_size = struct.unpack_from("<QQ", unterminated, strings + 24)
    unterminated[string_offset + string_size - 1] = ord("x")
    for image, diagnostic in (
        (mutated("<I", symbols + 4, 1), "ELF contains no symbol table"),
        (mutated("<Q", symbols + 56, 16), "ELF symbol table 1 is malformed"),
        (mutated("<Q", symbols + 32, 25), "ELF symbol table 1 is malformed"),
        (mutated("<I", symbols + 40, 9),
         "ELF symbol table 1 has an invalid string table"),
        (unterminated, "ELF symbol name is unterminated"),
    ):
        check(image, diagnostic)

    # Host code must not import the process-wide sigprocmask or the variadic
    # fcntl, in any Mach-O spelling.
    for symbol, description in (
        ("sigprocmask", "the process-wide Darwin sigprocmask"),
        ("_sigprocmask$UNIX2003", "the process-wide Darwin sigprocmask"),
        ("fcntl", "the variadic Darwin fcntl"),
        ("_fcntl$NOCANCEL", "the variadic Darwin fcntl"),
    ):
        check(kernel_elf([*active_16k, symbol]), f"the Linux image imports {description}")
    check(kernel_elf([*active_16k, "pthread_sigmask"]), None)

    # Kernel builds run the module's command line on the image they built. The
    # copy in the fixture directory reads the fixture declarations and policy.
    command_line = abi / "bridge_imports.py"
    shutil.copyfile(
        Path(module.__file__).resolve().parents[1] / "abi/bridge_imports.py",
        command_line,
    )

    def run(*arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(command_line), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    for page, kernel_slice, symbols in (
        ("16k", "arm64", active_16k),
        ("4k", "arm64", [everywhere, small, arm64]),
        ("4k", "x86_64", [everywhere, small]),
    ):
        printed = run("--print-expected", "--page-size", page, "--slice", kernel_slice)
        require(
            printed.returncode == 0 and printed.stdout.split() == sorted(symbols),
            f"{page} {kernel_slice} expected imports printed wrongly: {printed}",
        )
    for imports, diagnostic in (
        (active_16k, None),
        ([everywhere, small, arm64], "kernel bridge imports differ"),
        ([*active_16k, "_fcntl"], "the Linux image imports the variadic Darwin fcntl"),
    ):
        path.write_bytes(kernel_elf(imports))
        result = run("--kernel-binary", str(path), "--page-size", "16k", "--slice", "arm64")
        require(
            (result.returncode == 0) == (diagnostic is None)
            and (diagnostic is None or diagnostic in result.stderr),
            f"bridge import command line misjudged {imports}: {result.stderr}",
        )

    # The policy is a three-column table of declared conditional symbols.
    for row in (
        "darwin_uml_everywhere\t4k-only",
        "darwin_uml_everywhere\t16k-only\tfixture",
        "darwin_uml_everywhere\t4k-only\t",
        "darwin_uml_small_pages\tarm64-only\tagain",
        "uml_everywhere\t4k-only\tfixture",
    ):
        write_bridge_policy(abi, [*policy, row])
        check(kernel_elf(active_16k), "ABI import policy is malformed at line 4")
    write_bridge_policy(abi, [*policy, "darwin_uml_undeclared\t4k-only\tfixture"])
    check(kernel_elf(active_16k), "ABI import policy names undeclared bridge symbols")
    (abi / "import-policy.tsv").write_text(
        "symbol\tavailability\n" + "\n".join(policy) + "\n", encoding="utf-8"
    )
    check(kernel_elf(active_16k), "ABI import policy is malformed")
    (abi / "import-policy.tsv").write_text("", encoding="utf-8")
    check(kernel_elf(active_16k), "ABI import policy is malformed")
    write_bridge_policy(abi, policy)
    (abi / "darwin_uml_fixture.h").unlink()
    check(kernel_elf(active_16k), "shared ABI declarations are missing")


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
        16384,
        "android-6.18 family requires arm64 and 4096-byte pages",
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
        require_runtime_image(module, root, "btf", [text, btf], None)
        require_runtime_image(
            module,
            root,
            "dwarf",
            [text, btf, (".debug_info", SHT_PROGBITS, 0)],
            "kernel image carries DWARF sections: .debug_info",
        )
        require_runtime_image(
            module,
            root,
            "missing-btf",
            [text],
            "kernel config enables BTF but the image has no .BTF contents",
        )
        require_runtime_image(
            module,
            root,
            "unallocated-btf",
            [text, (".BTF", SHT_PROGBITS, 0)],
            "kernel .BTF section is not allocated",
        )
        verify_kernel_elf_contract(module, root)

    print("kernel artifact family/slice/page contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
