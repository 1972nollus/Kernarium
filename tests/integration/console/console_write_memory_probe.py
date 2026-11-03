#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-console-write-memory: console writes with allocations traced.

Usage: console_write_memory_probe.py RECORDS

Traces slab and non-sleeping page allocations with their stacks while it
writes RECORDS numbered 64-byte lines to /dev/hvc0, in raw mode, and then to
/dev/ttyS1, with the default output processing that turns each newline into
a carriage return and a newline. The host reads hvc0 as it arrives and holds
ttyS1's channel unread for a while, so that line's output ring fills and the
writer waits. An allocation is a console write's when its stack holds the
Virtio console's put_chars() or the UML line's line_write().

Prints one line per result:
  CONSOLE_WRITES hvc=BYTES line=BYTES line_seconds=S
  CONSOLE_ALLOCATIONS events=N hvc=N line=N lost=N
  CONSOLE_STACK <frames>     (for at most three console write allocations)
"""

import os
import re
import subprocess
import sys
import termios
import time

TRACING = "/sys/kernel/tracing"
RECORD = 64
# __GFP_DIRECT_RECLAIM: a page allocation that may not sleep lacks it, as
# every allocation a console write makes with its lock held must.
DIRECT_RECLAIM = 0x400
HVC_FRAME = re.compile(r"^put_chars\b")
LINE_FRAME = re.compile(r"^line_write\b")
EVENT = re.compile(r"^\s*\S+-\d+\s+\[\d+\]\s+\S+\s+[\d.]+:\s+(\w+):")


def record(index: int) -> bytes:
    body = f"R{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 2
    return (body[:RECORD - 1] + "\n").encode()


def write(path: str, text: str) -> None:
    with open(path, "w") as handle:
        handle.write(text)


def start_tracing() -> None:
    subprocess.run(
        f"mountpoint -q {TRACING} || mount -t tracefs nodev {TRACING}",
        shell=True,
        check=True,
    )
    write(f"{TRACING}/tracing_on", "0")
    write(f"{TRACING}/trace", "")
    write(f"{TRACING}/buffer_size_kb", "8192")
    for event in ("kmalloc", "kmem_cache_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "1")
    page_alloc = f"{TRACING}/events/kmem/mm_page_alloc"
    write(f"{page_alloc}/filter", f"!(gfp_flags & {DIRECT_RECLAIM:#x})")
    write(f"{page_alloc}/enable", "1")
    write(f"{TRACING}/options/stacktrace", "1")
    write(f"{TRACING}/tracing_on", "1")


def lost_events() -> int:
    lost = 0
    for cpu in os.listdir(f"{TRACING}/per_cpu"):
        with open(f"{TRACING}/per_cpu/{cpu}/stats") as stats:
            for line in stats:
                name, _, value = line.partition(":")
                if name.strip() in ("overrun", "dropped events"):
                    lost += int(value.split()[0])
    return lost


def allocations() -> tuple[list[list[str]], int]:
    write(f"{TRACING}/tracing_on", "0")
    lost = lost_events()
    events: list[list[str]] = []
    with open(f"{TRACING}/trace", errors="replace") as trace:
        for line in trace:
            if EVENT.match(line):
                events.append([])
            elif events and "=>" in line:
                events[-1].append(line.split("=>", 1)[1].strip())
    for event in ("kmalloc", "kmem_cache_alloc", "mm_page_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "0")
    write(f"{TRACING}/options/stacktrace", "0")
    return events, lost


def write_all(device: str, data: bytes, raw: bool) -> tuple[int, float]:
    descriptor = os.open(device, os.O_WRONLY | os.O_NOCTTY)
    if raw:
        attributes = termios.tcgetattr(descriptor)
        attributes[1] &= ~termios.OPOST
        termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
    started = time.monotonic()
    written = 0
    view = memoryview(data)
    while written < len(data):
        count = os.write(descriptor, view[written:written + 4096])
        if count <= 0:
            break
        written += count
    termios.tcdrain(descriptor)
    elapsed = time.monotonic() - started
    os.close(descriptor)
    return written, elapsed


def main() -> int:
    records = int(sys.argv[1])
    data = b"".join(record(index) for index in range(records))
    start_tracing()
    hvc, _ = write_all("/dev/hvc0", data, raw=True)
    print("CONSOLE_LINE_START", flush=True)
    line, line_seconds = write_all("/dev/ttyS1", data, raw=False)
    events, lost = allocations()
    hvc_stacks = [s for s in events if any(HVC_FRAME.match(f) for f in s)]
    line_stacks = [s for s in events if any(LINE_FRAME.match(f) for f in s)]
    print(f"CONSOLE_WRITES hvc={hvc} line={line} line_seconds={line_seconds:.2f}")
    print(
        f"CONSOLE_ALLOCATIONS events={len(events)} hvc={len(hvc_stacks)} "
        f"line={len(line_stacks)} lost={lost}"
    )
    for stack in (hvc_stacks + line_stacks)[:3]:
        print("CONSOLE_STACK " + " ".join(stack[:12]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
