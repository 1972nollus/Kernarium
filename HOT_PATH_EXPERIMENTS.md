# Gadget hot-path experiments

This log tracks the requested optimization trials in priority order.
Each trial is implemented in isolation on top of the last accepted build,
validated with the production Release gadget configuration, and then measured
with a complete UnixBench 6.0.1 run.

## Controlled benchmark

- Release production gadget build; debug interpreter run path disabled.
- 20 guest vCPUs and `mem=8G`.
- GPT partition 1 of `rootfs.img` mounted ext4 read-only.
- tmpfs-backed overlay upper/work directories; the original image is never
  modified.
- Exact guest command: `/root/byte-unixbench/UnixBench/Run`.
- Acceptance score: geometric mean of the single-copy and 20-copy headline
  indices. A trial must improve this score against the immediately preceding
  accepted build and pass correctness tests. Isolated sub-test regressions are
  profiled and investigated instead of automatically rejecting an
  overall-improving trial.

The recorded starting baseline is:

| Build | Single-copy | 20-copy | Composite | Decision |
|---|---:|---:|---:|---|
| Pre-trial baseline | 239.2 | 819.6 | 442.773 | Reference |
| P5-free baseline after requested revert | 305.0 | 976.9 | 545.852 | Trial 10 reference |

## Trials

| Priority | Trial | Single-copy | 20-copy | Composite | Decision |
|---:|---|---:|---:|---:|---|
| 1 | Preserve caches across address-space switches | 218.3 | 855.6 | 432.178 | Reverted |
| 2 | Share immutable programs across vCPUs | 174.8 | 215.9 | 194.266 | Reverted |
| 3 | Fixed-size set-associative cache and slab storage | 247.9 | 940.6 | 482.882 | Accepted |
| 4 | Data-only cached-block successor links | 265.8 | 979.1 | 510.142 | Accepted |
| 5 | Specialized and fused gadgets | 269.2 | 982.8 | 514.363 | Reverted by request |
| 6 | Reduce syscall-boundary state copying | 269.5 | 983.6 | 514.859 | Accepted |
| 7 | Optimize cold translation | 313.4 | 979.6 | 554.082 | Accepted |
| 8 | Larger cache capacity and translated-block size | — | — | — | Reverted |
| 9 | Asynchronous Darwin block I/O | 310.6 | 916.6 | 533.569 | Reverted |
| 10 | Host-register-mapped native NEON execution | 308.8 | 966.8 | 546.395 | Reverted |
| 11 | Vectorized bulk software-MMU translation | 304.1 | 969.7 | 543.034 | Reverted |

### Trial 1: preserve caches across address-space switches

The thread-local program cache is keyed by `{generation, code_changes, pc}`.
Switching Linux address spaces no longer clears translations belonging to
other generations. Code invalidation selects a new sequence key, so stale
translations cannot execute.

Release CTest passed 298/298. UnixBench completed with status 0, but the
composite score fell from 442.773 to 432.178 and the single-copy score
regressed by 8.7%, despite a 4.4% improvement in the 20-copy score. The change
was reverted.

### Trial 2: share immutable programs across vCPUs

A bounded process-wide cache owns immutable translated programs per address
space, code sequence, and PC. Each execution thread retains a local
`shared_ptr` front cache, so the block-boundary hit path stays thread-local and
lock-free; the shared mutex is consulted only after a local miss.

Release CTest passed 298/298 and UnixBench exited 0, but the composite score
fell to 194.266. Short-lived parallel workloads collapsed under shared-cache
miss synchronization and global capacity churn: 20-copy Execl fell from
1,484.5 to 67.5 lps, and the 20-copy headline index fell from 819.6 to 215.9.
The change was reverted.

### Trial 3: fixed set-associative cache and slab storage

The thread-local `unordered_map` is replaced with 4-way, direct-indexed cache
sets. Epochs invalidate old address-space/code-sequence entries in constant
time. Translated `BuildResult` objects live in stable 256-slot slabs allocated
on demand; set-local round-robin replacement reuses their storage, eliminating
the full-cache clear cliff.

The first full run exposed and prompted correction of an invalid placement-
construction lifetime. After the correction, Release CTest passed 298/298 and
the repeated UnixBench run completed with status 0 and both completion
markers. Its headline composite improved from 442.773 to 471.811, but it was
rejected under the per-workload regression rule:

| Regressed workload | Accepted baseline | Trial 3 | Change |
|---|---:|---:|---:|
| 20-copy Pipe Throughput | 1,129,243.3 lps | 1,022,287.5 lps | -9.5% |
| 20-copy System Call Overhead | 866,591.8 lps | 694,999.4 lps | -19.8% |

The fixed cache and slab implementation was initially reverted. After the
2026-07-24 reboot, the acceptance rule was revised to retain aggregate
improvements while profiling isolated regressions. Trial 3 was therefore
restored with safe `std::optional` object lifetimes inside lazily allocated
slabs.

Three-pass targeted 20-copy reruns did not reproduce either regression:
Pipe Throughput measured 1,144,064.2 and 1,148,769.7 lps, while System Call
Overhead measured 1,392,597.5 and 1,281,126.3 lps. Cache instrumentation
showed zero full-set evictions in the benchmark workers: Pipe workers made
roughly 11–12 million lookups with about 2,700 translations each; syscall
workers made roughly 18–20 million lookups with about 2,900 translations each.
The earlier slow samples therefore do not appear to be caused by indexed-cache
conflicts. All instrumentation was removed before the confirmation benchmark.

The complete production confirmation passed 298/298 tests and completed
UnixBench with both copy counts and its completion marker. It measured 247.9
single-copy and 940.6 at 20 copies, a 482.882 composite: 9.06% above the
442.773 starting baseline. The formerly regressed full-suite samples recovered
to 1,169,327.7 lps for 20-copy Pipe Throughput and 1,243,809.8 lps for
20-copy System Call Overhead. Trial 3 is accepted as the base for trial 4.

### Trial 4: data-only cached-block successor links

Direct and conditional control-flow gadgets now consume stable `BlockLink`
cells embedded in their data streams. A translated target activates its cell
with the successor stream and instruction count; an unresolved or evicted
target points at a one-gadget exit stream. Consequently the normal linked hot
path contains no opcode recheck, cache hash lookup, or linked/unlinked branch.
Chains return to C++ after at most 4,096 guest instructions so interrupt checks
remain prompt and the existing 64K fairness budget is preserved.

Link resolution, target/fallthrough selection, and specialized condition
selection are performed during translation. Floating-point blocks remain
unlinked so deferred FP-frame materialization stays correct. The assembly
engine returns its event and executed count in the AArch64 two-register
aggregate ABI; Apple-reserved `x18` remains untouched.

A new standalone `arm64-gadget-links` test file validates unresolved fallback,
resolved multi-block execution and counts, both conditional outcomes, and the
exact 4,096-instruction self-loop bound. The complete standard suite passes
299/299 tests.

The first complete run measured 266.1 single-copy and 983.0 at 20 copies, a
511.445 composite (5.92% above trial 3). Single-copy 4 KiB File Copy fell from
277,583.4 to 250,090.0 KB/s, so it was investigated rather than used to reject
the overall gain:

- three targeted runs measured 248,987.0, 248,471.0, and 247,667.0 KB/s;
- disabling all successor links at translation time did not recover the
  workload;
- replacing the initial stack result writeback with the register-return ABI
  also left the result unchanged;
- dispatch profiling showed roughly 4.94 million guest entries in 30 seconds,
  almost all direct system calls, with only 46.7 guest instructions per entry;
- a 15-second host stack sample found cold block translation and software-MMU
  fetch/translation above gadget execution among the sampled emulator work.

This workload is therefore structurally unable to benefit from block chaining,
and the A/B evidence does not attribute its variation to successor links. The
translation/MMU hotspots are retained for trials 7 and 11.

The clean final register-return build completed both copy groups and the
explicit child marker. It measured 265.8 single-copy and 979.1 at 20 copies,
a 510.142 composite: 5.65% above trial 3. Release CTest passed 299/299 and
`rootfs.img` retained its original size, mtime, and SHA-256. Trial 4 is
accepted as the base for trial 5. The complete log is
`/private/tmp/unixbench-priority4-final.log`; the host sample is
`/private/tmp/unixbench-priority4-fsdisk.sample.txt`.

### Trial 5: specialized and fused gadgets

A 15-second host sample of the accepted Dhrystone build identified specialized
`B.eq`/`B.lo` entries and flag-setting compare forms among the hottest gadget
PCs. The production translator now fuses an adjacent `CMP` plus `B.cond` for
immediate, unshifted-register, and common UXTB-extended compare forms. It
selects one of condition- and width-specific gadget entries and resolves both
successor cells at translation time.

The fused runtime path still commits architectural NZCV, but removes one
indirect gadget dispatch and the branch gadget's NZCV load/MSR pair. It does no
opcode testing and contains no taken/not-taken branch: a host `CSEL` chooses the
target `BlockLink`. Verification/debug builds continue to lower both guest
instructions independently.

The new standalone `arm64-gadget-fusions` test file differentially validates
all 14 meaningful conditions, 32/64-bit immediate/register/UXTB forms,
taken/fallthrough outcomes, NZCV preservation, linked successors, and executed
instruction counts. Release CTest passes 300/300 tests.

Targeted Dhrystone improved from 1,831,239.7 to 1,903,477.8 lps single-copy
and from 30,421,421.9 to 31,056,976.9 lps at 20 copies. The complete run
finished both copy groups and the explicit marker, measuring 269.2
single-copy and 982.8 at 20 copies, a 514.363 composite: 0.83% above trial 4.
Full-run Dhrystone improved by 4.56% single-copy and 4.62% at 20 copies.
Several parallel non-CPU results varied downward by at most 1.24%, with
unchanged phase durations and no common affected path; these are retained as
normal run variance rather than an overall regression. Trial 5 is accepted as
the base for trial 6. The full log is
`/private/tmp/unixbench-priority5.log`; the selection profile is
`/private/tmp/unixbench-priority5-dhry.sample.txt`.

On 2026-07-24 the user requested that this trial be reverted because its
0.83% composite improvement was marginal and workload-dependent. The
production translator once again emits adjacent `CMP` and `B.cond`
instructions independently. All P5-only compare/branch gadgets, declarations,
selection logic, CMake registration, and the standalone fusion test were
removed; the retained block-link machinery from trial 4 is unchanged. The
post-removal production build passes 301/301 standard CTests. Trials 6 and 7
remain in the source tree, but their recorded scores above were measured when
P5 was still present, so a fresh P5-free baseline is required before judging
trial 10.

That fresh baseline completed both copy groups and all child markers on
2026-07-24. It measured 305.0 single-copy and 976.9 at 20 copies, a 545.852
composite. Key P5-sensitive/control results were 1,811,304.2/30,124,572.3
Dhrystone lps, 472.5/1,820.0 Execl lps, and 458,313.6/1,734,678.1 KB/s for
the 4 KiB file-copy selector. The original image retained size 8,589,934,592,
mtime 1,784,725,767, and its previously verified SHA-256; all guest writes
were confined to the tmpfs OverlayFS upper.

### Trial 6: reduce syscall-boundary state copying

The guest entry path now owns one persistent `CpuState`, guest-memory view, and
gadget executor per Darwin vCPU thread. Each entry still reloads all
task-visible state, clears the local exclusive monitor, and lazily refreshes
the FP source frame, so vCPU migration cannot retain another task's
architectural state. Each exit writes back GPRs, status, TLS, and dirty FP
state. Production disassembly confirms that the former 0x4e0-byte stack object
and 529-byte zero-fill have disappeared; the entry frame is now 0x70 bytes.

The audit also found that FPMR and user-visible SVCR state had no Linux UML
register-frame slots and was silently reset at every boundary. The Darwin
guest ABI and `uml_pt_regs` now carry both values, while `flush_thread()` clears
them on exec. PSTATE writeback now preserves and restores NZCV, TCO, DIT, SSBS,
and DAIF instead of retaining only NZCV.

A new standalone `arm64-static-boundary-state` test file covers FPMR,
PSTATE/SVCR, local-monitor invalidation across SVC, and lazy FP refresh across
different task frames. The complete production suite passes 301/301 tests.
Targeted System Call Overhead before/after results were 431,552.6/440,426.6
lps single-copy (+2.06%) and 1,321,179.3/1,307,976.7 lps at 20 copies
(-1.00%).

