# AArch32 instruction corpus

`a32_instruction_corpus.pack` is the build-generated, self-contained input of
the `arm64-aarch32-corpus` CTest. A case is one program of the AArch32
profile (Armv8-A AArch32 at EL0, see
[`../../../../docs/a32-instruction-coverage.md`](../../../../docs/a32-instruction-coverage.md)),
the state it starts in, and what an oracle observed it do. The checked-in
`source/` tables hold the programs, the starting states and the observations;
CMake's `uml_arm64_aarch32_corpus` target generates the pack from them and
verifies the SHA-256 in `provenance.json`
([source contract](../../../../tools/instruction-corpus/README.md)). The
runner,
[`../aarch32_corpus_runner.cpp`](../aarch32_corpus_runner.cpp), needs only the
pack. Until an engine runs AArch32 it proves each case consistent with the
decoder and with itself; an engine adds its lane to that runner and the
inventory's `engine_status` moves from `pending` to `verified` per entry.

## A case

A program is A32 words or Thumb halfwords placed at `0x4f000000`, entered in
the instruction set the case names. The memory map is the oracle's:

| Address | Size | Contents |
| --- | --- | --- |
| `0x4f000000` | 16 KiB | the program, read-write-execute |
| `0x4f000800` | | exit stub that finishes in A32 state |
| `0x4f000900` | | exit stub that finishes in Thumb state |
| `0x50000000` | 32 KiB | data, read-write, refilled before each case |
| `0x50008000` | 16 KiB | read-only data |

Everything else is unmapped. A program ends by falling off its last
instruction into a stub, by branching to one (a program that branches ends in
an explicit `b` or `b.w` to the stub of its instruction set), or by raising an
event. Every register is the test's: the oracle loads r0-r14, APSR, FPSCR,
TPIDRURO, TPIDRURW and D0-D31, runs the program, and records the same state
at the stub or at the fault, plus the data bytes the program changed. The
data fill is the `hash32` byte recipe (start 1, multiplier `0x9e3779b1`) or
zero, with sparse patches.

Starting states are the named recipes of `source/states.jsonl`: zero, edge
values rotated among r0-r12, patterns, and the same for D0-D31 with
floating-point edge values (zeros, ones, infinities, NaNs, denormals,
extremes); SP points into the data region and LR at the exit stub. A case
names a recipe and the registers it changes (`si`, which includes APSR flags
and FPSCR modes), so that a result's state is the recipe, the changes and the
observed delta (`x`).

### Source rows

Each `source/<suite>-<oracle>.jsonl` starts with a header (`record`,
`oracle`, `defaults`, `environment`) and then one object per case:

| Key | Meaning |
| --- | --- |
| `n`, `f` | case name and the inventory encoding of the instruction under test |
| `i`, `p` | `A` or `T`, and the program in hexadecimal (words, or halfwords) |
| `t` | ordinal of the instruction under test |
| `s`, `si` | recipe and changes to it |
| `d`, `dp` | data fill (default `hash32`) and data patches `[offset, bytes]` |
| `x` | registers that differ from the start when the program ends or faults |
| `e`, `fi`, `fa` | event (a signal number; 255 for a system call), halfword index of the instruction it was raised at, and the fault address |
| `k` | instructions retired before the event or in all (default 1) |
| `m` | data bytes the program changed, `[offset, bytes]` |
| `fl` | `s` the program is straight-line, `x` it ended in Thumb state |

An event stops the program with the state at the fault; a SIGILL's address is
the instruction's. A program with branches is not straight-line: its
retired count is the plan's, which the producer checks against the oracle's
final state. The pack's byte layout is `tools/instruction-corpus/aarch32.py`.

## Oracles and suites

A record holds one suite observed by one oracle and is named
`<suite>-<oracle>`.

| Oracle | Meaning |
| --- | --- |
| `qemu` | `qemu-arm -cpu max` on the build host, run twice per case; both runs must agree (decision D2) |
| `profile` | no oracle ran it: the event the profile defines |
| `pi4`, `pi5` | hardware (decision D3), when recorded |

The suites, in the order the corpus grows: `alu` (data processing,
multiplies, bit fields, extension, saturation, packing, parallel arithmetic),
`memory` (single and multiple loads and stores at aligned and misaligned
addresses, across 4 KiB and 16 KiB lines, at the ends of the data region, in
the read-only region and in unmapped memory, with writeback), `prefetch` (PLD,
PLDW and PLI never fault), `exclusive` (LDREX/STREX sequences, CLREX,
misaligned and unmapped loads), `branch` (B, BL, BLX, BX, CBZ/CBNZ, TBB/TBH,
PC-writing loads and ALU instructions, interworking both ways, jumps to
unmapped addresses), `conditions` (every condition against every NZCV, and
every IT block), `pc` (instructions that read the PC), `system` (barriers,
hints, CPS, the APSR instructions, TPIDRURO, TPIDRURW, the CP15 barriers, BKPT
and the encodings Linux turns into SIGTRAP) and `absent` (UNDEFINED,
UNPREDICTABLE, unallocated and absent-feature encodings raise SIGILL; SVC
raises the system-call event).

## What the oracle's answers rest on

- Only the signal number, the fault address and the registers and memory
  after the program are golden; `si_code` differs between qemu and Linux.
- `MRS Rd, APSR` returns the APSR flags with `0x10` (user mode) in the low
  bits.
- Linux turns the A32 `0xe7f001f0`, T16 `0xde01` and T32 `0xf7f0a000`
  undefined-instruction encodings into SIGTRAP; every other UDF is SIGILL.
- qemu-arm refuses misaligned multi-word accesses (LDM, STM, LDRD, STRD,
  PUSH, POP) with an alignment fault where an arm64 kernel fixes them up for a
  compat task. The `unaligned` suite is therefore recorded only from hardware
  (`pi4` or `pi5`); the producer refuses to record it as `qemu`.
- CNTFRQ and CNTVCT depend on the host and are not in the corpus.

## Regeneration

```sh
python3 tools/arm64-tests/a32-corpus.py observe --archive ISA_AArch32_xml_A_profile-2026-06.tar.gz \
  --suite alu --host arm64-builder --runner 'qemu-arm -cpu max'
python3 tools/arm64-tests/a32-corpus.py finish
```

`observe` plans a suite from the decoder's table (it needs the pinned Arm XML
archive; the planner only reads it, and no pack or build is involved), builds
the freestanding armhf oracle (`tools/arm64-tests/a32_oracle.cpp`) with the
pinned clang and lld, runs it in a private directory on the host and removes
the directory. `finish` regenerates `provenance.json`, `manifest.tsv`,
`coverage.tsv` (cases per inventory entry) and `excluded.tsv` (entries with no
case). Commit the source tables and these files, never the pack.
