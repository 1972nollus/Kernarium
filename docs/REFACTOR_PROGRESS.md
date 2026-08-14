# Refactor progress

## Kernarium iOS application prototype (2026-08-14)

The `Kernarium` Xcode project embeds opt-in iOS device and Simulator runtime
slices and boots a 1 GiB UML guest from `Documents/rootfs.erofs`. Simulator
uses four vCPUs, while physical iOS matches the device's reported physical
processor count. Its basic SwiftUI `ContentView` owns a safe-area
`MTKView` whose point dimensions are the guest resolution. The real Virtio-GPU
device and virglrenderer use pinned UTM libepoxy plus ANGLE/Metal and present
the native Metal texture directly; the iOS target builds neither AppKit nor
VNC, while the macOS graph remains on its existing
AppKit/CGL/IOSurface/Metal and VNC paths.

Kernarium recreates a sparse 8 GiB temporary image on launch. The guest formats
it as ext4 and uses separate OverlayFS, Docker, and containerd directories on
the same free-space pool; the app does not create or activate disk-backed swap.
Both iOS targets and default 16 KiB macOS integration consume the exact same
canonical `/Volumes/<case-sensitive-volume>/build-16k/vmlinux` ET_DYN bytes. iOS carries
them in the framework's signed `__TEXT`; macOS embeds them through its existing
unsigned CLI path. Both iOS targets use the same scalable 60.875--62 GiB native
window for the 1 GiB guest and every product uses the 128 TiB sparse
software-MMU task range. The native window is guest RAM plus a page-rounded
one-eighth auxiliary allowance with a 32 MiB floor; 1 GiB therefore receives
exactly 128 MiB. macOS exercises the same scalable-window derivation,
relocation, and native-hole handling at a 96 TiB base. It retains the
standalone app's 8 GiB and ten-vCPU defaults, which derive a 9 GiB active
window. The common kernel enables zswap by default with zstd; normal desktop
launchers provide no backing swap, so no zswap pool is created.

Separate Virtio-input touchscreen and keyboard devices now receive UIKit
events, SLiRP supplies guest networking, and the app prevents automatic sleep
only while the guest is active. The keyboard overlay is a round bordered icon
button and uses native circular Liquid Glass on iOS 26. Both iOS dependency
and runtime slices, three combined XCFrameworks, the unsigned Simulator Xcode
app, the focused input unit, and the canonical macOS GPU workflow pass.
Extended Virtual Addressing and file sharing remain project-owned metadata.
Exactly one persistent host thread named `uml-vcpu-N-carrier` owns each vCPU,
including vCPU0; task creation no longer creates pthreads. Linux task contexts
migrate on 128 KiB kernel stacks, host-stop wakes carriers cooperatively, and
IRQ-enabled spin checkpoints service pending IPIs. The shipped scheduler policy
is 100 Hz with `NO_HZ_IDLE`.

A full no-systemd gadgets requalification on the common architecture scored
96/565 single/multi, versus the final pre-change 96/567. Focused Clang/Object
Detection profiles found no material fixed-carrier spin/IPI cost and kept
activation+synchronization below 1.2% of accumulated guest+syscall time. Two
individual SDOT stream-load reductions stopped at 0.00% and 0.39% probe gains
and were removed; no macOS-only recovery path is retained.

The corresponding clean full 4 KiB compatibility run scored 92/417. That is
-4.17%/-26.19% against the current 16 KiB 96/565 and +16.46%/+0.24% against
the historical G35 4 KiB 79/416; the latter is not a strict A/B. A separate
unscored live mapping check placed the executable kernel at 96 TiB and its
8 GiB physical backing in the same high native window, not directly above
64 GiB. The scored process was not profiled or suspended and shut down cleanly.
Final 4 KiB acceptance passed 53/53 tooling checks and 5/5 fast guest cases;
the focused loader sanitizer gate and the 16 KiB 5/5 guest set also passed.

A four-vCPU, multi-core-only follow-up completed all 16 Geekbench workloads
without executing the single-core suite. The current 16 KiB product scored
309 (263 integer / 415 floating point), versus 269 (226 / 373) at 4 KiB:
-12.94% aggregate, -14.07% integer, and -10.12% floating point. The workload
delta ranged from -1.23% to -35.36%; 4 KiB took 17.53% longer with only +0.35%
peak-resident-size movement. Both guests shut down cleanly. G148 records the
exact hashes and the ordered, active-host, single-observation limitation.

