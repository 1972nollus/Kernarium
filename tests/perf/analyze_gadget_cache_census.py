#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a gadget cache census written by a UML_GADGET_CACHE_CENSUS build.

Every executor's private cache appends one snapshot of itself at teardown,
and one every DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS seconds when that is
set. A snapshot is an `E` line with the cache's measured footprint and the
history it has counted so far, a `G` line per resident address space, a `P`
line per resident translation, and, for the bodies
DARWIN_UML_GADGET_CACHE_CENSUS_WORDS samples, a `W` line with the stream
word by word. Both gadget engines write the same records, so one capture
reads the same way for either; a field an engine has no counterpart for is
simply absent.

A translation is identified by the guest code it consumed rather than by the
stream it produced, because two executors translating the same code emit
equivalent streams that embed pointers to their own link records. That is what
makes the counts here answer how much of the cache is duplicate work: how much
would collapse if executors running one address space shared their bodies, and
how much more would collapse if a fork family's identical pages did too.

The report puts every part of the cache as a share of the measured bytes:
slot tags, body headers, streams, validation units, allocator rounding, link
and indirect records, duplicate bodies, and the address spaces that are
resident but not running. The lookup history gives the hit rate, why each
translation that had to be made again was gone, and how many of those an
exclusive victim tier of a given size would have served.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import math
from pathlib import Path
import sys
import zlib

MIB = 2**20
FINAL = "final"
# Bucket b of the distance histogram holds distances below 2**b.
VICTIM_SIZES = (1 << 12, 1 << 14, 1 << 16, 1 << 18, 1 << 20)


@dataclass
class Block:
    """One cache's records for one snapshot."""
    snapshot: int | str
    tag: str
    fields: dict[str, object]
    start: int
    end: int

    def get(self, name: str, default: int = 0) -> int:
        value = self.fields.get(name, default)
        return value if isinstance(value, int) else default


@dataclass
class Translation:
    generation: int
    pc: int
    instructions: int
    words: int
    units: int
    digest: int
    # Absent from captures written before the census carried the parts.
    allocated: int | None = None
    header: int | None = None
    stream: int | None = None
    source: int | None = None


@dataclass
class Space:
    generation: int
    current: bool
    rank: int
    idle_switches: int
    idle_seconds: float
    links: int
    indirect: int
    targets: int


@dataclass
class Contents:
    block: Block
    spaces: list[Space] = field(default_factory=list)
    translations: list[Translation] = field(default_factory=list)
    # (translation, tokens) for every sampled stream.
    words: list[tuple[Translation, list[str]]] = field(default_factory=list)
    # (address space, guest PC) of every sampled link record.
    links: list[tuple[int, int]] = field(default_factory=list)


def parse_value(text: str) -> object:
    if "," in text:
        return [int(part) for part in text.split(",")]
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def scan(path: Path) -> list[Block]:
    """Every cache snapshot in the capture, with where its records lie."""
    blocks: list[Block] = []
    offset = 0
    with path.open("rb") as stream:
        for line in stream:
            if line.startswith(b"E "):
                if blocks:
                    blocks[-1].end = offset
                fields: dict[str, object] = {}
                parts = line.decode().split()
                for pair in parts[2:]:
                    name, _, value = pair.partition("=")
                    fields[name] = parse_value(value)
                snapshot = fields.get("snapshot", FINAL)
                blocks.append(Block(
                    snapshot=snapshot if isinstance(snapshot, int) else FINAL,
                    tag=parts[1], fields=fields,
                    start=offset + len(line), end=-1))
            elif not blocks and line.strip():
                raise SystemExit(f"{path}: a record precedes its executor")
            offset += len(line)
    if blocks:
        blocks[-1].end = offset
    return blocks


