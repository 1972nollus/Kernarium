#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build, check, dump, and re-pack the checked-in A64 instruction corpus."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import csv
import dataclasses
import functools
import gzip
import hashlib
import json
import pathlib
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import Iterable, NoReturn


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
UNIT = REPOSITORY / "tests" / "unit" / "arm64"
CORPUS = UNIT / "corpus"
PACK_NAME = "a64_instruction_corpus.pack"
INVENTORY = UNIT / "a64_instruction_inventory.tsv"
ALLOCATIONS = UNIT / "a64_instruction_allocations.tsv"
PARTITIONS = UNIT / "a64_instruction_decode_partitions.tsv"
FEATURES = REPOSITORY / "darwin" / "platform" / "host_capabilities.hpp"

PACK_MAGIC = b"UMLA64C\0"
PACK_VERSION = 1
EXPORT_MAGIC = b"A64CEXP1"
CODE_BASE = 0x4F00_0000
PROFILES = {16384: 1, 4096: 2}
PROFILE_NAMES = {1: "16k", 2: "4k", 3: "16k+4k"}
DEFAULT_OPTIONS = 1
OPTION_NAMES = (
    "enable_pointer_authentication",
    "enable_guarded_control_flow",
    "force_software_checksum",
    "force_software_flags",
    "force_software_sha3",
    "force_software_fixed_transforms",
    "force_software_aes",
    "force_software_pmull",
    "force_software_dotprod",
    "force_software_i8mm",
)
EVENTS = (
    "system-call",
    "memory-fault",
    "gcs-fault",
    "illegal-instruction",
    "breakpoint",
    "floating-point-trap",
    "pc-alignment",
    "instruction-budget",
    "fatal",
)
ACCESSES = ("read", "write", "execute")
MINIMIZATION = ("full", "narrow", "broad")
UNBOUNDED = 0xFFFF_FFFF
NO_VALIDATOR = 0xFFFF

# Exported state order; see write_state() in the C++ exporter.
EXPORT_STATE = struct.Struct("<31QQQI512sIIQQBBH256sBHBQBBIBBBBBBBQQQQQBBQBBB10QB")
EXPORT_FIELDS = (
    "pc", "nzcv", "vectors", "fpcr", "fpsr", "fpmr", "tls", "mte_enabled",
    "tbi_enabled", "mte_exclude", "mte_tag_choices", "mte_random_tags",
    "mte_seed", "mte_tag", "mte_random_state", "cpa_enabled",
    "cpa_multiply_enabled", "accdata", "pstate_ssbs", "pstate_dit",
    "pstate_tco", "pstate_sm", "pstate_za", "pstate_daif_access",
    "pstate_daif", "exclusive_address", "exclusive_key", "exclusive_value",
    "exclusive_value2", "exclusive_version", "exclusive_size", "fp_dirty",
    "gcs_pointer", "gcs_implemented", "gcs_push_enabled",
    "gcs_store_enabled", "pointer_auth_keys",
    "pointer_authentication_enabled",
)

# Sparse non-register state fields: pack id, name, struct format.
STATE_FIELDS = (
    (1, "pc", "Q"),
    (2, "nzcv", "I"),
    (3, "fpcr", "I"),
    (4, "fpsr", "I"),
    (5, "fpmr", "Q"),
    (6, "tls", "Q"),
    (7, "mte_enabled", "B"),
    (8, "tbi_enabled", "B"),
    (9, "mte_exclude", "H"),
    (10, "mte_tag_choices", "256s"),
    (11, "mte_random_tags", "B"),
    (12, "mte_seed", "H"),
    (13, "mte_tag", "B"),
    (14, "mte_random_state", "Q"),
    (15, "cpa_enabled", "B"),
    (16, "cpa_multiply_enabled", "B"),
    (17, "accdata", "I"),
    (18, "pstate_ssbs", "B"),
    (19, "pstate_dit", "B"),
    (20, "pstate_tco", "B"),
    (21, "pstate_sm", "B"),
    (22, "pstate_za", "B"),
    (23, "pstate_daif_access", "B"),
    (24, "pstate_daif", "B"),
    (25, "exclusive_address", "Q"),
    (26, "exclusive_key", "Q"),
    (27, "exclusive_value", "Q"),
    (28, "exclusive_value2", "Q"),
    (29, "exclusive_version", "Q"),
    (30, "exclusive_size", "B"),
    (31, "fp_dirty", "B"),
    (32, "gcs_pointer", "Q"),
    (33, "gcs_implemented", "B"),
    (34, "gcs_push_enabled", "B"),
    (35, "gcs_store_enabled", "B"),
    (36, "pointer_auth_keys", "80s"),
    (37, "pointer_authentication_enabled", "B"),
)
FIELD_BY_NAME = {name: (identifier, fmt) for identifier, name, fmt in STATE_FIELDS}
FIELD_BY_ID = {identifier: (name, fmt) for identifier, name, fmt in STATE_FIELDS}
DETERMINISTIC_KEYS = (
    0x84BE_85CE_9804_E94B, 0xEC28_0D07_4B7A_C6D3,
    0x4A65_7B64_56CB_C3D1, 0x2B7E_1516_28AE_D2A6,
    0x09CF_4F3C_ABF7_1528, 0x7F4A_7C15_9E37_79B9,
    0xD1B5_4A32_D192_ED03, 0x94D0_49BB_1331_11EB,
    0x2545_F491_4F6C_DD1D, 0xBF58_476D_1CE4_E5B9,
)
DEFAULT_STATE = {
    "pc": 0, "nzcv": 0, "fpcr": 0, "fpsr": 0, "fpmr": 0, "tls": 0,
    "mte_enabled": 0, "tbi_enabled": 0, "mte_exclude": 0,
    "mte_tag_choices": bytes(256), "mte_random_tags": 0, "mte_seed": 1,
    "mte_tag": 0, "mte_random_state": 0x9E37_79B9_7F4A_7C15,
    "cpa_enabled": 0, "cpa_multiply_enabled": 0, "accdata": 0,
    "pstate_ssbs": 0, "pstate_dit": 0, "pstate_tco": 0, "pstate_sm": 0,
    "pstate_za": 0, "pstate_daif_access": 0, "pstate_daif": 0,
    "exclusive_address": 0xFFFF_FFFF_FFFF_FFFF, "exclusive_key": 0,
    "exclusive_value": 0, "exclusive_value2": 0, "exclusive_version": 0,
    "exclusive_size": 0, "fp_dirty": 0, "gcs_pointer": 0,
    "gcs_implemented": 0, "gcs_push_enabled": 0, "gcs_store_enabled": 0,
    "pointer_auth_keys": struct.pack("<10Q", *DETERMINISTIC_KEYS),
    "pointer_authentication_enabled": 1,
}


