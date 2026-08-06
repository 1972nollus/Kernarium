#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused evidence for the exact projected A64 system-state bridge.

The always-on portion checks the bridge surface and dispatch structure.  When
the pinned external archives and ASLRef tools are configured, the test also
validates every exposed selector against the official System Register XML and
executes the official MRS/MSR instruction procedures over the bridge.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from a64_aslref_dependencies import (  # noqa: E402
    AslDependencyCatalog,
    _declared_symbols,
)
from generate_a64_aslref_corpus import (  # noqa: E402
    load_interface_modules,
    resolved_interface_map,
    selected_owner_modules,
)
from generate_a64_instruction_allocations import archive_root  # noqa: E402


SYSTEM_REGISTER_ARCHIVE_NAME = "SysReg_xml_A_profile_FAT-2026-06.tar.gz"
SYSTEM_REGISTER_ARCHIVE_SHA256 = (
    "a2c13d741e608272db0cba36d651ac0401a3905ff70c2166f7b337c2274bf0e9"
)
SYSTEM_REGISTER_ROOT = "SysReg_xml_A_profile_FAT-2026-06"
FEATURE = re.compile(r"\bFEAT_[A-Za-z0-9_]+\b")
ENCODING_FIELDS = (("op0", 2), ("op1", 3), ("CRn", 4), ("CRm", 4), ("op2", 3))


@dataclass(frozen=True)
class RegisterRule:
    name: str
    fields: tuple[int, int, int, int, int]
    member_sha256: str
    features: frozenset[str]
    write_access: str

    @property
    def selector(self) -> int:
        value = 0
        for field, (_name, width) in zip(self.fields, ENCODING_FIELDS):
            value = (value << width) | field
        return value


RULES = (
    RegisterRule(
        "TPIDR_EL0", (3, 3, 13, 0, 2),
        "0cca40e7fa0e99945865814c5155c0aa7dfe84eaa7d5fd43a3905f1692495274",
        frozenset({"FEAT_AA64"}), "read-write",
    ),
    RegisterRule(
        "TPIDRRO_EL0", (3, 3, 13, 0, 3),
        "9ac31d658212b088e998517df81435eaf956f7e1400ba5aee6b5cc3bbce3ce0f",
        frozenset({"FEAT_AA64"}), "el0-undefined",
    ),
    RegisterRule(
        "DCZID_EL0", (3, 3, 0, 0, 7),
        "6e2c6c9492fcc8a2943bf5a90f92dd81a98e030dbdaebe600d1252d24073a5c0",
        frozenset({"FEAT_AA64"}), "absent",
    ),
    RegisterRule(
        "CTR_EL0", (3, 3, 0, 0, 1),
        "be0c92665e6b522f358ad78e32a2ce06bee814611069f37175e8531f61f938e8",
        frozenset({"FEAT_AA64"}), "absent",
    ),
    RegisterRule(
        "FPCR", (3, 3, 4, 4, 0),
        "6dd8403263a59adf4cf6615aa3437f799a4fce3a32e0cbb60114de3df95fcb46",
        frozenset({"FEAT_AA64"}), "read-write",
    ),
    RegisterRule(
        "FPSR", (3, 3, 4, 4, 1),
        "32c6d829af5de00d6aa0e9d815ef7f40fe0c2a37655ea1524e7b5c9abc55fffe",
        frozenset({"FEAT_AA64"}), "read-write",
    ),
    RegisterRule(
        "FPMR", (3, 3, 4, 4, 2),
        "3bfbde3027cb36bf67afaef43f853e9b7ce1feed6f1f95e7a36d24ffca311716",
        frozenset({"FEAT_AA64", "FEAT_FPMR"}), "read-write",
    ),
    RegisterRule(
        "NZCV", (3, 3, 4, 2, 0),
        "84c2c8031ce25e53f33002ffb9e58da61733bb789a8ed2b5e7e9c14cf4bb5e53",
        frozenset({"FEAT_AA64"}), "read-write",
    ),
    RegisterRule(
        "CNTFRQ_EL0", (3, 3, 14, 0, 0),
        "ba5709b19ef579e647f388a5607fd6358cef561b0287790dc2a59e85b96aaaa9",
        frozenset({"FEAT_AA64"}), "el0-undefined",
    ),
    RegisterRule(
        "CNTVCT_EL0", (3, 3, 14, 0, 2),
        "c8d59fe6984a26b885e672a218fad8341c8df7ae5e2a0c3ba9de6300fc4e4c68",
        frozenset({"FEAT_AA64"}), "absent",
    ),
)

RULE_BY_NAME = {rule.name: rule for rule in RULES}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_value_error(action, message: str) -> None:
    try:
        action()
    except ValueError:
        return
    raise AssertionError(message)


def bits(value: int, width: int) -> str:
    require(0 <= value < 1 << width, f"{value} does not fit bits({width})")
    return f"'{value:0{width}b}'"


def system_word(read: bool, rule: RegisterRule, target: int) -> int:
    op0, op1, crn, crm, op2 = rule.fields
    base = 0xD5200000 if read else 0xD5000000
    return base | op0 << 19 | op1 << 16 | crn << 12 | crm << 8 | op2 << 5 | target


