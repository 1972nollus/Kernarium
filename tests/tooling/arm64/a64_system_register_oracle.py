#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned Arm system-register selectors used by A64 semantic evidence.

The A64 ISA XML delegates MRS/MSR register selection to implementation-defined
helpers.  Arm's companion System Register XML is therefore the independent
source for the encoded selector and the selected register's feature condition.
Allocation requirements remain owned by the ISA XML; this module only licenses
word-specific runtime gates that are semantically related to the selected
register.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
import tarfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


RELEASE = "2026-06"
ARCHIVE_NAME = f"SysReg_xml_A_profile_FAT-{RELEASE}.tar.gz"
ARCHIVE_SHA256 = "a2c13d741e608272db0cba36d651ac0401a3905ff70c2166f7b337c2274bf0e9"
SOURCE_ORACLE = f"arm-a-profile-system-register-{RELEASE}-xml"
FEATURE_NAME = re.compile(r"\b(FEAT_[A-Za-z0-9_]+)\b")
ENCODING_FIELDS = (
    ("op0", 2),
    ("op1", 3),
    ("CRn", 4),
    ("CRm", 4),
    ("op2", 3),
)

# FPCR and FPSR are baseline AArch64 registers, but the harness must only use
# their native host state when the floating-point execution context is
# available.  FPMR has its own architectural condition; the host exposes that
# capability under the existing FP8 gate.  These are semantic/runtime gates,
# never allocation requirements.
NATIVE_SEMANTIC_FEATURES = {
    "FPCR": ("FEAT_FP",),
    "FPSR": ("FEAT_FP",),
    "FPMR": ("FEAT_FPMR",),
}
EXPECTED_CONDITIONS = {
    "FPCR": frozenset({"FEAT_AA64"}),
    "FPSR": frozenset({"FEAT_AA64"}),
    "FPMR": frozenset({"FEAT_AA64", "FEAT_FPMR"}),
}


@dataclass(frozen=True)
class SystemRegisterRule:
    register: str
    selector: int
    semantic_feature_expressions: tuple[str, ...]

    def text(self) -> str:
        expression = " && ".join(self.semantic_feature_expressions)
        return f"sysreg@5:15={self.selector:015b}=>{expression}"


class SystemRegisterCatalog:
    """Validated subset of a pinned Arm System Register XML archive."""

    def __init__(self, archive: Path):
        if archive.name != ARCHIVE_NAME:
            raise ValueError(
                f"system-register archive is {archive.name!r}, "
                f"expected {ARCHIVE_NAME!r}"
            )
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != ARCHIVE_SHA256:
            raise ValueError(
                f"{archive}: SHA-256 is {digest}, expected pinned "
                f"{ARCHIVE_SHA256}"
            )
        self.archive = archive
        self._rules = tuple(
            self._read_rule(name) for name in NATIVE_SEMANTIC_FEATURES
        )
        selectors = {rule.selector for rule in self._rules}
        if len(selectors) != len(self._rules):
            raise ValueError("pinned system registers have duplicate selectors")

    def _read_rule(self, name: str) -> SystemRegisterRule:
        member_name = (
            f"SysReg_xml_A_profile_FAT-{RELEASE}/AArch64-{name.lower()}.xml"
        )
        with tarfile.open(self.archive, "r:gz") as source:
            for member in source.getmembers():
                path = Path(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(
                        f"{self.archive}: unsafe archive member {member.name!r}"
                    )
                if member.issym() or member.islnk():
                    raise ValueError(
                        f"{self.archive}: link member is not accepted"
                    )
            member = source.getmember(member_name)
            stream = source.extractfile(member)
            if stream is None:
                raise ValueError(f"{self.archive}: cannot read {member_name}")
            root = ET.parse(stream).getroot()

        registers = root.findall(
            ".//register[@execution_state='AArch64'][@is_register='True']"
        )
        if len(registers) != 1:
            raise ValueError(f"{member_name}: expected one AArch64 register")
        register = registers[0]
        if register.findtext("reg_short_name") != name:
            raise ValueError(f"{member_name}: register name drift")
        conditions = frozenset(
            FEATURE_NAME.findall(register.findtext("reg_condition", ""))
        )
        if conditions != EXPECTED_CONDITIONS[name]:
            raise ValueError(
                f"{member_name}: feature condition is {sorted(conditions)!r}, "
                f"expected {sorted(EXPECTED_CONDITIONS[name])!r}"
            )
        groups = {
            text.strip()
            for text in register.itertext()
            if text.strip() == "Float"
        }
        if groups != {"Float"}:
            raise ValueError(f"{member_name}: expected Float register group")

        selectors: list[int] = []
        accessors: set[str] = set()
        for mechanism in register.findall(".//access_mechanism"):
            accessor = mechanism.get("accessor", "")
            if not (accessor.startswith("MRS ") or accessor.startswith("MSRregister ")):
                continue
            fields = {
                item.get("n", ""): item.get("v", "")
                for item in mechanism.findall("./encoding/enc")
            }
            if set(fields) != {field for field, _width in ENCODING_FIELDS}:
                raise ValueError(f"{member_name}: system-access encoding drift")
            values: dict[str, int] = {}
            for field, width in ENCODING_FIELDS:
                bits = fields[field]
                if not bits.startswith("0b") or len(bits[2:]) != width:
                    raise ValueError(
                        f"{member_name}: malformed {field} encoding {bits!r}"
                    )
                values[field] = int(bits[2:], 2)
            # A64 MRS/MSR fixes op0<1> to one and encodes op0<0> as o0 at
            # instruction bit 19.  The remaining fields occupy bits 18:5.
            if values["op0"] & 0b10 == 0:
                raise ValueError(f"{member_name}: op0 is not A64 system space")
            selector = values["op0"] & 1
            for field in ("op1", "CRn", "CRm", "op2"):
                width = dict(ENCODING_FIELDS)[field]
                selector = (selector << width) | values[field]
            selectors.append(selector)
            accessors.add(accessor.split(" ", 1)[0])
        if accessors != {"MRS", "MSRregister"} or len(selectors) != 2:
            raise ValueError(f"{member_name}: expected MRS and MSR accessors")
        if len(set(selectors)) != 1:
            raise ValueError(f"{member_name}: MRS/MSR selectors differ")
        return SystemRegisterRule(
            register=name,
            selector=selectors[0],
            semantic_feature_expressions=NATIVE_SEMANTIC_FEATURES[name],
        )

    def word_semantic_feature_rules(self) -> tuple[str, ...]:
        return tuple(rule.text() for rule in self._rules)
