# Instruction corpus sources

`source.py` generates the complete CPU correctness assets from the source
tables under `tests/unit/arm64/corpus/source`,
`tests/unit/x86_64/corpus/source`, and `tests/unit/x86_64/masked_flags/source`.
CMake owns materialization into each build tree. Generation uses Python's
standard library and the checked-in producers; it needs no Git database,
network, assembler, upstream checkout, execution engine, or reference hardware.
The unit binaries continue to consume only their generated asset.

Each `index.json` explicitly lists its inputs and fixes record order. JSON
lines files group programs and golden expectations by producer. Bounded
`$table` loops factor allocation matrices into constant fields, counters,
instruction bit fields, name formats, and repeated operand pools. Irregular
columns remain literal values. Record defaults factor common case fields,
state defaults factor common architectural fields, and memory defaults factor
common region descriptors. `$omit` removes a default field. A64 programs retain
their entry offsets and instruction words;
x86 programs retain their instruction bytes and named-register contracts or
per-instruction architectural deltas. These are test inputs, not engine output.

Byte recipes generate repeated lanes, masked byte ramps with block increments,
sparse patches, and the native request's integer hash fill. Large FEX programs
use loops over their accepted instruction bytes, including displacement and
immediate fields; ordinary generation does not assemble upstream sources.
State recipes generate regular register and vector fills; irregular operands
and results remain explicit. `$runs` generates equal memory writes at a fixed
stride. Recipes describe test data only and do not decide instruction semantics.

The maintained SME/SVE native generators supply their programs, allocation
loops, and initial states. Their source tables retain the independently
observed results, retirement counts, events, and memory deltas. The A64 SVE
trap records are derived by their maintained architecture generator from the
native SVE golden records rather than stored as expanded cases.

The versioned manifests, coverage and provenance stay beside the source tables.
Generation must reproduce their accepted asset SHA-256 before publishing an
output. Every case, profile, observation, validator and retirement expectation
is retained. Updating native expectations still requires the independently
observed hardware results described by each corpus README.

The AArch32 corpus (`--kind aarch32`, `tests/unit/arm64/aarch32_corpus/source`)
has no pack to import: `tools/arm64-tests/a32-corpus.py` writes its source
tables from oracle runs, and `aarch32.py` generates the pack from them. Its
README owns the format.

To promote an accepted producer output to source tables:

```sh
python3 -B tools/instruction-corpus/source.py --kind arm64 \
  --import-pack OUTPUT/a64_instruction_corpus.pack \
  --source tests/unit/arm64/corpus/source
```

Use `x86` or `masked-flags` for the other assets and their owning source
directory. The import also carries the producer's manifests and provenance.
Commit the source tables and metadata, never the generated pack or binary.
Literal golden observations still occupy more checkout space than compressed
packs, but Git can compress and delta individual source files across updates.