def immediate_word(op1: int, operand: int, op2: int) -> int:
    return 0xD500401F | op1 << 16 | operand << 8 | op2 << 5


def operation_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?m)^(?:readonly\s+)?func\s+{re.escape(name)}\s*\(",
        source,
    )
    if match is None:
        raise AssertionError(f"cannot locate {name} body")
    following = re.search(
        r"(?m)^(?:(?:readonly\s+)?func|accessor|type|constant)\s+",
        source[match.end() :],
    )
    end = len(source) if following is None else match.end() + following.start()
    return source[match.start() : end]


def source_contract_fixture() -> None:
    directory = Path(__file__).resolve().parent
    environment_path = directory / "a64_aslref_state_environment.asl"
    sysreg_path = directory / "a64_aslref_state_sysreg.asl"
    environment = environment_path.read_text(encoding="utf-8")
    sysreg = sysreg_path.read_text(encoding="utf-8")
    declared = _declared_symbols(environment, environment_path.name) | _declared_symbols(
        sysreg, sysreg_path.name
    )
    required_api = {
        "A64Oracle_InitializeProjectedSystemState",
        "A64Oracle_SetProjectedSigningKey",
        "A64Oracle_IsProjectedSystemStateInstalled",
        "A64Oracle_GetProjectedThreadPointer",
        "A64Oracle_GetProjectedPSTATESSBS",
        "A64Oracle_GetProjectedPSTATEDIT",
        "A64Oracle_GetProjectedPSTATETCO",
        "A64Oracle_GetProjectedPSTATESM",
        "A64Oracle_GetProjectedPSTATEZA",
        "A64Oracle_GetProjectedPSTATEDAIFAccess",
        "A64Oracle_GetProjectedPSTATEDAIF",
        "A64Oracle_GetProjectedMTEEnabled",
        "A64Oracle_GetProjectedMTEExclude",
        "A64Oracle_GetProjectedMTERandomTags",
        "A64Oracle_GetProjectedMTESeed",
        "A64Oracle_GetProjectedMTETag",
        "A64Oracle_GetProjectedCPAAddEnabled",
        "A64Oracle_GetProjectedCPAMultiplyEnabled",
        "A64Oracle_GetProjectedACCDATA",
        "A64Oracle_GetProjectedGCSPointer",
        "A64Oracle_GetProjectedGCSImplemented",
        "A64Oracle_GetProjectedGCSPushEnabled",
        "A64Oracle_GetProjectedGCSStoreEnabled",
        "A64Oracle_GetProjectedSigningKeyLow",
        "A64Oracle_GetProjectedSigningKeyHigh",
        "A64Oracle_GetProjectedSigningEnabled",
        "A64Oracle_SetCounterPolicy",
        "A64Oracle_ReadProjectedVirtualCounter",
        "A64Oracle_GetCounterFrequency",
        "A64Oracle_GetCounterNext",
        "A64Oracle_GetCounterStep",
        "A64Oracle_GetCounterReadCount",
        "A64Oracle_GetProjectedNormalizationObligations",
        "A64Oracle_InitializeProjectedFPSystemState",
        "A64Oracle_GetProjectedFPCR",
        "A64Oracle_GetProjectedFPSR",
        "A64Oracle_GetProjectedFPMR",
        "AArch64_SysRegRead",
        "AArch64_SysRegWrite",
        "AArch64_SysRegRead128",
        "AArch64_SysRegWrite128",
    }
    require(required_api <= declared, "projected system-state API is incomplete")
    require(
        "expected" not in operation_body(environment, "A64Oracle_ReadProjectedVirtualCounter").lower(),
        "counter policy depends on terminal expected state",
    )
    require(
        "return '1111';" in operation_body(
            environment, "A64Oracle_GetProjectedNormalizationObligations"
        ),
        "normalization obligations are not explicit and complete",
    )
    feature_body = operation_body(environment, "IsFeatureImplemented")
    require(
        "allocated && A64Oracle_GCSImplemented" in feature_body
        and "allocated && A64Oracle_SigningEnabled" in feature_body,
        "projected profile booleans can enable an unallocated feature",
    )
    require(
        "A64Oracle_ProjectedPSTATEDAIFHigh ::" in operation_body(
            environment, "A64Oracle_GetProjectedPSTATEDAIF"
        ),
        "non-architectural DAIF high bits are not preserved explicitly",
    )

    selector_helpers = {
        "TPIDR_EL0": "A64Oracle_IsTPIDREL0Selector",
        "TPIDRRO_EL0": "A64Oracle_IsTPIDRROEL0Selector",
        "DCZID_EL0": "A64Oracle_IsDCZIDEL0Selector",
        "CTR_EL0": "A64Oracle_IsCTREL0Selector",
        "FPCR": "A64Oracle_IsFPCRSelector",
        "FPSR": "A64Oracle_IsFPSRSelector",
        "FPMR": "A64Oracle_IsFPMRSelector",
        "NZCV": "A64Oracle_IsNZCVSelector",
        "CNTFRQ_EL0": "A64Oracle_IsCNTFRQEL0Selector",
        "CNTVCT_EL0": "A64Oracle_IsCNTVCTEL0Selector",
    }
    for rule in RULES:
        body = operation_body(sysreg, selector_helpers[rule.name])
        for value, (field, width) in zip(rule.fields, ENCODING_FIELDS):
            require(
                f"{field.lower()} == {bits(value, width)}" in body.lower(),
                f"{rule.name}: sparse selector helper encoding drift",
            )

    read_body = operation_body(sysreg, "AArch64_SysRegRead")
    write_body = operation_body(sysreg, "AArch64_SysRegWrite")
    for rule in RULES:
        helper = selector_helpers[rule.name]
        require(
            read_body.count(helper) == 1,
            f"{rule.name}: read selector is absent or duplicated",
        )
        expected_writes = 1 if rule.write_access == "read-write" else 0
        require(
            write_body.count(helper) == expected_writes,
            f"{rule.name}: write policy disagrees with the exposed profile",
        )
    require(
        read_body.count("A64OracleSysRegOutcome_UnsupportedRead64") == 1
        and write_body.count("A64OracleSysRegOutcome_UnsupportedWrite64") == 1,
        "sparse dispatch lacks one explicit unsupported fallback",
    )
    require(
        "A64OracleSysRegOutcome_UnsupportedRead128" in operation_body(
            sysreg, "AArch64_SysRegRead128"
        )
        and "A64OracleSysRegOutcome_UnsupportedWrite128" in operation_body(
            sysreg, "AArch64_SysRegWrite128"
        ),
        "128-bit selectors do not have explicit unsupported outcomes",
    )


