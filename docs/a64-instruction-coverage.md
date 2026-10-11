# A64 instruction coverage

The catalog contains 2,291 A64 entries: 516 Base, 467 FP/SIMD, 955 SVE, and
353 SME. Every entry has representative cases in the checked-in
[instruction corpus](../tests/unit/arm64/corpus/README.md), which runs them
through the gadget and JIT engines. Each Base and FP/SIMD case passed the
interpreter and gadget lanes, and the JIT lane where the export host implements
its features, before it was exported; the SVE and SME cases are generated from
the encoding table below, and their native results were recorded on an SME
host. `tests/unit/arm64/corpus/coverage.tsv` counts the cases of each entry,
and the corpus check fails if an entry has none. In
`tests/unit/arm64/a64_instruction_inventory.tsv`, 1,762 entries are marked
`verified`: 786 Base and FP/SIMD entries and the 976 SVE and SME entries with
an encoding in the SME profile. The other 529 are marked `absent`. The 197
Base and FP/SIMD ones need FEAT_MTE,
FEAT_GCS, FEAT_CPA, FEAT_PAuth, one of the memory extensions FEAT_LSE128,
FEAT_THE, FEAT_D128, FEAT_LRCPC3, FEAT_LSCP, FEAT_LS64, FEAT_LSFE, FEAT_LSUI,
and FEAT_LOR, one of the SIMD&FP extensions FEAT_FP8 (with its dot-product,
multiply-add, and matrix forms), FEAT_F16F32DOT, FEAT_F16MM, FEAT_F16F32MM,
FEAT_LUT, FEAT_FAMINMAX, FEAT_SM3, FEAT_SM4, and FEAT_FPRCVT, or one of
FEAT_CMPBR, FEAT_HBC, and FEAT_WFxT, which the virtual CPU omits. The 332 SVE
and SME ones need FEAT_SVE, which the SME profile lacks, or an SVE or SME
extension outside the profile, such as FEAT_SVE2p1 or FEAT_SME2p1.
Their non-hint encodings are UNDEFINED, and their hint-space forms (GCSB,
PACIASP, AUTIASP, XPACLRI, and the other PAC and AUT hints) are NOPs. Their
corpus cases expect exactly that. LDAPR and STLR stay `verified`: only their
FEAT_LRCPC3 writeback forms are UNDEFINED. So do DSB, whose FEAT_XS nXS form
is UNDEFINED, and MSR (immediate), whose FEAT_SSBS form is.

A guest learns what the virtual CPU implements from the identification
registers, which an EL0 MRS reads as on arm64 Linux, and from AT_HWCAP and
AT_HWCAP2. Each register field is set on the host predicate the engines'
native path uses, and the two words are read from those fields with arm64
Linux's own table (`guest_hwcaps()` in
[`darwin/cpu/arm64/id_registers.hpp`](../darwin/cpu/arm64/id_registers.hpp)),
so the two answers always agree. `arm64-advertised-features` pins the words,
the registers, and the `/proc/cpuinfo` Features line over named hosts, every
host feature they read toggled, and FEAT_AFP and FEAT_RPRES with and without
the host features AFP needs.

FEAT_AFP, which has no instructions, is implemented exactly where the host
implements it along with FEAT_FP16, FEAT_FHM, FEAT_BF16, FEAT_FRINTTS,
FEAT_FCMA, and FEAT_JSCVT, whose software stand-ins do not model it
(`implements_afp()` in
[`darwin/cpu/arm64/floating_controls.hpp`](../darwin/cpu/arm64/floating_controls.hpp)).
HWCAP2_AFP and ID_AA64MMFR1_EL1.AFP say so, and FPCR.FIZ, AH, and NEP are
writable. Both engines then run every floating-point instruction as the
host's own under the guest's FPCR and keep a scalar result's architectural
merge source in the host register that NEP merges from: the gadget engine
translates with FEAT_AFP twins of the entries that would not, and the JIT
reads the old destination where NEP merges into it. FEAT_RPRES, which raises
the single-precision FRECPE and FRSQRTE estimates to 12 bits under AH, follows
the host wherever FEAT_AFP is implemented, with HWCAP2_RPRES and
ID_AA64ISAR2_EL1.RPRES. Elsewhere FIZ, AH, and NEP are RES0: MSR FPCR and
every context restore clear them, and floating-point instructions behave as
if all three were zero. The other FPCR fields follow the AArch64 FPCR
description for a CPU with FEAT_FP16 and trapped exception handling but
without FEAT_EBF16 or AArch32. AHP, DN, FZ, RMode, FZ16, and the six trap
enables are writable; Len, Stride, and EBF read as zero, so BFDOT and BFMMLA
always take the standard BFloat16 behavior. The `arm64-base-system-registers`
corpus cases pin that write mask for a CPU without FEAT_AFP on every host, the
corpus's FEAT_AFP and FEAT_RPRES cases run where they are implemented, and
the guest HWCAP sanity payload checks the controls on each host.

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

One generated table decides the SVE and SME encodings for both engines:
[`darwin/cpu/arm64/detail/sme_encodings.hpp`](../darwin/cpu/arm64/detail/sme_encodings.hpp),
which `generate_sme_encoding_table.py` derives from the pinned Arm release and
LLVM MC. Its 2,929 rows cover every non-alias SVE and SME encoding. Each row
has two requirements, 0 where it never executes:
- in streaming mode, the features of the SME profile (FEAT_SME, FEAT_SME2,
  FEAT_SME_F64F64, FEAT_SME_I16I64) it needs (2,084 rows);