def load(path: Path, block: Block) -> Contents:
    contents = Contents(block)
    with path.open("rb") as stream:
        stream.seek(block.start)
        text = stream.read(block.end - block.start).decode()
    previous: Translation | None = None
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "P":
            numbers = [int(value, 0) for value in fields[1:]]
            translation = Translation(*numbers[:6])
            if len(numbers) >= 10:
                (translation.allocated, translation.header,
                 translation.stream, translation.source) = numbers[6:10]
            contents.translations.append(translation)
            previous = translation
        elif fields[0] == "G":
            values = {name: parse_value(value) for name, _, value in
                      (pair.partition("=") for pair in fields[2:])}
            contents.spaces.append(Space(
                generation=int(fields[1]),
                current=values.get("current") == 1,
                rank=int(values.get("rank", 0)),
                idle_switches=int(values.get("idle_switches", 0)),
                idle_seconds=float(values.get("idle_seconds", 0.0)),
                links=int(values.get("links", 0)),
                indirect=int(values.get("indirect", 0)),
                targets=int(values.get("targets", 0))))
        elif fields[0] == "W" and previous is not None:
            contents.words.append((previous, fields[2:]))
        elif fields[0] == "L":
            contents.links.append((int(fields[1]), int(fields[2], 16)))
    return contents


def snapshot_order(snapshot: int | str) -> float:
    return math.inf if snapshot == FINAL else float(snapshot)


def select(blocks: list[Block], wanted: str) -> list[Block]:
    snapshots = sorted({block.snapshot for block in blocks}, key=snapshot_order)
    if wanted == "last":
        chosen = snapshots[-1]
    elif wanted == FINAL:
        if FINAL not in snapshots:
            raise SystemExit("no teardown snapshot; pass --snapshot last")
        chosen = FINAL
    else:
        chosen = int(wanted)
    return [block for block in blocks if block.snapshot == chosen]


def engine_of(blocks: list[Block]) -> str:
    return "x86-64" if any("decoded" in b.fields for b in blocks) else "arm64"


def round_up(value: int, granule: int = 16) -> int:
    return (max(value, granule) + granule - 1) // granule * granule


def mib(value: float) -> str:
    return f"{value / MIB:8.1f} MiB"


def share(value: float, total: float) -> str:
    return f"{100.0 * value / total:5.1f}%" if total else "    -"


def slab_parts(block: Block, slot_bytes_default: int) -> tuple[int, int]:
    """The slot tags and the rest of the slab bytes (x86 successor sets and
    allocator rounding)."""
    slabs = block.get("slabs")
    slot_bytes = block.get("slot_bytes", slot_bytes_default)
    slab_slots = block.get("slab_slots", 256)
    count = block.get("slab_count", -1)
    if count < 0:
        count = slabs // (slab_slots * slot_bytes)
    tags = count * slab_slots * slot_bytes
    return tags, slabs - tags


def histories(blocks: list[Block]) -> Counter[str]:
    total: Counter[str] = Counter()
    for block in blocks:
        for name, value in block.fields.items():
            if isinstance(value, int):
                total[name] += value
    return total


def distances(blocks: list[Block]) -> list[int]:
    merged: list[int] = []
    for block in blocks:
        value = block.fields.get("distance")
        if isinstance(value, int):
            value = [value]
        if not isinstance(value, list):
            continue
        if len(merged) < len(value):
            merged.extend([0] * (len(value) - len(merged)))
        for index, count in enumerate(value):
            merged[index] += count
    return merged


@dataclass
class Duplication:
    resident: int = 0
    bytes: int = 0
    # Copies of a translation another cache of the same address space holds.
    space_copies: int = 0
    space_bytes: int = 0
    # Beyond those, copies of the same code another address space holds.
    family_copies: int = 0
    family_bytes: int = 0
    distinct_in_space: int = 0
    distinct_code: int = 0
    # Sampled link records, and how many are distinct within their space.
    links_sampled: int = 0
    links_distinct: int = 0