def _normalized(text: str) -> str:
    return " ".join(text.split())


def _mechanism_text(mechanism: ET.Element) -> str:
    return _normalized(" ".join(mechanism.itertext()))


def _el0_segment(text: str) -> str:
    start = text.find("EffectivelyAtEL0")
    if start < 0:
        raise ValueError("system access permission lacks an EL0 branch")
    ends = [
        position
        for marker in ("elsif PSTATE.EL == EL1", "elsif IsHighestEL")
        if (position := text.find(marker, start)) >= 0
    ]
    return text[start : min(ends) if ends else len(text)]


def validate_register_xml(rule: RegisterRule, payload: bytes, check_digest: bool) -> None:
    if check_digest and hashlib.sha256(payload).hexdigest() != rule.member_sha256:
        raise ValueError(f"{rule.name}: official XML member digest drift")
    root = ET.fromstring(payload)
    registers = root.findall(".//register[@execution_state='AArch64'][@is_register='True']")
    if len(registers) != 1:
        raise ValueError(f"{rule.name}: expected one AArch64 register")
    register = registers[0]
    if register.findtext("reg_short_name") != rule.name:
        raise ValueError(f"{rule.name}: register name drift")
    conditions = frozenset(FEATURE.findall(register.findtext("reg_condition", "")))
    if conditions != rule.features:
        raise ValueError(f"{rule.name}: feature condition drift")

    mechanisms: dict[str, ET.Element] = {}
    selectors: list[int] = []
    for mechanism in register.findall(".//access_mechanism"):
        accessor = mechanism.get("accessor", "")
        kind = accessor.split(" ", 1)[0]
        if kind not in {"MRS", "MSRregister"}:
            continue
        if kind in mechanisms:
            raise ValueError(f"{rule.name}: duplicate {kind} mechanism")
        mechanisms[kind] = mechanism
        encoded = {item.get("n", ""): item.get("v", "") for item in mechanism.findall("./encoding/enc")}
        if set(encoded) != {field for field, _width in ENCODING_FIELDS}:
            raise ValueError(f"{rule.name}: encoding field drift")
        values: list[int] = []
        for field, width in ENCODING_FIELDS:
            value = encoded[field]
            if not re.fullmatch(rf"0b[01]{{{width}}}", value):
                raise ValueError(f"{rule.name}: malformed {field} encoding")
            values.append(int(value[2:], 2))
        if tuple(values) != rule.fields:
            raise ValueError(f"{rule.name}: selector encoding drift")
        selector = 0
        for value, (_field, width) in zip(values, ENCODING_FIELDS):
            selector = selector << width | value
        selectors.append(selector)
    if "MRS" not in mechanisms:
        raise ValueError(f"{rule.name}: missing MRS access")
    if any(selector != rule.selector for selector in selectors):
        raise ValueError(f"{rule.name}: MRS/MSR selector mismatch")

    read_el0 = _el0_segment(_mechanism_text(mechanisms["MRS"]))
    if "X{64}(t) =" not in read_el0:
        raise ValueError(f"{rule.name}: EL0 read direction drift")
    write = mechanisms.get("MSRregister")
    if rule.write_access == "absent":
        if write is not None:
            raise ValueError(f"{rule.name}: unexpected MSR mechanism")
    elif write is None:
        raise ValueError(f"{rule.name}: missing MSR mechanism")
    else:
        write_el0 = _el0_segment(_mechanism_text(write))
        if rule.write_access == "read-write":
            if "= X{64}(t)" not in write_el0:
                raise ValueError(f"{rule.name}: EL0 write direction drift")
        elif "then Undefined();" not in write_el0 or "= X{64}(t)" in write_el0:
            raise ValueError(f"{rule.name}: EL0 read-only policy drift")