def fail(message: str) -> NoReturn:
    raise SystemExit(f"build-unit-corpus: {message}")


def fill_register(index: int) -> int:
    """Canonical GPR fill shared with the exporter and runner; 31 is SP."""
    index &= 31
    return 0x5EED_0000_0000_0000 | (index << 40) | (0x0101_0101 * (index + 1))


def fill_vector(index: int) -> bytes:
    index &= 31
    return bytes((((index * 16 + byte) * 0x9D) + 0x3B) & 0xFF for byte in range(16))


FILL_REGISTERS = tuple(fill_register(index) for index in range(32))
FILL_VECTORS = tuple(fill_vector(index) for index in range(32))


def feature_tokens() -> tuple[str, ...]:
    text = FEATURES.read_text(encoding="utf-8")
    start = text.index("arm_feature_tokens{")
    end = text.index("};", start)
    tokens = tuple(
        item.split('"')[1]
        for item in text[start:end].split("std::string_view{")[1:]
    )
    if len(tokens) < 100 or len(set(tokens)) != len(tokens):
        fail("cannot read the stable ArmFeature token table")
    return tokens


# --------------------------------------------------------------------------
# Canonical in-memory model


@dataclass(frozen=True)
class Region:
    address: int
    size: int
    flags: int
    blob: str
    runs: tuple[tuple[int, bytes], ...]
    tags: tuple[tuple[int, int], ...]
    expected_tags: tuple[tuple[int, int], ...]


@dataclass(frozen=True)
class Case:
    name: str
    entry: int
    words: tuple[tuple[int, int], ...]
    event: str
    invalid_encoding: bool
    minimization: str
    retired: tuple
    fault: tuple[int, str] | None
    syndrome: int
    native: tuple[str, ...]
    validator: str | None
    initial: tuple
    expected: tuple
    memory: tuple[Region, ...]
    # Producer check ordinals in the 16 KiB and 4 KiB exports; 0 is absent.
    ordinals: tuple[int, int] = (0, 0)

    def identity(self) -> tuple:
        return (
            self.name, self.entry, self.words, self.event,
            self.invalid_encoding, self.minimization, self.retired,
            self.fault, self.syndrome, self.native, self.validator,
            self.initial, self.expected, self.memory,
        )


@dataclass
class Record:
    name: str
    producer: str
    site: str
    options: int
    profiles: int
    programs: list[tuple[int, tuple[tuple[int, int], ...]]] = field(
        default_factory=list
    )
    cases: list[tuple[int, Case]] = field(default_factory=list)


@dataclass
class Corpus:
    meta: dict
    blobs: dict[str, bytes]
    records: list[Record]


@functools.lru_cache(maxsize=None)
def derived_tag_choices(enabled: int, exclude: int) -> bytes:
    """CpuState::set_mte_configuration() table for (enabled, exclude)."""
    def choose(start: int, offset: int) -> int:
        if not enabled or exclude == 0xFFFF:
            return 0
        tag = start & 15
        remaining = offset & 15
        if remaining == 0:
            while exclude & (1 << tag):
                tag = (tag + 1) & 15
        else:
            while remaining:
                remaining -= 1
                tag = (tag + 1) & 15
                while exclude & (1 << tag):
                    tag = (tag + 1) & 15
        return tag

    return bytes(choose(start, offset) for start in range(16) for offset in range(16))


def stored_fields(state: dict, base: dict) -> tuple:
    """Fields that differ from base. The tag-choice table is implicit when it
    is the one set_mte_configuration() derives from the MTE controls."""
    fields = []
    for _identifier, name, _format in STATE_FIELDS:
        if name == "mte_tag_choices":
            if state[name] != derived_tag_choices(state["mte_enabled"],
                                                  state["mte_exclude"]):
                fields.append((name, state[name]))
        elif state[name] != base[name]:
            fields.append((name, state[name]))
    return tuple(fields)


def sparse_initial(state: dict, entry: int) -> tuple:
    gprs = tuple(
        (index, state["gpr"][index])
        for index in range(32)
        if state["gpr"][index] != FILL_REGISTERS[index]
    )
    vectors = tuple(
        (index, state["vectors"][index])
        for index in range(32)
        if state["vectors"][index] != FILL_VECTORS[index]
    )
    return gprs, vectors, stored_fields(state, base_state(entry))


def sparse_expected(initial: dict, expected: dict) -> tuple:
    gprs = tuple(
        (index, expected["gpr"][index])
        for index in range(32)
        if expected["gpr"][index] != initial["gpr"][index]
    )
    vectors = tuple(
        (index, expected["vectors"][index])
        for index in range(32)
        if expected["vectors"][index] != initial["vectors"][index]
    )
    return gprs, vectors, stored_fields(expected, initial)


def apply_state(base: dict, sparse: tuple) -> dict:
    """Decoding rule shared with the runner."""
    state = {
        "gpr": list(base["gpr"]),
        "vectors": list(base["vectors"]),
        **{name: base[name] for _i, name, _f in STATE_FIELDS},
    }
    gprs, vectors, others = sparse
    for index, value in gprs:
        state["gpr"][index] = value
    for index, value in vectors:
        state["vectors"][index] = value
    explicit_table = False
    for name, value in others:
        state[name] = value
        explicit_table = explicit_table or name == "mte_tag_choices"
    if not explicit_table:
        state["mte_tag_choices"] = derived_tag_choices(state["mte_enabled"],
                                                       state["mte_exclude"])
    return state


def base_state(entry: int) -> dict:
    state = {
        "gpr": list(FILL_REGISTERS),
        "vectors": list(FILL_VECTORS),
        **DEFAULT_STATE,
    }
    state["pc"] = CODE_BASE + entry
    return state


def runs_between(initial: bytes, expected: bytes) -> tuple[tuple[int, bytes], ...]:
    runs = []
    offset = 0
    size = len(initial)
    while offset < size:
        if initial[offset] == expected[offset]:
            offset += 1
            continue
        start = offset
        # Merge runs separated by fewer than eight unchanged bytes.
        gap = 0
        end = offset
        while offset < size and gap < 8:
            if initial[offset] != expected[offset]:
                end = offset + 1
                gap = 0
            else:
                gap += 1
            offset += 1
        while end - start > 0xFFFF:
            runs.append((start, expected[start:start + 0xFFFF]))
            start += 0xFFFF
        runs.append((start, expected[start:end]))
        offset = end
    return tuple(runs)


