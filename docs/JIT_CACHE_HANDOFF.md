# JIT translation cache: handover

Written 2026-08-12. Covers the JIT block cache, code arena and cross-executor
translation sharing: what is accepted, what was measured and rejected, and what
is open. Read it before changing anything under
`darwin/cpu/arm64/jit/jit_cache*`, `jit_arena*` or `jit_shared_translations*`.

This is a forward-looking summary. Entries G127 to G138 in
`docs/HOT_PATH_EXPERIMENTS.md` own the dated predecessor measurements;
`AGENTS.md` owns the invariants that must survive. Do not use a dated score to
override the current contracts below.

## Lean experiment workflow

Git is the source authority. Build experiments directly from the current
checkout in separate build directories and record the revision, working-tree
state, configure options, command, raw measurements, and result. Do not copy or
seal source trees, hash every source/object/product, relink solely to force
binary equality, or require object/archive isolation. Compiler-generated
timestamps, versions, UUIDs, build IDs, and tool identities are ordinary build
metadata and may vary.

For an A/B, change the intended selector in otherwise comparable Release build
directories and use a same-session counterbalanced measurement. Functional
tests prove correctness; the benchmark and mechanism counters establish the
performance claim. The EROFS root image is mounted directly and is not hashed
before and after each run. Keep the useful outputs small: commands, raw score
or counter lines, exit/shutdown status, and a concise summary.

## Accepted state

The dated G133--G135 sequence ended at `f5786b9`. That commit is a predecessor,
not the identity of the current source:

| commit | what |
| --- | --- |
| `f97af21` | lock-free scan of published JIT store state (G131, G132) |
| `29b8291` | `UML_JIT_PROFILE_OVERLAP`, cross-thread duplication counter |
| `ff285cb` | block cache and arena sized to the guest working set (G133) |
| `7b66207` | translations shared between executors (G134) |
| `f5786b9` | cache geometry derived from a budget (G135) |

As of 2026-08-12, the source after that sequence is the current post-baseline
checkpoint. This section claims only gates that completed; the mechanisms below
are the contracts of that checkpoint.

### Fixed private working sets and a growing shared tier

`darwin/cpu/arm64/jit/jit_cache_geometry.cpp` publishes the whole-process
budget, expected executor count, and private per-executor ceiling as one
indivisible first-valid configuration. The total defaults to half the guest's
parsed `mem=` value and `--jit-cache-budget=` overrides it. Each executor gets
at most 64 MiB of complete private retained state; the host-only
`--jit-executor-cache-budget=` option may lower but not raise that ceiling.
After all private working sets are funded, every additional budget byte goes to
the process-global shared tier, so only that tier grows with guest memory.

`JitCacheGeometry::total_bytes()` charges eager executor/TLB state,
block-cache backing, the complete fallback validation-word slab, the direct
reverse index, fixed indirect-link storage, retained relocation scratch, and
page-rounded ordinary-code, tracked-store, and floating-transaction arenas.
If the minimum shared reservation and every executor's accounted minimum do
not fit, startup rejects the configuration. For a small total budget, private
geometry may fall below 64 MiB so the minimum shared tier remains available.

The first arena block that names a direct target owns one compact 64-byte
activation anchor aligned immediately after its code. A fixed reverse index
maps target PC to that arena-owned anchor; every later source reuses it instead
of creating another mutable copy. Code and anchors therefore retire together
on an arena reset, and the remaining private budget is assigned to that
combined arena because an arena reset also clears the block cache. Indirect PIC
records remain fixed-capacity stable storage. An index or table refusal retains
the semantic unlinked path and is reported; it does not allocate outside the
private ceiling.

### Shared translations and validation words

Ordinary builds enable sharing with the CMake option
`UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS`; the internal constexpr
`share_translations_between_executors` reflects that option. A cache key binds
the guest PC, exact consumed guest-word prefix, and the complete code-generation
profile. The production cache is bounded by its whole-process accounted-byte
share; that byte bound also limits entry population. The fixed directory is
small, while each shard's mutex/map backing is allocated only on its first
insertion. The byte charge covers conservative map/index ownership,
translation metadata, emitted words, relocations, and immutable guest
validation words. A candidate not yet admitted is transient compiler scratch,
not retained cache state. After local compilation and before constructing that
candidate, a nonallocating preflight first returns an exact canonical owner
that raced the compile, then rejects an overflow-safe lower-bound charge that
cannot fit the remaining byte budget. `insert()` still owns the authoritative
race and actual-capacity checks. A
`std::bad_alloc` anywhere in opportunistic candidate construction or insertion
returns to the existing private validation-word installation; unexpected
exceptions retain their fail-closed behavior. Failed cache insertion is counted
separately. `failed-insertion-retained-accounted-bytes` is the conservative
budget reservation retained after a successful map insertion followed by
entry-vector failure; it is not a measurement of live heap bytes.