The complete benchmark finished both copy groups and the explicit marker. It
measured 269.5 single-copy and 983.6 at 20 copies, a 514.859 composite: 0.10%
above trial 5. Full-run System Call Overhead improved by 1.25% single-copy and
1.93% at 20 copies. The largest negative row was 20-copy Execl at -2.38%;
focused repetitions measured 1,817.0 and 1,800.2 lps versus trial 5's full-run
1,837.9 lps.

A 10-second sample during the focused Execl run found software-MMU
translate/fetch, cold program allocation, and block linking above guest
boundary entry. The removed boundary zero-fill was absent. This small,
repeatable regression is therefore carried into trial 7's cold-translation
work instead of reverting trial 6's correctness and aggregate improvement.
Trial 6 is accepted as the base for trial 7. The full log is
`/private/tmp/unixbench-priority6.log`; the focused log and sample are
`/private/tmp/unixbench-priority6-execl-repeat.log` and
`/private/tmp/unixbench-priority6-execl.sample.txt`.

### Trial 7: optimize cold translation

On a cache miss, the executor previously called `StaticGuestMemory::fetch`
once for every instruction in the 64-instruction window. It now fetches one
page-bounded byte span at a time: one call normally, or two when the window
crosses a guest page. A new standalone `arm64-static-translation` test file
validates successful cross-page construction and preservation of a fault on
an unmapped second page.

The first batched-fetch Execl repetitions measured 1,798.1 and 1,804.3 lps,
showing that fetch batching alone did not repair trial 6's parallel Execl
variation. A matched profile also showed separate program-word allocations
and frees among the remaining cold-path costs. Production translations now
allocate their program vectors from an unsynchronized pool owned by each
vCPU's `ProgramCache`; verification/debug builds retain the default allocator.
Allocator-agnostic emit helpers preserve identical gadget streams.

With both changes, focused 20-copy Execl measured 1,857.0 and 1,852.4 lps,
recovering above trial 5's 1,837.9 lps. The matched profile confirmed the
mechanism: `StaticGuestMemory::fetch` top stacks fell from 3,417 to 270 and
`translate` from 5,724 to 241, while malloc/free stacks also declined. The
large decoder became the remaining cold-path hotspot; a risky reordering of
overlapping A64 families was not attempted without a generated top-level
decode table.

The complete production suite passes 302/302 tests. UnixBench completed both
copy groups and the explicit marker, measuring 313.4 single-copy and 979.6 at
20 copies, a 554.082 composite: 7.62% above trial 6. Cold-start-sensitive
single-copy rows improved sharply: Execl +63.1%, Process Creation +31.1%, and
the shell rows +30.9%/+17.1%. Parallel Execl improved 3.18% to 1,851.2 lps.

The full run showed lower 20-copy Dhrystone (-2.69%), Context Switching
(-3.81%), and System Call Overhead (-5.18%). They were investigated rather
than used to reject the aggregate win. A focused three-workload repeat
measured 31,532,095.6, 71,254.8, and 1,337,882.7 respectively, reducing those
deltas against trial 6 to -0.60%, -0.27%, and -0.90%. Matched profiles found
no program-pool allocation in the steady paths: Dhrystone remained
gadget-bound, Context Switching host synchronization/signal-bound, and System
Call Overhead boundary/timekeeping-bound. No targeted representation change
was justified.

`rootfs.img` retained its original size, mtime, and SHA-256 after the run.
Trial 7 is accepted as the base for trial 8. The complete log is
`/private/tmp/unixbench-priority7.log`; focused logs are
`/private/tmp/unixbench-priority7-pool-execl.log` and
`/private/tmp/unixbench-priority7-regressions.log`; matching profiles use the
`/private/tmp/unixbench-priority7-*.sample.txt` names.

### Trial 8: larger cache capacity and translated-block size

The accepted configuration has a 4-way 16,384-entry cache and a
64-instruction translation window. Larger settings were swept with an
identical 20-copy Dhrystone/Execl/System Call Overhead selector. The matched
accepted configuration's calculated partial index was 1,013.0.

| Candidate | Dhrystone | Execl | System call | Partial index |
|---|---:|---:|---:|---:|
| 16K entries / 64 instructions | 31,532,095.6 | 1,854.7 | 1,337,882.7 | 1,013.0 |
| 32K entries / 64 instructions | 31,672,705.0 | 1,845.8 | 1,302,057.1 | 1,003.7 |
| 64K entries / 64 instructions | 31,715,854.5 | 1,831.2 | 1,343,224.7 | 1,012.0 |
| 16K entries / 128 instructions | 28,729,969.9 | 1,796.1 | 1,307,374.5 | 964.1 |

Neither larger cache improved the selector. This matches trial 3's
instrumentation, where benchmark workers used only about 2,700–2,900
translations and observed no full-set evictions; the larger pointer arrays
add footprint without relieving capacity pressure. Doubling the translation
window caused an 8.9% Dhrystone regression and a 3.2% Execl regression because
more code is fetched/reserved before early control-flow termination. This met
the significant-downgrade stop rule, so a 256-instruction window was not run.

All candidates were reverted. The accepted 16K-entry/64-instruction trial 7
configuration remains the base for trial 9. Candidate logs are
`/private/tmp/unixbench-priority8-cache32k.log`,
`/private/tmp/unixbench-priority8-cache64k.log`, and
`/private/tmp/unixbench-priority8-block128.log`.

### Trial 9: asynchronous Darwin UBD I/O

The candidate replaced the Darwin UBD ordered Linux workqueue with a serial
Grand Central Dispatch queue. Blocking host `pread`/`pwrite` operations ran on
a Dispatch worker, while completed request pointers returned through a
nonblocking socketpair so that `blk_mq_end_request()` still ran only in the UML
interrupt context. Serial execution retained the original helper's ordering
and COW bitmap assumptions.

A standalone host queue test submitted 256 ordered `pread` jobs, verified that
they ran off the submitting thread, validated all bytes, checked signal
preparation, and drained before teardown. The candidate kernel and the
303-test production suite passed. A scratch 64 MiB UBD integration boot wrote
and compared a 3.8 MiB executable at 315 MB/s, then wrote the full device at
591 MB/s while a second guest task remained schedulable.

The first boot exposed a Darwin/Linux errno-number mismatch: Darwin's
nonblocking `EAGAIN` is 35 while the kernel constant is 11. A host-boundary
predicate removed the resulting false `spurious interrupt` report. Profiling
then identified a `pthread_sigmask()` call on every request; the corrected
candidate used pthread-specific state to prepare each Dispatch worker only
once, retaining signal safety without the repeated syscall.

The complete pre-correction benchmark had valid exit and completion markers:

| Configuration | 1 copy | 20 copies | Composite |
|---|---:|---:|---:|
| Accepted trial 7 | 313.4 | 979.6 | 554.082 |
| Dispatch UBD | 310.6 | 916.6 | 533.569 |

The apparent parallel regressions were investigated rather than immediately
used to reject the proposal. After the once-per-worker correction, a focused
20-copy repeat recovered Execl to 1,841.4 lps, Process Creation to 1,986.1
lps, Shell Scripts (1) to 5,805.9 lpm, and Shell Scripts (8) to 768.2 lpm.
These are effectively the accepted trial 7 levels. A matched 10-second Execl
sample found the Dispatch workqueue thread idle for the entire window because
the rootfs was already in the host/guest page caches. Translation and gadget
execution dominated; the earlier large row drops were run variance, not
active asynchronous I/O.

The proposal therefore passed correctness and provided functional
asynchronous block service, but did not improve the required overall
UnixBench comparison. It was reverted under the keep-only-if-improved rule,
including its now-unused Dispatch wrapper and standalone candidate test.
Trial 7 remains the accepted base for trial 10. Logs are
`/private/tmp/unixbench-priority9.log` and
`/private/tmp/unixbench-priority9-workerprep-repeat.log`; the matched profile
is `/private/tmp/unixbench-priority9-workerprep.sample.txt`.

### Trial 10: host-register-mapped native NEON execution

The first candidate fused adjacent vector loads and stores into native copy
gadgets. Runtime instrumentation found zero fusion hits in UnixBench, so it
was replaced with a persistent mapping of guest V0–V7 to host Q16–Q23. A
translation-time lookahead selected only complete local load-to-store regions;
an `x27` dirty mask and boundary flush preserved architectural state without
adding opcode decoding to the runtime hot path. The mapped forms covered
128-bit unsigned-offset and unscaled loads/stores plus adjacent even-register
128-bit pairs.

A standalone split test validated every selected addressing form, pair and
single-register behavior, helper and FP/SIMD boundaries, dirty writeback, and
fault behavior in both production and interpreter configurations. Production
disassembly confirmed direct native Q-register transfers and no runtime opcode
checks.

The complete benchmark exited successfully with both copy-count markers:

| Configuration | 1 copy | 20 copies | Composite |
|---|---:|---:|---:|
| Accepted P5-free build | 305.0 | 976.9 | 545.852 |
| Host-register NEON mapping | 308.8 | 966.8 | 546.395 |

The composite change was only +0.10%, while the parallel headline index fell
1.03%. More importantly, matched profiles showed zero mapped operations in
File Copy, Dhrystone, and Pipe Throughput. A shell-workload sample observed
only one mapped load and no matching mapped store during the sample window.
The measured delta therefore was not causally attributable to the candidate
and did not justify its permanent-register pressure, boundary machinery, or
maintenance cost.

The entire candidate was rejected and removed, including all unused mapping,
flush, and copy gadgets plus its standalone test. The accepted P5-free build
is restored as the base for trial 11. Profiles are
`/private/tmp/unixbench-priority10-persistent-fstime.sample.txt`,
`/private/tmp/unixbench-priority10-localmap-dhrystone.sample.txt`,
`/private/tmp/unixbench-priority10-localmap-pipe.sample.txt`, and
`/private/tmp/unixbench-priority10-localmap-shell.sample.txt`.

### Trial 11: vectorized bulk software-MMU translation

The Darwin software MMU is a sparse `unordered_map` of 4 KiB guest pages, so
there is no safe contiguous table to gather with NEON. The candidate instead
generated batches of two or four contiguous guest page addresses with native
NEON, resolved the sparse mappings while holding one shared lock, and retained
the existing per-vCPU TLB as the first lookup. Single-page reads, writes,
fetches, and validation stayed on their original specialized path.

A split test covered ordered four-page translation, partial permission
failure, invalid alignment/count handling, cross-page reads, and restartable
cross-page writes. The candidate passed the complete 302-test production
suite, the corresponding focused interpreter tests, and disassembly
inspection confirmed native `dup.2d`/`add.2d` address generation.

The complete overlay-root benchmark exited successfully with no remaining
children:

| Configuration | 1 copy | 20 copies | Composite |
|---|---:|---:|---:|
| Accepted P5-free build | 305.0 | 976.9 | 545.852 |
| Batched/vectorized MMU | 304.1 | 969.7 | 543.034 |

The composite regressed 0.52%. Parallel Dhrystone fell 5.49% and System Call
Overhead fell 7.72%; those workloads do not materially use the candidate.
Profiles confirmed only one P11 sample in Dhrystone and no hits during File
Copy. The helper was active during Execl and translated-block fetches, but
multi-page misses were a small fraction of sampled time and the result did not
improve Execl overall. The proposal therefore added work to cached cross-page
fetches without delivering a measurable aggregate benefit and was reverted,
including its API and standalone test.

Profiles are `/private/tmp/unixbench-priority11-dhrystone.sample.txt`,
`/private/tmp/unixbench-priority11-execl.sample.txt`,
`/private/tmp/unixbench-priority11-filecopy.sample.txt`, and
`/private/tmp/unixbench-priority11-parallel-execl.sample.txt`.

## Geekbench 6.7.1 profiling campaign

### Benchmark topology correction

The first Geekbench attempt exposed an invalid virtual CPU topology:
Geekbench reported `1 Processor, 1 Core, 20 Threads` and its multi-core pass
used only a little more than one host core. UML's generic topology supplied
the same default `core_id` for every vCPU, causing user space to collapse all
20 independent host-thread-backed vCPUs into one SMT core.

The arm64 UML topology now reports one physical package, one distinct core ID
per vCPU, and a singleton thread-sibling mask. The standalone SMP integration
suite verifies those sysfs attributes for every online CPU. The rebuilt guest
reports:

```text
Topology                      1 Processor, 20 Cores
```

The standard production suite passes 302/302 tests after this correction.
The standalone SMP integration suite also passes: two runnable guest jobs used
9.48 seconds of host CPU over 4.00 seconds of wall time (2.37 host cores), and
the CPU hotplug loop completed.

### Profiles and controlled-baseline status

Three macOS `sample` profiles were captured before source optimization:

- File Compression: linked direct/conditional branch gadgets dominate active
  engine samples, while C++ `run_block` is comparatively small. This supports
  deferring the guest-PC store on ordinary resolved successor edges.
- PDF Renderer: frequent `gadget_leave` and `run_block` transitions surround
  native vector arithmetic.
- Object Detection: `GadgetExecutor::run_block` (941 samples),
  `simd_unary_saddlp_8h` (688), `gadget_leave` (623), vector load (564), and
  several SIMD arithmetic gadgets dominate. This makes safe FP/SIMD successor
  links the highest-potential Geekbench-specific follow-up.

The topology-corrected baseline reached Multi-Core File Compression, but was
discarded before scoring when two orphaned Python multiprocessing pools began
using roughly 15--17 host cores. Their exact orphaned process groups were
terminated after approval. A separate active OpenCode workflow then began
repeatedly launching `./tools/crack_salt_c` at roughly nine host cores, so no
new baseline was started. Time Machine was stopped again. Benchmarking must
resume only after that user-owned workload is no longer active.

A later topology-corrected run completed every single-core and multi-core
workload in 4h06m51s. It is a functional/profile control rather than a scored
baseline:

- Time Machine restarted briefly during several long workloads. A 10-second
  watcher stopped each attempt, and the affected PDF Renderer, Photo Library,
  and Photo Filter rows are explicitly contaminated.
- A 15-second Multi-Core Navigation profile proved that Geekbench did create
  work across many vCPUs. Most carriers were blocked in the per-address-space
  Darwin walker mutex; TLB misses then contended on `mappings_mutex`, while
  mapping commits briefly closed the execution gate. This is the remaining
  reason the clean phase used only about 1--2 host cores after topology was
  corrected.
- The run uploads its result before printing scores.
  Guest DNS failed at that final step, so the process exited 255 and did not
  retain a `.gb6`/JSON result. The completed phase log is
  `/tmp/geekbench-watched-baseline.log` inside the discarded tmpfs overlay,
  and the host profile is
  `/private/tmp/geekbench-multicore-navigation.sample.txt`.

Network diagnosis reproduced the resolver failure independently. Direct TCP
through libslirp completed 200 fresh connections, and both the slirp worker
and guest route remained healthy. The virtual DNS proxy at `10.0.2.3`,
however, transiently failed after roughly 20 rapid lookups and recovered after
two seconds. Direct `1.1.1.1` survived roughly 790 rapid lookups. A rotated
resolver set of Cloudflare, Google, then the virtual proxy passed 2,000
consecutive Geekbench-domain lookups. The overlay launcher now installs that
fallback order with bounded retries; a fresh scored run is still required.

A scored retry began with the corrected topology and hardened resolver, but
was stopped during single-core HTML5 Browser rather than retained. A host
isolation check found that a macOS `ReportCrash` agent had already been using
approximately one full core when the benchmark started, and a leftover
two-vCPU network-diagnostic guest was still present. The diagnostic guest and
benchmark guest were stopped. `ReportCrash` was handling the corpse of an
earlier `uml-darwin` process (PID 87932); after the user agent was killed, the
root `com.apple.ReportCrash.Root` service continued the same work at 100% CPU.
The next scored run must wait until that root-owned reporter finishes, is
temporarily suspended with administrator authorization, or the host reboots.

### Accepted trial G1: deferred PC stores on linked edges

The G1 source trial is retained after a matched focused A/B run.
Each resolved successor edge now loads the target stream and instruction count
as one pair and defers the architectural PC store until an actual engine exit.
This removes the target-PC load, `CpuState::pc` store, and stream-register move
from the ordinary linked path. Unresolved links each own an immutable
`{link_exit, guest_pc}` fallback stream, while the exact 4,096-instruction
fairness exit still stores the current target PC before returning.

The split link test covers unresolved and resolved direct edges, BL, both
always-condition encodings, both outcomes of B.cond, CBZ/CBNZ, TBZ/TBNZ,
sequential fallthrough links, live retarget/reset, and the exact fairness
boundary. Production and safety-first interpreter suites both pass 302/302.
Production disassembly confirms the ordinary edge has no PC store; only the
unresolved/fairness exits retain it.

A new standalone performance integration suite runs a checked 300-million
iteration conditional linked-block loop in one or ten guest processes. It is
not registered in standard CTest. Both A/B executables contain the same final
bounded-range MMU kernel and link metadata; only the `follow_block_link`
assembly is changed:

- ten-worker old PC-store path: 2.981-second median, 9.74 host cores;
- ten-worker deferred-PC path: 2.953-second median, 9.77 host cores, 0.94%
  faster;
- one-worker old PC-store path: 2.879-second median;
- one-worker deferred-PC path: 2.867-second median, 0.42% faster.

The gain is small but consistent across the single- and ten-worker controls,
and the retained path removes three instructions from every resolved linked
edge. A full Geekbench comparison remains useful as an end-to-end confirmation
but is no longer required to retain G1.

Two immutable A/B executables are prepared under `.build/bench/` with the same
embedded topology-corrected kernel and all other accepted emulator changes:

- `uml-darwin-baseline`,
  SHA-256 `d2c8a8c0b0eb84a9a54169c8d5b482bb72fdf0ea3c94825bbab62dfc0ba7ef98`;
- `uml-darwin-g1-deferred-pc`,
  SHA-256 `69212f4e17e22ebe24264d3a93d0045d486f2869fb33a2a9f4cc9fbeeb7d7265`.

The normal production executable is byte-identical to the G1 artifact.
Disassembly of the baseline and G1 artifacts verifies that the intended
linked-edge sequence is their relevant difference. This removes rebuild
variance from the pending clean comparison.

The user subsequently selected 10 guest cores rather than 20 so macOS
background work can use the other 10 host cores. The controlled A/B campaign
therefore uses `ncpus=10` for both artifacts; Geekbench reports
`1 Processor, 10 Cores`. The 10-core immutable-baseline run started at guest
epoch 1784942167 with 8 GiB configured memory, the read-only image plus tmpfs
OverlayFS root, working rotated DNS, and Docker/containerd/apt timers stopped.

### Accepted shared-mm fix: bounded disjoint TLB synchronization ranges

At the user's request, the long 10-core baseline was sampled immediately and
stopped during Multi-Core PDF Renderer. An exact score was intentionally not
waited for. The guest then powered off cleanly; the read-only `rootfs.img`
lower layer was not modified.

The 15-second sample
`/private/tmp/geekbench-10c-multicore-pdf.sample.txt` proves the low CPU use
was inside the emulator rather than Geekbench's scheduler:

- all ten Geekbench vCPU carrier threads existed;
- eight carriers spent roughly 58%--91% of their samples waiting in
  `darwin_uml_static_vm_walker_lock`;
- the ten carrier call trees contain 63,045 walker-wait samples out of
  106,630 total carrier samples (59.1%);
- the lock owner was repeatedly committing
  `darwin_uml_static_vm_batch_end`, especially
  `AddressSpace::map_locked`.

The kernel tracked all pending PTE changes in one global
`{minimum address, maximum address}` envelope per Linux `mm`. Concurrent
faults in distant thread stacks, heaps, shared objects, and JIT-like mappings
therefore made the Darwin walker visit the enormous empty gaps between real
changes. Every sibling vCPU then queued behind that unnecessary walk.

The accepted fix retains up to 16 sorted, disjoint 16 KiB host-slot ranges per
`mm`. It is allocation-free under the existing spinlock. Overlapping and
adjacent intervals merge exactly; capacity overflow bridges only the smaller
neighboring gap rather than collapsing the entire address space. A walker
atomically snapshots the range set, walks all snapshots into one host batch,
and preserves every failed snapshot for retry. The existing per-mm turnstile,
VMA lock order, stale-epoch retry, and host execution gate remain unchanged,
so the optimization does not weaken mapping safety.

Validation:

- a new standard unit suite tests sorted insertion, duplicate/adjacent and
  multi-range merging, all overflow directions, tie-breaking, 4,000
  randomized updates, ordering, capacity, and coverage invariants;
- the production gadget suite and correctness-first interpreter suite both
  pass 303/303 standard tests;
- the Darwin UML kernel and final combined executable build successfully;
- the standalone 10-core SMP/topology suite passes on the old and new builds,
  with two runnable guest jobs using 2.07 host cores in both controls;
- existing 20-worker anonymous and file-mapping LTP stresses remain correct
  and already use 9.50 and 8.87 host cores respectively;
- a new freestanding shared-mm integration workload gives ten clone threads
  distant valid mappings. The old envelope cannot finish even 200 iterations
  in 30 seconds. The final bounded-range build completes the identical 200
  iterations in 0.127 seconds, a greater-than-236x lower-bound improvement,
  and completes 20,000 iterations in 9.197 seconds.

An accepted-build follow-up sample is
`/private/tmp/shared-mmap-multirange.sample.txt`. In its ten vCPU carriers,
walker-lock waits fell to 1,079 of 60,630 samples (1.78%); mapping-batch work
accounted for another 958 samples (1.58%). This is a different focused
workload from PDF Renderer, so it is diagnostic rather than a Geekbench score,
but it verifies that the pathological address-span walk has been removed.

The final lock-audited immutable candidate is
`.build/bench/uml-darwin-g1-multirange-final`, SHA-256
`3d73367c1a52acdbe548ed5a7c2cf85a8102b8b455525f8774ecb6cb724858dd`.
The final audit moved the range-count read under its spinlock. After that
correction the kernel rebuilt, the range unit suite passed, and the short
10-core integration control completed 2,000 iterations per thread in 0.938
seconds while consuming 2.480 host-CPU seconds (2.64 cores).
Geekbench's `--multi-core` and `--single-core` switches were not usable in this campaign. There is therefore no supported way
to jump directly to its multi-core phase; a later real Geekbench confirmation
must run the complete CPU suite.

### Accepted trial G2: FP/SIMD successor linking

The existing cache deliberately left every FP/SIMD target unresolved. A vector
loop therefore left the assembly engine, restored host state, entered C++,
looked up the same PC, and re-entered assembly at every block boundary even
after both blocks were hot. Existing Object Detection and PDF Renderer samples
showed frequent `gadget_leave` / `run_block` boundaries around dominant SIMD
gadgets, so this was tested with a short focused workload instead of waiting
for another multi-hour Geekbench run.

Translation now classifies each completed target once. Integer-only targets
still point directly at their gadget streams. FP/SIMD targets point at a
private immutable `{fp_link_entry, target_stream}` prefix owned by their link
record. The ordinary loaded-state prefix is only:

- one FP-loaded byte load and one predicted conditional branch;
- one FP-dirty byte store;
- one translation-selected target-stream load and the normal gadget dispatch.

There is no runtime opcode decode or per-instruction validity check. Only a
fresh deferred kernel FP frame takes the cold C++ materialization helper.
Unresolved targets retain their private `{link_exit, guest_pc}` stream, and
the existing 4,096-instruction fairness exit is unchanged. Eviction recognizes
both direct and FP-prefixed targets and resets them before program storage is
reused.

The expanded split link suite directly checks already-loaded and newly
deferred vector frames, FPCR/FPSR transfer, dirty marking, syscall events,
linked vector-load faults and fault PCs, combined instruction counts, and the
shared exact fairness boundary. A ProgramCache boundary test warms the FP
target with one kernel frame, then enters through a cached integer predecessor
with a different deferred frame and verifies that the linked target observes
the new value. Final validation is:

- production gadget CTest: 303/303 passed;
- correctness-first interpreter CTest: 303/303 passed;
- 10-vCPU standalone SMP control: 39.95 host-CPU seconds in 4.01 seconds,
  or 9.97 host cores.

The standalone FP link integration workload performs 30 million checked
Advanced SIMD loop iterations per worker. Its matched G1/G2 results are:

- one worker: 0.575-second G1 median versus 0.334-second G2 median, 41.9%
  faster;
- ten workers: 0.601-second G1 median versus 0.366-second G2 median, 39.1%
  faster;
- the integer-only ten-worker control is not regressed: 2.934-second G1
  median versus 2.925-second G2 median.

The accepted executable is `.build/bench/uml-darwin-g2-fp-links`, SHA-256
`10ac942d9d0ea1890a8e7bd5776307f50701d904d867db058ca1e859d68e5483`.
The normal production executable is byte-identical. The focused guest binary
has SHA-256
`e91ec92bca04b82fdd8e96af71851825b65353c7c5cdad70bc30cc9b3ca187eb`.
The runner and both FP/SIMD and integer controls live under `tests/perf/` and
remain standalone integration/performance suites rather than standard CTest
tests.

