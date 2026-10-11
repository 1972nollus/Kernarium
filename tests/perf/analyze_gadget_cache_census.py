#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a gadget cache census written by a UML_GADGET_CACHE_CENSUS build.

The census records what every executor's private cache still held when the run
ended: one `E` line per executor with its measured footprint, and one `P` line
per resident translation naming the address space, the guest PC, the stream
length and a digest of the guest code the translation consumed. Both gadget
engines write it, so one capture reads the same way for either.

A translation is identified by the guest code it consumed rather than by the
stream it produced, because two executors translating the same code emit
equivalent streams that embed pointers to their own link records. That is what
makes the counts here answer how much of the cache is duplicate work: how much
would collapse if executors running one address space shared their bodies, and
how much more would collapse if a fork family's identical pages did too.

Footprint fields an engine does not have are simply absent: only arm64 owns a
stream lookup index and counts the lookups against it, and only x86-64 owns a
per-address-space target link table and holds the decoded instructions of the
blocks a budget-limited replay has entered. Each record carries its cache's
geometry (slots, slot size, and how many slabs of slots were committed); the
command-line geometry stands in only for a capture written before it did.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
import sys


@dataclass
class Executor:
    tag: str
    generations: int = 0
    occupancy: int = 0
    # The geometry the cache was compiled with, absent from older captures.
    slots: int | None = None
    slot_bytes: int | None = None
    slab_slots: int | None = None
    slab_count: int | None = None
    footprint: dict[str, int] = field(default_factory=dict)


@dataclass
class Program:
    generation: int
    pc: int
    instructions: int
    words: int
    # arm64 counts the source in guest words and x86-64 in bytes; the digest
    # taken over it is what identifies a translation, so the unit only has to
    # be consistent within an engine.
    source: int
    digest: int


def parse(path: Path) -> tuple[list[Executor], list[list[Program]]]:
    executors: list[Executor] = []
    programs: list[list[Program]] = []
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "E":
            executor = Executor(tag=fields[1])
            for pair in fields[2:]:
                name, _, value = pair.partition("=")
                if name == "generations":
                    executor.generations = int(value)
                elif name == "occupancy":
                    executor.occupancy = int(value)
                elif name in ("slots", "slot_bytes", "slab_slots",
                              "slab_count"):
                    setattr(executor, name, int(value))
                else:
                    executor.footprint[name] = int(value)
            executors.append(executor)
            programs.append([])
        elif fields[0] == "P":
            if not programs:
                raise SystemExit(f"{path}: a translation precedes its executor")
            programs[-1].append(Program(
                generation=int(fields[1]),
                pc=int(fields[2], 16),
                instructions=int(fields[3]),
                words=int(fields[4]),
                source=int(fields[5]),
                digest=int(fields[6], 16)))
    return executors, programs


