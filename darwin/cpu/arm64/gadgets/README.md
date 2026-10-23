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

Cached returns consult a private per-carrier return-record ring after their site
cells miss: linked calls push the return PC with its block link, a matching
record follows that link and fills an empty cell of the site, and the executor
empties the ring on every address-space switch and after a link sweep, so a
stale record can only fall back to the ordinary miss exit. A cached indirect
branch whose cells (and, for a return, ring) miss then looks the target up in
its address space's direct-mapped target table, which every miss exit trains
with the target's block link; a hit follows that link and likewise fills only
an empty cell, so vCPUs sharing a polymorphic site do not keep rewriting it,
while the miss exit retrains its cells round-robin. The table belongs to the
address space's link state and is retired with it, so it never names another
address space's code, and a link reset by a code change leaves through its
ordinary unlinked exit.

## Software TLB

The software TLB keys its entries by guest page number and indexes 16,384
32-byte entries directly by the page number's low bits, so a cache line holds
two neighbouring pages. A same-page transfer compares the page of its final
byte against the entry its first byte indexes: the direct index never places
neighbouring pages in one entry, so a crossing transfer can only miss and is
recognized on the cold path. `arm64-tlb-probe-set` checks that every inline
probe of both engines hits the entry the C++ fill path placed and misses a
page its entry does not hold.
The gadget engine's fill path looks the page up in the generation its entered
execution context holds (`darwin_uml_static_vm_execution_context_translate()`),
without resolving the thread's address space and active generation each time.
It reads the generation's fill index (`darwin/memory/mmu/fill_index.hpp`), a
radix table of host addresses and guest permissions that mirrors the mapping
map and is read without a lock: whatever removes or changes a mapping quiesces
execution first, so an entered context never sees a stale word. A page the index
does not hold is looked up in the map under its reader lock.

## Streaming mode

The gadget engine implements all of the profile but the SME2 forms
(`sme_support` is `complete`). Its control core is SMSTART and SMSTOP, MRS
and MSR of SVCR and TPIDR2_EL0, RDSVL, ADDSVL and ADDSPL, LDR and STR of ZA
and ZT0, and ZERO of ZA and ZT0, with every size read from the vCPU's SVL at
run time. Its SME part follows the veneer island, so appending to it moves
no base entry and re-encodes no veneer call. A translation belongs to one SVCR mode, PSTATE.SM and
PSTATE.ZA: each mode has its own program cache and links, an SVCR write
ends its block, and a mode change empties the return ring. A program
translated with SM = 0 and ZA = 1 reaches ZA and ZT0 through a helper on the
register file. One translated with SM = 1 runs with the host in streaming
mode and, when ZA = 1, with guest ZA, and ZT0 under SME2, resident in the
host's; a helper call leaves streaming mode around itself, spilling and
refilling them, so no C++ runs in streaming mode. XNU resets FPSR on every
kernel entry while a thread is in streaming mode, so a streaming program
keeps the guest's FPSR in `CpuState` and never runs with the native FP
environment active: only the guest's FPCR, with its trap enables clear, is
the host's while it runs, and MSR FPCR runs as a twin that sets both
copies. Such a program may select
only the entries that `engine/sme.inc` lists: the control core, the base
gadgets that execute no AdvSIMD instruction and call no helper, and the
streaming twins of the base GPR gadgets that do. A twin is its base gadget
assembled a second time from the same source, as `uml_arm64_gadget_sm_`
plus the base name, with each helper call made through a trampoline that
leaves streaming mode around it and each AdvSIMD word of a slow path
replaced by an FMOV; CSSC CNT's twin counts in GPRs. A table maps each base
entry to its twin, and the builder selects the twin. Twins cover the integer
loads, stores and pairs, atomics and exclusives, DC ZVA, IC IVAU, the MOPS
forms and CSSC CNT, so their hot paths run in streaming mode unchanged and
only a TLB miss, a page crossing or an alignment fault leaves it. A
streaming program also runs every register-only SVE, SVE2 and SME form of
the SME1 profile, and every FP&SIMD data-processing class legal in streaming
mode, from gadgets that `engine/generate_streaming.py` writes at build time
from the encoding table, one per value of a form's bits that are not
register numbers. Each loads the registers its instruction names from the
register file at offsets the builder computes from the SVL, or from
`CpuState` for general registers, into fixed host registers, runs the
instruction on them and stores its destination back. A form whose
immediates would take hundreds of gadgets runs an equivalent register form
on an immediate, or a count from the SVL, that the builder computes. A
gadget that may change NZCV runs with the guest's flags in the host's, and a
floating-point one ORs the exceptions it raised into the guest's FPSR, and
runs again if FPSR.QC shows that a kernel entry reset the host's. RDVL,
ADDVL and ADDPL run as RDSVL, ADDSVL and ADDSPL. Every SVE and SME1 load and
store, and every SIMD&FP load and store legal in streaming mode, runs from
a generated vector memory gadget, one per instruction the host executes for
it (shape, direction, sizes, extension, ZA tile and slice direction), with
the address mode, registers and shape resolved by the builder into the
stream `darwin/cpu/arm64/gadgets/sme_layout.h` describes. Its fast path
forms the address, and when every byte the access spans lies in one guest
page the TLB holds, runs the native instruction on registers loaded from the
register file. Anything else takes a helper that does the whole access on
the register file: it touches only active elements, translates every page
they lie in before it moves a byte, and reports a fault as M4 hardware does,
a predicated access at its first faulting active byte rounded down to
4 KiB and an unpredicated one at that byte. SVE prefetches are NOPs. The
builder ends a block at the illegal-instruction exit on any other word:
AdvSIMD, a word outside the profile, or an SME2 form, which the guest is
not told of.
`arm64-gadget-streaming-safety` walks every listed entry, and every 16-byte
boundary of the generated gadgets, in the images each tree links and fails
on an AdvSIMD word executed in streaming mode, a helper called there, or an
indirect call.