### Accepted trial G3: additive mapping batches do not stop sibling vCPUs

The first complete, topology-correct 10-core Geekbench baseline is
(result). It scored **66 single-core**
and **306 multi-core** on the immutable G2 executable. The workload-boundary
profile is `/private/tmp/geekbench-g2-profile`.

The remaining large emulator-side serialization was
`AddressSpace::end_batch`. Every mapping batch acquired the global execution
gate and interrupted sibling vCPUs, even when the batch only published
previously absent demand-paged mappings. The software TLB stores positive
translations only, so publishing an absent page cannot leave a stale positive
entry in a running vCPU.

G3 classifies a completed mapping batch once, under the existing exclusive
mapping lock:

- batches which only add absent pages or replay identical mappings publish
  directly, without stopping sibling vCPUs;
- unmaps, permission changes, backing changes, and every other replacement
  retain the existing `ExecutionQuiesce` path;
- the invalidation epoch is checked before either path and checked again after
  acquiring quiescence for a destructive batch;
- a concurrent lookup either completes its miss before the exclusive mapping
  lock or observes the newly published mapping after the lock is released.

The new split `static-mmu-batch` unit suite holds an execution reader while
committing both classes. It directly proves that an additive batch completes
without an interrupt and that a replacement batch still interrupts and waits
for the reader. Production gadgets and the correctness-first interpreter each
pass **304/304** standard tests.

A matched ten-worker shared-mm stress improved from a 0.822-second median to
0.685 seconds (**16.7% faster**) and reduced host CPU consumption from 1.90 to
1.63 CPU-seconds (**14.2% less**). The immutable candidate is
`.build/bench/uml-darwin-g3-additive-batches`, SHA-256
`12b35d7864ee9ba4b44f27baf2ea428064cbf87fdd6bcca69c2e3d419dff5883`.

The complete G3 confirmation is
(result). It scored
**65 single-core / 324 multi-core**, versus 66/306 on G2: single-core changed
-1.5%, while the multi-core aggregate improved **5.9%**. Both runs used one
10-core guest processor, 8 GiB, the read-only first partition plus tmpfs
OverlayFS root, systemd, and the same benchmark preparation.

| Workload | G2 SC | G3 SC | G2 MC | G3 MC |
|---|---:|---:|---:|---:|
| File Compression | 84 | 82 | 322 | 359 |
| Navigation | 137 | 134 | 302 | 291 |
| HTML5 Browser | 54 | 54 | 398 | 396 |
| PDF Renderer | 74 | 73 | 247 | 362 |
| Photo Library | 67 | 66 | 382 | 450 |
| Clang | 40 | 43 | 310 | 290 |
| Text Processing | 38 | 38 | 48 | 47 |
| Asset Compression | 73 | 73 | 582 | 600 |
| Object Detection | 28 | 28 | 168 | 171 |
| Background Blur | 61 | 60 | 402 | 427 |
| Horizon Detection | 95 | 92 | 660 | 668 |
| Object Remover | 106 | 98 | 555 | 415 |
| HDR | 71 | 69 | 467 | 474 |
| Photo Filter | 90 | 87 | 147 | 198 |
| Ray Tracer | 74 | 75 | 703 | 686 |
| Structure from Motion | 65 | 63 | 301 | 384 |

The candidate profile is `/private/tmp/geekbench-g3-additive-profile`.
Representative measured changes are:

- Multi-Core PDF Renderer: 137.626 to 93.955 seconds, 4.92 to 5.68 host
  cores, and 5.69 to 8.35 Mpixels/s (**46.7% more throughput**). Its matched
  five-second call graphs contain 57.6% fewer walker-lock samples.
- Multi-Core File Compression: 71.913 to 65.757 seconds, 4.79 to 5.83 host
  cores, and 46.3 to 51.6 MB/s (**11.4% more throughput**).
- Multi-Core Photo Library improved 17.5%, Photo Filter 34.2%, and Structure
  from Motion 28.2% by reported throughput.
- Multi-Core Text Processing remained essentially serial at 1.32/1.33 host
  cores. This is a workload property rather than a missing guest CPU: Clang
  used 9.01/8.90 cores, and the standalone SMP control uses approximately all
  ten host cores.

The isolated regressions were profiled before accepting the aggregate gain.
Neither the G2 nor G3 Object Remover sample contains
`AddressSpace::end_batch` or a walker-lock wait, and both have the same
dominant scalar-load/arithmetic/branch gadget ordering. Its 25.1% multi-core
throughput regression therefore does not execute the changed path. Clang's
multi-core score fell 6.5%, but its walker/quiesce samples decreased and its
single-core score improved 7.5%. Navigation likewise showed fewer quiesce
occurrences despite a 3.6% score decline. No safe, causal correction was
available for these run-dependent exceptions, so G3 is retained for its
focused improvement, removed contention, and higher multi-core aggregate
rather than adding a speculative workload-specific branch.

### Rejected trial G4: per-memory-site L0 TLB hint

G4 moved the software-TLB index calculation out of the common memory gadget
path by retaining a mutable last-index hint at each translated memory site.
The candidate preserved the full page tag and permission checks, handled a
different `Tlb` object safely, and invalidated correctly across remapping. Its
focused correctness coverage passed.

The runtime cost of loading and maintaining the site hint exceeded the saved
hash instructions. A 50-million-iteration mixed scalar/SIMD memory control
changed from 1.105 to 1.107 seconds. The decisive 500-million-iteration run
changed from a 10.722-second G3 median to 10.840 seconds, a **1.10%**
regression. G4 was rejected and all implementation and CTest changes were
reverted. The standalone reusable control remains
`tests/perf/guest_memory_site_loop.S`.

### Accepted trial G5: keep guest NZCV in host x27

G5 reserves callee-saved host `x27` for the guest NZCV value across the
assembly engine. Translation-selected flag producers write `w27`; consumers
restore native PSTATE from `x27`; entry and exit perform the only normal
`CpuState::nzcv` load/store. Helper boundaries which can read or write flags
synchronize explicitly. Scratch users formerly occupying x27 were moved to
other already-saved registers.

The split `arm64-gadget-nzcv-residency` suite checks integer flag production,
MSR/MRS NZCV, FP compare flags, and a TLB-miss helper between producer and
consumer. The initial full sweep also exposed and repaired MOPS and software
FJCVT helper-boundary synchronization.

On the 300-million-iteration selector/control loop, seven matched G3 samples
had a 2.830-second median and G5 had a 2.278-second median: **19.5% faster**.
The 500-million-iteration mixed scalar/SIMD memory control improved from
10.722 to 9.921 seconds: **7.5% faster**. Production and interpreter each
passed the then-current 305-test standard suite. The retained immutable G5
comparison executable is `.build/bench/uml-darwin-g5-nzcv-register`, SHA-256
`9e51e9d66c20dd60f7ade135fbf98df5b350e7c9dfe6534760e23e0a60813c85`.

### Accepted trial G6: two-way indirect branch/return links

G6 gives each translated BR, BLR, and RET site a two-entry data-only target
cache. Translation fixes the source-register offset and cache-cell address.
A hit compares the guest target and follows the existing stable `BlockLink`
without returning to C++. Only a cold or polymorphic miss stores the target
PC, identifies its exact cache cell in private engine scratch, and returns to
the dispatcher for training. Target eviction resets the stable `BlockLink` to
its safe fallback stream, so cached sites never retain raw pointers to evicted
program storage. Code invalidation discards both programs and indirect cells
together.

The new split `arm64-gadget-indirect-links` suite covers a cold miss, both
cache ways, BR, BLR link-register semantics, RET with an arbitrary source
register, combined instruction accounting, and safe target-eviction fallback.
Production and correctness-first interpreter configurations each pass
**306/306** standard tests.

The dedicated 50-million-iteration monomorphic BLR/RET control improved from
a 1.440-second G5 median to 0.805 seconds, **44.1% faster**. An unaffected
direct-link control changed from 2.304 to 2.310 seconds (+0.26%, within run
noise), while the mixed memory/SIMD control improved from 1.032 to 1.026
seconds. G6 is retained. Its immutable executable is
`.build/bench/uml-darwin-g6-indirect-pic`, SHA-256
`381826874a7bcd263d786a2b7002a1d8346e8052e5938d5e06ca1508daf4600e`.
The standalone indirect control is
`tests/perf/guest_indirect_link_loop.S`.

### Accepted trial G7: hoist repeated FP-environment validation

G7 keeps the full FPCR comparison in the correctness-first interpreter
runpath, where the decoded C++ bridge can change `CpuState` directly. In the
production gadget engine, an active native FP environment is already current:
production FPCR/FPSR writers update it eagerly. Later FP/SIMD gadgets therefore
need only one load and one predicted branch instead of reloading and comparing
FPCR on every instruction.

The split `arm64-gadget-fp-environment` suite changes FPCR twice in one
translated block and verifies that native arithmetic observes each rounding
mode. It also verifies that clearing FPSR mid-block persists. Production and
correctness-first interpreter configurations each pass **307/307** standard
tests.

The 500-million-iteration FP control improved from a 4.485-second G6 median to
4.435 seconds, **1.1% faster**. G7 is retained. Its immutable executable is
`.build/bench/uml-darwin-g7-fp-environment`, SHA-256
`f00906ce747eb6fbbdb4087d2c9187be0dc2974273343d396f4015a2a2296da3`.
The standalone control is `tests/perf/guest_fp_environment_loop.S`.

### Rejected trial G8: wider predecoded GPR operand offsets

G8 changed the compact ADD/SUB-immediate and logical shifted-register
descriptors from architectural register numbers to `CpuState` byte offsets.
The stream stayed the same size and disassembly confirmed that two host
address-building instructions disappeared from each affected gadget.

The initial mixed control changed from 2.340 to 2.329 seconds, but a dedicated
1.6-billion-operation control tied exactly at 2.033 seconds. The decisive
8-billion-operation run changed from a 10.023-second G7 median to 10.029
seconds, a **0.06% regression**. The threaded indirect dispatch, rather than
the removed integer adds, limits this workload. G8 was rejected and the
runtime, translator, and lowering-test changes were reverted; the rebuilt
production executable is byte-identical to G7. The reusable control remains
`tests/perf/guest_operand_predecode_loop.S`.

### Accepted trial G9: generated top-level production decoder

G9 generates a 256-entry first-level table from A64's architectural `op0`
field. Production translation now jumps directly to branch/system,
data-processing, FP/SIMD, load/store, or exception lowering while retaining
the existing audited mask priority within each family. Explicit overrides
preserve ADR/ADRP's PC-relative lowering and the FEAT_PAuth_LR encodings which
overlap data-processing space. Matched families terminate before another
family can append a second gadget.

The table and direct family jumps are production-only. The correctness-first
interpreter runpath retains its decoded C++ bridge and original validation.
The exhaustive exception sweep and focused PC-relative, logical/bitfield,
pointer-auth, MTE, and SIMD suites caught and drove fixes for every overlap
before acceptance. The complete production and interpreter configurations
each pass **307/307** standard tests.

On a read-only tmpfs-overlay guest, five matched runs of 5,000 `/bin/true`
execs improved from a 26.509-second G7 median to 22.336 seconds, **15.7%
faster**. Three confirmation runs on the final frozen artifact had a
22.323-second median. G9 is retained. Its immutable executable is
`.build/bench/uml-darwin-g9-generated-decode`, SHA-256
`4dc3ca6eca7e5011906658523ec27a46be3e2fba4fb8ddf10df197cc3ca245d8`.
The reusable read-only guest command harness is
`tests/perf/run_guest_command_bench.py`.

### Final accepted-stack Geekbench confirmation

The complete G5+G6+G7+G9 confirmation is
(result). It scored
**71 single-core / 370 multi-core**, versus 65/324 on the accepted G3
comparison run: the aggregates improved **9.2% single-core** and
**14.2% multi-core**. Both runs used one 10-core guest processor, 8 GiB, the
read-only first partition plus tmpfs OverlayFS root, systemd, and the same
benchmark preparation. The final executable is the frozen G9 artifact,
SHA-256
`4dc3ca6eca7e5011906658523ec27a46be3e2fba4fb8ddf10df197cc3ca245d8`.