def histogram(counter: Counter[int], buckets: list[int]) -> list[str]:
    total = sum(counter.values())
    if total == 0:
        return []
    lines = []
    previous = 0
    for bound in buckets:
        count = sum(n for length, n in counter.items()
                    if previous < length <= bound)
        lines.append(f"    {previous + 1:>3}-{bound:<3} "
                     f"{count:>9,} {100.0 * count / total:5.1f}%")
        previous = bound
    count = sum(n for length, n in counter.items() if length > previous)
    if count:
        lines.append(f"    >{previous:<6} {count:>9,} "
                     f"{100.0 * count / total:5.1f}%")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("census", type=Path,
                        help="file written by DARWIN_UML_GADGET_CACHE_CENSUS")
    parser.add_argument("--slots", type=int, default=65536,
                        help="cache slots per executor, for a record that "
                             "does not carry its own (%(default)s)")
    parser.add_argument("--slot-bytes", type=int, default=40,
                        help="bytes per cache slot, for a record that does "
                             "not carry its own (%(default)s)")
    arguments = parser.parse_args()

    executors, programs = parse(arguments.census)
    if not executors:
        raise SystemExit(f"{arguments.census}: no executor records")

    resident = sum(len(group) for group in programs)
    print(f"executors                     {len(executors)}")
    print(f"resident translations         {resident:,}")
    if resident == 0:
        return 0

    print("\nper executor")
    for executor, group in zip(executors, programs):
        total = executor.footprint.get("total", 0)
        slots = executor.slots if executor.slots is not None else arguments.slots
        print(f"  {executor.tag}  address spaces {executor.generations}  "
              f"resident {len(group):>7,}  "
              f"occupancy {100.0 * executor.occupancy / slots:5.1f}%  "
              f"cache {total / 2**20:7.1f} MiB")
    measured = sum(e.footprint.get("total", 0) for e in executors)
    print(f"  measured cache total        {measured / 2**20:.1f} MiB")
    # How often a stream address had to be mapped back to its translation,
    # against the stores an index keyed by that address would be maintained at.
    stores = sum(e.footprint.get("stores", 0) for e in executors)
    lookups = sum(e.footprint.get("stream_lookups", 0) for e in executors)
    if stores:
        print(f"  stores {stores:,}   stream lookups {lookups:,}   "
              f"one lookup per {stores / max(lookups, 1):.0f} stores")

    # What one address space's executors would share: identical guest words at
    # the same PC in the same address space is the same translation.
    by_space: Counter[tuple[int, int, int]] = Counter()
    # What a fork family would additionally share: the same words at the same
    # PC, whatever address space observed them.
    by_code: Counter[tuple[int, int]] = Counter()
    lengths: Counter[int] = Counter()
    body_words = 0
    for group in programs:
        for program in group:
            by_space[(program.generation, program.pc, program.digest)] += 1
            by_code[(program.pc, program.digest)] += 1
            lengths[program.words] += 1
            body_words += program.words

    print("\nduplication")
    print(f"  distinct within an address space "
          f"{len(by_space):>9,}   factor {resident / len(by_space):.2f}x")
    print(f"  distinct ignoring address space   "
          f"{len(by_code):>9,}   factor {resident / len(by_code):.2f}x")
    shared_only = sum(1 for count in by_space.values() if count > 1)
    print(f"  bodies held by more than one executor "
          f"{shared_only:,} of {len(by_space):,} "
          f"({100.0 * shared_only / len(by_space):.1f}%)")

    print("\nstream length, in words")
    print(f"  mean {body_words / resident:.1f}   "
          f"median {sorted(lengths.elements())[resident // 2]}")
    for line in histogram(lengths, [4, 8, 16, 24, 32, 48, 64, 96]):
        print(line)
    inline = sum(n for length, n in lengths.items() if length <= 16)
    print(f"  fits a 16-word inline stream: {100.0 * inline / resident:.1f}%")

    # The committed slot array exists whether or not its ways are occupied,
    # so it is the part of the cache that sharing bodies cannot reduce. A
    # slab also holds what else each way needs (x86-64's successor sets),
    # and the allocator rounds it.
    tags = 0
    for executor in executors:
        slot_bytes = (executor.slot_bytes if executor.slot_bytes is not None
                      else arguments.slot_bytes)
        if executor.slab_count is not None:
            tags += (executor.slab_count * (executor.slab_slots or 256) *
                     slot_bytes)
        else:
            tags += arguments.slots * slot_bytes
    slabs = sum(e.footprint.get("slabs", 0) for e in executors)
    print("\nwhere sharing bodies would leave the cache")
    print(f"  slot tags                           {tags / 2**20:7.1f} MiB")
    if slabs > tags:
        print(f"  the rest of the committed slabs     "
              f"{(slabs - tags) / 2**20:7.1f} MiB")
    index = sum(e.footprint.get("stream_index", 0) for e in executors)
    print(f"  stream lookup maps                  {index / 2**20:7.1f} MiB")
    targets = sum(e.footprint.get("targets", 0) for e in executors)
    if targets:
        print(f"  target link tables                  "
              f"{targets / 2**20:7.1f} MiB")
    bodies = sum(e.footprint.get("streams", 0) for e in executors)
    decoded = sum(e.footprint.get("decoded", 0) for e in executors)
    if decoded:
        print(f"  instructions kept by a replay       "
              f"{decoded / 2**20:7.1f} MiB")
    links = sum(e.footprint.get("links", 0) +
                e.footprint.get("indirect", 0) for e in executors)
    print(f"  bodies and streams                  {bodies / 2**20:7.1f} MiB"
          f"  -> {bodies / 2**20 * len(by_space) / resident:.1f} MiB shared")
    print(f"  link and indirect records           {links / 2**20:7.1f} MiB"
          f"  -> {links / 2**20 / max(len(executors), 1):.1f} MiB shared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
