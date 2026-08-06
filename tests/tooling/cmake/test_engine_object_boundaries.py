#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit concrete engine object ownership and the assembly entry shape."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import bisect
import hashlib
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


def run(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(command)}\n{result.stdout}")
    return result.stdout


def object_list(value: str) -> list[Path]:
    result = [Path(entry) for entry in value.split("|") if entry]
    if not result:
        raise RuntimeError("configured object list is empty")
    missing = [str(path) for path in result if not path.is_file()]
    if missing:
        raise RuntimeError(f"configured objects are missing: {missing}")
    return result


def archive_members(path: Path) -> list[tuple[str, bytes]]:
    """Read ordinary and BSD extended-name members from a static archive."""
    contents = path.read_bytes()
    require(contents.startswith(b"!<arch>\n"), f"not an archive: {path}")
    members: list[tuple[str, bytes]] = []
    offset = 8
    while offset < len(contents):
        require(offset + 60 <= len(contents), f"truncated archive header: {path}")
        header = contents[offset : offset + 60]
        require(header[58:60] == b"`\n", f"invalid archive header: {path}")
        raw_name = header[:16].decode("ascii").rstrip()
        try:
            raw_size = int(header[48:58].decode("ascii").strip())
        except ValueError as error:
            raise RuntimeError(f"invalid archive member size: {path}") from error
        data_start = offset + 60
        data_end = data_start + raw_size
        require(data_end <= len(contents), f"truncated archive member: {path}")
        payload_start = data_start
        if raw_name.startswith("#1/"):
            try:
                name_size = int(raw_name[3:])
            except ValueError as error:
                raise RuntimeError(f"invalid extended archive name: {path}") from error
            require(name_size <= raw_size, f"invalid archive name size: {path}")
            name = (
                contents[data_start : data_start + name_size]
                .rstrip(b"\0")
                .decode("utf-8")
            )
            payload_start += name_size
        else:
            name = raw_name.rstrip("/")
        members.append((name, contents[payload_start:data_end]))
        offset = data_end + (raw_size & 1)
    require(offset == len(contents), f"misaligned archive payload: {path}")
    return members


def archive_object_members(archive: Path) -> Counter[tuple[str, bytes]]:
    require(archive.is_file(), f"static archive is missing: {archive}")
    return Counter(
        (name, hashlib.sha256(payload).digest())
        for name, payload in archive_members(archive)
        if not name.startswith("__.SYMDEF")
    )


def verify_archive_copy(original: Path, installed: Path) -> None:
    original_members = archive_object_members(original)
    installed_members = archive_object_members(installed)
    missing = original_members - installed_members
    unexpected = installed_members - original_members
    require(
        not missing and not unexpected,
        "installed archive object payload differs from the built archive: "
        f"missing={[name for name, _digest in missing]}, "
        f"unexpected={[name for name, _digest in unexpected]}",
    )


def verify_production_archive(archive: Path, objects: list[Path]) -> None:
    actual = archive_object_members(archive)
    expected = Counter(
        (path.name, hashlib.sha256(path.read_bytes()).digest()) for path in objects
    )
    missing = expected - actual
    unexpected = actual - expected
    require(
        not missing,
        "production archive does not contain every configured object "
        "byte-for-byte: "
        + ", ".join(name for name, _digest in missing),
    )
    require(
        not unexpected,
        "production archive contains an object outside the configured "
        "production graph: "
        + ", ".join(name for name, _digest in unexpected),
    )
    member_names = {name for name, _payload in archive_members(archive)}
    forbidden = {
        "jit_executor.cpp.o",
        "jit_test_support.cpp.o",
        "program.cpp.o",
    } & member_names
    require(
        not forbidden,
        f"production archive contains a removed or test-only object: {sorted(forbidden)}",
    )