| Workload | G3 SC | G9 SC | Delta | G3 MC | G9 MC | Delta |
|---|---:|---:|---:|---:|---:|---:|
| File Compression | 82 | 91 | +11.0% | 359 | 420 | +17.0% |
| Navigation | 134 | 150 | +11.9% | 291 | 385 | +32.3% |
| HTML5 Browser | 54 | 62 | +14.8% | 396 | 478 | +20.7% |
| PDF Renderer | 73 | 78 | +6.8% | 362 | 375 | +3.6% |
| Photo Library | 66 | 70 | +6.1% | 450 | 483 | +7.3% |
| Clang | 43 | 47 | +9.3% | 290 | 313 | +7.9% |
| Text Processing | 38 | 43 | +13.2% | 47 | 53 | +12.8% |
| Asset Compression | 73 | 79 | +8.2% | 600 | 649 | +8.2% |
| Object Detection | 28 | 28 | +0.0% | 171 | 191 | +11.7% |
| Background Blur | 60 | 60 | +0.0% | 427 | 444 | +4.0% |
| Horizon Detection | 92 | 98 | +6.5% | 668 | 686 | +2.7% |
| Object Remover | 98 | 110 | +12.2% | 415 | 585 | +41.0% |
| HDR | 69 | 78 | +13.0% | 474 | 512 | +8.0% |
| Photo Filter | 87 | 91 | +4.6% | 198 | 245 | +23.7% |
| Ray Tracer | 75 | 89 | +18.7% | 686 | 831 | +21.1% |
| Structure from Motion | 63 | 67 | +6.3% | 384 | 453 | +18.0% |

The instrumented profile is
`/private/tmp/geekbench-g9-retry-profile`, with its console in
`/private/tmp/geekbench-g9-retry-console.log`. Against the G3 workload
profile, 30 of 31 comparable phase/workload wall times improved. The only
exception, single-core Background Blur, changed by +0.37%, while its scored
result tied at 60. Representative improvements were 24.2% for single-core
File Compression, 25.1% for multi-core Navigation, 40.2% for multi-core
Object Remover, and 14.0% for multi-core File Compression. The latter used
5.66 host cores versus G3's 5.83; multi-core Clang used 8.84 host cores.

An earlier G9 attempt was stopped after its console remained in multi-core
File Compression for approximately six minutes. The first process check used
`ps` lifetime-average CPU, which was not a valid workload-interval measure.
After the controlling terminal exited, the emulator also ignored SIGTERM and
remained as a one-core orphan until its exact PID was killed. Before retrying,
all retained artifacts passed the standalone 10-vCPU control at
9.70--9.99 host cores and the shared-mm stress at 2.50--2.74 host cores.
The clean retry used one-second interval telemetry, reproduced multi-core File
Compression in 56.5 seconds at 5.66 host cores, completed every workload, and
uploaded successfully. No source rollback is warranted for the non-reproduced
aborted-run anomaly.

### Rejected trial G10: two-instruction TLB hash

The production `translate` macro was changed from a three-instruction TLB
index sequence (`UBFX`, `EOR`, `AND`) to the mathematically equivalent
two-instruction sequence `EOR address, address, LSR #13` plus `UBFX #12,#13`.
The candidate passed a one-million-address equivalence test and all **308/308**
standard tests in both production and correctness-first interpreter builds.
Disassembly confirmed that the mask instruction was absent from each
production memory gadget.

The short 50-million-iteration mixed scalar/SIMD memory control was
inconclusive: the G9 and G10 medians were 1.037 and 1.034 seconds. The
ten-times-longer control rejected the candidate: seven G9 runs had a
**10.140-second** median while seven G10 runs had a **10.175-second** median,
so G10 was 0.35% slower at the same 1.08-host-core utilization. The alternate
dependency chain did not turn its lower instruction count into throughput.
All G10 runtime, ABI helper, build, and test changes were removed; the
independent Geekbench profile analyzer fix remains. The rejected executable
is retained only as `.build/bench/uml-darwin-g10-tlb-hash`, SHA-256
`bbc3605621a54f509e7ee5bf06340edcf812539466809ea41071215a6dc4fd25`.

### Accepted trial G11: entries-first gadget TLB

The 32 Geekbench workload profiles showed memory gadgets consuming 25--53%
of active emulator samples in most sections. G11 moves the 8,192 hot
`TlbEntry` objects to offset zero in `Tlb` and places generation/fault
metadata after the 256 KiB entry array. Production gadgets can consequently
form `tlb + index * 32` with one shifted `ADD` rather than a header-base `ADD`
followed by an indexed `ADD`. The hash, hit validation, miss handling, and
correctness-first interpreter algorithm are unchanged.

A separate ABI-layout test fixes every entry and metadata offset.
Disassembly confirms one entry-address `ADD` in the production load/store
path. Production and interpreter configurations each pass **308/308**
standard tests.

To avoid comparing different embedded-kernel build identifiers, the accepted
G9 source was rebuilt with the same toolchain immediately before measurement.
Seven long single-worker mixed scalar/SIMD memory runs improved from a
**10.186-second** G9 median to **10.072 seconds**, or **1.12%**. Five
10-worker runs were neutral within noise: 10.344 versus 10.358 seconds
(+0.14%), with 9.88 versus 9.87 host cores. G11 is retained under the
aggregate rule. The production build is byte-identical to the frozen
`.build/bench/uml-darwin-g11-tlb-entries-first`, SHA-256
`1240e249fc5d525724ebf84590d0a208fd934c0181a43d0283c3519c9623739e`.

### Accepted trial G12: one translation for same-page SIMD pairs

The per-workload profile identified `vector_pair_load128_0` as 29.7% of
single-core and 27.6% of multi-core Background Blur samples. The pair gadget
already compared the first and final guest byte and diverted page-crossing or
wrapping accesses to the atomic slow path, but then performed two identical
TLB hashes and tag checks for same-page transfers. G12 translates the first
guest address once and derives the second host address by adding the fixed
element width. It does not fuse guest instructions and does not change the
interpreter path.

The exhaustive SIMD pair suite covers all element widths, addressing modes,
signed displacements, page edges, cross-page accesses, writeback, invalid
duplicate destinations, and load/store fault atomicity. Production and
interpreter configurations each pass **308/308** standard tests. Production
disassembly confirms one TLB lookup in the hot pair gadget.

Seven 200-million-iteration vector-pair runs improved from **2.492 to 2.430
seconds** with one worker (**2.49%**) and five ten-worker runs improved from
**2.535 to 2.485 seconds** (**1.97%**). An unaffected scalar/single-vector
memory loop also improved slightly from 1.050 to 1.043 seconds, excluding a
general regression. G12 is retained as
`.build/bench/uml-darwin-g12-vector-pair-one-translation`, SHA-256
`89c607caa7b923ab553861fd347ece5d4b548542887d264c7745c478f5e8310a`.
The reusable focused source is `tests/perf/guest_vector_pair_loop.S`.

### Accepted trial G13: one translation for same-page integer pairs

The integer `LDP`/`STP` production gadgets had the same redundant same-page
TLB lookup as the SIMD pair gadgets. G13 preserves the existing first-to-last
byte page/wrap check and atomic cross-page slow path, translates the first
guest address once, and derives the adjacent host address by adding the
translation-time element width. The interpreter remains unchanged.

Production disassembly confirms one TLB lookup in the specialized integer
pair gadget. The exhaustive integer pair suite covers 32- and 64-bit
transfers, all addressing modes, XZR source/destination cases, signed
displacements, page boundaries, writeback constraints, and fault atomicity.
Production and interpreter configurations each pass **308/308** standard
tests.

Seven 200-million-iteration integer-pair runs improved from **2.142 to 2.132
seconds** with one worker (**0.47%**) and five ten-worker runs improved from
**2.170 to 2.157 seconds** (**0.60%**) at the same 9.81-host-core
utilization. G13 is retained as
`.build/bench/uml-darwin-g13-integer-pair-one-translation`, SHA-256
`24f5727e526dd5ee213ece2457e35e3af5d056e5accfb64fb0f4fe10ca84dee1`.
The reusable focused source is `tests/perf/guest_integer_pair_loop.S`.

### Accepted trial G14: native host SIMD pair transfers

After G12 removed the duplicate translation, a valid same-page guest SIMD
`LDP`/`STP` still executed an address `ADD` and two independent host
`LDR`/`STR` instructions. G14 emits the matching host `LDP`/`STP` directly
for 32-, 64-, and 128-bit elements. The existing page/wrap guard guarantees
that the combined host access stays within one validated mapping; cross-page
accesses and precise/atomic faults continue through the unchanged slow path.
This maps one guest instruction to one host instruction and does not fuse
separate guest operations.

The initial 200-million-iteration control was within launch noise: -0.25%
with one worker and +0.44% with ten. A five-times-longer steady-state control
resolved it decisively. Five single-worker runs improved from **12.175 to
11.987 seconds** (**1.54%**), and three ten-worker runs improved from **12.345
to 12.241 seconds** (**0.84%**) while reducing host CPU consumption.
Disassembly confirms one native `LDP q0, q1` in the hottest 128-bit load
gadget. Production and interpreter configurations each pass **308/308**
standard tests.

G14 is retained as
`.build/bench/uml-darwin-g14-vector-pair-native-ldp`, SHA-256
`61c48abb9635bf577f62606ce729e2a78625c208a4381bc6832739d7623382cc`.
`tests/perf/guest_vector_pair_loop.S` now accepts `BENCH_ITERATIONS` at
assembly time so short and extended controls use one checked source.

### Accepted trial G15: native host integer pair transfers

G15 independently maps same-page integer `LDP`, `STP`, and sign-extending
`LDPSW` guest instructions to their matching host pair instruction. It
removes the second host address `ADD` and one memory instruction without
changing translation, page/wrap guards, XZR specialization, writeback,
cross-page handling, or interpreter behavior.

Five one-billion-iteration single-worker runs improved from **10.537 to
10.355 seconds** (**1.73%**). Three ten-worker runs improved from **10.677 to
10.528 seconds** (**1.40%**) at the same 9.91-host-core utilization and with
lower host CPU consumption. Disassembly confirms one native `LDP x6, x7` in
the 64-bit load gadget. Production and interpreter configurations each pass
**308/308** standard tests.

G15 is retained as
`.build/bench/uml-darwin-g15-integer-pair-native-ldp`, SHA-256
`58f3717c6749d400238449678dd62ade64db55f2dd05a7a77d5eb5ea54a25627`.
`tests/perf/guest_integer_pair_loop.S` also accepts `BENCH_ITERATIONS` for
matched short and extended controls.

### Accepted trial G16: eliminate impossible scalar-byte page crossing

One-byte transfers cannot cross a 4 KiB page or wrap across an address
boundary, but every production scalar byte gadget still masked the page
offset, compared it with 4095, and conditionally branched to an unreachable
cross-page path. G16 uses assembly-time width specialization to omit those
three instructions and the dead slow-path body for unsigned-offset,
unscaled/pre/post-indexed, and register-offset scalar byte loads/stores.
Wider accesses are unchanged, TLB validation still protects the addressed
page, and the correctness-first interpreter retains its checks.

This targets the all-workload profile directly: indexed byte load alone was
18.7% of single-core Object Remover samples, while byte transfers were also
prominent in PDF Renderer, HDR, Horizon Detection, and Ray Tracer.
Disassembly confirms that both leading byte-load forms enter the TLB lookup
immediately after address formation, with no page-cross branch.

Five one-billion-iteration register-offset byte-transfer runs improved from
**10.190 to 10.101 seconds** with one worker (**0.87%**). Three ten-worker
runs improved from **10.289 to 10.199 seconds** (**0.87%**) at 9.90 host
cores. One candidate repetition suffered an unrelated host scheduling stall
at only 7.05 utilized cores; the other two candidate runs were 10.199 and
10.179 seconds, so the median remains representative. Production and
interpreter configurations each pass **308/308** standard tests.

G16 is retained as
`.build/bench/uml-darwin-g16-scalar-byte-no-cross-check`, SHA-256
`cff15cf2c67169102d05d421c83df36da5f3596341f7134ce071733be7a5e4a0`.
The reusable focused source is `tests/perf/guest_scalar_byte_loop.S`.

### Rejected trial G17: direct ADD/SUB-immediate GPR addressing

The hot ADD/SUB-immediate production gadget formed `CpuState + X0 offset`
before indexed source and destination accesses even though the X0 offset is
zero. G17 removed those two `ADD #0` instructions and addressed the array
directly. Targeted correctness tests passed and disassembly confirmed the
shorter gadget.

The short control was dominated by a roughly 50 ms scheduling quantum, so a
five-times-longer ADD/EOR control was used for the decision. Five G16 runs
had a **10.116-second** median; five G17 runs had a **10.152-second** median,
a repeatable **0.36% regression** at the same 1.10 host cores. Shrinking the
large generated gadget region shifted later hot logical entries, and the
layout effect outweighed the instruction reduction on this host. The runtime
change was reverted; rebuilding produces a byte-identical accepted G16
executable with SHA-256
`cff15cf2c67169102d05d421c83df36da5f3596341f7134ce071733be7a5e4a0`.
The rejected executable is retained only as
`.build/bench/uml-darwin-g17-add-sub-immediate-direct-gpr`.

