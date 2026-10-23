# arm64 JIT engine

The JIT emits native blocks for A64, A32 and T32 guest code into
its executable arena (`jit_arena.cpp`), which reaches executable memory only
through `cpu/jit_memory/executable_memory.cpp`. `jit_compiler*.cpp` lowers
A64, `a32/` lowers A32 and T32, and `jit_executor*.cpp` owns the translation
cache, the links and the shared translation tier. The gadget engine includes
none of it. Exclusives, alignment, the SME profile, the AArch32 profile and
the semantic slow path's role are cross-engine contracts in
[`docs/architecture.md`](../../../../docs/architecture.md). This file records
the lowering contracts that are not obvious from the code; the
[gadget engine](../gadgets/README.md) states the rules both engines share.

## Streaming mode

The JIT is `complete` too. Its control core runs SMSTART and SMSTOP, MRS
and MSR of SVCR and TPIDR2_EL0, RDSVL, ADDSVL and ADDSPL, and LDR, STR and
ZERO of ZA and ZT0. It keys every translation, link
and indirect site by the guest's SVCR mode as well as its PC, so a block runs
only in the mode it was translated for; mode 0 translations are unchanged. A
guest write of SVCR ends its block. An SM=1 translation keeps the guest's Z,
P, ZA and ZT0 in the identically numbered host registers: its external
prologue enters streaming mode and fills them from the register file, and
every epilogue and host helper call first leaves streaming mode and spills
them. Helpers are terminal, so no SM=1 code runs after one, and a TLB miss
takes a streaming bridge that leaves and re-enters around the walker. The
transitions preserve NZCV, and SM=1 code emits no AdvSIMD instruction of its
own. An SM=1 translation also runs every register-only SVE, SVE2 and SME form
of the SME1 profile, and every FP&SIMD data-processing class legal in
streaming mode, as the guest's own word, with each general register it names,
and a ZA slice index, renamed to a host register that holds the guest's.
MOVPRFX runs as a move of its own. A form that may change NZCV runs with the
guest's flags in the host's. XNU resets FPSR on every kernel entry while a
thread is in streaming mode, so the guest's FPSR stays in `CpuState`: the
streaming entry installs the guest's FPCR with trap enables clear, which
kernel entries keep, MSR FPCR writes both copies, and the exit restores the
host's. A floating-point form runs on a cleared FPSR, ORs what it raised into
the guest's, and restores its destination and runs again if FPSR.QC shows
that a kernel entry reset the host's. Every SVE and SME1 load and store
runs as the guest's own word, its base renamed to the host address and its
offset zeroed, when every byte it can span, whichever elements are active,
lies in one guest page the TLB holds. A crossing, a TLB miss, a failed
translation, a misaligned SP base or a tagged base takes a terminal helper
that does the whole access on the register file: it touches only active
elements, translates every page they lie in before it moves a byte, and
reports a fault as the gadget engine does. The SIMD&FP loads and stores
legal in streaming mode run the same way, with any writeback after the
access. SVE prefetches are NOPs. SME2, the reserved bitmask immediates of
ORR, EOR, AND and DUPM, every other SVE and SME word, and in streaming mode
AdvSIMD and the FEAT_LSFE and FEAT_LSUI loads and stores are its
illegal-instruction exit; every other instruction it runs at SVCR 0 runs in
streaming mode too. SM=0 translations run with the host's SME off and
hold no SVE or SME instruction: they reach ZA and ZT0 in the register file
with GPR copies. SM=1 code reads SVL with RDSVL and SM=0 code from
`CpuState`, so no translation depends on it.

## AArch32

