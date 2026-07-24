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