def verify_native_page_profile(
    archive: Path, production_symbols: set[str], guest_page_size: int
) -> None:
    """Keep 4 KiB host-slot adaptation out of the 16 KiB product."""
    if guest_page_size != 16384:
        return

    forbidden_symbol_markers = (
        "NativeAddressSpace7Mapping",
        "NativeAddressSpace16load_page_locked",
        "NativeAddressSpace17flush_page_locked",
        "NativeAddressSpace17flush_slot_locked",
        "NativeAddressSpace23materialize_slot_locked",
        "NativeAddressSpace26set_slot_protection_locked",
        "NativeAddressSpace29update_slot_protection_locked",
        "NativeAddressSpace3mapE",
        "NativeAddressSpace5flushE",
        "NativeAddressSpace5retryE",
        "NativeAddressSpace5unmapE",
        "NativeAddressSpace7protectE",
        "NativeAddressSpace7refreshE",
    )
    forbidden_symbols = {
        symbol
        for symbol in production_symbols
        if any(marker in symbol for marker in forbidden_symbol_markers)
    }
    require(
        not forbidden_symbols,
        "16 KiB production archive contains 4 KiB native-MMU symbols: "
        f"{sorted(forbidden_symbols)}",
    )

    native_object_names = {
        "native_address_space.cpp.o",
        "static_api.cpp.o",
    }
    forbidden_payload_markers = (b"shadow", b"quarter")
    contaminated_members = {
        name: [
            marker.decode("ascii")
            for marker in forbidden_payload_markers
            if marker in payload.lower()
        ]
        for name, payload in archive_members(archive)
        if name in native_object_names
    }
    contaminated_members = {
        name: markers for name, markers in contaminated_members.items() if markers
    }
    require(
        not contaminated_members,
        "16 KiB production archive contains 4 KiB native-MMU payload markers: "
        f"{contaminated_members}",
    )


def verify_gadget_builder_ownership(
    source_root: Path, gadget_objects: list[Path]
) -> None:
    """Keep each decode family in a normal, independently compiled owner."""
    gadget_root = source_root / "darwin" / "cpu" / "arm64" / "gadgets"
    family_sources = {
        "decode_branch_system": "program_builder_branch_system.cpp",
        "decode_data_processing": "program_builder_data_processing.cpp",
        "decode_exception": "program_builder_exception.cpp",
        "decode_floating_simd": "program_builder_floating_simd.cpp",
        "decode_load_store": "program_builder_load_store.cpp",
    }
    required_objects = {
        "program_builder.cpp.o",
        *(f"{source}.o" for source in family_sources.values()),
    }
    object_names = {path.name for path in gadget_objects}
    require(
        required_objects <= object_names,
        "gadget decode-family source ownership is incomplete: "
        f"{sorted(object_names)}",
    )

    interface = (gadget_root / "program_builder_decode.hpp").read_text(
        encoding="utf-8"
    )
    orchestrator = (gadget_root / "program_builder.cpp").read_text(
        encoding="utf-8"
    )
    for function, source_name in family_sources.items():
        declaration = re.compile(
            rf"\[\[nodiscard\]\]\s+DecodeResult\s+{function}"
            rf"\(DecodeContext &context\);"
        )
        definition = re.compile(
            rf"\bDecodeResult\s+{function}\(DecodeContext &context\)\s*\{{"
        )
        require(
            declaration.search(interface) is not None,
            f"private builder interface does not declare {function}",
        )
        require(
            definition.search(interface) is None,
            f"private builder interface implements {function}",
        )
        owner = (gadget_root / source_name).read_text(encoding="utf-8")
        require(
            definition.search(owner) is not None,
            f"{source_name} does not own {function}",
        )
        require(
            definition.search(orchestrator) is None and function in orchestrator,
            f"program_builder.cpp does not remain a caller-only owner for {function}",
        )

    textual_cpp_include = re.compile(
        r'^\s*#\s*include\s*[<"][^>"]+\.(?:cpp|cc|cxx|inc|inl|ipp)[>"]',
        re.MULTILINE,
    )
    builder_sources = [
        gadget_root / "program_builder.cpp",
        *(gadget_root / source for source in family_sources.values()),
    ]
    composed = {
        path.name
        for path in builder_sources
        if textual_cpp_include.search(path.read_text(encoding="utf-8"))
    }
    require(
        not composed,
        f"gadget builder uses textual C++ composition: {sorted(composed)}",
    )
    fragment_files = sorted(
        path.name
        for path in gadget_root.glob("program_builder*")
        if path.suffix in (".inc", ".inl", ".ipp") or "_impl" in path.stem
    )
    require(
        not fragment_files,
        f"gadget builder has implementation fragments: {fragment_files}",
    )


