# A64 Base and SIMD/FP instruction implementation progress

> Historical execution log. References to `rootfs.ext2` describe pre-refactor
> results; use the immutable `rootfs.img` workflow documented in
> `docs/testing.md` for current work.

Source: `ISA_A64_xml_A_profile-2026-06.pdf`, Arm A64 ISA XML for A-profile architecture, 2026-06, document 111182 (ID102025).

Scope: all entries in the PDF inventories `A64 -- Base Instructions (alphabetic order)` (516 entries) and `A64 -- SIMD and Floating-point Instructions (alphabetic order)` (467 entries). Aliases, grouped mnemonics, scalar/vector forms, addressing forms, element widths, architectural feature variants, and every distinct encoding described by an entry must be tested before that entry is complete.

## Enabling input materialization

Input-materialization sidecars are **opt-in** and off by default. Every unit
test writes one `<producer>.tsv.inputs.jsonl.gz` when they are enabled, and only
`arm64-input-materialization-join` reads them. A complete 16 KiB profile costs
roughly **6.6 GiB** of compressed sidecars against 3.4 MiB of ordinary evidence,
so an ordinary unit or tooling run must not pay for them.

Configure with the option to record sidecars and register the join:

```sh
cmake -S . -B .build/presets/macos-unit-16k \
  -DUML_INSTRUCTION_INPUT_MATERIALIZATION=ON
```

The producers and the join share this one option, so the join can never be
selected without the inputs it requires. With the option off the suite
registers 618 tests and leaves a 12 KiB evidence directory; with it on the
suite registers 619 and regenerates the full profile.

Two known costs apply when it is enabled:

- The spool capacity policy is `expected_contributions * 1024 + 4 MiB` per
  producer, which is too small for producers whose rows carry long
  native-feature alternative lists. `arm64-atomic-floating-allocation`
  overflows its 165,044,224-byte cap at 164,941,824 bytes and reports
  `contribution spool exceeded its hard byte cap`. Pass
  `--spool-bytes-per-contribution 2048` to clear it. Note that the same message
  is also produced for genuine disk exhaustion, because the handler catches
  every `sqlite3.DatabaseError`.
- The sidecar codec is zlib level 6, chosen in
  `tests/unit/arm64/arm64_instruction_materialization.cpp`. Measured on a real
  120 MB sample, gzip level 9 recovers only 2.4%, while `xz -1` is **1.98x
  smaller at comparable write cost** (1.1 s versus 0.9 s, and 0.3 s to read)
  and `zstd -3` is **2.13x smaller and 9x faster**. Either would roughly halve
  the 6.6 GiB profile, but both need a vendored compressor: neither `lzma.h`
  nor `libzstd` is present in the macOS SDK.

## Frozen aggregate-manifest publication

The frozen manifest and identity are published only as one complete two-profile
operation. Partial producer replacement and validator-side writes are not
supported, so neither file can be refreshed through a weaker path:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  tests/tooling/arm64/freeze_a64_aggregate_manifest.py \
  --full-4k-manifest /absolute/path/to/full-4k.tsv \
  --full-16k-manifest /absolute/path/to/full-16k.tsv \
  --evidence-4k /absolute/path/to/4k/instruction-evidence \
  --evidence-16k /absolute/path/to/16k/instruction-evidence \
  --output tests/unit/arm64/a64_instruction_aggregate_manifest.tsv \
  --identity-output \
    tests/unit/arm64/a64_instruction_aggregate_manifest.identity.json
```

Full publication enumerates the exact 449 `*.tsv.raw` producer basenames in
each evidence directory, independently regenerates each profile aggregate from
those raw files with the checked required/decode models and pinned decoder,
and requires the supplied profile TSV to match that derivation exactly. It then
validates both profile sources, their exact page/cardinality, producer and
identity sets, canonical ordering, allocation model, and declared profile
exceptions before staging the bound manifest/identity replacement.
An interrupted replacement is fail-closed because consumers reject a mismatched
pair, and rerunning the same command resumes from the validated profile inputs.

Large completed raw profiles can be preserved losslessly in deterministic gzip
form after their aggregate has been validated:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  tests/tooling/arm64/compress_a64_raw_evidence.py \
  --evidence-directory /absolute/path/to/instruction-evidence \
  --guest-page-size 16384 \
  --free-space-reserve-gib 8
```

Readers accept exactly one `producer.tsv.raw` or `producer.tsv.raw.gz` for each
of the frozen 449 producer names. They reject missing producers, symlinks,
truncated streams, and duplicate plain/compressed basenames. Compression holds
a directory-scoped lock, reserves a conservative hard upper bound of twice the
raw size plus 1 MiB for its temporary gzip, and verifies the staged member
against the original uncompressed size and digest before removing the plain
file. A matching duplicate left by an interrupted replacement is verified and
resumed, while a mismatch fails closed.
The emitted canonical compression identity records every producer's raw and
compressed size/digest.

## Exact official-semantics campaign gate (not yet executed)