# --------------------------------------------------------------------------
# Export reader


class ByteReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        value = self.data[self.offset:self.offset + size]
        if len(value) != size:
            fail("truncated input")
        self.offset += size
        return value

    def unpack(self, fmt: str):
        size = struct.calcsize("<" + fmt)
        return struct.unpack("<" + fmt, self.take(size))

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return self.unpack("H")[0]

    def u32(self) -> int:
        return self.unpack("I")[0]

    def u64(self) -> int:
        return self.unpack("Q")[0]

    def string32(self) -> str:
        return self.take(self.u32()).decode()

    def string16(self) -> str:
        return self.take(self.u16()).decode()

    def done(self) -> bool:
        return self.offset == len(self.data)


def read_export_state(reader: ByteReader) -> dict:
    raw = EXPORT_STATE.unpack(reader.take(EXPORT_STATE.size))
    state: dict = {"gpr": list(raw[0:31]) + [raw[31]]}
    values = list(raw[32:])
    index = 0
    for name in EXPORT_FIELDS:
        if name == "pointer_auth_keys":
            state[name] = struct.pack("<10Q", *values[index:index + 10])
            index += 10
        elif name == "vectors":
            data = values[index]
            state["vectors"] = [data[offset:offset + 16] for offset in range(0, 512, 16)]
            index += 1
        else:
            state[name] = values[index]
            index += 1
    return state


@dataclass
class ExportCase:
    producer: str
    site: str
    class_key: str
    options: int
    ordinal: int
    sampled: bool
    forced: bool
    case: Case


@dataclass
class ExportProfile:
    page_size: int
    limit: int
    seed: int
    cases: list[ExportCase]
    classes: int
    declared: int
    exclusions: dict[str, dict[str, int]]
    producers: int
    forced: int


def read_export_directory(directory: pathlib.Path, page_size: int,
                          blobs: dict[str, bytes]) -> ExportProfile:
    streams = sorted(directory.glob("*.cases.gz"))
    if not streams:
        fail(f"{directory}: contains no export streams")
    limit = seed = None
    cases: list[ExportCase] = []
    classes = declared = forced = 0
    exclusions: dict[str, dict[str, int]] = collections.defaultdict(dict)
    for stream in streams:
        producer = stream.name[:-len(".cases.gz")]
        counts = {}
        with (directory / f"{producer}.classes.tsv").open(encoding="utf-8") as source:
            rows = list(csv.reader(source, delimiter="\t"))
        if rows[0] != ["class", "count", "site", "first_case"]:
            fail(f"{producer}: unexpected class table header")
        for key, count, _site, _first in rows[1:]:
            counts[key] = int(count)
        data = gzip.decompress(stream.read_bytes())
        reader = ByteReader(data)
        if reader.take(8) != EXPORT_MAGIC:
            fail(f"{stream}: not an export stream")
        stream_page = reader.u32()
        stream_limit = reader.u32()
        stream_seed = reader.u64()
        if reader.string32() != producer:
            fail(f"{stream}: producer mismatch")
        if stream_page != page_size:
            fail(f"{stream}: page size {stream_page}, expected {page_size}")
        if limit is None:
            limit, seed = stream_limit, stream_seed
        elif (limit, seed) != (stream_limit, stream_seed):
            fail(f"{stream}: sample limit or seed differs")
        exported = collections.Counter()
        stream_cases = 0
        trailer = None
        while not reader.done():
            tag = reader.u8()
            if tag == 2:
                observed = reader.u64()
                written = reader.u64()
                failures = [reader.string32() for _ in range(reader.u32())]
                trailer = (observed, written, failures)
                break
            if tag != 1:
                fail(f"{stream}: unknown record tag {tag}")
            item = read_export_case(reader, producer, blobs)
            if item.sampled:
                exported[item.class_key] += 1
            else:
                forced += 1
            stream_cases += 1
            cases.append(item)
        if trailer is None or not reader.done():
            fail(f"{stream}: incomplete export stream")
        observed, written, failures = trailer
        if failures:
            fail(f"{stream}: determinism failures: {failures[:3]}")
        if observed != sum(counts.values()) or written != stream_cases:
            fail(f"{stream}: trailer disagrees with its class table")
        for key, count in counts.items():
            if exported[key] != min(count, limit):
                fail(f"{producer}: class is not fully represented: {key}")
        if set(exported) - set(counts):
            fail(f"{producer}: exported an undeclared class")
        classes += len(counts)
        declared += observed
        exclusion_path = directory / f"{producer}.exclusions.tsv"
        with exclusion_path.open(encoding="utf-8") as source:
            rows = list(csv.reader(source, delimiter="\t"))
        for reason, _site, count in rows[1:]:
            exclusions[reason][producer] = (
                exclusions[reason].get(producer, 0) + int(count)
            )
    return ExportProfile(
        page_size=page_size, limit=limit, seed=seed, cases=cases,
        classes=classes, declared=declared,
        exclusions={key: dict(sorted(value.items()))
                    for key, value in sorted(exclusions.items())},
        producers=len(streams),
        forced=forced,
    )


