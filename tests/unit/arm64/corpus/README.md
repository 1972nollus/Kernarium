# A64 instruction unit corpus

`a64_instruction_corpus.pack` is the self-contained input of the ordinary
`arm64-instruction-corpus` CTest. Its runner,
[`../instruction_corpus_runner.cpp`](../instruction_corpus_runner.cpp),
executes every case of the build's page profile through the production gadget
engine and, when the host implements the case's native features or the case
is an invalid encoding, through the JIT. A case that needs FEAT_LSE2 runs in
neither lane on a host without it, and a case that needs FEAT_AFP or
FEAT_RPRES runs in neither lane where the virtual CPU does not implement it
(see [FEAT_AFP cases](#feat_afp-cases)). The runner needs no Python, network,
or file other than the pack.

A case is one engine-harness check exported from a producer unit test of a
producer commit (see [Regeneration](#regeneration-and-new-cases)): a program
on the code page at `0x4f000000`, the initial architectural state and
data regions, and the expected terminal event, retired-instruction count,
fault address and access, floating-point syndrome, architectural state, and
data bytes. The code page must be unchanged after every run.
A case whose architecture permits several results names a validator relation
instead of an exact state.

- `manifest.tsv` has one row per record: a producer CTest, check site, harness
  options, and page profiles, with program and case counts and hashes; the
  code hash also covers a record's SVL.
- `coverage.tsv` counts cases per `../a64_instruction_inventory.tsv` entry,
  attributing each entry word through its official allocation predicates and
  decode partition, with illegal-instruction cases counted separately.
- `provenance.json` records the producer commit, the force-include file and
  its hash, sampling limit and seed, per-profile export, boundary, forced,
  and exclusion counts, and the pack hash.
- `forced-cases.tsv` forces producer checks that the class samples leave out;
  see [Regeneration and new cases](#regeneration-and-new-cases).

## Sampling and minimization

The exporter classifies every producer case by check site, terminal event,
invalid-encoding flag, harness options, retirement and fault expectation,
validator relation, native requirements, entry offset, and, for every program
word, its official decode partition, the allocation predicates it matches,
and which register fields name register 31. Each class contributes its first
and last case plus a fixed-seed uniform sample, at most eight cases.

Each class also contributes every case that gives an operand field of one of
its program words a boundary value that no earlier such case of the class
gave that field. A word's operand fields are the bits its decode partition
leaves free: a five-bit register field at bit 0, 5, 10, or 16 when all five
bits are free, and each maximal run of free bits elsewhere. Boundary values
are 0, 30, and 31 for a register field, 0 and 1 for a single bit, and 0 and
all ones for a wider field. A decoder fault that only one register number or
immediate extreme exposes therefore stays visible to the corpus.

A case is exported only after the producer harness's interpreter, gadget, and
JIT lanes all pass it. Its retired count is the producer's declared count or,
when none was declared, the count on which every lane agreed.

Registers a program cannot name are replaced with a canonical fill, and the
minimized case is verified through every lane again. A failing candidate falls
back to a broader register set and then to the unmodified state. Cases with a
validator relation keep their complete state. Cases with engine hooks or a
deferred floating-point entry state stay in focused tests that still run
through the gadget and JIT harness; `provenance.json` counts them.

Identical cases exported by both page profiles are stored once; a record with
a `/16k` or `/4k` suffix holds profile-specific cases.

## Validator relations

| Relation | Accepted result |
| --- | --- |
| `host-counter-frequency/v1` | expected state with the host's CNTFRQ_EL0 in each MRS CNTFRQ_EL0 destination |
| `ignore-host-counter-register/v1` | expected state except system-register read destinations |
| `permitted-exclusive-store-failure/v1` | expected state except the opaque exclusive-monitor value and version |

Relations derive their operands from the program, so a new relation or a new
use of an existing one requires a runner change.

## Pack format

All integers are little-endian. The file is `UMLA64C\0`, u32 version 2, u32 0,
and u64 body length, followed by the zlib-compressed body:

```text
body     = meta:str32  tokens:u16*str16  native:u16*(u8*u16)
           validators:u16*str16  blobs:u32*(u32 size, bytes)  records:u32*record
record   = name:str16 producer:str16 site:str16 profiles:u8 options:u16 svl:u16
           programs:u32*(entry:u16, u16*(offset:u16, word:u32))  cases:u32*case
case     = program:u32 name:str16 ordinal_16k:u32 ordinal_4k:u32 event:u8
           flags:u8 retire [fault] [syndrome] native:u16 validator:u16
           initial:state expected:state regions:u8*region
retire   = 0 exact:u32 | 1 minimum:u32 step:u32 maximum:u32 | 2
fault    = address:u64 access:u8        present when flags bit 1 is set
syndrome = u32                          present for floating-point-trap
state    = u32 mask, u64 per set bit (bit 31 is SP)
           u32 mask, 16 bytes per set vector
           [sme]                                present when svl is not 0
           u8 count, (field id:u8, value) per field
sme      = u32 mask, svl bytes per set Z register
           u16 mask, svl/8 bytes per set P register
           svl/8-byte mask, svl bytes per set ZA row (horizontal vector)
           u8 0 | u8 1, 64 bytes                ZT0
region   = address:u64 size:u32 flags:u8 blob:u32
           u16*(offset:u32, length:u16, bytes)   expected bytes
           u16 0  u16 0                          retired tag lists
```

`str16` and `str32` are length-prefixed UTF-8. `meta` is JSON. Native sets
index the stable `arm_feature_tokens` table of
`darwin/platform/host_capabilities.hpp`. Case flags hold the invalid-encoding
bit, the fault-compared bit, and the minimization level (0 full, 1 narrow,
2 broad) in bits 2-3. `profiles` has bit 0 for 16 KiB and bit 1 for 4 KiB.
Option bits 0 and 1 are reserved, bits 2-9, in order, force the portable
checksum, flag, SHA3, fixed-transform, AES, PMULL, dot-product, and I8MM
gadget bodies, bit 10 (`without_afp`) translates the record for a virtual
CPU without FEAT_AFP on every host, and bit 11 (`without_sme`) does the same
for FEAT_SME. Region flags are readable, writable,
executable, guest code, and guest writable. Events follow the harness order:
system call, memory fault, reserved, illegal instruction, breakpoint,
floating-point trap, PC alignment, instruction budget, fatal. The reserved
option bits, event code 2, the field ids the table below omits, and non-empty
tag lists held FEAT_PAuth,
FEAT_MTE, FEAT_GCS, FEAT_CPA, FEAT_LS64, FEAT_FP8, and FEAT_SSBS state and
the producer's one-byte PSTATE.SM and ZA (see
[Absent-feature cases](#absent-feature-cases)); the runner rejects a pack
that uses them.
Validator `0xffff` means exact comparison; retirement maximum `0xffffffff` is
unbounded. Ordinals are the producer check ordinals, zero where a profile did
not export the case.

The initial state is encoded against a base state and the expected state
against the initial one. In the base, `Xi` (and SP as `i = 31`) is
`0x5eed000000000000 | i << 40 | 0x01010101 * (i + 1)`, byte `b` of `Vi` is
`((16i + b) * 0x9d + 0x3b) mod 256`, PC is the entry address, the exclusive
address is all ones, and every other field is zero. Field ids are:

| Id | Field | Id | Field |
| --- | --- | --- | --- |
| 1 | `pc` u64 | 25 | `exclusive_address` u64 |
| 2 | `nzcv` u32 | 26 | `exclusive_key` u64 |
| 3 | `fpcr` u32 | 27 | `exclusive_value` u64 |
| 4 | `fpsr` u32 | 28 | `exclusive_value2` u64 |
| 6 | `tls` u64 | 29 | `exclusive_version` u64 |
| 8 | `tbi_enabled` u8 | 30 | `exclusive_size` u8 |
| 19 | `pstate_dit` u8 | 31 | `fp_dirty` u8 |
| 23 | `pstate_daif_access` u8 | | |
| 24 | `pstate_daif` u8 | | |

| 38 | `svcr` u8 | 39 | `tpidr2` u64 |

Ids 5, 7, 9-18, 20-22, and 32-37 are reserved. A region's initial bytes are its
blob, and its expected bytes are the blob with the runs applied.

### SME state

A record's `svl` is the streaming vector length in bytes that its cases' SME
state is sized for and their expectations hold at, or 0 for a record without
SME state. Only a record with an SVL holds Z, P, ZA, ZT0, `svcr` (PSTATE.SM in
bit 0, PSTATE.ZA in bit 1), or `tpidr2`, and the runner runs its cases only
where the virtual CPU implements FEAT_SME with that SVL, so no case assumes
an SVL it does not name. Z and P exist only while a state's PSTATE.SM is set,
and its V registers are then the low 16 bytes of the Z registers, so its
vector mask is empty; ZA and ZT0 exist only while its PSTATE.ZA is set. The
expected state's SVCR is the initial one unless it names its own, and the
runner compares the SME state only where the expected SVCR makes it live.
An expected part that the initial state does not hold, such as Z after
SMSTART, is encoded against its base fill, and the V registers of an initial
state in streaming mode are its Z registers' low bytes. Each byte of the SME base fill is `(n * 0x9e3779b1 mod 2^32) >> 24` for an
index `n` that names the byte's position at the largest SVL: `256i + b` for
byte `b` of `Zi`, `0x2000 + 32i + b` of `Pi`, `0x10000 + 256r + b` of ZA row
`r`, and `0x30000 + b` of ZT0. A version 1 pack is a version 2 pack without
SVLs; `build-unit-corpus.py` still reads one, and writes only version 2.

## Running

```sh
uml_arm64_instruction_corpus_tests tests/unit/arm64/corpus/a64_instruction_corpus.pack \
  [--record TEXT] [--producer NAME] [--case TEXT] [--ordinal N] \
  [--engine gadgets|jit] [--assume-host-feature TOKEN] \
  [--max-failures N | --fail-fast] [--list]
```

Filters combine; `--ordinal` selects producer check ordinals of the build's
page profile. `--assume-host-feature` translates as if the host had the
feature; a case that needs it natively still needs the hardware to have it.
The `arm64-instruction-corpus-afp-twins` CTest runs the pack with every
feature `implements_afp()` needs assumed, so the FEAT_AFP twins run on every
host and must match the base entries on the cases whose FPCR leaves the
controls clear. The default stops after 25 failures; `--max-failures 0` never
stops early.
A failure names the record, case index, producer check ordinals, entry word,
engine, and the first mismatch. The summary counts gadget and JIT runs and the
JIT runs skipped for each missing host feature.

## Regeneration and new cases

Expected results come from independent derivation from the architecture or
from observation on native hardware, never from an engine's output.

The exporter, its driver, and the producer unit tests exist only in producer
commits; `provenance.json` names the one that produced the current pack as
`producer_commit`. Later commits removed the harness's interpreter lane and
export mode, `tools/arm64-tests/export-unit-corpus.py`, and the producers whose
cases the corpus holds. To add a case, check out the producer commit and copy
the current `forced-cases.tsv` into it, add or extend a check in a producer
unit test under the rule above or force an existing producer check into the
export, and build and export both page profiles there:

```sh
python3 -B tools/arm64-tests/export-unit-corpus.py \
  --build-dir .build/presets/macos-unit-16k --output .build/a64-corpus/16k \
  --include tests/unit/arm64/corpus/forced-cases.tsv
python3 -B tools/arm64-tests/export-unit-corpus.py \
  --build-dir .build/presets/macos-unit-4k --output .build/a64-corpus/4k \
  --include tests/unit/arm64/corpus/forced-cases.tsv
```

The exports still hold the state of features the virtual CPU no longer
implements, so this checkout's builder writes them as a dump, not a pack, for
the post-export rewrites below, with `PRODUCER` naming the producer checkout:

```sh
python3 -B tools/arm64-tests/build-unit-corpus.py \
  --export-16k "$PRODUCER/.build/a64-corpus/16k" \
  --export-4k "$PRODUCER/.build/a64-corpus/4k" \
  --include tests/unit/arm64/corpus/forced-cases.tsv \
  --producer-commit "$(git -C "$PRODUCER" rev-parse HEAD)" --text corpus.jsonl
```

`forced-cases.tsv` holds the force-include directives; their syntax is
described in the producer commit's
`tests/unit/arm64/arm64_instruction_corpus_export.hpp`. Each directive forces a
producer check that reaches something no sampled case reaches, and the
comments before it name that gadget entry, code region or branch direction,
or injected engine fault. Its `case` directives name check ordinals of the
producer commit, so a producer change that renumbers checks must re-pin that
producer's directives; the export summary reports a directive whose check is
missing or excluded. A harness abort names the ordinal of the failing check,
and each export directory lists the ordinal of every exported case in
`<producer>.cases.tsv`. Carry the regenerated pack, `manifest.tsv`,
`coverage.tsv`, `provenance.json`, and `forced-cases.tsv` back as one change.

`build-unit-corpus.py --check` verifies that the metadata and the canonical
encoding match the pack, that every inventory entry has a case or an
illegal-instruction case, and that every row of
[`../a64_instruction_evidence_bindings.tsv`](../a64_instruction_evidence_bindings.tsv)
names, by producer and case name, a pack case whose entry word is its witness,
or `-` for both columns. A regeneration that drops a binding's case names
another case that executes the witness, moves the witness to the entry word of
a kept case that the allocation generator maps to the same allocation row
(only `binding_encoding` changes), or sets `ctest` and `case` to `-`.
`--dump PACK --text FILE` writes a reviewable JSON lines form, and
`--repack FILE` rebuilds the pack and metadata from it.

## LSE2 alignment cases

The producer checks predate the FEAT_LSE2 alignment rule (see
[`docs/architecture.md`](../../../../docs/architecture.md)) and expect an
alignment fault from a misaligned LSE atomic or exclusive access inside
one 16-byte block, and the producer harness's interpreter lane does not
implement the rule. [`tools/arm64-tests/lse2-alignment-corpus.py`](../../../../tools/arm64-tests/lse2-alignment-corpus.py)
therefore rewrites a dump after export: it drops those cases and appends the
hand-authored `arm64-lse2-alignment` records, whose completing and
block-crossing cases follow the architecture and match native LSE2 hardware.
`provenance.json` records its hash and counts under `lse2_alignment`.

## Absent-feature cases

The virtual CPU implements none of FEAT_PAuth, FEAT_MTE, FEAT_GCS, and
FEAT_CPA: their non-hint encodings are UNDEFINED and their hint-space
encodings, such as PACIASP, AUTIASP, and GCSB, are NOPs. FEAT_PAuth_LR stays
unimplemented as before, so its cases are unchanged. Nor does it implement the
memory extensions FEAT_LSE128, FEAT_THE and FEAT_D128, FEAT_LRCPC3 (whose
LDAPR and STLR writeback forms join them), FEAT_LSCP, FEAT_LS64 with its
ACCDATA state, FEAT_LSFE, FEAT_LSUI, and FEAT_LOR, or the SIMD&FP extensions
FEAT_FP8 (with FP8DOT2, FP8DOT4, FP8FMA, F8F16MM, F8F32MM, and its FPMR state),
FEAT_F16F32DOT, FEAT_F16MM and FEAT_F16F32MM, FEAT_LUT, FEAT_FAMINMAX,
FEAT_SM3, FEAT_SM4, and FEAT_FPRCVT, or FEAT_CMPBR, FEAT_HBC, FEAT_SSBS,
FEAT_XS, FEAT_WFxT, FEAT_SVE, or any SVE or SME feature outside the SME
profile; every encoding they add is UNDEFINED. The producer harness
enabled the removed features in most cases' state, so
[`tools/arm64-tests/absent-feature-corpus.py`](../../../../tools/arm64-tests/absent-feature-corpus.py)
rewrites the dump after the LSE2 step. It drops the cases that exercise a
removed feature or hold one of its encodings without expecting it to be
undefined; a case that expects the undefined-instruction trap stays, because
its encodings stay undefined. It strips the removed state fields, harness
option, and allocation tags from every other case and record, and drops the
cases that `absent-feature-failing.tsv` lists because they fail without that
state. It then appends the `arm64-absent-<feature>` records, which expect at
least one word of every encoding class to be undefined and the hints to be
NOPs, and `arm64-absent-mte-tbi` records that restore the coverage the failing
cases took from their allocations.

FEAT_SME is two-sided, like FEAT_AFP: the virtual CPU implements the SME
profile only where the host does. The producer's SME model is retired, so the
rewrite drops its SME cases and strips its one-byte PSTATE.SM and ZA fields as
for an absent feature, but its `arm64-absent-sme` record carries the
`without_sme` option, which the runner translates for on every host. That
record expects SMSTART, SMSTOP, MRS and MSR SVCR, and the witness word of
every profile encoding in the generated SVE/SME encoding table
([`darwin/cpu/arm64/detail/sme_encodings.hpp`](../../../../darwin/cpu/arm64/detail/sme_encodings.hpp))
to be undefined, and so is the evidence-binding witness of each SVE or SME
entry without a table row: an alias, which its base instruction's row
decodes, or MOVT between ZT0 and a general register, which only Debug state
executes. The `arm64-absent-sve` record expects the witness of every table
encoding outside the profile to be undefined with or without SME. Each
witness is checked against LLVM MC by the table's generator or the allocation
generator, and its case name gives the entry's stable id, encoding or form,
and word.

FEAT_EBF16 has no instructions of its own, and the virtual CPU omits it too:
FPCR.EBF is RES0. The rewrite drops the cases that set EBF and need the
feature natively, clears it from every other case's FPCR, and drops the
listed cases that fail once it is clear. A native requirement on a case whose
FPCR does not set it came from the engines' former host gates, and the
rewrite removes it. The pack encoder refuses a state that sets EBF.

Both engines implement FEAT_CSSC and FEAT_MOPS on every host, so their native
requirements were likewise only the producer JIT's host gates. The rewrite
removes them from every case, and the JIT lane runs those cases on every host.

A failing MTE case's tagged address relied on the top-byte ignore that MTE
implied. Its replacement takes a kept two-word case of the same SIMD&FP
load/store allocation that completes through a general-register base with a
zero top byte, sets `tbi_enabled`, and tags the base; the architectural result
is the kept case's, except that a written-back base keeps its tag.
`provenance.json` records the script's hash, its feature list, the failing
file's and encoding table's hashes, and the drop, strip, host-gate, FEAT_AFP,
and addition counts under `absent_features`, and `build-unit-corpus.py
--check` verifies them. The
`arm64-instruction-corpus-absent-features` tooling test checks the pack
against the rewrite's rules: no kept case exercises a removed feature, holds
one of its encodings without expecting the undefined-instruction trap, or
keeps a host gate or a FEAT_AFP requirement its FPCR does not need; the RES0
pins are exactly the `without_afp` records' cases; and every authored record
and case is the rewrite's, in the counts it recorded.

Run both steps on the dump of every regenerated export and `--repack` the
result:

```sh
python3 -B tools/arm64-tests/lse2-alignment-corpus.py corpus.jsonl lse2.jsonl
python3 -B tools/arm64-tests/absent-feature-corpus.py \
  --failing tests/unit/arm64/corpus/absent-feature-failing.tsv lse2.jsonl absent.jsonl
python3 -B tools/arm64-tests/build-unit-corpus.py --repack absent.jsonl \
  --output tests/unit/arm64/corpus
```

A regeneration that changes which stripped cases fail refreshes the failing
list: rewrite without `--failing`, run the corpus test on that pack in both
page profiles with `--max-failures 0`, and pass both logs to
`absent-feature-corpus.py --collect-failing` with the rewrite's input dump as
`--source` and the current list as `--previous`. A case the current list
names keeps its feature; a newly failing case is attributed to the newest
feature whose state the rewrite stripped from it.

## Native SME cases

The producer had no SME model.
[`tools/arm64-tests/sme-native-corpus.py`](../../../../tools/arm64-tests/sme-native-corpus.py)
authors the `arm64-sme-native` records from the generated SVE/SME encoding
table and appends them after the absent-feature step, and
[`tools/arm64-tests/sme_native_oracle.cpp`](../../../../tools/arm64-tests/sme_native_oracle.cpp)
observes their outcomes on an SME host. The script's docstring lists what
each record covers and owns the oracle's request and response formats.

Every native record has `svl` 64, the M4's streaming vector length and the
only one with native expectations. The runner runs its cases only for an
engine whose `sme_support` is at least `complete` where the virtual CPU
implements SME at that SVL, and a case that needs `sme2` only for an engine
at `sme2` where the virtual CPU has the SME2 profile; it counts every other
case as skipped for the feature. Each case names the profile features it
needs (`sme`, `sme2`, `sme_f64f64`, `sme_i16i64`, `bf16`, `i8mm`). A case
starts with X18 zero, because Darwin clears X18 whenever a thread returns
to user mode, and forms only addresses with a zero top byte, because the
host ignores the top byte and the guest's default state does not.

The oracle catches a case's traps on a Mach exception port and reads its
state there, without a signal frame, so streaming state reaches the response
unchanged. XNU sets FPSR to 0x0800009F on every kernel entry in streaming
mode, interrupts included, so the oracle sets a case's FPSR in user mode
after its own last trap before the program. For a case that ends in
streaming mode it runs the case again with the word it ended at replaced by
a branch to an FPSR read, so the response holds the FPSR the instructions
left. The script reads only these version 2 responses, and the
`arm64-instruction-corpus-sme-native` test refuses a pack in which a case
that keeps PSTATE.SM set expects the reset value.

Where the encoding table decides a case's legality (each row's witness in
every mode it needs and in each mode that lacks one, the rows outside the
profile, and every FP&SIMD class in streaming mode), the script refuses a
response that disagrees, so a disagreement is a finding about the table,
never an expectation. The FP&SIMD classes of `absent` inventory entries are
left out: the host may implement their features (the M4 executes the
FEAT_LRCPC3 forms), but the guest's result is UNDEFINED, which the
absent-feature records expect. A witness the architecture leaves CONSTRAINED
UNPREDICTABLE, a SIMD&FP load pair with `Rt == Rt2`, takes `Rt2 = Rt + 1`
instead. The hardware decides everything else, including the fault address
of a crossing access and what a faulting access leaves in its destination.

Regenerate on an SME host, twice from clean directories (the M4 under its
lease and workdir rules), and require identical responses:

```sh
python3 -B tools/arm64-tests/sme-native-corpus.py --request sme.request
sme_native_oracle sme.request sme.response                # on the SME host
python3 -B tools/arm64-tests/sme-native-corpus.py absent.jsonl native.jsonl \
  --response sme.response --response sme.response2
python3 -B tools/arm64-tests/build-unit-corpus.py --repack native.jsonl \
  --output tests/unit/arm64/corpus
```

A regeneration that leaves the request unchanged needs no SME host:
`--carry tests/unit/arm64/corpus/a64_instruction_corpus.pack` copies the
current native records instead of `--response`. `provenance.json` records the
script, table, and request hashes and the outcome counts under `sme_native`,
`build-unit-corpus.py --check` verifies the hashes, and the
`arm64-instruction-corpus-sme-native` tooling test re-derives every case but
its outcome. The `arm64-sme-native-oracle` tooling test drives the oracle
with base A64 cases on any arm64 Mac.

## FEAT_AFP cases

FEAT_AFP has no instructions of its own. The virtual CPU implements it
exactly where `implements_afp()` in
[`darwin/cpu/arm64/floating_controls.hpp`](../../../../darwin/cpu/arm64/floating_controls.hpp)
says, and FEAT_RPRES where `implements_rpres()` says. The absent-feature
rewrite keeps every case whose FPCR sets FIZ, AH, or NEP, with those controls
and a native `afp` requirement, adding the requirement where the producer
harness left it out; the runner skips such a case in both lanes elsewhere. A
native `afp` requirement on a case whose FPCR sets none of them was a former
host gate and is removed. A case whose entry `MSR FPCR` writes a control that
its FPCR state never holds pins the controls RES0; the rewrite moves it to a
sibling record with the `without_afp` option. The pack encoder refuses a
state that sets the controls without the native requirement. The
single-precision FRECPE and FRSQRTE cases under FPCR.AH also need `rpres`:
their producer models FEAT_RPRES with FEAT_AFP, as both engines do on hosts
with both. No case pins AH tininess for FADD, FMUL, FDIV, FSQRT, FMADD or
vector FMLA: on an FEAT_AFP host both engines run them natively under the
guest FPCR, so their AH behavior is the hardware's, and a native oracle run
on such a host covers it.
