# A64 instruction unit corpus

`a64_instruction_corpus.pack` is the self-contained input of the ordinary
`arm64-instruction-corpus` CTest. Its runner,
[`../instruction_corpus_runner.cpp`](../instruction_corpus_runner.cpp),
executes every case of the build's page profile through the production gadget
engine and, when the host implements the case's native features or the case
is an invalid encoding, through the JIT. A case that needs FEAT_LSE2 runs in
neither lane on a host without it. The runner needs no Python, network, or
file other than the pack.

A case is one engine-harness check exported from a producer unit test of a
producer commit (see [Regeneration](#regeneration-and-new-cases)): a program
on the code page at `0x4f000000`, the initial architectural state and
data regions, and the expected terminal event, retired-instruction count,
fault address and access, floating-point syndrome, architectural state, data
bytes, and allocation tags. The code page must be unchanged after every run.
A case whose architecture permits several results names a validator relation
instead of an exact state.

- `manifest.tsv` has one row per record: a producer CTest, check site, harness
  options, and page profiles, with program and case counts and hashes.
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
| `guarded-control-call-fault-atomicity/v1` | exact state except the signed target register, whose stripped value is unchanged |
| `guarded-control-return-fault/v1` | exact state except X30, whose stripped value is unchanged |
| `guarded-control-zero-target-fault/v1` | expected state with a faulting PC whose stripped address is zero |
| `ignore-host-counter-register/v1` | expected state except system-register read destinations |
| `permitted-exclusive-store-failure/v1` | expected state except the opaque exclusive-monitor value and version |
| `permitted-tag-generator-choice/v1` | initial state with an IRG result that keeps the address and uses a permitted tag, and any generator state |
| `permitted-transformed-branch-failure/v1` | initial state plus the link update, with a faulting PC whose stripped address is zero or, when the entry signed the target with itself, the original target |
| `permitted-transformed-code/v1` | initial state with a PAC or AUT result that retains the architectural pointer bits |
| `permitted-transformed-generic/v1` | initial state with a PACGA result whose low half is zero |
| `transformed-address-fault-state/v1` | initial state except the signed base register, whose stripped value is unchanged |

Relations derive their operands from the program, so a new relation or a new
use of an existing one requires a runner change.

## Pack format

All integers are little-endian. The file is `UMLA64C\0`, u32 version 1, u32 0,
and u64 body length, followed by the zlib-compressed body:

```text
body     = meta:str32  tokens:u16*str16  native:u16*(u8*u16)
           validators:u16*str16  blobs:u32*(u32 size, bytes)  records:u32*record
record   = name:str16 producer:str16 site:str16 profiles:u8 options:u16
           programs:u32*(entry:u16, u16*(offset:u16, word:u32))  cases:u32*case
case     = program:u32 name:str16 ordinal_16k:u32 ordinal_4k:u32 event:u8
           flags:u8 retire [fault] [syndrome] native:u16 validator:u16
           initial:state expected:state regions:u8*region
retire   = 0 exact:u32 | 1 minimum:u32 step:u32 maximum:u32 | 2
fault    = address:u64 access:u8        present when flags bit 1 is set
syndrome = u32                          present for floating-point-trap
state    = u32 mask, u64 per set bit (bit 31 is SP)
           u32 mask, 16 bytes per set vector
           u8 count, (field id:u8, value) per field
region   = address:u64 size:u32 flags:u8 blob:u32
           u16*(offset:u32, length:u16, bytes)   expected bytes
           u16*(offset:u32, tag:u8)              initial tags
           u16*(offset:u32, tag:u8)              expected tags
```

`str16` and `str32` are length-prefixed UTF-8. `meta` is JSON. Native sets
index the stable `arm_feature_tokens` table of
`darwin/platform/host_capabilities.hpp`. Case flags hold the invalid-encoding
bit, the fault-compared bit, and the minimization level (0 full, 1 narrow,
2 broad) in bits 2-3. `profiles` has bit 0 for 16 KiB and bit 1 for 4 KiB.
Option bits, in order from bit 0, enable pointer authentication and guarded
control flow and force the portable checksum, flag, SHA3, fixed-transform,
AES, PMULL, dot-product, and I8MM gadget bodies. Region flags are readable,
writable, executable, guest code, and guest writable. Events follow the
harness order: system call, memory fault, GCS fault, illegal instruction,
breakpoint, floating-point trap, PC alignment, instruction budget, fatal.
Validator `0xffff` means exact comparison; retirement maximum `0xffffffff` is
unbounded. Ordinals are the producer check ordinals, zero where a profile did
not export the case.

The initial state is encoded against a base state and the expected state
against the initial one. In the base, `Xi` (and SP as `i = 31`) is
`0x5eed000000000000 | i << 40 | 0x01010101 * (i + 1)`, byte `b` of `Vi` is
`((16i + b) * 0x9d + 0x3b) mod 256`, PC is the entry address, MTE seed is 1,
the tag generator state is `0x9e3779b97f4a7c15`, the exclusive address is all
ones, pointer authentication is enabled with the deterministic `CpuState`
keys, and every other field is zero. Field ids are:

| Id | Field | Id | Field |
| --- | --- | --- | --- |
| 1 | `pc` u64 | 20 | `pstate_tco` u8 |
| 2 | `nzcv` u32 | 21 | `pstate_sm` u8 |
| 3 | `fpcr` u32 | 22 | `pstate_za` u8 |
| 4 | `fpsr` u32 | 23 | `pstate_daif_access` u8 |
| 5 | `fpmr` u64 | 24 | `pstate_daif` u8 |
| 6 | `tls` u64 | 25 | `exclusive_address` u64 |
| 7 | `mte_enabled` u8 | 26 | `exclusive_key` u64 |
| 8 | `tbi_enabled` u8 | 27 | `exclusive_value` u64 |
| 9 | `mte_exclude` u16 | 28 | `exclusive_value2` u64 |
| 10 | `mte_tag_choices` 256 bytes | 29 | `exclusive_version` u64 |
| 11 | `mte_random_tags` u8 | 30 | `exclusive_size` u8 |
| 12 | `mte_seed` u16 | 31 | `fp_dirty` u8 |
| 13 | `mte_tag` u8 | 32 | `gcs_pointer` u64 |
| 14 | `mte_random_state` u64 | 33 | `gcs_implemented` u8 |
| 15 | `cpa_enabled` u8 | 34 | `gcs_push_enabled` u8 |
| 16 | `cpa_multiply_enabled` u8 | 35 | `gcs_store_enabled` u8 |
| 17 | `accdata` u32 | 36 | `pointer_auth_keys` 5 x (low, high) u64 |
| 18 | `pstate_ssbs` u8 | 37 | `pointer_authentication_enabled` u8 |
| 19 | `pstate_dit` u8 | | |

An omitted field 10 is the table `CpuState::set_mte_configuration()` derives
from fields 7 and 9. A region's initial bytes are its blob, and its expected
bytes are the blob with the runs applied. When either tag list is non-empty,
every granule is restored to its initial tag (zero unless listed) and compared
with its expected tag.

## Running

```sh
uml_arm64_instruction_corpus_tests tests/unit/arm64/corpus/a64_instruction_corpus.pack \
  [--record TEXT] [--producer NAME] [--case TEXT] [--ordinal N] \
  [--engine gadgets|jit] [--max-failures N | --fail-fast] [--list]
```

Filters combine; `--ordinal` selects producer check ordinals of the build's
page profile. The default stops after 25 failures; `--max-failures 0` never
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
export, build both page profiles there, and regenerate:

```sh
python3 -B tools/arm64-tests/export-unit-corpus.py \
  --build-dir .build/presets/macos-unit-16k --output .build/a64-corpus/16k \
  --include tests/unit/arm64/corpus/forced-cases.tsv
python3 -B tools/arm64-tests/export-unit-corpus.py \
  --build-dir .build/presets/macos-unit-4k --output .build/a64-corpus/4k \
  --include tests/unit/arm64/corpus/forced-cases.tsv
python3 -B tools/arm64-tests/build-unit-corpus.py \
  --export-16k .build/a64-corpus/16k --export-4k .build/a64-corpus/4k \
  --include tests/unit/arm64/corpus/forced-cases.tsv \
  --producer-commit "$(git rev-parse HEAD)"
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
encoding match the pack. `--dump PACK --text FILE` writes a reviewable JSON
lines form, and `--repack FILE` rebuilds the pack and metadata from it.

## LSE2 alignment cases

The producer checks predate the FEAT_LSE2 alignment rule (see
[`docs/architecture.md`](../../../../docs/architecture.md)) and expect an
alignment fault from a misaligned LSE atomic, exclusive, or LSFE access inside
one 16-byte block, and the producer harness's interpreter lane does not
implement the rule. [`tools/arm64-tests/lse2-alignment-corpus.py`](../../../../tools/arm64-tests/lse2-alignment-corpus.py)
therefore rewrites a dump after export: it drops those cases and appends the
hand-authored `arm64-lse2-alignment` records, whose completing and
block-crossing cases follow the architecture and match native LSE2 hardware.
`provenance.json` records its hash and counts under `lse2_alignment`. Run it on
a `--dump` of every regenerated pack and `--repack` the result:

```sh
python3 -B tools/arm64-tests/build-unit-corpus.py \
  --dump tests/unit/arm64/corpus/a64_instruction_corpus.pack --text corpus.jsonl
python3 -B tools/arm64-tests/lse2-alignment-corpus.py corpus.jsonl lse2.jsonl
python3 -B tools/arm64-tests/build-unit-corpus.py --repack lse2.jsonl \
  --output tests/unit/arm64/corpus
```