def read_export_case(reader: ByteReader, producer: str,
                     blobs: dict[str, bytes]) -> ExportCase:
    name = reader.string32()
    site = reader.string32()
    class_key = reader.string32()
    reader.u64()  # class index
    reader.u64()  # class size
    ordinal = reader.u64()
    selection = reader.u8()
    if ordinal == 0 or ordinal > 0xFFFF_FFFF or not selection & 3:
        fail(f"{producer}: {name}: invalid check ordinal or selection")
    options = reader.u32()
    event = EVENTS[reader.u8()]
    flags = reader.u8()
    minimization = MINIMIZATION[reader.u8()]
    kind = reader.u8()
    if kind == 0:
        retired = ("exact", reader.u64())
    elif kind == 1:
        minimum, step = reader.u64(), reader.u64()
        has_maximum, maximum = reader.u8(), reader.u64()
        retired = ("pattern", minimum, step, maximum if has_maximum else None)
    elif kind == 2:
        retired = ("unchecked",)
    else:
        fail(f"{producer}: unknown retirement kind")
    fault_address = reader.u64()
    fault_access = ACCESSES[reader.u8()]
    syndrome = reader.u32()
    native_text = reader.string32()
    native = tuple(native_text.split(",")) if native_text else ()
    validator = reader.string32() or None
    entry = reader.u32()
    words = tuple(sorted((reader.u32(), reader.u32()) for _ in range(reader.u32())))
    initial = read_export_state(reader)
    expected = read_export_state(reader)
    memory = []
    for _ in range(reader.u32()):
        address = reader.u64()
        size = reader.u32()
        region_flags = reader.u8()
        initial_bytes = reader.take(size)
        expected_bytes = reader.take(size) if region_flags & 32 else initial_bytes
        tag_lists = []
        for _tag_list in range(2):
            tag_lists.append(tuple(
                (reader.u32(), reader.u8()) for _ in range(reader.u32())
            ))
        digest = hashlib.sha256(initial_bytes).hexdigest()
        blobs.setdefault(digest, initial_bytes)
        memory.append(Region(
            address=address, size=size, flags=region_flags & 31,
            blob=digest, runs=runs_between(initial_bytes, expected_bytes),
            tags=tag_lists[0], expected_tags=tag_lists[1],
        ))
    reader.u64()  # observed interpreter retirement
    reader.u8()  # executed lanes
    compare_fault = bool(flags & 2)
    if initial["pc"] != CODE_BASE + entry:
        fail(f"{producer}: {name}: initial PC is not the entry")
    initial_sparse = sparse_initial(initial, entry)
    expected_sparse = sparse_expected(initial, expected)
    if apply_state(base_state(entry), initial_sparse) != initial or \
            apply_state(initial, expected_sparse) != expected:
        fail(f"{producer}: {name}: sparse state does not reproduce the export")
    return ExportCase(
        producer=producer, site=site, class_key=class_key, options=options,
        ordinal=ordinal, sampled=bool(selection & 1),
        forced=bool(selection & 2),
        case=Case(
            name=name, entry=entry, words=words, event=event,
            invalid_encoding=bool(flags & 1), minimization=minimization,
            retired=retired,
            fault=(fault_address, fault_access) if compare_fault else None,
            syndrome=syndrome if event == "floating-point-trap" else 0,
            native=native, validator=validator,
            initial=initial_sparse,
            expected=expected_sparse,
            memory=tuple(memory),
        ),
    )


def merge_exports(profiles: dict[int, ExportProfile], blobs: dict[str, bytes],
                  meta: dict) -> Corpus:
    groups: dict[tuple, dict[int, list[Case]]] = collections.defaultdict(
        lambda: {1: [], 2: []}
    )
    for page_size, profile in profiles.items():
        mask = PROFILES[page_size]
        for item in profile.cases:
            ordinals = (item.ordinal, 0) if mask == 1 else (0, item.ordinal)
            groups[(item.producer, item.site, item.options)][mask].append(
                dataclasses.replace(item.case, ordinals=ordinals))
    records: list[Record] = []
    for (producer, site, options), by_profile in sorted(groups.items()):
        pool: dict[tuple, list[int]] = collections.defaultdict(list)
        for index, case in enumerate(by_profile[2]):
            pool[case.identity()].append(index)
        taken = set()
        split = {3: [], 1: [], 2: []}
        for case in by_profile[1]:
            matches = pool.get(case.identity())
            if matches:
                index = matches.pop(0)
                taken.add(index)
                split[3].append(dataclasses.replace(
                    case, ordinals=(case.ordinals[0],
                                    by_profile[2][index].ordinals[1])))
            else:
                split[1].append(case)
        split[2] = [case for index, case in enumerate(by_profile[2])
                    if index not in taken]
        for mask in (3, 1, 2):
            if not split[mask]:
                continue
            path, line = site.rsplit(":", 1)
            name = f"{producer}/{pathlib.PurePosixPath(path).name}:{line}"
            if options != DEFAULT_OPTIONS:
                name += f"/options={options:x}"
            if mask != 3:
                name += f"/{PROFILE_NAMES[mask]}"
            record = Record(name=name, producer=producer, site=site,
                            options=options, profiles=mask)
            program_index: dict[tuple, int] = {}
            for case in split[mask]:
                program = (case.entry, case.words)
                if program not in program_index:
                    program_index[program] = len(record.programs)
                    record.programs.append(program)
                record.cases.append((program_index[program], case))
            records.append(record)
    names = [record.name for record in records]
    if len(set(names)) != len(names):
        fail("record names are not unique")
    used = {region.blob for record in records for _p, case in record.cases
            for region in case.memory}
    return Corpus(meta=meta,
                  blobs={digest: blobs[digest] for digest in sorted(used)},
                  records=records)


# --------------------------------------------------------------------------
# Pack encoding


class ByteWriter:
    def __init__(self) -> None:
        self.parts: list[bytes] = []

    def pack(self, fmt: str, *values) -> None:
        self.parts.append(struct.pack("<" + fmt, *values))

    def raw(self, data: bytes) -> None:
        self.parts.append(data)

    def string16(self, text: str) -> None:
        data = text.encode()
        if len(data) > 0xFFFF:
            fail("string is too long")
        self.pack("H", len(data))
        self.raw(data)

    def string32(self, text: str) -> None:
        data = text.encode()
        self.pack("I", len(data))
        self.raw(data)

    def value(self) -> bytes:
        return b"".join(self.parts)


def encode_sparse(writer: ByteWriter, sparse: tuple) -> None:
    gprs, vectors, others = sparse
    mask = 0
    for index, _value in gprs:
        mask |= 1 << index
    writer.pack("I", mask)
    for _index, value in gprs:
        writer.pack("Q", value)
    mask = 0
    for index, _value in vectors:
        mask |= 1 << index
    writer.pack("I", mask)
    for _index, value in vectors:
        writer.raw(value)
    writer.pack("B", len(others))
    for name, value in others:
        identifier, fmt = FIELD_BY_NAME[name]
        writer.pack("B" + fmt, identifier, value)


def decode_sparse(reader: ByteReader) -> tuple:
    mask = reader.u32()
    gprs = tuple((index, reader.u64()) for index in range(32) if mask >> index & 1)
    mask = reader.u32()
    vectors = tuple((index, reader.take(16)) for index in range(32)
                    if mask >> index & 1)
    others = []
    for _ in range(reader.u8()):
        identifier = reader.u8()
        if identifier not in FIELD_BY_ID:
            fail(f"unknown state field {identifier}")
        name, fmt = FIELD_BY_ID[identifier]
        others.append((name, reader.unpack(fmt)[0]))
    return gprs, vectors, tuple(others)


