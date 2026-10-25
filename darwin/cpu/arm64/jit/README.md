# arm64 JIT engine

The JIT emits native blocks for A64, A32 and T32 guest code into its executable
arena (`jit_arena.cpp`), which reaches executable memory only through
`cpu/jit_memory/executable_memory.cpp`. `jit_compiler*.cpp` lowers A64, `a32/`
lowers A32 and T32, and `jit_executor*.cpp` owns the translation cache, the
links and the shared translation tier. The gadget engine includes none of it.
Exclusives, alignment, the SME profile, the AArch32 profile and the semantic
slow path's role are cross-engine contracts in
[`docs/architecture.md`](../../../../docs/architecture.md). This file records
the lowering contracts that are not obvious from the code; the
[gadget engine](../gadgets/README.md) states the rules both engines share.

## Top-byte ignore

When the guest ignores the top byte of data addresses (`CpuState::tbi_enabled`,
the host's `--guest-top-byte-ignore`), a translation is compiled for it and its
key says so (bit 61 of an A64 key, `translation_mode_tbi`), so a block runs only
under the setting it was translated for. Every access that does not name SP as
its base then strips bits 63:56 of its final address, after any displacement,
with one `ubfx` before the address is translated, and a translation compiled
without it holds no check at all. The TLB, a page fault and the fault address a
signal reports therefore see the untagged address, as the semantic path
reports it; an SP base is never untagged. Exclusives, atomics and ordered
accesses strip before they translate and record the host address, so the
monitor guards the location and not the pointer. A run of accesses off one base
strips once. The cold paths do not test the flag again.

## Streaming mode

The JIT follows the streaming-mode rules that the
[gadget engine](../gadgets/README.md#streaming-mode) states for both engines:
the mode a translation belongs to, the FPSR and FPCR handling, the forms a
streaming program runs and the helper that does a memory access the native
instruction cannot take. This section records what is the JIT's own.

Its control core runs SMSTART and SMSTOP, MRS and MSR of SVCR and TPIDR2_EL0,
RDSVL, ADDSVL and ADDSPL, and LDR, STR and ZERO of ZA and ZT0. It keys every
translation, link and indirect site by the guest's SVCR mode as well as its PC,
so a block runs only in the mode it was translated for; mode 0 translations are
unchanged. SM=0 translations run with the host's SME off and hold no SVE or SME
instruction. An SM=1 translation keeps the guest's Z, P, ZA and ZT0 in the
identically numbered host registers: its external prologue enters streaming
mode and fills them from the register file, and every epilogue and host helper
call first leaves streaming mode and spills them. Helpers are terminal, so no
SM=1 code runs after one, and a TLB miss takes a streaming bridge that leaves
and re-enters around the walker. The transitions preserve NZCV, and SM=1 code
emits no AdvSIMD instruction of its own.

It runs the register-only SVE, SVE2 and SME forms and the FP&SIMD classes as
the guest's own word, with each general register it names, and a ZA slice
index, renamed to a host register that holds the guest's; MOVPRFX runs as a
move of its own. The streaming entry installs the guest's FPCR with trap
enables clear, which kernel entries keep, and the exit restores the host's. A
floating-point form runs on a cleared FPSR; when it must run again, it restores
its destination first. Every SVE and SME1 load and store runs as the guest's
own word, its base renamed to the host address and its offset zeroed, when
every byte it can span, whichever elements are active, lies in one guest page
the TLB holds. A crossing, a TLB miss, a failed translation or a misaligned SP
base takes a terminal helper that does the whole access. The
SIMD&FP loads and stores legal in streaming mode run the same way, with any
writeback after the access. SME2, every other SVE and SME word, and in
streaming mode AdvSIMD and the FEAT_LSFE and FEAT_LSUI loads and stores are its
illegal-instruction exit; every other instruction it runs at SVCR 0 runs in
streaming mode too.

## AArch32

`a32/` compiles A32 and T32 blocks, decoded by the shared decoder, beside the
A64 compiler and without changing it. `A32Compiler`
([`a32_compiler.hpp`](a32/a32_compiler.hpp)) reuses the A64 compiler's register
residency, flag machine, cold paths, direct and indirect links, tracked stores
and publication. The comments of `a32/` state each family's lowering; this
section records the contracts that span them.

R0-R14 are resident host registers (`resident_gpr_hosts` in
`jit_compiler_families.hpp`). A linked translation keeps them there from block
to block: the registers are loaded before the block's internal entry, which
only the executor reaches, a link leaves them in their hosts, and every return
to the executor, a refused link included, writes all fifteen back. Its link
sequences therefore use x10 and x11, and no lowering may use a host register of
a core register as scratch. An unlinked translation (the tracked-store variant,
a single instruction) loads at first use and writes back at every exit.

NZCV is the host's with the lazy-flag states of the A64 compiler, and a
condition is a branch over the body. A conditional data-processing instruction
that sets no flags and writes no PC, and MOVW, instead run on both arms and a
CSEL on the host flags keeps the result only where the condition holds, so no
data-dependent branch is left to mispredict. A link's target is the key. Every
exit whose successor is static (a branch, the fall-through of a block the 60
instructions of a window cut short, the arm of a conditional instruction or CBZ
that is not taken) is a link while the ITSTATE that follows is zero, so a
straight run chains like a loop. A chain states what is set up for the whole
run, not what its first block needs: the floating-point state is loaded, and
the native environment is on whenever any earlier block switched it on, so a
chain that began at an integer block links into a floating-point one and back
instead of returning to the executor at each edge. An indirect branch looks in
its site's two cells and then in the executor's direct-mapped table of the
targets its misses have trained (`jit_linking.hpp`), and returns to the
executor only when both miss: a function with many callers, such as libm's
`lroundf`, has a return to each of them.

A conditional branch (B with a condition, CBZ, CBNZ) does not end a block: the
block goes on with its registers and flags in the host, while the taken path
leaves by a cold side exit that publishes what is dirty at the branch and links
to the target. A branch back is a loop's end or a `continue`, and the side exit
costs the link of the taken path either way. An unconditional branch ends the
block, and so does a conditional one back to the translation's own first
instruction: that block is a loop in the host, whose plan `a32_compiler.hpp`
describes.

Memory takes the inline TLB sequence after a 32-bit address and the alignment
rule of the form (the entry index comes from the address's own bits and the tag
compare shifts the address, two instructions fewer than the A64 compiler's). An
access already checked to be aligned to its size (a single-precision VLDR or
VSTR, a Neon transfer whose alignment hint covers its bytes, an ordered or
exclusive access) is not tested for crossing a page. Any other access of more
than one byte compares the entry's tag with the page of its last byte, so a
crossing access misses (the index never gives neighbouring pages of a 32-bit
address space one entry) and its cold path tells it from a miss; a load of at
most 32 bytes that crosses a page stays in the translation, staged through
`CpuState::a32_cross` when the two mappings are not adjacent. A single load or
store of a byte, halfword or word does not add the entry's delta to the address
in a register of its own: the access takes the delta in its addressing mode
(`[x9, w8, uxtw]`), and the miss path leaves the host pointer in x9 and zero in
x8; a pair, a multiple transfer, an acquire or release and a store that records
its host range keep the host pointer in x8. A store that crosses a page, a
faulting alignment, a PC base, a register list past the register file and a
page missing from the TLB run the instruction in the semantic library, which
fills the TLB.

The Armv8 load-acquire and store-release (LDA, STL and their byte and halfword
forms) are an alignment-checked LDAR and STLR. At the start of a block a
load-exclusive, up to four register instructions and the store-exclusive on the
same address become one host load-exclusive and store-exclusive, so the guest's
monitor is the host's; `a32_compiler_exclusive.cpp` says what else fuses and
what it declines. What the lowering declines (an access across a page or one
that must fault, an exclusive that does not fuse, the signed saturating
parallel forms, SSAT16, USAT16 and USAD8) leaves the block through
`uml_arm64_jit_a32_execute`, a terminal helper that runs the one instruction in
the AArch32 semantic library over the TLB, with the library's fault ordering.
The helper restores the host's FPCR and FPSR unless the instruction is a memory
transfer or integer arithmetic, which leave the guest's native environment
installed for the next block, and its exclusives run over the JIT's versioned
software reservation (`a32_execute.cpp`). An entry in the middle of an IT block
and a single step run one instruction in the single-instruction arena, with the
IT state the exit recorded.

VFP and Advanced SIMD on D0-D31 keep Q0-Q15 in the A64 compiler's vector
residency (CpuState vectors 0-15 are the resident host registers V16-V31). The
register views, the S-register shadows in V0-V4 and V7 and the switches between
the guest's FPCR and the Standard FPSCR value, which follow the
[gadget engine's rule](../gadgets/README.md#aarch32), are in
`a32_compiler_vector.cpp` and `a32_compiler.hpp`. The executor installs the
guest's FPCR and FPSR for a block that holds a VFP instruction. A VCMP leaves
its flags in FPSCR's copy in `CpuState` and in the host NZCV, so an
unconditional VMRS APSR_nzcv, FPSCR right after it keeps them there and the
branch on them waits for no reload.

The JIT presents the Armv8 extensions as well, each where the host has its A64
feature. It decodes against the same profile as the gadget engine
(`a32_decode_profile`), so a form of an extension the host lacks is UNDEFINED
and never reaches the semantic library. It lowers the ones that are the host's
own instruction on the same operands (`a32_compiler_armv8.cpp`); the rest of
the level runs in the semantic library: the other half-precision forms, the
fused widening half-precision multiply-accumulates, the BFloat16 and 8-bit
matrix forms and the mixed-sign dot products, and a half-precision variant of
an Advanced SIMD floating-point form that the compiler does not lower.