The bounded campaign, sharding, authority-reconstruction, cancellation/resume,
and two-profile promotion tooling is implemented. The multi-day official run
has **not** been executed, so it does not close or reaffirm any historical
instruction checkbox below. The canonical, copy/paste executable 4 KiB +
16 KiB command chain is
[`docs/testing.md`](docs/testing.md#exact-official-a64-campaign). It starts
from fresh unit evidence and includes materialization joins, authority-filtered
sharding, immutable plans, an exact four-shard pilot per profile, full resume,
independent verification, locked promotion, and promotion `--verify-only`.

Current frozen aggregate identity (2026-08-05):

- 32,169 aggregate rows: 16,084 for 4 KiB and 16,085 for 16 KiB;
- aggregate-ID SHA-256
  `00ebf4703abb2e9c59050d4e7f00418dcb6b5f61f0e61dddbe566c51f2057d66`;
- aggregate-row SHA-256
  `f42d770d290b88135f5da3eb04793d516cd89723e614b4e2911499fcc5541c0`.

These are aggregate counts, not official input-contract counts. Never reuse a
prior or estimated contract total. Each fresh sharding pass writes its exact
authority-filtered `total_contracts` to `shard-set.json`; that value is then
re-supplied to sharding and planning through `--expected-total-contracts`.

The campaign implementation now has the following fail-closed properties:

- checkpoint authority is reconstructed with a bounded fan-in merge (maximum
  32 live streams) and aggregate-sized memory, while raw/input bindings remain
  disk-backed and the per-input lookup is forced through the
  `authoritative_binding_by_input` index;
- sharding reconstructs one producer at a time, caps shards at 1,024 unique
  authority-bound contracts, holds canonical exclusive evidence locks for its
  full source/authority/publication snapshot, emits deterministic gzip, and can
  regenerate or byte-verify a partially published shard set;
- plan schema v4 freezes the exact Python runtime/source closure, official
  tools, checkpoints, aggregate identity, shard identities, and hard limits
  for encoded input, artifact bytes/files, and captured logs;
- pilot/run reserve the frozen per-worker output/local maximum before every
  submission and recheck after every completion. A failure consumes the whole
  completed batch before any refill, cancels queued work, terminates active
  probe process groups, and removes only its uniquely owned local partials;
- published shard trees are immutable resume points. Low-space and in-flight
  interruption fixtures prove no campaign report is emitted, no child/temp
  debris remains, and a later run preserves every prior `SUCCESS.json` byte;
- all metadata publication is file-before-directory synced. The production
  chain uses `--directory-sync-policy require`, so unsupported directory
  `fsync` is fatal rather than silently weakening evidence;
- final promotion locks both profile roots in canonical order, reconstructs
  both exact authorities under explicit database/scratch caps, stores results
  in locally rooted SQLite, streams indexed bindings aggregate by aggregate,
  publishes a bound TSV/identity pair, and verifies the same pair without
  repair.

Bounded scale evidence from the 2026-08-05 fixture run merged 262,144 sorted
checkpoint contributions from 64 synthetic producers with fan-in 8. Peak
Python allocation was 3,743,973 bytes and peak merge scratch was 1,500,707
bytes. A separate source-stream fixture processed 32,768 producers one at a
time, retained 1,024 authority IDs in a 16,384-byte SQLite file, and verified
the input-leading authority query plan. The promotion fixture then streamed
65,536 indexed input/result bindings into 64 aggregate summaries in a peak
68,354,048-byte database (1,043 measured bytes/input); measured Python peaks
were 45,987 bytes for 4,096 inputs and 49,687 bytes for 65,536, with SQLite
temporary storage pinned to the checked local root. Campaign capacity rounds
that measurement up to 1,536 bytes/input, models 40,000,000 inputs with a 3/2
safety factor (92,160,000,000 bytes), and defaults to a 137,438,953,472-byte
hard database cap. These fixtures establish bounded mechanics; they are not
substitutes for the fresh 449-producer production joins or the official
campaign.

Production completion checklist (all remain open):

- [ ] Run fresh 4 KiB and 16 KiB unit evidence and prove exactly 449 v4
  materialization sidecars per profile.
- [ ] Finish and verify both complete materialization joins/checkpoint sets.
- [ ] Derive, publish, and exact-resume both deterministic shard sets; record
  their fresh totals and identities.
- [ ] Build both immutable v4 plans with the source PDF/archive and official
  tool closure frozen.
- [ ] Pass each four-worker, four-full-shard pilot without a campaign report.
- [ ] Resume both complete campaigns, then run independent reserved-scratch
  verification.
- [ ] Publish and `--verify-only` the locked two-profile promotion pair.
- [ ] Only after all prior gates pass, review whether the exact promoted rows
  justify changing any instruction-completion status.

> **Seventh-audit qualification (2026-08-04):** the checkmarks below are a
> historical implementation ledger, not current mechanical proof that all
> variants are complete. The evidence schema currently permits exactly one
> record for each of the 983 stable entry IDs and credits only its selected
> case/word. It therefore cannot prove exhaustive coverage of every width,
> form, alias, state combination, valid/reserved partition, or feature variant.
> In addition, the allocation generator derives its form label and selected
> word from test-owned catalogs; its external decoder classifies only that
> selected word. Separately, feature validation compares requirements copied
> between test-owned inventory and binding fields. Whole-pipeline consistent
> mutations can still validate. Closure now
> requires independent variant identities plus an authoritative form-to-
> allocation and allocation-to-feature oracle, with removal, duplication,
> cross-variant-swap, reserved-field, and regenerated-catalog negatives. Do not
> treat or reaffirm the checked boxes as complete until those gates pass for
> both page modes and the exact production objects of all three engines. The
> shared fault model must also preserve instruction-fetch versus data-read
> direction through the runtime/kernel boundary, and translated-engine
> exception tests must prove
> architectural/host state cleanup before these boxes can close.

## Target closure rules

After the seventh-audit gates above pass, an entry can be complete only when
every independently identified required variant satisfies all five fields
below. Five checked boxes on one representative case are not sufficient:

- `Spec`: encoding constraints and operational pseudocode audited against the 2026-06 PDF.
- `Interpreter`: pure C++ behavior is implemented only for the debug interpreter configuration; it validates reserved encodings, alignment, accesses, and architectural constraints before side effects, favoring early diagnostics and safety over speed.
- `Gadget`: production emits only precompiled, data-only gadgets with no interpreter fallback. Encoding-constant choices (operation, width, ordering, feature form, and ZR/discard cases) are selected at translation time; the valid hot path keeps only unavoidable data-dependent and TLB/fault branches and uses exact host extensions where available.
- `JIT`: the native engine executes every allocated variant supported by the
  running host and rejects every invalid allocation. A case is skipped only
  when its specification-derived native feature requirement is absent.
- `Tests`: one independent specification oracle supplies the expected full
  architectural state, event/fault metadata, and memory image, then the same
  case runs interpreter, gadgets, and JIT in that order.

## Historical three-engine conformance campaign (2026-07-30)

- [x] Added a canonical 983-row inventory at
  `tests/unit/arm64/a64_instruction_inventory.tsv`, with stable IDs,
  section/label identity, shared-suite assignment, variant dimensions, and
  specification-derived native feature expressions.
- [x] Added a validator that proves the inventory is exactly 516 Base plus 467
  FP&SIMD rows and remains in exact order with this ledger.
- [x] Added one reusable `EngineHarness` that restores identical code, state,
  mapped memory, and allocation context before running interpreter, gadgets,
  and JIT. It compares the complete architecture-visible `CpuState`, terminal
  event, optional fault metadata, code image, and data images.
- [x] Native eligibility is detected centrally from the running host. Optional
  features fail closed; invalid encodings still run through JIT; interpreter
  and gadget validation is never skipped because a native feature is absent.
- [x] The JIT one-to-one contract is now checked independently of its decoder:
  every ordinary Base and FP/SIMD matrix case requires a native compute opcode
  class after register remapping, rejects accidental native memory/system/
  exception/control encodings, confines direct branches to the generated
  translation, and permits helper calls only through an immutable
  translation-owned literal.
- [x] Exact constant construction skips default leading halfwords and selects
  the one-filled alternative only when strictly shorter. The all-ones
  continuation sentinel is one instruction rather than four. The campaign's
  one-store TLB shapes were **32 hot / 76 total words** at 16 KiB and **34 hot
  / 74 total words** at 4 KiB.
- [x] A diagnostic-only GPR-pressure build records resident high-water,
  allocation refusals, overflow loads/stores, repeated accesses, and projected
  avoided accesses at larger capacities. Its saturation control reports
  high-water 8, 16 refusals, 8 loads, 8 stores, 8 repeats, and projections
  2/4/7. Enabled and disabled controls emit byte-identical production words
  and metadata; the normal binaries contain no diagnostic hook.
- [x] The production compiler now observes actual GPR accesses during its
  first translation pass and retries only when choosing a different stable
  eight-register resident set has a strict projected benefit. The retry is
  retained only when architectural metadata is identical, the hot prefix is
  strictly smaller, and total code is no larger. A representative pressure
  block removes six state loads and three intermediate stores, contracting by
  exactly nine hot words; low-pressure and zero-benefit controls remain
  byte-identical.
- [x] Translation frequency-selects at most two constrained indexed-vector
  source identities and prebinds them to q6/q7, stopping its scan at the
  translated terminal. The valid hot path contains only the remapped native
  semantic instruction after first live-in—no runtime decode, feature check,
  validity branch, or migration move. A third identity uses the existing
  cache-aware q2 fallback without splitting the block.
- [x] The cold TLB bridge preserves q6/q7 with the other resident state; its
  frame grows only off the hot path. Forced miss, direct-link, full-bank,
  alias, third-identity, terminal-boundary, and cold-fault controls cover the
  new bindings.
- [x] Ordinary vector allocation keeps q16-q31 first, then uses only unbound
  q6/q7 slots after the high bank fills. This requires no generated branch,
  check, decode, or new boundary work. An 18-vector pressure block contracts
  from **58 hot / 60 total words** to **55 hot / 58 total words**, while its
  low-pressure control remains **22 hot / 24 total words**.
- [x] Feature-specific state transfers fail closed at translation. Forced
  feature-off/on matrices cover every SSBS, DIT, TCO, SM/ZA immediate and
  every FPMR transfer register; supported paths contain no runtime feature
  check.
- [x] The shared harness supports single instructions, sparse multi-instruction
  programs, branches across translated blocks, mapped read/write/execute
  regions, allocation tags, and fault-atomic memory checks.
- [x] Gadget translation options are injectable through the same harness.
  Forced portable-path suites therefore still execute interpreter, gadget,
  and JIT in order against one oracle instead of using a separate gadget-only
  runner.
- [x] That campaign's migrations covered Base add/subtract, logical, move-wide,
  bitfield/extract, scalar bit operations, atomic/exclusive operations,
  conditional data processing, control flow, barriers, memory transfers, and
  checksum-like operations, plus substantial scalar and vector FP&SIMD
  families. These cases use independent arithmetic/bit/byte/memory models
  rather than treating any execution engine as an oracle.
- [x] Vector bitwise-register operations now exhaust all 524,288 combinations
  of operation, arrangement, and three register fields, plus 2,448 focused
  known-answer and alias cases. All three engines passed both page
  configurations with no native skip or unavailable case.
- [x] Base carry arithmetic, variable shifts, division, and multiply families
  now cover every width, operation, register/ZR alias, flag result, edge
  value, and reserved encoding in four arithmetic shards and three multiply
  shards. Their 27,537 cases per engine pass both page configurations.
- [x] Base control, condition, checksum, and flag-manipulation families now
  cover all allocated immediate/register fields, aliases, zero/discard forms,
  state inputs, reserved partitions, native feature gates, and portable
  gadget selections. Vector rearrangement and two-register-misc families
  likewise cover every allocated arrangement and scalar/vector hole. All
  focused suites pass both page configurations.
- [x] The four high-narrow vector families now exhaust all operation,
  lower/upper, element-size, and register fields, explicit register aliases,
  rounding/carry/underflow boundaries, and reserved size encodings. Their
  direct-offset gadgets have no runtime decode or conditional branch, and all
  2,744 cases per engine pass both page configurations.
- [x] DMB, DSB, ISB, PSSBB, and SSBB now cover every standard option, every
  nXS scope, both aliases, all architecturally reserved-but-executing options,
  and ordered multi-instruction sequences. Native nXS and SB selection is
  resolved at translation; each selected hot path is branch-free.
- [x] Signed and unsigned scalar minimum/maximum now exhaust both widths, all
  immediate values, every register field, zero/discard and destructive
  aliases, boundary values, and the complete reserved encoding partition.
  Translation selects one of 18 branch-free direct gadgets; native cases are
  skipped only when the running host lacks the required extension.
- [x] The primary architectural-hint space, BTI, CLRBHB, CSDB, DGH, ESB,
  GCSB, GSB, PSB, SB, SEV/SEVL, SHUH, TSB, WFE/WFI, and YIELD now cover every
  immediate, feature version, alias, reserved SB/GSB encoding, full-state
  preservation case, and the static EL0 GSB exception. Five live hint
  gadgets are selected during translation and contain no conditional branch.
- [x] STSHH and STCPH now own their three formerly anonymous primary-hint
  encodings in dedicated allocation, behavior, exact-feature, and gadget
  suites. All full-state cases pass both page configurations; this host
  records 15 exact valid-case JIT feature skips. Translation folds all three
  to the common branch-free NOP gadget, and the generic HINT suites no longer
  duplicate their 15 cases.
- [x] LDGM, STGM, and STZGM now exhaust every destination/source and base/SP
  register field in 3,072 full-state EL0 exception cases. Both page
  configurations pass interpreter and gadgets, while JIT records exact
  valid-case host-feature skips on this host. Translation folds all three
  operations and every register combination to the common unconditional
  illegal terminal before observing registers or memory.
- [x] The four 64-byte atomic load/store rows now cover all 25,344 allocated
  field combinations and all 42,240 reserved transfer-register combinations,
  plus operation-specific behavior, base/source/status aliases, accumulator
  substitution, address/tag controls, alignment, permissions, unmapped
  accesses, invalid-precedence rules, and exact no-partial-commit faults.
  Their 67,646 cases pass interpreter and gadgets in both page
  configurations; 42,244 invalid or reserved cases execute through JIT and
  25,402 valid cases are exact host-feature skips. Translation selects among
  exactly 12 fixed production gadgets with direct state offsets and no
  runtime opcode or register-number branches; all definitions and transfer
  helpers have live references.
- [x] The 40 floating-to-integer, narrowing, and JavaScript-conversion rows
  now use split independent oracles for every scalar/vector form, precision,
  width, signedness, fixed-point scale, register field, rounding mode,
  merge/control mode, boundary, exception flag, and reserved partition.
  Their 28,863 cases pass interpreter and gadgets in both page
  configurations; 23,927 execute through JIT and 4,936 are exact native
  feature skips. The audit also proves that the bit-21-set cells beside the
  8-bit conversion allocations remain owned by the older vector maximum
  family rather than being shadowed as false illegal encodings.
  Translation resolves operation, register offsets, shape, and native versus
  portable selection before execution, and the assigned gadget
  definition/reference audit is clean.
- [x] Added a guest synchronous floating-point exception event, syndrome, and
  signal path. Twenty-six split scalar, vector, and control cases verify
  enabled exceptional operands, result/NZCV rollback, cumulative status,
  deferred state, and restoration of the host floating environment through
  interpreter, gadgets, and JIT. A separate runtime test verifies delivery as
  `SIGFPE`; terminal gadget accounting also covers an exception reached
  through a linked successor block.
- [x] Scalar and vector fused floating arithmetic now covers every precision,
  arrangement, register field, destructive alias, operation sign, reserved
  shape, single-rounding witness, merge behavior, default-NaN behavior, and
  cumulative exception flag. Its 22 live gadgets consume direct vector
  offsets, removing all runtime register-field decode.
- [x] The twelve signed and unsigned widening add, subtract, and absolute-
  difference families now exhaust every lower/upper-half form, element size,
  register field, destructive alias, wrap boundary, and reserved scalar/size
  partition. Translation selects 72 live direct-offset gadgets; each valid
  hot path is branch-free and contains eight instructions, or nine when it
  reads and updates an accumulator.
- [x] FRECPE, FRECPS, FRECPX, FRSQRTE, and FRSQRTS now exhaust every scalar
  and vector allocation, element width, arrangement, register field, reserved
  one-lane double-width encoding, estimate bin, subnormal normalization
  position, exponent field, special-value pair, rounding mode, merge mode,
  and cumulative exception result. Translation selects 70 live direct-offset
  gadgets, with an exact host path and a checked path for older hosts.
- [x] The 27 scalar immediate, unprivileged, and unscaled load/store entries
  now exhaust every allocated size, addressing mode, immediate field,
  register/SP/ZR alias, writeback-overlap case, access permission, page
  boundary, fault-ordering rule, and fault-atomicity result. Translation
  selects 217 live direct-offset gadgets; ordinary-register hot paths contain
  no SP-alignment branch and SP-specific paths contain only the required
  alignment branch.
- [x] The scalar register-offset load/store entries and register-offset PRFM
  have an additional 31,548 cases per engine covering every extension,
  scaled/unscaled form, register field, alias, value boundary, unaligned and
  cross-page transfer, permission and mapping fault, reserved partition, and
  SP-alignment ordering rule. Translation selects exactly 312 live
  direct-offset transfer gadgets; ordinary-register paths contain no
  alignment branch, SP paths contain only the required branch, and PRFM
  becomes a non-faulting translation-time hint.
- [x] BCAX, EOR3, RAX1, and XAR now exhaust all 4,227,072 combinations of
  operation-specific register and immediate fields, plus focused truth-table,
  destructive-alias, edge-pattern, and neighboring reserved-partition cases.
  Translation selects one of 134 live direct-offset gadgets, resolves the
  immediate and native/portable choice before execution, and leaves every
  valid hot path branch-free. The portable gadget bodies ran through the same
  three-engine oracle even on a capable host.
- [x] The 27 indexed and vector integer multiply rows now cover every
  allocated element size, arrangement, lower/upper source half, indexed lane,
  register field, destructive alias, modular boundary, rounding boundary,
  two-stage saturation result, cumulative status result, and reserved
  scalar/size partition. Their 15,780 cases per engine pass both page
  configurations. Translation selected exactly 534 live native gadgets on the
  campaign host, resolved register fields and indexed lanes before execution,
  and keeps ordinary hot paths branch-free.
- [x] PMULL and PMULL2 now cover all 131,072 allocated combinations of
  lower/upper source half, element size, and three register fields, together
  with alias, boundary-value, forced-portable, and complete reserved-size
  partitions. Their 5,616 batched programs pass all three engines in both
  page configurations. Translation resolves all register offsets and the
  host-native/portable choice before execution; six live gadgets remain,
  with no conditional branch in either hot path.
- [x] The 25 horizontal floating arithmetic and min/max rows now cover all 92
  allocated forms, every register field and destructive alias, all reserved
  type/size/arrangement partitions, pair and reduction order, rounding,
  signed-zero, NaN, default-result, merge, flush, alternate-mode, and
  cumulative exception behavior. Their 12,901 cases pass interpreter and
  gadgets in both page configurations; 11,622 run natively and 1,279 are
  exact host-feature skips. The audit fixed half-precision input flushing,
  alternate number-min/max output flushing, and deferred input-denormal
  reporting. Translation selects 195 live gadgets, resolves register offsets
  before execution, and leaves only the necessary old-host control check.
- [x] The 13 allocation-tag arithmetic and memory rows now cover their
  complete immediate, register, addressing, writeback, alignment, tag,
  permission, alias, fault-ordering, and reserved partitions. The audit fixed
  tag-only unaligned stores and malformed arithmetic/memory allocations before
  side effects. Their 618,289 cases pass interpreter and gadgets in both page
  configurations; 24 run natively and 618,265 are exact host-feature skips.
- [x] The ten signed and unsigned integer-to-floating rows now cover all
  scalar, general-register, fixed-point, and vector forms, including every
  precision, scale, register field, rounding/control mode, exceptional input,
  alias, and reserved partition. Their 81,502 cases pass interpreter and
  gadgets in both page configurations; 72,870 run natively and 8,632 are
  exact host-feature skips.
- [x] The seven signed, unsigned, and mixed vector product-accumulate rows now
  cover every allocated vector shape, register field, indexed lane, alias,
  signedness combination, modular boundary, reserved scalar/size partition,
  and forced portable path. Translation resolves direct vector offsets and
  native/portable selection before execution. Their 2,297 cases pass
  interpreter and gadgets in both page configurations; 1,289 run natively and
  1,008 are exact host-feature skips.
- [x] The three integer vector matrix-product rows now exhaust all 98,304
  combinations of their three register fields, every destructive alias,
  signedness form, matrix position, arithmetic boundary, and all 32 members of
  the surrounding Q/U/size/operation allocation. Translation resolves three
  direct vector offsets and native/portable selection before execution; all
  six live gadget bodies are branch-free. Their 98,462 cases pass interpreter
  and gadgets in both page configurations; 29 invalid allocations execute
  through JIT and 98,433 valid cases are exact host-feature skips.
- [x] The nine Guarded Control Stack rows now cover every accessible register
  field, EL0-only privilege behavior, stack-switch token format, atomic
  compare-and-replace result, direct-store address and source alias, access
  control, alignment, permission, malformed-data, and decoder-neighbor
  partition. Their 4,470 cases pass interpreter and gadgets in both page
  configurations; seven invalid neighbors execute through JIT and 4,463 valid
  cases are exact host-feature skips. Translation specializes operation,
  register offsets, XZR, and SP choices, and all 12 live gadget entries have
  corresponding production references.
- [x] Nine feature-dependent system-alias rows now cover all 25 GIC
  operations, both GICR operations, both branch-record operations, the four
  prediction-restriction operations, APAS, TRCIT, and every Rt field. Their
  1,120 cases pass interpreter and gadgets in both page configurations and
  are exact native-feature skips on this host. The refactor also removed 386
  legacy cases that had incorrectly treated valid feature-dependent
  encodings as baseline-invalid; reserved neighboring SYS tuples remain in
  the generic allocation suite and continue through all three engines.
  Production translation folds every unavailable alias to the common
  unconditional illegal terminal with no runtime opcode work.
- [x] CHKFEAT now has a dedicated 68-case selector oracle covering every
  selector bit plus zero, all-one, and alternating patterns. The
  feature-absent interpreter and gadget behavior is an exact NOP, and
  translation folds it to the common NOP entry without runtime work; every
  valid JIT case is an exact host-feature skip. Five narrower legacy hint
  cases were removed when ownership moved to this dedicated suite.
- [x] AT now has a dedicated 544-case oracle covering all 17 operation names
  and every source-register field. The 12 baseline operations execute through
  JIT while the five optional operations use exact per-feature native skips;
  all forms are EL0-illegal in each executing engine. Translation folds every
  operation to the common unconditional illegal terminal with no runtime
  opcode work. The generic SYS allocation suite retains reserved neighbors
  but no longer duplicates or misclassifies named AT operations.
- [x] IC now has dedicated split suites for IALLUIS, IALLU, and IVAU across
  every source-register field. All 96 cases execute through all three engines
  in both page configurations: the first two operations are exact EL0
  exceptions, while IVAU preserves architectural state. Translation folds
  the privileged forms to the common illegal terminal and IVAU to the common
  NOP gadget. The generic SYS suites retain surrounding tuples but no longer
  duplicate the 40 named-IC cases they previously owned.
- [x] All 40 memory copy/set rows now have a shared 131,735-case oracle:
  every operation option, the full register-allocation matrices, all three
  stages, boundary lengths, tagged direction selection, overlap, cross-page
  execution, exact fault/restart state, malformed stage state, and all 392
  reserved allocation cells. Both page configurations pass interpreter and
  gadgets; JIT executes all 21,724 invalid or constrained cases and records
  110,011 exact valid-case host-feature skips. Translation resolves operation,
  stage, option, and direct state offsets before execution. All ten production
  gadget entries and ten transfer helpers are referenced, with no dead
  family-specific gadget symbols.
- [x] Scalar floating-point precision conversion now exhausts all six source
  and destination precision pairs, both merge modes, every source and
  destination register field, all nine reserved type-pair allocations, every
  rounding mode, representative classes and bit patterns, overflow,
  underflow, alternate-number controls, default-result controls, input/output
  flushing, cumulative status flags, and destructive aliases. Its 27,844
  cases pass interpreter and gadgets in both page configurations; 20,347 run
  through JIT and 7,497 are exact host-feature skips. Reserved encodings still
  execute through JIT and now terminate with the architectural illegal event.
  Translation selects exactly six live precision-specific gadgets and places
  direct vector offsets in the immutable stream, removing one runtime
  register-field decode instruction.
- [x] DC, SYS, SYSL, SYSP, MRRS, MSRR, TLBI, and TLBIP now use one
  PDF-derived ownership table with exact 40/40 data-cache operations, 166/166
  single-register invalidation operations, and 120/120 paired invalidation
  operations. Split suites cover every operation tuple, register field,
  legal pair, feature version, and reserved namespace. Their replacement
  adds a net 2,151,708 interpreter/gadget cases, 1,093,742 JIT executions,
  and 1,057,966 exact host-feature skips after removing 71,734 duplicated
  generic cases and extending GSB across its previously omitted register
  fields. Both page configurations and all adjacent system suites pass;
  profile-illegal families translate directly to the common terminal gadget,
  while cache operations resolve to cache-zero, NOP, or terminal behavior
  during translation.
- [x] Scalar and vector single-to-reduced-precision conversions now share an
  independent bit-level oracle covering both scalar merge modes, both vector
  halves, every source/destination register pair, destructive aliases, all
  rounding modes, representative number classes, alternate-number controls,
  input flushing, default results, cumulative status, overflow, and 256
  deterministic source patterns per form. All 5,395 cases pass interpreter
  and gadgets in both page configurations; the campaign host recorded 5,395
  exact native-feature skips, while the JIT emits native forms when that
  feature is present. Translation now places direct vector offsets in the
  immutable gadget stream, and all four family gadget entries are live.
- [x] All 33 ordered scalar, pair, and vector-memory rows now share one
  specification oracle covering every access size, addressing form, register
  allocation and alias, legal feature version, reserved partition, cross-page
  access, exact fault state, and load/store ordering contract. Their 71,927
  cases pass interpreter and gadgets in both page configurations; 53,306 run
  natively and 18,621 are exact feature skips. A reusable isolated native
  probe resolves hosts whose feature query omits a supported instruction
  without changing the parent signal state, moving 8,722 cases from false
  skips into JIT execution. All 144 owned production gadget entries remain
  referenced.
- [x] All 30 pointer-authentication rows now use split shared-engine suites
  covering every key/form combination, register field, alias, modifier,
  signed/unsigned result, enhanced return-address form, authenticated load,
  control transfer, fault, and reserved encoding. The 418,312 cases pass
  interpreter and gadgets in both page configurations; 155,946 execute
  natively and 262,366 are exact host-feature skips. Optional guarded-stack
  branch/return behavior is selected into dedicated gadgets during
  translation, including exact record-memory faults and failure atomicity,
  without adding checks to the default production entries. All 63 owned
  gadget symbols are live.
- [x] All three complex-vector rows now share a 17,826-case oracle covering
  every shape, rotation, indexed source, register field, destructive alias,
  legal allocation, reserved control cell, rounding mode, default-result and
  invalid-status behavior, input/output flushing, and cumulative flags.
  Every case passes all three engines on this host in both page
  configurations. JIT now emits the native operation directly, while
  translation selects all 62 distinct live gadgets and stores direct vector
  offsets in the immutable stream, reducing valid-hot-path register/address
  setup from seven instructions to three in each gadget.
- [x] MSR (immediate) now has split allocation, behavior, access, and
  gadget-shape suites driven by a direct transcription of the complete PSTATE
  field table. All 1,903 cases cover every op1/op2/CRm encoding, all ignored
  operand bits, every DAIF truth-table result and denied-access path, all
  streaming-state aliases, every privilege namespace, every feature version,
  and the complete reserved partition. Both page configurations pass:
  interpreter and gadgets execute all 1,903 cases, JIT executes 1,723, and
  180 valid cases are exact host-feature skips. Translation selects 44 live
  field/value/mask-specific gadgets, removes every runtime field decode and
  inline operand load, and retains only the required DAIF access branch.
- [x] LUTI2 and LUTI4 now share five split suites covering all 18 allocated
  element/segment/table forms, all six reserved control cells, every register
  field, destructive and wraparound aliases, every lookup index, and 256
  deterministic source patterns per form. All 7,056 cases pass interpreter
  and gadgets in both page configurations; 576 invalid cases execute through
  JIT and 6,480 valid cases are exact host-feature skips. Capable hosts now
  receive direct native JIT emission. Translation selects 18 branch-free
  native entries or one branch-free portable entry and supplies direct
  CpuState offsets, eliminating runtime register-field decoding.
- [x] All 16 read-check-write rows now share split scalar, pair, allocation,
  fault, and gadget suites covering strict allocation, every operation and
  ordering, register aliases, successful and failed checks, writeback,
  permissions, alignment, page splits, monitor cleanup, and fault atomicity.
  Their 18,636 cases pass interpreter and gadgets in both page
  configurations; 10,574 execute through JIT and 8,062 are exact feature
  skips. Translation selects 152 live operation-specific gadgets and removes
  the dead acquire/discard variants.
- [x] The remaining seven Base unprivileged-pair and 128-bit atomic rows now
  share split allocation, behavior, fault, and gadget suites covering every
  mode, operation, ordering, immediate, register permutation, alias,
  reserved cell, page split, permission, alignment, reservation, and exact
  fault result. Their 5,221 cases pass interpreter and gadgets in both page
  configurations; 680 invalid/allocation cases execute through JIT and 4,541
  valid cases are exact feature skips. Sixteen unreachable pair-store gadgets
  were removed, and definition/reference sets now match exactly.
- [x] The five reduced-precision dot, long-multiply, and matrix rows now use
  14 split shared-engine suites with independent arithmetic models covering
  every vector/indexed form, register and lane field, destructive alias,
  rounding boundary, control state, reserved allocation, portable path, and
  gadget shape. Their 8,378 cases pass interpreter and gadgets in both page
  configurations; 10 invalid cases execute through JIT and 8,368 valid cases
  are exact feature skips.
- [x] Both 8-bit-to-16-bit conversion rows now share five split suites driven
  by an independent exact-integer oracle. All four selector encodings, every
  source/destination register pair, every 8-bit input, both source formats,
  every legal scale, both vector halves, aliases, alternate-mode default
  results, cumulative status, unsupported formats, and same-block FPMR writes
  are covered. Their 22,594 cases pass interpreter and gadgets in both page
  configurations and are exact native-feature skips on this host. The audit
  fixed alternate-mode default-result sign handling, added direct JIT
  emission for capable hosts, and reduced each native gadget to 19 runtime
  instructions with no shape/register decode branch. The last unique
  cross-instruction status-state case now uses the shared three-engine
  harness, allowing four redundant generic FP8 suites and their legacy
  two-engine differential helper to be removed.
- [x] The four 128-bit round-transform rows now use five split suites with an
  independent byte/row oracle covering every operation, all register fields
  and aliases, deterministic input patterns, the complete reserved
  allocation, portable selection, and gadget shape. All 12,288 cases pass all
  three engines in both page configurations. The audit corrected the
  portable interpreter's operand order, selects native or portable direct-
  offset gadgets during translation, removes four obsolete runtime-decoding
  entries, and leaves eight live branch-free entries of at most nine runtime
  instructions.
- [x] The four scaled 8-bit dot-product rows now share five split suites
  driven by an independent arbitrary-precision oracle. All vector widths,
  indexed lanes, register fields and destructive aliases, every 8-bit source
  pattern, both source formats, every scale, saturation, special value,
  alternate-mode result, unsupported format, exact cancellation, signed
  zero, and same-block FPMR write are covered. Their 38,765 cases pass
  interpreter and gadgets in both page configurations and are exact native-
  feature skips on this host. Capable hosts receive direct native JIT
  emission; all native gadgets consume direct state offsets with no runtime
  shape or register decode.
- [x] The six selected-byte multiply-add and matrix rows now share six split
  suites driven by the same independent arbitrary-precision specification
  oracle. Every vector/indexed selector and index, all register fields and
  destructive aliases, every source byte pattern in both operand positions,
  both source formats, every scale and saturation control, special values,
  alternate-mode results, unsupported formats, exact cancellation, signed
  zero, row/column/segment mapping, and same-block FPMR writes are covered.
  Their 199,906 cases pass interpreter and gadgets in both page
  configurations and are exact native-feature skips on this host. Capable
  hosts receive direct native JIT emission. Translation selects direct-offset
  gadgets whose 23-instruction hot paths contain only the two unavoidable
  floating-environment branches and no opcode, shape, or register decode.
- [x] Both half-to-single dot-product rows now share six split suites driven
  by an independent exact-integer arithmetic oracle. Every vector/indexed
  form, lane and register field, destructive alias, rounding boundary,
  nontrapping control combination, special result class, cumulative status,
  and reserved scalar encoding is covered. Their 6,869 cases pass
  interpreter and gadgets in both page configurations; 74 forced-invalid
  cases execute through JIT and 6,795 valid cases are exact native-feature
  skips. Capable hosts receive direct JIT emission. Translation supplies
  direct state offsets to 11 live gadget entries and removes ten obsolete
  checked/branching entries.
- [x] All 20 load/store structure rows now share 12 split suites and an
  independent byte-layout and memory-fault oracle. Every structure count,
  contiguous/interleaved layout, element width, vector shape, lane,
  replication form, register wraparound and alias, offset/register
  post-index, SP alignment, address tag, permission, page boundary, mixed
  within-element fault priority, writeback, and reserved allocation is
  covered. Their 73,843 cases pass all three engines in both page
  configurations with zero skips. Translation selects exactly 348 multiple,
  192 lane, and 192 replicate gadgets; valid hot paths resolve all shape and
  addressing choices before execution.
- [x] The six half-precision accumulation and matrix rows now share 12 split
  suites driven by independent exact-integer arithmetic oracles. Every vector
  and indexed form, half selection, lane, register field and destructive
  alias, rounding boundary, nontrapping control combination, special result
  class, cumulative status, and reserved allocation is covered. Their 835
  cases pass interpreter and gadgets in both page configurations; 8 reserved
  cases execute through JIT and 827 valid cases are exact native-capability
  skips on this host. Capable hosts receive direct native JIT emission.
  Translation supplies direct state offsets, removes 74 obsolete checked
  gadget entries, and leaves valid hot paths without runtime shape or control
  branches.
- [x] The 23 fixed-width transform rows now share nine EngineHarness suites
  and one structural gadget suite driven by independent bit-level oracles.
  Every register field, complete alias partition, operation and selector
  control, and adjacent reserved cell is covered. Their 4,967 cases pass
  interpreter and gadgets in both page configurations; 2,165 execute through
  JIT and 2,802 valid cases are exact native-capability skips. All 543 invalid
  encodings execute through JIT and produce architectural illegal events.
  Translation resolves native versus portable choice and all state offsets;
  all 70 assigned gadget definitions are live and branch-free. The former
  packed-stream crash at `0x5e020020` is now an explicit dual-page regression
  test.
- [x] All 20 atomic floating-memory rows now share six split EngineHarness
  suites driven by independent encoding, floating-result, ordering, and
  memory-fault oracles. Every element format and width, operation,
  acquire/release form, return/discard form, register and SP base, address
  tag, alignment, permission, page boundary, source/destination alias,
  exception status, and reserved allocation is covered. Their 12,900 cases
  pass interpreter and gadgets in both page configurations; all 2,616 invalid
  encodings execute and reject through JIT, while 10,284 valid cases are exact
  native-capability skips on this host. Translation selects exactly 240
  native and 240 portable specialized gadget entries per build.
- [x] The ten ordinary SIMD-memory rows now share six behavioral
  EngineHarness suites plus a structural gadget suite. Every transfer width,
  unsigned, unscaled, pre/post-index, register-offset and pair form, register
  alias, GPR/SP base, writeback, alignment, permission, address tag,
  cross-page atomicity, and reserved allocation is covered. Their 20,156
  cases pass interpreter and gadgets in both page configurations; 18,424
  execute through JIT and 1,732 valid pair cases are exact native-capability
  skips. This includes 50 reserved immediate-mode cells recovered by the
  independent audit after the legacy tests were removed. The structural
  audit proves exactly 236 unique live definitions and
  304 translation selections with all opcode, width, addressing mode, and
  base-register choices removed from the runtime hot path. This migration
  also removed 40 former Base invalid cases whose encodings are allocated to
  these SIMD pair rows, correcting interpreter, gadget, and JIT totals by
  minus 40 without changing feature skips.
- [x] Removed the residual legacy two-engine differential framework:
  11 superseded CTest targets and 37 tracked source/support files are gone,
  the common SIMD helper now retains only live data helpers, and six
  hard-coded differential loops were removed from the static gadget smoke
  test. The only unique legacy concurrency checks were migrated into the
  shared 128-bit pair suite through tested per-engine lifecycle hooks,
  contributing two interpreter/gadget cases and two exact JIT feature skips.
  The whole-inventory baseline scanner remains only as a structural
  translation-acceptance test, not as a semantic oracle.
- [x] Historical sweep result: the final verbose shared-harness sweep ran 18,214,125 cases through
  both interpreter and gadgets and 12,493,442 through JIT, with 5,720,683
  exact specification-derived host-feature skips. JIT was unavailable for
  zero cases, and invalid encodings continued through all three engines.
  These totals include harness-contract, deferred-state, and synchronous-trap
  regressions in addition to the 983-row instruction inventory.
- [x] Historical closure claim, superseded by the seventh-audit qualification:
  the earlier audit reported 983/983 inventory rows, 516/516 Base and 467/467
  FP&SIMD, and reported every allocated variant and reserved partition as
  passing both page configurations. The current evidence schema cannot prove
  that exhaustiveness.
- [x] Migrate every remaining existing instruction test to the shared explicit
  oracle and remove the legacy two-engine differential helpers.
- [x] Historical ledger action: all 983 `JIT` boxes were marked complete after
  selected encodings executed natively or recorded a host-feature skip. Those
  marks are provisional until independent variant identities exist.
- [x] Run the full standard suite after the complete migration. The final
  16 KiB configuration passed all 521 registered tests and the 4 KiB
  configuration passed all 522; the loopback VNC smoke was run separately
  from the socket-restricted sandbox and passed in both.

## Legacy interpreter/gadget milestones

The following checkmarks record the earlier interpreter/gadget completion
campaign. The per-entry boxes were once treated as authoritative, but the
seventh-audit qualification at the top supersedes that status for every
engine.

- [x] PDF inventories extracted, form-feed boundaries validated, and counted: 983 entries.
- [x] Existing implementation and test coverage classified for every entry.
- [x] All Base entries complete.
- [x] All SIMD and Floating-point entries complete.
- [x] The decoded gadget fallback is absent; production platform defaults
  never select the diagnostic interpreter automatically.
- [x] Host-extension fast paths benchmarked and selected where semantically exact.
- [x] Every previously checked gadget family re-audited for translation-time specialization and minimal valid-hot-path branching.
- [x] Full unit and integration test suites pass in debug-interpreter and release-gadget configurations.
- [x] 8 GB, first-partition, in-memory-overlay boot verified and UnixBench baseline recorded.

## Historical final implementation and gadget audit

This section preserves dated implementation, test, integration, and performance
results. It is not current instruction-completeness or product-acceptance
evidence; use the seventh-audit qualification and refactor handoff instead.

- Historical inventory claim: **983/983** PDF entries (**516/516 Base** and
  **467/467 FP&SIMD**) had Spec, Interpreter, Gadget, JIT, and Tests checked.
  Those boxes currently represent selected cases rather than exhaustive
  independently identified variants.
- Runtime selection: `--execution-engine=jit`, `gadgets`, or `interpreter`
  chooses the userspace engine. macOS defaults to JIT and iOS defaults to
  gadgets. JIT has no gadget fallback; an unsupported translated block produces
  a guest-scoped illegal-instruction event with zero retirement, and the
  emulator remains reusable.
- Debug interpreter path: the pure C++ engine remains explicitly selectable
  for diagnostics and retains validation-first behavior before side effects.
  It is not selected by either platform's production default.
- Translation audit: production lowering carries direct `CpuState` byte
  offsets, immediate values, widths, operation variants, ordering, and
  addressing-mode choices. ADR-to-XZR is folded to a NOP at translation.
  Harness code pages are immutable and execute-only from the guest's
  perspective; each engine receives a restored state, data image, tags, and
  execution context.
- Hot-path audit: exact gadget entry points eliminate runtime opcode/mode
  decoding. TLB hits use one taken conditional branch; pair page-span checks
  use one combined page test; pair and structured SIMD hot epilogues are
  duplicated to avoid unconditional finish branches. Remaining instruction-
  state tests are architectural FPCR/RCW semantics, while remaining memory
  branches are TLB, page-span, alignment/fault, or atomic-retry checks.
- Dated dead-gadget audit: all **9,826** production gadget/helper definitions
  (**9,587** exported assembly/table symbols plus **239** C++ helpers) were
  compared with production object references, immutable native-table
  relocations, and assembly-internal targets. The 16 KiB and 4 KiB symbol
  sets were identical and that pass reported an empty unused-definition set.
  The seventh audit has since found one unreachable fatal helper/wrapper, so
  current closure requires its removal or an explicit documented ABI purpose.
- Host acceleration: gadgets use exact AArch64 host operations and extensions
  where their semantics match, including scalar/vector arithmetic, atomic
  operations, floating-point/vector execution, and specialized block/vector
  tables; guarded software behavior remains for semantics the host cannot
  reproduce exactly.
- Test organization: instruction coverage is split across **483 focused A64
  test source files** (**488 emulator test sources** including infrastructure),
  with separate allocation/encoding, behavior, control-state, fault/ordering,
  and gadget checks as applicable.
- Dated registered CTest matrix (guest integrations excluded):
  **521/521** tests pass with 16 KiB pages and **522/522** with 4 KiB pages.
  The extra 4 KiB test covers the shadow MMU. The LTP runner parser remains a
  unit test; guest SMP and the verified guest LTP campaign remain standalone
  targets, `guest-smp-parallel` and `guest-ltp-verified`.
- Final 2026-07-31 requalification after the JIT cache-layout work:
  production 16 KiB passes **562/562** non-VNC tests, the debug interpreter
  passes **560/560**, and 4 KiB compatibility passes **563/563**. Each build's
  VNC smoke passes separately. The full non-pressure LTP integration campaign
  accepts **2,142/2,142** cases, and both page-size register-pressure
  diagnostic tests pass.
- Final performance and graphics evidence: the accepted direct-check JIT
  scores **251/843** in Geekbench 6.7.1
  ((result)). Desktop WebGL Aquarium
  validates all 113 resources, 500 fish, a 1024x1024 WebGL 2.0 canvas, and
  measures **13.933 average FPS** across 30 post-warm-up samples. The canonical
  `rootfs.img` hash remains
  `501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
- Artifacts observed for this run: `.build/kernel` was regenerated with
  `CONFIG_PAGE_SIZE_16KB=y`, 4 KiB pages disabled, and
  `CONFIG_BLK_DEV_INITRD=y`; the matching `uml-darwin` production executable
  was relinked successfully.
- Standalone integration regression: the strict-JIT verified-LTP campaign
  passed its 2,142 selected guest cases, and the 10-vCPU SMP
  topology/parallelism check consumed 39.66 host-CPU seconds over 4.01 seconds
  wall time (**9.90 effective host cores**).

### Historical native JIT engine audit

- The optional JIT engine never enters a gadget. The former fallback flag,
  public API, CLI option, decoded-gadget call, gadget cache member, and gadget
  builder path have been removed; an unsupported JIT block is a guest-scoped
  illegal-instruction event with zero retirement and no fallback.
- Family compilers were moved to independently compiled `.cpp` translation
  units, but the executor remains textually composed from large implementation
  headers. The seventh audit therefore treats the split as partial: lifecycle,
  cache, linking, publication, diagnostics, semantic helpers, and verification
  still require normal responsibility-owned translation units.
- The campaign assumed every optional family propagated through guest HWCAP had
  a feature-gated native emitter while HWCAP2 remained zero. Its shared harness
  took each case's native requirement from test-owned allocation metadata,
  skipped a valid case missing that declared capability, and executed invalid
  or reserved encodings through JIT. The seventh audit requires an independent
  allocation-to-feature oracle before treating those skips as exact.
- The dated ordinary sandboxed matrices passed **559/559** registered tests
  with 16 KiB pages and **558/558** with 4 KiB pages. The host VNC smoke
  remains registered and passes separately with explicit localhost-bind
  permission in both builds, producing complete standard totals of
  **560/560** and **559/559**, respectively. Guest LTP and SMP remain
  standalone integration suites rather than ordinary CTest tests.
- The latest translation-time register phase passes **39/39** focused JIT
  tests in each 16 KiB/4 KiB build. Coverage includes exact retirement,
  one-to-one FP/SIMD, vector remapping, TLB-miss preservation, linked-vector
  runtime, lazy flags, pair remapping, zero/one/two constrained pins,
  full-cache fallback, TLB-helper preservation, terminal materialization, and
  a direct cold memory fault. Two test-only exact-shape expectations were
  corrected: lazy-flags still looked for the outlined second-way/miss path in
  the hot prefix, while pair-remap matched the internal paired TLB load instead
  of the semantic address pair. The structure-remapping codegen/runtime
  saturation fixture was expanded from the old 16-vector bank to all 18
  resident slots and passes in both page-size builds.
- Multi-register vector lookup lists are now assigned conflict-free
  consecutive q16-q31 runs during translation, after the stricter q6/q7
  constrained-source selection. A representative interleaved four-register
  block falls from **41 hot / 44 total words** to **37 hot / 40 total words**,
  removing exactly four staging moves without changing metadata or adding a
  runtime check. Dedicated split codegen and shared three-engine behavior
  files cover wrapped aliases, disjoint lists, terminal boundaries, the
  host-optional two-register form, and bounded conflict fallback. On this host
  the behavior cases ran interpreter 3, gadgets 3, and JIT 2, with one JIT
  case skipped only because its required native capability is absent.
- GPR residency is now selected from actual translation-time read/write
  traffic rather than first encounter alone when pressure exceeds the
  eight-slot bank. Bindings remain stable for the entire block, so cold exit
  snapshots stay exact. The compiler keeps its original first pass unless a
  second pass has identical metadata, a strictly shorter hot prefix, and
  nonlarger total code. The representative pressure case removes six loads
  and three stores (**nine hot words**) with no generated runtime check or
  branch. Split code-shape and shared three-engine runtime files cover the
  profitable case, low-pressure and zero-benefit identity, terminal bounds,
  and linked-block metadata.
- A matched network-enabled 10-vCPU, 8 GiB Geekbench 6.7.1 comparison is now
  complete with the same production executable and read-only OverlayFS lower
  image. JIT scored **214 single-core / 491 multi-core** versus gadgets at
  **77 / 495**. JIT improved single-core by **177.9%** and reduced total
  profiled wall time from **55m 13.2s** to **35m 19.2s**, while the multi-core
  aggregate remained effectively tied at **-0.8%**.
- The matched profiles show that JIT's multi-core limitation is not guest
  topology: the compiler workload sustained **9.45 effective host cores**.
  Several regressed workloads instead spend about **60--85%** of emulator
  samples in the global direct-JIT store/exclusive-operation publication
  protocol. The worst remaining image-analysis outlier has a different
  block-entry/run profile and is tracked separately in
  `HOT_PATH_EXPERIMENTS.md`.
- The pressure-guided eight-slot GPR selection and consecutive vector-list
  prebinding are complete. Expanding the resident bank into x22/x23 remains a
  separate candidate only if later runtime evidence clears its added
  outer-boundary save/restore cost; those code-shape gains remained pending
  matched workload measurement.
- The rebuilt real guest passes the standalone JIT contract:
  `AT_HWCAP=0x2200fb`, `AT_HWCAP2=0`, V8 dynamic-code generation,
  WebAssembly execution, repeated Node teardown, and a separate strict Node
  command returning `42`.
- Full non-pressure LTP was run with a separately configured 4 KiB kernel and
  build tree so the default 16 KiB kernel was never overwritten. The campaign
  exposed and fixed direct-JIT scalar/vector floating-horizontal lowering
  (including exact high-register `FABD S31,S30,S31` and
  `FABD D30,D15,D30`) and architectural `UDF #imm16` SIGILL delivery. Exact
  encodings are retained in the split integer and SIMD JIT test files.
- The final 2,142-case campaign completed in 1,480.7 seconds without a JIT
  fatal. Its only initial harness rejection was `memcg_stat`, whose documented
  legacy-cgroup result matrix legitimately varies among a clean pass,
  pass-with-warning, and pass-plus-userspace-break outcomes. The acceptance
  contract now covers all three while still requiring at least one TPASS;
  the isolated control was accepted with four TPASS and the known BusyBox
  cleanup limitation. `mmap18` passed all four checks in the full run and in
  16 simultaneous focused guests.
- The default 16 KiB production kernel was rebuilt after compatibility
  testing, restoring the matching static-kernel metadata in `rootfs.ext2`.

### 2026-07-24 scalar FABD runtime regression

- Reported failure: a long-running Python process reached scalar
  `FABD D30,D15,D30` (`0x7efed5fe`) and production translation emitted a fatal
  event at that word.
- Root cause: the shared scalar S/D FABD decoder and its test encoder used the
  unallocated bit-23-clear pattern (`0x7e20d400`/`0x7e60d400`). Architecturally
  allocated S/D encodings set bit 23
  (`0x7ea0d400`/`0x7ee0d400`), so ordinary low-register tests could pass while
  the real high-register instruction remained unrecognized.
- Fix: the strict shared decoder now recognizes the complete allocation,
  rejects the unallocated bit-23-clear siblings before side effects, and lets
  production translation select the existing width-specialized native `fabd`
  gadget. The obsolete broad interpreter decode-table entry was removed.
- Unit evidence: exact-encoding interpreter behavior, exact production-gadget
  differential execution, high-register variants, and invalid-sibling
  rejection pass in both `.build/cmake-macos` and
  `.build/cmake-interpreter`. The standard suite passes **302/302**.
- Live evidence: a read-only `rootfs.img` plus tmpfs OverlayFS/systemd guest
  completed a 180-second Python FP-format/parse stress (2,280,000 iterations),
  8,000 dynamic system-register calls, and 250,000 direct executions of
  `0x7efed5fe`, all without a fatal event or result mismatch. OpenJDK 21.0.11
  also started successfully, and the guest shut down cleanly.

### Independent encoding-allocation audit

- Every raw instruction word in the A64 test-support headers, every expected
  mnemonic table, and every native gadget-engine `.inst` word was
  independently disassembled with LLVM's AArch64 decoder rather than relying
  on the emulator decoder and its matching test encoder.
- The audit found and corrected three shared allocation mistakes:
  vector FABS/FNEG required bit 23 set; non-widening FMMLA required bit 29
  clear; and the vector FSQRT decoder needed bit 23 in its mask/value so that
  the valid bit-23-clear FRINT64X family was not claimed as FSQRT.
- Exact canonical words for all affected half/single/double, Q, and
  high-register forms are asserted in focused allocation files. Reserved
  siblings are rejected before source reads or architectural writes by the
  debug interpreter, while production resolves the allocation once during
  translation and keeps the valid gadget hot path branch-free.
- The non-widening FMMLA bit layout was additionally checked against the
  rendered 2026-06 PDF encoding diagram. After correction, every executable
  raw test/gadget word decodes to its intended architectural family; remaining
  LLVM-invalid constants are deliberately partial family values, reserved
  negative tests, or non-instruction FP data.

## UnixBench production baseline

The final accepted-source run completed successfully on 2026-07-24 with
UnixBench 6.0.1 and the Release gadget build (kernel
`6.18.38-g22c669fb9862`). The emulator
exposed 20 vCPUs and was passed `mem=8G`; the guest reported **8,216,340 KiB**
`MemTotal`.

The kernel's initrd feature remains enabled in both the maintained defconfig
and built kernel (`CONFIG_BLK_DEV_INITRD=y`). The benchmark boot used
`scripts/boot-overlay-systemd.sh`, mounted GPT partition 1 (`/dev/ubda1`) from
`rootfs.img` as ext4 read-only, mounted tmpfs for the OverlayFS upper/work
directories, pivoted to the overlay root, and ran systemd as PID 1 before
running exactly
`/root/byte-unixbench/UnixBench/Run`. Guest evidence was:

```text
EXT4-fs (ubda1): mounted filesystem ... ro
MemTotal:        8216340 kB
overlay / overlay rw,...,lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work
/dev/ubda1 /.uml-lower ext4 ro,relatime
```

| Test | 1-copy result | 1-copy index | 20-copy result | 20-copy index |
|---|---:|---:|---:|---:|
| Dhrystone 2 | 1,817,444.7 lps | 155.7 | 29,589,257.7 lps | 2,535.5 |
| Double-Precision Whetstone | 417.7 MWIPS | 76.0 | 7,072.6 MWIPS | 1,285.9 |
| Execl Throughput | 458.7 lps | 106.7 | 1,805.8 lps | 420.0 |
| File Copy, 1024-byte buffer | 540,525.3 KBps | 1,365.0 | 1,032,100.8 KBps | 2,606.3 |
| File Copy, 256-byte buffer | 138,603.6 KBps | 837.5 | 264,029.6 KBps | 1,595.3 |
| File Copy, 4096-byte buffer | 460,890.9 KBps | 794.6 | 1,715,671.0 KBps | 2,958.1 |
| Pipe Throughput | 672,031.0 lps | 540.2 | 1,187,436.8 lps | 954.5 |
| Pipe Context Switching | 55,321.3 lps | 138.3 | 56,308.9 lps | 140.8 |
| Process Creation | 1,385.8 lps | 110.0 | 1,851.6 lps | 147.0 |
| Shell Scripts, 1 concurrent | 976.7 lpm | 230.4 | 5,700.3 lpm | 1,344.4 |
| Shell Scripts, 8 concurrent | 604.6 lpm | 1,007.7 | 753.7 lpm | 1,256.2 |
| System Call Overhead | 443,769.5 lps | 295.8 | 1,304,863.5 lps | 869.9 |

- **Single-copy System Benchmarks Index Score: 305.0**
- **20-copy System Benchmarks Index Score: 942.1**
- **Composite score: 536.042**
- UnixBench exit status: **0**. The complete run lasted about 57 minutes
  without a fatal guest event, including the sustained FP and 20-vCPU phases.
- In the network-enabled validation boot, guest DNS resolved
  `deb.debian.org`, an HTTP request returned `200 OK`, and OpenJDK 21.0.11
  started successfully. The earlier no-network observation was reproduced
  only when the host emulator process itself was launched inside a
  DNS/network-restricted development sandbox.
- `rootfs.img` before and after: size **8,589,934,592 bytes**, mtime
  **1784725767**, SHA-256
  `9d1398970ea078dcef37bed42ed5eba6ba132f53cca6849420a3f8f188f510ba`.

## Base instructions (516/516 historical checkmarks; variant proof open)

- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ABS: Absolute value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADC: Add with carry.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADCS: Add with carry, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADD (extended register): Add extended and scaled register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADD (immediate): Add immediate value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADD (shifted register): Add optionally-shifted register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDG: Add with tag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDPT: Add checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDS (extended register): Add extended and scaled register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDS (immediate): Add immediate value, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDS (shifted register): Add optionally-shifted register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADR: Form PC-relative address.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADRP: Form PC-relative address to 4KB page.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AND (immediate): Bitwise AND (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AND (shifted register): Bitwise AND (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ANDS (immediate): Bitwise AND (immediate), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ANDS (shifted register): Bitwise AND (shifted register), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - APAS: Associate physical address space: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ASR (immediate): Arithmetic shift right (immediate): an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ASR (register): Arithmetic shift right (register): an alias of ASRV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ASRV: Arithmetic shift right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AT: Address translate: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTDA, AUTDZA: Authenticate data address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTDB, AUTDZB: Authenticate data address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIA, AUTIA1716, AUTIASP, AUTIAZ, AUTIZA: Authenticate instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIA171615: Authenticate instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIASPPC: Authenticate return address using key A, using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIASPPCR: Authenticate return address using key A, using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIB, AUTIB1716, AUTIBSP, AUTIBZ, AUTIZB: Authenticate instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIB171615: Authenticate instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIBSPPC: Authenticate return address using key B, using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AUTIBSPPCR: Authenticate return address using key B, using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AXFLAG: Convert floating-point condition flags from Arm to external format.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - B: Branch.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - B.cond: Branch conditionally.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BC.cond: Branch consistent conditionally.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFC: Bitfield clear: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFI: Bitfield insert: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFM: Bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFXIL: Bitfield extract and insert at low end: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BIC (shifted register): Bitwise bit clear (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BICS (shifted register): Bitwise bit clear (shifted register), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BL: Branch with link.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BLR: Branch with link to register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BLRAA, BLRAAZ, BLRAB, BLRABZ: Branch with link to register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BR: Branch to register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BRAA, BRAAZ, BRAB, BRABZ: Branch to register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BRB: Branch record buffer: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BRK: Breakpoint instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BTI: Branch target identification.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CAS, CASA, CASAL, CASL: Compare and swap word or doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CASB, CASAB, CASALB, CASLB: Compare and swap byte in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CASH, CASAH, CASALH, CASLH: Compare and swap halfword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CASP, CASPA, CASPAL, CASPL: Compare and swap pair of words or doublewords in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CASPT, CASPAT, CASPALT, CASPLT: Compare and swap pair unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CAST, CASAT, CASALT, CASLT: Compare and swap unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CB<cc> (immediate): Compare register with immediate and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CB<cc> (register): Compare registers and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBB<cc>: Compare bytes and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBBLE: Compare signed less than or equal to bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBBLO: Compare unsigned lower than bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBBLS: Compare unsigned lower than or same as bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBBLT: Compare signed less than bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBGE (immediate): Compare signed greater than or equal to immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBH<cc>: Compare halfwords and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBHLE: Compare signed less than or equal to halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBHLO: Compare unsigned lower than halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBHLS: Compare unsigned lower than or same as halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBHLT: Compare signed less than halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBHS (immediate): Compare unsigned higher than or same as immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLE (immediate): Compare signed less than or equal to immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLE (register): Compare signed less than or equal to register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLO (register): Compare unsigned lower than register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLS (immediate): Compare unsigned lower than or same as immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLS (register): Compare unsigned lower than or same as register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBLT (register): Compare signed less than register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBNZ: Compare and branch on nonzero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CBZ: Compare and branch on zero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CCMN (immediate): Conditional compare negative (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CCMN (register): Conditional compare negative (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CCMP (immediate): Conditional compare (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CCMP (register): Conditional compare (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CFINV: Invert carry flag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CFP: Control flow prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CHKFEAT: Check feature status.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CINC: Conditional increment: an alias of CSINC.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CINV: Conditional invert: an alias of CSINV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLRBHB: Clear branch history.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLREX: Clear exclusive.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLS: Count leading sign bits.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLZ: Count leading zeros.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMN (extended register): Compare negative (extended register): an alias of ADDS (extended register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMN (immediate): Compare negative (immediate): an alias of ADDS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMN (shifted register): Compare negative (shifted register): an alias of ADDS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMP (extended register): Compare (extended register): an alias of SUBS (extended register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMP (immediate): Compare (immediate): an alias of SUBS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMP (shifted register): Compare (shifted register): an alias of SUBS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMPP: Compare with tag: an alias of SUBPS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CNEG: Conditional negate: an alias of CSNEG.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CNT: Count bits.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - COSP: Clear other speculative prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPP: Cache prefetch prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFP, CPYFM, CPYFE: Memory copy forward-only.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPN, CPYFMN, CPYFEN: Memory copy forward-only, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPRN, CPYFMRN, CPYFERN: Memory copy forward-only, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPRT, CPYFMRT, CPYFERT: Memory copy forward-only, reads unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPRTN, CPYFMRTN, CPYFERTN: Memory copy forward-only, reads unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPRTRN, CPYFMRTRN, CPYFERTRN: Memory copy forward-only, reads unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPRTWN, CPYFMRTWN, CPYFERTWN: Memory copy forward-only, reads unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPT, CPYFMT, CPYFET: Memory copy forward-only, reads and writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPTN, CPYFMTN, CPYFETN: Memory copy forward-only, reads and writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPTRN, CPYFMTRN, CPYFETRN: Memory copy forward-only, reads and writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPTWN, CPYFMTWN, CPYFETWN: Memory copy forward-only, reads and writes unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPWN, CPYFMWN, CPYFEWN: Memory copy forward-only, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPWT, CPYFMWT, CPYFEWT: Memory copy forward-only, writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPWTN, CPYFMWTN, CPYFEWTN: Memory copy forward-only, writes unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPWTRN, CPYFMWTRN, CPYFEWTRN: Memory copy forward-only, writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYFPWTWN, CPYFMWTWN, CPYFEWTWN: Memory copy forward-only, writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYP, CPYM, CPYE: Memory copy.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPN, CPYMN, CPYEN: Memory copy, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPRN, CPYMRN, CPYERN: Memory copy, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPRT, CPYMRT, CPYERT: Memory copy, reads unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPRTN, CPYMRTN, CPYERTN: Memory copy, reads unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPRTRN, CPYMRTRN, CPYERTRN: Memory copy, reads unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPRTWN, CPYMRTWN, CPYERTWN: Memory copy, reads unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPT, CPYMT, CPYET: Memory copy, reads and writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPTN, CPYMTN, CPYETN: Memory copy, reads and writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPTRN, CPYMTRN, CPYETRN: Memory copy, reads and writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPTWN, CPYMTWN, CPYETWN: Memory copy, reads and writes unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPWN, CPYMWN, CPYEWN: Memory copy, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPWT, CPYMWT, CPYEWT: Memory copy, writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPWTN, CPYMWTN, CPYEWTN: Memory copy, writes unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPWTRN, CPYMWTRN, CPYEWTRN: Memory copy, writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CPYPWTWN, CPYMWTWN, CPYEWTWN: Memory copy, writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CRC32B, CRC32H, CRC32W, CRC32X: CRC32 checksum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CRC32CB, CRC32CH, CRC32CW, CRC32CX: CRC32C checksum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSDB: Consumption of speculative data barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSEL: Conditional select.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSET: Conditional set: an alias of CSINC.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSETM: Conditional set mask: an alias of CSINV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSINC: Conditional select increment.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSINV: Conditional select invert.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CSNEG: Conditional select negation.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CTZ: Count trailing zeros.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DC: Data cache operation: an alias of SYS. DC ZVA exposes a 64-byte block and uses a checked all-or-nothing interpreter write plus a translation-specialized four-store gadget; OpenJDK 21 startup is covered by live validation.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DCPS1: Debug change PE state to EL1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DCPS2: Debug change PE state to EL2.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DCPS3: Debug change PE state to EL3.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DGH: Data gathering hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DMB: Data memory barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DRPS: Debug restore PE state.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DSB: Data synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DVP: Data value prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EON (shifted register): Bitwise exclusive-OR NOT (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EOR (immediate): Bitwise exclusive-OR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EOR (shifted register): Bitwise exclusive-OR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ERET: Exception return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ERETAA, ERETAB: Exception return, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ESB: Error synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EXTR: Extract register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSB: Guarded Control Stack barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSPOPCX: Guarded Control Stack pop and compare exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSPOPM: Guarded Control Stack pop: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSPOPX: Guarded Control Stack pop exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSPUSHM: Guarded Control Stack push: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSPUSHX: Guarded Control Stack push exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSSS1: Guarded Control Stack switch stack 1: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSSS2: Guarded Control Stack switch stack 2: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSSTR: Guarded Control Stack store register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GCSSTTR: Guarded Control Stack store register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GIC: Generic Interrupt Controller: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GICR: Generic Interrupt Controller with result: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GMI: Tag mask insert.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - GSB: Generic Interrupt Controller synchronization barrier: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - HINT: Hint instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - HINTE: Hint instruction (extended).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - HLT: Halt instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - HVC: Hypervisor call.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - IC: Instruction cache operation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - IRG: Insert random tag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ISB: Instruction synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD64B: Single-copy atomic 64-byte load.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDADD, LDADDA, LDADDAL, LDADDL: Atomic add on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDADDB, LDADDAB, LDADDALB, LDADDLB: Atomic add on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDADDH, LDADDAH, LDADDALH, LDADDLH: Atomic add on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAP: Load-acquire pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPP: Load-acquire RCpc pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPR: Load-acquire RCpc register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPRB: Load-acquire RCpc register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPRH: Load-acquire RCpc register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPUR: Load-acquire RCpc register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPURB: Load-acquire RCpc register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPURH: Load-acquire RCpc register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPURSB: Load-acquire RCpc register signed byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPURSH: Load-acquire RCpc register signed halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPURSW: Load-acquire RCpc register signed word (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAR: Load-acquire register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDARB: Load-acquire register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDARH: Load-acquire register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDATXR: Load-acquire unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAXP: Load-acquire exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAXR: Load-acquire exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAXRB: Load-acquire exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAXRH: Load-acquire exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDCLR, LDCLRA, LDCLRAL, LDCLRL: Atomic bit clear on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDCLRB, LDCLRAB, LDCLRALB, LDCLRLB: Atomic bit clear on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDCLRH, LDCLRAH, LDCLRALH, LDCLRLH: Atomic bit clear on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDCLRP, LDCLRPA, LDCLRPAL, LDCLRPL: Atomic bit clear on quadword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDEOR, LDEORA, LDEORAL, LDEORL: Atomic exclusive-OR on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDEORB, LDEORAB, LDEORALB, LDEORLB: Atomic exclusive-OR on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDEORH, LDEORAH, LDEORALH, LDEORLH: Atomic exclusive-OR on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDG: Load Allocation Tag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDGM: Load tag multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDIAPP: Load-Acquire RCpc ordered pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDLAR: Load LOAcquire register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDLARB: Load LOAcquire register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDLARH: Load LOAcquire register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDNP: Load pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDP: Load pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDPSW: Load pair of registers signed word.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (immediate): Load register (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (literal): Load register (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (register): Load register (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRAA, LDRAB: Load register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRB (immediate): Load register byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRB (register): Load register byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRH (immediate): Load register halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRH (register): Load register halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSB (immediate): Load register signed byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSB (register): Load register signed byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSH (immediate): Load register signed halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSH (register): Load register signed halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSW (immediate): Load register signed word (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSW (literal): Load register signed word (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDRSW (register): Load register signed word (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSET, LDSETA, LDSETAL, LDSETL: Atomic bit set on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSETB, LDSETAB, LDSETALB, LDSETLB: Atomic bit set on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSETH, LDSETAH, LDSETALH, LDSETLH: Atomic bit set on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSETP, LDSETPA, LDSETPAL, LDSETPL: Atomic bit set on quadword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMAX, LDSMAXA, LDSMAXAL, LDSMAXL: Atomic signed maximum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMAXB, LDSMAXAB, LDSMAXALB, LDSMAXLB: Atomic signed maximum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMAXH, LDSMAXAH, LDSMAXALH, LDSMAXLH: Atomic signed maximum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMIN, LDSMINA, LDSMINAL, LDSMINL: Atomic signed minimum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMINB, LDSMINAB, LDSMINALB, LDSMINLB: Atomic signed minimum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDSMINH, LDSMINAH, LDSMINALH, LDSMINLH: Atomic signed minimum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTADD, LDTADDA, LDTADDAL, LDTADDL: Atomic add unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTCLR, LDTCLRA, LDTCLRAL, LDTCLRL: Atomic bit clear unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTNP: Load unprivileged pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTP: Load unprivileged pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTR: Load register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTRB: Load register byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTRH: Load register halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTRSB: Load register signed byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTRSH: Load register signed halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTRSW: Load register signed word (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTSET, LDTSETA, LDTSETAL, LDTSETL: Atomic bit set unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTXR: Load unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMAX, LDUMAXA, LDUMAXAL, LDUMAXL: Atomic unsigned maximum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMAXB, LDUMAXAB, LDUMAXALB, LDUMAXLB: Atomic unsigned maximum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMAXH, LDUMAXAH, LDUMAXALH, LDUMAXLH: Atomic unsigned maximum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMIN, LDUMINA, LDUMINAL, LDUMINL: Atomic unsigned minimum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMINB, LDUMINAB, LDUMINALB, LDUMINLB: Atomic unsigned minimum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUMINH, LDUMINAH, LDUMINALH, LDUMINLH: Atomic unsigned minimum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUR: Load register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDURB: Load register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDURH: Load register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDURSB: Load register signed byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDURSH: Load register signed halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDURSW: Load register signed word (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDXP: Load exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDXR: Load exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDXRB: Load exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDXRH: Load exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSL (immediate): Logical shift left (immediate): an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSL (register): Logical shift left (register): an alias of LSLV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSLV: Logical shift left variable.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSR (immediate): Logical shift right (immediate): an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSR (register): Logical shift right (register): an alias of LSRV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LSRV: Logical shift right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MADD: Multiply-add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MADDPT: Multiply-add checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MNEG: Multiply-negate: an alias of MSUB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (bitmask immediate): Move bitmask immediate value: an alias of ORR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (inverted wide immediate): Move inverted wide immediate value: an alias of MOVN.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (register): Move register value: an alias of ORR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (to/from SP): Move register value to or from SP: an alias of ADD (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (wide immediate): Move wide immediate value: an alias of MOVZ.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOVK: Move wide with keep.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOVN: Move wide with NOT.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOVZ: Move wide with zero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MRRS: Move System register to two adjacent general-purpose registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MRS: Move System register to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MSR (immediate): Move immediate value to special register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MSR (register): Move general-purpose register to System register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MSRR: Move two adjacent general-purpose registers to System register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MSUB: Multiply-subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MSUBPT: Multiply-subtract checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MUL: Multiply: an alias of MADD.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MVN: Bitwise NOT: an alias of ORN (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NEG (shifted register): Negate (shifted register): an alias of SUB (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NEGS: Negate, setting flags: an alias of SUBS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NGC: Negate with carry: an alias of SBC.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NGCS: Negate with carry, setting flags: an alias of SBCS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NOP: No operation.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORN (shifted register): Bitwise OR NOT (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORR (immediate): Bitwise OR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORR (shifted register): Bitwise OR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACDA, PACDZA: Pointer Authentication Code for data address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACDB, PACDZB: Pointer Authentication Code for data address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACGA: Pointer Authentication Code, using generic key.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIA, PACIA1716, PACIASP, PACIAZ, PACIZA: Pointer Authentication Code for instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIA171615: Pointer Authentication Code for instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIASPPC: Pointer Authentication Code for return address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIB, PACIB1716, PACIBSP, PACIBZ, PACIZB: Pointer Authentication Code for instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIB171615: Pointer Authentication Code for instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACIBSPPC: Pointer Authentication Code for return address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACM: Pointer authentication modifier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACNBIASPPC: Pointer Authentication Code for return address, using key A, not a branch target.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PACNBIBSPPC: Pointer Authentication Code for return address, using key B, not a branch target.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PRFM (immediate): Prefetch memory (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PRFM (literal): Prefetch memory (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PRFM (register): Prefetch memory (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PRFUM: Prefetch memory (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PSB: Profiling synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PSSBB: Physical speculative store bypass barrier: an alias of DSB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RBIT: Reverse bits.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWCAS, RCWCASA, RCWCASAL, RCWCASL: Read check write compare and swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWCASP, RCWCASPA, RCWCASPAL, RCWCASPL: Read check write compare and swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWCLR, RCWCLRA, RCWCLRAL, RCWCLRL: Read check write atomic bit clear on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWCLRP, RCWCLRPA, RCWCLRPAL, RCWCLRPL: Read check write atomic bit clear on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSCAS, RCWSCASA, RCWSCASAL, RCWSCASL: Read check write software compare and swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSCASP, RCWSCASPA, RCWSCASPAL, RCWSCASPL: Read check write software compare and swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSCLR, RCWSCLRA, RCWSCLRAL, RCWSCLRL: Read check write software atomic bit clear on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSCLRP, RCWSCLRPA, RCWSCLRPAL, RCWSCLRPL: Read check write software atomic bit clear on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSET, RCWSETA, RCWSETAL, RCWSETL: Read check write atomic bit set on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSETP, RCWSETPA, RCWSETPAL, RCWSETPL: Read check write atomic bit set on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSSET, RCWSSETA, RCWSSETAL, RCWSSETL: Read check write software atomic bit set on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSSETP, RCWSSETPA, RCWSSETPAL, RCWSSETPL: Read check write software atomic bit set on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSSWP, RCWSSWPA, RCWSSWPAL, RCWSSWPL: Read check write software swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSSWPP, RCWSSWPPA, RCWSSWPPAL, RCWSSWPPL: Read check write software swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSWP, RCWSWPA, RCWSWPAL, RCWSWPL: Read check write swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RCWSWPP, RCWSWPPA, RCWSWPPAL, RCWSWPPL: Read check write swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RET: Return from subroutine.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RETAA, RETAB: Return from subroutine, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RETAASPPC, RETABSPPC: Return from subroutine, with enhanced pointer authentication using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RETAASPPCR, RETABSPPCR: Return from subroutine, with enhanced pointer authentication using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV: Reverse bytes.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV16: Reverse bytes in 16-bit halfwords.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV32: Reverse bytes in 32-bit words.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV64: Reverse bytes: an alias of REV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RMIF: Rotate, mask insert flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ROR (immediate): Rotate right (immediate): an alias of EXTR.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ROR (register): Rotate right (register): an alias of RORV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RORV: Rotate right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RPRFM: Range prefetch memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SB: Speculation barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SBC: Subtract with carry.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SBCS: Subtract with carry, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SBFIZ: Signed bitfield insert in zeros: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SBFM: Signed bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SBFX: Signed bitfield extract: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SDIV (quotient): Signed divide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETF8, SETF16: Evaluation of 8-bit or 16-bit flag values.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETGP, SETGM, SETGE: Memory set with tag setting.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETGPN, SETGMN, SETGEN: Memory set with tag setting, non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETGPT, SETGMT, SETGET: Memory set with tag setting, unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETGPTN, SETGMTN, SETGETN: Memory set with tag setting, unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETP, SETM, SETE: Memory set.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETPN, SETMN, SETEN: Memory set, non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETPT, SETMT, SETET: Memory set, unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SETPTN, SETMTN, SETETN: Memory set, unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SEV: Send event.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SEVL: Send event local.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHUH: Shared update hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMADDL: Signed multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMAX (immediate): Signed maximum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMAX (register): Signed maximum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMC: Secure monitor call.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMIN (immediate): Signed minimum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMIN (register): Signed minimum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMNEGL: Signed multiply-negate long: an alias of SMSUBL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMSTART: Enable access to Streaming SVE mode and SME architectural state: an alias of MSR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMSTOP: Disable access to Streaming SVE mode and SME architectural state: an alias of MSR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMSUBL: Signed multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMULH: Signed multiply high.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMULL (32-bit): Signed multiply long: an alias of SMADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSBB: Speculative store bypass barrier: an alias of DSB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST2G: Store Allocation Tags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST64B: Single-copy atomic 64-byte store without status result.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST64BV: Single-copy atomic 64-byte store with status result.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST64BV0: Single-copy atomic 64-byte EL0 store with status result.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STADD, STADDL: Atomic add on word or doubleword, without return: an alias of LDADD, LDADDA, LDADDAL, LDADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STADDB, STADDLB: Atomic add on byte, without return: an alias of LDADDB, LDADDAB, LDADDALB, LDADDLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STADDH, STADDLH: Atomic add on halfword, without return: an alias of LDADDH, LDADDAH, LDADDALH, LDADDLH.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STCLR, STCLRL: Atomic bit clear on word or doubleword, without return: an alias of LDCLR, LDCLRA, LDCLRAL, LDCLRL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STCLRB, STCLRLB: Atomic bit clear on byte, without return: an alias of LDCLRB, LDCLRAB, LDCLRALB, LDCLRLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STCLRH, STCLRLH: Atomic bit clear on halfword, without return: an alias of LDCLRH, LDCLRAH, LDCLRALH, LDCLRLH.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STCPH: Store concurrent priority hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STEOR, STEORL: Atomic exclusive-OR on word or doubleword, without return: an alias of LDEOR, LDEORA, LDEORAL, LDEORL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STEORB, STEORLB: Atomic exclusive-OR on byte, without return: an alias of LDEORB, LDEORAB, LDEORALB, LDEORLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STEORH, STEORLH: Atomic exclusive-OR on halfword, without return: an alias of LDEORH, LDEORAH, LDEORALH, LDEORLH.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STG: Store Allocation Tag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STGM: Store Allocation Tag multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STGP: Store Allocation Tag and pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STILP: Store-release ordered pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLLR: Store LORelease register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLLRB: Store LORelease register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLLRH: Store LORelease register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLP: Store-release pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLR: Store-release register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLRB: Store-release register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLRH: Store-release register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLTXR: Store-release unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLUR: Store-release register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLURB: Store-release register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLURH: Store-release register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLXP: Store-release exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLXR: Store-release exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLXRB: Store-release exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLXRH: Store-release exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STNP: Store pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STP: Store pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STR (immediate): Store register (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STR (register): Store register (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STRB (immediate): Store register byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STRB (register): Store register byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STRH (immediate): Store register halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STRH (register): Store register halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSET, STSETL: Atomic bit set on word or doubleword, without return: an alias of LDSET, LDSETA, LDSETAL, LDSETL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSETB, STSETLB: Atomic bit set on byte, without return: an alias of LDSETB, LDSETAB, LDSETALB, LDSETLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSETH, STSETLH: Atomic bit set on halfword, without return: an alias of LDSETH, LDSETAH, LDSETALH, LDSETLH.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSHH: Store shared hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMAX, STSMAXL: Atomic signed maximum on word or doubleword, without return: an alias of LDSMAX, LDSMAXA, LDSMAXAL,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMAXB, STSMAXLB: Atomic signed maximum on byte, without return: an alias of LDSMAXB, LDSMAXAB, LDSMAXALB,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMAXH, STSMAXLH: Atomic signed maximum on halfword, without return: an alias of LDSMAXH, LDSMAXAH, LDSMAXALH,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMIN, STSMINL: Atomic signed minimum on word or doubleword, without return: an alias of LDSMIN, LDSMINA, LDSMINAL,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMINB, STSMINLB: Atomic signed minimum on byte, without return: an alias of LDSMINB, LDSMINAB, LDSMINALB, LDSMINLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STSMINH, STSMINLH: Atomic signed minimum on halfword, without return: an alias of LDSMINH, LDSMINAH, LDSMINALH, LDSMINLH.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTADD, STTADDL: Atomic add unprivileged, without return: an alias of LDTADD, LDTADDA, LDTADDAL, LDTADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTCLR, STTCLRL: Atomic bit clear unprivileged, without return: an alias of LDTCLR, LDTCLRA, LDTCLRAL, LDTCLRL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTNP: Store unprivileged pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTP: Store unprivileged pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTR: Store register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTRB: Store register byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTRH: Store register halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTSET, STTSETL: Atomic bit set unprivileged, without return: an alias of LDTSET, LDTSETA, LDTSETAL, LDTSETL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTXR: Store unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMAX, STUMAXL: Atomic unsigned maximum on word or doubleword, without return: an alias of LDUMAX, LDUMAXA, LDUMAXAL,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMAXB, STUMAXLB: Atomic unsigned maximum on byte, without return: an alias of LDUMAXB, LDUMAXAB, LDUMAXALB,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMAXH, STUMAXLH: Atomic unsigned maximum on halfword, without return: an alias of LDUMAXH, LDUMAXAH, LDUMAXALH,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMIN, STUMINL: Atomic unsigned minimum on word or doubleword, without return: an alias of LDUMIN, LDUMINA, LDUMINAL,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMINB, STUMINLB: Atomic unsigned minimum on byte, without return: an alias of LDUMINB, LDUMINAB, LDUMINALB, LDUMINLB.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUMINH, STUMINLH: Atomic unsigned minimum on halfword, without return: an alias of LDUMINH, LDUMINAH, LDUMINALH,
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUR: Store register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STURB: Store register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STURH: Store register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STXP: Store exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STXR: Store exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STXRB: Store exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STXRH: Store exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STZ2G: Store Allocation Tags, zeroing.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STZG: Store Allocation Tag, zeroing.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STZGM: Store Allocation Tag and zero multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUB (extended register): Subtract extended and scaled register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUB (immediate): Subtract immediate value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUB (shifted register): Subtract optionally-shifted register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBG: Subtract with tag.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBP: Subtract pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBPS: Subtract pointer, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBPT: Subtract checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBS (extended register): Subtract extended and scaled register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBS (immediate): Subtract immediate value, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBS (shifted register): Subtract optionally-shifted register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SVC: Supervisor call.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SWP, SWPA, SWPAL, SWPL: Swap word or doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SWPB, SWPAB, SWPALB, SWPLB: Swap byte in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SWPH, SWPAH, SWPALH, SWPLH: Swap halfword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SWPP, SWPPA, SWPPAL, SWPPL: Swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SWPT, SWPTA, SWPTAL, SWPTL: Swap unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SXTB: Signed extend byte: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SXTH: Sign extend halfword: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SXTW: Sign extend word: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SYS: System instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SYSL: System instruction with result.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SYSP: 128-bit system instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TBNZ: Test bit and branch if nonzero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TBZ: Test bit and branch if zero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TLBI: TLB invalidate operation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TLBIP: TLB invalidate pair operation: an alias of SYSP.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TRCIT: Trace instrumentation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TSB: Trace synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TST (immediate): Test bits (immediate): an alias of ANDS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TST (shifted register): Test (shifted register): an alias of ANDS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UBFIZ: Unsigned bitfield insert in zeros: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UBFM: Unsigned bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UBFX: Unsigned bitfield extract: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UDF: Permanently undefined.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UDIV (quotient): Unsigned divide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMADDL: Unsigned multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMAX (immediate): Unsigned maximum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMAX (register): Unsigned maximum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMIN (immediate): Unsigned minimum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMIN (register): Unsigned minimum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMNEGL: Unsigned multiply-negate long: an alias of UMSUBL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMSUBL: Unsigned multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMULH: Unsigned multiply high.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMULL (32-bit): Unsigned multiply long: an alias of UMADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UXTB: Unsigned extend byte: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UXTH: Unsigned extend halfword: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - WFE: Wait for event.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - WFET: Wait for event with timeout.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - WFI: Wait for interrupt.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - WFIT: Wait for interrupt with timeout.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - XAFLAG: Convert floating-point condition flags from external format to Arm format.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - XPACD, XPACI, XPACLRI: Strip Pointer Authentication Code.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - YIELD: Yield.

## SIMD and Floating-point instructions (467/467 historical checkmarks; variant proof open)

- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ABS: Absolute value (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADD (vector): Add (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDHN, ADDHN2: Add returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDP (scalar): Add pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDP (vector): Add pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ADDV: Add across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AESD: AES single round decryption.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AESE: AES single round encryption.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AESIMC: AES inverse mix columns.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AESMC: AES mix columns.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - AND (vector): Bitwise AND (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BCAX: Bit clear and exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BF1CVTL, BF1CVTL2, BF2CVTL, BF2CVTL2: 8-bit floating-point convert to BFloat16 (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFCVT: Single-precision convert to BFloat16 (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFCVTN, BFCVTN2: Single-precision convert to BFloat16 (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFDOT (by element): BFloat16 dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFDOT (vector): BFloat16 dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFMLALB, BFMLALT (by element): BFloat16 multiply-add to single-precision (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFMLALB, BFMLALT (vector): BFloat16 multiply-add to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BFMMLA (widening): BFloat16 matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BIC (vector, immediate): Bitwise bit clear (vector, immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BIC (vector, register): Bitwise bit clear (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BIF: Bitwise insert if false.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BIT: Bitwise insert if true.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - BSL: Bitwise select.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLS (vector): Count leading sign bits (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CLZ (vector): Count leading zero bits (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMEQ (register): Compare bitwise equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMEQ (zero): Compare bitwise equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMGE (register): Compare signed greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMGE (zero): Compare signed greater than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMGT (register): Compare signed greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMGT (zero): Compare signed greater than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMHI (register): Compare unsigned higher (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMHS (register): Compare unsigned higher or same (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMLE (zero): Compare signed less than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMLT (zero): Compare signed less than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CMTST: Compare bitwise test bits nonzero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - CNT: Population count per byte.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DUP (element): Duplicate vector element to vector or scalar.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - DUP (general): Duplicate general-purpose register to vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EOR (vector): Bitwise exclusive-OR (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EOR3: Three-way exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - EXT: Extract vector from pair of vectors.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - F1CVTL, F1CVTL2, F2CVTL, F2CVTL2: 8-bit floating-point convert to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FABD: Floating-point absolute difference (scalar and vector). Correct scalar S/D allocation-bit decoding is covered through both the strict interpreter and translation-specialized native gadgets, including the reported `0x7efed5fe` high-register encoding.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FABS (scalar): Floating-point absolute value (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FABS (vector): Floating-point absolute value (vector). Canonical allocation-word assertions and early rejection of the unallocated bit-23-clear siblings prevent decoder/test agreement on invalid encodings.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FACGE: Floating-point absolute compare greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FACGT: Floating-point absolute compare greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FADD (scalar): Floating-point add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FADD (vector): Floating-point add (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FADDP (scalar): Floating-point add pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FADDP (vector): Floating-point add pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FAMAX: Floating-point absolute maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FAMIN: Floating-point absolute minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCADD: Floating-point complex add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCCMP: Floating-point conditional quiet compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCCMPE: Floating-point conditional signaling compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMEQ (register): Floating-point compare equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMEQ (zero): Floating-point compare equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMGE (register): Floating-point compare greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMGE (zero): Floating-point compare greater than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMGT (register): Floating-point compare greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMGT (zero): Floating-point compare greater than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMLA: Floating-point complex multiply accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMLA (by element): Floating-point complex multiply accumulate (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMLE (zero): Floating-point compare less than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMLT (zero): Floating-point compare less than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMP: Floating-point quiet compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCMPE: Floating-point signaling compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCSEL: Floating-point conditional select (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVT: Floating-point convert precision (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAS (scalar SIMD&FP): Floating-point convert to signed integer, rounding to nearest with ties to away (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAS (scalar): Floating-point convert to signed integer, rounding to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAS (vector): Floating-point convert to signed integer, rounding to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding to nearest with ties to away (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAU (scalar): Floating-point convert to unsigned integer, rounding to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTAU (vector): Floating-point convert to unsigned integer, rounding to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTL, FCVTL2: Floating-point convert to higher precision long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward minus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMS (scalar): Floating-point convert to signed integer, rounding toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMS (vector): Floating-point convert to signed integer, rounding toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward minus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMU (scalar): Floating-point convert to unsigned integer, rounding toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTMU (vector): Floating-point convert to unsigned integer, rounding toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTN (half-precision to 8-bit floating-point): Half-precision convert to 8-bit floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTN, FCVTN2 (double to single-precision, single to half-precision): Floating-point convert to lower precision narrow (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTN, FCVTN2 (single-precision to 8-bit floating-point): Single-precision convert to 8-bit floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNS (scalar SIMD&FP): Floating-point convert to signed integer, rounding to nearest with ties to even (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNS (scalar): Floating-point convert to signed integer, rounding to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNS (vector): Floating-point convert to signed integer, rounding to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding to nearest with ties to even (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNU (scalar): Floating-point convert to unsigned integer, rounding to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTNU (vector): Floating-point convert to unsigned integer, rounding to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward plus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPS (scalar): Floating-point convert to signed integer, rounding toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPS (vector): Floating-point convert to signed integer, rounding toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward plus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPU (scalar): Floating-point convert to unsigned integer, rounding toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTPU (vector): Floating-point convert to unsigned integer, rounding toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTXN, FCVTXN2: Floating-point convert to lower precision narrow, rounding to odd (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward zero (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZS (scalar, fixed-point): Floating-point convert to signed fixed-point, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZS (scalar, integer): Floating-point convert to signed integer, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZS (vector, fixed-point): Floating-point convert to signed fixed-point, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZS (vector, integer): Floating-point convert to signed integer, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward zero (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZU (scalar, fixed-point): Floating-point convert to unsigned fixed-point, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZU (scalar, integer): Floating-point convert to unsigned integer, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZU (vector, fixed-point): Floating-point convert to unsigned fixed-point, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FCVTZU (vector, integer): Floating-point convert to unsigned integer, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDIV (scalar): Floating-point divide (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDIV (vector): Floating-point divide (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (8-bit floating-point to half-precision, by element): 8-bit floating-point dot product to half-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (8-bit floating-point to half-precision, vector): 8-bit floating-point dot product to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (8-bit floating-point to single-precision, by element): 8-bit floating-point dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (8-bit floating-point to single-precision, vector): 8-bit floating-point dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (half-precision to single-precision, by element): Half-precision dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FDOT (half-precision to single-precision, vector): Half-precision dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FJCVTZS: Floating-point JavaScript convert to signed fixed-point, rounding toward zero.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMADD: Floating-point fused multiply-add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAX (scalar): Floating-point maximum (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAX (vector): Floating-point maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXNM (scalar): Floating-point maximum number (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXNM (vector): Floating-point maximum number (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXNMP (scalar): Floating-point maximum number of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXNMP (vector): Floating-point maximum number pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXNMV: Floating-point maximum number across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXP (scalar): Floating-point maximum of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXP (vector): Floating-point maximum pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMAXV: Floating-point maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMIN (scalar): Floating-point minimum (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMIN (vector): Floating-point minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINNM (scalar): Floating-point minimum number (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINNM (vector): Floating-point minimum number (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINNMP (scalar): Floating-point minimum number of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINNMP (vector): Floating-point minimum number pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINNMV: Floating-point minimum number across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINP (scalar): Floating-point minimum of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINP (vector): Floating-point minimum pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMINV: Floating-point minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLA (by element): Floating-point fused multiply-add to accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLA (vector): Floating-point fused multiply-add to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLAL, FMLAL2 (by element): Floating-point fused multiply-add long to accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLAL, FMLAL2 (vector): Floating-point fused multiply-add long to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLALB, FMLALT (by element): 8-bit floating-point multiply-add to half-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLALB, FMLALT (vector): 8-bit floating-point multiply-add to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLALLBB, FMLALLBT, FMLALLTB, FMLALLTT (by element): 8-bit floating-point multiply-add to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLALLBB, FMLALLBT, FMLALLTB, FMLALLTT (vector): 8-bit floating-point multiply-add to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLS (by element): Floating-point fused multiply-subtract from accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLS (vector): Floating-point fused multiply-subtract from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLSL, FMLSL2 (by element): Floating-point fused multiply-subtract long from accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMLSL, FMLSL2 (vector): Floating-point fused multiply-subtract long from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMMLA (non-widening): Half-precision matrix multiply-accumulate. The canonical `0x4ec0ec00` allocation and native-host `.inst` are asserted independently; the former bit-29-set test sibling is rejected before side effects.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMMLA (widening, 8-bit floating-point to half-precision): 8-bit floating-point matrix multiply-accumulate to half-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMMLA (widening, 8-bit floating-point to single-precision): 8-bit floating-point matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMMLA (widening, half-precision to single-precision): Half-precision matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMOV (general): Floating-point move to or from general-purpose register without conversion.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMOV (register): Floating-point move register without conversion.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMOV (scalar, immediate): Floating-point move immediate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMOV (vector, immediate): Floating-point move immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMSUB: Floating-point fused multiply-subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMUL (by element): Floating-point multiply (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMUL (scalar): Floating-point multiply (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMUL (vector): Floating-point multiply (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMULX: Floating-point multiply extended.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FMULX (by element): Floating-point multiply extended (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FNEG (scalar): Floating-point negate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FNEG (vector): Floating-point negate (vector). Canonical allocation-word assertions and early rejection of the unallocated bit-23-clear siblings cover both interpreter and gadget paths.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FNMADD: Floating-point negated fused multiply-add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FNMSUB: Floating-point negated fused multiply-subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FNMUL (scalar): Floating-point multiply-negate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRECPE: Floating-point reciprocal estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRECPS: Floating-point reciprocal step.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRECPX: Floating-point reciprocal exponent (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT32X (scalar): Floating-point round to 32-bit integer, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT32X (vector): Floating-point round to 32-bit integer, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT32Z (scalar): Floating-point round to 32-bit integer toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT32Z (vector): Floating-point round to 32-bit integer toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT64X (scalar): Floating-point round to 64-bit integer, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT64X (vector): Floating-point round to 64-bit integer, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT64Z (scalar): Floating-point round to 64-bit integer toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINT64Z (vector): Floating-point round to 64-bit integer toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTA (scalar): Floating-point round to integral, to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTA (vector): Floating-point round to integral, to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTI (scalar): Floating-point round to integral, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTI (vector): Floating-point round to integral, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTM (scalar): Floating-point round to integral, toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTM (vector): Floating-point round to integral, toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTN (scalar): Floating-point round to integral, to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTN (vector): Floating-point round to integral, to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTP (scalar): Floating-point round to integral, toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTP (vector): Floating-point round to integral, toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTX (scalar): Floating-point round to integral exact, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTX (vector): Floating-point round to integral exact, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTZ (scalar): Floating-point round to integral, toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRINTZ (vector): Floating-point round to integral, toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRSQRTE: Floating-point reciprocal square root estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FRSQRTS: Floating-point reciprocal square root step.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FSCALE: Floating-point adjust exponent by vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FSQRT (scalar): Floating-point square root (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FSQRT (vector): Floating-point square root (vector). The strict allocation mask includes bit 23, with exact canonical-word checks proving the bit-23-clear FRINT64X family is not misclassified.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FSUB (scalar): Floating-point subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - FSUB (vector): Floating-point subtract (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - INS (element): Insert vector element from another vector element.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - INS (general): Insert vector element from general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD1 (multiple structures): Load multiple single-element structures to one, two, three, or four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD1 (single structure): Load one single-element structure to one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD1R: Load one single-element structure and replicate to all lanes (of one register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD2 (multiple structures): Load multiple 2-element structures to two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD2 (single structure): Load single 2-element structure to one lane of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD2R: Load single 2-element structure and replicate to all lanes of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD3 (multiple structures): Load multiple 3-element structures to three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD3 (single structure): Load single 3-element structure to one lane of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD3R: Load single 3-element structure and replicate to all lanes of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD4 (multiple structures): Load multiple 4-element structures to four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD4 (single structure): Load single 4-element structure to one lane of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LD4R: Load single 4-element structure and replicate to all lanes of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAP1 (SIMD&FP): Load-acquire RCpc one single-element structure to one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDAPUR (SIMD&FP): Load-acquire RCpc SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDBFADD, LDBFADDA, LDBFADDAL, LDBFADDL: Atomic BFloat16 add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDBFMAX, LDBFMAXA, LDBFMAXAL, LDBFMAXL: Atomic BFloat16 maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDBFMAXNM, LDBFMAXNMA, LDBFMAXNMAL, LDBFMAXNML: Atomic BFloat16 maximum number.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDBFMIN, LDBFMINA, LDBFMINAL, LDBFMINL: Atomic BFloat16 minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDBFMINNM, LDBFMINNMA, LDBFMINNMAL, LDBFMINNML: Atomic BFloat16 minimum number.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDFADD, LDFADDA, LDFADDAL, LDFADDL: Atomic floating-point add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDFMAX, LDFMAXA, LDFMAXAL, LDFMAXL: Atomic floating-point maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDFMAXNM, LDFMAXNMA, LDFMAXNMAL, LDFMAXNML: Atomic floating-point maximum number.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDFMIN, LDFMINA, LDFMINAL, LDFMINL: Atomic floating-point minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDFMINNM, LDFMINNMA, LDFMINNMAL, LDFMINNML: Atomic floating-point minimum number.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDNP (SIMD&FP): Load pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDP (SIMD&FP): Load pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (immediate, SIMD&FP): Load SIMD&FP register (immediate offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (literal, SIMD&FP): Load SIMD&FP register (PC-relative literal).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDR (register, SIMD&FP): Load SIMD&FP register (register offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTNP (SIMD&FP): Load unprivileged pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDTP (SIMD&FP): Load unprivileged pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LDUR (SIMD&FP): Load SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LUTI2: Lookup table read with 2-bit indices.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - LUTI4: Lookup table read with 4-bit indices.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MLA (by element): Multiply-add to accumulator (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MLA (vector): Multiply-add to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MLS (by element): Multiply-subtract from accumulator (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MLS (vector): Multiply-subtract from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (element): Move vector element to another vector element: an alias of INS (element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (from general): Move general-purpose register to a vector element: an alias of INS (general).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (scalar): Move vector element to scalar: an alias of DUP (element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (to general): Move vector element to general-purpose register: an alias of UMOV.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOV (vector): Move vector: an alias of ORR (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MOVI: Move immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MUL (by element): Multiply (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MUL (vector): Multiply (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MVN: Bitwise NOT (vector): an alias of NOT.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - MVNI: Move inverted immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NEG (vector): Negate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - NOT: Bitwise NOT (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORN (vector): Bitwise inclusive OR NOT (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORR (vector, immediate): Bitwise inclusive OR (vector, immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ORR (vector, register): Bitwise inclusive OR (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PMUL: Polynomial multiply.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - PMULL, PMULL2: Polynomial multiply long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RADDHN, RADDHN2: Rounding add returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RAX1: Rotate and exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RBIT (vector): Reverse bit order (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV16 (vector): Reverse elements in 16-bit halfwords (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV32 (vector): Reverse elements in 32-bit words (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - REV64: Reverse elements in 64-bit doublewords (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RSHRN, RSHRN2: Rounding shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - RSUBHN, RSUBHN2: Rounding subtract returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SABA: Signed absolute difference and accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SABAL, SABAL2: Signed absolute difference and accumulate long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SABD: Signed absolute difference.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SABDL, SABDL2: Signed absolute difference long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SADALP: Signed add and accumulate long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SADDL, SADDL2: Signed add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SADDLP: Signed add long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SADDLV: Signed add long across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SADDW, SADDW2: Signed add wide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SCVTF (scalar SIMD&FP): Signed integer convert to floating-point (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SCVTF (scalar, fixed-point): Signed fixed-point convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SCVTF (scalar, integer): Signed integer convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SCVTF (vector, fixed-point): Signed fixed-point convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SCVTF (vector, integer): Signed integer convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SDOT (by element): Dot product signed arithmetic (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SDOT (vector): Dot product signed arithmetic (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1C: SHA1 hash update (choose).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1H: SHA1 fixed rotate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1M: SHA1 hash update (majority).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1P: SHA1 hash update (parity).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1SU0: SHA1 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA1SU1: SHA1 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA256H: SHA-256 hash update (part 1).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA256H2: SHA-256 hash update (part 2).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA256SU0: SHA-256 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA256SU1: SHA-256 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA512H: SHA-512 hash update part 1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA512H2: SHA-512 hash update part 2.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA512SU0: SHA-512 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHA512SU1: SHA-512 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHADD: Signed halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHL: Shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHLL, SHLL2: Shift left long (by element size).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHRN, SHRN2: Shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SHSUB: Signed halving subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SLI: Shift left and insert (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3PARTW1: SM3PARTW1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3PARTW2: SM3PARTW2.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3SS1: SM3SS1.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3TT1A: SM3TT1A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3TT1B: SM3TT1B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3TT2A: SM3TT2A.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM3TT2B: SM3TT2B.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM4E: SM4 encode.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SM4EKEY: SM4 key.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMAX: Signed maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMAXP: Signed maximum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMAXV: Signed maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMIN: Signed minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMINP: Signed minimum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMINV: Signed minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMLAL, SMLAL2 (by element): Signed multiply-add long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMLAL, SMLAL2 (vector): Signed multiply-add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMLSL, SMLSL2 (by element): Signed multiply-subtract long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMLSL, SMLSL2 (vector): Signed multiply-subtract long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMMLA (vector): Signed 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMOV: Signed move vector element to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMULL, SMULL2 (by element): Signed multiply long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SMULL, SMULL2 (vector): Signed multiply long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQABS: Signed saturating absolute value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQADD: Signed saturating add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMLAL, SQDMLAL2 (by element): Signed saturating doubling multiply-add long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMLAL, SQDMLAL2 (vector): Signed saturating doubling multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMLSL, SQDMLSL2 (by element): Signed saturating doubling multiply-subtract long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMLSL, SQDMLSL2 (vector): Signed saturating doubling multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMULH (by element): Signed saturating doubling multiply returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMULH (vector): Signed saturating doubling multiply returning high half.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMULL, SQDMULL2 (by element): Signed saturating doubling multiply long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQDMULL, SQDMULL2 (vector): Signed saturating doubling multiply long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQNEG: Signed saturating negate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMLAH (by element): Signed saturating rounding doubling multiply accumulate returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMLAH (vector): Signed saturating rounding doubling multiply accumulate returning high half (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMLSH (by element): Signed saturating rounding doubling multiply subtract returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMLSH (vector): Signed saturating rounding doubling multiply subtract returning high half (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMULH (by element): Signed saturating rounding doubling multiply returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRDMULH (vector): Signed saturating rounding doubling multiply returning high half.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRSHL: Signed saturating rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRSHRN, SQRSHRN2: Signed saturating rounded shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQRSHRUN, SQRSHRUN2: Signed saturating rounded shift right unsigned narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSHL (immediate): Signed saturating shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSHL (register): Signed saturating shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSHLU: Signed saturating shift left unsigned (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSHRN, SQSHRN2: Signed saturating shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSHRUN, SQSHRUN2: Signed saturating shift right unsigned narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQSUB: Signed saturating subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQXTN, SQXTN2: Signed saturating extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SQXTUN, SQXTUN2: Signed saturating extract unsigned narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SRHADD: Signed rounding halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SRI: Shift right and insert (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SRSHL: Signed rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SRSHR: Signed rounding shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SRSRA: Signed rounding shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSHL: Signed shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSHLL, SSHLL2: Signed shift left long (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSHR: Signed shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSRA: Signed shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSUBL, SSUBL2: Signed subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SSUBW, SSUBW2: Signed subtract wide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST1 (multiple structures): Store multiple single-element structures from one, two, three, or four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST1 (single structure): Store a single-element structure from one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST2 (multiple structures): Store multiple 2-element structures from two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST2 (single structure): Store single 2-element structure from one lane of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST3 (multiple structures): Store multiple 3-element structures from three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST3 (single structure): Store single 3-element structure from one lane of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST4 (multiple structures): Store multiple 4-element structures from four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ST4 (single structure): Store single 4-element structure from one lane of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STBFADD, STBFADDL: Atomic BFloat16 floating-point add, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STBFMAX, STBFMAXL: Atomic BFloat16 floating-point maximum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STBFMAXNM, STBFMAXNML: Atomic BFloat16 floating-point maximum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STBFMIN, STBFMINL: Atomic BFloat16 floating-point minimum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STBFMINNM, STBFMINNML: Atomic BFloat16 floating-point minimum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STFADD, STFADDL: Atomic floating-point add, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STFMAX, STFMAXL: Atomic floating-point maximum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STFMAXNM, STFMAXNML: Atomic floating-point maximum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STFMIN, STFMINL: Atomic floating-point minimum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STFMINNM, STFMINNML: Atomic floating-point minimum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STL1 (SIMD&FP): Store-release a single-element structure from one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STLUR (SIMD&FP): Store-release SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STNP (SIMD&FP): Store pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STP (SIMD&FP): Store pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STR (immediate, SIMD&FP): Store SIMD&FP register (immediate offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STR (register, SIMD&FP): Store SIMD&FP register (register offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTNP (SIMD&FP): Store unprivileged pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STTP (SIMD&FP): Store unprivileged pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - STUR (SIMD&FP): Store SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUB (vector): Subtract (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUBHN, SUBHN2: Subtract returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUDOT (by element): Dot product with signed and unsigned integers (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SUQADD: Signed saturating accumulate of unsigned value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - SXTL, SXTL2: Signed extend long: an alias of SSHLL, SSHLL2.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TBL: Table vector lookup.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TBX: Table vector lookup extension.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TRN1: Transpose vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - TRN2: Transpose vectors (secondary).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UABA: Unsigned absolute difference and accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UABAL, UABAL2: Unsigned absolute difference and accumulate long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UABD: Unsigned absolute difference (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UABDL, UABDL2: Unsigned absolute difference long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UADALP: Unsigned add and accumulate long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UADDL, UADDL2: Unsigned add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UADDLP: Unsigned add long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UADDLV: Unsigned sum long across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UADDW, UADDW2: Unsigned add wide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UCVTF (scalar SIMD&FP): Unsigned integer convert to floating-point (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UCVTF (scalar, fixed-point): Unsigned fixed-point convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UCVTF (scalar, integer): Unsigned integer convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UCVTF (vector, fixed-point): Unsigned fixed-point convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UCVTF (vector, integer): Unsigned integer convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UDOT (by element): Dot product unsigned arithmetic (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UDOT (vector): Dot product unsigned arithmetic (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UHADD: Unsigned halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UHSUB: Unsigned halving subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMAX: Unsigned maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMAXP: Unsigned maximum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMAXV: Unsigned maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMIN: Unsigned minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMINP: Unsigned minimum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMINV: Unsigned minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMLAL, UMLAL2 (by element): Unsigned multiply-add long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMLAL, UMLAL2 (vector): Unsigned multiply-add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMLSL, UMLSL2 (by element): Unsigned multiply-subtract long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMLSL, UMLSL2 (vector): Unsigned multiply-subtract long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMMLA (vector): Unsigned 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMOV: Unsigned move vector element to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMULL, UMULL2 (by element): Unsigned multiply long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UMULL, UMULL2 (vector): Unsigned multiply long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQADD: Unsigned saturating add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQRSHL: Unsigned saturating rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQRSHRN, UQRSHRN2: Unsigned saturating rounded shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQSHL (immediate): Unsigned saturating shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQSHL (register): Unsigned saturating shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQSHRN, UQSHRN2: Unsigned saturating shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQSUB: Unsigned saturating subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UQXTN, UQXTN2: Unsigned saturating extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URECPE: Unsigned reciprocal estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URHADD: Unsigned rounding halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URSHL: Unsigned rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URSHR: Unsigned rounding shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URSQRTE: Unsigned reciprocal square root estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - URSRA: Unsigned rounding shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USDOT (by element): Dot product with unsigned and signed integers (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USDOT (vector): Dot product with unsigned and signed integers (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USHL: Unsigned shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USHLL, USHLL2: Unsigned shift left long (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USHR: Unsigned shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USMMLA (vector): Unsigned and signed 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USQADD: Unsigned saturating accumulate of signed value.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USRA: Unsigned shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USUBL, USUBL2: Unsigned subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - USUBW, USUBW2: Unsigned subtract wide.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UXTL, UXTL2: Unsigned extend long: an alias of USHLL, USHLL2.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UZP1: Unzip vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - UZP2: Unzip vectors (secondary).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - XAR: Exclusive-OR and rotate.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - XTN, XTN2: Extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ZIP1: Zip vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] JIT [x] Tests - ZIP2: Zip vectors (secondary).