def encode_pack(corpus: Corpus, tokens: tuple[str, ...]) -> bytes:
    native_sets: dict[tuple[str, ...], int] = {}
    validators: dict[str, int] = {}
    for record in corpus.records:
        for _program, case in record.cases:
            native_sets.setdefault(case.native, len(native_sets))
            if case.validator is not None:
                validators.setdefault(case.validator, len(validators))
    blob_index = {digest: index for index, digest in enumerate(corpus.blobs)}
    token_index = {token: index for index, token in enumerate(tokens)}
    body = ByteWriter()
    body.string32(json.dumps(corpus.meta, sort_keys=True, separators=(",", ":")))
    body.pack("H", len(tokens))
    for token in tokens:
        body.string16(token)
    body.pack("H", len(native_sets))
    for features in native_sets:
        body.pack("B", len(features))
        for token in features:
            if token not in token_index:
                fail(f"unknown native feature {token}")
            body.pack("H", token_index[token])
    body.pack("H", len(validators))
    for validator in validators:
        body.string16(validator)
    body.pack("I", len(corpus.blobs))
    for data in corpus.blobs.values():
        body.pack("I", len(data))
        body.raw(data)
    body.pack("I", len(corpus.records))
    for record in corpus.records:
        body.string16(record.name)
        body.string16(record.producer)
        body.string16(record.site)
        body.pack("BH", record.profiles, record.options)
        body.pack("I", len(record.programs))
        for entry, words in record.programs:
            body.pack("HH", entry, len(words))
            for offset, word in words:
                body.pack("HI", offset, word)
        body.pack("I", len(record.cases))
        for program, case in record.cases:
            encode_case(body, program, case, native_sets, validators, blob_index)
    payload = body.value()
    header = PACK_MAGIC + struct.pack("<IIQ", PACK_VERSION, 0, len(payload))
    return header + zlib.compress(payload, 9)


def encode_case(writer: ByteWriter, program: int, case: Case,
                native_sets: dict, validators: dict, blob_index: dict) -> None:
    writer.pack("I", program)
    writer.string16(case.name)
    writer.pack("II", *case.ordinals)
    flags = (1 if case.invalid_encoding else 0) | (2 if case.fault else 0)
    flags |= MINIMIZATION.index(case.minimization) << 2
    writer.pack("BB", EVENTS.index(case.event), flags)
    if case.retired[0] == "exact":
        writer.pack("BI", 0, case.retired[1])
    elif case.retired[0] == "pattern":
        _kind, minimum, step, maximum = case.retired
        writer.pack("BIII", 1, minimum, step,
                    UNBOUNDED if maximum is None else maximum)
    else:
        writer.pack("B", 2)
    if case.fault:
        writer.pack("QB", case.fault[0], ACCESSES.index(case.fault[1]))
    if case.event == "floating-point-trap":
        writer.pack("I", case.syndrome)
    writer.pack("HH", native_sets[case.native],
                NO_VALIDATOR if case.validator is None else validators[case.validator])
    encode_sparse(writer, case.initial)
    encode_sparse(writer, case.expected)
    writer.pack("B", len(case.memory))
    for region in case.memory:
        writer.pack("QIBI", region.address, region.size, region.flags,
                    blob_index[region.blob])
        writer.pack("H", len(region.runs))
        for offset, data in region.runs:
            writer.pack("IH", offset, len(data))
            writer.raw(data)
        for tags in (region.tags, region.expected_tags):
            writer.pack("H", len(tags))
            for offset, value in tags:
                writer.pack("IB", offset, value)


def decode_pack(data: bytes) -> tuple[Corpus, tuple[str, ...]]:
    if data[:8] != PACK_MAGIC:
        fail("not an A64 instruction corpus pack")
    version, reserved, size = struct.unpack_from("<IIQ", data, 8)
    if version != PACK_VERSION or reserved != 0:
        fail("unsupported pack version")
    payload = zlib.decompress(data[24:])
    if len(payload) != size:
        fail("pack body size mismatch")
    reader = ByteReader(payload)
    meta = json.loads(reader.string32())
    tokens = tuple(reader.string16() for _ in range(reader.u16()))
    native_sets = []
    for _ in range(reader.u16()):
        native_sets.append(tuple(tokens[reader.u16()] for _ in range(reader.u8())))
    validators = [reader.string16() for _ in range(reader.u16())]
    blob_list = []
    blobs = {}
    for _ in range(reader.u32()):
        blob = reader.take(reader.u32())
        digest = hashlib.sha256(blob).hexdigest()
        blob_list.append(digest)
        blobs[digest] = blob
    records = []
    for _ in range(reader.u32()):
        record = Record(name=reader.string16(), producer=reader.string16(),
                        site=reader.string16(), options=0, profiles=0)
        record.profiles, record.options = reader.unpack("BH")
        for _program in range(reader.u32()):
            entry, count = reader.unpack("HH")
            record.programs.append(
                (entry, tuple(reader.unpack("HI") for _ in range(count)))
            )
        for _case in range(reader.u32()):
            program = reader.u32()
            entry, words = record.programs[program]
            record.cases.append((program, decode_case(
                reader, entry, words, native_sets, validators, blob_list)))
        records.append(record)
    if not reader.done():
        fail("trailing pack data")
    return Corpus(meta=meta, blobs=blobs, records=records), tokens


def decode_case(reader: ByteReader, entry: int, words: tuple, native_sets: list,
                validators: list, blob_list: list) -> Case:
    name = reader.string16()
    ordinals = reader.unpack("II")
    event_index, flags = reader.unpack("BB")
    kind = reader.u8()
    if kind == 0:
        retired = ("exact", reader.u32())
    elif kind == 1:
        minimum, step, maximum = reader.unpack("III")
        retired = ("pattern", minimum, step, None if maximum == UNBOUNDED else maximum)
    else:
        retired = ("unchecked",)
    fault = None
    if flags & 2:
        address, access = reader.unpack("QB")
        fault = (address, ACCESSES[access])
    event = EVENTS[event_index]
    syndrome = reader.u32() if event == "floating-point-trap" else 0
    native_index, validator_index = reader.unpack("HH")
    initial = decode_sparse(reader)
    expected = decode_sparse(reader)
    memory = []
    for _ in range(reader.u8()):
        address, size, region_flags, blob = reader.unpack("QIBI")
        runs = []
        for _run in range(reader.u16()):
            offset, length = reader.unpack("IH")
            runs.append((offset, reader.take(length)))
        tag_lists = []
        for _tags in range(2):
            tag_lists.append(tuple(reader.unpack("IB") for _ in range(reader.u16())))
        memory.append(Region(address=address, size=size, flags=region_flags,
                             blob=blob_list[blob], runs=tuple(runs),
                             tags=tag_lists[0], expected_tags=tag_lists[1]))
    return Case(
        name=name, entry=entry, words=words, event=event,
        invalid_encoding=bool(flags & 1),
        minimization=MINIMIZATION[(flags >> 2) & 3], retired=retired,
        fault=fault, syndrome=syndrome, native=native_sets[native_index],
        validator=None if validator_index == NO_VALIDATOR else validators[validator_index],
        initial=initial, expected=expected, memory=tuple(memory),
        ordinals=ordinals,
    )


