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
pack and proves each case consistent with the decoder and with itself. An
engine lane includes the reader and case layout,
[`../aarch32_corpus_pack.hpp`](../aarch32_corpus_pack.hpp), and the state
conversion,
[`../aarch32_corpus_cpu_state.hpp`](../aarch32_corpus_cpu_state.hpp), runs each
case and compares what the pack records; the inventory's `engine_status` moves
from `pending` to `verified` per entry. A lane starts every case on a virtual
CPU that has run 64-bit code: the general-register slots that hold no AArch32
register keep values with both halves set, as an AArch64 task leaves x15-x30
and SP. A fault address wider than 32 bits, or a register slot left with its
upper half set, fails the case.

## A case

A program is A32 words or Thumb halfwords placed at `0x4f000000` plus the
case's origin (0 unless it tests instruction fetch near a line), entered in the
instruction set the case names. The memory map is the oracle's:

| Address | Size | Contents |
| --- | --- | --- |
| `0x4f000000` | 16 KiB | the program, read-write-execute |
| `0x4f000800` | | exit stub that finishes in A32 state |
| `0x4f000900` | | exit stub that finishes in Thumb state |
| `0x4f003f00`, `0x4f003f08` | | the oracle's trampolines to its epilogues: the words after the stubs' loads |
| `0x50000000` | 32 KiB | data, read-write, refilled before each case |
| `0x50008000` | 16 KiB | read-only data |