def symbols(nm: Path, objects: list[Path], undefined: bool) -> set[str]:
    result: set[str] = set()
    arguments = ["-u"] if undefined else ["-gU"]
    for path in objects:
        for line in run([str(nm), *arguments, str(path)]).splitlines():
            fields = line.split()
            if fields and not line.endswith(":"):
                result.add(fields[-1])
    return result


def strong_external_owners(
    nm: Path, objects: list[Path]
) -> dict[str, list[Path]]:
    owners: dict[str, list[Path]] = {}
    for path in objects:
        for line in run([str(nm), "-m", str(path)]).splitlines():
            if (
                " external " not in line
                or "(undefined)" in line
                or "weak external" in line
                or "private external" in line
            ):
                continue
            fields = line.split()
            if fields:
                owners.setdefault(fields[-1], []).append(path)
    return owners


def normalized(symbols_to_normalize: set[str]) -> set[str]:
    return {
        symbol[1:] if symbol.startswith("_uml_") else symbol
        for symbol in symbols_to_normalize
    }


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def entry_mnemonics(disassembler: Path, assembly_object: Path) -> list[str]:
    output = run([str(disassembler), "-tvV", str(assembly_object)])
    lines = output.splitlines()
    start = next(
        (
            index + 1
            for index, line in enumerate(lines)
            if line.strip() in (
                "_uml_arm64_gadget_enter:",
                "uml_arm64_gadget_enter:",
            )
        ),
        None,
    )
    require(start is not None, "disassembly has no engine entry label")
    mnemonics: list[str] = []
    for line in lines[start:]:
        if line and not line[0].isspace() and line.endswith(":"):
            break
        match = re.match(r"^[0-9a-fA-F]+\s+([a-z][a-z0-9.]*)\b", line)
        if match:
            mnemonics.append(match.group(1))
    return mnemonics


