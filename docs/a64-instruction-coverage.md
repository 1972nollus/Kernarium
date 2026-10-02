# A64 instruction coverage

The catalog contains 983 A64 entries: 516 Base and 467 FP/SIMD. Every entry has
representative cases in the checked-in
[instruction corpus](../tests/unit/arm64/corpus/README.md), which runs them
through the gadget and JIT engines. Each case passed the interpreter and gadget
lanes, and the JIT lane where the export host implements its features, before
it was exported. `tests/unit/arm64/corpus/coverage.tsv` counts the cases of each
entry. In `tests/unit/arm64/a64_instruction_inventory.tsv`, 784 entries are
marked `verified`. The other 199 are marked `absent`: they need FEAT_MTE,
FEAT_GCS, FEAT_CPA, FEAT_PAuth, one of the memory extensions FEAT_LSE128,
FEAT_THE, FEAT_D128, FEAT_LRCPC3, FEAT_LSCP, FEAT_LS64, FEAT_LSFE, FEAT_LSUI,
and FEAT_LOR, one of the SIMD&FP extensions FEAT_FP8 (with its dot-product,
multiply-add, and matrix forms), FEAT_F16F32DOT, FEAT_F16MM, FEAT_F16F32MM,
FEAT_LUT, FEAT_FAMINMAX, FEAT_SM3, FEAT_SM4, and FEAT_FPRCVT, or one of
FEAT_CMPBR, FEAT_HBC, FEAT_SME, and FEAT_WFxT, which the virtual CPU omits.
Their non-hint encodings are UNDEFINED, and their hint-space forms (GCSB,
PACIASP, AUTIASP, XPACLRI, and the other PAC and AUT hints) are NOPs. Their
corpus cases expect exactly that. LDAPR and STLR stay `verified`: only their
FEAT_LRCPC3 writeback forms are UNDEFINED. So do DSB, whose FEAT_XS nXS form
is UNDEFINED, and MSR (immediate), whose FEAT_SSBS form is.

The virtual CPU also omits FEAT_AFP, which has no instructions, whether or not
the host implements it. FPCR.FIZ, AH, and NEP are RES0: MSR FPCR and every
context restore clear them, and floating-point instructions behave as if all
three were zero. The other FPCR fields follow the AArch64 FPCR description for
a CPU with FEAT_FP16 and trapped exception handling but without FEAT_EBF16 or
AArch32. AHP, DN, FZ, RMode, FZ16, and the six trap enables are writable; Len,
Stride, and EBF read as zero, so BFDOT and BFMMLA always take the standard
BFloat16 behavior. The `arm64-base-system-registers` corpus cases pin that
write mask.

FEAT_ECV, which also has no instructions, is implemented exactly where the
host implements it, and HWCAP2_ECV and ID_AA64MMFR0_EL1.ECV say so. Both
engines read CNTVCTSS_EL0 with the host's own instruction; on other hosts it
is UNDEFINED. CNTPCTSS_EL0, like CNTPCT_EL0, is UNDEFINED everywhere, because
Linux gives EL0 no physical count. A counter value cannot be pinned in the
corpus, so `arm64-base-system-registers` and the guest HWCAP sanity payload
check it on each host.

FEAT_JSCVT is advertised exactly where the host implements it. Both engines
run FJCVTZS with the host's instruction and correct the Z flag that some
cores, such as the M1, set for a positive subnormal that FPCR.FZ flushes. The
corpus pins FJCVTZS on flushed subnormals of both signs, and the guest HWCAP
sanity payload checks it on its special inputs.

FEAT_CSSC is implemented on every host. Both engines execute ABS, CNT, CTZ,
and the integer minimum and maximum natively where the host has the extension
and otherwise as short sequences of base instructions, so HWCAP2_CSSC and
ID_AA64ISAR2_EL1.CSSC are always set. The corpus runs its CSSC cases through
both engines on every host, and the guest HWCAP sanity payload checks one
form of each operation in the guest.

FEAT_MOPS is implemented on every host as option B: the prologue formats the
registers, the main stage moves nothing, and the epilogue moves every byte, one
host memmove or memset per source and destination page region. A CPY that
overlaps with its destination above its source copies backward. A forward-only
CPYF in that arrangement moves each region in 64-byte blocks, and the corpus
pins that result. Both engines share this code, so HWCAP2_MOPS and
ID_AA64ISAR2_EL1.MOPS are always set. The JIT translates a prologue, main and
epilogue that name the same registers as one unit: an untagged transfer that
stays inside one destination page and one source page runs inline, in the
same 64-byte blocks and direction, and any other takes the shared code from
its prologue; `arm64-jit-mops-run-runtime` checks both arms against a byte
model. The corpus runs its MOPS cases through both engines on every host.

That status is not exhaustive variant proof. No inventory entry has an
independently reviewed complete partition of all forms, widths, aliases,
architectural state, optional features, reserved encodings, fault ordering, and
operand combinations. The variant-complete count is therefore 0/983.

## Sources of truth

- `a64_instruction_inventory.tsv` owns the stable catalog identity and status.
  The `arm64-instruction-inventory` tooling test validates it.
- `a64_instruction_allocations.tsv` and `a64_instruction_decode_partitions.tsv`
  map the pinned Arm A-profile 2026-06 release and LLVM MC 22.1.8 allocation
  space. The corpus builder attributes every case to inventory entries through
  them.
- `a64_instruction_evidence_bindings.tsv` supplies one witness encoding for
  each allocation, and `a64_instruction_variant_contracts.tsv` supplies
  independently reviewed encoding contracts that override the decoder's
  allocation for the witnesses they match. Both are inputs of the allocation
  generator; the bindings' `ctest` and `case` columns are provenance only.

The checked TSV files, not a Markdown checklist, are the exact inventory. The
generators live under `tests/tooling/arm64`; use their `--help` output for the
pinned regeneration inputs.