def official_xml_fixture(archive: Path) -> None:
    if archive.name != SYSTEM_REGISTER_ARCHIVE_NAME:
        raise AssertionError("configured System Register XML archive has the wrong release name")
    require(
        hashlib.sha256(archive.read_bytes()).hexdigest() == SYSTEM_REGISTER_ARCHIVE_SHA256,
        "configured System Register XML archive digest drift",
    )
    payloads: dict[str, bytes] = {}
    with tarfile.open(archive, "r:gz") as source:
        for member in source.getmembers():
            path = Path(member.name)
            require(
                not path.is_absolute() and ".." not in path.parts,
                f"unsafe System Register XML archive member {member.name!r}",
            )
            require(not member.issym() and not member.islnk(), "archive links are not accepted")
        for rule in RULES:
            name = f"{SYSTEM_REGISTER_ROOT}/AArch64-{rule.name.lower()}.xml"
            stream = source.extractfile(source.getmember(name))
            require(stream is not None, f"cannot read {name}")
            payloads[rule.name] = stream.read()
    for rule in RULES:
        validate_register_xml(rule, payloads[rule.name], True)

    root = ET.fromstring(payloads["TPIDR_EL0"])
    root.find(".//access_mechanism/encoding/enc[@n='op2']").set("v", "0b111")  # type: ignore[union-attr]
    expect_value_error(
        lambda: validate_register_xml(RULE_BY_NAME["TPIDR_EL0"], ET.tostring(root), False),
        "selector-encoding drift negative was accepted",
    )
    root = ET.fromstring(payloads["TPIDR_EL0"])
    root.find(".//reg_condition").text += " and FEAT_GCS is implemented"  # type: ignore[union-attr,operator]
    expect_value_error(
        lambda: validate_register_xml(RULE_BY_NAME["TPIDR_EL0"], ET.tostring(root), False),
        "feature-condition drift negative was accepted",
    )
    root = ET.fromstring(payloads["TPIDR_EL0"])
    read_text = root.find(".//access_mechanism[@accessor='MRS TPIDR_EL0']//pstext")
    require(read_text is not None and read_text.text is not None, "cannot construct access negative")
    read_text.text = read_text.text.replace("X{64}(t) =", "Z{64}(t) =")
    expect_value_error(
        lambda: validate_register_xml(RULE_BY_NAME["TPIDR_EL0"], ET.tostring(root), False),
        "EL0-access drift negative was accepted",
    )


def _projected_initialization(
    *, gcs_implemented: str = "FALSE", signing_enabled: str = "FALSE"
) -> str:
    return f"""A64Oracle_InitializeProjectedSystemState(
        {bits(0x0123456789ABCDEF, 64)},
        TRUE, FALSE, TRUE, FALSE, TRUE,
        TRUE, {bits(0xA5, 8)},
        TRUE, {bits(0xA55A, 16)}, FALSE,
        {bits(0x1234, 16)}, {bits(0xC7, 8)},
        TRUE, FALSE, {bits(0x89ABCDEF, 32)},
        {bits(0xFEDCBA9876543210, 64)},
        {gcs_implemented}, TRUE, FALSE, {signing_enabled}
    );"""


