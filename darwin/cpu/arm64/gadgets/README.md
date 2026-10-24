# arm64 gadget engine

The engine executes A64, A32 and T32 guest code as data-only streams of
pointers to handwritten gadgets (`engine.S` and `engine/*.inc`, with the
streaming-mode and AArch32 gadgets that `engine/generate_streaming.py` and
`engine/generate_a32.py` write at build time). `program_builder.cpp` turns
decoded A64 instructions into those streams, `a32/` does the same for
AArch32, and `executor.cpp` owns the translation cache and runs them. The
software TLB, the guest CPU state and the AArch32 decoder are shared with the
JIT and are not the engine's own. The region's placement, exclusives,
alignment, the SME profile and the AArch32 profile are cross-engine contracts
in [`docs/architecture.md`](../../../../docs/architecture.md). This file
records the contracts that are not obvious from the code.

## Stream encodings

An arm64 gadget stream carries no PC word for a memory transfer. The operand
of every load, store, pair, atomic, exclusive, ordered and SIMD structure form
ends in the instruction's position in its block, in its top byte, so a block
holds at most 256 instructions. A fault reports the position shifted left by
two with bit 0 set, which no instruction address and no translation key has,
and the boundary turns it into the PC from the block holding the faulting
stream, as `resolve_fault_position` does for A64; a T32 block, whose
instructions have two lengths, finds the instruction in the block's halfwords.
Only MOPS and a literal load whose target does not fit 48 bits keep a PC word.
`arm64-gadget-memory-fault-position` faults one instruction of each family at
several positions, directly and through a link.

A constant stored to a register is one stream word when it fits 48 bits or is
ones above bit 47: the destination's CpuState offset in bits 63:48 over the
value's low 48 bits, which MOVZ and MOVN take below a shift of 48, ADR and
ADRP (with the ADD that folds into them) and the emulated identification
registers when their value fits. Any other value keeps
`move_wide_constant` and its two words. `arm64-gadget-packed-immediate` runs
each form against the architectural definition.

A TBZ or TBNZ carries no mask word: its operand word holds the bit number in
bits 5:0, where the register shift that tests it reads it, and the register's
CpuState offset in bits 31:16, and an inlined branch's retire suffix sits in
bits 47:32. The `test_branch` entries of the former form stay in the bank
unselected, because removing them would move every later entry.
`arm64-gadget-test-bit-branch` runs every bit, both polarities, XZR and each
shape the builder emits.

A logical immediate on W registers shares the word that names its registers,
in bits 63:32 above the destination and source fields. A 64-bit immediate has a
word of its own, and a form whose destination is its source, which bakes the
register into the entry, has that word alone. `arm64-gadget-packed-immediate`
runs every W encoding and a sample of the X ones in each shape.

An SBFM or UBFM is a gadget pointer and one operand word: the two shifts that
compute it, whose amounts the builder derives from the encoding, and the
CpuState offsets of its source and destination. Only BFM keeps its two mask
words. The SBFM and UBFM entries of the former form stay in the bank
unselected, because removing them would move every later entry. A RET through
X30 is the gadget pointer and its indirect site; any other register keeps the
operand that names it.

## Return ring and target table

A cached return consults a private per-carrier ring of return records that
linked calls push, and a cached indirect branch the address space's
direct-mapped target table that every miss exit trains (`engine/control.inc`,
`program.hpp`), only after the site's cells miss, so a stale or wrong record
can only fall back to the ordinary miss exit. A hit follows its link and fills
only an empty cell, so vCPUs sharing a polymorphic site do not keep rewriting
it, while the miss exit retrains its cells round-robin. The executor empties
the ring on every address-space switch and after a link sweep. The table
belongs to the address space's link state and is retired with it, so it never
names another address space's code, and a link reset by a code change leaves
through its ordinary unlinked exit.

## Software TLB