Current 1 GiB normal boots pass on Simulator and the connected physical iPad.
The Simulator reached four CPUs, DHCP, EROFS/ext4 state, SDDM, automatic login,
and direct virgl/ANGLE/Metal presentation. The standard-signed Release app ran
without LLDB on the iPad, brought all six reported CPUs online, and followed
the same startup path for more than 90 seconds without the former RCU stall.
The existing device rootfs was preserved. Physical Aquarium is intentionally
not run because the workload is too heavy for the 1 GiB guest; the removed
disk-swap workaround is not part of the product. The earlier 2 GiB Simulator
WebGL 2 Aquarium result remains dated graphics compatibility evidence.

An opt-in physical no-systemd Geekbench launch also reached and completed 13
single-core workloads before the user stopped it during Photo Filter; that
unfinished memory-pressure phase is excluded and no aggregate or multi-core
score is claimed. Eight retained Time Profiler windows put non-carrier work at
no more than 0.040%, with more than 99.6% of active samples on A15 performance
cores and Nominal thermal state in the first and last retained traces. The
dominant costs are existing gadget assembly plus HTML5/Clang cache-link churn;
Object Detection separately exposes cold EROFS zstd decompression in the
native guest kernel. Candidate hot-path changes remain unimplemented pending
focused probes and matched representative A/Bs.

## Post-baseline JIT checkpoint (2026-08-12)

This checkpoint completes the foundational JIT-cache boundary: one authoritative
parsed `ncpus=`/`mem=` budget configuration, a complete 64 MiB private ceiling
per executor, a shared tier that alone receives additional guest-memory budget,
lazy shard backing, complete retained accounting, one compact arena-adjacent
activation anchor per direct target PC, and immutable shared validation words
with neutral relocation cells. Each installer regenerates its mutable
activation state; the canonical cache contains no executor address. Exact-match
and successful-materialization counters remain separate. A managed run
now owns at most its configured guest CPU count of session/profile-bound
executors and drains them before the runtime memory domain retires. The pool's
same-sequence migration regression is fixed by preparing each distinct
task-carrier `StaticGuestMemory` while keeping executor-TLB preparation
conditional on mapping changes.

The focused G138 diagnosis first observed Clang/Object Detection/summary
542/1139/786 at the compact 64 MiB geometry versus 460/856/628 before the
footer change. Deduplicating mutable anchors by target then produced
613/1238/871, +33.26%/+44.63%/+38.69% over the initial result. Profiling of the
compact precursor recorded 3,869 arena resets and 183,428,416 shared
materializations, 77.16%/62.54% below the first oversized-footer attempt.
Complete shared installation averaged 2,424.81 ns versus 16,322.84 ns for the
local compile path (6.73x faster). The final pre-shutdown sample peaked at
6.932 GiB RSS and no longer placed activation/deactivation in its collapsed
top-stack list, while precursor counters showed 3,285,847,540 accounted shared
bytes: the workload populated the lazy tier rather than merely reserving its
limit. A follow-up two-observation floor sweep selected 62 MiB: median
Clang/Object Detection/summary scores were 620.5/1255.5/883 versus
613/1242.5/872.5 at 64 MiB. A 60 MiB arm failed the Object Detection and
combined-score bands. The user retained the 64 MiB production ceiling because
62 MiB saves only 2 MiB per executor and leaves the whole-process budget
unchanged. These scores are focused observations, not broad acceptance.

Final JIT profiles are change-detected at the post-carrier explicit boundary,
which emits compile/overlap, pool, then shared before pool drain, and at the
idempotent app repeat. Compile/overlap and shared also retain normal-exit
callbacks; pool reporting is intentionally explicit-only because it requires
the quiescent pre-drain snapshot. Records go directly to the stderr descriptor
with a same-write leading LF. The writer resumes short or `EINTR` writes and
waits with `poll(POLLOUT)` after `EAGAIN`/`EWOULDBLOCK`; task carriers do not
take the shared stderr `FILE` lock.
`cmake --workflow --preset check-jitprof` passed 2/2 after the leading-LF
framing fix, including a full nonblocking-pipe fixture whose unterminated
foreign prefix is followed byte-exactly by framed records from both writer
APIs, and the 56-target / 50-layered-target / 57-registration architecture
audit.