Admission of a new canonical owner remains gated on two or more live executors.
Preflight and final insertion resolve an exact owner before applying that gate,
so a compilation that overlapped a peer may retain canonical immutable
validation ownership after the peer retires; an unoccupied identity remains
gated. A lookup `match` means only that the exact canonical entry was found.
Canonical words contain zero in every executor-pointer relocation cell, and
relocations carry only neutral guest keys. A `reuse` is counted later, after
the consumer validates every metadata field, copies the code through
`ExecutableArena::store_block()`, reuses or creates its small arena-adjacent
direct target anchors, resolves indirect sites, and installs the block. Mutable
activation state is cheap installation state and is never retained in the
shared cache.
Invalid metadata, indirect-link capacity, and private-arena exhaustion remain
materialization failures; each falls back to local compilation and cannot
masquerade as saved work. A local direct reverse-index refusal merely leaves
that source cold and does not change the canonical code shape. The GPR-link
profiling build remains executor-private because its emitted counter pointers
are deliberately outside the production relocation ABI.

`CompiledBlock` no longer owns an inline copy of every saved guest-word slot.
An admitted shared translation owns one immutable exact prefix and each
materialized block aliases it. A single-executor compilation, gated or refused
publication, and every other noncanonical result instead aliases its executor's
bounded fallback slab. The conservative geometry still charges that complete
slab, so the accounting proof does not depend on publication success, while
exact stale-block validation retains the same bytes and fault ordering.

The 2026-08-12 same-session `A B B A A B B A` saved-word campaign measured
candidate/control medians of -0.34% on Clang, -2.16% on Object Detection, and
+0.62% maximum RSS. Both score deltas are inside the campaign's 2.8% noise band,
the collapsed second candidate run was retained rather than discarded, and the
RSS moved in the wrong direction for a memory-win claim. The ownership change
is accepted for the single immutable source, bounded fallback, and accounting
contract; this campaign does **not** establish a throughput or RSS improvement.

### 64 MiB diagnosis and compact direct footer

The 2026-08-12 Clang/Object Detection investigation separates the four proposed
causes of the small-cache regression:

- executor migration is not the cause: the accepted pool trace returned the
  remembered executor on 99.997117% of P attempts, and its same-sequence slot
  changes were only 0.002883% of all borrows;
- a one-executor diagnostic still churned its private cache and arena, so the
  regression did not require migration or cross-executor sharing;
- shared installation is not slower than compilation: the compact precursor's
  profiling run measured 2,424.81 ns per complete shared
  lookup/materialization/install path versus 16,322.84 ns per complete local
  compile/publication/install path, making reuse 6.73 times faster (85.14% less
  time);
- 64 MiB was not intrinsically too small. The failure was the amount and
  placement of per-block direct-link state, which forced whole-arena resets and
  repeated materialization.

The first footer prototype put ten activation pointers in every 128-byte
record. Its ordinary Clang/Object Detection/summary scores were 332/975/569,
with 16,938 arena resets and 489,657,430 shared materializations in the
instrumented run. The retained compact anchor uses one internal entry plus a
ten-bit selector mask in 64 bytes. It is created after the first code block
that names a target PC and reused by every later source. At the same 64 MiB
private ceiling, the ordinary one-observation scores were 542/1139/786 versus
460/856/628 before the footer work (+17.83%/+33.06%/+25.16%). The instrumented
run recorded 3,869 resets and 183,428,416 materializations, reductions of
77.16% and 62.54%, with zero direct-capacity materialization failures. These
scores exposed a remaining activation problem: every source still received a
mutable record even when many sources named the same target. The arena now
creates one anchor for each distinct target PC and patches all source literals
to that anchor. A representative native regression fell from 688 seconds to
171 seconds after removing reset-time scans and then to 6.66 seconds after
deduplicating the anchors. The final ordinary observation scored 613/1238/871,
another +13.10%/+8.69%/+10.81% over 542/1139/786 and
+33.26%/+44.63%/+38.69% over the initial 64 MiB result. These remain focused
same-host observations, not a multi-run general benchmark.

The requested final live sample was taken before shutdown. Resident memory
peaked at 6.932 GiB during the final ordinary observation. Its collapsed
top-of-stack report contains no activation or deactivation routine; only 48
recursive samples reached the now-constant-time `activate_link`, while lookup,
generated execution, shared validation, and code publication remained visible.
The earlier instrumented sample reported 2.8 GiB physical footprint. At that
profiling shutdown, the lazy shared cache held 2,047,701 canonical entries and
accounted
3,285,847,540 of its 3,624,031,696-byte budget. Thus the high footprint was not
an eager allocation of the fixed limit: this workload actually populated most
of the large shared tier, in addition to ten bounded private working sets.
Mutable activation state is cheap, regenerated privately, and absent from the
canonical cache; the final sample does not justify another shared
representation or hot-path indirection for it.

### Bounded executor pool and memory preparation

A managed emulator run owns one pool whose capacity is the configured guest CPU
count. Lightweight task-carrier wrappers borrow an executor exclusively for a
guest entry, remember a preferred slot only as affinity, and yield to an already
queued FIFO borrower. An extra simultaneous borrower waits; it neither creates
an unbudgeted executor nor turns ordinary task fan-out into lease exhaustion.
The first wrapper binds the exact code-generation configuration for the
session. Session tokens cannot name a later run, and the pool drains before its
runtime memory domain retires. Direct diagnostic or embedding uses outside a
managed session still take the same bounded executor lease. Once the budget
registry reserves a managed run, standalone admission stays closed through
final teardown; the pool alone uses a private tagged lease path while that
session is active and not release-pending.