## AArch32

The gadget engine's AArch32 part translates A32 and T32 with the shared
decoder into the data-only streams A64 uses, so one executor, one program
cache and one execution boundary run both
([`gadgets/a32`](a32/a32_builder.hpp);
`run_translated_aarch32_blocks` in `cpu/arm64/engine_execution.hpp` is the
loop an engine's `run_aarch32` takes). Its gadgets come from the decoder's
table by `engine/generate_a32.py` and `engine/a32.inc`, in a bank, a cold
section and a veneer island of their own after the SME part, so no A64 entry
moves and the part holds no SVE or SME word. A conditional instruction is a
skip over a run of instructions sharing its condition, a conditional
terminal gives the skipped instruction back, and a block never ends inside an
IT block: one the block limit cuts off ends in an unlinked exit with ITSTATE
stored. A block holds at most 64 instructions (`a32::block_limit`); a T32
block stops five short of that unless an IT block is open, so that an IT
block it starts completes inside it. Conditional data processing that sets
no flags and does not write the PC (MOV, MVN, ADD, SUB, RSB, AND, ORR, EOR
and BIC of an immediate or a register, MOVW, and VMOV between S or D
registers) is a select instead: its gadget computes the result and keeps the
old value where the condition fails, so nothing branches on the guest's
flags. A plain CMP of a register or of an immediate below 2^24, outside an
IT block and followed at once by a conditional branch, is the A64 fused
compare-and-branch gadget of the branch's condition, which reads the
CpuState slots a register lives in, so the pair costs one dispatch. An
unconditional VCMP or VCMPE followed at once by an unconditional VMRS
APSR_nzcv is likewise one gadget that sets FPSCR's flags and copies them to
NZCV. ADD and SUB of an immediate to the same register have an entry per
register R0 to R14 with the displacement baked in. A word the decoder rejects and a coprocessor word the profile lacks end the
block in the illegal-instruction exit at the word whatever its condition, except a coprocessor word that is not
UNPREDICTABLE in the complete profile, which a failing condition skips like
any other instruction. SVC, BKPT and a retiring exit count their instruction;
a fault or undefined exit does not. SVC leaves PC after itself and ITSTATE
advanced; a fault, UDF or BKPT leaves PC at the instruction with its own
ITSTATE, which a T32 block recovers by decoding its saved halfwords again. An
entry with ITSTATE set, and a single step, run one uncached instruction at a
time until ITSTATE is zero. The parallel add and subtract family and SEL are
gadgets: Advanced SIMD does the lanes on the low half of a vector register, GE
is the byte-lane mask CpuState keeps, and the saturating forms compute their
saturation with lane arithmetic, because the saturating instructions would set
FPSR.QC, which the guest reads in FPSCR. LDM, STM, PUSH and POP that cross a
page, an STM that stores the PC, and the rare integer forms (USAD8, the dual
multiplies, the 16-bit saturates and extends, QADD and its relatives), run
through the shared AArch32 semantic library
(`cpu/arm64/aarch32/semantics.hpp`): the `a32_library` gadget and the
crossing paths spill NZCV, decode the word again and call `execute_a32`,
which translates every page before a load changes a
register, stores the words ahead of a faulting STM and leaves the base
register as it was. LDREX and STREX, and LDAEX and STLEX, are the A64
exclusive gadgets and share their forward-progress mechanism; LDA and STL are
the A64 ordered ones. Like them, the single and doubleword transfers, VLDR,
VSTR, TBB, TBH and the block transfers carry no key: their operand holds the
instruction's position in bits 31:24
(`darwin/cpu/arm64/gadgets/a32_layout.h`), which a fault reports as the A64
transfers do, and a block transfer's crossing path passes it to the library
in place of the key. The semantic library's unit-test variant counts its exits
per form, which names any lowered form that reaches the library, and the unit
tests that link it require none does.

VFP and Advanced SIMD are lowered onto the A64 instructions with the same
lanes. D0-D31 and S0-S31 are byte offsets into `CpuState::vectors`, and a
gadget stores only the bytes the A32 destination has. VFP runs under the
guest's FPCR in the native FP environment. Advanced SIMD floating point runs
under the standard FPSCR value (flush to zero, default NaN, round to nearest):
each such gadget starts with `a32_fp_standard` and each VFP gadget with
`a32_fp_guest` (`engine/a32.inc`), which write the host FPCR only when
`CpuState::a32_fp_standard` says the native environment holds the other one,
so a loop of one kind switches once and the execution boundary restores the
host's FPCR whichever it holds. A switch costs more than the instructions
around it, so the forms that interleave the two kinds in image and signal
code run in the active environment when their result is the same in both and
switch only for operands where it is not: the VFP conversions between floats
and integers (all but a denormal float to integer, and an integer to float
under a rounding mode other than round to nearest), VMAX and VMIN (a NaN or a
denormal), and the single-precision VADD, VSUB and VMUL of both kinds (a
nonzero operand whose exponent field is outside 64 to 191, which keeps a sum,
a difference and a product normal, and a guest rounding mode other than round
to nearest). The test is integer arithmetic on the operands' bits, made
before the instruction so that it raises no flags of its own. The A32 shifts by an immediate use the A64
shift-by-register instructions with a replicated signed amount, so a gadget
serves every amount; the forms that exist in A64 only with an immediate
(SQSHLU, the converts with a fraction width, VEXT) have a gadget per
immediate. VLDn and VSTn are native (`engine/a32_structure.py`): a copy for
VLD1 and VST1 of several registers, LD2 to LD4 and ST2 to ST4 for the
interleaved ones, element copies for one lane and LD1R to LD4R for all lanes,
with the register or immediate writeback; a VLD1 or VST1 copy that crosses a
page stays in the gadget and moves its bytes with the A64 slow transfer helper,
and one without an alignment hint has a gadget for each writeback that tests
nothing the instruction does not need. A copy with an alignment hint and a
VLD1 to all lanes of one or two registers have gadgets of their own too, which
test the alignment and leave a misaligned access to the library. An
unconditional ADD or SUB of an immediate that sets no flags, names no PC and
computes the base of a copy without hint or writeback runs inside the copy's
gadget when only unconditional Advanced SIMD data processing that reads no
core register comes between: the gadget writes the register before the
transfer, so a fault finds it as the ADD left it, and nothing between can
exit or fault.
VRECPS and VRSQRTS (AArch32's unfused steps, not the fused A64 ones), an
interleaved or one-lane structure access or VLDM and VSTM that crosses a page,
and a misaligned access with an alignment hint run
through the semantic library, whose exit commits the live FPSR to
CpuState before the call and reinstalls it after, as the A64 engine's
software floating-point helpers do. A saturating instruction leaves QC in the
cumulative FPSR flags the native environment keeps; `engine/a32_neon.py` and
`engine/a32_simd.py` document the groups and the operand words.

The Armv8 floating-point additions and the optional extensions are the
`complete` part of the profile. The engine decodes in the profile its host gives it
(`a32::decode_profile`): the extensions the host has the A64 feature for
(`a32_features`), which are the ones the guest is shown, so a form of any other
extension is UNDEFINED, as the HWCAPs say. The generator
(`engine/a32_armv8.py`) lowers each shown extension onto the A64 instruction
with the same lanes: CRC32, AES, SHA-1 and SHA-256, VMULL.P64, SB, VSEL,
VMAXNM, VMINNM, VRINT and VCVT with a rounding mode, the double/half
conversions, the half-precision VFP forms (arithmetic, compares, conversions,
VMOVX and VINS), half-precision Advanced SIMD, the dot products, VQRDMLAH and
VQRDMLSH, VCADD, VCMLA and VFMAL and VFMSL. A decoded form with no gadget is
the semantic library's, run through the `a32_library` gadget: the BF16 and
Int8 matrix forms and VJCVT, on a host that has them. The FP16 VFP forms share
the single-precision plans (aux bit 2) and store a half whole through its S
register, which a half result zero-extends into.
`arm64-gadget-a32-control` checks the contract above, `arm64-gadget-a32-forms`
that the engine defines exactly what the decoder defines, `arm64-gadget-a32-corpus`
that it matches the AArch32 corpus records, and `arm64-gadget-region-layout`
the part's placement.