The first current-source attempt is retained, unmodified and unscored, at
`.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md`.
It was not retried: every status, workload, final record, and terminal-order
gate passed, but exactly 39 shell continuation prompts prefixed the first
otherwise complete compile record on physical line 188, so strict record
integrity failed.

The post-framing current-source diagnostic is accepted at
`.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md`.
All statuses were zero, the 39 prompts remained intact on their own line, and
all 1,450 profile records began exactly with `Darwin JIT` and ended in LF:
1,445 periodic compile, two periodic shared, and exactly one compile, pool, and
shared final, with nothing after shared. Direct occupancy reached
262,144/262,144; shared translation accounting reached
268,434,608/268,435,456 with zero insertion failures and zero retained failure
bytes. Clang 758 and Object Detection 1257 are diagnostic/unscored because this
was one run without a matched control. The earlier backpressure artifact remains
valid predecessor-source evidence but is not the current counter authority.
The saved-word
`A B B A A B B A` campaign likewise supports ownership/accounting rather than
a speed or RSS claim (-0.34% Clang, -2.16% Object Detection, +0.62% maximum
RSS, within the 2.8% score band). The global-mask executor-pool A/B is complete
and rejected. Its corrected exact-session-token repeat is also complete and
rejected: primary invocation-median P wall/host CPU are 22.7025/167.400 seconds
versus 22.2960/164.130 (+1.82%/+1.99%), and E wall/host CPU are
17.4315/32.650 versus 14.9345/25.675 (+16.72%/+27.17%). P halves are
+4.25%/+0.32% wall, while E halves reverse at -7.92%/+16.93%; both class
ranges overlap and recorded host load limits causal precision, so no structural
cause is claimed. The pool remains required for the guest-CPU executor cap and
budget correctness.

The unscored current-source mechanism trace is recorded at
`.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md`.
P/E recorded 18,002,647/6,002,142 borrows, 99.9971%/99.9998% remembered-slot
return, 0.0029%/0.0004% fallback, 1.000081/1.000005 cell probes per borrow,
1,461/35 failed CASes, no waiters, and only the initial 10/4 constructions.
This excludes frequent acquisition scanning, CAS retry, waiter traffic, and
reconstruction as explanations for the rejected A/B. The trace found exactly
10/4 nonnull sequence identities with only 0/2 wrapper sequence changes, yet
every identity used multiple slots and maximum fanout reached 9/4. Those
fanout values are lifetime ORed associations rather than borrow-weighted churn.
Linux's pointer identifies a vCPU interrupt source rather than a task,
address-space, or executor-cache key, and the wrapper-local preference missed
on only 0.002883%/0.000183% of P/E borrows. Sequence affinity is closed without
code. Reopening requires a zero-overflow borrow-weighted cross-wrapper trace
correlated with executor-cache consequences sufficient to explain at least a
2% effect, or a symbolized scheduling profile placing at least 2% of carrier
CPU in the corresponding post-handoff cold paths; lifetime fanout alone does
not qualify. Trace-build timings are not score evidence.

The fixed x8/x9 cross-link carry prototype also completed its missing 4 KiB ON
focused/architecture and 16 KiB ON ASan/UBSan focused gates, then a matched
matched P10/E4 2x2 campaign with symmetric x0/x1 control. P raw/normalized wall
improved 4.443%/5.091% and normalized CPU improved 5.614%. E raw improved
2.503%, but its control improved 3.100%, normalized wall regressed 0.616%, and
normalized CPU improved 2.025%. The original strict disposition remains P
PASS/E FAIL/overall FAIL. The user separately accepts the prototype under an
explicitly post-hoc policy allowing E regression below twice the measured P
gain. The option stays OFF: the E control confound, wall/CPU disagreement,
QoS-not-affinity topology, waived host load, and lack of a representative
workload remain visible. Results are recorded at
`.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`.