Idle ownership now uses one 128-byte-aligned atomic session-token cell per slot
rather than one process-global free mask. A preferred-slot CAS of the requested
session token to zero remains the first attempt and the generic fallback remains
lowest-slot-first; after a successful acquire CAS, the stable slot owner supplies
the executor pointer. Release publishes its session token and drain exchanges
each created cell to zero separately. The inactive release path revalidates the
slot session and exact owner under the pool mutex before reclaiming only its own
token. An old session therefore cannot claim a replacement executor, including
under allocator pointer reuse, and a successor that already claimed a cell
cannot be destroyed by its predecessor's delayed return. Activation fails once
the monotonic token reaches its maximum rather than wrapping. The FIFO waiter
count, queue, and sequentially consistent publication/predicate pair remain the
admission and lost-wakeup authority.

Pooling deliberately lets an executor move among guest CPU states,
generations, and task-carrier `StaticGuestMemory` objects inside one domain.
Therefore each execution interval clears the remembered prepared-memory
pointer, and the first block for each distinct memory object calls
`memory.prepare()` even when generation and mapping-change values match the
executor's previous borrower. The executor's own TLB is prepared only when the
mapping sequence changes. Do not merge those two conditions: doing so can run
through a fresh carrier's unprepared memory merely because its sequence tuple
matches the prior carrier's.

The first focused executor-pool A/B is retained at
`.build/results/jit-cache-handoff/executor-pool-perf-20260812/README.md`. Its
prior-semantics control gave each task wrapper a standalone executor while
bypassing only the new lifetime lease ceiling, so geometry, code generation,
shared-cache policy, probe, and measurement logic stayed matched. Pooling was a
clear P-core win: 21.314 versus 25.619 seconds (-16.80%) and 159.105 versus
184.700 seconds host CPU (-13.86%), with nonoverlapping raw ranges and both
counterbalanced halves agreeing. It was not accepted because the E-core primary
median regressed from 17.590 to 18.489 seconds (+5.11%) and host CPU rose 18.08%.
The E-core raw ranges overlap and its halves reverse direction, so the magnitude
is noisy but still crosses the 2% review threshold.

No stack or contention profile was captured, so the old global free-mask RMW is
a mechanism hypothesis, not an observed attribution.

The corrected cacheline-separated exact-session-token repeat is retained at
`.build/results/jit-cache-handoff/executor-pool-exact-token-measurement-perf-20260812/README.md`.
Its isolated `standalone-unlimited-lease` control differs only in the pool and
geometry objects, selects the standalone adapter path, and bypasses managed-
session lease exclusion plus the live-executor ceiling while preserving the
same geometry, code generation, shared-cache policy, and probe. Under the same
P10/9-worker and E4/3-worker fixed counterbalanced order, the primary median of
invocation medians was:

| host class | exact-token pool wall | standalone wall | pool wall delta | exact-token pool host CPU | standalone host CPU | pool CPU delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| performance | 22.7025 s | 22.2960 s | **+1.82%** | 167.400 s | 164.130 s | **+1.99%** |
| efficiency | 17.4315 s | 14.9345 s | **+16.72%** | 32.650 s | 25.675 s | **+27.17%** |

It is also rejected as a performance optimization: the earlier P benefit was
not retained and the E wall loss is outside the 2% limit. Invocation-median
ranges overlap in both classes. P halves move +4.25% and +0.32% wall; E halves
reverse at -7.92% and +16.93%, with corresponding host-CPU changes of +0.25%
and +32.16% on E. The recorded host was not idle: a `ReportCrash agent` used
about one P core throughout, with variable `spotlightknowledged`, WindowServer,
and application activity. No sample was dropped or retried. Those limitations
do not change the failed acceptance result, but they prevent a structural
attribution for the noisy E magnitude and do not confirm the global-mask
hypothesis.

The pool itself remains required: it enforces the guest-CPU executor cap that
makes the whole-process geometry true. Do not restore unbounded wrapper-owned
executors merely because neither pool layout established a speedup.

The prerequisite mechanism trace is now complete at
`.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md`.
One unscored P10/9-worker run recorded 21 wrappers and 18,002,647 borrows;
18,002,107 of 18,002,626 preferred attempts returned the remembered slot
(99.9971%), with 530 fallbacks, 18,004,098 cell probes (1.000081 per borrow),
1,461 failed CASes, no waiters, ten one-time constructions, peak live/borrowed
10/9, and no standalone construction. One unscored E4/3-worker run recorded 15
wrappers and 6,002,142 borrows; 6,002,116 of 6,002,127 preferred attempts
returned the remembered slot (99.9998%), with 22 fallbacks, 6,002,173 probes
(1.000005 per borrow), 35 failed CASes, no waiters, four one-time
constructions, peak live/borrowed 4/3, and no standalone construction. Neither
run overflowed its bounded association sets.

Those direct counters exclude frequent multi-cell acquisition scanning, CAS
retry, waiter traffic, and reconstruction as explanations for the rejected
A/B. They do not identify another cause, and the profiling-build timings are
not score evidence. `preferred-hits` means returned remembered slot, not
first-CAS success.