### Accepted trial G18: inline immutable branch-link records

Translation/runtime accounts for roughly 20% of both Clang profiles.
`ProgramCache::link` previously allocated an `unordered_map` node, a separate
`LinkRecord` object, and stored a `unique_ptr` for every new direct branch
target. G18 constructs `LinkRecord` directly inside the node. Standard
unordered-map node references remain stable across rehash, so translated
streams retain the same stable link address while avoiding one allocation
and pointer chase per new target. The gadget execution hot path and link data
layout are unchanged.

Three matched 5,000-`/bin/true` single-worker runs improved from **20.062 to
19.854 seconds** (**1.04%**). Five matched runs distributing the same 5,000
execs over ten workers improved from **3.805 to 3.715 seconds** (**2.37%**)
and reduced host CPU consumption from about 33.9 to 32.7 seconds. Production
and interpreter configurations each pass **308/308** standard tests,
including direct-link, indirect-link, eviction, invalidation, and translation
coverage.

G18 is retained as `.build/bench/uml-darwin-g18-inline-link-records`,
SHA-256
`12f2a2b8f897723d01544812096b6d4cc4eb184b3c4ca833ec9fb199877943e1`.

### Accepted trial G19: inline indirect-link cells

G19 applies the same allocation cleanup independently to two-way indirect
branch/return link cells. Each `IndirectLink` now lives directly in its
unordered-map node instead of requiring a second `unique_ptr` allocation.
Node-stable addresses preserve every translated stream pointer, and the G6
two-entry PIC lookup/training hot path is byte-for-byte unchanged.

The effect is small but consistently non-negative. Seven parallel 5,000-exec
runs changed from **3.625 to 3.623 seconds** while reducing median
utilization from 8.83 to 8.79 host cores. Three single-worker runs changed
from **18.123 to 18.102 seconds** and reduced host CPU time from about 20.33
to 20.27 seconds. This is a noise-level **0.12% wall-time gain**, retained
because both controls reduce CPU work, neither regresses, and the change also
removes allocation/ownership complexity. Production and interpreter
configurations each pass **308/308** standard tests.

G19 is retained as `.build/bench/uml-darwin-g19-inline-indirect-links`,
SHA-256
`c77d9fb0959777368c61c647ceec83fafbfe396030d3e9ef5ba33353158c0ad4`.

### Rejected trial G20: pooled link-map nodes

G20 changed both inline link maps to PMR maps backed by the cache's existing
unsynchronized program-word pool, intending to recycle nodes across
fork/exec generation invalidations. Targeted link/translation tests passed.
The matched 5,000-exec single-worker control instead regressed from a
**19.176-second** G19 median to **19.423 seconds**, or **1.29%**, with no
host-CPU improvement. Mixing these larger node size classes into the word
pool costs more than the general allocator on this host. The PMR map change
was removed; the rebuilt executable is byte-identical to accepted G19. The
rejected artifact remains only as
`.build/bench/uml-darwin-g20-pooled-link-maps`, SHA-256
`85fff94459822611e1722dee173f85b393e10e8e6819efc4873548309d3806d6`.

### Rejected trial G21: 16K linked-chain checkpoint

G21 raised the direct-link engine checkpoint from 4,096 to 16,384 guest
instructions, intending to reduce `uml_arm64_gadget_leave` overhead observed
in the multi-core HDR profile while retaining four interrupt checks within
the dispatcher's 64K fairness window. A focused boundary test verified the
new exact 16,384-instruction exit, and all link, hot-path, and FP-environment
tests passed.

The extended one-billion-iteration controls did not establish a useful
single-core gain. Reverse-order FP/SIMD trials were effectively identical
(**8.805 versus 8.804 seconds**), while integer trials were also flat
(**7.785 versus 7.781 seconds**). Ten-worker integer throughput improved only
**0.15%** and the first FP series regressed before reverse-order repetitions
showed that result was run-order noise. Because the longer latency between
interrupt checks buys no repeatable throughput improvement, G21 was rejected
and the 4K checkpoint restored. The rejected artifact remains only as
`.build/bench/uml-darwin-g21-16k-link-checkpoint`, SHA-256
`4c4cc22c41c9cfcb32f67ca5eceb81ccaced28d732353167e89b9363e0c4963d`.

### Rejected trial G22: lightweight production builder callbacks

G22 targeted Clang's approximately 20% translation/runtime share by passing
direct function-pointer/context pairs from `ProgramCache` into the production
builder instead of constructing two capturing `std::function` wrappers. The
ordinary test API and generated decoder were unchanged, and the seven focused
translation, direct/indirect-link, NZCV, FP-environment, and gadget tests all
passed.

The initial 5,000-exec series appeared 7.7% faster, but reverse-order
measurement proved that gap was host-state drift: G22 and G19 medians were
**19.220 and 19.245 seconds**, only **0.13%** apart, with the same effective
host utilization. The additional callback ABI is not justified by a
noise-level result, so G22 was removed. Its rejected artifact remains only as
`.build/bench/uml-darwin-g22-fast-builder-callbacks`, SHA-256
`805e5b98fd4e04f869d85b4f60125274fd57d4448f746085403b094c5e413d66`.

### Rejected trial G23: combined scalar page/TLB checks

G23 tested two ways to reduce the separate page-cross and TLB-tag decisions
in the dominant 32/64-bit scalar memory gadgets. The first used the unsigned
identity `address - cached_page <= 4096 - width` to prove both properties in
one range check, removing three hot instructions. It passed scalar immediate,
cross-page, static-gadget, and hot-path tests, but the long memory control
regressed from **9.959 to 9.983 seconds** because the subtraction extended the
loaded-tag dependency chain.

G23b retained the original tag comparison and replaced only the early
cross-page branch with `CCMP`, moving the distinction to the cold failure
path. Disassembly confirmed one fewer hot conditional branch, but the flags
dependency serialized the lookup and decisively regressed the same control to
**10.238 seconds** (**2.8%**). Both variants were rejected and the independent
checks restored. The rejected artifacts are
`.build/bench/uml-darwin-g23-bounded-tlb-check`
(`2f2ce2b9e8475a36ba7a997f0fb043bb98a81e50e3b3ee5a256a88e2002b40c3`)
and `.build/bench/uml-darwin-g23b-ccmp-tlb-bound`
(`118467b246362078e9b6a3ebee132f08c2a98131d928ecab4741b2680cc419ec`).

### Accepted trial G24: predecoded one-table SIMD lookup offsets

The complete workload matrix identified the one-table `TBL` gadget as 9.0%
of single-core and 9.4% of multi-core Structure from Motion. G24 moves the
three guest-vector address calculations into translation for the one-table
`TBL`/`TBX` forms. Their stream word now carries three absolute 16-bit
`CpuState` vector offsets, allowing the production gadget to load and store
directly from `_cpu` without constructing the vector-bank base and three
scaled addresses at runtime. Multi-table forms retain their original compact
descriptor because their consecutive table-register list can wrap from V31
to V0.

Disassembly confirms that the valid one-table hot path removes four runtime
address-generation instructions and adds no branch. A one-billion-iteration
single-worker `TBL` control improved from **8.801 to 8.597 seconds**
(**2.32%**) and a 300-million-iteration ten-worker control improved from
**2.749 to 2.659 seconds** (**3.27%**) while sustaining 9.84 effective host
cores. The untouched two-table control was non-regressing at **8.971 versus
8.928 seconds**. Focused static-gadget, FP-environment, allocation, and
table-behavior tests passed, followed by all **307/307** production behavior
tests and **307/307** interpreter tests. The separate source-tree language
audit now discovers unrelated C sources in the user-added `external/` vendor
trees; no emulator behavior test failed.

G24 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g24-predecoded-tbl1-offsets`, SHA-256
`c9d801c02f801bda3e08a6b1b76d076e4dabb79b845b68ea8bbdb48463299dc3`.

### Accepted trial G25: predecoded indexed-FP vector offsets

Background Blur's three specialized indexed `FMLA.4S` lane gadgets account
for 25.3% of single-core and 22.5% of multi-core active samples. The same
family's indexed `FMUL.4S` gadget adds another 5.1% and 4.8%, respectively.
G25 changes the translated operand for all indexed floating-point forms to
carry absolute 16-bit destination, first-source, and element-source offsets
within `CpuState`. Production gadgets now use those offsets directly in
native vector loads and stores, removing the vector-bank base plus three
scaled address constructions. The operation, arrangement, lane, and
scalar/vector choice remain selected entirely at translation time.

The one-billion-iteration indexed `FMLA.4S` control improved from **8.768 to
8.568 seconds** (**2.28%**). Its 300-million-iteration ten-worker counterpart
improved from **2.701 to 2.648 seconds** (**1.96%**) while utilization rose
slightly from 9.83 to 9.86 effective host cores. G24's unrelated one-table
lookup control remained flat at **8.597 versus 8.603 seconds**. All
**307/307** production behavior tests and **307/307** interpreter tests
passed.

G25 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g25-predecoded-indexed-fp-offsets`, SHA-256
`ad399afb028484a858ed8d4c179d8fbc115de691efe46735dd3746108ae6c711`.

### Accepted trial G26: predecoded basic vector-FP offsets

Basic vector `FADD.4S` contributes 3.0% of single-core Asset Compression,
5.3% of single-core Background Blur, and similar shares in their multi-core
phases. Vector `FMUL` and `FSUB` in the same family appear in Asset
Compression and Structure from Motion. G26 gives the vector-only basic
floating-point gadgets the same translation-time absolute-offset encoding as
G25. Scalar forms retain their original register descriptor and NEP handling.
Each binary vector gadget eliminates the vector-bank base and three scaled
address calculations; unary vector forms eliminate the base and two scaled
addresses.

The one-billion-iteration `FADD.4S` control improved from **9.165 to 8.597
seconds** (**6.20%**). Its 300-million-iteration ten-worker control improved
from **2.802 to 2.670 seconds** (**4.71%**) with 9.79 effective host cores.
The reverse-order indexed-FMLA control was flat at **8.633 versus 8.640
seconds**. All **307/307** production behavior tests and **307/307**
interpreter tests passed.

G26 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g26-predecoded-basic-vector-fp-offsets`, SHA-256
`76d29510298e396094560586516a877e45d33882357d8b70259e724098241d70`.

### Accepted trial G27: predecoded three-same SIMD offsets

Object Detection spends 4.5% of its single-core and 3.3% of its multi-core
active samples in the native same-width `ADD.4S` gadget. G27 predecodes the
three vector offsets for the complete integer three-same and RDM families.
Their production gadgets now issue native loads and stores directly relative
to `CpuState`, eliminating the vector-bank base and three scaled address
calculations without adding a runtime decision.

The one-billion-iteration `ADD.4S` control improved from **8.599 to 8.481
seconds** (**1.37%**). The 300-million-iteration ten-worker control improved
from **2.666 to 2.636 seconds** (**1.13%**) while holding 9.83 effective host
cores. G26's unrelated vector-FADD control remained non-regressing. All
**307/307** production behavior tests and **307/307** interpreter tests
passed.

G27 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g27-predecoded-three-same-offsets`, SHA-256
`d956a7de2ddea9049b18d8daaac18b31f365eb872d3de60b7d4d8ea1e994cf92`.

### Rejected trial G28: predecoded pairwise-long SIMD offsets

G28 applied a two-offset stream format only to the pairwise-long
`SADDLP`/`UADDLP`/`SADALP`/`UADALP` gadgets, targeting Object Detection's
5.7% single-core `SADDLP.8H` share. This safely removed one address-generation
instruction and passed the focused two-register, reduction, and static-gadget
tests. However, matched one-billion-iteration runs of the exact
`SADDLP Vd.4S, Vn.8H` form produced identical **8.516-second** medians. The
extra translation encoding is not justified by a zero throughput result.

G28 was rejected and the current production executable is byte-identical to
G27. Its rejected artifact remains only as
`.build/bench/uml-darwin-g28-predecoded-pairwise-long-offsets`, SHA-256
`0ceca44e6dc056bf80f2f9eaee55a3a5740a5d79e8037f90373ee69de5bf1fe8`.

### Accepted trial G29: predecoded widening and high-narrow SIMD offsets