# --------------------------------------------------------------------------
# Text dump and re-pack


def hex64(value: int) -> str:
    return f"0x{value:016x}"


def dump_sparse(sparse: tuple) -> dict:
    gprs, vectors, others = sparse
    result = {}
    for index, value in gprs:
        result["sp" if index == 31 else f"x{index}"] = hex64(value)
    for index, value in vectors:
        result[f"v{index}"] = value.hex()
    for name, value in others:
        result[name] = value.hex() if isinstance(value, bytes) else hex64(value)
    return result


def load_sparse(values: dict) -> tuple:
    gprs, vectors, others = [], [], []
    for key, text in values.items():
        if key == "sp":
            gprs.append((31, int(text, 16)))
        elif key[0] == "x" and key[1:].isdigit():
            gprs.append((int(key[1:]), int(text, 16)))
        elif key[0] == "v" and key[1:].isdigit():
            data = bytes.fromhex(text)
            if len(data) != 16:
                fail(f"vector {key} is not sixteen bytes")
            vectors.append((int(key[1:]), data))
        elif key in FIELD_BY_NAME:
            _identifier, fmt = FIELD_BY_NAME[key]
            value = bytes.fromhex(text) if fmt.endswith("s") else int(text, 16)
            if fmt.endswith("s") and len(value) != struct.calcsize(fmt):
                fail(f"{key} has the wrong size")
            others.append((key, value))
        else:
            fail(f"unknown state field {key}")
    order = {name: identifier for identifier, name, _fmt in STATE_FIELDS}
    return (tuple(sorted(gprs)), tuple(sorted(vectors)),
            tuple(sorted(others, key=lambda item: order[item[0]])))


def dump_corpus(corpus: Corpus, tokens: tuple[str, ...]) -> Iterable[str]:
    yield json.dumps({"format": "arm64-instruction-corpus", "version": PACK_VERSION,
                      "meta": corpus.meta, "feature_tokens": list(tokens)},
                     sort_keys=True)
    for digest, data in corpus.blobs.items():
        yield json.dumps({"blob": digest,
                          "zlib": zlib.compress(data, 9).hex()}, sort_keys=True)
    for record in corpus.records:
        yield json.dumps({
            "record": record.name, "producer": record.producer,
            "site": record.site,
            "profiles": PROFILE_NAMES[record.profiles].split("+"),
            "options": [name for bit, name in enumerate(OPTION_NAMES)
                        if record.options >> bit & 1],
            "programs": [{"entry": entry,
                          "words": [[offset, f"{word:08x}"] for offset, word in words]}
                         for entry, words in record.programs],
            "cases": [dump_case(program, case) for program, case in record.cases],
        }, sort_keys=True)


def dump_case(program: int, case: Case) -> dict:
    result = {
        "name": case.name, "program": program, "event": case.event,
        "ordinals": list(case.ordinals),
        "invalid_encoding": case.invalid_encoding,
        "minimization": case.minimization,
        "native": list(case.native),
        "initial": dump_sparse(case.initial),
        "expected": dump_sparse(case.expected),
    }
    if case.retired[0] == "exact":
        result["retired"] = case.retired[1]
    elif case.retired[0] == "pattern":
        result["retired"] = {"minimum": case.retired[1], "step": case.retired[2],
                             "maximum": case.retired[3]}
    else:
        result["retired"] = None
    if case.fault:
        result["fault"] = {"address": hex64(case.fault[0]), "access": case.fault[1]}
    if case.event == "floating-point-trap":
        result["syndrome"] = f"0x{case.syndrome:08x}"
    if case.validator is not None:
        result["validator"] = case.validator
    if case.memory:
        result["memory"] = [{
            "address": hex64(region.address), "size": region.size,
            "flags": region.flags, "blob": region.blob,
            "expected": [[offset, data.hex()] for offset, data in region.runs],
            "tags": [list(tag) for tag in region.tags],
            "expected_tags": [list(tag) for tag in region.expected_tags],
        } for region in case.memory]
    return result


def load_dump(lines: Iterable[str]) -> tuple[Corpus, tuple[str, ...]]:
    iterator = iter(lines)
    header = json.loads(next(iterator))
    if header.get("format") != "arm64-instruction-corpus" or \
            header.get("version") != PACK_VERSION:
        fail("unsupported corpus dump")
    tokens = tuple(header["feature_tokens"])
    blobs: dict[str, bytes] = {}
    records: list[Record] = []
    for line in iterator:
        if not line.strip():
            continue
        item = json.loads(line)
        if "blob" in item:
            data = zlib.decompress(bytes.fromhex(item["zlib"]))
            if hashlib.sha256(data).hexdigest() != item["blob"]:
                fail("blob digest mismatch")
            blobs[item["blob"]] = data
            continue
        profiles = sum({"16k": 1, "4k": 2}[name] for name in item["profiles"])
        options = sum(1 << OPTION_NAMES.index(name) for name in item["options"])
        record = Record(name=item["record"], producer=item["producer"],
                        site=item["site"], options=options, profiles=profiles)
        for program in item["programs"]:
            record.programs.append((program["entry"], tuple(
                (offset, int(word, 16)) for offset, word in program["words"])))
        for case in item["cases"]:
            entry, words = record.programs[case["program"]]
            record.cases.append((case["program"], load_case(case, entry, words, blobs)))
        records.append(record)
    return Corpus(meta=header["meta"], blobs=blobs, records=records), tokens