Sequence affinity is closed without a code arm at this checkpoint. Linux
returns `&guest_ipi_sequences[cpu]`, so the traced pointer identifies a vCPU's
interrupt source rather than a task, address space, or executor-cache key.
Local JIT block reuse is indexed by generation and PC and matches generation,
code-change sequence, and PC; TLB and link preparation follow generation plus
the mapping and code-change sequences. The current adapter instead remembers
one executor slot per task-carrier wrapper. It returned that slot on
18,002,107 of 18,002,626 P attempts (99.997117%) and 6,002,116 of 6,002,127 E
attempts (99.999817%). The exact 519/11 shortfalls were same-sequence slot
changes: only 0.002883% of all P borrows and 0.000183% of all E borrows.

The trace still preserves the complete lifetime fanout observation: P reported
5 shared and 10 multislot sequences, 64 extra slot associations, and maximum
slot fanout 9; E reported 4 shared and 4 multislot sequences, 8 extra slot
associations, and maximum fanout 4. Those fields OR each sequence's lifetime
slot set, however, so one rare startup or fallback association adds a slot
without measuring borrow-weighted churn or an executor-cache miss. A
vCPU-to-slot table would still pay every per-entry session check, claim CAS,
and release publication that distinguishes the pool from standalone, and it
could displace wrapper-correlated address-space and PC locality. The current
evidence therefore does not justify added shared hot-path state.

Reopen sequence affinity only if a profiling-only, zero-overflow trace measures
borrow-weighted global same-sequence slot transitions across wrappers and
correlates them with executor-local block misses or recompilation, validation
failures, or link/TLB reprepare work sufficient to explain at least a 2%
host-CPU or wall-time effect; a symbolized scheduling profile independently
placing at least 2% of carrier CPU in those post-handoff cold paths also
qualifies. Lifetime fanout alone does not. A later candidate must first use the
current exact-token wrapper-affinity pool as its matched causal control; a
fresh standalone arm is additionally required before claiming recovery of the
pool tax, and score runs must disable the trace.

The initial pointer-cell follow-up passed both pool tests in the ordinary
16 KiB and 4 KiB profiles, including `architecture-audit` in each build, and
passed the same 2/2 selection under ASan/UBSan and TSan. A later adversarial
source audit rejected it: a borrower paused after its old-token check could
claim a newly restarted session's untagged pointer cell. The exact-session-token
repair and managed-activation lease exclusion now pass cache geometry plus both
pool fixtures in the ordinary 16 KiB and 4 KiB profiles and under ASan/UBSan and
TSan. Both ordinary profiles also pass `architecture-audit`; diagnostic pause
hooks remain confined to diagnostic source targets. These correctness and
structure gates are not throughput evidence.

### Historical G135 numbers

Multi-core Clang then Object Detection, JIT, 10 vCPUs, 8 GiB guest, Geekbench
6.7.1 workload scores:

| | Clang | Object Detection | peak RSS |
| --- | ---: | ---: | ---: |
| before this work (8192 sets, 64 MiB arena) | 432 | 433 | 4682 MiB |
| G133 peak (131072 sets, 512 MiB arena) | 794 | 1383 | 8229 MiB |
| **G135 budget-derived arm** | **695** | **1205** | **7569 MiB** |

These observations predate the current shared-cache carve, complete retained
storage accounting, and executor pool. They show why G135 accepted a bounded
geometry below the G133 peak, but they are not performance or RSS evidence for
the current source. A matched current-tree campaign remains required.

## Historical G133/G134 motivation

`UML_JIT_PROFILE_OVERLAP=1` over the Clang plus Object Detection campaign:

| | |
| --- | ---: |
| total block translations | 109,056,465 |
| sum of per-thread distinct PCs | 2,095,646 |
| **union of distinct PCs** | **283,353** |
| PCs translated by exactly 10 threads | 197,859 (70%) |

**385 translations per distinct block**, factoring into **52x within-thread**
retranslation and **7.4x cross-thread** duplication. The within-thread factor
was seven times larger and was pure capacity: the cache held 32,768 entries
against a 283,353 block working set. G133 addressed that and cut total
translations from 109 million to under 4.2 million.

This is predecessor evidence, not a counter report for the current executor
pool or geometry. Any new capacity or duplication claim needs a current-tree
run that proves which mechanism moved.

## Rejected, with reasons

Do not repeat these without new evidence.

- **Enlarging the arena alone.** At 8192 sets, growing the arena from 64 MiB to
  256 MiB made Clang *worse* (432 to 311). Arena exhaustion calls
  `clear_cache()`, so a large arena behind a small cache only holds more dead
  code. The two are one resource and must move together.
- **Arena beyond 512 MiB.** 1 GiB measured no better than 512 MiB, and peak RSS
  is identical from 256 MiB upward because the mapping faults in on use.
- **FEX-style arena growth and page-granular invalidation.** Both attacked
  invalidation while the measured constraint was capacity. The historical
  page-granular prototype reported fewer discarded blocks without moving the
  score. The older G87/G88 page-epoch metadata was also removed in favor of
  exact saved-word validation; there is no preserved raw artifact that turns it
  into a current acceptance result.
