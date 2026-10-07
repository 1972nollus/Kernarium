#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the native non-streaming SVE cases of the A64 instruction corpus.

No producer exported SVE, so the cases that hold SVE outside streaming mode
come from this script and from the hardware. It derives every case from the
generated SVE/SME encoding table
(darwin/cpu/arm64/detail/sme_encodings.hpp), writes them as a request for
tools/arm64-tests/sve_native_oracle.cpp, which runs each one on an arm64
Linux host with FEAT_SVE at a 16-byte vector length, and turns the oracle's
response into arm64-sve-native records appended to a corpus dump after the
native SME step:

    sve-native-corpus.py --request sve.request
    sve-native-oracle.sh HOST sve.request sve.response  (two runs on HOST)
    sve-native-corpus.py native.jsonl sve.jsonl \
        --response sve.response.1 --response sve.response.2

The rows are those the SVE profile executes at PSTATE.SM = 0 without
FEAT_SME. The SCLAMP-class rows, which need FEAT_SME there too, cannot run
on an SVE host without SME and are left out; the withheld features are left
out as well, since the host may implement them but the guest's result is
UNDEFINED, which the absent-feature records expect. The cases cover:

- classify: each row's witness, and the evidence-binding witness of each SVE
  entry without a row whose base row is one of them, from a state whose SVE
  state is not live, so each also shows that it makes the state live;
- operands: each operand field at the lowest and highest register it
  allows, with the other operands distinct; every value of each free field
  of up to two bits (every element size among them) and both extremes of a
  wider one; every class of bitmask immediate that DecodeBitMasks
  reserves, for the logical-immediate rows; predicates all-true,
  all-false, partial, and alternating at each element size;
  general-register sources at both extremes; and FPCR's flush,
  default-NaN, and rounding controls for floating-point rows;
- memory: each contiguous, structure, replicate, fill, first-fault, and
  non-fault form aligned and misaligned, crossing a page with both pages
  mapped, crossing a 4 KiB boundary inside a mapping, with only the first
  page mapped (all elements active, only those below the boundary active,
  only those above it, and an element straddling it), with only the second
  mapped, inactive over unmapped memory, and stores to a read-only page;
- gather: each gather, scatter, first-fault gather, and gather prefetch
  with distinct, duplicate, and misaligned element addresses, each element
  faulting (active and inactive), an element straddling into an unmapped
  page, and a negative 32-bit offset under each extension;
- live: whether the SVE state is live after programs that execute SVE, run
  only general-register or AdvSIMD instructions, or trap, from either
  state, and that P and FFR survive non-SVE instructions.

Page profiles. Every region of a record for both profiles is 16 KiB-aligned
and a multiple of 16 KiB, so it is valid for the 16 KiB and 4 KiB guest
profiles alike and its page crossings are crossings in both; a 4 KiB
boundary inside a mapped region is crossed too, which only the 4 KiB tree
splits. The arm64-sve-native/memory/4k record, for the 4 KiB profile only,
maps 4 KiB regions so that a crossing faults on a 4 KiB boundary. The oracle
host's own pages are 4 KiB, so each region maps exactly; a fault reports the
lowest active byte on an unmapped page, which these mappings make the same
in the profile a record claims.

Every expectation is observed at VL 16, the one vector length outside
streaming mode (cpu/arm64/sve_profile.hpp), and a record's vl says so. A
case whose SVE state is live starts with fixed fills in P0-P15 and FFR; one
whose state is not live starts with them zero, as Linux leaves them.

The response must come from a host with the profile's features (SVE, SVE2,
SVE-AES, SVE-PMULL128, SVE-SHA3, SVE-I8MM, and SVE-BF16) at VL 16. The table
decides the legality of each row's witness, of its own registers, and of a
word no row decodes; a value of a free field that a row decodes may still be
UNDEFINED by its pseudocode (a reserved bitmask immediate), which the
hardware decides. A case whose outcome differs from the table's where the
table decides, or whose live bit disagrees with the frame's sve_context
size, is a finding: the script reports each one and writes nothing.

What the ASL leaves CONSTRAINED UNPREDICTABLE stays out of the
expectations: a MOVPRFX is followed by the instruction it prefixes, and a
completed first-fault or non-fault load names the
unpredictable-nonfault-load-elements/v1 relation, with its unpredictable
destination elements recorded as zero, one of the permitted values. The
responses of several runs (tools/arm64-tests/sve-native-oracle.sh makes
two) must give identical records; they may differ only in those elements.

Oracle request (little-endian):

    "UMLSVEQ\\0" u32 version=1, u32 vl, u32 count, count * case
    case   = u32 words, u32 entry, words * u32, state, u8 regions, region*
    state  = u32 X mask (bit i: Xi, bit 31: SP, each set one follows as u64),
             u32 nzcv, u32 fpcr, u32 fpsr, u32 V mask, 16 bytes per set V,
             [sve]                                    (when vl is not 0)
    sve    = u8 live, u16 P mask, vl/8 bytes per set P, u8 FFR flag,
             vl/8 bytes if set
    region = u64 address, u32 size, u8 flags (1 readable, 2 writable,
             0x80 explicit), then size bytes if explicit, else u32 seed

A request state is encoded against the corpus base state (the pack's base
fills, with P, FFR, and live zero). A region without explicit bytes holds
byte fill_hash(seed * 0x01000193 + offset) at each offset.

Oracle response:

    "UMLSVER\\0" u32 version=1, u32 host vl, u32 host page size, u64
    AT_HWCAP, u64 AT_HWCAP2, u32 count,
    count * (u32 event, u32 pc offset, u64 fault address, u32 ESR,
             u32 sve_context size, state,
             per request region: u32 runs, runs * (u32 offset, u32 length,
             bytes))

A response state is encoded against its case's initial state. Events are
numbered as in the pack, with 8 for a run the oracle could not attribute.
The sve_context size is the signal frame's at the case's last trap: 16 when
the SVE state is not live and SVE_SIG_CONTEXT_SIZE(1) when it is.

A regeneration of the earlier steps that leaves this script's request
unchanged needs no SVE host: --carry PACK copies the native records of the
current pack.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
import zlib
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", HERE / "build-unit-corpus.py")
sme_table = _load("sme_encoding_table", HERE / "sme_encoding_table.py")
# The operand and register-allocation helpers are the SME generator's.
sme_native = _load("sme_native_corpus", HERE / "sme-native-corpus.py")

