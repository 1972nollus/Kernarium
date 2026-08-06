#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative fixtures for deterministic ASL state-module composition."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import sys
import os
import re
import subprocess
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from a64_aslref_dependencies import (  # noqa: E402
    ASL_STANDARD_CALLS,
    AslDependencyCatalog,
    Definition,
    DependencyStatus,
    _declared_symbols,
    _global_identifiers,
)
from generate_a64_aslref_corpus import (  # noqa: E402
    diagnostic_digest,
    load_interface_modules,
    resolved_interface_map,
    selected_owner_modules,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except ValueError:
        return
    raise AssertionError(message)


def write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


def module_ownership_fixtures(directory: Path) -> None:
    base = write(directory / "base.asl", "func X(n : integer) => bits(64)\n")
    core = write(
        directory / "a64_aslref_state_core.asl",
        "type CoreState of record { value : bits(64), };\n"
        "func _PC() => bits(64)\n",
    )
    environment = write(
        directory / "a64_aslref_state_environment.asl",
        "var PSTATE : bits(64);\n"
        "readonly func IsFeatureImplemented(feature : integer) => boolean\n",
    )
    paths, declarations, owners = load_interface_modules(
        base, [core, environment]
    )
    require(set(paths) == {core.name, environment.name}, "module paths drifted")
    require(
        declarations[core.name] == frozenset({"CoreState", "_PC"}),
        "core operation/type parse failed",
    )
    frontier = {
        ("core-state", "_PC"),
        ("execution-environment", "IsFeatureImplemented"),
        ("execution-environment", "PSTATE"),
        ("memory-and-ordering", "Mem"),
    }
    resolved = resolved_interface_map(frontier, owners)
    require(resolved[("core-state", "_PC")], "owned core symbol unresolved")
    require(
        resolved[("execution-environment", "IsFeatureImplemented")],
        "owned environment symbol unresolved",
    )
    require(
        resolved[("execution-environment", "PSTATE")],
        "owned top-level state symbol unresolved",
    )
    require(
        not resolved[("memory-and-ordering", "Mem")],
        "absent module symbol was credited",
    )

    wrong = write(
        directory / "a64_aslref_state_memory.asl",
        "func _PC() => bits(64)\n",
    )
    _paths, _declarations, wrong_owners = load_interface_modules(base, [wrong])
    expect_failure(
        lambda: resolved_interface_map({("core-state", "_PC")}, wrong_owners),
        "wrong-owner declaration was accepted",
    )

    duplicate = write(
        directory / "a64_aslref_state_events.asl",
        "func SharedHelper()\n",
    )
    write(environment, "func SharedHelper()\n")
    expect_failure(
        lambda: load_interface_modules(base, [environment, duplicate]),
        "cross-module duplicate declaration was accepted",
    )

    unknown = write(directory / "unknown.asl", "func Unknown()\n")
    expect_failure(
        lambda: load_interface_modules(base, [unknown]),
        "unknown owner module was accepted",
    )


def official_definition_replacement_fixture() -> None:
    catalog = AslDependencyCatalog.__new__(AslDependencyCatalog)
    catalog.definitions = {
        "owned": Definition(
            name="owned",
            symbols=frozenset({"_PC"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset({"Downstream"}),
            identifiers=frozenset(),
            text="func _PC() => bits(64)\n",
        ),
        "kept": Definition(
            name="kept",
            symbols=frozenset({"Keep"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset(),
            identifiers=frozenset(),
            text="func Keep() => bits(64)\n",
        ),
        "downstream": Definition(
            name="downstream",
            symbols=frozenset({"Downstream"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset(),
            identifiers=frozenset(),
            text="func Downstream() => bits(64)\n",
        ),
        "adapter-type": Definition(
            name="adapter-type",
            symbols=frozenset({"AdapterType"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset(),
            identifiers=frozenset(),
            text="type AdapterType of record { value : integer };\n",
        ),
    }
    catalog.links = {}
    catalog.symbols = {
        "_PC": frozenset({"owned"}),
        "Keep": frozenset({"kept"}),
        "Downstream": frozenset({"downstream"}),
        "AdapterType": frozenset({"adapter-type"}),
    }
    catalog.type_symbols = {"AdapterType": frozenset({"adapter-type"})}
    catalog.constant_symbols = {}
    catalog.variable_symbols = {}
    catalog.lexical_symbols = frozenset({"AdapterType"})
    status = DependencyStatus(
        direct_definitions=frozenset({"owned", "kept"}),
        transitive_definitions=frozenset({"owned", "kept", "downstream"}),
        dependency_digest="fixture",
        impdef_symbols=frozenset(),
        external_symbols=frozenset(),
        missing_interfaces=(("core-state", "_PC"),),
        official_operation_present=True,
    )
    rendered = catalog.shared_subset(
        status,
        frozenset({"_PC", "AdapterState"}),
        ("var AdapterState : AdapterType;\n",),
    )
    require("func _PC" not in rendered, "supplied definition was duplicated")
    require(
        "func Downstream" not in rendered,
        "dependency reachable only behind a supplied boundary was retained",
    )
    require("func Keep" in rendered, "unrelated official definition was removed")
    require(
        "type AdapterType" in rendered,
        "official type used only by a selected adapter was omitted",
    )


def type_boundary_fixture() -> None:
    catalog = AslDependencyCatalog.__new__(AslDependencyCatalog)
    catalog.definitions = {
        "shared/functions/system/EndOfInstructionException": Definition(
            name="shared/functions/system/EndOfInstructionException",
            symbols=frozenset({"EndOfInstructionException"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset(),
            identifiers=frozenset(),
            text="type EndOfInstructionException of exception { reason : integer };\n",
        ),
        "shared/exceptions/exceptions/Exception": Definition(
            name="shared/exceptions/exceptions/Exception",
            symbols=frozenset({"Exception"}),
            links=frozenset(),
            linked_dependencies=frozenset(),
            calls=frozenset(),
            identifiers=frozenset(),
            text="type Exception of enumeration { Example };\n",
        ),
    }
    require(
        not catalog._boundary_requirements(
            "shared/functions/system/EndOfInstructionException"
        ),
        "ordinary exception type leaked into the replaceable interface frontier",
    )
    require(
        catalog._boundary_requirements("shared/exceptions/exceptions/Exception")
        == {("control-and-events", "Exception")},
        "explicitly owned exception state stopped at the official boundary",
    )


def feature_prelude_fixture() -> None:
    catalog = AslDependencyCatalog.__new__(AslDependencyCatalog)
    catalog.definitions = {}
    catalog.links = {}
    catalog.symbols = {}
    catalog.type_symbols = {}
    catalog.constant_symbols = {}
    catalog.variable_symbols = {}
    catalog.lexical_symbols = frozenset()
    catalog.feature_symbols = frozenset({"FEAT_Other"})
    catalog.pmu_symbols = frozenset()
    status = DependencyStatus(
        direct_definitions=frozenset(),
        transitive_definitions=frozenset(),
        dependency_digest="fixture",
        impdef_symbols=frozenset(),
        external_symbols=frozenset(),
        missing_interfaces=(),
        official_operation_present=True,
    )
    rendered = catalog.shared_subset(
        status,
        supplemental_sources=(
            "readonly func IsFeatureImplemented(feature : Feature) => boolean\n"
            "begin return feature == FEAT_Example; end;\n",
        ),
    )
    require(
        "type Feature of enumeration" in rendered
        and "FEAT_Example" in rendered
        and "FEAT_Other" in rendered,
        "pinned XML feature identifiers did not supply ASLRef's missing type",
    )


def alias_source_fixture(directory: Path) -> None:
    write(
        directory / "shared_pseudocode.xml",
        "<root><ps name='shared/Keep'><pstext>func Keep()</pstext></ps></root>",
    )
    write(
        directory / "target.xml",
        "<instructionsection><ps_section><pstext>Keep();</pstext>"
        "</ps_section></instructionsection>",
    )
    write(
        directory / "alias.xml",
        "<instructionsection><aliasto refiform='target.xml'/></instructionsection>",
    )
    write(
        directory / "alias2.xml",
        "<instructionsection><aliasto refiform='alias.xml'/></instructionsection>",
    )
    catalog = AslDependencyCatalog(directory)
    require(
        catalog.operation_source_file("alias2.xml") == "target.xml",
        "alias chain did not resolve to the concrete operation source",
    )
    require(
        catalog.for_file("alias2.xml").direct_definitions == frozenset(
            {"shared/Keep"}
        ),
        "alias dependency status did not follow the concrete operation",
    )
    write(
        directory / "cycle-a.xml",
        "<instructionsection><aliasto refiform='cycle-b.xml'/></instructionsection>",
    )
    write(
        directory / "cycle-b.xml",
        "<instructionsection><aliasto refiform='cycle-a.xml'/></instructionsection>",
    )
    expect_failure(
        lambda: catalog.operation_source_file("cycle-a.xml"),
        "alias cycle was accepted",
    )


def module_dependency_fixture() -> None:
    frontier = (("sme-state", "_ZA"),)
    resolved = {("sme-state", "_ZA"): True}
    require(
        selected_owner_modules(frontier, resolved)
        == ["a64_aslref_state_sme.asl", "a64_aslref_state_sve.asl"],
        "SME state did not select the SVE constants/types it depends on",
    )
    sysreg_frontier = (("system-register-state", "AArch64_SysRegRead"),)
    sysreg_resolved = {
        ("system-register-state", "AArch64_SysRegRead"): True
    }
    require(
        selected_owner_modules(sysreg_frontier, sysreg_resolved)
        == [
            "a64_aslref_state_core.asl",
            "a64_aslref_state_environment.asl",
            "a64_aslref_state_events.asl",
            "a64_aslref_state_fp.asl",
            "a64_aslref_state_sysreg.asl",
        ],
        "system-register dispatch omitted its core/environment/FP/event closure",
    )


def diagnostic_digest_fixture() -> None:
    first_root = Path("/private/tmp/run-one/form-7")
    second_root = Path("/private/tmp/run-two/form-91")
    first = diagnostic_digest(
        f"File {first_root}/shared_subset.asl\nASL Static error: Missing",
        ((first_root, "<form>"),),
    )
    second = diagnostic_digest(
        f"File {second_root}/shared_subset.asl\nASL Static error: Missing",
        ((second_root, "<form>"),),
    )
    require(first == second, "temporary paths made diagnostic hashes drift")


def _constant(source: str, name: str) -> int:
    match = re.search(
        rf"constant\s+{re.escape(name)}\s*:\s*integer\{{\}}\s*=\s*(\d+)\s*;",
        source,
    )
    if match is None:
        raise AssertionError(f"missing integer constant {name}")
    return int(match.group(1))


def _cpp_memory_region_array_counts(source: str) -> list[int]:
    """Count CTAD MemoryRegion array elements without counting nested commas."""
    clean = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    clean = re.sub(r"//[^\n]*", " ", clean)
    clean = re.sub(r'"(?:\\.|[^"\\])*"', '""', clean)
    counts: list[int] = []
    for match in re.finditer(r"\bstd::array\s+[A-Za-z_]\w*\s*\{", clean):
        opening = match.end() - 1
        braces = 0
        parentheses = 0
        brackets = 0
        separators: list[int] = []
        closing = -1
        for index in range(opening, len(clean)):
            character = clean[index]
            if character == "{":
                braces += 1
            elif character == "}":
                braces -= 1
                if braces == 0:
                    closing = index
                    break
            elif character == "(":
                parentheses += 1
            elif character == ")":
                parentheses -= 1
            elif character == "[":
                brackets += 1
            elif character == "]":
                brackets -= 1
            elif (
                character == ","
                and braces == 1
                and parentheses == 0
                and brackets == 0
            ):
                separators.append(index)
        if closing < 0:
            raise AssertionError("unterminated C++ std::array initializer")
        body = clean[opening + 1 : closing]
        if not (
            "MemoryRegion" in body
            or re.search(r"\bmemory_region\s*\(", body) is not None
        ):
            continue
        boundaries = [opening, *separators, closing]
        elements = sum(
            bool(clean[left + 1 : right].strip())
            for left, right in zip(boundaries, boundaries[1:])
        )
        counts.append(elements)
    return counts


def memory_bridge_source_fixture() -> None:
    repository = Path(__file__).resolve().parents[3]
    path = Path(__file__).with_name("a64_aslref_state_memory.asl")
    source = path.read_text(encoding="utf-8")
    declared = _declared_symbols(source, path.name)
    required_api = {
        "A64Oracle_BeginMemoryRegions",
        "A64Oracle_ConfigureMemoryRegion",
        "A64Oracle_SetMemoryRegionByteChunk",
        "A64Oracle_SetMemoryRegionTagChunk",
        "A64Oracle_FinalizeMemoryRegions",
        "A64Oracle_GetMemoryRegionCount",
        "A64Oracle_GetMemoryRegionAddress",
        "A64Oracle_GetMemoryRegionSize",
        "A64Oracle_GetMemoryRegionPASpace",
        "A64Oracle_GetMemoryRegionReadable",
        "A64Oracle_GetMemoryRegionWritable",
        "A64Oracle_GetMemoryRegionExecutable",
        "A64Oracle_GetMemoryRegionGuestCode",
        "A64Oracle_GetMemoryRegionGuestWritable",
        "A64Oracle_GetMemoryRegionByteChunk",
        "A64Oracle_GetMemoryRegionTagChunk",
        "A64Oracle_SetLocalExclusiveMonitorState",
        "A64Oracle_SetGlobalExclusiveMonitorState",
        "A64Oracle_GetLocalExclusiveMonitorAddress",
        "A64Oracle_GetLocalExclusiveMonitorKey",
        "A64Oracle_GetLocalExclusiveMonitorValue",
        "A64Oracle_GetLocalExclusiveMonitorValue2",
        "A64Oracle_GetLocalExclusiveMonitorVersion",
        "A64Oracle_GetLocalExclusiveMonitorSize",
        "A64Oracle_GetGlobalExclusiveMonitorAddress",
        "A64Oracle_GetGlobalExclusiveMonitorKey",
        "A64Oracle_GetGlobalExclusiveMonitorValue",
        "A64Oracle_GetGlobalExclusiveMonitorValue2",
        "A64Oracle_GetGlobalExclusiveMonitorVersion",
        "A64Oracle_GetGlobalExclusiveMonitorSize",
        "A64Oracle_GetLastAccessPresent",
        "A64Oracle_GetLastAccessClass",
        "A64Oracle_GetLastAccessDomain",
        "A64Oracle_GetLastAccessPASpace",
        "A64Oracle_GetLastAccessAddress",
        "A64Oracle_GetLastAccessWrite",
        "A64Oracle_GetLastAccessByteCount",
        "A64Oracle_GetLastAccessStatus",
        "A64Oracle_GetLastAccessStatusCode",
        "A64Oracle_GetLastAccessExtFlag",
        "A64Oracle_GetLastAccessErrorState",
        "A64Oracle_GetLastAccessStore64BStatus",
        "A64Oracle_GetLastFaultPresent",
        "A64Oracle_GetLastFaultDomain",
        "A64Oracle_GetLastFaultPASpace",
        "A64Oracle_GetLastFaultAddress",
        "A64Oracle_GetLastFaultStatus",
        "A64Oracle_GetLastFaultStatusCode",
        "A64Oracle_GetLastFaultExtFlag",
        "A64Oracle_GetLastFaultErrorState",
        "A64Oracle_GetLastFaultStore64BStatus",
        # The old execution observer still calls these names directly.
        "A64Oracle_SetMemoryBase",
        "A64Oracle_SetMemoryPASpace",
        "A64Oracle_SetMemoryByte",
        "A64Oracle_GetMemoryByte",
        "A64Oracle_SetMemoryTag",
        "A64Oracle_GetMemoryTag",
    }
    require(required_api <= declared, "bounded memory bridge API is incomplete")
    require(
        len(re.findall(r"\bA64Oracle_Record(?:Virtual|Physical)Access\s*\(", source))
        == 11,
        "memory access provenance is not wired to exactly nine access sites",
    )
    for enum_name in (
        "A64OracleMemoryAccessClass",
        "A64OracleMemoryAddressDomain",
    ):
        require(
            f"type {enum_name} of enumeration" in source,
            f"{enum_name} provenance enum is absent",
        )

    slot_bound = _constant(source, "A64Oracle_MaximumMemoryRegionCount")
    byte_bound = _constant(source, "A64Oracle_MaximumMemoryByteCount")
    tag_bound = _constant(source, "A64Oracle_MaximumMemoryTagCount")
    counts: list[int] = []
    fixture_root = repository / "tests/unit/arm64"
    for fixture in sorted(fixture_root.glob("*.cpp")) + sorted(
        fixture_root.glob("*.hpp")
    ):
        counts.extend(
            _cpp_memory_region_array_counts(fixture.read_text(encoding="utf-8"))
        )
    require(counts and max(counts) == 3, "current maximum region count drifted")
    require(slot_bound >= max(counts), "ASL region slots do not cover fixtures")
    largest = (
        fixture_root / "arm64_base_mops_copy_tests.cpp"
    ).read_text(encoding="utf-8")
    require(
        "std::vector<std::byte> initial(4 * mops::page_size);" in largest,
        "largest current memory fixture changed without a bounds review",
    )
    require(
        "std::vector<std::byte> initial(transfer_size + 2 * mops::page_size);"
        in largest,
        "full-extent memory fixture changed without a bounds review",
    )
    cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
    page = re.search(r"set\(UML_GUEST_PAGE_SIZE\s+(\d+)\)", cmake)
    require(page is not None, "default guest page size is not explicit")
    observed_maximum = 64 * 1024 + 2 * int(page.group(1))
    require(
        observed_maximum == 98304 and byte_bound >= observed_maximum,
        "ASL byte pool no longer covers the largest emitted region",
    )
    require(
        tag_bound >= (byte_bound + 15) // 16 + slot_bound,
        "ASL tag pool does not cover per-region rounding",
    )
    require(
        f"0 to A64Oracle_MaximumMemoryByteCount - 1" not in source
        and f"0 to A64Oracle_MaximumMemoryTagCount - 1" not in source,
        "memory reset regressed to work proportional to unused capacity",
    )
    reset = re.search(
        r"func\s+A64Oracle_ResetMemoryState\(\)\s*begin(.*?)\nend;",
        source,
        flags=re.DOTALL,
    )
    require(
        reset is not None and "for " not in reset.group(1),
        "memory reset eagerly traverses storage instead of using lazy zero state",
    )
    for permission in (
        "Readable", "Writable", "Executable", "GuestCode", "GuestWritable"
    ):
        require(
            f"A64Oracle_MemoryRegion{permission}" in source,
            f"memory-region {permission} state is absent",
        )
    require(
        "assert limit <= previous_first || previous_limit <= first;" in source,
        "region overlap rejection is absent",
    )
    require(
        "A64Oracle_AddressRangeDoesNotWrap(address, size)" in source,
        "region wrap rejection is absent",
    )


class _BoundedRegionModel:
    """Small independent contract fixture for acceptance/rejection cases."""

    def __init__(self, slots: int, capacity: int) -> None:
        self.slots = slots
        self.capacity = capacity
        self.regions: list[tuple[int, int, bytearray, list[int]]] = []
        self.finalized = False

    def configure(self, address: int, size: int) -> None:
        if self.finalized or size <= 0 or not 0 <= address < 1 << 64:
            raise ValueError("invalid region")
        if address + size > 1 << 64:
            raise ValueError("wrapped region")
        if len(self.regions) == self.slots:
            raise ValueError("too many regions")
        if sum(item[1] for item in self.regions) + size > self.capacity:
            raise ValueError("byte capacity exceeded")
        if any(
            address < other + other_size and other < address + size
            for other, other_size, _bytes, _tags in self.regions
        ):
            raise ValueError("overlapping regions")
        self.regions.append((address, size, bytearray(), []))

    def bytes(self, region: int, offset: int, value: bytes) -> None:
        entry = self.regions[region]
        if offset != len(entry[2]) or not 1 <= len(value) <= 16:
            raise ValueError("non-contiguous byte chunk")
        if offset + len(value) > entry[1]:
            raise ValueError("byte chunk overflow")
        entry[2].extend(value)

    def tags(self, region: int, offset: int, value: list[int]) -> None:
        entry = self.regions[region]
        if offset != len(entry[3]) or not 1 <= len(value) <= 16:
            raise ValueError("non-contiguous tag chunk")
        if offset + len(value) > (entry[1] + 15) // 16:
            raise ValueError("tag chunk overflow")
        if any(not 0 <= tag < 16 for tag in value):
            raise ValueError("invalid tag")
        entry[3].extend(value)

    def finalize(self) -> None:
        if any(
            len(data) != size or len(tags) != (size + 15) // 16
            for _address, size, data, tags in self.regions
        ):
            raise ValueError("incomplete region")
        self.finalized = True


def memory_bridge_contract_fixture() -> None:
    model = _BoundedRegionModel(8, 65536)
    model.configure(0x3000, 17)
    model.configure(0x1000, 16)
    model.bytes(0, 0, bytes(range(16)))
    model.bytes(0, 16, b"\xff")
    model.tags(0, 0, [3, 7])
    model.bytes(1, 0, bytes(reversed(range(16))))
    model.tags(1, 0, [12])
    model.finalize()
    require(
        [region[0] for region in model.regions] == [0x3000, 0x1000],
        "region input order was not preserved",
    )

    for action in (
        lambda: (
            _BoundedRegionModel(8, 65536).configure((1 << 64) - 1, 2)
        ),
        lambda: (
            (lambda overlap: (
                overlap.configure(0x1000, 32), overlap.configure(0x1010, 16)
            ))(_BoundedRegionModel(8, 65536))
        ),
        lambda: (
            (lambda incomplete: (
                incomplete.configure(0x1000, 16), incomplete.finalize()
            ))(_BoundedRegionModel(8, 65536))
        ),
        lambda: (
            (lambda chunks: (
                chunks.configure(0x1000, 16), chunks.bytes(0, 1, b"x")
            ))(_BoundedRegionModel(8, 65536))
        ),
    ):
        expect_failure(action, "invalid bounded-region contract was accepted")


ASL_MEMORY_FIXTURE_PRELUDE = r"""
constant NUM_PABITS : integer{} = 56;

type PASpace of enumeration {
    PAS_NonSecure,
    PAS_Secure,
    PAS_Root,
    PAS_Realm
};

type Fault of enumeration {
    Fault_None,
    Fault_Alignment,
    Fault_Translation,
    Fault_SyncExternal,
    Fault_Permission
};

type ErrorState of enumeration { ErrorState_CE };
type MBReqTypes of enumeration { MBReqTypes_All };
type MBMaintenanceScope of enumeration { MBMaintenanceScope_None };
type MemType of enumeration { MemType_Normal, MemType_Device };
type AccessType of enumeration {
    AccessType_Normal,
    AccessType_GCS,
    AccessType_IFETCH
};

type FullAddress of record {
    paspace : PASpace,
    address : bits(NUM_PABITS)
};

type PhysMemRetStatus of record {
    statuscode : Fault,
    extflag : bit,
    merrorstate : ErrorState,
    store64bstatus : bits(64)
};

type AccessDescriptor of record {
    read : boolean,
    write : boolean,
    ispair : boolean,
    exclusive : boolean,
    acctype : AccessType,
    rcw : boolean,
    rcws : boolean,
    ls64 : boolean,
    atomicop : boolean,
    acqsc : boolean,
    acqpc : boolean,
    relsc : boolean,
    tagaccess : boolean
};

type FaultRecord of record {
    statuscode : Fault,
    vaddress : bits(64),
    level : integer,
    write : boolean,
    extflag : bit,
    merrorstate : ErrorState
};

type FixtureFault of record { statuscode : Fault };
type FixtureMemAttrs of record { memtype : MemType };
type AddressDescriptor of record {
    paddress : FullAddress,
    fault : FixtureFault,
    memattrs : FixtureMemAttrs
};
type CPYParams of record { fixture : boolean };
type SETParams of record { fixture : boolean };

func FixtureAccess(tagaccess : boolean) => AccessDescriptor
begin
    var result : AccessDescriptor;
    result.read = !tagaccess;
    result.write = FALSE;
    result.ispair = FALSE;
    result.exclusive = FALSE;
    result.acctype = AccessType_Normal;
    result.rcw = FALSE;
    result.rcws = FALSE;
    result.ls64 = FALSE;
    result.atomicop = FALSE;
    result.acqsc = FALSE;
    result.acqpc = FALSE;
    result.relsc = FALSE;
    result.tagaccess = tagaccess;
    return result;
end;

func NoFault(accdesc : AccessDescriptor, address : bits(64)) => FaultRecord
begin
    var result : FaultRecord;
    result.statuscode = Fault_None;
    result.vaddress = address;
    result.level = 0;
    result.write = FALSE;
    result.extflag = '0';
    result.merrorstate = ErrorState_CE;
    return result;
end;
"""


ASL_MEMORY_FIXTURE_POSITIVE = r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    assert !A64Oracle_GetLastAccessPresent();
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_None;
    assert A64Oracle_GetLastAccessDomain() == A64OracleMemoryAddressDomain_None;
    assert A64Oracle_GetLastAccessPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastAccessAddress() == Zeros{64};
    assert !A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 0;
    assert A64Oracle_GetLastAccessStatusCode() == Fault_None;
    assert A64Oracle_GetLastAccessExtFlag() == '0';
    assert A64Oracle_GetLastAccessErrorState() == ErrorState_CE;
    assert A64Oracle_GetLastAccessStore64BStatus() == Zeros{64};
    assert !A64Oracle_GetLastFaultPresent();
    assert A64Oracle_GetLastFaultDomain() == A64OracleMemoryAddressDomain_None;
    assert A64Oracle_GetLastFaultPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastFaultAddress() == Zeros{64};
    assert A64Oracle_GetLastFaultStatusCode() == Fault_None;
    assert A64Oracle_GetLastFaultExtFlag() == '0';
    assert A64Oracle_GetLastFaultErrorState() == ErrorState_CE;
    assert A64Oracle_GetLastFaultStore64BStatus() == Zeros{64};
    assert A64Oracle_GetMemoryByte(3) == Zeros{8};
    assert A64Oracle_GetMemoryTag(5) == Zeros{4};
    A64Oracle_SetMemoryByte(3, Ones{8});
    A64Oracle_SetMemoryTag(5, Ones{4});
    assert A64Oracle_GetMemoryByte(3) == Ones{8};
    assert A64Oracle_GetMemoryByte(4) == Zeros{8};
    assert A64Oracle_GetMemoryTag(5) == Ones{4};
    assert A64Oracle_GetMemoryTag(6) == Zeros{4};
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(
        0, '0000000000000000000000000000000000000000000000000001000000000000',
        17, PAS_NonSecure, TRUE, TRUE, FALSE, FALSE, TRUE
    );
    A64Oracle_ConfigureMemoryRegion(
        1, '0000000000000000000000000000000000000000000000000011000000000000',
        16, PAS_Secure, FALSE, FALSE, TRUE, TRUE, FALSE
    );
    A64Oracle_ConfigureMemoryRegion(
        2, '0000000000000000000000000000000000000000000000000001000000010001',
        15, PAS_NonSecure, TRUE, TRUE, TRUE, FALSE, TRUE
    );
    A64Oracle_SetMemoryRegionByteChunk(0, 0, 16, Ones{128});
    A64Oracle_SetMemoryRegionByteChunk(0, 16, 1, Ones{128});
    A64Oracle_SetMemoryRegionTagChunk(0, 0, 2, Ones{64});
    A64Oracle_SetMemoryRegionByteChunk(1, 0, 16, Zeros{128});
    A64Oracle_SetMemoryRegionTagChunk(1, 0, 1, Zeros{64});
    A64Oracle_SetMemoryRegionByteChunk(2, 0, 15, Zeros{128});
    A64Oracle_SetMemoryRegionTagChunk(2, 0, 1, Zeros{64});
    A64Oracle_FinalizeMemoryRegions();
    assert A64Oracle_GetMemoryRegionCount() == 3;
    assert A64Oracle_GetMemoryRegionSize(0) == 17;
    assert A64Oracle_GetMemoryRegionPASpace(1) == PAS_Secure;
    assert A64Oracle_GetMemoryRegionReadable(0);
    assert A64Oracle_GetMemoryRegionWritable(0);
    assert !A64Oracle_GetMemoryRegionExecutable(0);
    assert !A64Oracle_GetMemoryRegionGuestCode(0);
    assert A64Oracle_GetMemoryRegionGuestWritable(0);
    assert !A64Oracle_GetMemoryRegionWritable(1);
    assert A64Oracle_GetMemoryRegionExecutable(1);
    assert A64Oracle_GetMemoryRegionGuestCode(1);
    assert !A64Oracle_GetMemoryRegionGuestWritable(1);
    assert A64Oracle_GetMemoryRegionByteChunk(0, 0, 16) == Ones{128};
    assert A64Oracle_GetMemoryRegionByteChunk(0, 16, 1)[7:0] == Ones{8};
    assert A64Oracle_GetMemoryRegionTagChunk(0, 0, 2)[7:0] == Ones{8};
    let cross_region : bits(128) = Mem{128}(
        '0000000000000000000000000000000000000000000000000001000000001000',
        FixtureAccess(FALSE)
    );
    assert cross_region[71:0] == Ones{72};
    assert cross_region[127:72] == Zeros{56};
    A64Oracle_SetLocalExclusiveMonitorState(
        TRUE, PAS_NonSecure,
        '0000000000000000000000000000000000000000000000000001000000000001',
        7, 8, Ones{64}, Zeros{64}, Ones{64}, Ones{64}
    );
    A64Oracle_SetGlobalExclusiveMonitorState(
        FALSE, PAS_Realm, Ones{64}, 11, 0,
        Zeros{64}, Ones{64}, Zeros{64}, Ones{64}
    );
    assert A64Oracle_GetLocalExclusiveMonitorValid();
    assert A64Oracle_GetLocalExclusiveMonitorPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLocalExclusiveMonitorAddress() ==
        '0000000000000000000000000000000000000000000000000001000000000001';
    assert A64Oracle_GetLocalExclusiveMonitorProcessorID() == 7;
    assert A64Oracle_GetLocalExclusiveMonitorSize() == 8;
    assert A64Oracle_GetLocalExclusiveMonitorKey() == Ones{64};
    assert A64Oracle_GetLocalExclusiveMonitorValue() == Zeros{64};
    assert A64Oracle_GetLocalExclusiveMonitorValue2() == Ones{64};
    assert A64Oracle_GetLocalExclusiveMonitorVersion() == Ones{64};
    assert !A64Oracle_GetGlobalExclusiveMonitorValid();
    assert A64Oracle_GetGlobalExclusiveMonitorPASpace() == PAS_Realm;
    assert A64Oracle_GetGlobalExclusiveMonitorAddress() == Ones{64};
    assert A64Oracle_GetGlobalExclusiveMonitorProcessorID() == 11;
    assert A64Oracle_GetGlobalExclusiveMonitorSize() == 0;
    assert A64Oracle_GetGlobalExclusiveMonitorKey() == Zeros{64};
    assert A64Oracle_GetGlobalExclusiveMonitorValue() == Ones{64};
    assert A64Oracle_GetGlobalExclusiveMonitorValue2() == Zeros{64};
    assert A64Oracle_GetGlobalExclusiveMonitorVersion() == Ones{64};
    var access : AccessDescriptor = FixtureAccess(FALSE);
    let value : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000001000000000000',
        access
    );
    assert value == Ones{8};
    assert A64Oracle_GetLastAccessPresent();
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_DataRead;
    assert A64Oracle_GetLastAccessDomain() == A64OracleMemoryAddressDomain_Virtual;
    assert A64Oracle_GetLastAccessPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastAccessAddress() ==
        '0000000000000000000000000000000000000000000000000001000000000000';
    assert !A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 1;
    assert A64Oracle_GetLastAccessStatusCode() == Fault_None;
    assert !A64Oracle_GetLastFaultPresent();
    let denied : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000011000000000000',
        access
    );
    assert denied == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Permission;
    assert A64Oracle_GetLastFaultPresent();
    assert A64Oracle_GetLastFaultDomain() == A64OracleMemoryAddressDomain_Virtual;
    assert A64Oracle_GetLastFaultPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastFaultAddress() ==
        '0000000000000000000000000000000000000000000000000011000000000000';
    assert A64Oracle_GetLastFaultStatusCode() == Fault_Permission;
    Mem{8}(
        '0000000000000000000000000000000000000000000000000011000000000000',
        access
    ) = Ones{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Permission;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_DataWrite;
    assert A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 1;
    assert A64Oracle_GetMemoryRegionByteChunk(1, 0, 1)[7:0] == Zeros{8};
    access.acctype = AccessType_IFETCH;
    let fetched : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000011000000000000',
        access
    );
    assert fetched == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_None;
    let metadata_denied_fetch : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000001000000010001',
        access
    );
    assert metadata_denied_fetch == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Permission;
    access.acctype = AccessType_Normal;
    access.tagaccess = FALSE;
    let denied_tag : bits(4) = AArch64_MemTag(
        '0000000000000000000000000000000000000000000000000001000000000000',
        access
    );
    assert denied_tag == Zeros{4};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Permission;
    access.tagaccess = TRUE;
    let permitted_tag : bits(4) = AArch64_MemTag(
        '0000000000000000000000000000000000000000000000000001000000000000',
        access
    );
    assert permitted_tag == Ones{4};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_None;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_TagRead;
    assert !A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 1;
    assert A64Oracle_GetLastAccessStatusCode() == Fault_None;
    AArch64_MemTag(
        '0000000000000000000000000000000000000000000000000001000000000000',
        access
    ) = Ones{4};
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_TagWrite;
    assert A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 16;
    assert A64Oracle_GetLastAccessStatusCode() == Fault_None;
    let unmapped : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000101000000000000',
        access
    );
    assert unmapped == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Translation;
    A64Oracle_SetMemoryFault(Fault_Translation);
    let injected : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000001000000000000',
        access
    );
    assert injected == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Translation;
    let permission_precedes_injection : bits(8) = Mem{8}(
        '0000000000000000000000000000000000000000000000000011000000000000',
        access
    );
    assert permission_precedes_injection == Zeros{8};
    assert A64Oracle_LastMemoryStatus.statuscode == Fault_Permission;
    A64Oracle_SetMemoryFault(Fault_None);
    var physical : AddressDescriptor;
    physical.paddress.paspace = PAS_Secure;
    physical.paddress.address =
        Zeros{NUM_PABITS - 16} :: '0011000000000000';
    physical.fault.statuscode = Fault_None;
    physical.memattrs.memtype = MemType_Normal;
    var physical_status : PhysMemRetStatus;
    var physical_value : bits(8);
    var physical_tag_value : bits(4);
    access.acctype = AccessType_Normal;
    (physical_status, physical_value) = PhysMemRead{8}(physical, access);
    assert physical_status.statuscode == Fault_SyncExternal;
    access.acctype = AccessType_IFETCH;
    (physical_status, physical_value) = PhysMemRead{8}(physical, access);
    assert physical_status.statuscode == Fault_None;
    assert physical_value == Zeros{8};
    physical.paddress.paspace = PAS_NonSecure;
    (physical_status, physical_value) = PhysMemRead{8}(physical, access);
    assert physical_status.statuscode == Fault_SyncExternal;
    physical.fault.statuscode = Fault_Translation;
    (physical_status, physical_value) = PhysMemRead{8}(physical, access);
    assert physical_status.statuscode == Fault_SyncExternal;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_DataRead;
    assert A64Oracle_GetLastAccessDomain() == A64OracleMemoryAddressDomain_Physical;
    assert A64Oracle_GetLastAccessPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastAccessAddress() ==
        '0000000000000000000000000000000000000000000000000011000000000000';
    assert A64Oracle_GetLastAccessByteCount() == 1;
    assert A64Oracle_GetLastAccessStatusCode() == Fault_SyncExternal;
    assert A64Oracle_GetLastAccessExtFlag() == '0';
    assert A64Oracle_GetLastAccessErrorState() == ErrorState_CE;
    assert A64Oracle_GetLastAccessStore64BStatus() == Zeros{64};
    assert A64Oracle_GetLastFaultPresent();
    assert A64Oracle_GetLastFaultDomain() == A64OracleMemoryAddressDomain_Physical;
    assert A64Oracle_GetLastFaultPASpace() == PAS_NonSecure;
    assert A64Oracle_GetLastFaultAddress() ==
        '0000000000000000000000000000000000000000000000000011000000000000';
    assert A64Oracle_GetLastFaultStatusCode() == Fault_SyncExternal;
    assert A64Oracle_GetLastFaultExtFlag() == '0';
    assert A64Oracle_GetLastFaultErrorState() == ErrorState_CE;
    assert A64Oracle_GetLastFaultStore64BStatus() == Zeros{64};
    physical.fault.statuscode = Fault_None;
    physical.paddress.paspace = PAS_NonSecure;
    physical.paddress.address =
        Zeros{NUM_PABITS - 16} :: '0001000000000000';
    access.acctype = AccessType_Normal;
    physical_status = PhysMemWrite{8}(physical, access, Ones{8});
    assert physical_status.statuscode == Fault_None;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_DataWrite;
    assert A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 1;
    (physical_status, physical_tag_value) = PhysMemTagRead(physical, access);
    assert physical_status.statuscode == Fault_None;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_TagRead;
    assert !A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 16;
    physical_status = PhysMemTagWrite(physical, access, Zeros{4});
    assert physical_status.statuscode == Fault_None;
    assert A64Oracle_GetLastAccessClass() == A64OracleMemoryAccessClass_TagWrite;
    assert A64Oracle_GetLastAccessWrite();
    assert A64Oracle_GetLastAccessByteCount() == 16;
    print "ASL_MEMORY_BRIDGE_PASS";
    return 0;
end;
"""


ASL_MEMORY_FIXTURE_NEGATIVES = (
    r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(0, Zeros{64}, 32, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    A64Oracle_ConfigureMemoryRegion(1, 16, 16, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    return 0;
end;
""",
    r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(0, Ones{64}, 2, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    return 0;
end;
""",
    r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(0, Zeros{64}, 1, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    A64Oracle_FinalizeMemoryRegions();
    return 0;
end;
""",
    r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(0, Zeros{64}, 16, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    A64Oracle_SetMemoryRegionByteChunk(0, 1, 1, Zeros{128});
    return 0;
end;
""",
    r"""
func main() => integer
begin
    A64Oracle_ResetMemoryState();
    A64Oracle_BeginMemoryRegions();
    A64Oracle_ConfigureMemoryRegion(0, Zeros{64}, 98305, PAS_NonSecure,
        TRUE, TRUE, FALSE, FALSE, TRUE);
    return 0;
end;
""",
    "func main() => integer\nbegin\n"
    "    A64Oracle_ResetMemoryState();\n"
    "    A64Oracle_BeginMemoryRegions();\n"
    + "".join(
        "    A64Oracle_ConfigureMemoryRegion("
        f"{index}, Zeros{{64}} + {index * 16}, 1, PAS_NonSecure, "
        "TRUE, TRUE, FALSE, FALSE, TRUE);\n"
        for index in range(9)
    )
    + "    return 0;\nend;\n",
)


def memory_bridge_aslref_fixture() -> None:
    executable_name = os.environ.get("UML_ASLREF_TEST_EXECUTABLE")
    if executable_name is None:
        return
    executable = Path(executable_name)
    require(executable.is_file(), "configured ASLRef executable is absent")
    module = Path(__file__).with_name("a64_aslref_state_memory.asl")
    with tempfile.TemporaryDirectory(prefix="a64-asl-memory-fixture-") as name:
        directory = Path(name)
        prelude = write(directory / "prelude.asl", ASL_MEMORY_FIXTURE_PRELUDE)

        def execute(source: str, suffix: str) -> subprocess.CompletedProcess[str]:
            harness = write(directory / f"harness-{suffix}.asl", source)
            return subprocess.run(
                [
                    str(executable), "--type-check-strict", "--exec",
                    "-1", str(prelude), "-1", str(module), str(harness),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        positive = execute(ASL_MEMORY_FIXTURE_POSITIVE, "positive")
        require(
            positive.returncode == 0
            and "ASL_MEMORY_BRIDGE_PASS" in positive.stdout,
            "ASLRef rejected the positive bounded-memory fixture:\n"
            + positive.stderr
            + positive.stdout,
        )
        for index, source in enumerate(ASL_MEMORY_FIXTURE_NEGATIVES):
            negative = execute(source, f"negative-{index}")
            require(
                negative.returncode != 0,
                f"ASLRef accepted bounded-memory negative fixture {index}",
            )


def lexical_type_dependency_fixture() -> None:
    catalog = AslDependencyCatalog.__new__(AslDependencyCatalog)
    terminal = Definition(
        name="terminal",
        symbols=frozenset({"EndOfInstructionException"}),
        links=frozenset(),
        linked_dependencies=frozenset(),
        calls=frozenset(),
        identifiers=frozenset({"exception", "reason"}),
        text=(
            "type EndOfInstructionException of exception "
            "{ reason : integer };\n"
        ),
    )
    caller = Definition(
        name="caller",
        symbols=frozenset({"EndOfInstruction"}),
        links=frozenset(),
        linked_dependencies=frozenset(),
        calls=frozenset(),
        identifiers=frozenset({"EndOfInstructionException"}),
        text=(
            "func EndOfInstruction()\n"
            "begin throw EndOfInstructionException { reason = 0 }; end;\n"
        ),
    )
    catalog.definitions = {"caller": caller, "terminal": terminal}
    catalog.links = {}
    catalog.symbols = {
        "EndOfInstruction": frozenset({"caller"}),
        "EndOfInstructionException": frozenset({"terminal"}),
    }
    catalog.type_symbols = {
        "EndOfInstructionException": frozenset({"terminal"})
    }
    catalog.constant_symbols = {}
    catalog.variable_symbols = {}
    catalog.lexical_symbols = frozenset({"EndOfInstructionException"})
    dependencies, external = catalog._definition_references(caller)
    require(
        dependencies == {"terminal"} and not external,
        "non-call exception type was omitted from the dependency graph",
    )
    require(
        _declared_symbols(
            "func Decode()\n"
            "constant DecodeLimit : integer = 4;\n"
            "config DecodeMaximum : integer = 8;\n"
            "type DecodeOutcome of enumeration { Pass, Fail };\n",
            "fallback",
        )
        == frozenset(
            {"Decode", "DecodeLimit", "DecodeMaximum", "DecodeOutcome"}
        ),
        "mixed operation/constant/type declarations were not indexed",
    )


def main() -> int:
    require(
        {"case", "do", "looplimit", "of", "var", "while"}
        <= ASL_STANDARD_CALLS,
        "ASL grammar tokens regressed into the interface frontier",
    )
    require(
        {"Real", "RoundDown", "RoundUp", "SqrtRounded"}
        <= ASL_STANDARD_CALLS,
        "ASLRef standard functions regressed into the interface frontier",
    )
    require(
        _global_identifiers(
            "PSTATE.[N,Z,C,V] = value; record.reason = 1; "
            "type Width of integer{0..MaxWidth};"
        )
        == {"PSTATE", "value", "record", "type", "Width", "of", "integer", "MaxWidth"},
        "record/collection fields leaked into global dependency resolution",
    )
    with tempfile.TemporaryDirectory(prefix="a64-aslref-modules-") as name:
        directory = Path(name)
        module_ownership_fixtures(directory)
        alias_source_fixture(directory)
    official_definition_replacement_fixture()
    type_boundary_fixture()
    feature_prelude_fixture()
    lexical_type_dependency_fixture()
    module_dependency_fixture()
    diagnostic_digest_fixture()
    memory_bridge_source_fixture()
    memory_bridge_contract_fixture()
    memory_bridge_aslref_fixture()
    print("A64 ASLRef state-module composition fixtures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