- **Segmented arena, pending entirely new evidence.** The old trial produced
  the observations below, but its harness did not establish that every workload
  completed the intended work. A fresh private-arena baseline later reproduced
  the range previously called the segmented "regression," so causation is not
  established. Treat the table as an inconclusive historical observation, not
  as evidence that segmentation either fails or saves memory:

  | historical config | Clang | Object Detection | peak RSS |
  | --- | ---: | ---: | ---: |
  | predecessor baseline | 695 | 1205 | 7569 MiB |
  | 8 segments | 511, 530 | 280, 287 | 1267 MiB |
  | 2 segments | 550, 550 | 288, 282 | 1267 MiB |
- **Publishing every translation to the shared cache.** A single-executor
  process copies millions of mostly-unique translations for a reader that never
  arrives; this turned a 23-second instruction-evidence test into more than 20
  minutes. Publication is now gated on two or more live executors, and lookups
  stay unconditional because an empty cache makes them a hash probe.
- **One mutex for the shared cache.** Convoyed ten executors on every miss.
  Sharding 64 ways by guest PC recovered about a third of the loss; having the
  arena apply relocations as it stores removed a second full copy.

## Closed design questions

### Page epochs are not a sharing prerequisite

The local block cache still observes the address-space-wide code sequence and
uses exact saved guest words to revalidate a stale candidate. The process-global
translation cache instead keys directly on PC, consumed bytes, and code-generation
profile. If those bytes match, the emitted translation remains valid; if they do
not, it is a different key. That exact-key design already shares decode and emit
work without page epochs. The historical page-granular trials remain rejected
and are not an unfinished prerequisite for the present sharing contract. A new
page-epoch proposal needs its own current mechanism and score evidence.

### The executable arena remains private (Route B)

The current design deliberately keeps executable storage, link records, and
activation state executor-private. Sharing stops at immutable translation
metadata; each consumer validates it and materializes relocated code into its
own arena. A link-table-base register (Route A) would consume a scarce host GPR,
add work to the linked path, and reopen cross-executor activation lifetime and
ordering. It has not been accepted. The earlier segmentation evidence is too
confounded to justify replacing Route B, so the private arena remains the
current disposition unless a new completed, matched campaign establishes a
different result.

## Pre-publication-follow-up final-profile evidence

The direct-descriptor reporting and bounded-capacity boundary was closed by the
one-shot diagnostic at
`.build/results/jit-cache-handoff/link-capacity-profile-backpressure-fix-20260812/README.md`.
The verified guest completed Clang at 807 and Object Detection at 1242, returned
zero from the workload, rootfs wrapper, `tee`, post-run verifier, and emulator,
and preserved the rootfs digest before and after. Its raw console contains 1,240
complete periodic compile records, two complete periodic shared records,
exactly one complete compile final, exactly one complete shared final, no
malformed record, and no record after the shared final.

The compile final reports direct-link occupancy at its exact 262,144-record
capacity, indirect occupancy 21,168/262,144, 43,477 direct branch-operand
refusals, zero indirect refusals, 33,728 own-target refusals, and 27 primary-
arena resets. The shared final reports 4,354,180 exact matches and 4,352,573
successful installations; its 1,607 materialization failures are completely
explained by 1,597 direct-link-capacity failures and ten private-arena
exhaustions. It also reports 4,832,300 combined shard/byte admission rejections,
with accounted shared storage at 268,434,848 of 268,435,456 bytes. A subsequent
call-flow audit found that this artifact's implementation constructed and copied
a complete transient candidate before each such rejection. The current
preflight closes that source-level churn and allocation-failure boundary. The
later current-source diagnostic below verifies the corrected reporting and
counter boundary, but it is not a retrospective or matched throughput claim.

This is accepted evidence for complete final-report delivery, the predecessor
source's counter semantics and bounded-capacity behavior, and orderly pooled
execution. It predates the canonical-before-live-gate and insertion-failure
accounting follow-ups, so it is not a current-source counter capture. It is one
diagnostic run without a matched control, so it is not throughput evidence and
does not replace either rejected executor-pool A/B or the full
sharing-enabled/disabled Geekbench A/B. The current focused
`check-jitprof` workflow passed 2/2; its fixture covers a held stderr `FILE` lock, partial writes,
`EINTR`, the writer's `EAGAIN`/`EWOULDBLOCK` plus `poll(POLLOUT)` recovery, a
full nonblocking pipe that forces `EAGAIN`, byte-exact suffix resumption,
concurrent long records, and change-detected final phases.

The rejected pre-framing current-source diagnostic at
`.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md`
is preserved as a failed, unscored attempt and was not retried. Its guest,
wrapper, `tee`, emulator, rootfs verification, workloads, three final records,
and terminal order all passed, but physical console line 188 placed exactly 39
unterminated shell continuation prompts before the first otherwise complete
compile record. The emulator's stdout and stderr already shared one PTY slave,
so separating the outer wrapper streams or parsing a suffix would hide rather
than repair the output contract. The current writer instead puts a leading LF
and the record body in the same bounded descriptor write; the deterministic
full-pipe fixture pins that exact framing for both writer APIs after an
unterminated foreign prefix.

