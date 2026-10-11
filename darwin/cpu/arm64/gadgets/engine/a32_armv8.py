# SPDX-License-Identifier: GPL-2.0-or-later
"""The Armv8 additions and optional extensions of the gadget engine's
AArch32 generator (the decoder's `complete` level).

generate_a32.py imports this module. A form of the complete level that
plan_of_complete does not name is the shared semantic library's
(cpu/arm64/aarch32/semantics.hpp), run through the a32_library gadget as the
whole instruction; one it names is lowered onto the host's A64 instruction,
in the same way a32_simd.py and a32_neon.py lower VFP and Advanced SIMD.
"""

from __future__ import annotations

# The families this module adds, after a32_simd's.
FAMILIES: list[str] = []


def plan_of_complete(name: str, mnemonic: str):
    """(family, sub, aux) for a form of the complete level, or None when the
    form stays the library's."""
    return None


def need_of_complete(name: str, mnemonic: str) -> str:
    """The host feature (a32_neon.NEEDS) a lowered complete-level form needs."""
    return "none"