- outside it, the features of the SVE profile (FEAT_SVE, FEAT_SVE2,
  FEAT_SVE_AES, FEAT_SVE_PMULL128, FEAT_SVE_SHA3) it needs, or the SME
  profile's for an SME form legal in either mode (1,677 rows).

Both profiles include the SVE forms of FEAT_BF16 and FEAT_I8MM. 1,457 rows
execute in both modes, 627 only in streaming mode, 220 only outside it, and
625 in neither: FEAT_SME_FA64, FEAT_SVE2p1 and the other later extensions are
outside both profiles, and FEAT_SVE_BitPerm and FEAT_SVE_SM4 are withheld
until a workload needs them. The pseudocode lets a CPU with FEAT_SVE and
FEAT_SME execute 29 SVE encodings that FEAT_SME or FEAT_SVE2p1 added (SCLAMP,
PSEL, the predicate-pair WHILE forms and others) outside streaming mode, so
their non-streaming requirement names FEAT_SME too. Each row also records
the smallest streaming vector length at which it is allocated, the PSTATE.SM
and PSTATE.ZA state its access checks demand on a CPU without FEAT_SVE, its
register operands and groups, and its memory shape, including the gathers,
scatters, first-fault and non-fault loads that execute only outside
streaming mode. A memory shape names the vector length that sizes it: VL
outside streaming mode and SVL in it, SVL in both for the ZA array vectors,
or none for ZT0. Nothing in the table assumes a length. A companion table
classifies each FP&SIMD encoding as AdvSIMD, which traps in streaming mode,
or FP, as the 2026-06 pseudocode does on a CPU without FEAT_FPRCVT; the
pseudocode makes UMOV and SMOV of element 0 and the scalar FRECPE, FRECPS,
FRSQRTE, FRSQRTS, and FMULX FP. `sme_decode.hpp` decodes against both, with
the streaming-mode requirement: no engine executes an SVE form outside
streaming mode yet, so those words stay UNDEFINED. `arm64-sme-encoding-table`
checks the decoder against the table, every row's LLVM MC witness against its
row, and each row's requirements against its access checks. The table is a
header, not an `.inc` contract, because C++ sources may not include `.inc`
files.

FEAT_SME is two-sided. An SVE or SME entry is `verified` when one of its
encodings needs only the SME profile (FEAT_SME, FEAT_SME2, FEAT_SME_F64F64,
FEAT_SME_I16I64, and the profile's FEAT_BF16 and FEAT_I8MM forms), and so are
SMSTART and SMSTOP. Where the virtual CPU lacks SME, on a host without the
profile or with the guest-SME policy forced off, the corpus pins
SMSTART, SMSTOP, the SVCR accesses, and one word of every profile encoding and
alias UNDEFINED. The `arm64-sme-native` records hold the results the M4 host
gives for the profile's encodings at its streaming vector length, 64 bytes,
and say so; they run where an engine implements SME at that length. The
records confirm the FP&SIMD classification in streaming mode: UMOV and SMOV of
element 0 and the scalar FRECPE, FRECPS, FRSQRTE, FRSQRTS, and FMULX execute
there. MOVT between ZT0 and a general register executes only in Debug state, so
it has no table row and is UNDEFINED in every mode.

That status is not exhaustive variant proof. No inventory entry has an
independently reviewed complete partition of all forms, widths, aliases,
architectural state, optional features, reserved encodings, fault ordering, and
operand combinations. The variant-complete count is therefore 0/2,291.

## Sources of truth

- `a64_instruction_inventory.tsv` owns the stable catalog identity and status.
  The `arm64-instruction-inventory` tooling test validates it.
- `a64_instruction_allocations.tsv` and `a64_instruction_decode_partitions.tsv`
  map the pinned Arm A-profile 2026-06 release and LLVM MC 22.1.8 allocation
  space. The corpus builder attributes every case to inventory entries through
  them. The architecture oracle numbers the forms of all four index sections,
  Base, FP&SIMD, SVE (`SVE-`) and SME (`SME-`), 2,291 in all, in index order,
  so the decode partitions relate SVE and SME encodings to their forms.
- `darwin/cpu/arm64/detail/sme_encodings.hpp` maps the same release and LLVM
  MC to the SVE and SME decode both engines share. Its generator requires each
  row's witness to fall in its own encoding's decode partition, and the opt-in
  `arm64-sme-encoding-table-freshness` tooling test regenerates it.
- `a64_instruction_evidence_bindings.tsv` supplies one witness encoding for
  each allocation, and `a64_instruction_variant_contracts.tsv` supplies
  independently reviewed encoding contracts that override the decoder's
  allocation for the witnesses they match. Both are inputs of the allocation
  generator, whose `binding_encoding` column copies the witness; the
  bindings' `ctest` and `case` columns are provenance only. They name the
  pack case, by producer and case name, whose entry word is the witness, or
  `-` for both when no case is, and the corpus check enforces it.

The checked TSV files, not a Markdown checklist, are the exact inventory. The
generators live under `tests/tooling/arm64`; use their `--help` output for the
pinned regeneration inputs.
