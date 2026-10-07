#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive the native SVE oracle on this host.

tools/arm64-tests/sve_native_oracle.cpp records the non-streaming SVE corpus
expectations on an arm64 Linux host with SVE at a 16-byte vector length,
which this host need not be. Its signal path, register transfer, and region
handling are the same for every case, so on any arm64 host this test runs a
request without SVE state through them: a completed case, an undefined
instruction, a breakpoint, read and write faults with their addresses and
syndromes, NZCV, SP, and SIMD&FP results, an initial FPSR, and a store's
region runs. On an SVE host at VL 16 it also runs cases with SVE state: the
live bit and the sve_context size after SVE, general-register, and AdvSIMD
instructions from either state, P and FFR into and out of a case, and a
first-fault load that clears FFR at an unmapped page.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sve-native-corpus.py"

FADD_SCALAR = 0x1E21_2802  # FADD S2, S0, S1
ONE = struct.pack("<I", 0x3F80_0000)  # 1.0
TINY = struct.pack("<I", 0x3080_0000)  # 2^-30: 1.0 + TINY is inexact
DZC, IXC = 0x02, 0x10
PTRUE_P0 = 0x2518_E3E0  # PTRUE P0.B
RDFFR_P1 = 0x2519_F001  # RDFFR P1.B
ORR_P2 = 0x2583_4C62  # ORR P2.B, P3/Z, P3.B, P3.B (MOV P2.B, P3.B)
LDFF1B = 0xA41F_6020  # LDFF1B {Z0.B}, P0/Z, [X1]
ADD_X = 0x8B02_0020  # ADD X0, X1, X2
ADD_V = 0x4EA2_8420  # ADD V0.4S, V1.4S, V2.4S


def run_oracle(oracle: str, native, cases: list, vl: int):
    with tempfile.TemporaryDirectory(prefix="sve-oracle-") as scratch:
        request = pathlib.Path(scratch) / "request"
        response = pathlib.Path(scratch) / "response"
        request.write_bytes(native.write_request(cases, vl))
        subprocess.run([oracle, str(request), str(response)], check=True,
                       timeout=60)
        return native.read_response(response.read_bytes(), cases, vl)


def sve_failures(oracle: str, native) -> list[str]:
    """Cases with SVE state: (case, event, pc offset, live, sve_context
    size, P and FFR changes)."""
    State, Region, Case = native.WireState, native.WireRegion, native.WireCase
    svc, data, page = native.SVC, native.DATA, native.SMALL_PAGE
    rw = native.REGION_READABLE | native.REGION_WRITABLE
    ones = b"\xff\xff"
    filled = {i: native.P_FILL[i] for i in range(16)}

    def live(**fields) -> State:
        return State(live=1, p={**filled, **fields.pop("p", {})},
                     ffr=fields.pop("ffr", native.FFR_FILL), **fields)

    header, registers = native.SVE_HEADER_SIZE, native.SVE_LIVE_SIZE
    checks = [
        # The first SVE instruction makes the state live with P and FFR
        # zero, so only P0 changes.
        (Case((PTRUE_P0, svc), 0, State()),
         "system-call", 4, 1, registers, {0: ones}, None),
        (Case((PTRUE_P0, svc), 0, live()),
         "system-call", 4, 1, registers, {0: ones}, None),
        (Case((ADD_X, svc), 0, State()),
         "system-call", 4, 0, header, {}, None),
        (Case((ADD_V, svc), 0, State()),
         "system-call", 4, 0, header, {}, None),
        (Case((ADD_V, svc), 0, live()),
         "system-call", 4, 1, registers, {}, None),
        (Case((RDFFR_P1, ORR_P2, svc), 0, live(p={3: b"\x5a\xa5"})),
         "system-call", 8, 1, registers,
         {1: native.FFR_FILL, 2: b"\x5a\xa5"}, None),
        # Five bytes below an unmapped page: FFR keeps elements 0-4.
        (Case((LDFF1B, svc), 0, live(x={1: data + page - 5}, p={0: ones},
                                     ffr=ones),
              (Region(data, page, rw, seed=1),)),
         "system-call", 4, 1, registers, {}, b"\x1f\x00"),
    ]
    _, results = run_oracle(oracle, native, [check[0] for check in checks],
                            native.VL)
    failures = []
    for index, ((case, event, pc, is_live, size, p, ffr), result) in \
            enumerate(zip(checks, results)):
        got = (result.event, result.pc_offset, result.state.live,
               result.sve_size, result.state.p, result.state.ffr)
        want = (event, pc, is_live, size, p, ffr)
        if got != want:
            failures.append(f"SVE case {index}: got {got}, expected {want}")
    return failures


