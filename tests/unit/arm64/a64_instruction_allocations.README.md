# A64 instruction evidence tables

The TSV files in this directory separate four concerns:

- catalog identity and three-engine status;
- architectural and decoder allocation;
- representative semantic expectations; and
- reviewed variant completeness.

Allocation data is generated from the pinned Arm A-profile A64 ISA 2026-06
archive and LLVM MC 22.1.8. The generators verify the source archive identity,
map every selected word to the official form/decode constraints, and keep
host/test feature gates separate from architectural requirements.

`a64_instruction_inventory.tsv` is the stable 983-entry catalog.
`a64_instruction_required_variants.tsv` and
`a64_instruction_decode_partitions.tsv` describe allocation partitions.
`a64_instruction_semantic_contracts.tsv` records representative expected
outcomes, while `a64_instruction_variant_contracts.tsv` owns reviewed
completeness. `a64_instruction_aggregate_manifest.tsv` summarizes raw evidence
without becoming an independent semantic oracle.

No entry is currently variant-complete. Representative executions across the
interpreter, gadgets, and JIT are regression witnesses, not proof of every
operand, alias, feature, state, reserved, and fault partition.

Generators and validators live under `tests/tooling/arm64`. Their command-line
`--help` is the canonical regeneration interface. Any generated-table change
must be reviewed with its pinned source identity and both page-profile evidence
workflows described in
[`../../../docs/a64-instruction-coverage.md`](../../../docs/a64-instruction-coverage.md).