Object Detection spends 4.4% of its single-core and 3.7% of its multi-core
active samples in the native `SMULL.8B`/`SMULL2.16B` widening gadgets. G29
predecodes all three vector-register offsets for the shared widening gadget
family. The high-narrow family uses the same assembly macro, so its translator
now emits the matching absolute-offset descriptor as well. The runtime path
eliminates the vector-bank base and four address-building instructions without
adding a conditional branch.

Matched 300-million-iteration `SMULL.8B` runs improved from **2.642 to 2.573
seconds** (**2.61%**) for one worker and from **2.717 to 2.635 seconds**
(**3.02%**) for ten workers. Both ten-worker medians sustained 9.84 effective
host cores. The unrelated `ADD.4S` control remained exactly flat at **2.568
seconds**. All **307/307** production behavior tests and **307/307**
correctness-first interpreter tests passed.

G29 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g29-predecoded-widening-offsets`, SHA-256
`69ded38f676ac5e1a5984c39d20384000475fcd12df72604615d26f8a16eb0b2`.

### Accepted trial G30: dispatch-scoped native FP environment

Multi-core HDR was the only profiled workload with a large engine-leave
share: 12.8% of active emulator samples. The old leave path serialized four
native system-register operations on every internal translated-block exit:
it committed guest FPSR and restored host FPCR/FPSR even though the next block
usually selected the same guest environment again.

G30 retains guest FPCR/FPSR in the native registers across internal production
dispatches and restores the host environment only at the outer guest-entry
boundary. A `CpuState` flag makes the steady FP gadget path one load plus one
predicted branch; first-use activation is in one shared cold routine rather
than duplicated into every gadget. Direct `gadgets::execute` calls and the
correctness-first interpreter still restore on every call. System-register
writes update the active native state immediately, and the outer restore
commits FPSR before copying the guest boundary state.

The first layout moved the large SIMD gadget bank to a different cache-line
phase and caused an apparent integer-control regression. Restoring its
established 64-byte phase removed that effect: matched `ADD.4S` medians are
**2.601 seconds** for G30 and **2.604 seconds** for G29. The ordinary linked
FADD control is **2.600 versus 2.582 seconds** for one worker (a bounded
**0.70% regression**) and **2.641 versus 2.664 seconds** for ten workers (a
**0.86% gain**).

The exit-heavy polymorphic FP control exposes the intended multicore effect.
Seven ten-worker repetitions improved from a **4.343-second**, **7.25-core**
G29 median to **2.998 seconds** and **9.73 cores** with G30: **31.0% less wall
time**. Its one-worker median changed from **2.900 to 2.924 seconds**
(**0.83% slower**), so the change is accepted for its substantial multicore
gain under the revised aggregate rule. All **307/307** production and
**307/307** interpreter tests pass; the split FP-environment test verifies
both dispatch-scoped preservation and exact host restoration.

G30 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g30c-dispatch-fp-environment-aligned`, SHA-256
`547aa0dbe03e80e825c70face32d6ba9e7f225384b610f0fc02cf737626405c5`.

### Accepted trial G31: single link-map lookup

Clang is the profile's cold-translation outlier: `Builder`, `ProgramCache::link`,
and `run_block` together account for roughly one fifth of its active samples.
The direct-link miss path formerly called `find` and then `try_emplace`, hashing
the same PC twice. G31 adds a private `link_record` helper which performs one
`try_emplace`; direct links, indirect links, and cache activation initialize
the returned record only when insertion actually occurred. This changes no
translated-program format and adds nothing to the assembly hot path.

The ten-worker `/bin/true` fork/exec control improved from **3.149 to 3.120
seconds** (**0.92%**) and reduced host CPU time from **28.12 to 27.84
seconds**. A matched reverse-order single-worker pair was **18.266 seconds**
for G31 versus **18.221 seconds** for G30 (**0.25% slower**), while host CPU
time fell from **20.22 to 20.05 seconds**. The change is retained under the
aggregate rule because it improves the multicore Clang-shaped workload and
reduces CPU consumption without adding any steady translated-code cost.

G31's frozen executable is
`.build/bench/uml-darwin-g31-single-link-map-lookup`, SHA-256
`fccb6ca08e200959cc005ed388ae58d72837d7ed9772d5d627562d391b6bbbab`.

### Accepted trial G32: bounded initial program reserve

Every cold `Builder` previously reserved enough word storage for the
worst-case expansion of the entire 64-instruction fetch window: 130 words,
even when a branch or fault ended the block much earlier. G32 keeps the exact
same growth behavior but caps the initial reserve at **34 words**, avoiding
most excess cold allocation and zeroing while leaving normal short programs
allocation-free.

Matched single-worker `/bin/true` fork/exec runs improved from **17.826
seconds** for G31 to **16.686 seconds** for G32 (**6.39%**). The reverse-order
ten-worker control improved from **3.113 to 3.093 seconds** (**0.64%**) and
reduced host CPU time from **27.77 to 27.26 seconds**. A reserve sweep rejected
18, 26, and 50 words: 34 words was the repeatable local optimum, including
reverse-order comparisons which separated it from host-frequency drift.

All **307/307** production tests and **307/307** correctness-first interpreter
tests pass with the final G31+G32 source. G32's frozen executable is
`.build/bench/uml-darwin-g32-bounded-program-reserve`, SHA-256
`6c2d64a38f8b3b78bcd0f9b4181fa4102dd02c86f9222effffaa9ed55e0affa8`.

### Rejected trials G33/G33b/G33c: cached active-link pointers

G33 stored the active direct-link record in each fixed-cache slot so cache
hits could avoid a second map lookup. Although its ten-worker fork/exec
control improved by roughly 0.95%, the enlarged slot stride regressed the
single-worker control by **3.1%**. G33b and G33c moved the pointer into a
separate side array, including a version without redundant initialization.
Alternating single-worker pairs showed a small apparent gain, but the decisive
ten-worker control regressed from **3.221 to 3.274 seconds** (**1.65%**).

All three variants were rejected and their source removed. Their diagnostic
artifacts remain under `.build/bench/` only; the production source and binary
returned exactly to G32.

### Rejected trial G34: retained direct-link map nodes

G34 retained up to 32,768 direct-link map nodes across address-space
generations and lazily reset each node through an epoch field. The intent was
to avoid allocator and hash-node churn during fork/exec-heavy workloads.
Instead, the single-worker control regressed from **16.958 to 17.061
seconds** (**0.61%**) and the ten-worker control from **3.171 to 3.234
seconds** (**2.0%**). The extra epoch state and larger retained working set
outweighed allocation reuse.

G34 was rejected and completely removed. Its rejected artifact remains only
as `.build/bench/uml-darwin-g34-retained-link-nodes`; the rebuilt production
executable is byte-identical to frozen G32.

### Accepted trial G35: branch-free logical MOV alias

The all-workload profile identified `ORR Rd, ZR, Rm`—the integer register MOV
alias—as 3.7% to 6.4% of active samples in Photo Library, HTML5 Browser, Clang,
and Text Processing. Although translation already selected a dedicated
logical-register entry, its runtime still materialized the zero left operand,
executed the redundant ORR, and recalculated the unchanged `CpuState` register
base before the result store.

G35 makes the existing entry's translation-time constants effective in its
assembly body. The 64-bit alias loads the source and stores it directly; the
32-bit alias performs the required zero-extension and stores the result.
Every result-writing logical-register gadget also reuses its live register-bank
base. No stream word, opcode check, conditional branch, or new gadget entry is
added. The hot 64-bit MOV entry falls from **11 to 8 host instructions**.
Unreachable padding after each indirect dispatch preserves every later
logical gadget at its G32 address and cache-line phase.

The exact 1.6-billion-MOV control improves from **2.431 to 1.816 seconds**
(**25.3%**) for one worker and from **2.528 to 1.893 seconds** (**25.1%**) for
ten workers while sustaining 9.82 effective host cores. The mixed ADD/EOR
non-alias control is neutral/slightly faster at **9.854 versus 9.861
seconds**. The existing exhaustive logical-register matrix covers 32- and
64-bit MOV aliases, including 32-bit zero-extension from a source with set
upper bits. All **307/307** production tests and **307/307** correctness-first
interpreter tests pass.

G35 is accepted. Its frozen executable is
`.build/bench/uml-darwin-g35-logical-move-alias-aligned`, SHA-256
`55bd43f8c83b09dadd522da25f28620386b029fd59835fa632000e7536eb00ac`.

#### Full G35 LTP and Geekbench confirmation

The 2026-07-26 from-scratch non-pressure LTP requalification selected 2,141
ordinary cases and one serial timing case while explicitly skipping all 374
`host-pressure` entries. It completed **2,141/2,142 accepted in 1,324.5
seconds**. The sole timeout, `futex_wake02`, had already emitted all eleven
TPASS assertions and stalled during teardown; after one isolated reproduction,
the unchanged G35 executable passed nine consecutive focused reruns. The full
run is still recorded conservatively as 2,141/2,142. See `LTP_PROGRESS.md` for
the controls and exact disposition.

The matching 10-core, 8 GiB, read-only-lower OverlayFS Geekbench 6 run
completed in **3,302 seconds** and uploaded as
(result). It scored **79 single-core**
and **416 multi-core**, versus the pre-G35 G9 result
(result) at **71 / 370**. That is an
aggregate improvement of **11.3% single-core** and **12.4% multi-core**.
Thirty-one of 32 section scores improved:

| Workload | G9 SC | G35 SC | Delta | G9 MC | G35 MC | Delta |
|---|---:|---:|---:|---:|---:|---:|
| File Compression | 91 | 102 | +12.1% | 420 | 448 | +6.7% |
| Navigation | 150 | 161 | +7.3% | 385 | 466 | +21.0% |
| HTML5 Browser | 62 | 71 | +14.5% | 478 | 515 | +7.7% |
| PDF Renderer | 78 | 82 | +5.1% | 375 | 439 | +17.1% |
| Photo Library | 70 | 78 | +11.4% | 483 | 532 | +10.1% |
| Clang | 47 | 55 | +17.0% | 313 | 369 | +17.9% |
| Text Processing | 43 | 48 | +11.6% | 53 | 58 | +9.4% |
| Asset Compression | 79 | 85 | +7.6% | 649 | 716 | +10.3% |
| Object Detection | 28 | 33 | +17.9% | 191 | 218 | +14.1% |
| Background Blur | 60 | 71 | +18.3% | 444 | 517 | +16.4% |
| Horizon Detection | 98 | 102 | +4.1% | 686 | 719 | +4.8% |
| Object Remover | 110 | 128 | +16.4% | 585 | 664 | +13.5% |
| HDR | 78 | 102 | +30.8% | 512 | 636 | +24.2% |
| Photo Filter | 91 | 101 | +11.0% | 245 | 239 | -2.4% |
| Ray Tracer | 89 | 98 | +10.1% | 831 | 924 | +11.2% |
| Structure from Motion | 67 | 76 | +13.4% | 453 | 549 | +21.2% |

The only regression is multi-core Photo Filter, **245 to 239 (-2.4%)**.
Observed host utilization confirms that representative multi-core sections
now scale across the guest topology: File Compression used about 8.1 cores,
Navigation about 8.5, and Clang 9.7--9.9. Text Processing remains the known
workload-specific exception at about 1.1 host cores. DNS and Java prechecks
passed, the upload succeeded, the guest powered off cleanly, and no emulator
process remained.

### Rejected trial G36: reused ADD-immediate register-bank base

Ordinary 64-bit ADD-immediate is the largest non-memory symbol after G35 in
the aggregate 32-section profile. G36 kept its source-load `CpuState` base
live through the native addition and reused it for the result store, removing
one runtime instruction without changing layout or adding a branch.

The exact 1.6-billion-ADD control found no repeatable benefit. Two surrounding
single-worker G35 baselines had **3.381** and **3.437-second** medians, whose
**3.409-second** average matches G36's **3.411 seconds**. The first ten-worker
pair appeared to favor G36 at **3.510 versus 3.544 seconds**, but an immediate
reverse-order G35 run was **3.507 seconds** with the same 34.56–34.58 host-CPU
seconds. This is host-frequency and scheduling variance, not an improvement.

G36 was rejected and the runtime change removed. Rebuilding returns exactly to
frozen G35; `guest_add_immediate_loop.S` remains as a standalone diagnostic
control.

### Geekbench resume checklist

- [x] Correct the guest topology from one core / 20 SMT threads to 20
  independent cores.
- [x] Verify Geekbench `--sysinfo` reports `1 Processor, 20 Cores`.
- [x] Keep `rootfs.img` partition 1 read-only under the tmpfs OverlayFS root,
  use 8 GiB memory and systemd; use the user-selected 10-vCPU topology for
  current benchmark runs.
