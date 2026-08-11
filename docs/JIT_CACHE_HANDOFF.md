# JIT translation cache: handover

Written 2026-08-12. Covers the JIT block cache, code arena and cross-executor
translation sharing: what is accepted, what was measured and rejected, and what
is open. Read it before changing anything under
`darwin/cpu/arm64/jit/jit_cache*`, `jit_arena*` or `jit_shared_translations*`.

This is a forward-looking summary. The authoritative records are
`docs/HOT_PATH_EXPERIMENTS.md` (entries G127 to G134) for measurement detail,
and `AGENTS.md` for the invariants that must survive. Where they disagree with
this document, they win.

## Accepted state

`main` at `f5786b9`, with these commits in order:

| commit | what |
| --- | --- |
| `f97af21` | lock-free scan of published JIT store state (B-3, G131) |
| `29b8291` | `UML_JIT_PROFILE_OVERLAP`, cross-thread duplication counter |
| `ff285cb` | block cache and arena sized to the guest working set (G132) |
| `7b66207` | translations shared between executors (G133) |
| `f5786b9` | cache geometry derived from a budget (G134) |

Geometry is derived per executor in
`darwin/cpu/arm64/jit/jit_cache_geometry.cpp` from a whole-process budget that
defaults to half of guest memory, read from the guest's own `mem=` argument and
overridable with `--jit-cache-budget=`. Current shares: 30% lookup structures,
45% arena, 8% tracked stores, set count rounded down to a power of two and
clamped to [1024, 131072], link tables at one record per cached block.

For an 8 GiB guest at 10 vCPUs that is 65536 sets, a 184 MiB arena, about
352 MiB per executor, 3.44 GiB total. A 4 GiB guest gets 1.72 GiB.

Translation sharing is enabled. Each entry is keyed by guest PC plus the exact
guest instruction words, so it is a pure function of its input and never goes
stale; the shared cache is bounded at 2^18 entries across 64 shards and only
publishes when two or more executors are live.

### Where the numbers stand

Multi-core Clang then Object Detection, JIT, 10 vCPUs, 8 GiB guest, Geekbench
6.7.1 workload scores:

| | Clang | Object Detection | peak RSS |
| --- | ---: | ---: | ---: |
| before this work (8192 sets, 64 MiB arena) | 432 | 433 | 4682 MiB |
| G132 peak (131072 sets, 512 MiB arena) | 794 | 1383 | 8229 MiB |
| **accepted (budget derived)** | **695** | **1205** | **7569 MiB** |

The accepted configuration is deliberately below the G132 peak: it trades about
8% for a memory bound that follows the guest instead of ignoring it.

## The finding everything else rests on

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
against a 283,353 block working set. G132 addressed that and cut total
translations from 109 million to under 4.2 million.

Anyone re-opening this work should re-run that measurement first. It is cheap
and it is what tells you whether you are attacking capacity, duplication or
something else.

## Rejected, with reasons

Do not repeat these without new evidence.

- **Enlarging the arena alone.** At 8192 sets, growing the arena from 64 MiB to
  256 MiB made Clang *worse* (432 to 311). Arena exhaustion calls
  `clear_cache()`, so a large arena behind a small cache only holds more dead
  code. The two are one resource and must move together.
- **Arena beyond 512 MiB.** 1 GiB measured no better than 512 MiB, and peak RSS
  is identical from 256 MiB upward because the mapping faults in on use.
- **Segmented arena (first attempt), FEX-style growth, page-granular
  invalidation.** All three attacked invalidation while the real constraint was
  capacity. Page-granular invalidation worked mechanically, cutting discarded
  blocks 64%, and still moved no score.
- **Publishing every translation to the shared cache.** A single-executor
  process copies millions of mostly-unique translations for a reader that never
  arrives; this turned a 23-second instruction-evidence test into more than 20
  minutes. Publication is now gated on two or more live executors, and lookups
  stay unconditional because an empty cache makes them a hash probe.
- **One mutex for the shared cache.** Convoyed ten executors on every miss.
  Sharding 64 ways by guest PC recovered about a third of the loss; having the
  arena apply relocations as it stores removed a second full copy.

## Open threads, in the order I would take them

### 1. The segmented arena regression, unresolved

Implemented, measured, reverted. It is the highest-value open item because it
is what would let the private tier shrink.

The design: split `ExecutableArena` into segments with per-segment epochs, give
`CompiledBlock` an `arena_slot` checked in `find()`, touch the segment on every
cache hit for true LRU by use, and reclaim one segment instead of calling
`clear_cache()`. Link records must survive reclamation because code in
surviving segments embeds their addresses, so only deactivation is safe.

All 40 JIT tests passed, including a rewritten
`arena_reset_discards_saved_validation_words` pinning the new contract. Then:

| config | Clang | Object Detection | peak RSS |
| --- | ---: | ---: | ---: |
| baseline | 695 | 1205 | 7569 MiB |
| 8 segments | 511, 530 | 280, 287 | 1267 MiB |
| 2 segments | 550, 550 | 288, 282 | 1267 MiB |

Three facts that should drive the next attempt:

- **8 and 2 segments are indistinguishable**, so reclaim granularity is not the
  variable. That kills the obvious hypothesis, that a full link-deactivation
  sweep per reclaim was running eight times more often.