def projected_state_harness() -> str:
    lines = [
        "func main() => integer",
        "begin",
        "    A64Oracle_ResetUserRegisterState();",
        "    A64Oracle_ResetStateEnvironment();",
        "    A64Oracle_ResetFPState();",
        "    A64Oracle_ResetSysRegState();",
        "    " + _projected_initialization().replace("\n", "\n    "),
        "    A64Oracle_InitializeProjectedFPSystemState(",
        f"        {bits(0x76543210, 32)}, {bits(0x89ABCDEF, 32)},",
        f"        {bits(0x0123456789ABCDEF, 64)}",
        "    );",
        "    assert A64Oracle_IsProjectedSystemStateInstalled();",
        f"    assert A64Oracle_GetProjectedThreadPointer() == {bits(0x0123456789ABCDEF, 64)};",
        "    assert A64Oracle_GetProjectedPSTATESSBS();",
        "    assert !A64Oracle_GetProjectedPSTATEDIT();",
        "    assert A64Oracle_GetProjectedPSTATETCO();",
        "    assert !A64Oracle_GetProjectedPSTATESM();",
        "    assert A64Oracle_GetProjectedPSTATEZA();",
        "    assert A64Oracle_GetProjectedPSTATEDAIFAccess();",
        f"    assert A64Oracle_GetProjectedPSTATEDAIF() == {bits(0xA5, 8)};",
        "    assert A64Oracle_GetProjectedMTEEnabled();",
        f"    assert A64Oracle_GetProjectedMTEExclude() == {bits(0xA55A, 16)};",
        "    assert !A64Oracle_GetProjectedMTERandomTags();",
        f"    assert A64Oracle_GetProjectedMTESeed() == {bits(0x1234, 16)};",
        f"    assert A64Oracle_GetProjectedMTETag() == {bits(0xC7, 8)};",
        "    assert A64Oracle_GetProjectedCPAAddEnabled();",
        "    assert !A64Oracle_GetProjectedCPAMultiplyEnabled();",
        f"    assert A64Oracle_GetProjectedACCDATA() == {bits(0x89ABCDEF, 32)};",
        f"    assert A64Oracle_GetProjectedGCSPointer() == {bits(0xFEDCBA9876543210, 64)};",
        "    assert !A64Oracle_GetProjectedGCSImplemented();",
        "    assert A64Oracle_GetProjectedGCSPushEnabled();",
        "    assert !A64Oracle_GetProjectedGCSStoreEnabled();",
        "    assert !A64Oracle_GetProjectedSigningEnabled();",
        f"    assert A64Oracle_FPCR == {bits(0x76543210, 64)};",
        f"    assert A64Oracle_FPSR == {bits(0x89ABCDEF, 64)};",
        f"    assert A64Oracle_FPMR == {bits(0x0123456789ABCDEF, 64)};",
        "    assert A64Oracle_GetProjectedNormalizationObligations() == '1111';",
    ]
    key_accessors = ("APIA", "APIB", "APDA", "APDB", "APGA")
    for index, prefix in enumerate(key_accessors):
        low = 0x1111111111111100 + index
        high = 0xEEEEEEEEEEEEEE00 + index
        lines.extend(
            [
                f"    A64Oracle_SetProjectedSigningKey({index}, {bits(low, 64)}, {bits(high, 64)});",
                f"    assert A64Oracle_GetProjectedSigningKeyLow({index}) == {bits(low, 64)};",
                f"    assert A64Oracle_GetProjectedSigningKeyHigh({index}) == {bits(high, 64)};",
                f"    {prefix}KeyLo_EL1() = {bits(low + 0x10, 64)};",
                f"    {prefix}KeyHi_EL1() = {bits(high + 0x10, 64)};",
                f"    assert A64Oracle_GetProjectedSigningKeyLow({index}) == {bits(low + 0x10, 64)};",
                f"    assert A64Oracle_GetProjectedSigningKeyHigh({index}) == {bits(high + 0x10, 64)};",
            ]
        )
    lines.extend(
        [
            "    var gcr : GCR_EL1_Type = Zeros{64};",
            "    gcr.RRND = '1';",
            f"    gcr.Exclude = {bits(0x0F0F, 16)};",
            "    GCR_EL1() = gcr;",
            "    assert A64Oracle_GetProjectedMTERandomTags();",
            f"    assert A64Oracle_GetProjectedMTEExclude() == {bits(0x0F0F, 16)};",
            "    var rgsr : RGSR_EL1_Type = Zeros{64};",
            f"    rgsr.SEED = Zeros{{32}} :: {bits(0xBEEF, 16)};",
            "    rgsr.TAG = '1010';",
            "    RGSR_EL1() = rgsr;",
            f"    assert A64Oracle_GetProjectedMTESeed() == {bits(0xBEEF, 16)};",
            f"    assert A64Oracle_GetProjectedMTETag() == {bits(0xCA, 8)};",
            "    assert SCTLR2_EL1().CPTA0 == '1';",
            "    assert SCTLR2_EL1().CPTM0 == '0';",
            "    assert SCTLR_EL1().ATA0 == '1';",
            "    assert SCTLR_EL1().UMA == '1';",
            "    assert SCTLR_EL1().EnIA == '0';",
            "    var accdata : ACCDATA_EL1_Type = Zeros{64};",
            f"    accdata.ACCDATA = {bits(0x10203040, 32)};",
            "    ACCDATA_EL1() = accdata;",
            f"    assert A64Oracle_GetProjectedACCDATA() == {bits(0x10203040, 32)};",
            "    var gcscr : GCSCRE0_EL1_Type = Zeros{64};",
            "    gcscr.STREn = '1';",
            "    gcscr.PUSHMEn = '0';",
            "    GCSCRE0_EL1() = gcscr;",
            "    assert !A64Oracle_GetProjectedGCSPushEnabled();",
            "    assert A64Oracle_GetProjectedGCSStoreEnabled();",
            f"    GCSPR_EL0() = {bits(0x0BADF00DCAFEBABE, 64)};",
            f"    assert A64Oracle_GetProjectedGCSPointer() == {bits(0x0BADF00DCAFEBABE, 64)};",
            f"    TPIDR_EL0() = {bits(0xDEADBEEF01234567, 64)};",
            f"    assert A64Oracle_GetProjectedThreadPointer() == {bits(0xDEADBEEF01234567, 64)};",
            f"    A64Oracle_SetCounterPolicy({bits(24_000_000, 64)}, {bits((1 << 64) - 2, 64)}, {bits(2, 64)});",
            "    let counter_first : bits(64) = A64Oracle_ReadProjectedVirtualCounter();",
            f"    assert counter_first == {bits((1 << 64) - 2, 64)};",
            "    let counter_second : bits(64) = A64Oracle_ReadProjectedVirtualCounter();",
            "    assert counter_second == Ones{64};",
            "    let counter_third : bits(64) = A64Oracle_ReadProjectedVirtualCounter();",
            "    assert counter_third == Ones{64};",
            "    assert A64Oracle_GetCounterNext() == Ones{64};",
            f"    assert A64Oracle_GetCounterFrequency() == {bits(24_000_000, 64)};",
            f"    assert A64Oracle_GetCounterStep() == {bits(2, 64)};",
            "    assert A64Oracle_GetCounterReadCount() == 3;",
            "    A64Oracle_ResetStateEnvironment();",
            "    " + _projected_initialization(gcs_implemented="TRUE", signing_enabled="TRUE").replace("\n", "\n    "),
            "    assert !IsFeatureImplemented(FEAT_GCS);",
            "    assert !IsFeatureImplemented(FEAT_PAuth);",
            "    A64Oracle_EnableFeature(FEAT_GCS);",
            "    A64Oracle_EnableFeature(FEAT_PAuth);",
            "    assert IsFeatureImplemented(FEAT_GCS);",
            "    assert IsFeatureImplemented(FEAT_PAuth);",
            "    " + _projected_initialization(gcs_implemented="FALSE", signing_enabled="FALSE").replace("\n", "\n    "),
            "    assert !IsFeatureImplemented(FEAT_GCS);",
            "    assert !IsFeatureImplemented(FEAT_PAuth);",
            '    print "A64_PROJECTED_SYSTEM_STATE_PASS";',
            "    return 0;",
            "end;",
            "",
        ]
    )
    return "\n".join(lines)