def load_generator():
    spec = importlib.util.spec_from_file_location("sve_native_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_sve_native_oracle.py ORACLE")
    oracle = sys.argv[1]
    native = load_generator()
    State, Region, Case = native.WireState, native.WireRegion, native.WireCase
    data, svc = native.DATA, native.SVC
    page = native.PAGE
    rw = native.REGION_READABLE | native.REGION_WRITABLE

    def state(**fields) -> State:
        # Darwin clears X18 whenever a thread returns to user mode.
        x = fields.pop("x", {})
        return State(x={18: 0, **x}, **fields)

    # (case, event, pc offset, fault address, X changes)
    checks = [
        (Case((0x8B02_0020, svc), 0, state(x={1: 5, 2: 7})),  # ADD X0, X1, X2
         "system-call", 4, 0, {0: 12}),
        (Case((0x0000_0000, svc), 0, state()),  # UDF #0
         "illegal-instruction", 0, 0, {}),
        (Case((0xD503_201F, 0xD420_0020, svc), 0, state()),  # NOP; BRK #1
         "breakpoint", 4, 0, {}),
        (Case((0xF940_0020, svc), 0, state(x={1: data + page}),
              (Region(data, page, rw, seed=1),)),  # LDR X0, [X1]
         "memory-fault", 0, data + page, {}),
        (Case((0xF900_0020, svc), 0, state(x={1: data + 8}),
              (Region(data, page, native.REGION_READABLE, seed=1),)),
         "memory-fault", 0, data + 8, {}),  # STR X0, [X1], read-only
        (Case((0xF900_0020, svc), 0,
              state(x={0: 0x1122_3344_5566_7788, 1: data + 8}),
              (Region(data, page, rw, seed=1),)),
         "system-call", 4, 0, {}),
        (Case((0xAB02_0020, svc), 0,  # ADDS X0, X1, X2
              state(x={1: 0x7FFF_FFFF_FFFF_FFFF, 2: 1}, nzcv=0x2000_0000)),
         "system-call", 4, 0, {0: 0x8000_0000_0000_0000}),
        (Case((0x9100_03E0, svc), 0, state(x={31: data + 0x10})),
         "system-call", 4, 0, {0: data + 0x10}),  # ADD X0, SP, #0
        (Case((0x4E22_D420, svc), 0, state()),  # FADD V0.4S, V1.4S, V2.4S
         "system-call", 4, 0, {}),
        (Case((0xD503_201F, svc, svc), 4, state()),  # entry past a NOP
         "system-call", 4, 0, {}),
        (Case((FADD_SCALAR, svc), 0,
              state(fpsr=DZC, v={0: ONE + bytes(12), 1: TINY + bytes(12)})),
         "system-call", 4, 0, {}),
    ]
    cases = [check[0] for check in checks]
    host, results = run_oracle(oracle, native, cases, 0)
    failures = []
    for index, ((case, event, pc, fault, x), result) in enumerate(
            zip(checks, results)):
        got = (result.event, result.pc_offset, result.fault_address,
               dict(result.state.x))
        want = (event, pc, fault, x)
        if got != want:
            failures.append(f"case {index}: got {got}, expected {want}")
    if results[6].state.nzcv != 0x9000_0000:
        failures.append(f"ADDS: NZCV {results[6].state.nzcv:#x}")
    if results[4].esr >> 6 & 1 != 1 or results[3].esr >> 6 & 1 != 0:
        failures.append("fault syndromes do not give the access")
    if results[5].runs != (((8, bytes.fromhex("8877665544332211")),),):
        failures.append(f"STR: runs {results[5].runs}")
    if set(results[8].state.v) != {0} or results[8].state.fpsr == 0:
        failures.append("FADD: V0 and FPSR.IXC were not reported")
    if results[10].state.fpsr != DZC | IXC:
        failures.append(
            f"FADD from FPSR.DZC: FPSR {results[10].state.fpsr:#x}")
    sve = host["vl"] == native.VL
    if sve:
        failures += sve_failures(oracle, native)
    if failures:
        raise SystemExit("sve_native_oracle: " + "; ".join(failures))
    vector = f"VL {host['vl']}" if sve else "no SVE"
    print(f"sve_native_oracle: {len(cases)} base A64 cases"
          f"{' and 7 SVE cases' if sve else ''} on a host with {vector} and "
          f"{host['page']}-byte pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