The JIT lowers AArch32 in `cpu/arm64/jit/a32`, beside the A64 compiler and
without changing it. An A32 or T32 block is decoded by the shared decoder and
compiled by its own compiler, which reuses the A64 compiler's register
residency, flag machine, cold paths, direct and indirect links, tracked stores
and publication: R0-R14 are resident host registers (each core register has
one: x3-x7, x12, x13, x17 and the callee-saved x22-x28, which an AArch32
translation's frame saves and the A64 translations never touch). A linked
translation keeps them there from block to block: the registers are loaded
before the block's internal entry, which only the executor reaches, a link
leaves them in their hosts, and every return to the executor, a refused link
included, writes all fifteen back; its link sequences therefore use x10 and
x11, and no lowering may use a host register of a core register as scratch.
An unlinked translation (the tracked-store variant, a single instruction) loads
at first use and writes back at every exit. NZCV is the host's with the lazy-flag states of the A64 compiler, a
condition is a branch over the body (a conditional data-processing instruction
that sets no flags and writes no PC, and MOVW, instead runs on both arms and
a CSEL on the host flags keeps its result only where the condition holds, so
no data-dependent branch is left to mispredict), and a link's target is the
key. Every exit
whose successor is static (a branch, the fall-through of a block the 60
instructions of a window cut short, the arm of a conditional instruction or
CBZ that is not taken) is a link while the ITSTATE that follows is zero, so a
straight run chains like a loop. A chain states what is set up for the whole
run, not what its first block needs: the floating-point state is loaded, and
the native environment is on whenever any earlier block switched it on, so a
chain that began at an integer block links into a floating-point one and back
instead of returning to the executor at each edge. An indirect branch of
AArch32 code looks in its site's two cells and then in the executor's
direct-mapped table of the targets its misses have trained (4096 entries,
tagged with the link epoch so that the translation of the same address in
another address space or after a code change never matches), and returns to the
executor only when both miss: a function with many callers, such as libm's
`lroundf`, has a return to each of them. A conditional branch (B with a
condition, CBZ, CBNZ) does not end a block: the block goes on with its
registers and flags in the host, while the taken path leaves by a cold side
exit that publishes what is dirty at the branch and links to the target. A
branch back is a loop's end or a `continue`, and the side exit costs the link
of the taken path either way. An unconditional branch ends the block, and so
does a conditional one back to the translation's own first instruction: that
block is a loop in the host. A probe compilation records the vector registers it holds at
the back edge and the core registers the body names, the
translation loads them before the loop head and counts
every one as dirty so that each exit writes them back, and the back edge is the
chain budget's test for one more iteration and a branch. The iteration that
cannot be afforded leaves like any linked exit. Memory
takes the inline TLB sequence after a 32-bit address and the alignment
rule of the form (the entry index comes from the address's own bits and the
tag compare shifts the address, two instructions fewer than the A64
compiler's). An access already checked to be aligned to its size (a
single-precision VLDR or VSTR, a Neon transfer whose alignment hint covers its
bytes, an ordered or exclusive access) is not tested for crossing a page. Any
other access of more than one byte compares the entry's tag with the page of
its last byte, so a crossing access misses (the index never gives neighbouring
pages of a 32-bit address space one entry) and its cold path tells it from a
miss. A single load or store of a byte, halfword or word does not add the entry's
delta to the address in a register of its own: the access takes the delta in
its addressing mode (`[x9, w8, uxtw]`), and the miss path leaves the host
pointer in x9 and zero in x8; a pair, a multiple transfer, an acquire or
release and a store that records its host range keep the host pointer in x8.
The Armv8 load-acquire and store-release (LDA, STL and their
byte and halfword forms) are an alignment-checked LDAR and STLR. A
load-exclusive, up to four register instructions and the store-exclusive
(acquire and release forms included) on the same address at the start of a
block become one host load-exclusive and store-exclusive, so the guest's
monitor is the host's; a failed store leaves the status to the guest's retry
loop. A conditional forward branch or CBZ/CBNZ between the two (the
compare-and-swap that leaves on a mismatch, the try-lock) stays in the run as
a side exit whose taken arm clears the monitor and links to the target. The
parallel add and subtract forms run in the vector unit on the
packed lanes (UMAX and UMIN stand in for the unsigned saturating forms and a
widening add for GE, so the host's QC flag is never set). What the lowering
declines (an access across a page or one that must fault, an exclusive that
does not fuse, the signed saturating parallel forms, SSAT16, USAT16 and USAD8)
leaves the block through `uml_arm64_jit_a32_execute`, a terminal helper that
runs the one instruction in the AArch32 semantic library over the TLB, with
the library's fault ordering. The helper restores the host's FPCR and FPSR
unless the instruction is a memory transfer or integer arithmetic, which
leave the guest's native environment installed for the next block. The
exclusives run there too, over the JIT's versioned software reservation: the
helper overrides the library's monitor hooks, because the guest monitor is
shared with the tracked stores and the fused host exclusives and the library's
default compare-and-swap monitor is not. An entry in the middle of an IT block
and a single step run one instruction in the single-instruction arena, with
the IT state the exit recorded. Translations validate their saved guest bytes
in the ISA's unit, so a halfword of a 32-bit T32 instruction rewritten under a
translation is seen. Executable memory comes only from the arena the A64 JIT
already uses; the A32 files allocate and publish none.

The JIT's AArch32 vector support (VFP and Advanced SIMD on D0-D31) keeps Q0-Q15
in the A64 compiler's vector residency: CpuState vectors 0-15 are the resident
host registers V16-V31, a D register is the low or high half of its Q and an S
register a 32-bit lane, so a narrower operand is staged in a scratch register
and a result is inserted into its half or lane. An insert rewrites the whole Q,
so in a run of unconditional single-precision VFP instructions an S register
that was read from its lane or written also keeps a copy, a shadow, in one of
V0-V4 and V7, which the run's later reads take instead of the lane: operations
on different lanes of one Q do not wait for each other's inserts. The Q
register still takes every write, so an exit, a link and a cold path need no
shadow; a conditional, double-precision or Advanced SIMD instruction, or any
other that may write an S register or use those host registers, forgets them,
and the TLB miss bridge preserves V0-V7. A VFP instruction runs under the
guest's FPCR and FPSR, which the executor installs for a block that holds one
(the native floating-point environment of the A64 compiler); an Advanced SIMD
floating-point instruction runs under the Standard FPSCR value (FZ and DN set,
round to nearest, AHP and FZ16 the guest's). The host FPCR keeps the mode it
has across exits and links, as the gadget engine's does:
`CpuState::a32_fp_standard` records it, the executor clears it where it
activates the environment, and a translation tests the byte at the first
instruction that needs a mode and writes the FPCR only when the mode differs.
The two modes part only on a nonzero denormal operand, a NaN operand, a
result that is tiny before rounding and a rounding mode other than to nearest,
so a Neon VMAX, VMIN, VADD, VSUB, VMUL, VMLA or VMLS of single precision in a
run of at most eight Neon floating-point operations that a VFP instruction
(or the end of the block) follows runs under the guest's FPCR behind integer
tests of its operands, and under the Standard value from a cold path when a
test fails; a longer run switches once, and so does a loop that holds such
operations and no VFP instruction or FPSCR write (the probe compilation counts
them): it sets the Standard value before its head and runs every lap unguarded.
A run under the guest's FPCR leaves its cumulative
exception flags behind, so what only it would raise is found before it: a
product whose operand exponents sum to less than 129 (it may underflow,
inexactly) is tested before the multiplication, and a sum needs no such test
because a tiny sum of normal numbers is exact. A saturating
instruction sets QC in the host FPSR, which is the guest's. A VCMP
leaves its flags in FPSCR's copy in `CpuState` and in the host NZCV, so an
unconditional VMRS APSR_nzcv, FPSCR right after it keeps them there and
the branch on them waits for no reload. VRECPS and VRSQRTS
round the product before the step, as the AArch32 pseudocode does and the A64
instructions do not, and answer 2.0 or 1.5 for an infinity times a zero
without raising Invalid Operation. VLDR, VSTR and the register-list transfers
move whole Q registers where the list is aligned to one; the structure loads
and stores use the host LDn and STn on scratch registers. They share the
scalar transfers' TLB sequence and alignment rule. A faulting alignment, a PC
base or a list past the register file runs the instruction in the semantic
library instead, as does a store that crosses a page. A load of at most 32
bytes that crosses a page stays in the translation: its cold path probes the
TLB for both pages and, when the two mappings are adjacent in the host, reads
through the first as within one page, and otherwise copies the last 32 bytes
of the first page and the first 32 of the second side by side into
`CpuState::a32_cross` and reads from there at the guest's offset. A page
missing from the TLB takes the library, which fills it.

The JIT presents the Armv8 extensions as well, each where the host has its A64
feature: it decodes against `a32_decode_profile`, the
extensions the guest is shown, so a form of an extension the host lacks is
UNDEFINED and never reaches the semantic library. It lowers the ones that are
the host's own instruction on the same operands: CRC32, the AES and SHA-1 and
SHA-256 instructions, VSEL (FCSEL on the guest flags), VMAXNM and VMINNM, VRINT
and VCVT with a named rounding (scalar under the guest's FPCR, Advanced SIMD
under the Standard FPSCR value), the signed and unsigned dot products, VQRDMLAH
and VQRDMLSH, VCADD and VCMLA (half precision as well, with FP16) and SB (a DSB
and an ISB). VJCVT is FJCVTZS with its Z flag taken into FPSCR; on a host whose
FJCVTZS reports a flushed subnormal as exact, the A64 JIT's correction (a
subnormal input is never exact) applies here too. A D-register form that A64
has no 64-bit arrangement for (single-precision VCMLA by element) runs on the
128-bit arrangement with the doubleword in both halves, which raises the same
flags. The rest of the level runs in the semantic library: the other
half-precision forms, the fused widening half-precision multiply-accumulates,
the BFloat16 and 8-bit matrix forms and the mixed-sign dot products, and a
half-precision variant of an Advanced SIMD floating-point form that the
compiler does not lower, which shares the form of the single-precision one and
differs in the operands' esize.