The TLB's geometry, and why a transfer probes by the page of its final byte so
that a crossing can only miss, are in `darwin/memory/tlb.hpp`.
`arm64-tlb-probe-set` checks that every inline probe of both engines hits the
entry the C++ fill path placed and misses a page its entry does not hold. The
gadget engine's fill path looks the page up in the generation its entered
execution context holds (`darwin_uml_static_vm_execution_context_translate()`),
without resolving the thread's address space and active generation each time.
It reads the generation's fill index (`darwin/memory/mmu/fill_index.hpp`),
which is read without a lock because whatever removes or changes a mapping
quiesces execution first. A page the index does not hold is looked up in the
map under its reader lock.

## Streaming mode

Both engines run a guest in streaming mode on the host's own, and the rules in
the next two paragraphs hold for the [JIT](../jit/README.md#streaming-mode)
too.

A translation belongs to one SVCR mode, PSTATE.SM and PSTATE.ZA, and an SVCR
write ends its block. A program translated with SM = 1 runs with the host in
streaming mode and, when ZA = 1, with guest ZA, and ZT0 under SME2, resident in
the host's. No C++ runs in streaming mode: a helper call leaves it, spilling
that state. XNU resets FPSR on every kernel entry while a thread is in
streaming mode, so a streaming program keeps the guest's FPSR in `CpuState` and
never runs with the native FP environment active: only the guest's FPCR, with
its trap enables clear, is the host's while it runs, and MSR FPCR sets both
copies. A floating-point form ORs the exceptions it raised into the guest's
FPSR, and runs again if FPSR.QC shows that a kernel entry reset the host's; a
form that may change NZCV runs with the guest's flags in the host's.

A streaming program runs every register-only SVE, SVE2 and SME form of the SME1
profile, every FP&SIMD data-processing class legal in streaming mode, and every
SVE and SME1 load and store and SIMD&FP load and store legal there. SVE
prefetches are NOPs. AdvSIMD, a word outside the profile and an SME2 form,
which the guest is not told of, are the illegal-instruction exit. A memory
access the native instruction cannot take goes to a helper that does the whole
access on the register file: it touches only active elements, translates every
page they lie in before it moves a byte, and reports a fault as M4 hardware
does, a predicated access at its first faulting active byte rounded down to
4 KiB and an unpredicated one at that byte (`sme_helpers.cpp`; the JIT's is
`jit/jit_semantic_sme.cpp`).

The gadget engine's control core is SMSTART and SMSTOP, MRS and MSR of SVCR and
TPIDR2_EL0, RDSVL, ADDSVL and ADDSPL, LDR and STR of ZA and ZT0, and ZERO of ZA
and ZT0, with every size read from the vCPU's SVL at run time; RDVL, ADDVL and
ADDPL run as RDSVL, ADDSVL and ADDSPL. Its SME part follows the veneer island,
so appending to it moves no base entry and re-encodes no veneer call. Each SVCR
mode has its own program cache and links, and a mode change empties the return
ring. A program translated with SM = 0 and ZA = 1 reaches ZA and ZT0 through a
helper on the register file.

A streaming program may select only the entries that `engine/sme.inc` lists:
the control core, the base gadgets that execute no AdvSIMD instruction and call
no helper, and the streaming twins of the base GPR gadgets that do (`sme.inc`
says how a twin is assembled and how its helper calls leave streaming mode and
enter it again). The twins cover the integer loads, stores and pairs, atomics
and exclusives, DC ZVA, IC IVAU, the MOPS forms and CSSC CNT, so their hot
paths run in streaming mode unchanged and only a TLB miss, a page crossing or
an alignment fault leaves it. The register-only forms and the vector memory
forms run from gadgets that `engine/generate_streaming.py` writes at build time
from the encoding table; its docstring says how each gadget loads and stores
its registers, and the builder resolves a memory form's address mode, registers
and shape into the stream `darwin/cpu/arm64/gadgets/sme_layout.h` describes. A
memory gadget's fast path forms the address and, when every byte the access
spans lies in one guest page the TLB holds, runs the native instruction on
registers loaded from the register file; anything else takes the helper above.
`arm64-gadget-streaming-safety` walks every listed entry, and every 16-byte
boundary of the generated gadgets, in the images each tree links and fails on
an AdvSIMD word executed in streaming mode, a helper called there, or an
indirect call.

## AArch32

`a32/` builds the streams ([`a32_builder.hpp`](a32/a32_builder.hpp)), and
`run_translated_aarch32_blocks` in `cpu/arm64/engine_execution.hpp` is the loop
an engine's `run_aarch32` takes. The gadgets come from the decoder's table by
`engine/generate_a32.py` and the hand-written `engine/a32.inc`, in a bank, a
cold section and a veneer island of their own after the SME part, so no A64
entry moves and the part holds no SVE or SME word. The generators' docstrings
(`engine/a32_*.py`) and `a32.inc` state each family's operand layout and
lowering, and the memory transfers that carry no key report their position as
the A64 ones do (`a32_layout.h`).

A block never ends inside an IT block: one the block limit cuts off ends in an
unlinked exit with ITSTATE stored, and an entry with ITSTATE set, like a single
step, runs one uncached instruction at a time until ITSTATE is zero. A
conditional instruction that sets no flags and does not write the PC is a
select where its form has one, so nothing branches on the guest's flags, and a
plain CMP followed at once by a conditional branch is the A64 fused
compare-and-branch gadget; an unconditional VCMP or VCMPE followed at once by
an unconditional VMRS APSR_nzcv is likewise one gadget (`a32/translator.hpp`).
A word the decoder rejects and a coprocessor word the profile lacks end the
block in the illegal-instruction exit at the word whatever its condition,
except a coprocessor word that is not UNPREDICTABLE in the complete profile,
which a failing condition skips like any other instruction. SVC, BKPT and a
retiring exit count their instruction; a fault or undefined exit does not.

What the gadgets do not lower runs through the shared AArch32 semantic library
(`routes_to_library` in `a32_builder.hpp` lists it): the rare integer forms,
VRECPS and VRSQRTS (AArch32's unfused steps, not the fused A64 ones), LDM, STM,
PUSH, POP, VLDM, VSTM and the interleaved and one-lane structure accesses that
cross a page, an STM that stores the PC, and a misaligned access with an
alignment hint. The `a32_library` gadget spills NZCV, decodes the word again
and calls `execute_a32`; its exit commits the live FPSR to `CpuState` before
the call and reinstalls it after, as the A64 engine's software floating-point
helpers do. The library's unit-test variant counts its exits per form, which
names any lowered form that reaches the library, and the unit tests that link
it require none does. The AArch32 exclusives and LDA and STL are the A64
gadgets and share their forward-progress mechanism.

VFP runs under the guest's FPCR in the native FP environment, and Advanced SIMD
floating point under the standard FPSCR value (flush to zero, default NaN,
round to nearest). Each such gadget starts with `a32_fp_standard` and each VFP
gadget with `a32_fp_guest` (`engine/a32.inc`), which write the host FPCR only
when `CpuState::a32_fp_standard` says the native environment holds the other
one, so a loop of one kind switches once and the execution boundary restores
the host's FPCR whichever it holds; the JIT follows the same rule. A switch
costs more than the instructions around it, so the forms that interleave the
two kinds in image and signal code (the VFP conversions, VMAX and VMIN, and the
single-precision VADD, VSUB and VMUL of both kinds) run in the active
environment when their result is the same in both and switch only for operands
where it is not; the generators' guards in `engine/a32_simd.py` and
`engine/a32_neon.py` test integer bits of the operands before the instruction,
so they raise no flags of their own.

The engine decodes in the profile its host gives it (`a32::decode_profile`):
the extensions the host has the A64 feature for (`a32_features`), which are the
ones the guest is shown, so a form of any other extension is UNDEFINED, as the
HWCAPs say; the JIT derives the same profile. `engine/a32_armv8.py` lowers each
shown extension onto the A64 instruction with the same lanes, and a decoded
form with no gadget is the library's through `a32_library`: the BF16 and Int8
matrix forms and VJCVT, on a host that has them. `arm64-gadget-a32-control`
checks the contract above, `arm64-gadget-a32-forms` that the engine defines
exactly what the decoder defines, `arm64-gadget-a32-corpus` that it matches the
AArch32 corpus records, and `arm64-gadget-region-layout` the part's placement.
