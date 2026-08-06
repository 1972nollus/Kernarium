# A64 instruction allocation oracle

`a64_instruction_allocations.tsv` is generated from two independent,
version-pinned sources by
`tests/tooling/arm64/generate_a64_instruction_allocations.py`:

- Arm's machine-readable A-profile A64 ISA release `2026-06`, commit
  `2026-06_rel`, downloaded from
  `https://developer.arm.com/-/cdn-downloads/permalink/Exploration-Tools-A64-ISA/ISA_A64/ISA_A64_xml_A_profile-2026-06.tar.gz`
  with SHA-256
  `63a01a1696483bbe2edfef9e0f0cd053d6c1c619ec0587876cb7a60bb344f354`;
- LLVM MC decoder version `22.1.8`.

The Arm XML resolves each selected binding word to an exact architectural
section, form, source file, encoding name, fixed-bit mask and value, supported
field constraints, and native architectural feature alternatives. The
generator rejects an archive with a different filename, digest, index count,
or release commit, and it rejects a binding whose word, form, or exact native
feature requirement is not licensed by that XML.

`a64_instruction_required_variants.tsv` is the binding-independent allocation
model. It contains all 2,396 form/decode intersection predicates for the 983
indexed Base and FP/SIMD forms: 2,168 primary decode partitions and 228
architectural aliases. Each
row is cross-linked to the terminal `encodingindex.xml` decode class and
retains both the form-page and decode-table constraints; the latter adds 109
reserved/dependent-field predicates that a form diagram alone would miss.
Operation-pseudocode and alias-declaration digests detect source semantic
drift, but allocation coverage is not treated as behavioral completeness.

`a64_instruction_decode_partitions.tsv` is generated independently from the
official decode hierarchy and instruction tables. It contains 638 allocated
hierarchy leaves, 461 unallocated hierarchy leaves, 4,334 allocated terminal
rows, and 792 unallocated terminal rows. Exact, wildcard, and `!=` predicates
are preserved, including pair-dependent reserved encodings. Regenerate these
two tables with:

```sh
python3 -B tests/tooling/arm64/generate_a64_instruction_required_variants.py \
  --architecture-archive /path/to/ISA_A64_xml_A_profile-2026-06.tar.gz
python3 -B tests/tooling/arm64/generate_a64_instruction_decode_partitions.py \
  --architecture-archive /path/to/ISA_A64_xml_A_profile-2026-06.tar.gz
```

LLVM independently decodes each selected word. The generator flips every one
of its 32 bits and records a decoder mask containing bits whose change selects
a different opcode or an invalid allocation. This keeps the architecture
allocation and decoder allocation as separate oracles rather than reproducing
the execution binding as an unchecked complete word.

`encoding_mask` and `encoding_value` are independently reviewed semantic
variant partitions when a matching row exists in
`a64_instruction_variant_contracts.tsv`; otherwise they equal the selected
Arm architecture mask and value. Semantic result contracts are intentionally
separate from this form/allocation oracle and remain incomplete until their
`entry_complete` fields are reviewed and promoted.

Auxiliary runtime gates such as `afp` describe the host/test environment and
are not claimed to come from the ISA XML. Runtime evidence records required,
observed, and missing native features separately, and validation requires an
execution exactly when none are missing and a skip exactly when some are
missing.

## Representative semantic execution contracts

`a64_instruction_semantic_contracts.tsv` records the inputs and expected
outcomes declared by the execution harness independently of the results
reported by the interpreter, gadget, and JIT engines. Its semantic identity
includes the stable ID, exact case locator and encoding, guest page-size
profile, native requirement, initial state and memory digests, expected state
and memory digests, event, retirement, fault, and syndrome. The architecture
and decoder fields are joined from `a64_instruction_allocations.tsv`; observed
host features and engine results are deliberately excluded.

The checked-in artifact contains 4,744 profile-specific contracts harvested
from the legacy representative bindings: 2,372 records for each guest
page-size profile, touching all 983 inventory IDs. These records are useful
exact regression witnesses, but are not an exhaustive semantic-variant
inventory. Run the harvester with both evidence directories so that neither
profile is dropped:

```sh
python3 -B tests/tooling/arm64/harvest_a64_instruction_semantic_contracts.py \
  --evidence-directory .build/audit-round7-unit4k/instruction-evidence \
  --evidence-directory .build/audit-round7-unit16/instruction-evidence
```

Harvesting never infers semantic completeness. It preserves reviewed flags
for unchanged semantic identities and writes new identities as incomplete.
No inventory ID is currently complete: a fixed representative word, including
NOP, cannot establish that every required allocation, operand, state, feature,
or reserved partition was enumerated.

Set `UML_INSTRUCTION_RAW_EVIDENCE=1` for an audit sweep to write an unfiltered
`.tsv.raw` sidecar for every shared-harness invocation. A 4 KiB sweep on
2026-08-04 captured 18,215,739 invocations (18,210,619 unique declared
contracts), including 6,201,334 invalid/reserved cases, from 449 evidence
producers. The generated sidecars occupied 6.4 GiB and are not checked in.
They revealed 16,627,248 unique instruction words but only 1,044 source call
sites. Durable closure therefore requires compact, officially mapped
source-site/allocation/semantic-partition aggregates with counts and canonical
digests; source site alone is not a variant boundary.

`aggregate_a64_raw_instruction_evidence.py` performs that streaming map
without using test names or the representative binding catalog for identity.
It preserves every source location, splits words that occupy multiple
official alias/form allocations, classifies unmatched words through the
official hierarchy and terminal unallocated tables, evaluates word-dependent
feature rules, and records LLVM 22.1.8 results plus declared/observed digests.
`validate_a64_instruction_aggregate_manifest.py` validates duplicate, swapped,
count, digest, feature, reserved-predicate, custom-oracle-locator, and profile
drift. The aggregate manifest is generated evidence, not an independent
behavioral oracle: strict completion additionally requires each aggregate to
bind to an independently reviewed expected-result variant for every eligible
production engine.

The normal validator checks every current-profile execution against exactly
one semantic contract and rejects missing, duplicate, or swapped contracts.
`--require-variant-complete` is the qualification gate; it intentionally
rejects the current artifact because all 983 inventory IDs still lack an
independently enumerated total partition manifest.

Regeneration requires the exact pinned Arm archive via
`--architecture-archive`; the archive itself is not checked into the source
tree. Changes to either oracle or to the selected bindings require review.