def duplication(contents: list[Contents]) -> Duplication:
    result = Duplication()
    by_space: dict[tuple[int, int, int], list[int]] = defaultdict(list)
    for content in contents:
        for item in content.translations:
            size = item.allocated if item.allocated is not None else 0
            by_space[(item.generation, item.pc, item.digest)].append(size)
            result.resident += 1
            result.bytes += size
    by_code: dict[tuple[int, int], list[int]] = defaultdict(list)
    for (generation, pc, digest), sizes in by_space.items():
        result.space_copies += len(sizes) - 1
        result.space_bytes += sum(sizes) - max(sizes)
        by_code[(pc, digest)].append(max(sizes))
    for sizes in by_code.values():
        result.family_copies += len(sizes) - 1
        result.family_bytes += sum(sizes) - max(sizes)
    result.distinct_in_space = len(by_space)
    result.distinct_code = len(by_code)
    sampled = [link for content in contents for link in content.links]
    result.links_sampled = len(sampled)
    result.links_distinct = len(set(sampled))
    return result


def varint(value: int) -> int:
    return max(1, (value.bit_length() + 6) // 7)


def zigzag(value: int) -> int:
    value &= (1 << 64) - 1
    if value >= 1 << 63:
        value -= 1 << 64
    return (value << 1) ^ (value >> 63)


@dataclass
class Codec:
    bodies: int = 0
    stored: int = 0
    words: int = 0
    tokens: Counter[str] = field(default_factory=Counter)
    model: float = 0.0
    deflated: int = 0


def codec(contents: list[Contents]) -> Codec:
    """Size the sampled bodies under the victim codec C2 proposes: a gadget
    by its rank among the gadgets the sample uses, a link or indirect record
    by the guest PC it names relative to the block, any other word as a
    signed or PC-relative varint, and the validation units as they are;
    zlib on the raw words is the general-purpose reference."""
    sample = [entry for content in contents for entry in content.words]
    result = Codec()
    if not sample:
        return result
    gadgets: Counter[str] = Counter()
    for _, tokens in sample:
        gadgets.update(token for token in tokens if token[0] == "g")
    rank = {name: index for index, (name, _) in
            enumerate(gadgets.most_common())}
    for translation, tokens in sample:
        result.bodies += 1
        result.words += len(tokens)
        if translation.allocated is not None:
            result.stored += translation.allocated
        source = translation.source or 0
        # A count byte, the instruction count and the source units.
        cost = 2.0 + source
        raw = bytearray()
        for token in tokens:
            kind, value = token[0], int(token[1:], 16)
            result.tokens[kind] += 1
            raw += value.to_bytes(8, "little")
            if kind == "g":
                cost += 1 if rank[token] < 128 else 2
            elif kind in "li":
                cost += varint(zigzag(value - translation.pc))
            else:
                cost += 0.25 + min(varint(zigzag(value)),
                                   varint(zigzag(value - translation.pc)))
        result.model += cost
        result.deflated += len(zlib.compress(bytes(raw), 9)) + 2 + source
    return result


def victim_served(histogram: list[int], entries: int) -> int:
    bound = int(math.log2(entries))
    return sum(histogram[: bound + 1])


def report(path: Path, blocks: list[Block], arguments: argparse.Namespace) -> None:
    contents = [load(path, block) for block in blocks]
    engine = engine_of(blocks)
    snapshot = blocks[0].snapshot
    seconds = max(float(b.fields.get("seconds", 0.0)) for b in blocks)
    resident = sum(len(c.translations) for c in contents)
    print(f"{engine} snapshot {snapshot} at {seconds:.1f} s: "
          f"{len(blocks)} caches, {resident:,} resident translations")
    if resident == 0:
        return

    print("\nper cache")
    for content in contents:
        block = content.block
        slots = block.get("slots", arguments.slots)
        lookups = block.get("lookups")
        rate = ((block.get("hits") + block.get("revalidated")) / lookups
                if lookups else float("nan"))
        print(f"  {block.tag}  spaces {block.get('generations'):>2}  "
              f"resident {len(content.translations):>8,}  "
              f"occupancy {100.0 * block.get('occupancy') / slots:5.1f}%  "
              f"cache {block.get('total') / MIB:7.1f} MiB  "
              f"hit rate {100.0 * rate:5.1f}%")

    total = sum(block.get("total") for block in blocks)
    tags = rest = 0
    for block in blocks:
        part_tags, part_rest = slab_parts(block, arguments.slot_bytes)
        tags += part_tags
        rest += part_rest
    translations = [t for c in contents for t in c.translations]
    have_parts = all(t.allocated is not None for t in translations)
    print(f"\nwhere the bytes are, of {total / MIB:.1f} MiB measured")
    rows: list[tuple[str, int]] = [("slot tags", tags)]
    if engine == "x86-64":
        rows.append(("successor sets and slab rounding", rest))
    rows.append(("stream lookup maps", sum(b.get("stream_index") for b in blocks)))
    allocated = sum(t.allocated or 0 for t in translations)
    if have_parts:
        headers = sum(t.header or 0 for t in translations)
        streams = sum(t.stream or 0 for t in translations)
        sources = sum(t.source or 0 for t in translations)
        rows += [("translation headers", headers),
                 ("streams", streams),
                 ("validation units", sources),
                 ("allocation rounding",
                  allocated - headers - streams - sources)]
        if engine == "arm64":
            arena = sum(b.get("streams") for b in blocks)
            if any("arena_empty" in b.fields for b in blocks):
                # Whole chunks with no live body, apart from the free
                # space between the bodies of chunks that still hold one.
                empty = sum(b.get("arena_empty") for b in blocks)
                rows += [("arena chunks holding no body", empty),
                         ("free space in chunks holding a body",
                          arena - allocated - empty)]
            else:
                rows.append(("arena chunks not holding a body",
                             arena - allocated))
    else:
        rows.append(("bodies and streams", sum(b.get("streams") for b in blocks)))
    rows += [("replay decodes", sum(b.get("decoded") for b in blocks)),
             ("link records", sum(b.get("links") for b in blocks)),
             ("indirect sites", sum(b.get("indirect") for b in blocks)),
             ("target tables", sum(b.get("targets") for b in blocks))]
    for name, value in rows:
        if value:
            print(f"  {name:34s} {mib(value)}  {share(value, total)}")

    dup = duplication(contents)
    print("\nduplicate translations")
    print(f"  distinct within an address space {dup.distinct_in_space:>10,}   "
          f"factor {resident / max(dup.distinct_in_space, 1):.3f}x")
    print(f"  distinct ignoring address space  {dup.distinct_code:>10,}   "
          f"factor {resident / max(dup.distinct_code, 1):.3f}x")
    if have_parts:
        print(f"  copies another cache of the same address space holds "
              f"{dup.space_copies:>9,}  {mib(dup.space_bytes)}  "
              f"{share(dup.space_bytes, total)} of the cache")
        print(f"  copies another address space holds besides          "
              f"{dup.family_copies:>9,}  {mib(dup.family_bytes)}  "
              f"{share(dup.family_bytes, total)} of the cache")
    if dup.links_sampled:
        links = sum(b.get("links") for b in blocks)
        spare = links * (1 - dup.links_distinct / dup.links_sampled)
        print(f"  link records another cache of the same space keeps, "
              f"from {dup.links_sampled:,} sampled: "
              f"{100.0 * (1 - dup.links_distinct / dup.links_sampled):.1f}%"
              f"  {mib(spare)}  {share(spare, total)} of the cache")

    print("\naddress spaces resident but not running")
    by_cache_space: dict[tuple[str, int], int] = defaultdict(int)
    for content in contents:
        for item in content.translations:
            by_cache_space[(content.block.tag, item.generation)] += \
                item.allocated or 0
    buckets = ((0.0, 1.0), (1.0, 10.0), (10.0, 60.0), (60.0, math.inf))
    dormant_total = 0
    for low, high in buckets:
        spaces = bodies = state = 0
        for content in contents:
            for space in content.spaces:
                if space.current or not low <= space.idle_seconds < high:
                    continue
                spaces += 1
                bodies += by_cache_space[(content.block.tag, space.generation)]
                state += space.links + space.indirect + space.targets
        dormant_total += bodies + state
        label = f"idle {low:g}-{high:g} s" if high != math.inf else f"idle >{low:g} s"
        print(f"  {label:16s} spaces {spaces:>4}  translations {mib(bodies)}  "
              f"link state {mib(state)}  {share(bodies + state, total)}")
    print(f"  {'all':16s} {'':11s} {mib(dormant_total)} "
          f"{share(dormant_total, total)} of the cache")

    history = histories(blocks)
    lookups = history["lookups"]
    if lookups:
        hits = history["hits"] + history["revalidated"]
        print("\nlookups since the run began")
        print(f"  lookups {lookups:,}   hit rate {100.0 * hits / lookups:.2f}%"
              f"   revalidated {100.0 * history['revalidated'] / lookups:.2f}%"
              f"   misses {history['misses']:,}")
        stores = history["stores"]
        print(f"  stores {stores:,}")
        for name, label in (("first", "first in this cache"),
                            ("again_evicted", "again after way eviction"),
                            ("again_retired", "again after address-space retirement"),
                            ("again_discarded", "again after a discard"),
                            ("changed", "again with different code"),
                            ("restored", "beside a copy still resident")):
            print(f"    {label:38s} {history[name]:>11,}  "
                  f"{share(history[name], stores)}")
        print(f"  translated first by another executor of the space "
              f"{history['other_executor']:>11,}  "
              f"{share(history['other_executor'], stores)}")
        print(f"  translated first in another address space          "
              f"{history['other_space']:>11,}  "
              f"{share(history['other_space'], stores)}")
        evictions = history["evictions"]
        mean_evicted = history["evicted_bytes"] / evictions if evictions else 0.0
        print(f"  departures: evicted {evictions:,} (mean {mean_evicted:.0f} B)"
              f"  retired {history['retirements']:,}"
              f"  discarded {history['discards']:,}")
        histogram = distances(blocks)
        if evictions:
            print("  an exclusive victim tier of V bodies would have served")
            for entries in VICTIM_SIZES:
                served = victim_served(histogram, entries)
                print(f"    V {entries:>9,}  {served:>11,} returns  "
                      f"{share(served, history['misses'])} of misses  "
                      f"hit rate {100.0 * (hits + served) / lookups:.2f}%  "
                      f"holding {mib(entries * mean_evicted)} uncompressed per cache")

    if have_parts:
        print("\nwhat digest validation (C4) would free")
        digest8 = digest16 = 0
        for t in translations:
            assert t.allocated is not None and t.header is not None
            assert t.stream is not None and t.source is not None
            if engine == "arm64":
                digest8 += t.allocated - round_up(t.header + t.stream + 8)
                digest16 += t.allocated - round_up(t.header + t.stream + 16)
            else:
                digest8 += round_up(t.source) - 8
                digest16 += round_up(t.source) - 16
        print(f"  validation units for an 8-byte digest   {mib(digest8)}  "
              f"{share(digest8, total)}")
        print(f"  validation units for a 16-byte digest   {mib(digest16)}  "
              f"{share(digest16, total)}")

    sampled = codec(contents)
    if sampled.bodies:
        print(f"\nsampled streams: {sampled.bodies:,} bodies, "
              f"{sampled.words:,} words "
              + "  ".join(f"{kind} {100.0 * count / sampled.words:.1f}%"
                          for kind, count in sorted(sampled.tokens.items())))
        if sampled.stored:
            print(f"  stored {sampled.stored:,} B   victim codec "
                  f"{sampled.model:,.0f} B ({sampled.stored / sampled.model:.2f}x)"
                  f"   zlib -9 {sampled.deflated:,} B "
                  f"({sampled.stored / sampled.deflated:.2f}x)")

    lengths = Counter(t.words for t in translations)
    print("\nstream length, in words")
    ordered = sorted(lengths.elements())
    print(f"  mean {sum(ordered) / resident:.1f}   median {ordered[resident // 2]}")
    previous = 0
    for bound in (4, 8, 16, 24, 32, 48, 64, 96):
        count = sum(n for length, n in lengths.items() if previous < length <= bound)
        print(f"    {previous + 1:>3}-{bound:<3} {count:>9,} {100.0 * count / resident:5.1f}%")
        previous = bound
    count = sum(n for length, n in lengths.items() if length > previous)
    if count:
        print(f"    >{previous:<6} {count:>9,} {100.0 * count / resident:5.1f}%")


def series(path: Path, blocks: list[Block]) -> None:
    """One line per snapshot: what the caches held, and how the lookups
    since the previous snapshot went."""
    snapshots = sorted({b.snapshot for b in blocks}, key=snapshot_order)
    print(f"{'snapshot':>8} {'s':>7} {'caches':>6} {'resident':>9} "
          f"{'MiB':>7} {'bodies':>7} {'dup/space':>9} {'dup/code':>8} "
          f"{'dormant':>7} {'lookups':>11} {'hit rate':>8} {'stores':>9} "
          f"{'evicted':>8}")
    previous: dict[str, Block] = {}
    for snapshot in snapshots:
        chosen = [b for b in blocks if b.snapshot == snapshot]
        contents = [load(path, block) for block in chosen]
        dup = duplication(contents)
        total = sum(b.get("total") for b in chosen)
        dormant = 0
        for content in contents:
            idle = {s.generation for s in content.spaces if not s.current}
            dormant += sum(t.allocated or 0 for t in content.translations
                           if t.generation in idle)
            dormant += sum(s.links + s.indirect + s.targets
                           for s in content.spaces if not s.current)
        delta: Counter[str] = Counter()
        for block in chosen:
            before = previous.get(block.tag)
            for name in ("lookups", "hits", "revalidated", "stores", "evictions"):
                delta[name] += block.get(name) - (before.get(name) if before else 0)
            previous[block.tag] = block
        rate = ((delta["hits"] + delta["revalidated"]) / delta["lookups"]
                if delta["lookups"] else float("nan"))
        seconds = max(float(b.fields.get("seconds", 0.0)) for b in chosen)
        print(f"{snapshot!s:>8} {seconds:7.1f} {len(chosen):6d} {dup.resident:9,} "
              f"{total / MIB:7.1f} {dup.bytes / MIB:7.1f} "
              f"{share(dup.space_bytes, total):>9} {share(dup.family_bytes, total):>8} "
              f"{share(dormant, total):>7} {delta['lookups']:11,} "
              f"{100.0 * rate:7.2f}% {delta['stores']:9,} {delta['evictions']:8,}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("census", type=Path,
                        help="file written by DARWIN_UML_GADGET_CACHE_CENSUS")
    parser.add_argument("--snapshot", default=None,
                        help="final (the default when the capture has one), "
                             "last, or a periodic snapshot's number")
    parser.add_argument("--series", action="store_true",
                        help="one line per snapshot instead of one report")
    parser.add_argument("--slots", type=int, default=65536,
                        help="cache slots per executor, for a record that "
                             "does not carry its own (%(default)s)")
    parser.add_argument("--slot-bytes", type=int, default=40,
                        help="bytes per cache slot, for a record that does "
                             "not carry its own (%(default)s)")
    arguments = parser.parse_args()

    blocks = scan(arguments.census)
    if not blocks:
        raise SystemExit(f"{arguments.census}: no executor records")
    if arguments.series:
        series(arguments.census, blocks)
        return 0
    wanted = arguments.snapshot
    if wanted is None:
        wanted = FINAL if any(b.snapshot == FINAL for b in blocks) else "last"
    report(arguments.census, select(blocks, wanted), arguments)
    return 0


if __name__ == "__main__":
    sys.exit(main())
