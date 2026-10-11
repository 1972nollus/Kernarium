# SPDX-License-Identifier: GPL-2.0-or-later
"""The Armv8 additions and optional extensions of the gadget engine's
AArch32 generator (the decoder's `complete` level).

generate_a32.py imports this module. A form of the complete level that
plan_of_complete does not name is the shared semantic library's
(cpu/arm64/aarch32/semantics.hpp), run through the a32_library gadget as the
whole instruction; one it names is lowered onto the host's A64 instruction,
in the way a32_simd.py and a32_neon.py lower VFP and Advanced SIMD. A
lowering that needs an optional A64 feature says so (a32_neon.NEEDS): a
translation on a host without it is the library's, so the gadgets never
execute an instruction the host lacks.

Advanced SIMD forms are groups of a32_neon.py (the crypto instructions work
on whole Q registers); this module adds the groups and the families that are
not Advanced SIMD:

  crc   CRC32 and CRC32C of the low byte, halfword or word of Rm into Rn.
        Operand word: d[0:16] n[16:32] m[32:48], byte offsets of the core
        registers in CpuState.
"""

from __future__ import annotations

import re

import a32_neon
from a32_neon import Variant, add, INDEX_SINGLE

# The families this module adds, after a32_simd's.
FAMILIES: list[str] = ["crc"]

CRC_SIZES = ["b", "h", "w"]


# ---------------------------------------------------------------------------
# Plans

def is_vfp_name(name: str) -> bool:
    """A complete-level form that is VFP (or a VFP-shaped one), not Advanced
    SIMD, though its decoder stem may be shared with an Advanced SIMD form."""
    return re.search(r"_(A2|T2)_[HSD]$|_vfp_", name) is not None


def plan_of_complete(name: str, mnemonic: str):
    """(family, sub, aux) for a form of the complete level, or None when the
    form stays the library's."""
    if mnemonic == "SB":
        return ("hint", 0, 0)
    if mnemonic.startswith("CRC32"):
        castagnoli = "C" if mnemonic.startswith("CRC32C") else ""
        size = CRC_SIZES.index(mnemonic[len("CRC32" + castagnoli):].lower())
        return ("crc", size | (4 if castagnoli else 0), 0)
    if not is_vfp_name(name):
        neon = a32_neon.plan_of(name)
        if neon is not None:
            return ("neon", neon[0], neon[1])
    return None


def need_of_complete(name: str, mnemonic: str) -> str:
    """The host feature (a32_neon.NEEDS) of a lowered form that is not an
    Advanced SIMD one: Advanced SIMD variants carry theirs (kNeonNeed)."""
    if mnemonic.startswith("CRC32"):
        return "crc32"
    return "none"


# ---------------------------------------------------------------------------
# Advanced SIMD groups

def crypto_groups() -> None:
    """The Q-register cryptographic instructions: AES (FEAT_AES) and SHA-1
    and SHA-256 (FEAT_SHA1, FEAT_SHA256). The A32 and A64 forms compute the
    same functions on the same 128-bit operands."""
    def two(name, stem, body, need, acc=True):
        v = Variant(m1="q", acc="q" if acc else None, out="q", body=body,
                    need=need)
        add(name, [stem], "dm", "none", explicit={0: ("", v)},
            index=INDEX_SINGLE, count=1)

    def three(name, stem, body, need):
        v = Variant(n="q", m="q", acc="q", out="q", body=body, need=need)
        add(name, [stem], "dnm", "none", explicit={0: ("", v)},
            index=INDEX_SINGLE, count=1)

    two("aese", "AESE", ["aese v0.16b, v1.16b"], "aes")
    two("aesd", "AESD", ["aesd v0.16b, v1.16b"], "aes")
    two("aesmc", "AESMC", ["aesmc v0.16b, v1.16b"], "aes", acc=False)
    two("aesimc", "AESIMC", ["aesimc v0.16b, v1.16b"], "aes", acc=False)
    two("sha1su1", "SHA1SU1", ["sha1su1 v0.4s, v1.4s"], "sha1")
    two("sha256su0", "SHA256SU0", ["sha256su0 v0.4s, v1.4s"], "sha2")
    # SHA1H: the rotated low word of Qm, the rest of Qd zero.
    two("sha1h", "SHA1H", ["sha1h s0, s1"], "sha1", acc=False)
    three("sha1c", "SHA1C", ["sha1c q0, s1, v2.4s"], "sha1")
    three("sha1p", "SHA1P", ["sha1p q0, s1, v2.4s"], "sha1")
    three("sha1m", "SHA1M", ["sha1m q0, s1, v2.4s"], "sha1")
    three("sha1su0", "SHA1SU0", ["sha1su0 v0.4s, v1.4s, v2.4s"], "sha1")
    three("sha256h", "SHA256H", ["sha256h q0, q1, v2.4s"], "sha2")
    three("sha256h2", "SHA256H2", ["sha256h2 q0, q1, v2.4s"], "sha2")
    three("sha256su1", "SHA256SU1", ["sha256su1 v0.4s, v1.4s, v2.4s"], "sha2")


crypto_groups()


# ---------------------------------------------------------------------------
# Gadgets

def emit_crc(asm) -> None:
    for castagnoli in (0, 1):
        for size, suffix in enumerate(CRC_SIZES):
            mnemonic = f"crc32{'c' if castagnoli else ''}{suffix}"
            asm.gadget(mnemonic, "\n".join([
                "ldr x8, [_stream], #8",
                "ubfx x9, x8, #16, #16", "ubfx x10, x8, #32, #16",
                "and x8, x8, #0xffff",
                "ldr w9, [_cpu, x9]", "ldr w10, [_cpu, x10]",
                f"{mnemonic} w9, w9, w10", "str w9, [_cpu, x8]",
                "next_gadget"]))


def emit(asm) -> None:
    emit_crc(asm)


def header_tables(names: set[str]) -> list[str]:
    def g(name: str) -> str:
        return f"G::{name}" if name in names else "G::count"

    out = ["/* CRC32 and CRC32C: [castagnoli][byte, halfword, word]. */",
           "inline constexpr G kCrc32[2][3] = {"]
    for castagnoli in (0, 1):
        out.append("\t{" + ", ".join(
            g(f"crc32{'c' if castagnoli else ''}{s}") for s in CRC_SIZES) +
            "},")
    out.append("};")
    return out
