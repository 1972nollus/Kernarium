#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a gadget cache census written by a UML_GADGET_CACHE_CENSUS build.

The census records what every executor's private cache still held when the run
ended: one `E` line per executor with its measured footprint, and one `P` line
per resident translation naming the address space, the guest PC, the stream
length and a digest of the guest words the translation consumed.

A translation is identified by the guest words it consumed rather than by the
stream it produced, because two executors translating the same code emit
equivalent streams that embed pointers to their own link records. That is what
makes the counts here answer how much of the cache is duplicate work: how much
would collapse if executors running one address space shared their bodies, and
how much more would collapse if a fork family's identical pages did too.
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
    footprint: dict[str, int] = field(default_factory=dict)


@dataclass
class Program:
    generation: int
    pc: int
    instructions: int
    words: int
    source_words: int
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
                source_words=int(fields[5]),
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
                        help="cache slots per executor, for occupancy (%(default)s)")
    parser.add_argument("--slot-bytes", type=int, default=72,
                        help="bytes per cache slot in the measured build (%(default)s)")
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
        print(f"  {executor.tag}  address spaces {executor.generations}  "
              f"resident {len(group):>7,}  "
              f"occupancy {100.0 * executor.occupancy / arguments.slots:5.1f}%  "
              f"cache {total / 2**20:7.1f} MiB")
    measured = sum(e.footprint.get("total", 0) for e in executors)
    print(f"  measured cache total        {measured / 2**20:.1f} MiB")

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

    # The slot array exists whether or not its ways are occupied, so it is the
    # part of the cache that sharing bodies cannot reduce.
    tags = len(executors) * arguments.slots * arguments.slot_bytes
    print("\nwhere sharing bodies would leave the cache")
    print(f"  slot tags, {len(executors)} x {arguments.slots:,} x "
          f"{arguments.slot_bytes} B   {tags / 2**20:7.1f} MiB")
    index = sum(e.footprint.get("stream_index", 0) for e in executors)
    print(f"  stream index                        {index / 2**20:7.1f} MiB")
    bodies = sum(e.footprint.get("streams", 0) for e in executors)
    links = sum(e.footprint.get("links", 0) +
                e.footprint.get("indirect", 0) for e in executors)
    print(f"  bodies and streams                  {bodies / 2**20:7.1f} MiB"
          f"  -> {bodies / 2**20 * len(by_space) / resident:.1f} MiB shared")
    print(f"  link and indirect records           {links / 2**20:7.1f} MiB"
          f"  -> {links / 2**20 / max(len(executors), 1):.1f} MiB shared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
