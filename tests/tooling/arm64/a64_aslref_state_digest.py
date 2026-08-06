#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Project an executed user-state ASL specimen into the evidence digest.

This is a representation adapter, not an instruction-semantics oracle.  The
instruction result comes from ASLRef.  Fields outside the minimal ASL user
state use the explicitly listed deterministic ``CpuState{}`` defaults so that
the instruction-boundary result can be joined to native and harness evidence.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import struct
from collections.abc import Mapping, Sequence


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
GUEST_PC = 0x4F000000
EMPTY_MEMORY_DIGEST = "47fe0d7eaf8e51e3"
POINTER_AUTH_KEYS = (
    (0x84BE85CE9804E94B, 0xEC280D074B7AC6D3),
    (0x4A657B6456CBC3D1, 0x2B7E151628AED2A6),
    (0x09CF4F3CABF71528, 0x7F4A7C159E3779B9),
    (0xD1B54A32D192ED03, 0x94D049BB133111EB),
    (0x2545F4914F6CDD1D, 0xBF58476D1CE4E5B9),
)
ADAPTER_CONTRACT = (
    "cpu-state-v2:user-asl-registers+zero-sp+explicit-pc+nzcv-nibble-at-31:28+"
    "zero-vectors-fpcr-fpsr-fpmr-tls+mte-reset-seed-1-random-"
    "0x9e3779b97f4a7c15+exclusive-address-all-ones+deterministic-pauth-keys+"
    "pauth-enabled"
)


class Digest:
    def __init__(self) -> None:
        self.value = FNV_OFFSET

    def bytes(self, values: bytes) -> None:
        for value in values:
            self.value = ((self.value ^ value) * FNV_PRIME) & MASK64

    def scalar(self, value: int, size: int) -> None:
        self.bytes(value.to_bytes(size, "little", signed=False))

    def boolean(self, value: bool) -> None:
        self.scalar(int(value), 1)


def architecture_state_digest(
    registers: list[int],
    *,
    pc: int,
    nzcv: int,
    nzcv_raw: int | None = None,
    sp: int = 0,
    vectors: Sequence[int] | None = None,
    fpcr: int = 0,
    fpsr: int = 0,
    fpmr: int = 0,
    tls: int = 0,
    pstate_ssbs: bool = False,
    pstate_dit: bool = False,
    pstate_tco: bool = False,
    pstate_sm: bool = False,
    pstate_za: bool = False,
    pstate_daif_access: bool = False,
    pstate_daif: int = 0,
    mte_enabled: bool = False,
    mte_exclude: int = 0,
    mte_tag_choices: Sequence[int] | None = None,
    mte_random_tags: bool = False,
    mte_seed: int = 1,
    mte_tag: int = 0,
    mte_random_state: int = 0x9E3779B97F4A7C15,
    cpa_enabled: bool = False,
    cpa_multiply_enabled: bool = False,
    accdata: int = 0,
    exclusive_address: int = MASK64,
    exclusive_key: int = 0,
    exclusive_value: int = 0,
    exclusive_value2: int = 0,
    exclusive_version: int = 0,
    exclusive_size: int = 0,
    gcs_pointer: int = 0,
    gcs_implemented: bool = False,
    gcs_push_enabled: bool = False,
    gcs_store_enabled: bool = False,
    pointer_auth_keys: Sequence[tuple[int, int]] | None = None,
    pointer_authentication_enabled: bool = True,
) -> str:
    """Digest ASL-visible state plus the declared deterministic defaults."""
    if len(registers) != 31:
        raise ValueError("A64 state projection requires exactly 31 GPRs")
    if any(value < 0 or value > MASK64 for value in registers):
        raise ValueError("A64 register value is outside bits(64)")
    if not 0 <= nzcv < 16:
        raise ValueError("ASL NZCV projection is not a four-bit value")
    if nzcv_raw is not None and not 0 <= nzcv_raw < 1 << 32:
        raise ValueError("raw CpuState NZCV storage is outside bits(32)")
    if vectors is None:
        vectors = (0,) * 32
    if len(vectors) != 32 or any(
        value < 0 or value >= 1 << 128 for value in vectors
    ):
        raise ValueError("A64 state projection requires 32 bits(128) vectors")
    for name, value, width in (
        ("sp", sp, 64),
        ("pc", pc, 64),
        ("fpcr", fpcr, 32),
        ("fpsr", fpsr, 32),
        ("fpmr", fpmr, 64),
        ("tls", tls, 64),
        ("pstate_daif", pstate_daif, 8),
        ("mte_exclude", mte_exclude, 16),
        ("mte_seed", mte_seed, 16),
        ("mte_tag", mte_tag, 8),
        ("mte_random_state", mte_random_state, 64),
        ("accdata", accdata, 32),
        ("exclusive_address", exclusive_address, 64),
        ("exclusive_key", exclusive_key, 64),
        ("exclusive_value", exclusive_value, 64),
        ("exclusive_value2", exclusive_value2, 64),
        ("exclusive_version", exclusive_version, 64),
        ("exclusive_size", exclusive_size, 8),
        ("gcs_pointer", gcs_pointer, 64),
    ):
        if value < 0 or value >= 1 << width:
            raise ValueError(f"{name} is outside bits({width})")
    if mte_tag_choices is None:
        mte_tag_choices = (0,) * 256
    if len(mte_tag_choices) != 256 or any(
        value < 0 or value >= 1 << 8 for value in mte_tag_choices
    ):
        raise ValueError("MTE tag-choice projection requires 256 bytes")
    if pointer_auth_keys is None:
        pointer_auth_keys = POINTER_AUTH_KEYS
    if len(pointer_auth_keys) != 5 or any(
        len(key) != 2 or any(value < 0 or value > MASK64 for value in key)
        for key in pointer_auth_keys
    ):
        raise ValueError("pointer-auth projection requires five bits(128) keys")

    digest = Digest()
    for value in registers:
        digest.scalar(value, 8)
    digest.scalar(sp, 8)
    digest.scalar(pc, 8)
    digest.scalar(nzcv << 28 if nzcv_raw is None else nzcv_raw, 4)
    for value in vectors:
        digest.scalar(value, 16)
    digest.scalar(fpcr, 4)
    digest.scalar(fpsr, 4)
    digest.scalar(fpmr, 8)
    digest.scalar(tls, 8)
    digest.boolean(mte_enabled)
    digest.scalar(mte_exclude, 2)
    digest.bytes(bytes(mte_tag_choices))
    digest.boolean(mte_random_tags)
    digest.scalar(mte_seed, 2)
    digest.scalar(mte_tag, 1)
    digest.scalar(mte_random_state, 8)
    digest.boolean(cpa_enabled)
    digest.boolean(cpa_multiply_enabled)
    digest.scalar(accdata, 4)
    for value in (
        pstate_ssbs,
        pstate_dit,
        pstate_tco,
        pstate_sm,
        pstate_za,
        pstate_daif_access,
    ):
        digest.boolean(value)
    digest.scalar(pstate_daif, 1)
    digest.scalar(exclusive_address, 8)
    digest.scalar(exclusive_key, 8)
    digest.scalar(exclusive_value, 8)
    digest.scalar(exclusive_value2, 8)
    digest.scalar(exclusive_version, 8)
    digest.scalar(exclusive_size, 1)
    digest.scalar(gcs_pointer, 8)
    digest.boolean(gcs_implemented)
    digest.boolean(gcs_push_enabled)
    digest.boolean(gcs_store_enabled)
    for low, high in pointer_auth_keys:
        digest.scalar(low, 8)
        digest.scalar(high, 8)
    digest.boolean(pointer_authentication_enabled)
    return f"{digest.value:016x}"