The sole post-framing current-source diagnostic is accepted at
`.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md`;
its 57-file manifest SHA-256 is
`6e3de38c0e46f60a661bbf535eeebf6702335e665ac23345337275114c801ff8`.
All guest, wrapper, pipeline, verifier, and emulator statuses are zero. The 39
prompts remain intact on their own line, and all 1,450 physical profile records
start exactly with `Darwin JIT` and end in LF: 1,445 periodic compile, two
periodic shared, and exactly one compile, pool, and shared final in post-stop
order, with nothing after shared. Direct/indirect occupancy is
262,144/262,144 and 21,469/262,144. The current shared final records 5,131,797
matches, 5,127,800 reuses, 5,672,507 admission rejections, zero insertion
failures or retained failure bytes, 3,986 direct-capacity plus 11 arena
failures, and 268,434,608/268,435,456 accounted bytes. Clang 758 and Object
Detection 1257 are diagnostic and unscored; this remains one run without a
matched control.

## Open threads

### 1. Sharing follow-up is mechanism-led, not another long score rerun

The shortened current-checkpoint full Geekbench sequence completed in
`ON/OFF/OFF/ON` order, two observations per arm. ON observations were
SC/MC/RSS 186/976/11,648,794,624 and 341/903/11,121,295,360; OFF observations
were 184/812/11,186,814,976 and 185/977/10,947,624,960. The two-run medians were
ON 263.5/939.5/11,385,044,992 and OFF
184.5/894.5/11,067,219,968. Thus sharing increased median peak tree RSS by
317,825,024 bytes (+2.87%) instead of reducing it. The user's memory exception
does not apply. The large within-arm score movement (ON SC 186→341 and OFF MC
812→977) makes the score deltas descriptive rather than causal.

Do not spend another full campaign merely to refine this rejection. Sharing
remains enabled for its accepted translation-reuse mechanism and bounded
budget contract, not for a demonstrated RSS reduction or a newly established
Geekbench speedup. Reopen performance work only with a concrete mechanism that
can plausibly remove significant retained memory or with a representative
regression that needs diagnosis.

### 2. Default-off hot-path experiments

- **Cross-block register contract.** Two independent GPR-link captures selected
  guest x8/x9, and a correctness prototype now exists behind default-off
  `UML_JIT_CARRY_X8_X9_ACROSS_LINKS`. Its matched microprobe result is recorded
  at `.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`.
  The preregistered result is P PASS/E FAIL/overall FAIL: P raw/normalized wall
  improved 4.443%/5.091%, while E raw improved 2.503% but normalized wall
  regressed 0.616% after a 3.100% control improvement; normalized E CPU instead
  improved 2.025%. The user separately accepted it under a post-hoc cross-core
  policy whose E regression ceiling is twice the measured P gain. Preserve
  both dispositions and the E control, wall/CPU, QoS-not-affinity, and waived
  host-load limitations. The option remains OFF until representative-workload
  or Geekbench evidence supports changing the default. Its value is part of
  the shared cache's code-generation profile, so different arms cannot reuse
  one another's translations.
- **Gadget successor fetch.** The 32-entry add/sub-immediate prototype behind
  default-off `UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST` is now
  focused-accepted. Its matched polymorphic/control wall improvements are
  P 13.723512%/0.516300% (13.207211-point difference-in-differences) and
  E 7.258795%/5.481481% (1.777314 points); polymorphic host CPU improves
  13.888889%/9.683605% on P/E. The prospective dynamic cross-core policy and
  the original stricter screen both pass, although the stronger 2-point
  difference-in-differences preference fails on E and its halves. The option
  remains OFF pending representative-workload or Geekbench confirmation.
  Results are recorded under
  `.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/`;
  This is gadget work, not a JIT-cache or arena dependency, and the G107 arm
  does not alter JIT translation-sharing identity.
- **Rejected gadget micro-optimization class: closed without an E-only
  rerun.** G8, G17, and G36 improved the P-core primary by no more than noise
  and therefore none clears the prospective `P > 2%` gate that makes an E arm
  eligible. G106 later pinned all 9,663 gadget entries while removing 3,968
  instructions and measured the same flat result on both core classes; its
  operative rule is dependency depth, not instruction count. Preserved
  predecessor binaries are not current matched products, G17 is explicitly
  layout-confounded, and no G36 candidate survives, so running those artifacts
  only on E would be archival curiosity rather than acceptance evidence. The
  dated G96 checklist remains history. Reopen a specific transform only by
  reconstructing a current layout-pinned arm and screening P first; reject it
  without an E campaign unless its experiment primary is strictly above 2%.
- **Gadget engine: vector residency is optional future work, not a handoff
  blocker.** G130's reverted JIT diagnostic found 52% of operand accesses in
  v0-v7 and measured a fixed eight-register bank at 6-26% over no residency.
  It is a bound on the prize, not gadget-engine evidence: the gadget engine has
  no resident/dirty vector state or flush protocol. A reopen needs a default-off
  bounded gadget prototype, starting with one nonfaulting accumulate family and
  destination-only v0-v7 variants rather than the 729-form cross-product. It
  must materialize dirty state before every unaware access, helper, terminal,
  fault, interrupt, link, or block boundary; preserve retirement and all
  unrelated gadget positions/phases; and pass focused 16/4 KiB plus sanitizer
  correctness before a matched P-first/E-conditional sensitive/control
  campaign. Apply the prospective policy to control-normalized wall: P must be
  strictly above 2%, and any E regression must be strictly below twice the
  measured P gain. A focused pass still leaves the option OFF pending a
  representative gadget workload.