G107's bounded 32-entry add/sub-immediate successor-hoist optimization is now
enabled by default. Fresh 16 KiB and 4 KiB Release OFF/ON trees passed its focused
structure/terminal checks plus architecture, and a fresh 16 KiB ON ASan/UBSan
tree passed the same focused tests. The P10/E4 campaign
retained all 96 scored samples. P polymorphic/control wall improved
13.723512%/0.516300% with 13.207211 points difference-in-differences; E improved
7.258795%/5.481481% with 1.777314 points. Polymorphic host CPU improved
13.888889%/9.683605%, all OFF/ON wall ranges are disjoint, and all halves are
beneficial. The prospective dynamic cross-core policy and the independently
preserved strict screen pass, while the stronger preference for at least two
difference-in-differences points fails on E overall and by half. A current
representative check improved full Geekbench Single-Core 94 to 97 and
Multi-Core 564 to 567; targeted Clang/Object Detection improved 438 to 441,
and rerun File Compression/HTML5 outliers stayed within 1.3% of OFF. Results
are under `.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/`
and `.build/results/gadgets-optimization-20260813/`.

G140 closes the bounded G130 gadget-residency route. Four-or-longer nonfaulting
integer MLA runs with one v0-v7 destination and nonaliasing sources keep only
that destination in q24 and flush it before the translated boundary. The
sensitive probe improved 69.6% P and 43.5% E; its out-of-bank control moved
-1.1%/-5.7%. Targeted SIMD Geekbench changed 91 to 92. A clean full run changed
the G107 predecessor's Single-Core/Multi-Core 97/567 to 96/575. One earlier
full attempt was stopped after an RCU stall in Multi-Core Text Processing;
isolated and clean-full reruns completed that workload at 62/63. The bounded
selector is now ON by default; no cross-block or whole-bank state is implied.

The residual gadget bullets do not block this handoff. The historical
G8/G17/G36 E-core rerun is closed without a new measurement because none clears
the prospective `P > 2%` eligibility gate and G106 later measured the same
noncritical instruction-count class flat on P and E.
The broader whole-bank/cross-block vector-residency idea remains closed: G140
implements only the evidence-backed bounded destination-only family.

G141 now keeps decoded gadget programs across a same-generation code-change
notification only after exact page-bounded comparison of all consumed guest
words, with every direct and indirect activation reset before reuse. Its P/E
invalidation probe improved 62.4%/50.6%; targeted multi-core Clang/Object
Detection changed 394/461 to 383/464, and full Geekbench changed from G140's
96/575 to 96/567 (original baseline 94/564). Lazy grow-on-demand saved-word
storage replaced the first fixed-array prototype, and the selector is ON by default.
A live follow-up put decode at 2.36% of configured ten-vCPU capacity, so global
decoded templates are closed without code: their private stream copy and executor-link
relocation would consume the available gain.

The shortened sharing ON/OFF/OFF/ON full-Geekbench sequence found median peak
tree RSS 2.87% higher with sharing and noisy two-run score samples; it rejects
the requested memory exception without establishing a causal score delta. The
current 16 KiB host workflow passes 575/575 units and 47/47 tooling tests. The
4 KiB unit run passed 575/576 tests; its sole failure was the unrelated,
timing-sensitive `arm64-jit-executor-pool-multislot`, which passed immediately
when rerun alone, and the 4 KiB tooling suite passes 47/47. Every gadget test
passed in both page profiles. The Apple package workflow passes all three
slices and its installed, raw, and Xcode consumers. Full sanitizer and
complete integration matrices remain uncredited. The accepted
2026-08-10 baseline below is not relabeled as current-tree evidence.

## Current campaign result (2026-08-10)