def load_case(item: dict, entry: int, words: tuple, blobs: dict) -> Case:
    retired_value = item["retired"]
    if retired_value is None:
        retired = ("unchecked",)
    elif isinstance(retired_value, int):
        retired = ("exact", retired_value)
    else:
        retired = ("pattern", retired_value["minimum"], retired_value["step"],
                   retired_value["maximum"])
    fault = item.get("fault")
    memory = []
    for region in item.get("memory", []):
        if region["blob"] not in blobs:
            fail(f"{item['name']}: unknown memory blob")
        if len(blobs[region["blob"]]) != region["size"]:
            fail(f"{item['name']}: memory blob size mismatch")
        memory.append(Region(
            address=int(region["address"], 16), size=region["size"],
            flags=region["flags"], blob=region["blob"],
            runs=tuple((offset, bytes.fromhex(data))
                       for offset, data in region["expected"]),
            tags=tuple(tuple(tag) for tag in region["tags"]),
            expected_tags=tuple(tuple(tag) for tag in region["expected_tags"]),
        ))
    if item["event"] not in EVENTS:
        fail(f"{item['name']}: unknown event")
    return Case(
        name=item["name"], entry=entry, words=words, event=item["event"],
        invalid_encoding=item["invalid_encoding"],
        minimization=item["minimization"], retired=retired,
        fault=(int(fault["address"], 16), fault["access"]) if fault else None,
        syndrome=int(item.get("syndrome", "0"), 16),
        native=tuple(item["native"]), validator=item.get("validator"),
        initial=load_sparse(item["initial"]),
        expected=load_sparse(item["expected"]),
        memory=tuple(memory),
        ordinals=tuple(item["ordinals"]),
    )


# --------------------------------------------------------------------------
# Coverage and metadata


class Pattern:
    def __init__(self, mask: str, value: str, constraints: str) -> None:
        self.mask = int(mask, 16)
        self.value = int(value, 16)
        self.constraints = []
        if constraints != "none":
            for item in constraints.split(";"):
                field_text, _at, rest = item.partition("@")
                location, operator, pattern = (
                    rest.partition("!=") if "!=" in rest else rest.partition("==")
                )
                low, width = (int(part) for part in location.split(":"))
                if len(pattern) != width:
                    fail(f"constraint width mismatch in {item}")
                self.constraints.append((low, width, operator == "==", pattern))

    def admits_top_byte(self, top: int) -> bool:
        return (top << 24) & self.mask & 0xFF00_0000 == self.value & 0xFF00_0000

    def matches(self, word: int) -> bool:
        if word & self.mask != self.value:
            return False
        for low, width, equal, pattern in self.constraints:
            actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
            same = all(a == "x" or a == b for a, b in zip(pattern, actual))
            if same != equal:
                return False
        return True


class Classifier:
    """Maps an entry word to inventory entries and to reserved partitions."""

    def __init__(self) -> None:
        self.allocations = []
        with ALLOCATIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                self.allocations.append((
                    Pattern(row["architecture_mask"], row["architecture_value"],
                            row["architecture_constraints"]),
                    row["stable_id"],
                ))
        self.hierarchy = []
        self.tables: dict[str, list] = collections.defaultdict(list)
        with PARTITIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                related = [] if row["related_stable_ids"] == "none" else \
                    row["related_stable_ids"].split("+")
                item = (Pattern(row["architecture_mask"], row["architecture_value"],
                                row["architecture_constraints"]),
                        row["partition_kind"], row["decode_class"], related)
                if row["decode_layer"] == "hierarchy":
                    self.hierarchy.append(item)
                else:
                    self.tables[row["decode_class"]].append(item)
        self.cache: dict[int, tuple[str, ...]] = {}
        # Index both predicate lists by the top encoding byte.
        self.allocations_by_top = [
            [item for item in self.allocations if item[0].admits_top_byte(top)]
            for top in range(256)
        ]
        self.hierarchy_by_top = [
            [item for item in self.hierarchy if item[0].admits_top_byte(top)]
            for top in range(256)
        ]

    def classify(self, word: int) -> tuple[str, ...]:
        """Returns the inventory entries a word exercises or reserves.

        An alias is credited through its official allocation predicate; every
        other allocated or reserved word is credited to the entries that the
        official decode partition relates to it."""
        if word in self.cache:
            return self.cache[word]
        top = word >> 24
        ids = {stable_id for pattern, stable_id in self.allocations_by_top[top]
               if pattern.matches(word)}
        leaves = [item for item in self.hierarchy_by_top[top]
                  if item[0].matches(word)]
        if len(leaves) == 1:
            _pattern, kind, decode_class, related = leaves[0]
            if kind == "unallocated":
                ids.update(related)
            else:
                rows = [item for item in self.tables[decode_class]
                        if item[0].matches(word)]
                if len(rows) == 1:
                    ids.update(rows[0][3])
        result = tuple(sorted(ids))
        self.cache[word] = result
        return result


def entry_word(case: Case) -> int:
    for offset, word in case.words:
        if offset == case.entry:
            return word
    fail(f"{case.name}: entry word is absent")


def coverage_rows(corpus: Corpus) -> tuple[list[list[str]], dict]:
    classifier = Classifier()
    counts: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    words: dict[str, set[int]] = collections.defaultdict(set)
    unattributed = collections.Counter()
    for record in corpus.records:
        for _program, case in record.cases:
            word = entry_word(case)
            ids = classifier.classify(word)
            column = "illegal" if case.event == "illegal-instruction" else "cases"
            if not ids:
                unattributed[column] += 1
            for stable_id in ids:
                counts[stable_id][column] += 1
                words[stable_id].add(word)
    rows = [["stable_id", "section", "label", "cases", "illegal_cases",
             "distinct_entry_words"]]
    covered = illegal = 0
    with INVENTORY.open(encoding="utf-8") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            stable_id = row["stable_id"]
            count = counts[stable_id]
            covered += count["cases"] > 0
            illegal += count["illegal"] > 0
            rows.append([stable_id, row["section"], row["pdf_label"],
                         str(count["cases"]), str(count["illegal"]),
                         str(len(words[stable_id]))])
    summary = {
        "entries": len(rows) - 1,
        "entries_with_cases": covered,
        "entries_with_illegal_cases": illegal,
        "unattributed_cases": unattributed["cases"],
        "unattributed_illegal_cases": unattributed["illegal"],
    }
    return rows, summary


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def record_hashes(record: Record) -> tuple[str, str]:
    code = ByteWriter()
    for entry, words in record.programs:
        code.pack("HH", entry, len(words))
        for offset, word in words:
            code.pack("HI", offset, word)
    payload = ByteWriter()
    for program, case in record.cases:
        payload.raw(json.dumps(dump_case(program, case), sort_keys=True).encode())
        payload.raw(b"\n")
    return sha256(code.value()), sha256(payload.value())


def render_tsv(rows: list[list[str]]) -> bytes:
    return "".join("\t".join(row) + "\n" for row in rows).encode()


