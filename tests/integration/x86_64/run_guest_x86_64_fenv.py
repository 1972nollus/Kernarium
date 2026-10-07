#!/usr/bin/env python3
"""Check what glibc shows of x86's sticky floating-point exception flags.

Boots an x86-64 image with a C++ toolchain and glibc, such as the x86-64 LTP
rootfs, on an emulator built with the macos-x86_64 preset, builds
fenv_probe.cpp there with g++, and runs it. With every exception masked, 29
operations -- SSE arithmetic and conversions, comparisons, libm calls,
feraiseexcept, the rounding functions, x87 arithmetic, an FTZ flush and a
DAZ input -- must leave what fetestexcept() and STMXCSR showed on an Intel
Xeon 8581C, and glibc's fadd, which reads the precision flag to round to
odd, must return the Xeon's value. The documented exception is DE while DAZ
is clear, which no host flag shows (user decision 2026-10-07). `--pending`
is for an engine that does not keep masked flags yet: it may leave flags
out, and fails once it leaves none out.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("fenv_probe.cpp")
GUEST_SOURCE = "/usr/local/libexec/uml-test/fenv_probe.cpp"
GUEST_BINARY = "/tmp/fenv-probe"
BUILD = (f"g++ -std=c++20 -O2 -Wall -Wextra -Werror -o {GUEST_BINARY} "
         f"{GUEST_SOURCE}")
PROBE_TIMEOUT_SECONDS = 900
DENORMAL_OPERAND = 0x02
FLAG_BITS = 0x3F

# The probe built with g++ 14.2 -O2 against glibc 2.41 on Debian 13, run on
# an Intel Xeon Platinum 8581C: operation, fetestexcept(), MXCSR.
XEON = """\
div0 ZE 1f84
inv IE 1f81
sqrtneg IE 1f81
ovf OE,PE 1fa8
unf UE,PE 1fb0
inexact PE 1fa0
exact - 1f80
denorm - 1f82
cvtnan IE 1f81
cvtbig IE 1f81
cmpnan IE 1f81
eqnan - 1f80
fltovf OE,PE 1fa8
rcp - 1f80
log0 ZE 1f84
expbig OE,PE 1fa8
expsmall UE,PE 1fb0
powneg IE 1f81
sinbig PE 1fa0
raisediv ZE 1f84
raiseinv IE 1f81
raiseovf OE 1f80
nearbyint - 1f80
lrint PE 1fa0
rint PE 1fa0
x87div PE 1f80
x87sin PE 1fa0
ftzflush UE,PE 1fb0
dazdenorm - 1f80
"""
XEON_NARROW = 0x3F800001
# fadd's value when the sum's precision flag is lost: rounded toward zero.
NARROW_WITHOUT_PRECISION = 0x3F800000


def expected() -> dict[str, tuple[set[str], int]]:
    cases = {}
    for line in XEON.splitlines():
        name, flags, mxcsr = line.split()
        cases[name] = (set() if flags == "-" else set(flags.split(",")),
                       int(mxcsr, 16))
    return cases


def parse(output: str) -> tuple[dict[str, tuple[set[str], int]], int | None]:
    cases = {}
    narrow = None
    for line in output.splitlines():
        fields = line.split()
        if fields[:3] == ["fenv", "narrow", "fadd"] and len(fields) == 4:
            narrow = int(fields[3], 16)
        elif (len(fields) >= 5 and fields[0] == "fenv" and
              fields[2] == "fetestexcept" and fields[-2] == "mxcsr"):
            flags = set(fields[3:-2]) - {"-"}
            cases[fields[1]] = (flags, int(fields[-1], 16))
    return cases, narrow


def compare(actual: tuple[set[str], int], want: tuple[set[str], int],
            pending: bool) -> str:
    """'ok', 'residual', 'pending' or 'FAIL'."""
    flags, mxcsr = actual
    want_flags, want_mxcsr = want
    if flags == want_flags and mxcsr == want_mxcsr:
        return "ok"
    missing_only = flags <= want_flags and \
        (mxcsr & ~want_mxcsr & FLAG_BITS) == 0 and \
        (mxcsr & ~FLAG_BITS) == (want_mxcsr & ~FLAG_BITS)
    # DE, which fetestexcept cannot show, while DAZ (bit 6) is clear.
    if flags == want_flags and mxcsr | DENORMAL_OPERAND == want_mxcsr and \
            want_mxcsr & 0x40 == 0:
        return "residual"
    if pending and missing_only:
        return "pending"
    return "FAIL"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path,
                        help="x86-64 image with g++ and glibc")
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    parser.add_argument("--pending", action="store_true",
                        help="the engine does not keep masked flags yet")
    parser.add_argument("--timeout", type=int, default=180)
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {rootfs}")
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")

    with Guest(
        emulator,
        rootfs,
        "1G",
        "tmpfs",
        init_shell=True,
        engine=arguments.engine,
        payloads={GUEST_SOURCE: GuestFile(PROBE.read_bytes(), 0o644)},
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(
            f"{BUILD} && {GUEST_BINARY} flags && {GUEST_BINARY} narrow",
            PROBE_TIMEOUT_SECONDS,
        )
    print(result.output.strip())
    if result.return_code != 0:
        raise RuntimeError(f"the probe failed with status {result.return_code}")
    cases, narrow = parse(result.output)
    failures = 0
    pending_used = False
    for name, want in expected().items():
        if name not in cases:
            print(f"FAIL {name}: no result")
            failures += 1
            continue
        status = compare(cases[name], want, arguments.pending)
        pending_used |= status == "pending"
        if status != "ok":
            flags, mxcsr = cases[name]
            print(f"{status} {name}: fetestexcept {sorted(flags)} mxcsr "
                  f"{mxcsr:04x}, x86 {sorted(want[0])} {want[1]:04x}")
        failures += status == "FAIL"
    if narrow != XEON_NARROW:
        if arguments.pending and narrow == NARROW_WITHOUT_PRECISION:
            pending_used = True
            print(f"pending narrow: fadd {narrow:08x}, x86 {XEON_NARROW:08x}")
        else:
            print(f"FAIL narrow: fadd {narrow if narrow is None else hex(narrow)}"
                  f", x86 {XEON_NARROW:08x}")
            failures += 1
    if arguments.pending and not pending_used:
        print("FAIL the engine keeps every flag: drop --pending")
        failures += 1
    if failures:
        raise RuntimeError(f"{failures} result(s) differ from the Xeon's")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