The historical Object Remover discrepancy is no longer an open defect. A
focused current binary comparison did not reproduce the earlier large loss; it
remains ordinary benchmark variance unless a new matched campaign says
otherwise.

## How to measure this without fooling yourself

Every one of these cost me a wrong conclusion at least once.

- **Use a same-session control.** Over one session this host drifted from 794
  to 753 on Clang and 1383 to 1271 on Object Detection with identical code.
  Measured against a morning baseline the shared cache looked like -10%;
  against a same-session control it was -5.9%.
- **Counterbalance the order.** ABBA, four runs per arm minimum. All-A-then-
  all-B confounds drift with the effect.
- **Prove the path is live.** An entire segmentation experiment once measured
  dead code, because a constructor's member-init list overrode the default
  member initializer and the configured value never took effect. Assert the
  mechanism moved, not just the score.
- **Instrument the mechanism, not the score.** Individual multi-core workloads
  swing far more than the aggregate. Object Detection alone spans 1150 to 1390
  on identical code. If an effect is under about 10% the score cannot resolve
  it; count the thing you changed instead.
- **Do not pass `--quiet` to `tools/guest-shell.py`** when you want profiling
  output: it swallows the emulator's stderr, which is where every counter goes.
- **Geekbench runs workloads in ID order** regardless of the order you list
  them, and prints all results at the end, so a workload's position in the
  output is not a temporal marker. To measure "after Clang", use a workload
  with a higher ID than Clang's 201.
- **Benchmark without systemd.** `tools/guest-shell.py` boots in about five
  seconds and starts no services to add noise.
- **Detach long runs.** Harness background shells die with the session process.

### Tooling

Build the profiling emulator with the `macos-jitprof` preset
(`UML_ENABLE_RUNTIME_PROFILING=ON`); the production emulator is
`macos-integration`. All four diagnostics are opt-in and compiled out of
ordinary builds:

| variable | reports |
| --- | --- |
| `UML_JIT_PROFILE_COMPILE=1` | blocks, guest instructions, evictions, own-target and branch-operand link refusals, direct/indirect link-table high-water and capacity, whole-cache arena resets, validation outcomes, block lengths |
| `UML_JIT_PROFILE_OVERLAP=1` | distinct guest PCs per thread, their union, the duplication factor and a shared-by histogram |
| `UML_JIT_PROFILE_SHARED_TRANSLATIONS=1` | entries, publications, exact matches, successful reuses, misses, admission refusals, malformed/degraded rejection, materialization failures, and retained-byte accounting |
| `UML_JIT_PROFILE_EXECUTOR_POOL=1` | unscored wrapper-local borrow/preference, cell probe, waiter/FIFO, managed construction, observed post-claim population, slot movement, session/interrupt-sequence wrapper and slot fanout, overflow, and cold standalone lifetime evidence; never raw keys |

The compile profile's `arena-resets=` is the failed primary-arena store path
that immediately clears the complete block and link caches before resetting the
arena; tracked-store and floating-transaction arena recycling is separate. Its
periodic lines are cumulative for one translating host task thread. A final
compile line combines retired recorders with the unmerged tails of still-live
recorders, sums refusal and reset counts, and takes the process-wide maximum of
each link-table occupancy and capacity. The overlap final uses the same
retired-plus-live rule. In the shared report, `matched=` is an exact cache lookup
and `reused=` is the narrower successful installation count; the difference is
explained by the reported invalid-metadata, direct-link-capacity,
indirect-link-capacity, and arena-exhaustion fields.

Runtime profiling does not write through the shared `stderr` `FILE`. Each
bounded record is formatted in private storage and sent directly to
`STDERR_FILENO`, resuming the exact unwritten suffix after a partial write or
`EINTR`. A nonblocking descriptor that returns `EAGAIN`/`EWOULDBLOCK` waits in
`poll(POLLOUT)` and resumes rather than truncating the record. The same bounded
descriptor write starts with an LF, which terminates unrelated unterminated PTY
text before the record body. Long expansion dumps use separately identifiable
continuation records instead of holding a global output lock. Do not
reintroduce `fprintf(stderr, ...)`,
`fflush(stderr)`, or a `FILE` lock on a guest task-carrier path: another carrier
can retire while the process-wide reporter is collecting its state.

For an embedded kernel, the first explicit reporter runs after the ELF entry
has returned and its task carriers have joined, but before the session-owned
executor pool is destroyed. The macOS app repeats the same idempotent facade
after `uml_emulator_run()` and before its deliberate `std::_Exit()`.
Compile/overlap and shared reporters retain normal-exit callbacks for non-CLI
users. The executor-pool reporter is intentionally explicit-only because its
snapshot requires the quiescent pre-drain boundary. Each reporter is
change-detected: an unchanged explicit repeat is silent, as are later
compile/overlap and shared component callbacks, while later activity permits a
new cumulative final and then becomes silent again. Thus a large synchronous
pool drain cannot erase the only final snapshot, and repeated boundaries do not
manufacture duplicate evidence.