def metadata(corpus: Corpus, pack: bytes) -> dict[str, bytes]:
    manifest = [["name", "producer", "site", "options", "profiles", "programs",
                 "cases", "code_sha256", "payload_sha256"]]
    profile_cases = collections.Counter()
    minimization = collections.Counter()
    validators = collections.Counter()
    events = collections.Counter()
    programs = 0
    for record in corpus.records:
        code_hash, payload_hash = record_hashes(record)
        manifest.append([record.name, record.producer, record.site,
                         f"{record.options:x}", PROFILE_NAMES[record.profiles],
                         str(len(record.programs)), str(len(record.cases)),
                         code_hash, payload_hash])
        programs += len(record.programs)
        profile_cases[PROFILE_NAMES[record.profiles]] += len(record.cases)
        for _program, case in record.cases:
            minimization[case.minimization] += 1
            events[case.event] += 1
            if case.validator is not None:
                validators[case.validator] += 1
    coverage, coverage_summary = coverage_rows(corpus)
    cases = sum(profile_cases.values())
    provenance = {
        "schema": 1,
        "producer_commit": corpus.meta["producer_commit"],
        "sample_limit": corpus.meta["sample_limit"],
        "seed": corpus.meta["seed"],
        "exports": corpus.meta["exports"],
        "records": len(corpus.records),
        "programs": programs,
        "cases": cases,
        "cases_by_profile": dict(sorted(profile_cases.items())),
        "cases_by_event": dict(sorted(events.items())),
        "state_minimization": dict(sorted(minimization.items())),
        "validator_cases": dict(sorted(validators.items())),
        "memory_blobs": len(corpus.blobs),
        "coverage": coverage_summary,
        "pack_bytes": len(pack),
        "pack_sha256": sha256(pack),
    }
    return {
        "manifest.tsv": render_tsv(manifest),
        "coverage.tsv": render_tsv(coverage),
        "provenance.json": (json.dumps(provenance, indent=2, sort_keys=True)
                            + "\n").encode(),
    }


# --------------------------------------------------------------------------
# Commands


def build(options: argparse.Namespace) -> int:
    tokens = feature_tokens()
    blobs: dict[str, bytes] = {}
    profiles = {
        16384: read_export_directory(options.export_16k, 16384, blobs),
        4096: read_export_directory(options.export_4k, 4096, blobs),
    }
    limits = {(profile.limit, profile.seed) for profile in profiles.values()}
    if len(limits) != 1:
        fail("the two exports used different sample limits or seeds")
    limit, seed = limits.pop()
    meta = {
        "producer_commit": options.producer_commit,
        "sample_limit": limit,
        "seed": f"0x{seed:016x}",
        "exports": {
            PROFILE_NAMES[PROFILES[page_size]]: {
                "producers": profile.producers,
                "classes": profile.classes,
                "declared_cases": profile.declared,
                "exported_cases": len(profile.cases),
                "forced_cases": profile.forced,
                "excluded_cases": profile.exclusions,
            }
            for page_size, profile in sorted(profiles.items(), reverse=True)
        },
    }
    corpus = merge_exports(profiles, blobs, meta)
    pack = encode_pack(corpus, tokens)
    options.output.mkdir(parents=True, exist_ok=True)
    (options.output / PACK_NAME).write_bytes(pack)
    for name, data in metadata(corpus, pack).items():
        (options.output / name).write_bytes(data)
    print(f"wrote {options.output / PACK_NAME}: {len(pack)} bytes, "
          f"{len(corpus.records)} records, "
          f"{sum(len(record.cases) for record in corpus.records)} cases")
    return 0


def check(options: argparse.Namespace) -> int:
    pack = (options.output / PACK_NAME).read_bytes()
    corpus, tokens = decode_pack(pack)
    if not set(tokens) <= set(feature_tokens()):
        fail("the pack names a feature the runtime no longer defines")
    stale = []
    for name, data in metadata(corpus, pack).items():
        path = options.output / name
        if not path.is_file() or path.read_bytes() != data:
            stale.append(name)
    if encode_pack(corpus, tokens) != pack:
        stale.append(PACK_NAME + " (canonical encoding)")
    if stale:
        fail("stale corpus files: " + ", ".join(stale))
    print(f"A64 instruction corpus is fresh: "
          f"{sum(len(record.cases) for record in corpus.records)} cases in "
          f"{len(corpus.records)} records")
    return 0


def dump(options: argparse.Namespace) -> int:
    corpus, tokens = decode_pack(options.dump.read_bytes())
    if options.text is None:
        for line in dump_corpus(corpus, tokens):
            sys.stdout.write(line + "\n")
        return 0
    with options.text.open("w", encoding="utf-8") as sink:
        for line in dump_corpus(corpus, tokens):
            sink.write(line + "\n")
    return 0


def repack(options: argparse.Namespace) -> int:
    with options.repack.open(encoding="utf-8") as source:
        corpus, tokens = load_dump(source)
    pack = encode_pack(corpus, tokens)
    options.output.mkdir(parents=True, exist_ok=True)
    (options.output / PACK_NAME).write_bytes(pack)
    for name, data in metadata(corpus, pack).items():
        (options.output / name).write_bytes(data)
    print(f"re-packed {sum(len(record.cases) for record in corpus.records)} "
          f"cases into {options.output / PACK_NAME}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, default=CORPUS,
                        help="corpus directory (default: %(default)s)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--export-16k", type=pathlib.Path,
                      help="16 KiB export directory; requires --export-4k")
    mode.add_argument("--check", action="store_true",
                      help="verify metadata freshness against the pack")
    mode.add_argument("--dump", type=pathlib.Path, metavar="PACK",
                      help="write a reviewable JSON-lines dump of PACK")
    mode.add_argument("--repack", type=pathlib.Path, metavar="DUMP",
                      help="rebuild the pack and metadata from a dump")
    parser.add_argument("--export-4k", type=pathlib.Path)
    parser.add_argument("--producer-commit",
                        help="commit whose harness produced both exports")
    parser.add_argument("--text", type=pathlib.Path,
                        help="dump destination (default: standard output)")
    options = parser.parse_args()
    if options.export_16k is not None:
        if options.export_4k is None or not options.producer_commit:
            parser.error("--export-16k requires --export-4k and --producer-commit")
        return build(options)
    if options.check:
        return check(options)
    if options.dump is not None:
        return dump(options)
    return repack(options)


if __name__ == "__main__":
    sys.exit(main())