def gadget_reachability(
    nm: Path,
    disassembler: Path,
    assembly_object: Path,
    gadget_objects: list[Path],
    source_root: Path,
) -> tuple[int, int, int, int]:
    """Build compiled, source-table, and assembly-internal symbol edges."""
    gadget_prefix = "uml_arm64_gadget_"

    def canonical(symbol: str) -> str:
        return symbol[1:] if symbol.startswith("_uml_") else symbol

    defined: set[str] = set()
    for line in run([str(nm), "-U", str(assembly_object)]).splitlines():
        fields = line.split()
        if fields and gadget_prefix in fields[-1]:
            defined.add(canonical(fields[-1]))
    require(defined, "gadget assembly object defines no gadget symbols")

    non_assembly = [path for path in gadget_objects if path != assembly_object]
    compiled_roots = {
        canonical(symbol)
        for symbol in symbols(nm, non_assembly, True)
        if gadget_prefix in symbol
    } & defined

    # Optional translation-time tables may be compiled out on the current
    # host. Keep their real source references as configuration roots while
    # deliberately excluding the generated declaration catalog.
    source_roots: set[str] = set()
    gadget_source_root = source_root / "darwin" / "cpu" / "arm64" / "gadgets"
    for path in gadget_source_root.rglob("*"):
        if path.suffix not in (".cpp", ".hpp") or path.name == "gadget_symbols.hpp":
            continue
        content = path.read_text(encoding="utf-8")
        source_roots.update(re.findall(r"\buml_arm64_gadget_[A-Za-z0-9_]+", content))
    source_roots &= defined

    disassembly = run([str(disassembler), "-tvV", str(assembly_object)])
    edges: dict[str, set[str]] = {symbol: set() for symbol in defined}
    symbol_at_address: list[tuple[int, str]] = []
    current: str | None = None
    label_pattern = re.compile(r"^(_?uml_arm64_gadget_[A-Za-z0-9_]+):$")
    address_pattern = re.compile(r"^([0-9a-fA-F]+)\s")
    target_pattern = re.compile(r"_?(uml_arm64_gadget_[A-Za-z0-9_]+)")
    for line in disassembly.splitlines():
        label = label_pattern.match(line)
        if label:
            current = canonical(label.group(1))
            continue
        if current is None:
            continue
        address = address_pattern.match(line)
        if address and (
            not symbol_at_address or symbol_at_address[-1][1] != current
        ):
            symbol_at_address.append((int(address.group(1), 16), current))
        for target in target_pattern.findall(line):
            target = canonical(target)
            if target in defined and target != current:
                edges[current].add(target)

    # Relocations cover data tables and externalized assembly references.
    # Map each relocation address back to its containing gadget body.
    starts = [entry[0] for entry in symbol_at_address]
    relocations = run([str(disassembler), "-rv", str(assembly_object)])
    relocation_pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+.*\s(_?uml_arm64_gadget_[A-Za-z0-9_]+)$"
    )
    for line in relocations.splitlines():
        relocation = relocation_pattern.match(line)
        if not relocation or not starts:
            continue
        index = bisect.bisect_right(starts, int(relocation.group(1), 16)) - 1
        if index < 0:
            continue
        source = symbol_at_address[index][1]
        target = canonical(relocation.group(2))
        if target in defined and target != source:
            edges[source].add(target)

    roots = compiled_roots | source_roots | {"uml_arm64_gadget_enter"}
    reachable: set[str] = set()
    pending = list(roots & defined)
    while pending:
        symbol = pending.pop()
        if symbol in reachable:
            continue
        reachable.add(symbol)
        pending.extend(edges[symbol] - reachable)
    unreachable = sorted(defined - reachable)
    require(
        not unreachable,
        "unreachable gadget symbols remain after compiled/source/table/assembly "
        f"graph traversal: {unreachable}",
    )
    return len(defined), len(compiled_roots), len(source_roots), len(reachable)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--disassembler", type=Path, required=True)
    parser.add_argument("--jit-objects", required=True)
    parser.add_argument("--executor-objects", required=True)
    parser.add_argument("--gadget-objects", required=True)
    parser.add_argument("--neutral-objects", required=True)
    parser.add_argument("--adapter-objects", action="append", required=True)
    parser.add_argument(
        "--configured-production-objects", action="append", required=True
    )
    parser.add_argument("--production-archive", type=Path, required=True)
    parser.add_argument(
        "--guest-page-size", type=int, choices=(4096, 16384), required=True
    )
    parser.add_argument("--ordinary-product", type=Path, action="append", default=[])
    parser.add_argument("--test-hook-objects", action="append", default=[])
    parser.add_argument(
        "--diagnostic-executor-objects", action="append", default=[]
    )
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--allow-profile", action="store_true")
    parser.add_argument("--conformance-binary", type=Path)
    options = parser.parse_args()

    jit_objects = object_list(options.jit_objects)
    executor_objects = object_list(options.executor_objects)
    gadget_objects = object_list(options.gadget_objects)
    neutral_objects = object_list(options.neutral_objects)
    adapter_objects = [
        path for value in options.adapter_objects for path in object_list(value)
    ]
    production_objects = [
        path
        for value in options.configured_production_objects
        for path in object_list(value)
    ]
    test_hook_objects = [
        path for value in options.test_hook_objects for path in object_list(value)
    ]
    diagnostic_executor_objects = [
        path
        for value in options.diagnostic_executor_objects
        for path in object_list(value)
    ]

    verify_production_archive(options.production_archive, production_objects)
    production_defined = symbols(options.nm, production_objects, False)
    production_undefined = symbols(options.nm, production_objects, True)
    production_symbols = production_defined | production_undefined
    verify_native_page_profile(
        options.production_archive, production_symbols, options.guest_page_size
    )
    verify_gadget_builder_ownership(options.source_root, gadget_objects)
    forbidden_production_test_symbols = {
        symbol
        for symbol in production_symbols
        if any(
            marker in symbol
            for marker in (
                "for_testing",
                "7testing",
                "allocation_checkpoint",
                "backing_mapping_created",
                "backing_mapping_destroyed",
                "fail_allocation_at",
                "fail_next_allocation",
            )
        )
    }
    require(
        not forbidden_production_test_symbols,
        "ordinary production objects contain test-only symbols: "
        f"{sorted(forbidden_production_test_symbols)}",
    )
    retired_runtime_debug_controls = (
        b"UML_VERIFY_JIT",
        b"UML_VERIFY_JIT_MIN_PC",
        b"UML_VERIFY_JIT_MAX_PC",
        b"UML_STATIC_VERIFY_GADGETS",
        b"UML_STATIC_TRACE_BLOCKS",
        b"UML_STATIC_DISABLE_BLOCK_LINKS",
        b"UML_JIT_DISABLE_BLOCK_LINKS",
    )
    debug_control_objects = {
        path.name
        for path in production_objects
        if any(marker in path.read_bytes()
               for marker in retired_runtime_debug_controls)
    }
    require(
        not debug_control_objects,
        "ordinary production engine contains a retired runtime verification "
        f"or trace control: {sorted(debug_control_objects)}",
    )
    for product in options.ordinary_product:
        require(product.is_file(), f"ordinary product is missing: {product}")
        product_symbols = symbols(options.nm, [product], False) | symbols(
            options.nm, [product], True
        )
        test_symbols = {
            symbol
            for symbol in product_symbols
            if "for_testing" in symbol or "7testing" in symbol
        }
        require(
            not test_symbols,
            f"ordinary product contains test-only symbols ({product}): "
            f"{sorted(test_symbols)}",
        )
    duplicate_owners = {
        symbol: paths
        for symbol, paths in strong_external_owners(
            options.nm, production_objects
        ).items()
        if len(paths) > 1
    }
    require(
        not duplicate_owners,
        "production objects have duplicate strong external owners: "
        + ", ".join(
            f"{symbol} ({', '.join(str(path) for path in paths)})"
            for symbol, paths in sorted(duplicate_owners.items())
        ),
    )

    executor_names = {path.name for path in executor_objects}
    require(
        {
            "jit_cache.cpp.o",
            "jit_diagnostics.cpp.o",
            "jit_executor_cache.cpp.o",
            "jit_executor_execution.cpp.o",
            "jit_executor_lifecycle.cpp.o",
            "jit_executor_linking.cpp.o",
            "jit_linking.cpp.o",
            "jit_publication_state.cpp.o",
            "jit_semantic_memory.cpp.o",
        }
        <= executor_names,
        f"executor source ownership is incomplete: {sorted(executor_names)}",
    )
    require(
        {
            "jit_compiler.cpp.o",
            "jit_compiler_decode.cpp.o",
            "jit_compiler_families.cpp.o",
            "jit_compiler_memory.cpp.o",
            "jit_emitter.cpp.o",
        }
        <= {path.name for path in jit_objects},
        "compiler/emitter source ownership is incomplete",
    )
    require(
        {
            "gadget_memory_selection.cpp.o",
            "gadget_scalar_selection.cpp.o",
            "gadget_simd_selection.cpp.o",
            "program_builder.cpp.o",
            "program_builder_branch_system.cpp.o",
            "program_builder_data_processing.cpp.o",
            "program_builder_exception.cpp.o",
            "program_builder_floating_simd.cpp.o",
            "program_builder_load_store.cpp.o",
            "program_execution.cpp.o",
        }
        <= {path.name for path in gadget_objects},
        "gadget selection/builder/execution source ownership is incomplete",
    )
    require(
        not any("testing" in name or "_impl" in name for name in executor_names),
        f"production executor contains a test or fragment object: {executor_names}",
    )

    jit_undefined = symbols(options.nm, jit_objects + executor_objects, True)
    gadget_defined = symbols(options.nm, gadget_objects, False)
    neutral_defined = symbols(options.nm, neutral_objects, False)
    cross_engine = jit_undefined & gadget_defined
    require(
        not cross_engine,
        f"JIT undefined symbols are supplied by gadget objects: {sorted(cross_engine)}",
    )
    required_neutral = {
        "uml_arm64_guest_code_synchronize",
        "uml_arm64_simd_structure_memory_transfer",
    }
    require(
        required_neutral <= normalized(neutral_defined),
        "neutral execution helpers do not own both shared runtime bridges",
    )
    require(
        required_neutral <= normalized(jit_undefined),
        "JIT no longer exercises the audited neutral helper boundary",
    )
    require(
        required_neutral.isdisjoint(normalized(gadget_defined)),
        "a shared runtime bridge is still defined by gadget objects",
    )

    executor_defined = symbols(options.nm, executor_objects, False)
    require(
        not any("for_testing" in symbol or "7testing" in symbol
                for symbol in executor_defined),
        "production executor exports focused-test hooks",
    )
    diagnostic_symbol_markers = (
        "cache_validation_diagnostics",
        "indirect_link_diagnostics",
    )
    production_diagnostic_symbols = {
        symbol
        for symbol in executor_defined
        if any(marker in symbol for marker in diagnostic_symbol_markers)
    }
    require(
        not production_diagnostic_symbols,
        "production executor contains test-only diagnostic getters: "
        f"{sorted(production_diagnostic_symbols)}",
    )
    if diagnostic_executor_objects:
        require(
            {path.name for path in diagnostic_executor_objects}
            == {path.name for path in executor_objects},
            "diagnostic executor does not recompile the exact executor source set",
        )
        diagnostic_defined = symbols(
            options.nm, diagnostic_executor_objects, False
        )
        for marker in diagnostic_symbol_markers:
            require(
                any(marker in symbol for symbol in diagnostic_defined),
                f"diagnostic executor has no {marker} getter",
            )
        production_by_name = {path.name: path for path in executor_objects}
        diagnostic_by_name = {
            path.name: path for path in diagnostic_executor_objects
        }
        for object_name in (
            "jit_executor_cache.cpp.o",
            "jit_executor_execution.cpp.o",
            "jit_executor_lifecycle.cpp.o",
            "jit_executor_linking.cpp.o",
        ):
            require(
                production_by_name[object_name].read_bytes()
                != diagnostic_by_name[object_name].read_bytes(),
                f"diagnostic policy does not alter {object_name}",
            )
    if not options.allow_profile:
        production_profile_symbols = {
            symbol
            for symbol in production_symbols
            if any(
                marker in symbol
                for marker in (
                    "profile_jit",
                    "jit_compile_profile",
                    "profile_dispatch",
                    "profile_reader",
                    "gate_profile",
                    "traceGpuCommands",
                )
            )
        }
        require(
            not production_profile_symbols,
            "ordinary production native engine contains optional profiling symbols: "
            f"{sorted(production_profile_symbols)}",
        )
        optional_profile_controls = (
            b"UML_JIT_PROFILE_",
            b"UML_JIT_DUMP_",
            b"UML_JIT_GPR_LINK_PROFILE_PATH",
            b"UML_PROFILE_TLB_GATE",
            b"UML_PROFILE_DISPATCH",
            b"UML_MMU_TRACE",
            b"UML_GPU_TRACE",
            b"Darwin static MMU: writer thread=",
            b"Darwin dispatch profile calls=",
        )
        profile_control_objects = {
            path.name
            for path in production_objects
            if any(
                marker in path.read_bytes()
                for marker in optional_profile_controls
            )
        }
        require(
            not profile_control_objects,
            "ordinary production native engine contains optional profiling "
            f"controls: {sorted(profile_control_objects)}",
        )
        profile_control_products = {
            str(path)
            for path in options.ordinary_product
            if any(marker in path.read_bytes()
                   for marker in optional_profile_controls)
        }
        require(
            not profile_control_products,
            "ordinary product contains optional profiling controls: "
            f"{sorted(profile_control_products)}",
        )
    lifecycle = next(
        path for path in executor_objects if path.name == "jit_executor_lifecycle.cpp.o"
    )
    lifecycle_undefined = symbols(options.nm, [lifecycle], True)
    require(
        not any(
            name in symbol
            for symbol in lifecycle_undefined
            for name in ("JitBlockPublication", "JitExclusiveOperationScope")
        ),
        "publication hot path gained an out-of-line class call",
    )

    require(
        "uml_arm64_gadget_fatal" not in normalized(gadget_defined),
        "unreachable fatal helper is still present in the gadget object",
    )
    assembly_object = next(
        (path for path in gadget_objects if path.name in ("engine.S.o", "engine.S.obj")),
        None,
    )
    require(assembly_object is not None, "gadget assembly object is missing")
    reachability = gadget_reachability(
        options.nm,
        options.disassembler,
        assembly_object,
        gadget_objects,
        options.source_root,
    )
    expected_entry = [
        "stp", "stp", "stp", "stp", "stp", "stp", "str", "str",
        "mov", "mov", "ldr", "ldr", "br",
    ]
    actual_entry = entry_mnemonics(options.disassembler, assembly_object)
    require(
        actual_entry[: len(expected_entry)] == expected_entry,
        f"engine entry hot path changed: {actual_entry[:len(expected_entry)]}",
    )

    if options.conformance_binary is not None:
        require(
            options.conformance_binary.is_file(),
            f"common conformance binary is missing: {options.conformance_binary}",
        )
        conformance_symbols = symbols(
            options.nm, [options.conformance_binary], False
        )
        adapter_markers = (
            "make_cpu_engine",
            "cpu_engine_available",
            "make_interpreter_engine",
            "make_gadget_engine",
            "make_jit_engine",
            "native_jit_engine_available",
        )
        adapter_symbols = symbols(options.nm, adapter_objects, False)
        required_adapter_entries = {
            symbol
            for symbol in adapter_symbols
            if any(marker in symbol for marker in adapter_markers)
        }
        for marker in adapter_markers:
            require(
                any(marker in symbol for symbol in required_adapter_entries),
                f"configured production adapters have no {marker} entry",
            )
        require(
            required_adapter_entries <= conformance_symbols,
            "common conformance binary is not linked to every configured "
            "production adapter entry: "
            f"{sorted(required_adapter_entries - conformance_symbols)}",
        )
        require(
            any("JitExecutor" in symbol for symbol in conformance_symbols),
            "common conformance binary does not contain the production JIT executor",
        )
        require(
            "uml_arm64_gadget_enter" in normalized(conformance_symbols),
            "common conformance binary does not contain the production gadget engine",
        )
        test_hook_symbols = {
            symbol
            for symbol in (
                symbols(options.nm, test_hook_objects, False)
                if test_hook_objects
                else set()
            )
            if "jit7testing" in symbol or "for_testing" in symbol
        }
        require(
            conformance_symbols.isdisjoint(test_hook_symbols),
            "common conformance binary contains a test-only engine hook: "
            f"{sorted(conformance_symbols & test_hook_symbols)}",
        )
        diagnostic_getters = {
            symbol
            for symbol in (
                symbols(options.nm, diagnostic_executor_objects, False)
                if diagnostic_executor_objects
                else set()
            )
            if any(marker in symbol for marker in diagnostic_symbol_markers)
        }
        require(
            conformance_symbols.isdisjoint(diagnostic_getters),
            "common conformance binary contains test-only JIT diagnostics: "
            f"{sorted(conformance_symbols & diagnostic_getters)}",
        )

    print(
        "engine object ownership, entry disassembly, and gadget reachability "
        f"passed: {reachability[0]} symbols, {reachability[1]} compiled roots, "
        f"{reachability[2]} source/table roots, {reachability[3]} reachable"
    )
    return 0


def entrypoint() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--compare-static-archives":
        verify_archive_copy(Path(sys.argv[2]), Path(sys.argv[3]))
        print("installed archive preserves every object payload")
        return 0
    return main()


if __name__ == "__main__":
    try:
        raise SystemExit(entrypoint())
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