- **Peak RSS is exactly 1267 MiB in all four runs** across two configurations.
  That is a hard limit, not a tuning curve.
- **Object Detection alone was healthy** under the same build: 28,672 blocks,
  80 evictions, 2 reclaims, no thrashing. The pathology is specific to the
  Clang-then-Object-Detection sequence.

The thread to pull: the identical RSS suggests the guest itself stopped
allocating, which would mean Geekbench completed far less work and the low
memory is a symptom rather than the win it appears to be. **Confirm the
workload actually ran to completion before trusting any of those numbers.**
Then profile the `201,301` sequence with `UML_JIT_PROFILE_COMPILE=1` against
the same sequence on baseline.

### 2. Share the saved guest words

`saved_guest_words_` is 128 MiB per executor of verbatim guest instruction
bytes, byte-identical in every executor, and since G133 stored a third time
inside each shared translation entry. Replacing the 256-byte inline slot in the
block cache with a pointer to the shared entry collapses it to about 1 MiB per
executor.

The complication is blocks with no shared entry: single-executor processes
never publish, and entries can be refused at the cap. Those need a fallback
store, or the block simply cannot use the stale-validation path.

### 3. Share the arena

512 MiB per executor, 56% of the private footprint, and per-executor only
because generated code embeds absolute link pointers. Two routes:

- **Link table base in a register.** Makes code fully executor-independent, so
  one shared executable arena. Costs a reserved GPR on a JIT that already has
  register-pressure work, an indirection on the hottest path, and it
  reintroduces cross-thread link *activation*, which is the genuinely dangerous
  part: the shared `entries[]` and epoch protocol.
- **Keep per-executor materialization** as G133 does and accept that the arena
  stays private. This is the current design.

Roughly 70% of a private budget is shareable in principle. The block cache
genuinely resists sharing: 72 of its 104 bytes per entry are executor-specific
pointers and bookkeeping.

### 4. Finish the full Geekbench A/B for sharing

The two-workload ABBA said sharing is free on Clang (+0.2%) and costs 5.9% on
Object Detection, with 262,144 translations published and 1.3 million reused at
an 81% reuse rate. A full Geekbench run was started and stopped after 3 of 8
runs, and it pointed the *other* way:

| | single core | multi core |
| --- | ---: | ---: |
| shared (n=1) | 327 | 1291 |
| control (n=2) | 319, 321 | 1247, 1267 |

Suggestive that sharing pays across a broader workload mix than Clang plus
Object Detection, but n=1 on one arm proves nothing. Rerun it against the
current geometry. `share_translations_between_executors` in
`jit_runtime_contract.hpp` selects the arms.

### 5. Untouched items from the original plan

C-1, C-3, G-1, G-3, the G-4 implementation (destination-only vector residency
plus its flush protocol, sized at 6-26% in G130), and the Object Remover
regression noted in G126.

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
`macos-integration`. All three counters are opt-in and compiled out of ordinary
production objects:

| variable | reports |
| --- | --- |
| `UML_JIT_PROFILE_COMPILE=1` | blocks, guest instructions, evictions, arena resets, validation outcomes, block lengths |
| `UML_JIT_PROFILE_OVERLAP=1` | distinct guest PCs per thread, their union, the duplication factor and a shared-by histogram |
| `UML_JIT_PROFILE_SHARED_TRANSLATIONS=1` | shared cache entries, publications, reuses, misses, refusals |

A representative run:

```sh
UML_JIT_PROFILE_COMPILE=1 tools/rootfs/with-verified-rootfs.sh \
  --rootfs rootfs.erofs -- \
  python3 tools/guest-shell.py \
    --emulator .build/presets/macos-jitprof/bin/uml-darwin \
    --rootfs rootfs.erofs --engine jit --cpus 10 --memory 8G --timeout 3600 \
    -c "cd /tmp && /opt/geekbench/release/geekbench6 \
        --no-upload --section 2 --workload 201,301 2>&1 | grep -E '/sec'"
```

Sample peak RSS by polling `ps -o rss=` against `uml-darwin --execution` while
the run is alive; the emulator does not report it.

## Constraints that must survive

From `AGENTS.md`, and each of these has already caught a mistake in this work:

- **G132/G134**: the cache geometry is budget-derived and pinned by
  `the_cache_holds_more_than_twice_the_former_capacity`. Every per-executor
  structure must stay inside the budget, because they multiply by vCPU count;
  fixed sizes previously reached 9 GiB at ten vCPUs.
- **G133**: every pointer a translation embeds into its compiler's link tables
  must be recorded as a `JitRelocation`, and only
  `ExecutableArena::store_relocated` may rewrite them. Miss one and a reusing
  executor branches through another executor's freed records. The pinning test
  destroys the compiling executor first, so a miss is a sanitizer failure
  rather than silent corruption — which is why **ASan is the gate that matters
  most here**, not the unit pass count.
- Only `jit_arena.cpp` owns executable allocation, RW-to-RX publication, JIT
  write protection and instruction-cache publication.
- No profiling control, including a bare `getenv`, may appear in an ordinary
  production engine object. `engine-object-boundary-audit` enforces it.
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