SITE = "tools/arm64-tests/sve-native-corpus.py"
PRODUCER = "arm64-sve-native"
VL = corpus.SVE_VL
PBYTES = VL // 8
REQUEST_MAGIC = b"UMLSVEQ\0"
RESPONSE_MAGIC = b"UMLSVER\0"
REQUEST_VERSION = 1
RESPONSE_VERSION = 1
SVC = 0xD400_0001
SENTINEL = 0xD420_00E0  # BRK #7: the oracle runs each SVC as this
REGION_READABLE = 1
REGION_WRITABLE = 2
REGION_EXPLICIT = 0x80
EVENTS = {0: "system-call", 1: "memory-fault", 3: "illegal-instruction",
          4: "breakpoint", 5: "floating-point-trap", 6: "pc-alignment",
          8: "fatal"}
# AT_HWCAP's SVE bit, and the AT_HWCAP2 bits of the profile's features:
# SVE2, SVE-AES, SVE-PMULL128, SVE-SHA3, SVE-I8MM, and SVE-BF16.
HWCAP_SVE = 1 << 22
HWCAP2_SVE2 = 1 << 1
HWCAP2_PROFILE = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 9) | \
    (1 << 12)
# Linux's sve_context: its header alone, and with the registers at VL 16.
SVE_HEADER_SIZE = 16
SVE_LIVE_SIZE = 576
# Data regions: above every mapping Linux and XNU place themselves and below
# 2^48, and one below 4 GiB for the vector bases of 32-bit elements.
DATA = 0x100_0000_0000
LOW = 0x6000_0000
PAGE = 0x4000            # valid in both page profiles
SMALL_PAGE = 0x1000      # the 4 KiB profile's own records
WIDE = 0x10000
MIDDLE = DATA + WIDE // 2
BOUNDARY = DATA + PAGE
# Corpus region flags: readable, writable, and guest writable; readable.
READ_WRITE = 0x13
READ_ONLY = 0x01
# DN, FZ, round towards zero, FZ16.
FLUSH_FPCR = 0x03C8_0000
ALL_TRUE = b"\xff" * PBYTES
NONE = bytes(PBYTES)
# P0-P15 and FFR of a case whose SVE state is live, unless it names its own.
P_FILL = tuple(corpus.fill_p(index, VL) for index in range(16))
FFR_FILL = bytes(corpus.fill_hash(0x2200 | byte) for byte in range(PBYTES))
# Predicate patterns: alternating elements of 1, 2, 4, and 8 bytes.
PATTERNS = (("all-true", 0xFFFF), ("all-false", 0x0000),
            ("partial", 0x0007), ("alternating bytes", 0x5555),
            ("alternating halfwords", 0x3333),
            ("alternating words", 0x0F0F),
            ("alternating doublewords", 0x00FF))
GROUPS = ("classify", "operands", "memory", "gather", "live")
BOTH = ("16k", "4k")


def predicate(value: int) -> bytes:
    return value.to_bytes(PBYTES, "little")


# --------------------------------------------------------------------------
# Wire format


@dataclass
class WireState:
    """A sparse state: the entries a case sets over its reference."""
    x: dict[int, int] = field(default_factory=dict)
    nzcv: int = 0
    fpcr: int = 0
    fpsr: int = 0
    v: dict[int, bytes] = field(default_factory=dict)
    live: int = 0
    p: dict[int, bytes] = field(default_factory=dict)
    ffr: bytes | None = None


WireRegion = sme_native.WireRegion


@dataclass
class WireCase:
    words: tuple[int, ...]
    entry: int
    state: WireState
    regions: tuple[WireRegion, ...] = ()


@dataclass
class WireResult:
    event: str
    pc_offset: int
    fault_address: int
    esr: int
    sve_size: int
    state: WireState
    runs: tuple[tuple[tuple[int, bytes], ...], ...]


def _put_state(out: bytearray, state: WireState, vl: int) -> None:
    out += struct.pack("<I", sum(1 << i for i in state.x))
    for i in sorted(state.x):
        out += struct.pack("<Q", state.x[i])
    out += struct.pack("<III", state.nzcv, state.fpcr, state.fpsr)
    out += struct.pack("<I", sum(1 << i for i in state.v))
    for i in sorted(state.v):
        out += state.v[i]
    if not vl:
        return
    out += struct.pack("<BH", state.live, sum(1 << i for i in state.p))
    for i in sorted(state.p):
        out += state.p[i]
    out += b"\0" if state.ffr is None else b"\1" + state.ffr


def write_request(cases: list[WireCase], vl: int) -> bytes:
    out = bytearray(REQUEST_MAGIC)
    out += struct.pack("<III", REQUEST_VERSION, vl, len(cases))
    for case in cases:
        out += struct.pack("<II", len(case.words), case.entry)
        out += struct.pack(f"<{len(case.words)}I", *case.words)
        _put_state(out, case.state, vl)
        out += struct.pack("<B", len(case.regions))
        for region in case.regions:
            explicit = region.data is not None
            out += struct.pack("<QIB", region.address, region.size,
                               region.flags | (REGION_EXPLICIT if explicit
                                               else 0))
            out += region.data if explicit else struct.pack("<I", region.seed)
    return bytes(out)