Everything else is unmapped. A stub is `ldr pc, [pc, #-4]` (Thumb:
`ldr.w pc, [pc]`) followed by `0x4f003f00` (A32) or `0x4f003f08` (Thumb), the
same at the end of a program as at `0x4f000800` and `0x4f000900`, so a program
that loads from a stub reads words every oracle shares; an engine lane puts
its own exit code at the two trampolines. A program ends by falling off its
last instruction into a stub, by branching to one (a program that branches ends in
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
| `o` | byte offset of the program from `0x4f000000` (default 0) |
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
| `pi4` | a Raspberry Pi 4 Model B (Cortex-A72, Armv8.0) running the oracle natively under its 64-bit Linux, twice per case (decision D3) |
| `pi5` | a Raspberry Pi 5 (Cortex-A76), when recorded |

The suites, in the order the corpus grows. The base level (A32 and Thumb-2,
no floating point or SIMD):

- `alu`: data processing, multiplies, bit fields, extension, saturation,
  packing and parallel arithmetic;
- `memory`: single and multiple loads and stores at aligned and misaligned
  addresses, across 4 KiB and 16 KiB lines, at the ends of the data region, in
  the read-only region and in unmapped memory, with writeback; and LDM, STM,
  PUSH and POP of two, six, seven and the most registers under a condition
  that fails and passes, within a page and across one;
- `prefetch`: PLD, PLDW and PLI never fault;
- `positions`: single-register and doubleword transfers, VLDR, VSTR, TBB and
  TBH that fault after other instructions, some in a block a branch reaches:
  unmapped, crossing from the read-only region into unmapped memory, and a
  literal below the code region, the unmapped and literal ones also under a
  condition that fails and passes, so an engine names the faulting
  instruction by its place in the block; then LDM, STM, PUSH and POP,
  unmapped and crossing from unmapped memory into the data region, and an
  A32 STM that stores the PC, unmapped and where it stores, within a page
  and across one, each also under a condition that fails and passes;
- `exclusive`: LDREX/STREX and LDAEX/STLEX sequences, CLREX, misaligned and
  unmapped loads;
- `exclusive-spill`: the same pairs with a register spill (STR or STRD to the
  stack) between the load and the store, which an engine whose monitor a
  store clears fails; the lanes that interrupt the guest leave it out;
- `branch`: B, BL, BLX, BX, CBZ/CBNZ, TBB/TBH (also in an IT block whose
  condition passes and fails), PC-writing loads (the multiple ones also under
  a condition that fails and passes) and ALU instructions, interworking both
  ways, jumps to unmapped addresses;
- `conditions`: every condition against every NZCV, every IT block, and
  runs of one to four conditional instructions that fail and pass, under
  every condition;
- `pc`: instructions that read the PC, among them the PC as a register
  operand under a condition that fails and passes and as the operand of an
  ADD or SUB whose other operand is its destination;
- `system`: barriers, hints, CPS, the APSR instructions, TPIDRURO, TPIDRURW,
  the CP15 barriers, BKPT and the encodings Linux turns into SIGTRAP;
- `absent`: UNDEFINED, UNPREDICTABLE, unallocated and absent-feature
  encodings raise SIGILL, SWP and SWPB under every condition among them
  (the guest kernel emulates them behind `abi.swp`); SVC raises the
  system-call event.

On a boundary (any level):

- `fetch`: every Thumb-2 form once with its two halfwords on either side of
  the 4 KiB line at `0x4f001000` (origin `0xffe`); a 32-bit instruction whose
  second halfword is past the end of the 16 KiB code region (SIGSEGV at
  `0x4f004000`, pc at the instruction, none retired); and programs that run
  to the end of the region and fall off it (SIGSEGV, pc and fault address
  `0x4f004000`);
- `straddle`: STM, PUSH, VSTM and FSTMX whose block crosses from the data
  region into the read-only region. The words ahead of the faulting one are
  stored, and the base register is as before.

Sequences an engine may translate together (any level):

- `sequences`: an ADD or SUB of an immediate that computes the base of a
  VLD1 or VST1, next to it or with Advanced SIMD data processing between, at
  the memory suite's addresses and one that wraps around 2^32, and the shapes
  that keep the two apart (flags set, a core register read, VFP or another
  ADD between, another base, writeback, a hint, a PC base); VCMP and VCMPE
  of each precision followed by VMRS APSR_nzcv, and apart, to a core
  register or conditional; and the conditional data-processing instructions
  that set no flags, MOVW and VMOV under conditions that pass and fail,
  alone and in runs with conditional loads and stores, in A32 and IT blocks.
  A copy that faults does so with the instructions before it retired.

The `simd` level (VFPv4-D32 and Advanced SIMD):

- `simd`: every register form, each word started in D registers of
  floating-point edge values (zeros, infinities, NaNs, denormals, extremes),
  of integer lane limits (8, 16 and 32 bits) or of random bits, under every
  rounding mode, flush-to-zero, default-NaN and alternative half-precision
  setting; the element-size and signedness variants come from the form's free
  fields;
- `simd-steps`: VRECPS and VRSQRTS with each infinity against zeros,
  denormals and one in half and single precision, which the `simd` suite's
  sampled states do not pair (FZ16 stays clear, so a half-precision denormal
  is a nonzero number);
- `simd-convert`: VCVT and VCVTR between single or double precision and
  32-bit integers, with fractions at and either side of a half, the largest
  values below the integer limits and the first past them, a denormal, a NaN
  and an infinity, under each rounding mode and flush-to-zero, and integers
  that a single cannot hold, which the `simd` suite's edge values leave out;
- `simd-memory`: VLDR, VSTR, VLDM, VSTM, FLDMX, FSTMX and the structure loads
  and stores VLD1-4 and VST1-4, aimed at the memory suite's addresses;
- `simd-hints`: VLD1 and VST1 copies with each alignment hint and none, and
  VLD1 to all lanes with and without one, at addresses the hint accepts and
  refuses (the alignment fault comes before an unmapped page's), across the
  4 KiB and 16 KiB lines, with each writeback;
- `fpscr`: VMRS and VMSR, including the exception trap enables the profile
  reads as zero and ignores writes to, and FZ16, which holds what is written
  to it wherever FEAT_FP16 is.

The `complete` level (the Armv8 additions):

- `armv8`: CRC32, AES, SHA-1, SHA-2, VRINT*, VCVT[ANPM], VSEL, VMAXNM,
  VMINNM, the half-precision arithmetic, VCADD, VCMLA, VQRDMLAH, VQRDMLSH,
  VJCVT and the dot-product, matrix-multiply and half-to-single
  multiply-accumulate forms, in the simd suite's states plus half-precision
  lanes;
- `armv8-memory`: the half-precision VLDR and VSTR;
- `armv8-system`: SB;
- `armv8-fz16`: VFMAL and VFMSL, vector and by scalar, D and Q, with
  FPSCR.FZ16 set and clear and half-precision denormals, infinities and NaNs
  among the operands, which the `armv8` suite's states leave unpaired;
- `armv8-convert16`: the Advanced SIMD conversions from half precision to
  16-bit integers (VCVT, VCVTA, VCVTN, VCVTP, VCVTM) and 16-bit fixed point,
  D and Q, with FZ16 set and clear and lanes that overflow after lanes that
  are inexact, so that one lane's saturation leaves another's flags.

LDA, STL, LDAEX, STLEX in every size and SEVL are base: every Armv8 AArch32
EL0 has them, and arm64 Linux's compat kuser helpers execute STLEX and STLEXD.
They are in the `memory`, `exclusive` and `system` suites.

The oracle's store-exclusive always succeeds when its load-exclusive ran
first, and a case is one attempt. An engine whose exclusives are the host's
can fail one without cause, as the architecture allows: the host's monitor
ends at any interrupt between the load and the store. A lane therefore runs a
failing store-exclusive case again, a bounded number of times: when the only
difference is a status of 1 where the pack has 0 and the store that did not
happen, up to 16 attempts in all, in the gadget lane and the JIT lane alike.

Three suites are recorded only from hardware: `unaligned` (172 cases:
multi-word accesses at misaligned addresses), `positions-unaligned` (54
cases: the `positions` transfers at a misaligned address) and
`fpscr-reserved` (144 cases: VMRS and VMSR with Stride and Len set, which an
implementation may or may not hold). The producer refuses to record them as
`qemu`, and nothing derives them from rules. All are `pi4` records. The
Cortex-A72 reads Stride and Len as zero and ignores writes to them, as the
profile does, and a misaligned multi-word access raises SIGBUS at its first
address.

## What the oracle's answers rest on

- Only the signal number, the fault address and the registers and memory
  after the program are golden; `si_code` differs between qemu and Linux.
- `MRS Rd, APSR` returns the APSR flags with `0x10` (user mode) in the low
  bits.
- Linux turns the A32 `0xe7f001f0`, T16 `0xde01` and T32 `0xf7f0a000`
  undefined-instruction encodings into SIGTRAP; every other UDF is SIGILL.
- A misaligned multi-word access (LDM, STM, LDRD, STRD, PUSH, POP) is an
  alignment fault, SIGBUS at its first address. An arm64 kernel fixes it up
  for a compat task only with `CONFIG_COMPAT_ALIGNMENT_FIXUPS`; the profile
  has no fixup, and neither has the `pi4` oracle's kernel. qemu-arm agrees
  with both, but the `unaligned` suite stays a hardware record.
- qemu-arm clears the FPSCR exception trap enables on a write (the profile's
  read-as-zero, write-ignored) and keeps Stride, Len and FZ16. Stride and Len
  are the `fpscr-reserved` suite's, so the `qemu` records never set them.
  FZ16 is FEAT_FP16's and holds wherever that is, as in qemu-arm's `-cpu
  max`; the `fpscr`, `armv8-fz16` and `armv8-convert16` suites write it, and
  every other suite starts with it clear, because an oracle without FEAT_FP16
  reads it as zero.
- CNTFRQ and CNTVCT depend on the host and are not in the corpus.

## The hardware cross-check

`compare` of every `qemu` record against the `pi4` oracle's observations of
the same cases agrees on every case, including the fault addresses of the
`fetch` and `straddle` suites, except for two kinds. The first is the cases
whose instruction needs a feature the Cortex-A72 lacks; they raise SIGILL
there and `--absent` leaves them out. Those features are FEAT_FP16
(including the half-precision variants of the VFP and Advanced SIMD forms),
the cryptographic extensions with PMULL, the dot product, RDM, FCMA, JSCVT,
FHM, BF16, I8MM and SB. The second is the differences that
[`known-differences.tsv`](known-differences.tsv) lists. There the
architecture, Linux or the A72's missing features leave the answer open,
and the corpus keeps qemu-arm's, which is the profile's:

- A store multiple that faults part-way (`straddle`): the architecture
  leaves the memory it wrote UNKNOWN. The profile stores the words ahead of
  the faulting one; the A72 stores none.
- VTRN.32 with Dd = Dm (Qd = Qm): the result is ARBITRARY. The profile swaps
  the two words; the A72 copies the even word into both. The 8- and 16-bit
  forms, and VSWP, VUZP and VZIP with Dd = Dm, are ARBITRARY too, and there
  the A72 agrees with the profile. So does an STM that stores its
  written-back base after the first register: the stored value is the
  original base.
- An LDRD whose two words both fault, in two pages (at `0xfffffffc` and
  `0`): the profile reports the first word's address; the A72 reports
  either, varying from one run to the next. Where both words are in one
  page, and for an STRD, it reports the first.
- WFI: arm64 Linux leaves SCTLR_EL1.nTWI clear, so WFI at EL0 traps and the
  program gets SIGILL, as the `pi4` records show. The profile completes WFI
  at once, as it does WFE
  ([`docs/architecture.md`](../../../../docs/architecture.md)), so the six
  WFI cases of `system` differ from `pi4` by design.
- FZ16 in the `fpscr` suite reads as zero on the A72, which lacks FEAT_FP16.

`observe` runs each case twice, and the two runs must agree, except in the
fields `known-differences.tsv` lists for the oracle and the case, where the
answer is open and the first run's is kept. Any other difference fails it
and names the cases.

## Regeneration

```sh
python3 tools/arm64-tests/a32-corpus.py observe --archive ISA_AArch32_xml_A_profile-2026-06.tar.gz \
  --suite alu --host HOST --runner 'qemu-arm -cpu max'
python3 tools/arm64-tests/a32-corpus.py finish
```

`observe` plans a suite from the decoder's table (it needs the pinned Arm XML
archive; the planner only reads it, and no pack or build is involved), builds
the freestanding armhf oracle (`tools/arm64-tests/a32_oracle.cpp`) with the
pinned clang and lld, runs it in a private directory on the host and removes
the directory. `finish` regenerates `provenance.json`, `manifest.tsv`,
`coverage.tsv` (cases per inventory entry) and `excluded.tsv` (entries with no
case). Commit the source tables and these files, never the pack.

`check --archive ...` plans every suite again without an oracle and requires
each source table to hold exactly the planned cases, in order (the opt-in
`arm64-a32-corpus-plan-freshness` test runs it). A change to the forms or the
planner that moves a case fails it until the suite is observed again.

Hardware records use the same producer without `--runner`, so the harness runs
natively on an aarch64 Linux host whose cores run AArch32 at EL0 (a Raspberry
Pi 4 or 5 with a 64-bit kernel); `--host-directory` keeps its private
directory inside one the host's owner set aside. `compare` then lists where
another oracle's observations of the same cases differ from the checked-in
ones. It leaves out the cases that need a feature the hardware lacks, and it
counts the differences `known-differences.tsv` explains apart; only the
others fail it:

```sh
python3 tools/arm64-tests/a32-corpus.py observe --archive ISA_AArch32_xml_A_profile-2026-06.tar.gz \
  --suite alu --host HOST --oracle pi5 --host-directory DIR --source /tmp/a32-pi5
python3 tools/arm64-tests/a32-corpus.py compare --right /tmp/a32-pi5 \
  --archive ISA_AArch32_xml_A_profile-2026-06.tar.gz --absent bf16,i8mm,fhm,sb,fcma,jscvt
```
