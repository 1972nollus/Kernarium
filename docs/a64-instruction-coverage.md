# A64 instruction coverage

The catalog contains 983 A64 entries: 516 Base and 467 FP/SIMD. Every entry has
representative cases in the checked-in
[instruction corpus](../tests/unit/arm64/corpus/README.md), which runs them
through the gadget and JIT engines. Each case passed the interpreter and gadget
lanes, and the JIT lane where the export host implements its features, before
it was exported. `tests/unit/arm64/corpus/coverage.tsv` counts the cases of each
entry. In `tests/unit/arm64/a64_instruction_inventory.tsv`, 949 entries are
marked `verified`. The other 34 are marked `absent`: they need FEAT_MTE,
FEAT_GCS, or FEAT_CPA, which the virtual CPU omits. Their non-hint encodings
are UNDEFINED, and GCSB is a NOP. Their corpus cases expect exactly that.

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