def mrs_harness() -> str:
    expected = {
        "TPIDR_EL0": 0x0123456789ABCDEF,
        "TPIDRRO_EL0": 0,
        "DCZID_EL0": 4,
        "CTR_EL0": 0x84448004,
        "FPCR": 0x07F79F07,
        "FPSR": 0xF800009F,
        "FPMR": 0x0000003FFF7FC1FF,
        "NZCV": 0xA0000000,
        "CNTFRQ_EL0": 19_200_000,
        "CNTVCT_EL0": 0x100,
    }
    lines = [
        "func main() => integer",
        "begin",
        "    A64Oracle_ResetUserRegisterState();",
        "    A64Oracle_ResetStateEnvironment();",
        "    A64Oracle_ResetFPState();",
        "    A64Oracle_ResetSysRegState();",
        "    " + _projected_initialization().replace("\n", "\n    "),
        "    A64Oracle_InitializeProjectedFPSystemState(Ones{32}, Ones{32}, Ones{64});",
        "    A64Oracle_SetArchitecturalNZCV('1010');",
        f"    A64Oracle_SetCounterPolicy({bits(19_200_000, 64)}, {bits(0x100, 64)}, {bits(7, 64)});",
    ]
    for target, rule in enumerate(RULES):
        lines.extend(
            [
                f"    instr_mrs({bits(system_word(True, rule, target), 32)});",
                f"    assert A64Oracle_R[[{target}]] == {bits(expected[rule.name], 64)};",
                "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_Read64;",
                f"    assert A64Oracle_LastSysRegSelector == {bits(rule.selector, 16)};",
                f"    assert A64Oracle_LastSysRegTarget == {target};",
            ]
        )
    lines.extend(
        [
            "    assert A64Oracle_GetCounterReadCount() == 1;",
            f"    instr_mrs({bits(system_word(True, RULE_BY_NAME['CNTVCT_EL0'], 10), 32)});",
            f"    assert A64Oracle_R[[10]] == {bits(0x107, 64)};",
            "    assert A64Oracle_GetCounterReadCount() == 2;",
            f"    A64Oracle_SetX(11, {bits(0x55AA55AA55AA55AA, 64)});",
            f"    instr_mrs({bits(0xD5200000 | 3 << 19 | 11, 32)});",
            f"    assert A64Oracle_R[[11]] == {bits(0x55AA55AA55AA55AA, 64)};",
            "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_UnsupportedRead64;",
            f"    assert A64Oracle_LastSysRegSelector == {bits(0xC000, 16)};",
            "    AArch64_SysRegRead128('11', '000', '0000', '0000', '000', 12, 13);",
            "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_UnsupportedRead128;",
            "    assert A64Oracle_LastSysRegTarget == 12;",
            "    assert A64Oracle_LastSysRegTarget2 == 13;",
            '    print "A64_OFFICIAL_MRS_PASS";',
            "    return 0;",
            "end;",
            "",
        ]
    )
    return "\n".join(lines)