def memory_region_digest(
    memory: bytes,
    *,
    address: int,
    tags: Sequence[int] | None = None,
    readable: bool = True,
    writable: bool = True,
    executable: bool = False,
    guest_code: bool = False,
    guest_writable: bool = True,
) -> str:
    """Match one deterministic ``MemoryRegion`` evidence projection."""
    return memory_regions_digest(
        (
            {
                "memory": memory,
                "address": address,
                "tags": tags,
                "readable": readable,
                "writable": writable,
                "executable": executable,
                "guest_code": guest_code,
                "guest_writable": guest_writable,
            },
        )
    )


def memory_regions_digest(regions: Sequence[Mapping[str, object]]) -> str:
    """Match the ordered, complete C++ ``MemoryRegion`` projection."""
    digest = Digest()
    digest.scalar(len(regions), 8)
    for region in regions:
        memory = region["memory"]
        address = region["address"]
        tags = region.get("tags")
        if not isinstance(memory, bytes):
            raise ValueError("memory-region bytes are not a bytes value")
        if not isinstance(address, int):
            raise ValueError("memory-region address is not an integer")
        if not memory:
            raise ValueError("a memory-region projection cannot be empty")
        if address < 0 or address > MASK64:
            raise ValueError("memory-region address is outside bits(64)")
        expected_tags = (len(memory) + 15) // 16
        if tags is None:
            tags = (0,) * expected_tags
        if not isinstance(tags, Sequence) or isinstance(tags, (str, bytes)):
            raise ValueError("memory tags are not a sequence")
        if len(tags) != expected_tags or any(
            not isinstance(tag, int) or not 0 <= tag < 16 for tag in tags
        ):
            raise ValueError("memory tags do not cover every 16-byte granule")

        digest.scalar(address, 8)
        digest.scalar(len(memory), 8)
        for name in (
            "readable",
            "writable",
            "executable",
            "guest_code",
            "guest_writable",
        ):
            value = region.get(
                name,
                name in {"readable", "writable", "guest_writable"},
            )
            if not isinstance(value, bool):
                raise ValueError(f"memory permission {name} is not boolean")
            digest.boolean(value)
        digest.bytes(memory)
        for tag in tags:
            digest.scalar(tag, 1)
    return f"{digest.value:016x}"


def exact_outcome_digest(fields: Mapping[str, str]) -> str:
    """Digest a typed ASL terminal/event observation independently of ASL."""
    digest = Digest()
    for name, value in sorted(fields.items()):
        encoded_name = name.encode()
        encoded_value = value.encode()
        digest.scalar(len(encoded_name), 8)
        digest.bytes(encoded_name)
        digest.scalar(len(encoded_value), 8)
        digest.bytes(encoded_value)
    return f"{digest.value:016x}"


def add_specimen_digests(x0: int, x5: int, nzcv: int) -> dict[str, str]:
    initial = [0] * 31
    initial[5] = 0x25
    result = initial.copy()
    result[0] = x0
    result[5] = x5
    return {
        "initial": architecture_state_digest(
            initial, pc=GUEST_PC, nzcv=0xA
        ),
        "instruction": architecture_state_digest(
            result, pc=GUEST_PC + 4, nzcv=nzcv
        ),
        "terminal_suffix": architecture_state_digest(
            result, pc=GUEST_PC + 8, nzcv=nzcv
        ),
    }


def _empty_memory_digest() -> str:
    digest = Digest()
    digest.bytes(struct.pack("<Q", 0))
    return f"{digest.value:016x}"


if _empty_memory_digest() != EMPTY_MEMORY_DIGEST:
    raise RuntimeError("empty-memory representation digest drift")
