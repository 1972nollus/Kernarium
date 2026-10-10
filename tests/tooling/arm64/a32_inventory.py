#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 instruction inventory: one row per XML encoding.

tests/unit/arm64/a32_instruction_inventory.tsv lists every encoding of the
pinned Arm AArch32 release (A32, T16 and T32, Base and FP&SIMD) with what the
virtual CPU's profile does with it. generate_a32_encoding_table.py writes the
file from the model; validate_a32_instruction_inventory.py checks it against
the generated table without the XML. Both use the rules below.

Columns:

- stable_id: BASE-nnnn or FPSIMD-nnnn, in the release's index order.
- section: Base or FP&SIMD, as the release indexes the page.
- heading: the instruction page's heading in the release's index.
- encoding: the XML encoding name, which is also the decoder's Form.
- isa: A32, T16 or T32.
- mnemonic: the encoding's mnemonic.
- level: the smallest A32Support level that runs it (base, simd, complete).
- feature: the optional extension a host must have, or baseline.
- profile: executed, restricted (the fields select what EL0 may touch),
  nop (executes as a NOP) or undefined (no word of it executes).
- engine_status: absent for undefined encodings, whose corpus cases expect an
  illegal instruction; pending for the others until an engine passes the
  corpus for them (verified).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import a32_profile_policy as policy


COLUMNS = (
    "stable_id", "section", "heading", "encoding", "isa", "mnemonic", "level",
    "feature", "profile", "engine_status",
)
SECTION_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD"}
ENGINE_STATUSES = ("absent", "pending", "verified")
PROFILES = ("executed", "restricted", "nop", "undefined")
LEVELS = ("base", "simd", "complete")


def profile_of(name: str, availability: str, check: str, nop: bool) -> str:
    """What the profile does with an encoding, from its table entry."""
    if availability == "undefined_at_el0" or name in policy.NEVER_EXECUTED:
        return "undefined"
    if check != "none":
        return "restricted"
    return "nop" if nop else "executed"


def status_of(profile: str) -> str:
    return "absent" if profile == "undefined" else "pending"


def rows(entries) -> list[dict[str, str]]:
    """Rows from (section, heading, name, isa, mnemonic, level, feature,
    availability, check, nop) tuples in index order."""
    counts = {section: 0 for section in SECTION_PREFIXES}
    result = []
    for section, heading, name, isa, mnemonic, level, feature, availability, check, nop in entries:
        counts[section] += 1
        profile = profile_of(name, availability, check, nop)
        result.append({
            "stable_id": f"{SECTION_PREFIXES[section]}-{counts[section]:04d}",
            "section": section,
            "heading": heading,
            "encoding": name,
            "isa": isa,
            "mnemonic": mnemonic,
            "level": level,
            "feature": feature or "baseline",
            "profile": profile,
            "engine_status": status_of(profile),
        })
    return result