def msr_register_harness() -> str:
    writable = [rule for rule in RULES if rule.write_access == "read-write"]
    readonly = [rule for rule in RULES if rule.write_access != "read-write"]
    values = {
        "TPIDR_EL0": 0xF0E1D2C3B4A59687,
        "FPCR": (1 << 64) - 1,
        "FPSR": (1 << 64) - 1,
        "FPMR": (1 << 64) - 1,
        "NZCV": 0x50000000,
    }
    expected = {
        "TPIDR_EL0": 0xF0E1D2C3B4A59687,
        "FPCR": 0x07F79F07,
        "FPSR": 0xF800009F,
        "FPMR": 0x0000003FFF7FC1FF,
        "NZCV": 0x5,
    }
    getter = {
        "TPIDR_EL0": "A64Oracle_GetProjectedThreadPointer()",
        "FPCR": "A64Oracle_FPCR",
        "FPSR": "A64Oracle_FPSR",
        "FPMR": "A64Oracle_FPMR",
        "NZCV": "ZeroExtend{64}(A64Oracle_GetArchitecturalNZCV())",
    }
    lines = [
        projected_state_harness().replace("func main()", "func projected_fixture()"),
        "func main() => integer",
        "begin",
        "    let projected_result : integer = projected_fixture();",
        "    assert projected_result == 0;",
        "    A64Oracle_ResetUserRegisterState();",
        "    A64Oracle_ResetStateEnvironment();",
        "    A64Oracle_ResetFPState();",
        "    A64Oracle_ResetSysRegState();",
        "    " + _projected_initialization().replace("\n", "\n    "),
    ]
    for target, rule in enumerate(writable):
        lines.extend(
            [
                f"    A64Oracle_SetX({target}, {bits(values[rule.name], 64)});",
                f"    instr_msr_reg({bits(system_word(False, rule, target), 32)});",
                f"    assert {getter[rule.name]} == {bits(expected[rule.name], 64)};",
                "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_Write64;",
                f"    assert A64Oracle_LastSysRegSelector == {bits(rule.selector, 16)};",
            ]
        )
    for target, rule in enumerate(readonly, start=len(writable)):
        lines.extend(
            [
                f"    A64Oracle_SetX({target}, {bits(0x1111000000000000 + target, 64)});",
                f"    instr_msr_reg({bits(system_word(False, rule, target), 32)});",
                "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_UnsupportedWrite64;",
                f"    assert A64Oracle_LastSysRegSelector == {bits(rule.selector, 16)};",
            ]
        )
    lines.extend(
        [
            "    AArch64_SysRegWrite128('11', '000', '0000', '0000', '000', 12, 13);",
            "    assert A64Oracle_LastSysRegOutcome == A64OracleSysRegOutcome_UnsupportedWrite128;",
            "    assert A64Oracle_LastSysRegTarget == 12;",
            "    assert A64Oracle_LastSysRegTarget2 == 13;",
            '    print "A64_OFFICIAL_MSR_REGISTER_PASS";',
            "    return 0;",
            "end;",
            "",
        ]
    )
    return "\n".join(lines)


def msr_immediate_harness() -> str:
    instructions = {
        "ssbs_set": immediate_word(3, 1, 1),
        "ssbs_clear": immediate_word(3, 0, 1),
        "dit_set": immediate_word(3, 1, 2),
        "dit_clear": immediate_word(3, 0, 2),
        "tco_set": immediate_word(3, 1, 4),
        "tco_clear": immediate_word(3, 0, 4),
        "sm_set": immediate_word(3, 3, 3),
        "sm_clear": immediate_word(3, 2, 3),
        "za_set": immediate_word(3, 5, 3),
        "za_clear": immediate_word(3, 4, 3),
        "smza_set": immediate_word(3, 7, 3),
        "smza_clear": immediate_word(3, 6, 3),
        "daif_set": immediate_word(3, 0xB, 6),
        "daif_clear": immediate_word(3, 0x3, 7),
    }
    call = lambda name: f"    instr_msr_imm({bits(instructions[name], 32)});"
    return "\n".join(
        [
            "func main() => integer",
            "begin",
            "    A64Oracle_ResetUserRegisterState();",
            "    A64Oracle_ResetPCState();",
            "    A64Oracle_ResetStateEnvironment();",
            "    A64Oracle_ResetEventState();",
            "    A64Oracle_ResetFPState();",
            "    A64Oracle_ResetSVEState();",
            "    A64Oracle_ResetSMEState();",
            "    " + _projected_initialization().replace("\n", "\n    "),
            "    A64Oracle_EnableFeature(FEAT_SSBS);",
            "    A64Oracle_EnableFeature(FEAT_DIT);",
            "    A64Oracle_EnableFeature(FEAT_MTE);",
            "    A64Oracle_EnableFeature(FEAT_SME);",
            call("ssbs_set"), "    assert A64Oracle_GetProjectedPSTATESSBS();",
            call("ssbs_clear"), "    assert !A64Oracle_GetProjectedPSTATESSBS();",
            call("dit_set"), "    assert A64Oracle_GetProjectedPSTATEDIT();",
            call("dit_clear"), "    assert !A64Oracle_GetProjectedPSTATEDIT();",
            call("tco_set"), "    assert A64Oracle_GetProjectedPSTATETCO();",
            call("tco_clear"), "    assert !A64Oracle_GetProjectedPSTATETCO();",
            call("sm_set"), "    assert A64Oracle_GetProjectedPSTATESM();",
            call("sm_clear"), "    assert !A64Oracle_GetProjectedPSTATESM();",
            call("za_set"), "    assert A64Oracle_GetProjectedPSTATEZA();",
            call("za_clear"), "    assert !A64Oracle_GetProjectedPSTATEZA();",
            call("smza_set"),
            "    assert A64Oracle_GetProjectedPSTATESM();",
            "    assert A64Oracle_GetProjectedPSTATEZA();",
            call("smza_clear"),
            "    assert !A64Oracle_GetProjectedPSTATESM();",
            "    assert !A64Oracle_GetProjectedPSTATEZA();",
            call("daif_set"),
            f"    assert A64Oracle_GetProjectedPSTATEDAIF() == {bits(0xAF, 8)};",
            call("daif_clear"),
            f"    assert A64Oracle_GetProjectedPSTATEDAIF() == {bits(0xAC, 8)};",
            '    print "A64_OFFICIAL_MSR_IMMEDIATE_PASS";',
            "    return 0;",
            "end;",
            "",
        ]
    )