The refactor campaign is accepted at implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97`. The final Linux source is the
14-topic replay at tree `5b7b50cd573b7a1077586955cad9cf13770df35d`,
bounded at 155 changed files and 6 outside-port files. The host/kernel bridge is
ABI v6 (`0x6524b65f20239ea1`); the installed public API remains v4
(`0xd9a69804d3303044`). G99, G100, G121, and the G123 forward repair remain in
the accepted implementation. The later unified-EROFS migration supersedes the
baseline's old partitioned-image workflow; it does not relabel the broader
baseline evidence as newly rerun.

Final local acceptance passed 572/572 ordinary 16 KiB units, 573/573 ordinary
4 KiB units, 51/51 tooling tests in each profile, 572/572 ASan+UBSan, 8/8 TSan,
1/1 malformed-loader ASan, and both normal and sanitized GPU gates at 5/5 units
plus 1/1 VNC tooling. Guest `integration-fast` passed 5/5 in each page profile;
the 10-vCPU SMP workload reached 8.19 and 8.03 effective host cores. All three
Apple slices passed package/consumer acceptance and formed the XCFramework; a
real Xcode consumer build succeeded.

A post-baseline macOS display addition introduces `native` and `vnc` provider
modes. `scripts/boot_kde_vnc.py` now selects a native Metal window unless
`--display vnc` is requested. The native Virgl path shares two IOSurfaces
between CGL and Metal, drops superseded ready frames instead of adding a FIFO,
and retains CPU readback only as fallback. This does not reattribute the dated
interactive KDE/VNC campaign as native-window acceptance. Both normal and
ASan+UBSan GPU workflows passed 5/5 units plus the 1/1 VNC smoke, and the
production CLI rebuilt successfully. A focused 800x500,
four-vCPU native run reached `graphical.target`, passed its overlay guard, and
reported direct CGL/IOSurface/Metal presentation before Ctrl-C. Native input
interaction and timed latency remain unmeasured.

For the EROFS migration, the final host workflows passed 572/572 units plus
49/49 tooling at 16 KiB and 573/573 plus 49/49 at 4 KiB. Both rebuilt
production page profiles then passed the regenerated five-test
`integration-fast` paths with raw `rootfs.erofs` at `/dev/ubda`: 5/5 in 65.42
seconds at 16 KiB and 5/5 in 69.33 seconds at 4 KiB. The corresponding
complete workflows passed 52/52 tooling checks in each profile. Their
overlay/systemd tests passed in 34.06 and 38.19 seconds with separate Docker
and containerd tmpfs mounts and an offline source-built hello-world container.
Provisioning, launcher, runner, emulator-option, and API contracts also pass.
EROFS itself supplies the read-only lower-filesystem contract, so mode,
`ubd0r`, and `ro` enforcement are gone. The 16 KiB profile records the upstream
experimental subpage-compression warning because the EROFS image uses 4 KiB
filesystem blocks; the 4 KiB profile does not. That artifact identity remains
historical evidence. Current guest runs use EROFS directly without per-run
hashing or a sidecar lock; only explicit provisioning reads
`tools/rootfs/manifest.json`.

The image now provides `/opt/ltp`; the manifests contain 2,428 verified and 135
disabled cases with no overlap. Exactly 88 cases were disabled because they
require deliberately absent HugeTLBFS, transparent-huge-page, or CAN policy.
Full LTP was not rerun for this migration. The credited final-image evidence is
44 focused gadgets-engine rows, all accepted, including `statx05` at 4/0 in
both profiles and `zram01` at 12/1 with rc 0 in 167.6 seconds. Geekbench uses
its local `/opt` payload with `--no-upload`, but the final scoring run remains
intentionally skipped and no new score is claimed.

This is local-only evidence: no hosted CI workflow exists. Input
materialization remained off for the full refactor acceptance and stays
opt-in; instruction evidence remains **0/983 variant-complete**. Source-size
guidance remains advisory.

[`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) is the authoritative acceptance
record. [`docs/refactor-evidence.md`](refactor-evidence.md) is the detailed
evidence index; [`docs/testing.md`](testing.md) contains current commands
and acceptance boundaries.

## Seventh-audit progress snapshot (historical; superseded)