- [x] Stop Docker, containerd, apt timers, and Time Machine before each run.
- [x] Capture representative scalar/control-flow and FP/SIMD profiles.
- [x] Run the standalone SMP topology/parallelism integration test on an idle
  host.
- [x] Complete a full topology-corrected functional/profile run. All workloads
  finished in 4h06m51s; the result upload failed before scores were
  printed.
- [x] Diagnose the failed upload and harden the tmpfs overlay resolver with
  rotated public/public/virtual fallbacks; validate 2,000 rapid lookups.
- [x] Stop a contaminated scored retry after detecting a full-core
  `ReportCrash` loop and a leftover diagnostic emulator.
- [x] Start the user-selected 10-core baseline at epoch 1784942167, reach
  Multi-Core PDF Renderer, capture the live low-utilization profile, and stop
  the run early as explicitly requested instead of waiting for an exact
  baseline score.
- [x] Diagnose the dominant multi-core serialization, retain bounded disjoint
  TLB synchronization ranges, add direct unit and standalone integration
  coverage, and validate the greater-than-236x focused A/B improvement.
- [x] Strengthen the standalone SMP integration control to run one busy
  process per vCPU; the accepted 10-vCPU build consumes 38.18 host-CPU seconds
  in 4.00 seconds, or 9.54 host cores.
- [x] Profile the accepted build and reduce walker-lock waiting from 59.1% in
  the observed Geekbench carriers to 1.78% in the focused shared-mm carriers.
- [x] Accept G1 after matched focused link-loop A/B runs: 0.42% faster with one
  worker and 0.94% faster with ten workers, while removing three hot-path
  instructions per resolved linked edge.
- [x] Accept G2 after matched focused FP/SIMD-link A/B runs: 41.9% faster with
  one worker and 39.1% faster with ten workers, no integer-control regression,
  303/303 tests in each execution mode, and a 9.97-core SMP control.
- [x] Complete one full 10-core Geekbench confirmation on the accepted
  shared-mm plus G1/G2 build and retain its result URL plus every sub-test
  score. The `--multi-core` shortcut cannot be used.
- [x] Profile the remaining mapping-batch serialization, implement G3's
  additive publication fast path, pass 304/304 tests in production and
  interpreter modes, and retain its 5.9% multi-core aggregate improvement.
- [x] Profile any regressed sub-tests and try focused corrections before
  rejecting an aggregate improvement.
- [x] Reject G4's per-site L0 TLB hint after a 1.10% decisive-control
  regression and restore the accepted G3 source.
- [x] Accept G5's resident-NZCV path after 19.5% and 7.5% focused gains plus
  complete production/interpreter validation.
- [x] Accept G6's two-way indirect branch/return links after a 44.1% focused
  gain, neutral unaffected controls, and 306/306 tests in both modes.
- [x] Accept G7's production FP-environment hoist after a 1.1% focused gain
  and 307/307 tests in both execution modes.
- [x] Reject G8's wider predecoded GPR offsets after a decisive 0.06%
  regression, then restore a byte-identical G7 production executable.
- [x] Accept G9's generated production top-level decoder after a 15.7%
  exec/startup gain and 307/307 tests in both execution modes.
- [x] Complete the final 10-core G5+G6+G7+G9 Geekbench confirmation at
  71 single-core / 370 multi-core, retain its result URL and profile, and
  verify every scored subtest improved or tied against G3.
- [x] Reject G10's two-instruction TLB hash after the long focused control
  regressed 0.35%, and remove its runtime/test changes.
- [x] Analyze all 32 G9 per-workload samples and rank workload-local
  bottlenecks in `GEEKBENCH_WORKLOAD_PROFILE.md`.
- [x] Accept G11's entries-first TLB after a 1.12% long single-worker memory
  gain, neutral ten-worker control, and 308/308 tests in both modes.
- [x] Accept G12's same-page SIMD-pair translation reuse after 2.49%
  single-worker and 1.97% ten-worker gains plus 308/308 tests in both modes.
- [x] Accept G13's same-page integer-pair translation reuse after 0.47%
  single-worker and 0.60% ten-worker gains plus 308/308 tests in both modes.
- [x] Accept G14's native host SIMD `LDP`/`STP` mapping after 1.54%
  single-worker and 0.84% ten-worker extended-control gains plus 308/308
  tests in both modes.
- [x] Accept G15's native host integer `LDP`/`STP`/`LDPSW` mapping after
  1.73% single-worker and 1.40% ten-worker gains plus 308/308 tests in both
  modes.
- [x] Accept G16's scalar-byte assembly-time no-cross specialization after
  matching 0.87% single-worker and ten-worker gains plus 308/308 tests in
  both modes.
- [x] Reject G17's direct ADD/SUB-immediate GPR addressing after the extended
  focused control regressed 0.36%, then restore a byte-identical G16 build.
- [x] Accept G18's inline branch-link records after 1.04% single-worker and
  2.37% ten-worker fork/exec gains plus 308/308 tests in both modes.
- [x] Accept G19's inline indirect-link cells after non-regressing single-
  and ten-worker controls, lower host CPU use, and 308/308 tests in both
  modes.
- [x] Reject G20's PMR-backed link maps after a 1.29% single-worker exec
  regression, then restore a byte-identical G19 build.
- [x] Reject G21's 16K linked-chain checkpoint after extended integer and
  FP/SIMD controls showed no repeatable single-core gain; restore the 4K
  interrupt/fairness boundary.
- [x] Reject G22's lightweight builder callbacks after reverse-order
  fork/exec controls reduced the apparent gain to 0.13%.
- [x] Reject both G23 combined page/TLB checks after dependency-chain
  regressions of 0.24% and 2.8%, then restore independent predicted branches.
- [x] Accept G24's predecoded one-table `TBL`/`TBX` vector offsets after
  2.32% single-worker and 3.27% ten-worker target gains, a non-regressing
  two-table control, and 307/307 production plus interpreter behavior tests.
- [x] Accept G25's predecoded indexed-FP vector offsets after 2.28%
  single-worker and 1.96% ten-worker indexed-FMLA gains, an unaffected
  one-table control, and 307/307 production plus interpreter tests.
- [x] Accept G26's predecoded basic vector-FP offsets after 6.20%
  single-worker and 4.71% ten-worker FADD gains, a flat reverse-order
  indexed-FMLA control, and 307/307 production plus interpreter tests.
- [x] Accept G27's predecoded three-same SIMD offsets after 1.37%
  single-worker and 1.13% ten-worker ADD gains, stable multicore utilization,
  and 307/307 production plus interpreter tests.
- [x] Reject G28's predecoded pairwise-long offsets after the exact
  SADDLP.8H-source control remained identical at 8.516 seconds; restore a
  byte-identical G27 executable.
- [x] Accept G29's predecoded widening and high-narrow SIMD offsets after
  2.61% single-worker and 3.02% ten-worker SMULL gains, a flat unrelated
  ADD.4S control, and 307/307 production plus interpreter tests.
- [x] Accept G30's dispatch-scoped native FP environment after removing its
  cache-layout regression, improving the exit-heavy ten-worker control by
  31.0%, and passing 307/307 production plus interpreter tests.
- [x] Accept G31's single-lookup link insertion after a 0.92% ten-worker
  fork/exec gain, lower host CPU use, and no assembly hot-path growth.
- [x] Accept G32's 34-word bounded initial Builder reserve after a 6.39%
  single-worker and 0.64% ten-worker fork/exec gain; reject the 18-, 26-, and
  50-word alternatives and pass 307/307 tests in both execution modes.
- [x] Reject G33/G33b/G33c active-link pointer caching after either a 3.1%
  single-worker regression or a 1.65% ten-worker regression, and remove all
  variants.
- [x] Reject G34 retained link-map nodes after 0.61% single-worker and 2.0%
  ten-worker regressions, then restore a byte-identical G32 executable.
- [x] Accept G35's branch-free logical MOV alias after 25.3% single-worker
  and 25.1% ten-worker target gains, a neutral non-alias control, exact
  cache-phase preservation, and 307/307 tests in both execution modes.
- [x] Reject G36's ADD-immediate base reuse after reverse-order scalar and
  ten-worker controls reduced the apparent gain to measurement variance, then
  restore the byte-identical G35 executable.
- [x] Remove every rejected/unused gadget and finish with production plus
  interpreter validation; after G3, both standard configurations pass
  304/304 tests and G3 adds no gadget entry.

## Pause and resume checklist

Work paused on 2026-07-23 after the initial trial 3 rejection because the host
was scheduled to reboot.

Work resumed on 2026-07-24. Trial 3 was restored under the revised acceptance
rule, profiled, and accepted after its complete confirmation run.

- [x] Trial 1 measured, rejected, and reverted.
- [x] Trial 2 measured, rejected, and reverted.
- [x] Trial 3 corrected, profiled after reboot, confirmed with the revised
  aggregate acceptance rule, and retained.
- [x] Accepted comparison point restored to 239.2 / 819.6 / 442.773.
- [x] Reboot completed; no benchmark session survived.
- [x] Confirm `rootfs.img` is still 8,589,934,592 bytes with mtime 1784725767;
  verify SHA-256
  `9d1398970ea078dcef37bed42ed5eba6ba132f53cca6849420a3f8f188f510ba`
  before resuming benchmark work.
- [x] Rebuild `.build/cmake-macos` and run the 298-test standard Release CTest
  suite to confirm the reverted accepted source.
- [x] The old benchmark initramfs was removed by reboot. The equivalent
  benchmark boot now mounts partition 1 read-only and constructs a tmpfs upper
  overlay from a PID-1 rescue shell before entering the overlay-root chroot.
- [x] Complete trial 3 confirmation and retain it if its composite score is
  still improved; investigate rather than automatically reject isolated
  sub-test regressions.
- [x] Resume with **trial 4**, data-only cached-block successor links. Do not
  repeat trials 1–3.
- [x] Trial 4 passed 299/299 tests, completed the final production benchmark,
  was profiled around its isolated 4 KiB File Copy regression, and was retained
  for its improved composite.
- [x] Trial 5 used a measured hot-gadget profile and initially improved the
  composite, then was fully reverted by request on 2026-07-24 because the
  0.83% gain was marginal and workload-dependent. The P5-free build passes
  301/301 standard tests.
- [x] Trial 6 removed repeated large boundary-object initialization, repaired
  FPMR/SVCR and PSTATE persistence, added a split boundary-state test, passed
  301/301 tests, and was retained after profiling its small Execl regression.
- [x] Trial 7 batched translation fetches, pooled program-word storage, added a
  split cross-page translation test, passed 302/302 tests, recovered Execl,
  and was retained after profiling and repeating its regressed sub-tests.
- [x] Trial 8 swept 32K/64K caches and a 128-instruction block, rejected every
  candidate from matched UnixBench results, stopped before 256 after a
  significant Dhrystone downgrade, and restored 16K/64.
- [x] Trial 9 moved UBD host I/O to a serial Dispatch queue, passed queue/unit
  and writable scratch-UBD checks, completed UnixBench, fixed the discovered
  errno and per-request signal-mask issues, profiled/repeated the regressed
  selectors, and reverted the neutral-to-negative candidate.
- [x] Trial 10 mapped selected guest NEON registers to persistent host vector
  registers, passed split production/interpreter tests and completed
  UnixBench, but profiling found effectively no workload hits; its marginal
  result was rejected and all candidate gadgets/tests were removed.
- [x] Trial 11 batched contiguous sparse-MMU lookups and used native NEON for
  page-address generation, passed its split tests and the complete production
  suite, completed UnixBench, and was reverted after profiles showed limited
  causal use and the composite regressed 0.52%.
- [x] For every optimization trial, require correctness plus an improved
  composite. Profile isolated regressions and attempt targeted fixes instead of
  automatically rejecting an overall-improving proposal. Require the full
  UnixBench exit and child-completion markers before deciding.
- [x] Run the final accepted-source production/interpreter validation:
  301/301 tests pass in each configuration. The dead-gadget audit compared
  all 8,642 assembly entries with production references and internal
  relocations and found no unused entry.
- [x] Confirm the final benchmark handoff against the accepted P5-free
  305.0 / 976.9 / 545.852 run and record the immutable-rootfs evidence.

Temporary benchmark logs from this session are
`/private/tmp/unixbench-priority1.log`,
`/private/tmp/unixbench-priority2.log`, and
`/private/tmp/unixbench-priority3-repeat.log`; later trial logs are named in
their trial sections and may not survive reboot.