def _get_state(reader, vl: int) -> WireState:
    state = WireState()
    mask = reader.unpack("I")
    for i in range(32):
        if mask >> i & 1:
            state.x[i] = reader.unpack("Q")
    state.nzcv, state.fpcr, state.fpsr = reader.unpack("III")
    mask = reader.unpack("I")
    state.v = {i: reader.take(16) for i in range(32) if mask >> i & 1}
    if not vl:
        return state
    state.live, mask = reader.unpack("BH")
    state.p = {i: reader.take(vl // 8) for i in range(16) if mask >> i & 1}
    if reader.unpack("B"):
        state.ffr = reader.take(vl // 8)
    return state


def read_response(data: bytes, cases: list[WireCase], vl: int
                  ) -> tuple[dict, list[WireResult]]:
    reader = sme_native._Reader(data)
    if reader.take(8) != RESPONSE_MAGIC:
        raise ValueError("not an oracle response")
    version, host_vl, page, hwcap, hwcap2, count = reader.unpack("IIIQQI")
    if version != RESPONSE_VERSION or count != len(cases):
        raise ValueError("the oracle response does not answer this request")
    host = {"vl": host_vl, "page": page, "hwcap": hwcap, "hwcap2": hwcap2}
    results = []
    for case in cases:
        event, pc_offset, fault_address, esr, sve_size = \
            reader.unpack("IIQII")
        state = _get_state(reader, vl)
        runs = []
        for _region in case.regions:
            region_runs = []
            for _ in range(reader.unpack("I")):
                offset, length = reader.unpack("II")
                region_runs.append((offset, reader.take(length)))
            runs.append(tuple(region_runs))
        results.append(WireResult(EVENTS.get(event, f"event-{event}"),
                                  pc_offset, fault_address, esr, sve_size,
                                  state, tuple(runs)))
    if reader.offset != len(data):
        raise ValueError("trailing bytes in the oracle response")
    return host, results


# --------------------------------------------------------------------------
# Dense states


@dataclass
class Dense:
    x: list[int]
    nzcv: int
    fpcr: int
    fpsr: int
    v: list[bytes]
    live: int
    p: list[bytes]
    ffr: bytes

    def apply(self, state: WireState) -> Dense:
        x = list(self.x)
        for i, value in state.x.items():
            x[i] = value
        v, p = list(self.v), list(self.p)
        for target, items in ((v, state.v), (p, state.p)):
            for i, value in items.items():
                target[i] = value
        return Dense(x=x, nzcv=state.nzcv, fpcr=state.fpcr, fpsr=state.fpsr,
                     v=v, live=state.live, p=p,
                     ffr=state.ffr if state.ffr is not None else self.ffr)


BASE = Dense(x=list(corpus.FILL_REGISTERS), nzcv=0, fpcr=0, fpsr=0,
             v=list(corpus.FILL_VECTORS), live=0, p=[NONE] * 16, ffr=NONE)


def hex64(value: int) -> str:
    return corpus.hex64(value)


def state_fields(reference: Dense, state: Dense) -> dict:
    """A state against a reference, as the pack's dump writes it."""
    out: dict[str, str] = {}
    for i in range(32):
        if state.x[i] != reference.x[i]:
            out["sp" if i == 31 else f"x{i}"] = hex64(state.x[i])
    for name in ("nzcv", "fpcr", "fpsr"):
        if getattr(state, name) != getattr(reference, name):
            out[name] = hex64(getattr(state, name))
    for i in range(32):
        if state.v[i] != reference.v[i]:
            out[f"v{i}"] = state.v[i].hex()
    for i in range(16):
        if state.p[i] != reference.p[i]:
            out[f"sve_p{i}"] = state.p[i].hex()
    if state.ffr != reference.ffr:
        out["sve_ffr"] = state.ffr.hex()
    if state.live != reference.live:
        out["sve_live"] = hex64(state.live)
    return out


# --------------------------------------------------------------------------
# Drafts


@dataclass
class Draft:
    group: str
    name: str
    words: tuple[int, ...]
    live: int = 1
    x: dict[int, int] = field(default_factory=dict)
    nzcv: int = 0
    fpcr: int = 0
    fpsr: int = 0
    v: dict[int, bytes] = field(default_factory=dict)
    p: dict[int, bytes] = field(default_factory=dict)
    ffr: bytes | None = None
    regions: tuple[WireRegion, ...] = ()
    native: tuple[str, ...] = ("sve",)
    store: bool = False
    # "legal" or "illegal" where the table decides; "" where only the
    # hardware does.
    expect: str = ""
    invalid: bool = False
    profiles: tuple[str, ...] = BOTH

    def wire(self) -> WireCase:
        p, ffr = dict(self.p), self.ffr
        if self.live:
            p = {i: p.get(i, P_FILL[i]) for i in range(16)}
            ffr = FFR_FILL if ffr is None else ffr
        elif any(map(any, p.values())) or (ffr is not None and any(ffr)):
            raise SystemExit(f"{self.name}: P or FFR while not live")
        state = WireState(x=dict(self.x), nzcv=self.nzcv, fpcr=self.fpcr,
                          fpsr=self.fpsr, v=dict(self.v), live=self.live,
                          p=p, ffr=ffr)
        return WireCase(words=self.words + (SVC,), entry=0, state=state,
                        regions=self.regions)


encode = sme_native.encode
numbers = sme_native.numbers
occupied = sme_native.occupied
allocate = sme_native.allocate
operand_value = sme_native.operand_value
free_runs = sme_native.free_runs
with_run = sme_native.with_run
zero_offset = sme_native.zero_offset
address_registers = sme_native.address_registers
memory_registers = sme_native.memory_registers
RowIndex = sme_native.RowIndex


@functools.cache
def table():
    return sme_table.load()


@functools.cache
def row_index() -> RowIndex:
    return RowIndex(table())


def executes(row) -> bool:
    """Whether the SVE profile executes the row outside streaming mode on a
    CPU without SME: the rows this host can observe."""
    if row is None:
        return False
    outside = row.non_streaming_requirement
    return "sve" in outside and "sme" not in outside


def sve_rows() -> list:
    return [row for row in table().rows if executes(row)]


def row_name(row, word: int) -> str:
    return f"{row.stable_id} {row.encoding} {word:08x}"


def native_of(row) -> tuple[str, ...]:
    return tuple(row.non_streaming_requirement)


def gather_shape(row) -> bool:
    return row.memory is not None and row.memory.shape in (
        "gather", "gather_first_fault", "gather_prefetch")


def element_count(row, word: int) -> int:
    return VL * 8 // row.memory.element_bits(word)


def expectation(row, word: int) -> str | None:
    """What the table says of a variant of a row's word: "legal" where the
    row decodes it, "illegal" where no row does, and None where another row
    does (that row's own cases cover it)."""
    found = row_index().row_for(word)
    if found is None:
        return "illegal"
    return "legal" if found is row else None


def prefixed(row, word: int) -> tuple[int, ...]:
    """The word, and after a MOVPRFX the instruction it prefixes, without
    which the architecture leaves it CONSTRAINED UNPREDICTABLE: ADD Zd.B,
    Zd.B, #1 after the unpredicated form, and ADD of the next register into
    Zd under its predicate and element size after the predicated one."""
    destination = word & 31
    if row.encoding == "movprfx_z_z_":
        return word, 0x2520_C020 | destination
    if row.encoding == "movprfx_z_p_z_":
        return word, 0x0400_0000 | (word & 0x00C0_1C00) | \
            ((destination + 1) % 32) << 5 | destination
    return (word,)


def base_draft(group: str, row, word: int, label: str, live: int = 1,
               expect: str = "legal") -> Draft:
    draft = Draft(group=group, name=f"{row_name(row, word)} {label}",
                  words=prefixed(row, word), live=live, native=native_of(row),
                  expect=expect,
                  store=row.memory is not None and
                  row.memory.direction == "store")
    if row.memory is None:
        return draft
    if gather_shape(row):
        draft.regions, registers = gather_layout(row, word, spread(row, word))
        draft.x.update(registers[0])
        draft.v.update(registers[1])
        return draft
    if row.memory.shape != "prefetch":
        draft.regions = (WireRegion(DATA, WIDE, REGION_READABLE |
                                    REGION_WRITABLE, seed=1),)
    draft.x.update(address_registers(row, word, MIDDLE))
    return draft


# --------------------------------------------------------------------------
# Gathers


def spread(row, word: int, base: int | None = None) -> list[int]:
    """Distinct element addresses two elements apart, in the row's
    region."""
    mbytes = row.memory.msize // 8
    start = (LOW + WIDE // 2 if low_region(row, word) else MIDDLE) \
        if base is None else base
    return [start + 2 * mbytes * i for i in range(element_count(row, word))]


def low_region(row, word: int) -> bool:
    """A vector base of 32-bit elements addresses only the low 4 GiB."""
    memory = row.memory
    return memory.offset in ("immediate_element", "register_byte") and \
        memory.element_bits(word) == 32


def element_bytes_of(values: list[int], esize: int) -> bytes:
    data = b"".join((value & ((1 << esize) - 1)).to_bytes(esize // 8,
                                                          "little")
                    for value in values)
    return data.ljust(16, b"\0")


def gather_layout(row, word: int, addresses: list[int],
                  regions: tuple[WireRegion, ...] | None = None,
                  xs: int | None = None
                  ) -> tuple[tuple[WireRegion, ...], tuple[dict, dict]]:
    """Regions and the X and V values that make the row's elements access
    `addresses`. A vector-index form takes a scalar base and computes the
    offsets; a vector-base form takes the addresses in its Z register."""
    memory = row.memory
    esize = memory.element_bits(word)
    low = low_region(row, word)
    if regions is None:
        regions = (WireRegion(LOW if low else DATA, WIDE, REGION_READABLE |
                              REGION_WRITABLE, seed=1),)
    x: dict[int, int] = {}
    v: dict[int, bytes] = {}
    base = operand_value(row, word, memory.base_operand)
    if memory.offset == "vector_index":
        scalar = (LOW if low else DATA) + WIDE // 2
        x[base] = scalar
        shift = memory.index_shift
        offsets = []
        for address in addresses:
            offset = (address - scalar) >> shift
            if (offset << shift) + scalar != address:
                raise SystemExit(f"{row_name(row, word)}: unscaled address")
            offsets.append(offset)
        index = operand_value(row, word, memory.index_operand)
        v[index] = element_bytes_of(offsets, esize)
    elif memory.offset == "immediate_element":
        v[base] = element_bytes_of(addresses, esize)
    elif memory.offset == "register_byte":
        index = operand_value(row, word, memory.index_operand)
        delta = 0x10 if index != 31 else 0
        if index != 31:
            x[index] = delta
        v[base] = element_bytes_of([address - delta for address in
                                    addresses], esize)
    else:
        raise SystemExit(f"{row_name(row, word)}: gather offset "
                         f"{memory.offset}")
    return regions, (x, v)


# --------------------------------------------------------------------------
# Case generation


def predicate_operands(row, word: int) -> list[int]:
    """The predicate registers the row reads or writes, but for the P
    register that LDR and STR (predicate) transfer."""
    if row.memory is not None and row.memory.shape == "fill_p":
        return []
    numbers_ = []
    for operand in row.operands:
        if operand.kind == "p":
            for _kind, number in occupied(operand, operand.number(word)):
                numbers_.append(number)
    return numbers_


def with_predicates(draft: Draft, row, word: int, value: bytes) -> Draft:
    for number in predicate_operands(row, word):
        draft.p[number] = value
    return draft


@functools.cache
def alias_witnesses(accept=executes) -> tuple[tuple[str, str, int], ...]:
    """The evidence-binding witnesses of the SVE entries without a row of
    their own whose base row `accept` takes: by default those this host
    can observe."""
    rows = {row.stable_id for row in table().rows}
    out = []
    with corpus.BINDINGS.open(encoding="utf-8", newline="") as source:
        for binding in csv.DictReader(source, delimiter="\t"):
            if binding["section"] != "SVE" or binding["stable_id"] in rows:
                continue
            word = int(binding["encoding"], 16)
            if accept(row_index().row_for(word)):
                out.append((binding["stable_id"],
                            binding["architectural_form"], word))
    return tuple(out)


def classify_drafts(rows=None, accept=executes) -> list[Draft]:
    """Each row's witness, and each alias witness `accept` takes, from a
    state that is not live; by default the rows this host observes."""
    drafts = []
    for row in sve_rows() if rows is None else rows:
        drafts.append(base_draft("classify", row, row.witness,
                                 "witness, not live", live=0))
    for stable_id, form, word in alias_witnesses(accept):
        row = row_index().row_for(word)
        draft = base_draft("classify", row, word, "witness, not live",
                           live=0)
        draft.name = f"{stable_id} {form} {word:08x} witness, not live"
        drafts.append(draft)
    return drafts


# DecodeBitMasks makes these immediates UNDEFINED (reserved): N = 1 with
# imms all ones, N = 0 with imms 11111x, and for each smaller element size
# the pattern with every element bit set.
RESERVED_BITMASKS = (0x103F, 0x003E, 0x003F, 0x003D, 0x003B, 0x0037, 0x002F,
                     0x001F)
BITMASK_ROWS = ("and_z_zi_", "eor_z_zi_", "orr_z_zi_", "dupm_z_i_")


def operand_drafts(rows=None) -> list[Draft]:
    """The operand cases of each row; by default the rows this host
    observes."""
    drafts = []
    # A word no row decodes can come from several rows of one encoding.
    illegal: set[int] = set()
    for row in sve_rows() if rows is None else rows:
        count = len(row.operands)
        seen = {row.witness}
        low_word = allocate(row, row.witness, list(range(count)), False)

        def add(word: int, label: str, decides: bool = True,
                **changes) -> Draft | None:
            """A case of the word. The table decides that a word no row
            decodes is UNDEFINED, and that the row's own registers are
            legal; a value of a free field it decodes may still be
            UNDEFINED by the row's pseudocode (a reserved bitmask
            immediate), which the hardware decides."""
            expect = expectation(row, word)
            if expect is None or (word in seen and not changes) or \
                    (expect == "illegal" and word in illegal):
                return None
            if expect == "legal" and not decides:
                expect = ""
            seen.add(word)
            if expect == "illegal":
                illegal.add(word)
            draft = base_draft("operands", row, word, label, expect=expect)
            if expect == "illegal":
                draft.invalid = True
                draft.regions = ()
            for key, value in changes.items():
                if key in ("x", "p", "v"):
                    getattr(draft, key).update(value)
                else:
                    setattr(draft, key, value)
            drafts.append(draft)
            return draft

        for rotation in range(max(count, 1)):
            order = list(range(rotation, count)) + list(range(rotation))
            for high in (False, True):
                word = allocate(row, row.witness, order, high)
                add(word, f"registers {'high' if high else 'low'} from "
                          f"operand {rotation}")
        for run in free_runs(row):
            low, width = run
            values = range(1 << width) if width <= 2 else \
                (0, (1 << width) - 1)
            for value in values:
                add(with_run(low_word, run, value),
                    f"bits {low + width - 1}:{low} = {value:#x}",
                    decides=False)
        if row.encoding in BITMASK_ROWS:
            for value in RESERVED_BITMASKS:
                add(with_run(low_word, (5, 13), value),
                    f"reserved bitmask immediate {value:#06x}",
                    decides=False)
        if predicate_operands(row, low_word):
            for label, value in PATTERNS:
                add(low_word, f"predicates {label}",
                    p={number: predicate(value) for number in
                       predicate_operands(row, low_word)})
        sources = [index for index, operand in enumerate(row.operands)
                   if operand.kind == "gpr" and
                   index not in memory_registers(row)]
        if sources:
            for value in (0, 0xFFFF_FFFF_FFFF_FFFF):
                registers = {operand_value(row, low_word, index): value
                             for index in sources}
                registers.pop(31, None)
                if registers:
                    add(low_word, f"general sources = {value:#x}",
                        x=registers)
        if row.encoding.startswith(("f", "bf")):
            add(low_word, "FPCR DN FZ FZ16 RZ", fpcr=FLUSH_FPCR)
    return drafts


def footprint(row, word: int) -> int:
    memory = row.memory
    shape = memory.shape
    if shape == "fill_z":
        return VL
    if shape == "fill_p":
        return PBYTES
    if shape == "replicate":
        return memory.msize // 8
    if shape == "replicate_quad":
        return 16
    elements = VL * 8 // memory.element_bits(word)
    return memory.msize // 8 * elements * memory.registers


def half_predicate(row, word: int, upper: bool) -> bytes:
    """The elements below (or from) half the vector active."""
    elements = element_count(row, word)
    esize = VL // elements
    bits = 0
    for element in range(elements):
        if (element >= elements // 2) == upper:
            bits |= 1 << (element * esize)
    return predicate(bits)


def memory_drafts() -> list[Draft]:
    drafts = []
    readwrite = REGION_READABLE | REGION_WRITABLE
    wide = WireRegion(DATA, WIDE, readwrite, seed=1)
    first = WireRegion(DATA, PAGE, readwrite, seed=2)
    second = WireRegion(BOUNDARY, PAGE, readwrite, seed=3)
    read_only = WireRegion(DATA, WIDE, REGION_READABLE, seed=1)
    small = DATA + SMALL_PAGE
    small_first = WireRegion(DATA, SMALL_PAGE, readwrite, seed=4)
    small_second = WireRegion(small, SMALL_PAGE, readwrite, seed=5)
    small_read_only = WireRegion(small, SMALL_PAGE, REGION_READABLE, seed=5)
    shapes = ("contiguous", "structure", "replicate", "replicate_quad",
              "fill_z", "fill_p", "first_fault", "non_fault")
    for row in sve_rows():
        if row.memory is None or row.memory.shape not in shapes:
            continue
        word = zero_offset(row, allocate(row, row.witness,
                                         list(range(len(row.operands))),
                                         False))
        if expectation(row, word) != "legal":
            continue
        size = footprint(row, word)
        predicated = bool(predicate_operands(row, word))
        mbytes = row.memory.msize // 8
        first_fault = row.memory.shape in ("first_fault", "non_fault")

        def add(label: str, base: int, regions: tuple[WireRegion, ...],
                pattern: bytes | None = ALL_TRUE,
                profiles: tuple[str, ...] = BOTH,
                ffr: bytes | None = None) -> None:
            draft = base_draft("memory", row, word, label)
            draft.regions = regions
            draft.profiles = profiles
            draft.x.update(address_registers(row, word, base, index=0))
            if pattern is not None:
                with_predicates(draft, row, word, pattern)
            if first_fault:
                draft.ffr = ALL_TRUE if ffr is None else ffr
            drafts.append(draft)

        crossing = BOUNDARY - size // 2
        add("aligned", MIDDLE, (wide,))
        add("misaligned by 3", MIDDLE + 3, (wide,))
        add("crossing a page", crossing, (first, second))
        add("crossing a page misaligned by 1", crossing + 1, (first, second))
        add("crossing a 4 KiB boundary inside a mapping",
            DATA + SMALL_PAGE - size // 2, (wide,))
        add("crossing into an unmapped page", crossing, (first,))
        add("crossing from an unmapped page", crossing, (second,))
        if mbytes > 1 and row.memory.shape in ("contiguous", "first_fault",
                                               "non_fault"):
            add("an element straddling into an unmapped page",
                BOUNDARY - mbytes // 2 - mbytes * (size // mbytes // 2),
                (first,))
        if predicated and row.memory.shape in ("contiguous", "structure",
                                               "first_fault", "non_fault"):
            add("active only below an unmapped page", crossing, (first,),
                half_predicate(row, word, False))
            add("active only on an unmapped page", crossing, (first,),
                half_predicate(row, word, True))
        if predicated:
            add("inactive over unmapped memory", crossing, (), NONE)
        if first_fault:
            add("crossing into an unmapped page, FFR partly clear",
                crossing, (first,), ffr=FFR_FILL)
        if row.memory.direction == "store":
            add("into a read-only page", MIDDLE, (read_only,))
        # A 4 KiB boundary faults only in the 4 KiB profile.
        crossing = small - size // 2
        add("crossing into an unmapped 4 KiB page", crossing,
            (small_first,), profiles=("4k",))
        add("crossing from an unmapped 4 KiB page", crossing,
            (small_second,), profiles=("4k",))
        if row.memory.direction == "store":
            add("crossing into a read-only 4 KiB page", crossing,
                (small_first, small_read_only), profiles=("4k",))
    return drafts


def gather_drafts() -> list[Draft]:
    drafts = []
    for row in sve_rows():
        if not gather_shape(row):
            continue
        word = zero_offset(row, allocate(row, row.witness,
                                         list(range(len(row.operands))),
                                         False))
        memory = row.memory
        variants = [(word, "")]
        if memory.index_sign is not None:
            sign = memory.index_sign
            variants = [(with_run(word, (sign.low, sign.width), value),
                         " SXTW" if value else " UXTW") for value in (0, 1)]
        for word, extension in variants:
            if expectation(row, word) != "legal":
                continue
            mbytes = memory.msize // 8
            elements = element_count(row, word)
            low = low_region(row, word)
            start = (LOW if low else DATA)
            region = WireRegion(start, WIDE, REGION_READABLE |
                                REGION_WRITABLE, seed=1)
            unmapped = start + WIDE + 8 * mbytes
            first_fault = memory.shape == "gather_first_fault"

            def add(label: str, addresses: list[int],
                    pattern: bytes = ALL_TRUE,
                    regions: tuple[WireRegion, ...] = (region,)) -> None:
                draft = base_draft("gather", row, word, label + extension)
                draft.regions, (x, v) = gather_layout(row, word, addresses,
                                                      regions)
                draft.x = x
                draft.v = v
                with_predicates(draft, row, word, pattern)
                if first_fault:
                    draft.ffr = ALL_TRUE
                drafts.append(draft)

            middle = start + WIDE // 2
            distinct = spread(row, word, middle)
            add("distinct elements", distinct)
            add("duplicate addresses", [middle] * elements)
            add("misaligned elements",
                [middle + 3 + 5 * mbytes * i for i in range(elements)]
                if memory.index_shift == 0 else
                [middle + (5 * i + 1) * mbytes for i in range(elements)])
            for element in range(elements):
                addresses = list(distinct)
                addresses[element] = unmapped
                add(f"element {element} unmapped", addresses)
                add(f"element {element} unmapped and inactive", addresses,
                    predicate(sum(1 << (k * (VL // elements))
                                  for k in range(elements) if k != element)))
            if mbytes > 1 and memory.index_shift == 0:
                addresses = list(distinct)
                addresses[-1] = start + WIDE - mbytes // 2
                add("last element straddling into an unmapped page",
                    addresses)
            if memory.direction == "store":
                add("into a read-only page", distinct, ALL_TRUE,
                    (WireRegion(start, WIDE, REGION_READABLE, seed=1),))
            if memory.offset == "vector_index" and memory.index_bits == 32:
                # The same 32-bit offset, -64 elements' worth: inside the
                # region under SXTW, 4 GiB above it under UXTW.
                draft = base_draft("gather", row, word,
                                   "offsets -2 as 32 bits" + extension)
                draft.regions, (x, v) = gather_layout(
                    row, word, [middle] * elements, (region,))
                index = operand_value(row, word, memory.index_operand)
                v[index] = element_bytes_of(
                    [0xFFFF_FFFE] * elements, memory.element_bits(word)) \
                    if memory.element_bits(word) == 32 else \
                    element_bytes_of([0xFFFF_FFFE] * elements, 64)
                draft.x, draft.v = x, v
                with_predicates(draft, row, word, ALL_TRUE)
                if first_fault:
                    draft.ffr = ALL_TRUE
                drafts.append(draft)
    return drafts


@functools.cache
def unallocated_word() -> int:
    """The lowest word of the SVE encoding space that no table row
    decodes."""
    index = row_index()
    for word in range(0x0400_0000, 0x0600_0000, 0x400):
        if index.row_for(word) is None:
            return word
    raise SystemExit("no unallocated SVE word")


def live_drafts() -> list[Draft]:
    """Whether the SVE state is live after a program, from each state."""
    ptrue = 0x2518_E3E0             # PTRUE P0.B
    add_x = 0x8B02_0020             # ADD X0, X1, X2
    add_v = 0x4EA2_8420             # ADD V0.4S, V1.4S, V2.4S
    ld1b = 0xA400_A000              # LD1B {Z0.B}, P0/Z, [X0]
    ldr_q = 0x3DC0_0000             # LDR Q0, [X0]
    brk = 0xD420_0020               # BRK #1
    undefined = unallocated_word()
    unmapped = {0: DATA}
    cases = (
        ("PTRUE P0.B", (ptrue,), 0, {}, None),
        ("PTRUE P0.B", (ptrue,), 1, {}, None),
        ("ADD X0, X1, X2", (add_x,), 0, {}, None),
        ("ADD X0, X1, X2", (add_x,), 1, {}, None),
        ("ADD V0.4S, V1.4S, V2.4S", (add_v,), 0, {}, None),
        ("ADD V0.4S, V1.4S, V2.4S", (add_v,), 1, {}, None),
        ("BRK #1", (brk,), 0, {}, None),
        ("BRK #1", (brk,), 1, {}, None),
        (f"an unallocated SVE word {undefined:08x}", (undefined,), 0, {},
         None),
        (f"an unallocated SVE word {undefined:08x}", (undefined,), 1, {},
         None),
        ("LD1B {Z0.B}, P0/Z, [X0] over unmapped memory", (ld1b,), 0,
         unmapped, None),
        ("LD1B {Z0.B}, P0/Z, [X0] faulting", (ld1b,), 1, unmapped,
         ALL_TRUE),
        ("LDR Q0, [X0] faulting", (ldr_q,), 0, unmapped, None),
        ("LDR Q0, [X0] faulting", (ldr_q,), 1, unmapped, None),
        ("PTRUE P0.B; ADD V0.4S, V1.4S, V2.4S", (ptrue, add_v), 0, {},
         None),
        ("ADD V0.4S, V1.4S, V2.4S; PTRUE P0.B", (add_v, ptrue), 0, {},
         None),
        ("PTRUE P0.B; LDR Q0, [X0] faulting", (ptrue, ldr_q), 0, unmapped,
         None),
        ("PTRUE P0.B; BRK #1", (ptrue, brk), 0, {}, None),
    )
    drafts = []
    for name, words, live, x, p0 in cases:
        # A state that is not live holds a zero P0, so its LD1B loads
        # nothing and completes.
        draft = Draft(group="live",
                      name=f"{name} from a state that is "
                           f"{'live' if live else 'not live'}",
                      words=words, live=live, x=dict(x),
                      p={0: p0} if p0 is not None and live else {})
        if undefined in words:
            draft.expect, draft.invalid = "illegal", True
        drafts.append(draft)
    return drafts


@functools.cache
def drafts() -> tuple[Draft, ...]:
    return tuple(classify_drafts() + operand_drafts() + memory_drafts() +
                 gather_drafts() + live_drafts())


def request_bytes() -> bytes:
    return write_request([draft.wire() for draft in drafts()], VL)


# --------------------------------------------------------------------------
# Records


def corpus_region(region: WireRegion, runs) -> dict:
    return sme_native.corpus_region(region, runs)


class Finding(Exception):
    pass


# The validator relation of a completed first-fault or non-fault load
# (tests/unit/arm64/corpus/README.md).
NONFAULT = "unpredictable-nonfault-load-elements/v1"
NONFAULT_SHAPES = ("first_fault", "non_fault", "gather_first_fault")


def unpredictable_from(case: WireCase, ffr: bytes) -> tuple[int, int] | None:
    """For a first-fault or non-fault load at the case's entry, its
    destination and the first byte of it that the ASL leaves CONSTRAINED
    UNPREDICTABLE: the elements from the first whose FFR element is FALSE
    hold the loaded data, zero, or their old value. None for another
    entry."""
    word = case.words[case.entry // 4]
    row = row_index().row_for(word)
    if row is None or row.memory is None or \
            row.memory.shape not in NONFAULT_SHAPES:
        return None
    step = row.memory.element_bits(word) // 8
    first = next((byte for byte in range(0, VL, step)
                  if not ffr[byte // 8] >> byte % 8 & 1), VL)
    return word & 31, first


def case_skeleton(draft: Draft, case: WireCase, program: int) -> dict:
    """The record case without what the hardware decides: its event,
    expected state, retired count, fault, and expected bytes."""
    initial = BASE.apply(case.state)
    item = {
        "name": draft.name, "program": program, "ordinals": [0, 0],
        "invalid_encoding": draft.invalid, "minimization": "full",
        "native": list(draft.native),
        "initial": state_fields(BASE, initial),
    }
    if case.regions:
        item["memory"] = [corpus_region(region, ()) for region in
                          case.regions]
    return item


def record_case(draft: Draft, case: WireCase, result: WireResult,
                program: int) -> dict:
    if result.event == "fatal" or result.event.startswith("event-"):
        raise Finding(f"{draft.name}: the oracle could not attribute the run "
                      f"(pc offset {result.pc_offset:#x})")
    legal = result.event != "illegal-instruction"
    if draft.expect == "legal" and not legal or \
            draft.expect == "illegal" and legal:
        raise Finding(f"{draft.name}: {result.event} (ESR "
                      f"{result.esr:#010x}), the table expects "
                      f"{draft.expect}")
    if result.sve_size not in (SVE_HEADER_SIZE, SVE_LIVE_SIZE) or \
            result.state.live != (result.sve_size == SVE_LIVE_SIZE):
        raise Finding(f"{draft.name}: live {result.state.live} with an "
                      f"sve_context of {result.sve_size} bytes")
    words = len(case.words)
    if result.pc_offset % 4 or result.pc_offset // 4 >= words:
        raise Finding(f"{draft.name}: stopped outside its program")
    completed = result.event == "system-call"
    if completed and case.words[result.pc_offset // 4] != SVC:
        raise Finding(f"{draft.name}: completed at a word that is not SVC")
    initial = BASE.apply(case.state)
    final = initial.apply(result.state)
    if not final.live and (any(map(any, final.p)) or any(final.ffr)):
        raise Finding(f"{draft.name}: P or FFR while not live")
    # The relation masks the unpredictable elements; the record holds zero,
    # one of the permitted values, so that it does not depend on the run.
    unpredictable = unpredictable_from(case, final.ffr) if completed else None
    if unpredictable is not None:
        destination, first = unpredictable
        v = list(final.v)
        v[destination] = v[destination][:first] + bytes(VL - first)
        final = dataclasses.replace(final, v=v)
    item = case_skeleton(draft, case, program)
    if unpredictable is not None:
        item["validator"] = NONFAULT
    if not draft.expect:
        item["invalid_encoding"] = not legal
    item["event"] = result.event
    # The oracle stops at the SVC's sentinel; a completed system call
    # leaves the PC after the SVC, as Linux reports it.
    pc_offset = result.pc_offset + (4 if completed else 0)
    expected = state_fields(initial, final)
    if pc_offset != case.entry:
        expected["pc"] = hex64(corpus.CODE_BASE + pc_offset)
    item["expected"] = expected
    item["retired"] = sme_native.retired_count(draft.name, result.event,
                                               result.pc_offset)
    if result.event == "memory-fault":
        if result.esr >> 26 == 0x26:
            access = "write" if draft.store else "read"
        else:
            access = "write" if result.esr >> 6 & 1 else "read"
        item["fault"] = {"address": hex64(result.fault_address),
                         "access": access}
    if case.regions:
        item["memory"] = [corpus_region(region, runs)
                          for region, runs in zip(case.regions, result.runs)]
    return item


def record_key(draft: Draft) -> tuple[str, tuple[str, ...]]:
    return draft.group, draft.profiles


def assemble(items: list[tuple[Draft, tuple[int, ...], dict]]) -> list[dict]:
    """The records holding the cases, each given with its draft and its
    program's words, in group order and with each group's 4 KiB record
    after it."""
    records: dict[tuple, dict] = {}
    programs: dict[tuple, dict[tuple, int]] = {}
    for draft, words, item in items:
        key = record_key(draft)
        group, profiles = key
        name = f"{PRODUCER}/{group}" + ("" if profiles == BOTH else
                                        f"/{profiles[0]}")
        record = records.setdefault(key, {
            "record": name, "producer": PRODUCER, "site": SITE,
            "profiles": list(profiles), "options": [], "vl": VL,
            "programs": [], "cases": []})
        index = programs.setdefault(key, {})
        if words not in index:
            index[words] = len(record["programs"])
            record["programs"].append({
                "entry": 0,
                "words": [[4 * i, f"{word:08x}"]
                          for i, word in enumerate(words)]})
        record["cases"].append(dict(item, program=index[words]))
    return [records[key] for group in GROUPS
            for key in ((group, BOTH), (group, ("4k",)))
            if key in records]


def build_records(host: dict, results: list[WireResult]
                  ) -> tuple[list[dict], dict[str, bytes], dict]:
    items = drafts()
    cases = [draft.wire() for draft in items]
    if host["vl"] != VL or not host["hwcap"] & HWCAP_SVE or \
            host["hwcap2"] & HWCAP2_PROFILE != HWCAP2_PROFILE:
        raise SystemExit("the response is not from a host with the SVE "
                         f"profile's features at a {VL}-byte VL")
    blobs: dict[str, bytes] = {}
    findings = []
    counts = {"cases": len(items), "events": {}}
    recorded = []
    for draft, case, result in zip(items, cases, results):
        try:
            item = record_case(draft, case, result, 0)
        except Finding as finding:
            findings.append(str(finding))
            continue
        recorded.append((draft, case.words, item))
        counts["events"][item["event"]] = \
            counts["events"].get(item["event"], 0) + 1
        for region in case.regions:
            data = region.initial()
            blobs[hashlib.sha256(data).hexdigest()] = data
    if findings:
        for finding in findings:
            print(f"sve-native-corpus: {finding}", file=sys.stderr)
        raise SystemExit(f"sve-native-corpus: {len(findings)} cases disagree "
                         "with the encoding table or the frame")
    return assemble(recorded), blobs, counts


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def native_meta(request: bytes, host: dict, counts: dict) -> dict:
    return {
        "script": SITE, "sha256": script_hash(),
        "table": {"file": sme_table.TABLE_PATH, "sha256": table().sha256},
        "request_sha256": hashlib.sha256(request).hexdigest(),
        "vl": VL, "host": {"vl": host["vl"], "page": host["page"],
                           "hwcap": f"{host['hwcap']:#x}",
                           "hwcap2": f"{host['hwcap2']:#x}"},
        "cases": counts["cases"],
        "events": dict(sorted(counts["events"].items())),
    }


def strip_native(lines: list[str]) -> list[str]:
    return [line for line in lines
            if json.loads(line).get("producer") != PRODUCER]


def append(options: argparse.Namespace) -> int:
    """Append the records of the responses, which must be identical: the
    responses themselves may differ only in the elements that the
    first-fault relation leaves unpredictable, which the records hold as
    zero."""
    header, lines = sme_native.read_dump(options.input)
    lines = strip_native(lines)
    request = request_bytes()
    cases = [draft.wire() for draft in drafts()]
    runs = [read_response(path.read_bytes(), cases, VL)
            for path in options.response]
    built = [build_records(host, results) for host, results in runs]
    host, results = runs[0]
    records, blobs, counts = built[0]
    differing = 0
    for path, (other_host, other), (other_records, _blobs, _counts) in zip(
            options.response[1:], runs[1:], built[1:]):
        if other_host != host or other_records != records:
            names = [draft.name for draft, one, two in
                     zip(drafts(), results, other) if one != two]
            raise SystemExit(f"sve-native-corpus: {path}: the records differ "
                             f"from the first response's ({len(names)} "
                             f"cases, the first {names[:1]})")
        for draft, case, one, two in zip(drafts(), cases, results, other):
            if one == two:
                continue
            if record_case(draft, case, one, 0).get("validator") != \
                    NONFAULT or (one.esr, one.sve_size) != \
                    (two.esr, two.sve_size):
                raise SystemExit(f"sve-native-corpus: {path}: {draft.name} "
                                 "differs outside an unpredictable element")
        differing = max(differing, sum(one != two for one, two in
                                       zip(results, other)))
    header["meta"]["sve_native"] = native_meta(request, host, counts)
    sme_native.write_dump(options.output, header, lines, blobs, records)
    print(f"appended {counts['cases']} native SVE cases in {len(records)} "
          f"records: " + ", ".join(f"{name} {count}" for name, count in
                                   sorted(counts["events"].items())))
    if len(runs) > 1:
        print(f"{len(runs)} responses give identical records; at most "
              f"{differing} cases differ between them, each only in "
              "elements a first-fault load leaves unpredictable")
    return 0


def carry(options: argparse.Namespace) -> int:
    header, lines = sme_native.read_dump(options.input)
    lines = strip_native(lines)
    pack, tokens = corpus.decode_pack(options.carry.read_bytes())
    meta = pack.meta.get("sve_native")
    request = request_bytes()
    if meta is None or \
            meta["request_sha256"] != hashlib.sha256(request).hexdigest():
        raise SystemExit("sve-native-corpus: the request changed since the "
                         "pack's native records; run the oracle again")
    native = [record for record in pack.records
              if record.producer == PRODUCER]
    dumped = [json.loads(line) for line in corpus.dump_corpus(
        corpus.Corpus(meta=pack.meta, blobs=pack.blobs, records=native),
        tokens)]
    blobs = {item["blob"]: zlib.decompress(bytes.fromhex(item["zlib"]))
             for item in dumped if "blob" in item}
    used = {region["blob"] for item in dumped if "cases" in item
            for case in item["cases"] for region in case.get("memory", [])}
    # Each record holds its drafts' cases in order.
    queues: dict[tuple, list] = {}
    for item in dumped:
        if "cases" not in item:
            continue
        key = (item["record"].split("/")[1], tuple(item["profiles"]))
        queues[key] = [
            (tuple(int(word, 16) for _offset, word in
                   item["programs"][case["program"]]["words"]), case)
            for case in reversed(item["cases"])]
    carried = []
    for draft in drafts():
        queue = queues.get(record_key(draft))
        words, case = queue.pop() if queue else ((), {})
        if case.get("name") != draft.name or words != draft.wire().words:
            raise SystemExit(f"sve-native-corpus: {draft.name}: the pack's "
                             "native cases do not answer the request")
        carried.append((draft, words, case))
    if any(queues.values()):
        raise SystemExit("sve-native-corpus: the pack holds native cases "
                         "the request lacks")
    header["meta"]["sve_native"] = dict(meta, sha256=script_hash())
    sme_native.write_dump(options.output, header, lines,
                          {digest: blobs[digest] for digest in sorted(used)},
                          assemble(carried))
    print(f"carried {len(carried)} native SVE cases")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument("--request", type=pathlib.Path,
                        help="write the oracle request and exit")
    parser.add_argument("--response", type=pathlib.Path, action="append",
                        default=[], help="an oracle response to the request; "
                        "several must be identical")
    parser.add_argument("--carry", type=pathlib.Path,
                        help="copy the native records of this pack instead")
    options = parser.parse_args()
    if options.request is not None:
        data = request_bytes()
        options.request.write_bytes(data)
        print(f"wrote {len(drafts())} cases, {len(data)} bytes, sha256 "
              f"{hashlib.sha256(data).hexdigest()}")
        return 0
    if options.input is None or options.output is None or \
            bool(options.response) == (options.carry is not None):
        parser.error("give INPUT OUTPUT and either --response or --carry")
    return carry(options) if options.carry is not None else append(options)


if __name__ == "__main__":
    sys.exit(main())