Status as of 2026-08-04: the seventh independent audit reopened repository-
owned work. Its highest-priority findings were an instruction-evidence oracle
that is reproducible but not independent, a ledger that proves one selected
case per instruction entry rather than all variants, erased instruction-fetch
fault direction, incomplete gadget exception cleanup, unbounded/fallible
memory-domain retirement, GPU shutdown and descriptor-ownership hazards,
incomplete suite build aggregates, and stale current-product evidence. Rootfs
file management remains explicitly deferred
to a later testing redesign. Work needing a special guest image, credentials,
physical devices, public services, interactive graphics, or controlled
performance hardware remains external acceptance rather than an unfinished
source change.
At the time, [`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) was the authoritative
acceptance-level checklist.

[`docs/refactor-evidence.md`](refactor-evidence.md) is the detailed
evidence index. [`REFACTORING_PLAN.md`](REFACTORING_PLAN.md) remains the original
specification and records the clarified policy that generated build metadata
does not need to be byte-for-byte deterministic.

## Seventh-audit open work (historical; superseded)

- Replace the binding-derived allocation/feature generator with an independent
  authoritative form-to-allocation and allocation-to-feature oracle. Model
  dependent and reserved fields, and add whole-pipeline negatives that remain
  invalid after regenerating every test-owned catalog.
- Give every required valid/invalid instruction variant an independent identity
  and expected state, event, fault, and memory result. The current 983 unique
  records each credit only one selected case, so their checkmarks do not prove
  the entry's width/form/alias/state/feature partitions are exhaustive.
- Preserve instruction-fetch fault direction through interpreter, translated
  engines, TLB metadata, the C/kernel ABI, and Linux retry handling. Repair
  gadget exception cleanup so direct engine callers always receive materialized
  state with the host environment restored, and make interrupt-token
  registration failure deterministic rather than silently discarded.
- Replace permanent retired-domain tombstones with bounded, allocation-free
  identity/lifetime protection. Make GPU framed I/O stop-aware, define distinct
  service/guest endpoint ownership, reset device state across owners, and make
  command mutations exception-atomic with direct host-only tests.
- Turn the gadget and native-code implementation fragments into ordinary
  responsibility-owned translation units. Resolve included headers to engine
  ownership in the architecture audit, and run common conformance through the
  exact production engine compositions rather than test-local recompilations.
- Split mixed Linux topic 0002 into reviewable final-state concerns; make fresh
  integration presets accept the required case-sensitive kernel checkout; make
  every suite aggregate build every registered cross-directory fixture; and
  rebuild both kernels, the current CLI, all three package slices, and package
  consumers only after source work is complete.
- Make the symmetric engine-edge fixture and source-tree audit comfortably fit
  their registered timeouts. Remove the unreachable fatal gadget, production
  runtime/network test instrumentation, unconditional disabled-profiling work,
  unbounded profiler records, implicit registry destruction ordering, and the
  raw-CTest label-verification bypass. Clarify whether “Python-free unit” means
  test implementation/execution or also the configure/build toolchain.

Fresh seventh-audit host verification passed **562/562** 16 KiB units,
**563/563** 4 KiB units, and full ASan+UBSan **562/562** from current-source
build directories. Architecture, TSan **4/4**, loader ASan+UBSan **1/1**, and
normal/sanitized GPU ownership gates passed. The ordinary tooling run passed
**22/23**: the symmetric engine-edge
test timed out at 60.01 seconds but its exact command passed directly in 59.71
seconds, confirming a timeout-margin defect rather than a semantic failure.
Regenerating the 983-row allocation file was byte-identical at SHA-256
`0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`; that
only proves deterministic reproduction of the dependent method. No guest,
benchmark, current kernel/product, or rootfs campaign was used for this audit.

## Sixth-audit closure claim (historical; superseded)

The seventh-audit findings above supersede this completeness claim. The
following records what the sixth pass reported and remains useful dated
evidence.

- The ARM64 allocation catalog is generated independently with LLVM MC 22.1.8:
  983 rows, 79 distinct masks, and only 21 exact-word masks. The validator joins
  architectural form plus opcode provenance and seven negatives reject the
  formerly accepted self-consistent wrong-form case. Both page-size reports
  contain all 983 IDs and are byte-identical at SHA-256
  `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`.
- Native 4 KiB shadow state is domain-owned and retired on normal and abnormal
  run exit. Sequential, concurrent, and abnormal public-handle tests plus map,
  protect, unmap, and refresh fault injection prove exception containment,
  rollback, isolation, and retry. Native/non-JIT storage is data-only.
- GPU queue descriptors and VNC initialization use RAII/transactional
  ownership. Production replacement/failure/retry and exact-port-reuse cases
  run in canonical rootfs-free normal and ASan+UBSan GPU workflows.
- Gadget and JIT responsibilities are split into logical owners while
  preserving decoder order and the production JIT object's bytes. Cross-engine
  link/object/aggregate/include/source-include edges are rejected symmetrically
  by a positive fixture and 24 negative cases.
- The virtual-transport DMA-mask fix is independent Linux topic 11. Kernel and
  dependency policy reject extra unlisted patches. Queued-transmit restart is
  deterministic, and the NEON inventory now lives under `tests/unit/arm64`.
- Source sizing remains advisory: 1,060 files are reported, 60 exceed a
  guideline, 42 lack optional review context, and zero changed since review.

Fresh closure verification passed 562/562 16 KiB units, 23/23 tooling checks,
563/563 4 KiB units, 562/562 full ASan+UBSan, 2/2 focused native 4 KiB
ASan+UBSan, 4/4 TSan, 1/1 loader ASan+UBSan, 1/1 normal GPU, 1/1 sanitizer GPU,
and 1/1 rootfs-free network coverage. iOS device and simulator 21-target graphs
passed; archive scans found no executable publication outside the explicit JIT
arena. A fresh authenticated eleven-topic Linux replay passes exact topic-path
overlap policy, verifies the 162/13 delta, and reaches canonical tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`.

No guest boot, Node.js, Go, browser, benchmark, or controlled performance
workload was run for this closure. Physical sandboxed/entitled iOS and other
external campaigns remain explicit acceptance work, not repository refactor
items.

## Fifth-audit closure claim (historical)

This section records what the fifth pass reported. The sixth audit above
supersedes its completeness claim; statements below use their original wording
so the implementation history remains understandable.

- Exact ARM64 evidence now joins every encoding to an independent 983-row
  allocation catalog and records exact required/observed host features. Six
  negatives cover encoding-only and unrelated-feature mutations. Both page
  sizes produce 983 stable IDs with 710 native executions and 273 exact skips.
- Native-code targets no longer link, embed, include, or call gadget-owned code;
  shared transfer/TLB/environment support is neutral, and the architecture
  audit rejects any future native-code-to-gadget edge. Interpreter and gadget
  paths contain no executable-memory publication primitive, W+X mapping, or
  RW/RX transition; explicit JIT ownership remains available.
- Context/provider publication is a rollback-safe joint transaction. MMU
  mappings, history, invalidation, stale batch identity, and native 4 KiB shadow
  state are exception-atomic under deterministic Nth-failure coverage. Queued
  embedded stop admission has deterministic wait-boundary coverage.
- GPU descriptors acquire RAII ownership immediately after `recvmsg`; repeated
  malformed, short, oversized, dispatch-exception, and allocation-failure cases
  do not leak. Startup failure injection is present only in a test variant.
- Evidence reset/production/validation has standalone presets, raw tooling is
  non-destructive, the loader has a canonical ASan+UBSan gate, and next-version
  policy emits tested carry/drop/retest/upstream actions for applied, contained,
  and conflicting patches. ABI, provider ownership, TSan, and replay docs
  matched behavior at that time.
- The default dependency prefix was rebuilt from pinned release inputs and
  passes its full verifier. macOS, iOS device, and iOS simulator archives pass
  architecture and raw/install-tree C/C++ consumers; iOS symbol audits confine
  executable publication to the explicit JIT arena.

Dated fifth-audit verification:

- deterministic 16 KiB evidence pipeline: **563/563** in 286.37 seconds;
- deterministic 4 KiB evidence pipeline: **564/564** in 279.60 seconds, with
  architecture at **553 targets / 585 registrations**;
- non-destructive raw tooling: **22/22**;
- default-prefix GPU ownership fixture: **1/1** after 64 repetitions per error
  class; GPU architecture at **562 targets / 586 registrations**;
- canonical malformed-loader ASan+UBSan: **1/1**; TSan passed exactly **4/4**
  in 4.44 seconds;
- authenticated Linux replay matches canonical final tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; replay commit identity and build
  metadata remain intentionally dynamic;
- independent public and kernel bridge ABI v4 identities, 162/13 Linux delta
  policy, next-version/topology negatives, rebuilt dependency verification,
  exact 16/4 KiB active-import checks, pristine LibVNCServer 0.9.15, the then-current
  16 KiB production kernel/app final link, all three package graphs, and C/C++
  consumers pass; and
- source size remains advisory: 1,034 files, 53 above guideline, 42 without
  optional review context, and six changed since review.

No guest boot, Node.js, Go, browser, or benchmark workload was run during this
closure. Rootfs redesign and external/device campaigns remain outside this
repository-owned completion claim.

## Fourth-audit closure (historical)

The following was the reported end state of the fourth audit. It remains useful
dated evidence but is superseded by the later historical closure records and
the current seventh-audit checklist above.

- Exact instruction evidence uses a checked-in 983-row catalog, exact encoding
  and architectural-form joins, per-engine executed/retired results, declared
  aliases only, and four negative fixtures. Fresh 16 KiB and 4 KiB reports each
  validate all 983 IDs: 713 JIT executions and 270 exact feature skips.
- Every engine maps a valid implementation gap to the guest illegal-instruction
  event without retirement or fallback and remains reusable. Same-handle runs
  fail fast; different embedded handles remain process-serialized with
  stop-aware queued admission.
- Software-MMU state is domain/run isolated, C allocation failures are
  contained and retryable, and the ELF loader uses non-destructive bounded
  mappings with complete cleanup and malformed-image coverage.
- The installed emulator archive is provider-neutral. The macOS app owns and
  injects SLiRP, while networking-enabled raw and installed archives link from
  genuine C and C++ consumers. Public API identity v4 is independent from the
  kernel bridge v4 identity.
- SLiRP readiness, runtime target composition, test/fixture ownership, API
  documentation, GPU descriptor RAII, and final-link provenance now have
  focused acceptance tests. The final-link audit is an allowlist, including an
  arbitrary ambient-archive rejection fixture.
- LibVNCServer is the pristine pinned 0.9.15 release tree
  `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`.
  A fresh verified prefix, loopback VNC smoke, and all package consumers pass.
- Linux replay enforces ten exact nonempty patch IDs, subjects/order, and no
  merges. The authenticated release replays to exact tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` over 162 paths, including removal
  of the obsolete `DARWIN_LIBELF_INCLUDE` hook.
- Expired weak registrations are pruned under 3,000-cycle churn, network
  implementation includes are private, and root `.DS_Store` debris is removed
  and rejected by the source audit.

Rootfs file management is deferred to a later testing redesign. Image hashes,
sidecar locking, maintenance serialization, and rootfs cleanup are not current
refactor exit criteria.

## Fourth-audit verification (historical)

- The then-current 4 KiB host units passed **562/562**; that audit's 16 KiB unit
  selection passed **561/561**, with focused MMU, GPU, VNC, lifecycle, gadget,
  and package checks repeated after its final edits.
- The embedded-loader fixture passes at the exact production link window and
  in a targeted ASan+UBSan build covering occupied-window refusal, malformed
  dynamic/relocation metadata, cleanup, MMU isolation, and run admission.
- Architecture audits pass for the 559-target unit/tooling/GPU graph, the
  tooling-disabled external-integration graph (39 targets / 10 registrations),
  and all three package product graphs.
- Public ABI v4 is fingerprint `0xd9a69804d3303044`, SHA-256
  `d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`.
  Kernel bridge ABI v4 is independently fingerprinted as
  `0xfe2d71a33e3d446e`.
- The fresh private dependency prefix passes source-tree, receipt, payload,
  archive-member, architecture, platform, and deployment-target verification.
  Build timestamps, identities, compiler notes, and binary hashes remain
  dynamic observations.
- The three arm64 package slices pass raw/install-tree C and C++ consumer
  links. iOS device and simulator symbol audits find no executable-memory
  publication primitive outside `jit_arena.cpp`; non-JIT paths perform no
  RW/RX transition and never map W+X.
- The source-size report covers 1,023 files: 54 above an advisory guideline, 43
  without optional review context, and five changed since review. These are
  informational, not line-count failures.
- The previously recorded rootfs mode, size, and digest remain historical
  observations only; rootfs management is no longer a refactor acceptance
  gate.
- Interpreter/gadget state and MMU storage remain data-only; executable
  publication is reachable only through the explicitly selected JIT arena.

## Preserved earlier regression matrices

These dated results remain useful broad evidence and were not deleted: normal
unit/tooling **580/580 at 16 KiB** and **581/581 at 4 KiB**; **562/562
ASan+UBSan**; **5/5 targeted TSan**; and fast guest integration **24/24** at
each page size. They are not substituted for the targeted current-source
acceptance tests in the handoff.

The preserved 16 KiB gadget Geekbench result is **80 single / 506 multi**
(result), +3.90%/-0.39%
against the immediately preceding 77/508 run. No heavy benchmark was rerun as
part of the fourth audit.

## External acceptance (historical snapshot)

An LTP-enabled guest image, physical sandboxed or
entitled iOS devices, opt-in public-network/interactive-GUI/rendered-browser
campaigns, and repeated controlled gadget/JIT performance runs remain external
prerequisites. They are not silent pass claims and are separate from the open
repository-owned seventh-audit checklist.
Rootfs file management is deferred rather than treated as external acceptance.