def execute_official_forms(architecture: Path, aslref: Path, bundler: Path) -> None:
    directory = Path(__file__).resolve().parent
    base_interface = directory / "a64_aslref_user_state_interface.asl"
    harnesses = {
        "mrs.xml": (mrs_harness(), "A64_OFFICIAL_MRS_PASS"),
        "msr_reg.xml": (msr_register_harness(), "A64_OFFICIAL_MSR_REGISTER_PASS"),
        "msr_imm.xml": (msr_immediate_harness(), "A64_OFFICIAL_MSR_IMMEDIATE_PASS"),
    }
    with tempfile.TemporaryDirectory(prefix="a64-system-state-") as temporary:
        temporary_path = Path(temporary)
        architecture_root = archive_root(architecture, temporary_path / "architecture")
        catalog = AslDependencyCatalog(architecture_root)
        module_paths, module_declarations, owners = load_interface_modules(base_interface, None)
        frontier = {
            interface
            for filename in harnesses
            for interface in catalog.for_file(filename).missing_interfaces
        }
        resolved = resolved_interface_map(frontier, owners)
        for index, (filename, (harness_source, marker)) in enumerate(harnesses.items()):
            status = catalog.for_file(filename)
            selected = selected_owner_modules(status.missing_interfaces, resolved)
            supplied = frozenset(
                symbol for owner in selected for symbol in module_declarations[owner]
            )
            form = temporary_path / f"form-{index}"
            form.mkdir()
            generated = subprocess.run(
                [
                    sys.executable, "-B", str(bundler), "--make-funs", "-j", "1",
                    "-o", str(form),
                    str(architecture_root / catalog.operation_source_file(filename)),
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )
            require(
                generated.returncode == 0 and (form / "instructions.asl").is_file(),
                f"cannot bundle {filename}:\n{generated.stderr}{generated.stdout}",
            )
            shared = form / "shared_subset.asl"
            shared.write_text(
                catalog.shared_subset(
                    status,
                    supplied,
                    tuple(module_paths[owner].read_text(encoding="utf-8") for owner in selected),
                ),
                encoding="utf-8",
            )
            harness = form / "harness.asl"
            harness.write_text(harness_source, encoding="utf-8")
            module_arguments: list[str] = []
            for owner in selected:
                module_arguments.extend(["-1", str(module_paths[owner])])
            executed = subprocess.run(
                [
                    str(aslref), "--type-check-strict", "--exec",
                    "-1", str(form / "instructions.asl"),
                    "-1", str(shared),
                    "-1", str(base_interface),
                    *module_arguments,
                    str(harness),
                ],
                capture_output=True,
                text=True,
                timeout=90,
            )
            require(
                executed.returncode == 0 and marker in executed.stdout,
                f"ASLRef rejected {filename} system-state fixture:\n"
                + executed.stderr
                + executed.stdout,
            )


def configured_external_fixtures() -> None:
    system_archive_name = os.environ.get("UML_A64_SYSTEM_REGISTER_ARCHIVE")
    architecture_name = os.environ.get("UML_A64_ARCHITECTURE_ARCHIVE")
    aslref_name = os.environ.get("UML_ASLREF_TEST_EXECUTABLE")
    bundler_name = os.environ.get("UML_ASLREF_BUNDLER")
    if system_archive_name is not None:
        archive = Path(system_archive_name)
        require(archive.is_file(), "configured System Register XML archive is absent")
        official_xml_fixture(archive)
    configured = [architecture_name, aslref_name, bundler_name]
    require(
        not any(configured) or all(configured),
        "official execution requires architecture archive, ASLRef, and bundler together",
    )
    if all(configured):
        architecture = Path(architecture_name)  # type: ignore[arg-type]
        aslref = Path(aslref_name)  # type: ignore[arg-type]
        bundler = Path(bundler_name)  # type: ignore[arg-type]
        for path, label in (
            (architecture, "architecture archive"),
            (aslref, "ASLRef executable"),
            (bundler, "ASLRef bundler"),
        ):
            require(path.is_file(), f"configured {label} is absent")
        execute_official_forms(architecture, aslref, bundler)


def main() -> int:
    source_contract_fixture()
    configured_external_fixtures()
    print("A64 ASLRef projected system-state fixtures passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