The executor-pool trace is a mechanism diagnostic rather than a performance
probe. Each profiling-build JIT wrapper owns fixed counters and bounded
session/nonnull-sequence/slot-mask associations. Every successful borrow ORs
its claimed slot into the local association. Borrow operations mutate only that
wrapper-local record: they perform no global profiling atomic increment. A
mutex-protected intrusive registry is touched at wrapper construction,
retirement, cold standalone lease construction/destruction, and the quiescent
final. That final combines retired and still-live wrappers, merges associations
by session/sequence, ORs their slot masks, and emits distinct/shared, extra-
wrapper-association, wrapper-fanout, multi-slot, extra-slot-association and
maximum-slot-fanout indicators without either key. The consecutive same-
sequence/different-slot field is supplemental only. The pool reporter is
called before the shared-cache reporter, so its record precedes the shared
record when both changed; change suppression can make either call silent.
`peak-borrowed-observed=` is only the maximum of diagnostic
post-claim scans; concurrent claims and releases make it non-linearizable.
When the environment control is unset, adapters pass null and skip the scan.
Because the profiling graph still adds conditional branches and enabled runs
add those scans, no timing from it is score evidence. All trace source, strings,
state and calls compile out of ordinary builds. Its bounded final may be
larger than `PIPE_BUF`; quiescent emission avoids carrier concurrency and the
direct writer completes partial suffixes, so the record remains one line.

A representative run:

```sh
UML_JIT_PROFILE_COMPILE=1 python3 tools/guest-shell.py \
  --emulator .build/presets/macos-jitprof/bin/uml-darwin \
  --rootfs rootfs.erofs --engine jit --cpus 10 --memory 8G --timeout 3600 \
  -c "cd /tmp && /opt/geekbench/release/geekbench6 \
      --no-upload --section 2 --workload 201,301 2>&1 | grep -E '/sec'"
```

Sample peak RSS by polling `ps -o rss=` against `uml-darwin --execution` while
the run is alive; the emulator does not report it.

## Constraints that must survive

From `AGENTS.md`, and each of these has already caught a mistake in this work:

- **G133/G135**: the cache geometry is budget-derived and pinned by
  `private_cache_retains_its_bounded_working_set`. Every per-executor
  eager or retained structure must stay within the 64 MiB private ceiling,
  because it multiplies by vCPU count; only the shared remainder may grow with
  guest memory. The block cache, reverse direct index, fixed indirect table,
  relocation scratch, fallback saved words, and rounded arenas are part of
  that proof, not incidental allocator use.
- **G134**: every executor-pointer literal is a `JitRelocation`. Canonical
  shared words keep those cells zero and `ExecutableArena::store_block`
  reuses or creates the consumer's adjacent direct target anchors and resolves
  indirect sites. Miss one and a reusing executor can branch through another
  executor's freed state. The pinning test destroys the compiler first, so
  **ASan remains the gate that matters most here**, not the unit pass count. The key's
  code-generation profile and exact consumed bytes, metadata validation,
  degraded-publication rejection, lazy byte-bounded backing, and
  match-versus-reuse distinction are part of the same lifetime contract.
- The executor-pool cap is the guest CPU count, not the number of task wrappers.
  Keep its session/profile binding, FIFO waiter priority, pre-domain drain, and
  prepared-memory identity separate from mapping-sequence TLB preparation.
  Availability cells remain cacheline-separated and carry the exact owning
  session token; claim, release, and drain update only that slot, and inactive
  teardown must not destroy an owner already claimed by a successor.
- Only `jit_arena.cpp` owns executable allocation, RW-to-RX publication, JIT
  write protection and instruction-cache publication.
- Ordinary builds must compile without optional profiling controls, including
  their environment lookups. The profiling preset contract pins that
  selection; profiling behavior is tested in its dedicated build.
- A task-carrier profile record must not take the shared stderr `FILE` lock.
  Preserve direct descriptor writes and the change-detected explicit boundary
  after carrier join but before pool destruction.
- Gates for any change here: unit at 16 KiB and 4 KiB, tooling, ASan/UBSan and
  TSan. Both page profiles, because the geometry is indexed differently.

## Things that are easy to get wrong

- `cache_index()` takes the set count as an argument now. Tests that hardcode a
  mask will silently stop colliding rather than fail;
  `cache_replacement_keeps_validation_metadata_bounded` had exactly that bug and
  now searches for colliding offsets at run time.
- `JitBacking` takes a page count, not a word count.
- `maximum_block_links` used to be `cache_sets * cache_ways * 2`, so raising the
  set count silently raised the link tables with it, at 128 bytes per
  `JitBlockLink`. That coupling caused the 30 GB desktop regression. Link
  capacity is now one record per cached block and should stay decoupled.
- The emulator layer must not include `cpu/` headers. The budget reaches the CPU
  layer through `runtime/execution_engine.hpp`.
