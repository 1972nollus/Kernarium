# A64 instruction coverage

The catalog contains 983 A64 entries: 516 Base and 467 FP/SIMD. Every entry has
representative reference-oracle, gadget, and JIT execution evidence, so all
rows in `tests/unit/arm64/a64_instruction_inventory.tsv` are marked
`verified`.

That status is not exhaustive variant proof. No inventory entry currently has
an independently reviewed complete partition of all forms, widths, aliases,
architectural state, optional features, reserved encodings, fault ordering, and
operand combinations. The current variant-complete count is therefore 0/983.

## Sources of truth

- `a64_instruction_inventory.tsv` owns the stable catalog identity and current
  three-engine status.
- `a64_instruction_allocations.tsv`, `a64_instruction_required_variants.tsv`,
  and `a64_instruction_decode_partitions.tsv` map the pinned Arm A-profile
  2026-06 release and LLVM MC 22.1.8 allocation space.
- `a64_instruction_semantic_contracts.tsv` and
  `a64_instruction_variant_contracts.tsv` own reviewed semantic expectations
  and completeness flags.
- `a64_instruction_aggregate_manifest.tsv` summarizes raw evidence without
  claiming completeness.

The checked TSV files, not a Markdown checklist, are the exact inventory.
Generation and validation tools live under `tests/tooling/arm64`; use their
`--help` output for regeneration inputs.

## Validation

Run both page profiles with:

```sh
cmake --workflow --preset check-instruction-evidence
cmake --workflow --preset check-instruction-evidence-4k
```

Input materialization is intentionally off in ordinary builds because a full
profile consumes several GiB. Enable it only for an explicit official-semantics
campaign. Promotion requires the qualification validator's
`--require-variant-complete` gate; representative evidence alone cannot promote
an entry.
