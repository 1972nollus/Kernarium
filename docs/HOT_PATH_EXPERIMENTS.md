# Gadget hot-path experiments

This log tracks the requested optimization trials in priority order.
Each trial is implemented in isolation on top of the last accepted build,
validated with the production Release gadget configuration, and then measured
with a complete UnixBench 6.0.1 run.

Artifact hashes and byte-identity statements below are historical observations,
not current workflow requirements. Git is the source authority. Current A/B
work uses separate build directories from the same checkout, records the
revision/options/commands/raw measurements, and does not snapshot or seal
sources, hash products or objects, prove object-level isolation, or relink to
manufacture binary equality. Toolchain-generated timestamps, versions, UUIDs,
build IDs, and tool identities may vary. EROFS images are used directly without
per-run pre/post hashing.

## Current campaign disposition (2026-08-10)

The accepted implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97` retains G99 gadget ThinLTO, G100's
exactly five audited decode-family `.ipp` fragments, G121's published
per-thread lifecycle slots, and G123's forward Darwin mapping repair. G123's
point-in-time Linux tree `92749de0046c6c82e901db75f57eeec3500cf358` and
161/12 delta below are historical evidence for that trial; the final accepted
replay is the 14-topic tree
`5b7b50cd573b7a1077586955cad9cf13770df35d` under the 155/6 policy.

After all other host, guest, Linux, and package acceptance completed, the user
directed that the final Geekbench rerun be skipped. Accordingly, this log keeps
the prior matched Geekbench results, focused probes, and G123's matched MMU-miss
screen as the performance evidence, but it makes no new end-to-end performance
claim for the final baseline. A skipped final run is not a rejection of the
retained changes and is not represented as a completed benchmark.

The later rootfs migration does not change that disposition. Geekbench 6 now
uses the image-local `/opt` payload with `--no-upload` and no network preflight,
but no scored run was added. Future runs now have a checked, 120-second
pre-measurement quiescence phase: after systemd login and before profiling or
Geekbench, the runner stops Docker, its socket, containerd, and both apt
update/upgrade service/timer pairs, verifies that none remains active, and
syncs. Failure or timeout aborts measurement, and the service-state changes
remain in the disposable OverlayFS upper. The latest regenerated EROFS guest
suites passed 5/5 in 65.42 seconds at 16 KiB and 5/5 in 69.33 seconds at 4 KiB;
their offline Docker hello-world check uses disposable Docker/containerd tmpfs
state and is a correctness smoke, not a benchmark result. The 16 KiB guest's upstream experimental
subpage-compression warning for a 4 KiB-block EROFS image is recorded as a
filesystem limitation, not as a performance result. All partitioned
`rootfs.img`, upload, and mode-0444 observations below remain dated experiment
evidence rather than current run instructions.

### Native Virtio-GPU presentation path (2026-08-10)

The native macOS display path is a structural latency and bandwidth change,
not a scored CPU benchmark. Virglrenderer's public resource information returns
the GL texture that already backs the selected guest resource. A CGL
framebuffer blit copies that texture into IOSurface storage on the GPU, and a
Metal texture created from the same IOSurface is sampled into the window
drawable. The direct-path unit fixture proves a resource flush does not call
`virgl_renderer_get_rect` or the CPU display update once native presentation
accepts the frame.

Two IOSurfaces were accepted over a single surface because OpenGL must not
overwrite storage still referenced by an in-flight Metal command buffer. A
third queued surface and FIFO presentation were rejected: the implementation
keeps at most one Metal presentation in flight and replaces an older ready
slot with the newest guest frame; CAMetalLayer is explicitly limited to two
drawables. Thus the second slot is a producer/consumer
hazard boundary, not another frame of intentional buffering. VNC and
unsupported GL targets retain the measurement-independent CPU fallback. No
frame latency or throughput number is claimed until an interactive rendered
campaign is captured on controlled display hardware.

The focused 800x500 native guest run reached `graphical.target` and logged the
direct path, while both normal and sanitized host GPU gates passed. It was
stopped manually and did not record frame timestamps, so it is correctness
evidence only and does not change the no-latency-number boundary above.

## Historical controlled benchmark

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

That availability statement records the dated campaign behavior. A
2026-08-14 recheck of the current local Geekbench 6.7.1 payload found
that `--multi-core` enters the multi-core phase directly; G148 uses that accepted path and emits no
single-core section.

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
  score. The `--multi-core` shortcut could not be used in that
  dated campaign; the later current-payload recheck in G148 supersedes that
  availability assumption for new runs.
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

## G37: build-selected 16 KiB guest pages

Started 2026-07-26. The new `UML_GUEST_16K_PAGES` CMake option defaults to
`ON`, configures `CONFIG_PAGE_SIZE_16KB=y`, and supplies one page-size/shift
contract to the C++ MMU, translated gadgets, assembly TLB, and tests. Passing
`-DUML_GUEST_16K_PAGES=OFF` preserves the 4 KiB compatibility build.

The 16 KiB kernel uses three populated page-table levels. An initial
four-populated-level configuration was rejected during boot validation because
its 128 TiB top-level span made the 64 TiB UML task limit fold to zero and sent
address-space teardown through an impractically large range. The corrected
three-level geometry uses 11-bit table indices, `PMD_SHIFT=25`, and
`PGDIR_SHIFT=36`.

Native kernel physical aliases now map the shared backing directly. The 16 KiB
build excludes the 4 KiB native-shadow object and its tests, quarter-page stack
reconciliation, shadow flush/copy calls at syscall and uaccess boundaries, and
the extra allocator alignment used only by the fallback. Guest mapping
generation checks remain because concurrent PTE replacement is independent of
page size. Arbitrary unaligned scalar/pair accesses can still cross a 16 KiB
boundary, so their necessary cross-page gadgets remain; their hot check is
specialized to three host instructions using the build-time page shift.

- [x] Add the default-on CMake flag and kernel configuration.
- [x] Add correct three-level 16 KiB UML page-table geometry.
- [x] Compile the native 4 KiB shadow MMU and shadow-only kernel paths out of
  the 16 KiB build.
- [x] Make MMU, TLB, gadget, and split test fixtures page-size aware.
- [x] Build the production gadget-only configuration.
- [x] Pass the initial 308/308 non-VNC production tests and the VNC test.
- [x] Pass 306/306 non-VNC interpreter tests and the VNC test after making the
  SLiRP-only DHCP assertion configuration-aware.
- [x] Build and pass the 4 KiB fallback: 307/307 non-VNC tests plus VNC.
- [x] Boot the production guest with 10 CPUs and 8 GiB through the tmpfs
  overlay/systemd launcher; verify 16,384-byte pages, read-only lower rootfs,
  DNS, and Java 21.
- [x] Rebuild after the final dead shadow-path audit and pass 308/308 non-VNC
  production tests plus the standalone VNC smoke test.
- [x] Repeat the final debug interpreter matrix: 306/306 non-VNC tests plus
  VNC.
- [x] Repeat the final 4 KiB compatibility matrix: 307/307 non-VNC tests plus
  VNC, including its native-shadow-MMU test.
- [x] Complete the full 10-core Geekbench 6 comparison against G35.

The final production run used the exact post-audit source, 10 guest cores,
7.91 GiB of visible memory, a 16,384-byte runtime page size, and the
read-only-lower tmpfs OverlayFS launcher. It completed all workloads and
powered off cleanly in **3,272 seconds** (54m32s), uploading
(result). The result was **77
single-core / 488 multi-core**, compared with G35's 4 KiB
(result) result at **79 / 416**:
**-2.5% single-core and +17.3% multi-core**. The substantial multi-core gain
outweighs the small single-core regression, so the default-on 16 KiB page
configuration is accepted; the 4 KiB compatibility option remains available
for comparison and fallback.

## G38-G42: 16 KiB single-core TLB profiling

Started 2026-07-27 in response to the G37 single-core regression. Per-workload
1 ms samples showed that translated memory gadgets account for approximately
24-47% of sampled gadget tops in representative single-core workloads. The
existing gadget TLB used an 8,192-entry, 32-byte direct table and computed its
index with `UBFX`, `EOR`, and `AND` before loading the entry. Under 16 KiB
pages, the extra fold was retained from the 4 KiB design even though a larger
low-index range is affordable.

The experiments used G37's immutable executable
`.build/bench/uml-darwin-g37-16k-baseline` as the comparison point. Focused
memory controls include the existing general translation loop and a new
freestanding alias control, `tests/perf/guest_tlb_128m_alias_loop.S`, whose two
hot pages are exactly 128 MiB apart.

- **G38, 4,096-entry table: rejected.** Halving the table retained the index
  work and increased capacity pressure. Its full result was **77 / 454** at
  (result), below G37's 77 / 488.
- **G39, 8,192 one-way direct entries: rejected.** Removing `EOR` and `AND`
  improved the general single-worker control from 9.922 s to 9.771 s
  (**1.52%**) and the ten-worker control from 10.344 s to 10.059 s
  (**2.76%**). However, Ray Tracer grew from 58.574 s to 88.814 s and its
  sample contained 49 TLB-miss stack lines. The 128 MiB alias control took
  0.677 s, confirming deterministic direct-index thrashing. The diagnostic
  full result is (result).
- **G40, 16,384 one-way direct entries: rejected.** The alias control improved
  to 0.193 s and the general single-worker control to 9.651 s, but Ray Tracer
  still took 87.904 s with 41 TLB-miss stack lines. The run was stopped after
  single-core because the capacity increase did not fix the workload's
  conflict pattern.
- **G41, one-instruction folded 16,384-entry index: rejected.** Combining the
  fold into `EOR address,address,LSR #14` followed by `UBFX` protected the
  alias control at 0.197 s, but the general control regressed to 9.855 s and
  surrendered most of the direct-index gain.
- **G42, 16,384-set two-way direct TLB: accepted.** The 16 KiB table now has
  two adjacent 32-byte ways per set. The primary way remains the hot path:
  one `UBFX`, one scaled `ADD`, one tag load, one compare, and one predicted
  hit branch. The second tag is tested only after a primary miss. The C++ miss
  helper co-locates read/write translations for the same guest page, fills an
  unused way first, then uses per-set round-robin replacement. The 4 KiB
  fallback retains its original 8,192-entry folded one-way layout.

G42's alias control is 0.198 s, the general single-worker control is 9.666 s
(**2.58% faster than G37**), and the ten-worker control is 9.988 s at 9.85
host cores (**3.44% faster**). Its full single-core workload wall-time sum is
2,167.409 s versus G37's 2,264.746 s, a **4.30% reduction**. Large improvements
include Clang at 148.498 s versus 179.754 s (**17.39%**) and Text Processing at
238.381 s versus 269.418 s (**11.52%**). The accepted full run completed and
uploaded (result).

Ray Tracer still selected the approximately 89-second calibration duration in
G42, but unlike both one-way candidates its sample contained **zero** gadget
TLB-miss stack lines. Horizon Detection similarly alternated between
approximately 59 and 89 seconds across otherwise unchanged runs. Those
duration steps are Geekbench adaptive-run variability rather than evidence of
remaining TLB conflicts; the controlled alias workload and the miss profile
are the acceptance controls.

The accepted executable is
`.build/bench/uml-darwin-g42-16k-two-way-direct-tlb` with SHA-256
`79514d09b5ed0d80018f9a96ab9ea1c73bc8b183f109ea9927b2e65ac0c05f72`.
Profiles and console output are in
`/private/tmp/geekbench-g42-two-way-tlb-profile` and
`/private/tmp/geekbench-g42-two-way-tlb-console.log`; these temporary paths may
not survive a reboot.

- [x] Profile every G37 single-core Geekbench section.
- [x] Measure smaller, direct, wider, and folded TLB alternatives separately.
- [x] Add a deterministic 128 MiB set-alias performance control.
- [x] Keep the minimum-instruction primary hit path and move replacement work
  to the translation/miss stage.
- [x] Add build-specific one-way/two-way collision and independent read/write
  unit coverage.
- [x] Complete the accepted G42 full Geekbench run.
- [x] Pass 308/308 production non-VNC tests plus standalone VNC.
- [x] Pass 306/306 debug-interpreter non-VNC tests plus standalone VNC.
- [x] Pass 307/307 4 KiB fallback non-VNC tests plus standalone VNC.

## G43: native JIT bring-up and host-safe CPU features

Started 2026-07-27. Execution-engine selection is now a runtime CLI decision:
`--execution-engine=jit`, `gadgets`, or `interpreter`. macOS defaults to JIT
when the option is omitted; iOS defaults to gadgets. The interpreter remains
a correctness/debug path. The ARM64 JIT uses a `MAP_JIT` arena, generated
native blocks, a four-way fixed-size cache, direct Base/FP/SIMD emitters, and
inline 16 KiB software-TLB lookups. The JIT is strict: unsupported block
starts are fatal and there is no JIT-to-gadget fallback state, API, compiled
block type, or CLI switch. Supported native prefixes are retained.

Guest feature reporting now has one host-derived contract. `AT_HWCAP`,
`AT_HWCAP2`, and `/proc/cpuinfo` advertise mandatory FP/ASIMD plus only the
optional AES, PMULL, SHA1, SHA2, CRC32, SHA3, and SHA512 features reported by
Darwin `sysctlbyname`. HWCAP2 is zero. Direct JIT emission also gates optional
CSSC and FP16 instructions on the same host feature source. This prevents
compliant guest JITs from generating instructions the Darwin ARM64 host cannot
execute. Direct JIT emitters cover every advertised optional family: AES,
PMULL, SHA1, SHA2, CRC32, SHA3, and SHA512.

The standalone `tests/integration/jit/run_guest_jit_smoke.py` boots the real rootfs and
checks the auxiliary vector against host sysctls, exact `/proc/cpuinfo`
agreement, V8 dynamic code generation, WebAssembly, and repeated Node process
teardown. Focused JIT tests are split into integer, memory, SIMD, and crypto
files. The JIT compiler implementation is split into independently compiled
integer, floating-point, SIMD-arithmetic, and crypto `.cpp` translation units
behind a narrow internal family interface, while executor/cache/MMU
orchestration remains in `jit_executor.cpp`.

Strict integration qualification uses a separate `.build/kernel-4k` and
`.build/cmake-macos-4k` compatibility tree; the default `.build/kernel`
remains the 16 KiB production configuration. Full non-pressure LTP found two
real strict-JIT gaps. Scalar/vector floating-horizontal instructions now lower
directly, including the exact high-register FABD words `0x7ebfd7df` and
`0x7efed5fe`. `UDF #imm16` now exits generated code with the unchanged fault
PC and delivers architectural SIGILL instead of being treated as an
unsupported JIT block.

The final campaign executed all 2,142 selected cases in 1,480.7 seconds
without a JIT fatal. `mmap18` passed in the full campaign and in 16 concurrent
focused guests. The only initial harness rejection was the documented
timing-dependent legacy-cgroup `memcg_stat` matrix; its contract now accepts
clean, warning-only, and mixed userspace outcomes while requiring a real
TPASS. The isolated control produced four TPASS before the known BusyBox
cleanup break. The standalone 10-vCPU integration control also passed with
39.66 host-CPU seconds over 4.01 seconds wall time, or 9.90 effective cores.

- [x] Add runtime engine selection and platform-specific defaults.
- [x] Add the initial native ARM64 JIT, W^X handling, cache, TLB, and split
  unit suites.
- [x] Propagate a conservative host-safe CPU feature mask through both ELF
  auxiliary vectors and `/proc/cpuinfo`.
- [x] Pass `emulator-native-entry-api`, `arm64-jit-integer`,
  `arm64-jit-memory`, and `arm64-jit-simd`.
- [x] Pass the expanded real-guest JIT/Node/WebAssembly smoke:
  `AT_HWCAP=0x2200fb`, `AT_HWCAP2=0`.
- [x] Audit and complete strict removal of the gadget fallback. Extended
  register arithmetic, pair/ordered/exclusive memory helpers, SIMD structure
  transfers, literal loads, and the FP/SIMD families reached by a normal Node
  process now have direct JIT or correctness-preserving JIT slow paths. Strict
  Node prints `42` and exits normally.
- [x] Add and retain direct extended-register add/sub. A split unit test covers
  all 320 width/op/flag/extension/shift combinations plus SP semantics.
  Differential verification and eight consecutive one-vCPU Node runs pass;
  median startup improved from 1.384 s to 1.344 s (approximately 2.9%).
- [x] Reject and remove both direct pair-memory trials. Rewriting to host
  `LDP/STP` initially improved startup approximately 3.3%, but the second Node
  process hung during teardown. A safer one-span/two-scalar-transfer emitter
  passed full guest differential verification and eight Node runs, but
  regressed the median from 1.344 s to 1.413 s (approximately 5.1%).
- [x] Complete the native-pair stability audit before removal. The fast trial
  remained unstable in a one-vCPU guest even when limited to integer stores
  and when the dispatch budget was reduced from 64 Ki to 8 Ki instructions.
  Two-vCPU and CPU-pinned controls were stable, while Mach thread sampling
  showed live UML task carriers switching through the scheduler rather than a
  stale task-retirement pointer. All pair emitters and temporary diagnostics
  were removed. The restored build passed 20 consecutive one-vCPU Node runs
  with a 1.388 s median.
- [x] Fix JIT differential verification so an early cross-page cold-path exit
  is compared only when the entire generated block completed; equal completed
  instruction counts alone do not imply equal architectural boundaries.
- [x] Remove `--jit-gadget-fallback`, its runtime API/state, decoded gadget
  call, cached gadget block, and gadget builder path. The obsolete option is
  rejected. Engine selection is now the only way to choose gadgets.
- [x] Add direct native CRC32, PMULL, SHA1, SHA2, SHA3, and SHA512 emitters
  alongside AES. The split `arm64-jit-crypto` suite compares every operation
  in all seven advertised optional families with the interpreter, including
  destructive aliases; all host feature sysctls are `1` on the test host and
  the native suite passes.
- [x] Pass the final **314/314** standard production suite, including the VNC
  smoke test and all four split native-JIT instruction suites.
  `guest-ltp-verified` and the guest SMP integration runner remain absent from
  the standard CTest suite.
- [x] Split every JIT compiler family into a real independently compiled
  `.cpp` translation unit; retain no `.inc` implementation fragments.
- [x] Pass strict-JIT SMP at 9.90 effective host cores.
- [x] Complete full non-pressure strict-JIT LTP, fix FABD and UDF gaps, and
  qualify the variable legacy-cgroup result contract.
- [ ] Expand the strict direct-JIT instruction corpus beyond the families
  exercised by boot, Node, and the advertised-extension suite.
- [ ] Run and profile full 10-core Geekbench with the accepted JIT, compare
  against G42, and retain only measured improvements.

## G44: JIT code-expansion and cache-validity audit

Started 2026-07-29 after Geekbench showed that native JIT remained slower than
the gadget engine. Opt-in compile diagnostics now report guest block length,
generated hot-section words, total words including appended cold paths, cache
evictions, arena resets, and code-sequence transitions. The diagnostics run
only while compiling or replacing a block and add no checks to generated
production code. `UML_JIT_DUMP_EXPANSION=1` dumps up to 32 worst-expansion
blocks per host thread; its output is serialized so multi-vCPU dumps remain
decodable.

The audit confirms **both excessive generated code and a more urgent cache
validity defect**:

- A controlled one-instruction `STR X1, [X0]` block emits **48 hot host
  instructions** and **71 total**. Disassembly shows that even a first-way TLB
  hit executes approximately **30 host instructions**: six for the per-block
  entry, address/range and TLB checks, the actual load/store, local-monitor
  maintenance, PC materialization, result materialization, and return.
- A controlled six-instruction block containing five ordinary memory accesses
  and a system call emits **191 hot** and **306 total host instructions**.
- A real one-vCPU boot plus `geekbench6 --help` control compiled 314,368 blocks
  covering 1,806,979 guest instructions: **5.75 guest instructions per
  block**, **24.70 hot generated words per guest instruction**, and **34.08
  total generated words per guest instruction**.
- Temporary lookup instrumentation observed 4,737,523 outer cache lookups and
  315,215 misses. Of those misses, **245,339 (77.8%)** found the same
  generation and PC already cached under an older address-space code sequence.
  They were direct stale-sequence retranslations, not first-time compilation.
  The same short control observed 1,713 code-sequence transitions, 230,171
  occupied-way evictions, and three 64 MiB arena resets. The temporary
  per-lookup instrumentation was removed immediately after this measurement.

The generated-code expansion has four structural causes:

1. There is no block register allocator. Each ordinary arithmetic operation
   reloads its guest operands from `CpuState`, executes one native operation,
   then stores its result back even when the next guest instruction consumes
   it.
2. Each guest memory operation independently emits the complete range check,
   two-way software-TLB probe, inline miss-helper call setup, context reload,
   fault branch, and address adjustment. Adjacent accesses to the same stack
   page do not share a translated page or host base.
3. Every block emits a six-instruction entry and a return/tail-exit sequence.
   A successful linked control-flow exit itself is approximately 27 host
   instructions because it selects a publication-safe target, checks the
   fairness budget, updates chain metadata and guest PC, restores arguments,
   and tail-calls the successor.
4. Blocks are short. The real controls average roughly 4.5-5.8 guest
   instructions, so entry/exit and PC materialization are paid frequently.

The cache-validity problem comes from the address-space-wide `code_changes`
counter. Any write into a mapping marked as guest code, executable mapping
change, or explicit code invalidation increments one global sequence. JIT
cache entries require exact `{generation, code_changes, PC}` equality, and
every sequence change also deactivates all links. Loading or faulting one new
executable page therefore makes unrelated library and loader blocks
unfindable. Since each host task thread owns a separate `JitExecutor`, the same
immutable block can then be retranslated independently on several host
threads.

macOS `sample` cannot unwind or symbolize the anonymous `MAP_JIT` ranges
reliably. Samples attributed to the C++ call-return site inside `run_block`
mostly represent time executing generated code, not time in that C++ branch.
The profiles still identify two independent issues: the reservation
publication gate consumed about 7% of Multi-Core Clang samples, while
Multi-Core Text Processing used only about 1.3 host cores and needs a separate
guest-workload/synchronization investigation.

Increasing the main arena from 32 MiB to 64 MiB is retained provisionally
because it reduced Single-Core Clang from 182.71 s to 137.06 s and the partial
Multi-Core Clang control from more than 424 s to 325.82 s. It treats the
retranslation symptom rather than the cause, raises peak RSS to approximately
10.2 GiB in the ten-vCPU compiler workload, and should not be increased again
before cache validity and sharing are fixed.

The measured optimization order is:

- [x] Measure generated footprint and disassemble representative hot paths.
- [x] Distinguish anonymous generated execution from C++ return-site samples.
- [x] Prove stale-sequence misses with temporary lookup instrumentation, then
  remove that instrumentation from the runtime hot path.
- [x] Retain exact-byte validation of same-generation, same-PC stale entries.
  A candidate is reactivated only after every saved guest word still matches
  and the mapping/code sequences remain unchanged through the proof.
- [ ] Replace global code-sequence cache validity with page-granular code
  epochs and invalidate only blocks and links covering changed code pages.
- [ ] Share immutable compiled blocks per address space across host task
  threads while keeping mutable execution state thread-local.
- [x] Add block-level guest-register allocation with lazy `CpuState`
  writeback at observable exits.
- [x] Reuse page translations across adjacent same-page homogeneous SP and
  allocated-base scalar memory operations, with proof and fault edges emitted
  before any architectural destination is changed.
- [x] Introduce a persistent linked-block ABI so CPU, TLB, and chain pointers
  remain resident and successful linked exits become substantially smaller.
- [ ] Re-profile the reservation publication gate and the underutilized
  Multi-Core Text Processing workload after eliminating retranslation noise.

Reproducible diagnostic logs are
`/private/tmp/jit-runtime-expansion-final.log` and
`/private/tmp/jit-cache-lookup-audit.log`; temporary paths may not survive a
reboot.

### G44 continuation: retained direct-code reductions

The following independently measured changes are now retained:

- The direct-link selector uses one host-owned indexed load instead of two
  candidate loads plus a conditional select. Guest state cannot supply a
  native target.
- Generated-code invalidation now treats the instruction-side cache operation
  as a synchronization boundary and terminates the current block. The
  data-side clean operation remains a no-op. Self-modifying-code tests cover
  both JIT and gadget execution.
- The block compiler retains eight guest GPRs and sixteen guest vector
  registers in translation-time-selected host registers. Ordinary supported
  integer and vector compute instructions therefore emit one remapped native
  instruction after the first live-in; x18 and the executor-owned x19-x21
  range are excluded.
- Two-to-four same-direction SP scalar unsigned-offset accesses now share one
  alignment proof and one permission translation. The translated operations
  preserve their original native encodings with only base/transfer registers
  remapped.
- Linked blocks use a persistent host-owned ABI: x15 holds `CpuState`, x14 the
  TLB, x19 the chain record, x20 the committed instruction count, and x21 the
  publication selector. A linked successor enters after the outer prologue.
  Successful direct and indirect links no longer restore and reconstruct this
  state at every block boundary.
- Two-to-four same-direction allocated-base scalar unsigned-offset accesses
  now share one exact same-page proof and one permission translation.
  Translation rejects load/base aliases and tracked-store builds up front;
  the generated valid path contains no opcode or validity checks. Every
  retained guest transfer remains one remapped native instruction through the
  checked host address.

The persistent ABI reduced the direct-link control from 2.233 s to 1.209 s
(45.9%) with one worker and from 2.313 s to 1.265 s (45.3%) with ten workers.
The indirect-link control improved from 0.370 s to 0.189 s (48.9%) and from
0.406 s to 0.222 s (45.3%), respectively. The SP-memory loop improved a
further 16.3%, from 0.529 s to 0.443 s, while 1,000 `/bin/true` executions
remained neutral/slightly better at 8.718 s versus 8.674 s.

The allocated-base memory-run trial reduced its dedicated one-worker loop
from 0.780 s to 0.442 s (43.3%) and its ten-worker loop from 0.834 s to
0.484 s (42.0%). The unrelated SP control was exactly unchanged at 0.440 s;
the register-only control was neutral at 3.922 s versus 3.918 s. It is
therefore retained.

The current retained executable is
`.build/bench/uml-darwin-jit-gpr-memory-runs`, SHA-256
`bb6215a186bda050c2d7f6526b5dae65854e177472b8b8ef64996a967ed69a0a`.
Its predecessor `.build/bench/uml-darwin-jit-persistent-chain-abi` has
SHA-256
`b452060fe38771a1b64eae84492923156103e8ecb3f3db5173622f689d022518`.

Focused qualification currently passes all 26 JIT and scalar
immediate-memory suites in each of four configurations: default 16 KiB and
4 KiB compatibility builds, each with ordinary execution and
`UML_VERIFY_JIT=1`. The newly split allocated-base codegen/runtime suites
cover all 13 scalar unsigned-offset allocation cells, run lengths, register
aliases, page edges, 4 KiB quarter-page behavior, permission and translation
faults, dirty-prefix state, store masks, monitor clearing, and reservation
overlap. Runtime behavior is checked against expected architectural results
through interpreter, gadgets, and JIT in one shared harness.

### G44 continuation: vector remapping and shared SIMD memory translation

The single-register structure-transfer trial is retained provisionally.
Full-register, lane, and replicate forms now keep a resident guest vector
register mapped to its host vector register instead of staging every transfer
through `CpuState`. The one-worker control improved from 0.937 s to 0.922 s
(1.6%), and the ten-worker control improved from 0.982 s to 0.973 s (0.9%).
This removes nine staging instructions per loop iteration without changing
unrelated controls. The retained artifact is
`.build/bench/uml-darwin-jit-single-structure-remap`, SHA-256
`9e77a53c782b0373a78f838e69db034a3af9c1bfe2ebb4006a54b2a450d6a005`.

Two-to-eight same-direction FP/SIMD unsigned-offset accesses now share one
alignment/tag-state decision, one exact same-page proof, and one permission
translation. Translation accepts all valid B/H/S/D/Q allocation cells, SP or
X0-X30 bases, and V0-V31 transfers. Each retained guest access remains one
native memory instruction whose base is rewritten to the software-MMU result
in x8 and whose transfer register is translation-time remapped. Reserved
cells, mixed bases or directions, tracked-store translations, and optional
tagged-address state take the existing semantic path before any architectural
state is changed.

The dedicated vector-memory loop improved from 0.940 s to 0.850 s with one
worker (9.6%) and from 0.978 s to 0.896 s with ten workers (8.4%). The
allocated-base scalar control was neutral at 0.442 s versus 0.443 s, while
the structure-transfer control improved from 0.930 s to 0.921 s. The trial is
therefore retained. The current qualified artifact is
`.build/bench/uml-darwin-jit-simd-unsigned-memory-runs`, SHA-256
`df019f2b33bbc0b79b75a4aee4086f73231dcb1c2ea0383d0ab9925617846157`.

The generated path keeps the host security boundary even though it omits
guest-validity checks:

- Every native memory instruction uses only the software-MMU-translated x8
  base after an exact one-page proof and permission lookup.
- No guest value becomes a native branch target. Direct and indirect links
  select only host-owned compiled-code addresses.
- Privileged, stateful, reserved, and unsupported encodings are filtered
  while translating, and executable arenas remain MAP_JIT/W^X managed.
- The resident register pools exclude host platform and executor-owned
  registers. TLB helpers preserve the bindings needed after a miss.

Ordinary integer and vector compute already meet the desired shape inside a
block: after a first live-in, most supported guest operations emit one native
instruction with registers remapped. The irreducible exceptions are
PC-relative constant construction, guest-to-host branch target translation,
the first flags import, a small number of constrained-register forms, and
memory/MMU or architecturally stateful operations. Block boundaries still
flush and reload resident values; removing that cost safely requires a
cross-block register contract rather than copying guest branches directly.

One dormant semantic gap was recorded during the security audit. Scalar
ordinary-memory paths in all three engines do not perform the optional
allocation-tag check when `CpuState::mte_enabled`; differential testing alone
cannot expose a defect shared by every engine. The current Darwin host
feature contract fails this feature closed and does not advertise it, so this
cannot be reached by a compliant guest and does not bypass page permissions
or allow guest privilege escalation. All three engines must gain a common
architectural test and implementation before this feature is ever advertised.

Final qualification after the SIMD-memory change:

- Default 16 KiB build: **543/543** standard non-VNC tests passed.
- 4 KiB compatibility build: **544/544** standard non-VNC tests passed.
- Standalone production VNC integration suites: **2/2** passed.
- Focused JIT/scalar/vector memory set: **14/14** passed in each page-size
  build both normally and with `UML_VERIFY_JIT=1`.
- Strict code-sign verification passed for the production binary.

The focused code-generation audit also fixed undefined behavior in three
test helpers: a span was returned into a temporary `CompiledCode` object.
All 24 temporary call sites now receive an owned hot-word vector, so the
generated-code assertions test stable storage.

The successful-link profiler is enabled only with
`-DDARWIN_UML_JIT_GPR_LINK_PROFILE=ON`. Set
`UML_JIT_GPR_LINK_PROFILE_PATH=/path/to/profile.log` for a capture, then rank
the result with:

```sh
python3 tests/perf/analyze_jit_gpr_link_profile.py \
    /path/to/profile.log --top 20
```

The 32 candidates are X0-X30 plus architectural SP; XZR remains excluded even
though it shares encoding number 31. The analyzer charges four native words
per outer invocation to adjacent `CpuState` pairs, including X30+SP, because
they can use paired boundary transfers. Non-adjacent pairs cost six by
default. `--pair-boundary-cost=N` remains available for a uniform experimental
override.

Diagnostic successful-link counters use one literal pointer load followed by
load/add/store. The diagnostic emitter has four fixed pointer-patch slots for
the two link and two counter pointers of a conditional block; production
retains its proved two-slot array and identical generated bytes. Counters are
drained before PIC-slot reuse, link/code clearing, and executor destruction.
Ordinary `end_execution()` no longer walks every compiled edge or acquires the
global reporting lock. A normal app shutdown collects any still-live TLS
executors and writes exactly one parseable snapshot before its intentional
`std::_Exit`. Forced `SIGTERM` or `SIGKILL` can lose the unreported tail;
reporting from the signal exit bridge is intentionally avoided because its
mutex, allocation, and stdio work would not be async-signal-safe.

Next measured steps:

- [x] Retain block-local GPR/vector remapping and shared scalar/SP/SIMD
  same-page translations only after targeted and control benchmarks.
- [x] Rebuild and run the complete 16 KiB and 4 KiB suites after the latest
  JIT changes; do not rely on stale CTest executables.
- [x] Add a diagnostic-build-only successful-link profiler with literally
  byte-identical production code when disabled. Count taken direct/PIC edges
  and combine source dirty-live-out with target read-before-write masks.
- [ ] Use two short, diverse Geekbench captures to rank all fixed two-register
  cross-block banks. Proceed only if the winning pair is stable across
  workloads and remains positive after outer-entry/terminal publication cost.
- [ ] Prototype the selected fixed bank across successful links, retaining
  precise publication on syscall, fault, fairness, and fallback exits.
- [ ] Replace global sequence validity with page-granular code epochs, then
  share immutable compiled programs across vCPUs without sharing mutable
  execution state.
- [ ] Run the full 10-core Geekbench JIT/gadget comparison after the bounded
  link-boundary experiment, profile every workload, and investigate rather
  than automatically reject isolated regressions when the total improves.

### G45 continuation: stateful native links and compact block boundaries

The direct-remapping audit now covers the complete ordinary Base and FP/SIMD
compute lowering surface. **74 of 75** methods emit exactly one remapped
native semantic instruction after their first live-in: 14/14 integer, 18/19
scalar floating-point, 25/25 SIMD arithmetic, 7/7 fixed transforms, and all
ten additional vector-compute cases in the executor. The sole exception,
`emit_floating_javascript_convert`, emits its native operation followed by a
six-word status correction containing two data-dependent branches. A
temporary native probe confirmed that the current host needs this correction:
under flush control, positive nonzero subnormal inputs produce the correct
integer and exception but the wrong zero indicator. The probe was removed
after the result was recorded. Remaining expansions are architecturally
required memory translation, flags import/publication, PC-relative
construction, or constrained-register staging. The dominant performance
cost is therefore block handoff rather than instruction-body emulation.

Stateful native blocks can now remain inside the assembly engine. Each outer
entry selects one of ten host-owned capability combinations covering direct
store publication, materialized vector state, and the two native execution
environments. A successor is published only in entries whose setup satisfies
its translation-time requirements. Guest branch data can equality-select a
PIC record, but the final native branch still consumes only a host-published
internal entry.

The first retained stateful-link artifact,
`.build/bench/uml-darwin-jit-capability-links-16-entry`, has SHA-256
`d21410ba19ffbddd197eb55d7d8d3e00ab5b8695798106345efa2ca08ac6a314`.
Against the preceding
`.build/bench/uml-darwin-jit-gpr-profile-off-arena-recompile` artifact, the
one-worker vector-link control improved from **0.636 s to 0.183 s (71.2%)**,
and the ten-worker control improved from **0.704 s to 0.238 s (66.2%)**.

Three independently qualified boundary reductions are retained on top:

1. Dense selector states place PC, instruction count, and all ten native
   entries in one aligned 128-byte descriptor. One paired metadata load
   replaces two scalar loads, reducing the successful selector-to-branch path
   from 12 to 11 instructions.
2. Stable descriptor and PIC pointers use one PC-relative literal load.
   Pointer data is 8-byte aligned after all executable paths, no generated
   branch targets it, and an out-of-range translation uses a cold
   translation-owned absolute-materialization stub. This removes two executed
   instructions without increasing ordinary arena footprint.
3. The host-owned fairness state is a remaining budget. Its subtraction is
   scheduled immediately after the pointer load to overlap that dependency;
   rejected links undo it only on the cold path. This removes two more
   instructions while preserving exact 4,096-instruction fairness and
   non-dividing block quantization.

The final successful direct boundary is **10 executed host instructions**
from its pointer load through `BR x13`, down from 15 before these three
changes. The matched first-way PIC path is 13. Exact code-generation tests
also prove that the only native indirect branch remains `BR x13`.

Measured medians for the final
`.build/bench/uml-darwin-jit-remaining-budget` artifact (SHA-256
`5e86bc0bd047be930d40aa53b166b4b574d95d93325b46434ebbb8c8c284e142`)
are:

| Control | Pre-capability | Final | Change |
|---|---:|---:|---:|
| Direct link, 1 worker | 0.421 s | 0.403 s | 4.3% faster |
| Direct link, 10 workers | 0.485 s | 0.471 s | 2.9% faster |
| Stateful vector link, 1 worker | 0.636 s | 0.185 s | 70.9% faster |
| Stateful vector link, 10 workers | 0.704 s | 0.243 s | 65.5% faster |
| Indirect link, 1 worker, compact-layout baseline | 0.220 s | 0.201 s | 8.6% faster |

The shortest vector control remains about 0.5% slower with one worker and
3.8% slower with ten workers than the intermediate descriptor-layout build,
even though it is dramatically faster than the pre-capability baseline.
This workload-dependent loss is not grounds to discard the overall
improvement. The pointer load is now interleaved with independent arithmetic;
the remaining difference will be checked against per-workload Geekbench
profiles instead of overfitting another special-case path to one synthetic
loop.

Focused qualification currently passes:

- 16 KiB and 4 KiB builds, seven direct/PIC/fairness/state suites each;
- the same suites with `UML_VERIFY_JIT=1`;
- profile-enabled 16 KiB and 4 KiB builds, nine suites each, normally and
  with verification;
- strict code-sign verification, with no diagnostic profiler symbols or
  strings in the production artifact.

The post-change isolation audit found no guest-to-host pointer injection,
native branch injection, host-memory permission bypass, or guest privilege
escape.

#### Translation-boundary hardening

Both availability findings are resolved in the retained
`.build/bench/uml-darwin-jit-translation-boundary-hardening` artifact,
SHA-256
`47b661c170ecca90ff813fc0233c6ecd671cc4bd935db8cc484714fd5b2dbcd0`.
Optional standalone barrier encodings are selected entirely at translation:
a supported host receives one native word and another host receives the
stronger baseline substitute. There is no runtime feature test or opcode
branch. An unavailable or unsupported first word now raises the precise
guest illegal-instruction event with unchanged PC and zero retired
instructions instead of falling back to gadgets or terminating the emulator.
A supported prefix stops immediately before a later unsupported word.

The split barrier code-generation/runtime and illegal-runtime suites pass in
16 KiB and 4 KiB builds, normally and with `UML_VERIFY_JIT=1`; their
profile-enabled variants pass as well. The host fault bridge was hardened at
the same boundary: it formats a bounded message on the stack, performs one
`write`, and exits without invoking non-async-signal-safe formatting.

The deliberately lean valid hot path remains confined by these non-negotiable
translation and memory boundaries:

- Every guest memory access uses permission-tagged software-MMU/TLB
  translation or an exact same-page proof before a native load or store.
- Generated arenas and link descriptors are not guest-mapped.
- Privileged system encodings are rejected or virtualized, while reserved and
  host-unsupported encodings are resolved before execution.
- Literal link pointers are host-owned data placed after terminal code.
  Direct and PIC links can end only in the host-owned `BR x13` target.
- Executable storage remains under bounded MAP_JIT/W^X write toggling.

#### Fixed pointer-patch metadata

A complete emitted-shape audit proved that a production block needs at most
two stable pointer patches: one for an unconditional direct or indirect exit
and two for the two successor exits of a conditional branch. Cold paths emit
none. Production translation metadata therefore uses a fixed two-entry array
plus a count and an eight-bit literal mask. The separately enabled link
profiler uses four fixed entries so each successor can also load its
translation-owned counter in one word. Exceeding the applicable proved bound
aborts during translation; no validity check was added to generated code.
Production emitted bytes are unchanged.

The retained artifact is
`.build/bench/uml-darwin-jit-literal-fixed-metadata`, SHA-256
`4bab12e9db02c5989d673d89307211cb401f5339696d9f7a85acf4d470719499`.
Focused tests cover the exact two-literal pool, two independent out-of-range
fallbacks, and the production conditional shape with exactly two aligned
literals and no hot absolute-materialization sequence. The 16 KiB focused
set passes 9/9 normally and 9/9 with verification.

In a matched 1,000-run cold `/bin/true` test, the remaining-budget baseline
median was 8.901 s and the fixed-metadata build was 8.498 s, a **4.5%**
improvement that fully recovered the literal-patch cold regression. Its hot
controls remained:

| Control | Median |
|---|---:|
| Direct link, 1 worker | 0.384 s |
| Direct link, 10 workers | 0.412 s |
| Indirect link, 1 worker | 0.181 s |
| Stateful vector link, 1 worker | 0.166 s |
| Stateful vector link, 10 workers | 0.181 s |

#### Exact-retirement link ABI

The linked-block return ABI now packs the event in the low 32 bits and the
exact block-local retired count in the high 32 bits. Translation performs the
non-retiring-terminal subtraction for constant exits. The seven dynamic
helpers return an exact packed 0/1 result or their dispatcher result, and
dynamic prefix composition uses an addition; a zero prefix emits neither the
load nor the addition. This removes the post-return terminal-count correction
and both of its per-edge metadata stores.

`JitChainState` is now 40 bytes: remaining budget at offset 0, store state at
8, indirect-miss state at 16, selector at 24, and indirect source PC at 32.
The successful direct persistent path falls from 10 to **8** instructions
(the memory ABI from 14 to 12), while the first-way PIC persistent path falls
from 16 to **14** (the memory ABI from 20 to 18). Split code-generation and
runtime tests cover direct and PIC syscall, breakpoint, and illegal exits,
plus odd-prefix cross-page dynamic success/fault behavior and the absence of
the retired metadata stores. The 16 KiB and 4 KiB normal suites pass 11/11,
their verification suites pass 7/7, and both profile-enabled code-shape
suites pass 4/4.

The retained artifact is
`.build/bench/uml-darwin-jit-exact-retirement`, SHA-256
`75ccd2b3c7ab0a55df10c5eb387bf4eb335612387478d654f694d8e375682c89`.
Matched results against the fixed-metadata artifact are:

| Control | Fixed metadata | Exact retirement | Change |
|---|---:|---:|---:|
| Cold `/bin/true`, 1,000 runs | 8.498 s | 8.507 s | 0.1% slower |
| Direct link, 1 worker | 0.384 s | 0.330 s | 14.1% faster |
| Direct link, 10 workers | 0.412 s | 0.350 s | 15.0% faster |
| Indirect link, 1 worker | 0.181 s | 0.168 s | 7.2% faster |
| Stateful vector link, 1 worker | 0.166 s | 0.170 s | 2.4% slower |
| Stateful vector link, 10 workers | 0.181 s | 0.178 s | 1.7% faster |

The build is retained because it is substantially positive overall. The
isolated one-worker vector result will be investigated in real per-workload
profiles rather than used to reject the improvement.

Current continuation checklist:

- [x] Resolve optional barriers and unsupported first-word events entirely at
  the translation boundary, with no gadget fallback.
- [x] Replace variable pointer-patch containers with proved fixed metadata and
  recover the cold-translation regression.
- [x] Replace inferred terminal retirement with the exact packed return ABI
  and remove the hot per-edge metadata stores.
- [x] Audit all ordinary Base and FP/SIMD compute lowerers: 74/75 are
  one-to-one after first live-in, and the current host requires the sole
  conversion-status exception.
- [ ] Finish and strictly code-sign the production-equivalent
  exact-retirement profiling artifact.
- [ ] Capture a current 10-core, 8 GiB JIT Geekbench run through every
  workload with five-second samples, using
  `/private/tmp/geekbench-g45-current-profile`,
  `/private/tmp/geekbench-g45-current-console.log`, and
  `/private/tmp/geekbench-g45-current-gpr-links.log`.
- [ ] Rank successful-link register pairs from the current capture, then
  prototype a fixed cross-block GPR bank only if the evidence is stable across
  workloads.
- [ ] Evaluate the two-register low native vector bank for constrained source
  forms only after the current profile establishes that staging is material;
  preserve the bindings across the rare TLB-miss bridge.
- [ ] Rebuild and run the complete 16 KiB and 4 KiB standard suites, then run
  non-pressure LTP, UnixBench, and the final 10-core JIT/gadget Geekbench
  comparison.

### G46: deferred architectural state and strict one-to-one code shape

Ordinary generated compute now defers the architectural floating-point status
read/mask/merge until an actual observer or semantic boundary. The removed
sequence was 15 host instructions after every generated invocation. Native
state is materialized before semantic helpers, CPU switches, differential
verification, execution end, and destruction, so the production hot path no
longer pays for state that a linked successor can continue to own.

The isolated artifact is
`.build/bench/uml-darwin-jit-deferred-fp-only`, SHA-256
`cb69a4e092fdeea5d45cb220014412e8f6d5d8032b919cbf5c2f14e736704edb`.
Focused lifecycle, observer, helper, CPU-switch, and real verification-mode
tests pass in 16 KiB and 4 KiB builds, both normally and with
`UML_VERIFY_JIT=1`.

The first profiled comparison was deliberately treated as diagnostic rather
than performance evidence. Its baseline used five-second `sample` captures
while the isolated run used two seconds. On this large multi-threaded process,
report generation can outlive the requested capture window and materially
perturb a workload. The apparent 24.9% single-core gain was therefore invalid.
The call-tree result remains useful: the removed status offsets had zero
samples in all 16 new profiles; 57.97% of dominant-carrier self samples were
inside anonymous generated code and 8.79% were in helpers explicitly called by
generated code. The apparent generated-call return site itself had zero self
samples.

A matched clean comparison replaced the sample command with `/usr/bin/true`
in both runs while retaining identical 8 GiB memory, ten-core topology,
network, overlay root, and engine settings:

| Single-core aggregate | Time |
|---|---:|
| Exact-retirement baseline | 1001.636 s |
| Deferred-state artifact | 994.563 s |
| Change | **0.71% faster** |

The largest clean gains were PDF Renderer (5.48%) and HDR (6.34%). Photo
Library was 9.70% slower in this ordering, but recent values span 31.3-35.3 s,
its host utilization was unchanged, and no translation/cache regression was
visible. Clang, Asset Compression, Background Blur, and Structure from Motion
had smaller regressions while the aggregate remained positive. The change is
retained provisionally and the combined artifact's clean pass is the
reverse-order repeat.

The one-to-one code-generation contract is now enforced by two split suites:
Base and FP/SIMD. Each case compiles a target plus a distinct ordinary
successor, so a helper or runtime decision is detectable both before and after
the semantic word. The tests verify translation-selected metadata, exact
register-field remapping, GPR/SP/XZR and vector operand roles, alias
consistency, exclusion of host-reserved registers, and the absence of runtime
control transfers around ordinary valid operations. Coverage now includes the
previously missing register-move, compare-zero, conditional-compare,
integer-indexed, lane-transfer, and flag-manipulation lowering shapes. The one
documented host-status correction is locked to its exact bounded sequence so
the exception cannot spread silently. Both suites pass with `-Werror` in 16
KiB and 4 KiB builds.

### G47: reservation-helper convoy removal

The multi-core baseline exposed a publication-gate convoy rather than a
guest-scheduler problem. In steady Horizon Detection, every carrier spent
69-75% of its samples spinning while multiple reservation-sensitive helpers
simultaneously announced themselves active and rescanned every vCPU
participant.

The candidate serializes those helpers before they increment the active
counter. Queued helpers park in the host mutex implementation and remain
outside the participant scan; the one owner performs the unchanged
publication handshake. The normal direct-store generated path gains no
instruction, validity check, conditional branch, or mutex operation. This
mostly narrows a region already serialized by the architectural exclusive
memory mutex.

Reservation identity is also separated from fault-safe access. The semantic
memory bridge returns the stable translated host-backing identity used by the
reservation protocol without allowing unchecked direct native access.
Tracked native stores publish translated host ranges, so two guest virtual
aliases of the same backing invalidate one another while non-overlapping
ranges remain independent. `CpuState` stores the stable reservation key
separately from the architectural address.

Focused publication, alias, runtime, and scalar/vector memory suites passed in
16 KiB and 4 KiB builds, normally and with verification. The publication-gate
tests also passed 1,000 consecutive ordinary and 1,000 verified repetitions in
each page-size build.

The combined candidate is
`.build/bench/uml-darwin-jit-deferred-fp-reservation-turn`, SHA-256
`6c588b4fb0c98c3fda4b7809d35b36329667ff27598db4cde6cddbf902368573`;
strict code-sign verification passes.

The `std::mutex` implementation is **rejected** even though its focused
correctness tests pass. An equal-duration profiled run was stopped when PDF
Renderer appeared 16.45% slower; a clean repeat proved that result was a
sampling artifact and completed single-core 0.82% faster than the isolated
deferred-state artifact. Single-core Horizon nevertheless consumed 6.35% more
host CPU work and was 5.96% slower, exposing an uncontended helper-turn tax.

The clean run was stopped during multi-core after two confirmed regressions:
HTML5 Browser consumed about 43% more host CPU work and became the slowest
recent run, while PDF Renderer was at least 12.64% slower than the older
profiled baseline. This rejects the mutex, not the required serialization
semantics. The next isolated candidate is a FIFO ticket turn whose contended
waiters park with atomic wait/notify. A store joins the phase protocol only
after it actually observes an active helper, leaving the ordinary
no-contention store path unchanged. The next ticket owner yields to those
published stores before announcing itself active.

Current continuation checklist:

- [x] Remove the eager 15-instruction state merge and prove every observer
  materializes state before using it.
- [x] Strengthen the split one-to-one code-shape suites and pass both page-size
  configurations.
- [x] Prove reservation-helper serialization and virtual-alias invalidation in
  focused normal and verified tests.
- [x] Run a clean matched isolated single-core comparison; retain the 0.71%
  aggregate gain provisionally and investigate regressed subtests.
- [x] Reject the `std::mutex` turn after clean single-core Horizon and
  multi-core HTML/PDF regressions; do not confuse the earlier profiled PDF
  artifact with the clean result.
- [ ] Implement and qualify the parked ticket/store-phase turn, then repeat
  the clean multi-core gate workloads before another full run.
- [ ] Run the accepted combined artifact without host sampling for the repeat
  single-core table and final score.
- [ ] Trial the measured next optimizations one at a time: inline the common
  reservation-reconciliation predicate; translation-select dedicated
  pointer-integrity helpers; reduce the dominant software permutation leaf.
- [ ] Capture a current realistic generated-code expansion ratio with
  compile-only diagnostics; keep all decode, host-feature, and legality
  decisions at translation.
- [ ] Rebuild and run complete 16 KiB and 4 KiB suites, non-pressure LTP,
  UnixBench, and the final JIT/gadget Geekbench comparison.

### G48: native-remap contract and cold translation paths

The JIT contract is now explicit: an ordinary valid guest compute operation
must become one native semantic instruction after its first live-in, with
register fields remapped during translation. Decode, allocation legality, and
host-feature selection must not survive into the generated hot path. This
permission to omit guest-validity checks does not extend to the host boundary:
native memory operands still come only from permission-specific software-MMU
translations, native branch targets remain host-owned, privileged/stateful
encodings are filtered while translating, and executable storage remains
MAP_JIT/W^X managed.

The split Base and FP/SIMD one-to-one suites now independently classify the
emitted semantic word's top-level opcode class. Every matrix case rejects a
remap that becomes native memory, control flow, exception, or system state.
Every hot direct branch must target the same translation, and the only
permitted indirect transfers are the host return or a helper call through x16
loaded from an aligned immutable literal-pool entry. The ordinary lowering
audit remains **74/75** one-to-one; the sole bounded exception on this host is
the required status correction for one conversion operation.

Additional translation-only reductions are retained:

- ADR and ADRP now load their fully resolved guest address with one native
  literal load rather than a two-to-four-word MOV-wide sequence.
- Fixed helper pointers, guest-PC stores, and fixed guest link-register values
  share the immutable literal tail. This shortens generated call and exit
  sites without allowing literal data to become a native branch target.
- The TLB ABI stores each access-specific page tag adjacent to its host
  backing delta. JIT and gadget lookups use one paired load rather than
  separate tag and delta loads.
- The common JIT way-0 hit is now
  `LDP/EOR/CBNZ(not-taken)/ADD` after index formation. The 16 KiB second-way
  probe and the preserving miss bridge are emitted after `hot_words` and
  branch back to the exact access continuation. A one-store block measures
  **32 hot / 76 total words** at 16 KiB and **34 hot / 74 total words** at
  4 KiB; the former hot ceiling was 52 words.

The cold bridge continues to preserve every resident GPR and vector binding,
NZCV, the floating-point environment, CPU/TLB context, and LR. Runtime tests
force a first miss with all eight GPR and all sixteen vector slots occupied.
A separate 16 KiB collision test fills both ways, takes way 1, rejoins before
the native memory operation, and proves that an intervening architectural
operation executes exactly once. Focused memory/runtime qualification passed
8/8 in both page-size builds.

The contained system-state policy gaps found during the security review are
also closed. FPMR MRS/MSR now require the corresponding host capability;
SSBS, DIT, SM/ZA, and TCO aliases require their respective host features.
These checks occur only while translating. Forced feature-off/on matrices
cover all relevant immediates and all 32 transfer-register variants, and the
focused system/PSTATE set passes in both page-size builds.

Two signed artifacts bracket the cold-translation trial:

- pre-outline literal/paired-TLB artifact:
  `.build/bench/uml-darwin-jit-literals-paired-tlb`, SHA-256
  `69e53815cbf2a474cb2ef0901a6abb18466b38a6edf9f76ffb287a99ea2e68b9`;
- cold-way1/miss candidate:
  `.build/bench/uml-darwin-jit-cold-tlb-literals`, SHA-256
  `e0665f2fe1209130c49ee09e03f1938f82162853a062a232a3fc7a60d9897cfd`.

A short real-guest compile-shape capture reached `geekbench6 --help`. It is
not a wall-time comparison because the old and current captures differ in
length and block mix. Normalized emitted shape changed from 25.960 to 19.208
hot words per guest instruction (-26.01%) and from 35.142 to 33.223 total
words (-5.46%); the non-hot tail grew because work and literals deliberately
moved out of the executed prefix.

The first matched runtime control could not be completed: the 1 GiB systemd
guest timed out during boot, and the standard 8 GiB retry was rejected by the
host execution-approval usage limit. No alternate launch path was attempted.
The candidate remains provisional until the matched memory controls and
Geekbench workloads can run.

Current continuation checklist:

- [x] Enforce ordinary-compute opcode-class and host-owned-target invariants.
- [x] Reduce PC-relative, helper-pointer, guest-PC, and fixed-LR
  materialization to one immutable literal load when in range.
- [x] Pair TLB tag/delta loads and move way 1 plus the preserving miss bridge
  out of the hot prefix.
- [x] Force feature-specific state instructions through translation-time
  on/off tests with no generated runtime feature branch.
- [x] Add diagnostic-only register-pressure counters with byte-identical
  production code; trial only x22/x23 if dynamic reuse clears its two-word
  outer-boundary break-even.
- [x] Preallocate q6/q7 for constrained indexed sources, then make any
  unbound low slot available to ordinary allocation only after q16-q31 fill;
  alias and helper-boundary preservation tests remain exact.
- [ ] Complete matched memory-loop and realistic workload measurements once
  local emulator execution is available, then retain or reject the cold TLB
  candidate from measured results.
- [ ] Run complete 16 KiB and 4 KiB suites, non-pressure LTP, UnixBench, and
  the final JIT/gadget Geekbench comparison.

### G49: exact constants and constrained vector prebinding

Constant construction now chooses its first MOV-wide operation at the first
non-default 16-bit half instead of first constructing a zero low half. The
translator counts the zero-filled and one-filled alternatives, chooses the
one-filled form only when it is strictly shorter, and keeps the zero-filled
form for ties so that generated shape remains deterministic. Exact tests cover
zero, upper-only and sparse values, all-ones and sparse-complement values, and
the tie rule. The all-ones continuation sentinel falls from four instructions
to one, removing three executed instructions from that path.

The resulting exact hot-word reductions are 33 to 32 for a scalar store, 34 to
33 for an SP pair, 35 to 34 for a GPR pair, 34 to 33 for a vector pair, 25 to
24 for an ordinary-compute pair, 28 to 27 for a directly linked block, 40 to
39 for an indirect-cache link, and 49 to 47 for a conditional direct link.
Successful link paths remain 8 and 14 words. At 16 KiB, total generated sizes
for the scalar, SP, GPR, vector, and ordinary-compute cases fall from
78/86/70/86/28 to 76/84/68/84/26 words. The canonical one-store TLB shape is
therefore **32 hot / 76 total words** at 16 KiB and **34 hot / 74 total
words** at 4 KiB, matching the corrected G48 record.

A build-time-only GPR-pressure profile now measures resident-slot high-water,
allocation refusals after the eight-slot bank fills, overflow reads and
writes, repeated overflow accesses, and estimated avoided accesses at
capacities 10, 12, and 15. Its saturation control reports high-water 8,
16 refusals, 8 reads, 8 writes, 8 repeated accesses, and estimated avoided
accesses **2/4/7**. Its nonallocating post-index control reports no refusal,
2 reads, 1 write, 2 repeated accesses, and **1/1/1** estimated avoided
accesses. The no-pressure and cache-disabled controls remain zero. Enabling
the counters does not change generated words or metadata, and ordinary
production binaries contain neither the diagnostic environment hook nor its
reporting strings.

Translation now counts the constrained source identities used by indexed
vector forms and prebinds the two most frequent identities to q6 and q7, with
first encounter breaking a frequency tie. Planning stops at the first
translated terminal, so fetched words beyond the actual block cannot affect
the live prefix. An earlier producer of a selected identity receives the same
low host register immediately; its later constrained use is consequently the
single remapped native semantic instruction, with no staging move, runtime
decode, feature test, or validity branch. A third distinct constrained
identity retains the bounded, cache-aware q2 staging path and does not split
the block.

The preserving TLB miss bridge now saves and restores q6 and q7 in addition
to the existing resident bank. This expands only the cold bridge frame from
368 to 400 bytes; the TLB-hit prefix is unchanged. Forced-miss, direct-link,
alias, full-bank, third-identity, terminal-boundary, and cold-fault controls
exercise the low-bank mask bits and state publication paths.

Ordinary vector allocation preserves the existing q16-q31-first order and,
only after that bank fills, consumes any q6/q7 slot not prebound to a
constrained identity. Blocks with zero, one, or two constrained pins therefore
gain two, one, or zero additional ordinary resident slots without changing
low-pressure register assignments. Selection is completed by the translator:
the generated path gains no runtime branch, validity check, decode, or
entry/helper/exit boundary work. The existing cold bridge already preserves
both low registers.

An 18-vector pressure block falls from **58 hot / 60 total words** to **55 hot
/ 58 total words**. Its low-pressure control remains exactly **22 hot / 24
total words**, proving that the extra capacity is dormant until q16-q31 are
full.

Two stale exact-shape tests were corrected without changing production code:

- `arm64-jit-lazy-flags-codegen` still expected the second TLB way and the
  returning miss call in the hot prefix after those paths had been outlined
  into the cold tail.
- `arm64-jit-pair-remap-codegen` matched the internal paired TLB tag/delta
  load rather than the semantic checked-address pair load.

The focused JIT set passes **35/35** in each of the 16 KiB and 4 KiB builds.
It includes exact-retirement, one-to-one FP/SIMD, vector-remapping, TLB-miss
preservation, linked-vector runtime, lazy-flags, and pair-remapping suites,
plus ordinary-capacity cases with zero, one, and two constrained pins, a
full-cache fallback, preservation across the TLB helper, terminal
materialization, and a direct cold memory fault. The structure-remapping
codegen/runtime saturation fixture was also expanded from the old 16-vector
bank to all 18 resident slots; both repaired tests pass in both page-size
builds.

The then-current sandboxed matrices passed **555/555** at 16 KiB and
**554/554** at 4 KiB after excluding only `virtio-gpu-vnc-smoke`. That host
integration test could not bind its localhost listener in the managed sandbox
(`PermissionError: [Errno 1]`). It remained registered and subsequently
passed in both page-size builds with explicit permission outside the socket
restriction. The verified guest LTP campaign and guest SMP runner remain
standalone integration suites and are not part of standard CTest.

Matched guest performance measurements are deferred. The 1 GiB control timed
out during boot, and the 8 GiB retry was rejected by the explicit host
execution-approval usage limit through **2026-08-05 13:09**. No alternate
launch path was attempted, so this phase does not claim a measured runtime
gain.

Current continuation checklist:

- [x] Shorten exact constant construction without changing its value or
  deterministic tie behavior.
- [x] Add diagnostic-only GPR-pressure metrics while preserving production
  generated bytes and metadata.
- [x] Prebind two constrained vector identities to q6/q7 entirely at
  translation and preserve them on every cold/helper boundary.
- [x] Bound frequency planning at translated terminals and retain q2 as the
  no-split third-identity fallback.
- [x] Let ordinary allocation use unbound q6/q7 only after q16-q31 fill,
  reducing the 18-vector pressure shape without changing its low-pressure
  control or adding generated/boundary work.
- [x] Correct the two stale test-only shape expectations and pass the focused
  **35/35** JIT tests in each page-size build.
- [x] Expand the structure-remapping saturation fixtures to all 18 resident
  vector slots and pass the complete runnable standard matrices:
  **555/555** at 16 KiB and **554/554** at 4 KiB.
- [ ] Use the pressure measurements to evaluate frequency/liveness-based GPR
  selection, or x22/x23 only when its outer-boundary cost clears the measured
  break-even.
- [x] Evaluate translation-time contiguous prebinding for nonconsecutive
  table lists and eliminate their staging moves when a conflict-free host
  register run is available.
- [x] Rerun the socket-restricted VNC smoke with explicit permission and pass
  it in both page-size builds.
- [ ] Rerun the standalone integrations, then perform matched memory and
  realistic workload measurements after host emulator execution is available.

### G50: translation-time consecutive vector-list planning

Multi-register vector lookup operations now receive a consecutive host
register run while the block is translated. The planner recognizes only exact,
valid lookup encodings that the host can execute natively, ignores the
single-register form that is already direct, and stops at reservation
boundaries and translated terminals. It runs after the constrained q6/q7
selection from G49, then allocates from the high end of q16-q31. This preserves
the ordinary q16-first assignment order while reserving conflict-free
consecutive runs for lists of two through four registers.

The generated guest hot path contains no planning check, opcode check, feature
branch, or list-staging decision. A compatible lookup is emitted as its single
remapped native semantic instruction after the required live-ins. Wrapped
guest lists, destination/table aliases, and multiple disjoint lists are
planned explicitly. If an earlier binding makes a consecutive assignment
impossible, translation retains the existing bounded staging path rather than
changing semantics or splitting the block.

A dedicated code-shape suite compares the same interleaved four-register block
with planning enabled and disabled. The planned form falls from **41 hot / 44
total words** to **37 hot / 40 total words**, removing exactly four vector
staging moves. Its architectural metadata is byte-for-byte identical. Exact
checks also cover one list, two independent four-register lists, wrapped
aliases, the host-optional two-register form, and planning termination at a
system-call boundary.

The behavior coverage is split into a separate shared three-engine runtime
file. On this host the interpreter ran 3 cases, gadgets ran 3, and JIT ran 2;
the remaining optional case was skipped only for its missing native host
capability. The focused JIT matrix now passes **37/37** in both page-size
builds. The ordinary sandboxed matrices pass **557/557** at 16 KiB and
**556/556** at 4 KiB; the registered VNC smoke passes separately with explicit
socket permission in both builds. The complete standard totals are therefore
**558/558** at 16 KiB and **557/557** at 4 KiB. Guest LTP and SMP remain
standalone integration suites.

The change affects translation-time register assignment only. Guest-memory
access still crosses the checked software-MMU boundary, native branch targets
remain host-owned, stateful and privileged encodings retain translation-time
policy checks, and executable storage remains MAP_JIT/W^X managed.

Matched guest performance measurements remain deferred through
**2026-08-05 13:09** by the explicit host execution-approval usage limit. No
runtime-performance claim is made from code shape alone, and no alternate
emulator launch path was attempted.

Current continuation checklist:

- [x] Allocate conflict-free two-, three-, and four-register lookup lists
  consecutively at translation time.
- [x] Preserve q6/q7 constrained-source priority and the ordinary q16-first
  low-pressure assignment order.
- [x] Keep the valid planned hot path free of runtime checks, decisions, and
  staging moves.
- [x] Add split exact-codegen and shared three-engine behavior suites.
- [x] Pass focused **37/37** JIT tests, the socket-restricted VNC smoke in
  both builds, and complete **558/558** 16 KiB plus **557/557** 4 KiB
  standard matrices.
- [ ] Use the existing pressure measurements to evaluate GPR
  frequency/liveness selection.
- [ ] Rerun the standalone integrations, then perform matched memory and
  realistic workload measurements after host emulator execution is available.

### G51: translation-observed GPR residency selection

The native compiler now observes the actual GPR reads and writes performed by
the normal lowering pass. When more than eight guest identities compete for
the fixed resident bank, translation estimates the state accesses avoided by
each identity and chooses the best stable eight, with first access breaking a
tie. It charges each selected identity for its required initial read and final
dirty publication, so a retry is attempted only when the projected traffic is
strictly better than the existing first-use assignment.

The original compilation remains the default result. A candidate translation
must reproduce all architectural metadata exactly, make the hot prefix
strictly shorter, and keep total generated code no larger. Otherwise it is
discarded. Resident identities do not change within a block, preserving the
existing cold-exit dirty masks and snapshots. Low-pressure blocks compile only
once and remain byte-identical.

A representative pressure sequence first touches eight single-use identities,
then repeatedly uses x8 and x9. The selected form removes exactly six state
loads and three intermediate stores, reducing its hot path by **nine words**;
the translation estimate is also nine avoided state accesses. Every guest
arithmetic operation remains one remapped native semantic instruction after
its live-in. No runtime opcode check, feature check, validity branch,
selection branch, or eviction logic was added.

Coverage is split between an exact-codegen file and a shared three-engine
runtime file. It proves the profitable selection, byte-identical low-pressure
and zero-benefit controls, terminal-bounded observation, and linked-block
metadata. The runtime oracle executes the same pressure program in
interpreter, gadgets, and JIT; this host reports **1/1/1** executions with no
feature skip. The build-time pressure and link-profile diagnostics also pass
unchanged in both page-size modes. One stale diagnostic-only literal-count
expectation was corrected to distinguish four link-metadata literals from two
guest-PC literals introduced by the earlier literal-loading work.

The focused JIT matrix passes **39/39** in both page-size builds. The ordinary
matrices pass **559/559** at 16 KiB and **558/558** at 4 KiB after excluding
only the registered VNC smoke. With explicit permission for its local endpoint
binds, VNC passes in both builds, making the complete standard totals
**560/560** and **559/559**, respectively. The verified guest LTP campaign and
guest SMP runner remain standalone integration suites.

Guest performance measurement remains deferred through
**2026-08-05 13:09** by the explicit host execution-approval usage limit. The
source candidate is retained because its gate proves a strict generated hot
path reduction with unchanged metadata and no total-size growth, but no
runtime-performance claim is made until a matched guest measurement is
available.

Current continuation checklist:

- [x] Replace first-use GPR selection under real pressure with
  translation-observed benefit ranking.
- [x] Keep resident identities stable and add no generated runtime checks,
  branches, or eviction work.
- [x] Retain the baseline unless a candidate has identical metadata, a
  strictly smaller hot prefix, and no total-size growth.
- [x] Add split codegen and shared three-engine runtime suites, including
  low-pressure, zero-benefit, terminal, and linked-block controls.
- [x] Pass focused **39/39** JIT tests, both diagnostic configurations, and
  complete **560/560** 16 KiB plus **559/559** 4 KiB standard matrices,
  including the permissioned VNC smoke.
- [ ] Consider x22/x23 only if matched runtime evidence exceeds their
  outer-boundary cost.
- [x] Run matched 10-core Geekbench workloads after guest benchmark execution
  became available.

### G52: matched 10-core JIT versus gadgets Geekbench

On 2026-07-30, the same production executable was run twice through the
network-enabled OverlayFS launcher, changing only
`--execution-engine={jit,gadgets}`. Both guests used 10 vCPUs, 8 GiB of memory,
systemd, DHCP, `/dev/ubda1`, and the same read-only lower image. The executable
SHA-256 was
`14b9b01d4383f395d5f76205297bfd92b6ab3b4c8cc692789cfc95c76877f431`.
The lower image SHA-256 remained
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`
after both runs.

Official results:

- JIT: (result)
- gadgets: (result)

| Engine | Single-core | Multi-core | Profile wall | Single-core phase | Multi-core phase |
| --- | ---: | ---: | ---: | ---: | ---: |
| JIT | 214 | 491 | 35m 19.2s | 15m 55.6s | 19m 20.5s |
| gadgets | 77 | 495 | 55m 13.2s | 38m 16.7s | 16m 52.4s |
| JIT delta | **+177.9%** | **-0.8%** | **-36.0%** | **-58.4%** | **+14.6%** |

JIT won 15 of 16 single-core scores. Its largest gains were File Compression
(+367.4%), Photo Library (+366.7%), Object Detection (+350.0%), Ray Tracer
(+304.2%), and Object Remover (+279.7%). PDF Renderer was the only single-core
regression at -1.2%. This confirms that the direct engine is materially faster
for ordinary serial execution, rather than merely reducing launch overhead.

The multi-core aggregate was effectively tied because eight wins were offset
by eight losses:

| Multi-core workload | gadgets | JIT | JIT score delta | JIT wall delta | gadgets cores | JIT cores |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| File Compression | 584 | 1544 | +164.4% | -56.3% | 5.76 | 4.82 |
| Navigation | 1162 | 2394 | +106.0% | -46.0% | 5.64 | 4.28 |
| HTML5 Browser | 530 | 246 | -53.6% | +67.4% | 5.86 | 7.68 |
| PDF Renderer | 505 | 745 | +47.5% | -29.4% | 5.86 | 6.67 |
| Photo Library | 619 | 1795 | +190.0% | -64.2% | 4.68 | 4.43 |
| Clang | 377 | 219 | -41.9% | +70.6% | 8.99 | 9.45 |
| Text Processing | 59 | 94 | +59.3% | -36.9% | 1.45 | 4.24 |
| Asset Compression | 720 | 716 | -0.6% | +2.6% | 7.64 | 7.23 |
| Object Detection | 229 | 333 | +45.4% | -22.5% | 6.63 | 6.63 |
| Background Blur | 557 | 219 | -60.7% | +106.6% | 7.20 | 7.52 |
| Horizon Detection | 811 | 77 | -90.5% | +553.2% | 4.98 | 8.29 |
| Object Remover | 701 | 303 | -56.8% | +83.6% | 5.11 | 8.57 |
| HDR | 767 | 913 | +19.0% | -10.5% | 6.14 | 5.33 |
| Photo Filter | 415 | 388 | -6.5% | -20.3% | 3.16 | 3.16 |
| Ray Tracer | 944 | 1301 | +37.8% | -23.5% | 2.23 | 2.50 |
| Structure from Motion | 622 | 457 | -26.5% | +25.5% | 6.10 | 6.98 |

The regressions are not explained by a failure to schedule guest vCPUs. The
JIT compiler workload sustained 9.45 effective host cores, and the largest
image-workload regressions used 7.5--8.6 cores. Per-workload samples instead
identify a shared coordination bottleneck:

- HTML5 Browser spent 56.1% in `wait_for_jit_store_phase`, 17.5% in the
  exclusive helper, and 11.7% invalidating published store ranges.
- Clang spent 46.1%, 9.7%, and 3.7% in those paths, respectively.
- Background Blur spent 48.6%, 14.8%, and 11.7% there.
- Object Remover spent 44.0%, 28.7%, and 10.6% there.

Those four workloads therefore spend about 60--85% of sampled emulator time
coordinating the global direct-JIT store/exclusive-operation publication
protocol. Horizon Detection has a separate profile: 63.6% is attributed to
JIT block execution/entry while only 8.2% reaches the publication/exclusive
symbols in its top eight samples. It should be diagnosed independently.

The next performance trial should reduce the global publication protocol's
cross-vCPU serialization while preserving the existing reservation and
self-modifying-code tests. It should then rerun the affected focused
multi-core workloads before another full Geekbench comparison. The Horizon
Detection path should be inspected for frequent cold exits, helper lowering,
or short-block entry overhead as a separate proposal.

### G53-G79: publication scaling and gadget-regression recovery

The direct-engine publication protocol was progressively changed from the
globally serialized G52 design to a lock-free ordinary-store fast path.
Translation decides whether a block can publish stores directly. At runtime,
each vCPU publishes a small host-owned participant state; the uncommon
reservation helper closes the phase and waits for already-published stores
instead of making every ordinary store acquire the old global gate. Store
ranges use host backing identities, bounded overlap filtering, and the shared
reservation-version table so aliases remain correct. Generated valid paths do
not inspect guest opcodes or repeat translation-time validity checks.

The focused concurrency, reservation-alias, native-store, link, and SMP tests
were expanded alongside these trials. The accepted direct-engine result,
G68, scored **251 single-core / 875 multi-core** at
(result), a **3.49x** aggregate scaling
factor. Its 10-vCPU compiler section sustained **9.08 effective host cores**.

Later trials bounded participant scans and corrected ownership/lifetime
details shared with the gadget engine. The final G79 gadget build recovered
the earlier regression and scored **79 / 506** at
(result), a **6.41x** scaling factor.
The repaired gadget result is the final gadget comparison point; later
accepted changes in this campaign are confined to the direct engine or
semantically neutral code-publication cleanup.

### G80-G94: JIT cache-layout regression audit

A later full run exposed a direct-engine regression at **238 / 701**
(G85, (result)). Profiles and binary
layout comparison showed that the regression was not failed vCPU scheduling:
the implementation object had grown from G68's approximately 1 MiB to about
12.9 MiB per vCPU, placing large cache tables inline and increasing
initialization and working-set pressure.

The following proposals were evaluated independently:

- G80-G84 shared-template and alternative flat-cache layouts were rejected.
  Sharing introduced mutable-lifetime and ownership costs without a decisive
  workload gain; all dead implementations were removed.
- G86 restored G68's cache hash. G87-G88 restored the compact
  **104-byte descriptor / 416-byte four-way set** and removed page-epoch
  metadata that duplicated exact saved-word validation. G88 reached
  **244 / 806** at (result).
- G89 reduced the maximum translated block from 64 to eight instructions.
  File Compression exceeded 135 seconds versus approximately 53 seconds, so
  the run was stopped and the 64-instruction limit restored.
- G90-G91 moved the descriptor table and saved-word table to overwrite-
  allocated heap storage. This restored the complete implementation object to
  G68's `0x104780` bytes while preserving exact byte validation. G91 scored
  **248 / 795** at (result); many
  individual sections improved even though the multi-core aggregate varied.
- G92 removed polling for the obsolete dynamic verifier trigger. Verification
  remains selected once by `UML_VERIFY_JIT`; production cache misses no longer
  call the trigger parser.
- G93 removed early translated-terminal detection and always fetched 64
  words. File Compression improved, but Photo Library regressed from roughly
  30.2 to 44.4 seconds. The proposal was rejected immediately and early
  termination restored.
- G94 replaced the six direct high-byte checks with a 256-byte lookup table.
  Its full diagnostic run scored **250 / 932** at
  (result), the best multi-core result
  in this campaign. A matched 10-worker, 10,000-exec table/direct/table
  control then measured **19.312 / 19.137 / 19.501 seconds**. The table was
  therefore rejected and removed; the faster direct checks are final.

G94's per-workload telemetry resolves the apparent scaling concern. Compared
with the original G68 capture, all 16 completed multi-core sections used less
or equal wall time except HDR by 0.27%. Representative improvements were
Object Detection **-28.6%**, Object Remover **-33.2%**, Photo Filter
**-59.2%**, and Ray Tracer **-45.3%**. Clang used **9.00 host cores**; the
remaining low-utilization Text Processing section used **1.22**, matching the
workload-specific behavior seen in earlier engines rather than a vCPU
scheduler limit. Official sub-scores still vary because the benchmark
adapts work units between runs, so isolated score drops with substantially
shorter wall time are not evidence of an emulator regression.

Final retained direct-engine state:

- [x] G68 cache hash and compact four-way descriptors.
- [x] Heap-backed cache and lazy saved-word storage; no large per-vCPU inline
  table initialization.
- [x] Exact saved-word cache validation with no duplicate page epochs.
- [x] One release publication for each mapping-side code change.
- [x] Construction-time verification selection with no dynamic production
  polling.
- [x] Early terminal-bounded 64-instruction translation using direct checks.
- [x] No shared-template, page-epoch, eight-word-block, full-fetch, lookup-
  table, or other rejected implementation remains.

### G95: final direct-check qualification and desktop graphics baseline

The accepted direct-check source was rebuilt as
`.build/cmake-macos/bin/uml-darwin`, SHA-256
`9320cb9f5c2a3d1ab3b5238c6a0cf3c660e870344fe303789f095dba42c61bbf`.
The final network-enabled 10-vCPU, 8 GiB Geekbench run scored
**251 single-core / 843 multi-core**:
(result).

This exactly matches G68's best single-core aggregate. The multi-core result
is 3.7% below G68 and 9.5% below the G94 lookup-table diagnostic, but remains
66.6% above the repaired final gadget result. A live compiler-workload
snapshot measured approximately 800% host CPU, confirming that the earlier
one-core scheduling failure is absent. G94's per-section profiles also showed
that every completed multi-core section except one 0.27% tie/noise case used
less or equal wall time than G68. The benchmark changes work units
between runs, so its aggregate and sub-scores are not a stable substitute for
the matched table/direct/table cold-path control. That control selected the
direct checks (19.137 seconds versus 19.312 and 19.501 seconds for the table),
so the lookup table remains rejected.

Final correctness and integration qualification:

- production 16 KiB: **562/562** non-VNC tests plus the standalone VNC smoke;
- debug interpreter: **560/560** non-VNC tests plus the standalone VNC smoke;
- 4 KiB compatibility: **563/563** non-VNC tests plus the standalone VNC
  smoke;
- register-pressure instrumentation: focused tests pass in both 16 KiB and
  4 KiB builds;
- standalone non-pressure LTP: **2,142/2,142 accepted** in 631.1 seconds,
  with all 374 host-pressure cases excluded as intended.

The stale boundary-state test was corrected to use ordinary production-profile
instructions. Optional feature-family behavior remains covered by its
dedicated feature-enabled suites; the boundary test now exclusively validates
guest-owned key-state initialization and persistence.

The requested desktop-mode WebGL Aquarium run used JIT, 10 vCPUs, 8 GiB,
networking, KDE/SDDM, Chromium, VNC capture, and the read-only first partition
of `rootfs.img` beneath a tmpfs OverlayFS. After a 15-second warm-up, 30
steady-state samples measured **13.933 average FPS**, **7 minimum**, and
**17 maximum**. The validation gate confirmed **113/113 resources**, **500
fish**, a **1024x1024** canvas, and WebGL 2.0. The captured framebuffer shows
the complete textured scene. Artifacts are under
`/private/tmp/aquarium-final-jit/20260731-131241/`.

The canonical `rootfs.img` SHA-256 remained
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`
after Geekbench and Aquarium, proving that guest changes remained volatile.

## G96: measurement rules for the gadget-first campaign

Started 2026-08-06. This section adds no runtime change. It builds the two
measurement controls that the next campaign's acceptance rule depends on, after
a review of every trial above found two method problems that make part of the
existing rejection set unsafe to rely on for the iOS target.

### Acceptance rule for the following trials

One optimization at a time, in isolation on the last accepted build, measured
before the next begins. A change is accepted only when it passes every
correctness gate and satisfies one of:

1. the 10-vCPU Geekbench result improves, judged on **single-core and
   multi-core together** — both phases improve, or one improves while the other
   moves only within the noise band. A small gain in one phase does not buy a
   significant loss in the other; that combination is rejected unless the
   regression is diagnosed and fixed;
2. the targeted focused benchmark improves with **no** Geekbench regression
   beyond run-to-run variation, in either aggregate or any per-workload
   section.

This is stricter than the rule used through G35, under which G30 was accepted
at a 0.83% single-core loss and G37 at −2.5% single-core / +17.3% multi-core.
Both would now need the regressed phase inside the noise band, or explained and
repaired first. Establishing that noise band from at least three baseline runs
is a prerequisite for the first trial, not an afterthought.

### Problem 1: gadget-body results were confounded by bank layout

G17 was rejected because "shrinking the large generated gadget region shifted
later hot logical entries, and the layout effect outweighed the instruction
reduction". G30 hit the same effect and had to restore "its established 64-byte
phase". G35 later established the remedy — unreachable padding after each
indirect dispatch preserves every later gadget's address and cache-line phase —
but by then G17 and G36 had already been discarded.

`tests/tooling/cmake/test_gadget_bank_layout.py` makes that remedy checkable.
It records each defined `uml_arm64_gadget_*` entry's offset relative to the
first gadget, its 64-byte cache-line phase, and its size, then compares two
builds and fails on any entry the trial did not declare in an acknowledgement
file. Comparison accepts either a written baseline (`--baseline`) or the
accepted build's objects directly (`--baseline-objects`).

The comparator carries its own negatives: an always-executed self-test proves
it detects a move, a phase-only change, an addition, and a removal, that a size
change stays advisory, and that acknowledgement globs neither over- nor
under-apply. Two out-of-tree mutation checks confirmed the registered
`performance-runner-cli` contract fails when `core_class` forwarding is dropped
from a runner, when an unknown core class is accepted, and when the efficiency
prefix is silently ignored.

Registered as the `gadget-bank-layout-snapshot` tooling test, which writes
`reports/gadget-bank-layout.tsv` for every build so an accepted build always
leaves behind the evidence a later trial compares against.

Calibration on two existing builds of this tree: exactly **one** gadget grew by
16 bytes, and **5,925 later entries moved and changed 64-byte phase** as a
result. That is the confound, quantified: a single-gadget edit relocates more
than half the bank unless it is padded.

### Problem 2: every rejection was measured on performance cores

G8 (−0.06%), G17 (−0.36%), G23 (−0.24%), G23b (−2.8%) and G36 (noise) were all
measured on a Mac, so on P-cores. iOS defaults to the gadget engine
(`UML_DEFAULT_EXECUTION_ENGINE_JIT` is `$<BOOL:${UML_TARGET_MACOS}>`, and the
JIT requires a `MAP_JIT` arena) and runs heavily on E-cores, which have much
smaller reorder windows and fewer load units. G8's conclusion — that threaded
indirect dispatch rather than gadget-body instruction count is the limit — is
therefore a P-core result that may not hold for the platform that matters most.

The four `tests/perf` runners now take `--core-class={performance,efficiency}`.
The efficiency mode launches the emulator through `taskpolicy -c background`,
whose process-wide QoS clamp Apple's scheduler honours by placing the task on
the efficiency cores; `taskpolicy` execs in place, so `process_id` and the
existing host-CPU metrics are unaffected.

Verified on the measurement host, an **Apple M1 Ultra with 16 performance and
4 efficiency cores**: a four-thread native spinner takes **0.213 s**
unclamped and **0.907 s** clamped, a 4.3x slowdown consistent with confinement
to four E-cores. `taskpolicy -b` was rejected for this purpose — it leaves the
thread QoS class unchanged (33) while `-c background` sets it to 9.

Read E-core numbers accordingly: with only four efficiency cores on this host, a
10-vCPU guest is heavily oversubscribed there, so **E-core runs are evidence
about per-instruction cost, not about multi-core scaling**. Use the single-worker
focused probes for the E-core half of a gadget trial.

**Topology rule: efficiency-core runs use `--cpus 4`, performance-core runs use
`--cpus 10`.** Four vCPUs match the four efficiency cores, so the E-core
measurement reflects the change under test rather than guest-scheduler queuing.
The two core classes are therefore never compared to each other; each is
compared only against the same core class on the accepted build.

- [x] Add the gadget bank layout comparator, its self-test, and the
  `gadget-bank-layout-snapshot` registration plus manifest row.
- [x] Add `--core-class` to all four performance runners and extend the
  registered CLI contract to require the forwarding.
- [x] Confirm the efficiency clamp reaches spawned threads by measurement
  rather than by API inspection.
- [x] Capture the three-run Geekbench noise band that the acceptance rule
  requires, at 10 vCPUs with `--engine gadgets`. Recorded below.
- [ ] Re-measure G8, G17 and G36 on both core classes with the bank layout
  pinned, and record whether any of them reopens.

> **Later disposition (2026-08-12): superseded without another run.** This
> checklist predates the prospective cross-core policy. G8 (-0.06%), G17
> (-0.36%), and G36 (noise) do not clear its mandatory `P > 2%` first gate, so
> an E-only rerun cannot accept them. G106 subsequently held all 9,663 gadget
> positions fixed while removing 3,968 instructions and found overlapping P/E
> controls, supplying the missing class-level evidence that noncritical
> instruction-count reductions remain flat on E. The old artifacts are not
> current matched arms, G17 is layout-confounded, and no G36 candidate survives.
> This item is therefore closed rather than checked off or rewritten. Reopening
> one exact transform requires a current layout-pinned prototype and P-first
> screen; do not spend an E campaign unless its experiment primary is strictly
> above 2%.

### The noise band

Three matched runs on the accepted build (turnstile fix + G97 page hash, with
B-4 reverted so the baseline carries no unmeasured change), 10 vCPUs,
`--engine gadgets`, 8 GB, same rootfs and host. Zero RCU stalls and all 32
workloads in every run.

| Run | Result | SC | MC |
| --- | --- | ---: | ---: |
| 1 | (result) | 77 | 473 |
| 2 | (result) | 77 | 481 |
| 3 | (result) | 77 | 477 |

**Aggregates.** Single-core is 77 in all three runs - zero spread, so any
movement at all in the single-core aggregate is signal. Multi-core is 477 mean
with a full range of 8 points, **1.7% peak to peak**; treat a multi-core
aggregate move below roughly 2% as indistinguishable from noise.

**Per-workload.** The aggregates are far steadier than the sections, which is
the trap this band exists to prevent.

| | worst section | spread |
| --- | --- | ---: |
| Single-core | Text Processing, Ray Tracer | 2.1% |
| Multi-core | **Photo Filter** | **8.1%** |
| Multi-core | PDF Renderer | 6.1% |
| Multi-core | Clang | 5.6% |

Nine of sixteen single-core sections moved by at most one point and six did not
move at all, so single-core per-workload evidence is strong. Multi-core
per-workload evidence is weak for Photo Filter, PDF Renderer and Clang, and a
single-run multi-core section delta under about 8% for those three establishes
nothing.

**Host condition.** Measured deliberately as-is rather than on an idle host, to
stay comparable with the runs already recorded: a stuck `ReportCrash` agent
(~0.8 core, 261 hours of accumulated CPU) and Chrome Remote Desktop (~1.3 core)
were left running. Roughly 4.9 of 20 cores were busy before the guest started.
An idle host would give a tighter band, at the cost of a step change against
every earlier result in this log.

## G97: guest page addresses hashed onto four buckets in the batch planner

The refactor at `d15e98c` introduced a per-batch planning index in
`AddressSpace::apply_batch_locked()`:

```cpp
constexpr std::size_t maximum_plan_reserve = 64 * 1024;
...
std::unordered_map<std::uint64_t, std::size_t> page_indices;
page_indices.reserve(plan_reserve);
```

Three facts combine badly:

- `maximum_plan_reserve` is a **power of two**, and it is the value
  `plan_reserve` saturates to for exactly the large batches that matter;
- libc++ keeps a power-of-two bucket count when `reserve()` asks for one, and
  then constrains a hash by masking rather than by a modulus;
- `std::hash<std::uint64_t>` is the identity, and a guest page address has
  `log2(page_size)` low zero bits.

So a 65,536-bucket index reaches `65536 / 16384 = 4` buckets at 16 KiB pages.
Measured directly against the host toolchain, inserting 65,536 page-aligned
keys:

| `reserve()` | buckets | occupied | longest chain | time |
| --- | ---: | ---: | ---: | ---: |
| none | 102,877 | 65,536 | 1 | **0.002 s** |
| 1,024 | 65,536 | 4 | 16,384 | **3.274 s** |
| 65,536 | 65,536 | 4 | 16,384 | **2.590 s** |
| 65,537 | 65,537 | 65,536 | 1 | **0.001 s** |

Filling the index is quadratic, and it runs with `mappings_mutex` held and — on
the destructive path — the execution gate closed and every sibling vCPU
stopped.

**How it was found.** Burst samples taken at live Geekbench workload-23 RCU
stalls looked empty because `sample` cannot symbolize the UML kernel, which
loads at its link address near `0x6000_0000_0000`; nearly every frame in a
32 MB capture printed as `??? (in <unknown binary>)`. Resolving those addresses
against `nm` output from the matching `vmlinux.unstripped` showed one carrier
spending **1,255 of 1,255 samples** — the entire ten-second capture — inside

```
__um_tlb_sync+0x4c4 -> darwin_uml_static_vm_batch_end -> apply_batch_locked
  -> page_for lambda -> std::__hash_table::__emplace_unique_key_args
```

with every other emulator thread idle. Both hot frames are the index's hash
operations.

**Change.** `GuestPageHash` folds the page number through a multiply-shift
finalizer, so the distribution no longer depends on the bucket count the
container happens to choose. The reservation and its allocation checkpoint are
unchanged. `GenerationState::mappings` deliberately keeps `std::hash`: it is
never `reserve()`d, so libc++ grows it through primes, and changing it would
perturb the hot `translate()` path inside the same trial.

`static-mmu-batch` gained `test_page_index_hash_spreads_page_aligned_addresses()`,
which asserts the bucket distribution rather than a wall-clock bound, so the
guarantee does not depend on host speed.

- [x] 23/23 MMU, memory and batch tests including the two TSan variants.
- [x] Full 16 KiB unit suite, 618/618. (One unrelated failure first:
  `source-tree-cxx-only` flagged a Finder `.DS_Store` in the repo root as
  obsolete audit debris; removed.)
- [x] Stall-catch Geekbench, 10 vCPUs, `--engine gadgets`.

### Result: accepted

| Build | SC | MC | Text Processing MC | RCU stalls |
| --- | ---: | ---: | ---: | ---: |
| pre-refactor `b931e9b` | 81 | 514 | 61 | 0 |
| post-refactor baseline | 79 | 471 | 33 | 2 |
| turnstile fix, run 1 | 78 | 464 | - | 1 |
| turnstile fix, run 2 | 78 | 458 | - | 3 |
| turnstile fix, run 4 | 79 | 435 | 20 | 4 |
| + rejected batch-scan trial | 78 | 456 | - | 6 |
| **+ G97 page hash** | **78** | **478** | **60** | **0** |

(result), 3,285 s wall against 3,375 s
and 3,520 s for the two runs it replaces.

Against the last accepted build (turnstile fix): single-core flat at 78, and
multi-core **+3.0% / +4.4%** over its two runs. Both aggregates satisfy
condition 1, so the change is accepted.

The mechanism shows in one workload. Multi-Core Text Processing is the only
Geekbench section that both stalls and fails to scale, and its score tracks the
stall count exactly - 20 at four stalls, 33 at two, **60 at zero**, against 61
before the refactor. That section is now fully recovered; every RCU stall in
this investigation landed inside it.

**What remains.** A broad residual regression against pre-refactor survives and
is *not* this bug: single-core 78 vs 81 and multi-core 478 vs 514.

**Corrected once the noise band existed.** This paragraph originally described
the residual as a uniform tax and cited multi-core Photo Filter at -14.2% as
its worst case. Both readings were wrong. Photo Filter is the noisiest section
in the suite at 8.1% run to run, so it never supported a conclusion. And the
residual is not uniform: it is a -2.5% to -3.5% floor plus a tail on the
translation-heavy workloads in single-core (Clang -11.8% with zero run-to-run
spread, Object Remover -9.6%, File Compression -8.3%), while multi-core is led
by Navigation -12.2% and File Compression -12.0% with Clang only -7.7%. Those
are two different causes, worked separately: see `REFACTOR_HANDOFF.md` for the
decomposition, the lost cross-translation-unit inlining candidate, and the
separate MMU-path investigation the multi-core half needs.

## G98: B-4 step 1, guest YIELD lowered to host ISB

Guest `YIELD` is what glibc's aarch64 `atomic_spin_nop()` and the kernel's
`cpu_relax()` emit while waiting on a lock. Arm defines its only architectural
effect on symmetric multi-threading hardware, which Apple Silicon does not
have, so lowering it to a host `YIELD` is a complete no-op: a waiting guest
thread keeps emulating its spin loop at full speed for its whole host quantum.
`ISB` is the substitution Arm's own porting guidance gives in place of x86
`PAUSE`. It is strictly stronger than the hint it replaces, so nothing
guest-visible changes, while the pipeline restart throttles the spin.

Applied in both engines: `gadget yield` in
`darwin/cpu/arm64/gadgets/engine/system_atomic.inc`, and `HINT #1` in
`emit_primary_hint()` in `darwin/cpu/arm64/jit/jit_compiler.cpp`. One
instruction and one emitted word either way.

### Correctness

- Unit preset 572/572, tooling preset 45/45.
- Bank layout pinned per the G96 rule, and this is the first trial to use that
  comparator in anger: snapshots taken with and without the change are
  **byte-identical across all 9,664 gadget entries**, so it moved no entry and
  changed no 64-byte cache-line phase.

### Result on Geekbench: neutral

(result), 10 vCPUs, `--engine gadgets`,
zero RCU stalls, all 32 workloads.

| | band | B-4 |
| --- | --- | ---: |
| Single-core | 77, 77, 77 | 77 |
| Multi-core | 473, 481, 477 | 478 |

Single-core is identical and multi-core lands mid-band, so neither aggregate
moves. Per-workload, single-core tilts very slightly negative — Object Remover
111 against a band of 113 in every run, and several sections one point low —
but nothing clears the noise. **This does not meet acceptance condition 1.**

### Why Geekbench cannot decide this one

The measurement host has 16 performance cores and the run uses 10 vCPUs, so the
guest is never oversubscribed and a guest thread essentially never waits on a
descheduled vCPU. That is precisely the condition the change exists to improve,
and it is the condition iOS lives in. Condition 2 therefore needs a focused
benchmark, and none of the 30 existing probes could supply one: every one of
them either owns its data, like `guest_atomic_add_loop`, or runs as independent
parallel copies, so no guest thread ever waits on another and the hint is never
reached under contention.

`tests/perf/guest_yield_spin_contention_loop.S` was written to close that gap.
`BENCH_THREADS` threads share one address space through `clone(CLONE_VM)` and
contend for a single test-and-set lock, so at any moment one thread holds it and
every other sits in the `YIELD` backoff. Wall time is then governed by how much
host throughput the waiters consume while the holder works. The critical section
does an unlocked read-modify-write on a shared counter on purpose: that is safe
only if the lock is correct, so the final count is also a proof that mutual
exclusion held for every measured acquisition. Registered in `probes.tsv` and
run with `--workers 1`, since the probe generates its own contention.

### Result on the focused probe: rejected

Both configurations, `--workers 1 --repetitions 5 --shell-init`, median of five:

| Configuration | baseline | B-4 | |
| --- | ---: | ---: | --- |
| Performance cores, 10 vCPUs | 4.210 s, 9.43 cores | 4.364 s, 9.50 cores | **+3.7% slower** |
| Efficiency cores, 4 vCPUs, 10 guest threads | 5.477 s, 2.00 cores | 5.756 s, 2.05 cores | **+5.1% slower** |

The second row is the oversubscribed, iOS-shaped case the change was written
for: ten guest threads on four vCPUs on four efficiency cores, so a thread
genuinely does spin while the lock holder is descheduled. B-4 is slower there,
and slower by *more* than on the unloaded performance cores. It also consumes
slightly more host CPU in both. Run-to-run spread is about 5%, and four of five
B-4 runs sit above the baseline median in each configuration.

**Rejected.** Neutral on Geekbench and consistently negative on the one
benchmark built to give it its best case.

**Why the reasoning was wrong.** The premise was right — a host `YIELD` really
is a no-op on Apple Silicon, so the hint does nothing today. The error was
assuming the replacement must therefore be an improvement. `ISB` throttles the
*spinning* thread by flushing its pipeline, and on a core with no second
hardware thread there is no sibling to hand those fetch and issue slots to, so
the throttling buys nothing. What it does buy is latency: a slower spinner
takes longer to observe that the lock was released, which lengthens every
handoff. That is why the penalty grows under oversubscription, where handoffs
are most frequent. Any future attempt at B-4 step 2 should treat the hint as a
scheduling signal — exiting the block and yielding the host thread, as the plan
itself proposes — rather than as an instruction to substitute.

## G99: interprocedural optimization for the gadget translation library

Accepted. This is the first repair of the post-refactor regression.

### Mechanism

Gadget translation was one 14,160-line translation unit before the refactor
(`darwin/backend/cpu/arm64/gadgets/program.cpp`) and is now fifteen, the largest
2,926 lines. `Builder::build()` calls `decode_branch_system`,
`decode_data_processing`, `decode_floating_simd`, `decode_load_store` and
`decode_exception` **once per guest instruction translated**; every one is a bare
declaration in `program_builder_decode.hpp` defined in a separate `.cpp`, and
each calls on into the `gadget_*_selection.cpp` units.
`gadget_selection_api.hpp` is types plus 182 declarations with no inline bodies.
The build is `Release`, `-O3 -DNDEBUG`, with no LTO and no unity build, so the
entire decode -> select -> emit chain the compiler used to inline sits behind
hard call boundaries.

Neither obvious repair is available. `verify_gadget_builder_ownership()` in
`test_engine_object_boundaries.py` requires all five `program_builder_*.cpp.o`
to exist as distinct objects, which a unity build cannot satisfy, and forbids
textual `.cpp/.inc/.inl/.ipp` composition and `*_impl` fragments, which rules
out header inlining. Link-time optimization is the only route that changes
neither source organization nor object file names.

### The change

`INTERPROCEDURAL_OPTIMIZATION` on `uml_arm64_gadgets`, guarded by
`check_ipo_supported()`, in `darwin/cpu/CMakeLists.txt`. CMake selects
`-flto=thin` for Clang.

### Correctness

- Unit preset 572/572, tooling preset 45/45, production static-link closure
  verified.
- The thirteen C++ objects become LLVM bitcode. Apple `nm` still reads them, so
  `test_engine_object_boundaries.py` keeps working; `engine.S` still assembles
  to an ordinary Mach-O object, so the engine-entry disassembly check is
  untouched.
- Bank layout: **no gadget entry moved and no 64-byte phase changed.**
- The feared cost of LTO at roughly a hundred `$<TARGET_OBJECTS:>` consumers did
  not materialise: a full ThinLTO rebuild of the unit preset takes 76 s.

### Result

(result), 10 vCPUs, `--engine gadgets`,
zero RCU stalls, all 32 workloads.

| | band | G99 | |
| --- | --- | ---: | --- |
| Single-core | 77, 77, 77 | **78** | +1 on a band with zero spread |
| Multi-core | 473, 481, 477 | **497** | **+4.2%** against a 1.7% band |

Both aggregates improve, so this meets acceptance condition 1. Multi-core is 16
points above the highest of the three band runs.

Against pre-refactor (18926314), this recovers **54% of the multi-core
regression** (477 -> 497 of the way to 514) and 25% of single-core (77 -> 78 of
the way to 81).

### The mechanism is real but was not the one predicted

The prediction was that translation-heavy workloads would gain most, and above
all single-core Clang, which the workload profile puts at 19% cold translation
and which had the largest single-core deficit at -11.8%. That is not what
happened. **Single-core Clang moved 45 -> 44, slightly down.** The gains landed
instead on the workloads whose *multi-core* deficit was worst:

| Workload | band | G99 | vs band |
| --- | ---: | ---: | ---: |
| Navigation MC | 1047-1079 | 1158 | +79 over the band maximum |
| File Compression MC | 553-565 | 601 | +36 |
| HTML5 Browser MC | 533-543 | 578 | +35 |
| Object Remover MC | 655-663 | 686 | +23 |
| Object Remover SC | 113 in every run | 118 | +5 |
| Clang MC | 344-364 | 379 | +15 |
| **Clang SC** | **45 in every run** | **44** | **-1** |

So the win is not confined to the decode families at all. It is spread across
the library, which points at the *execution*-time helpers rather than the
translation-time ones: `complex_memory_helpers.cpp`, `software_helpers.cpp` and
`gadget_memory_selection.cpp` are called from gadget bodies at run time, and
Navigation and File Compression are the memory-heavy workloads. Inlining across
those units is the plausible source of a gain that tracks memory intensity
rather than translation volume.

Clang single-core remains the one section that has not recovered anything, and
it is the cleanest signal in the suite, with zero run-to-run spread across three
band runs. ThinLTO imports across modules heuristically and declines expensive
imports; the decode families are exactly the large functions a cost model
refuses. That is the specific reason to keep going with textual composition,
which forces them into one unit rather than leaving it to a heuristic.

### One regression to be honest about

The G96 bank layout comparator now sees **9,577 of 9,664 entries: 87 fewer**.
The missing ones are gadgets defined in C++ rather than assembly, whose symbols
the comparator cannot resolve inside a bitcode object. No entry moved - every
surviving row is byte-identical - but audit coverage dropped by 0.9%. The fix is
to read the final linked image rather than the objects, which would also make
the comparator independent of how the objects are produced. Not done here.

## G100: textual composition of the gadget decode families

Accepted, and it completes the single-core half of the post-refactor repair.

### Why this was needed after G99

G99 restored cross-unit inlining through ThinLTO and recovered 54% of the
multi-core regression, but it left **single-core Clang at 44 against 45 in the
band and 51 pre-refactor** - the most translation-heavy workload, the one the
mechanism predicted would gain most, and the cleanest signal in the suite with
zero run-to-run spread across three band runs. ThinLTO imports across modules on
a cost model that declines large functions, and the five decode families are
exactly that: 45 to 1,690 lines each of switch dispatch. Link-time optimization
was never going to inline them; only putting them in one translation unit would.

### The change

The five `program_builder_<family>.cpp` become
`program_builder_<family>_impl.ipp` fragments, composed by `program_builder.cpp`,
so `Builder::build()` and the whole decode -> select -> emit chain compile as one
unit again. The fragments moved verbatim - every family already had an identical
two-line `#pragma once` include prologue and its own namespace block - so this is
a build-shape change, not a code change.

`.ipp` rather than `.inc` for two reasons: every existing `.inc` in this tree is
an assembly fragment of the gadget bank (`engine/core_integer.inc` and its
siblings), so reusing that suffix for C++ would overload an established
convention; and editors do not recognise `.inc` as C++, while `.ipp` is in the
stock VS Code C++ language definition alongside `.inl`, `.tpp` and `.ixx`.

`verify_gadget_builder_ownership()` in `test_engine_object_boundaries.py` forbade
exactly this, and was rewritten rather than deleted. It now pins that the
composition stays disciplined instead of pinning that the families compile apart:
one fragment owns exactly one family, no fragment defines another family's
function, the private interface still only declares them, `program_builder.cpp`
is the sole composer and includes exactly the five, no fragment includes another,
and no family may regain its own object. A second hard-coded object list further
down the same audit needed the same update.

**Mutation-tested, since a rewritten audit that cannot fail is worse than none.**
Dropping one include from the composer fails with "program_builder.cpp does not
compose exactly the decode family fragments"; adding a second includer fails with
"decode family fragments have extra includers: ['program_execution.cpp']"; both
pass again when restored.

### Correctness

Unit preset 572/572, tooling preset 45/45, production static-link closure
verified, and no gadget entry moved or changed 64-byte phase.

### Result

(result), 10 vCPUs, `--engine gadgets`,
zero RCU stalls, all 32 workloads.

| | band | G99 LTO | **G100** | pre-refactor |
| --- | --- | ---: | ---: | ---: |
| Single-core | 77, 77, 77 | 78 | **80** | 81 |
| Multi-core | 473, 481, 477 | 497 | **495** | 514 |

Single-core gains two points on a band with **zero** spread, so the improvement
is unambiguous. Multi-core moves -0.4%, far inside the 1.7% band. One aggregate
improves while the other stays within noise, which is acceptance condition 1.

**The predicted mechanism finally shows up.** G99's gains landed on memory-heavy
workloads and missed translation entirely; G100's land exactly where the theory
said they would:

| Single-core | pre | band | G99 | **G100** |
| --- | ---: | ---: | ---: | ---: |
| **Clang** | 51 | 45 | 44 | **50** |
| **Object Remover** | 125 | 113 | 118 | **128** |
| **File Compression** | 102 | 93.5 | 95 | **102** |
| Asset Compression | 87 | 84 | 86 | 87 |
| HDR | 102 | 99.5 | 101 | 102 |
| Text Processing | 50 | 48.5 | 49 | 50 |
| Ray Tracer | 101 | 95.5 | 98 | 100 |

Clang recovers 45 -> 50 against 51 pre-refactor, a **+13.6%** move on the section
LTO could not shift at all. File Compression, Asset Compression, HDR and Text
Processing return to their pre-refactor values exactly, and Object Remover at 128
**exceeds** its pre-refactor 125.

### Where the campaign stands against pre-refactor

| | pre-refactor | post-refactor band | now | recovered |
| --- | ---: | ---: | ---: | ---: |
| Single-core | 81 | 77 | **80** | **75%** |
| Multi-core | 514 | 477 | **495** | **49%** |

Single-core is all but repaired: one point short, with several sections at or
above their pre-refactor scores. The multi-core remainder is not this cause -
it is led by Navigation and File Compression and scales with concurrency, which
points at the MMU translate and miss path the refactor rewrote. That is the next
investigation and it is tracked separately.

## G101: B-1 step 1, the address space resolved once per thread

Accepted. This is the multi-core half of the post-refactor repair that G100
identified but did not fix.

### The cause of the multi-core remainder

G100 recorded that the remainder was "led by Navigation and File Compression and
scales with concurrency, which points at the MMU translate and miss path". It
does, and the diff against `618b686` names the mechanism.

Before the engine split, `darwin/runtime/mmu/address_space.cpp:1183` reached the
address space through

```cpp
AddressSpace &address_space()
{
	static AddressSpace instance;
	return instance;
}
```

a plain reference to a process-global instance. A software-TLB miss therefore
paid **one** lock: the `shared_lock` on the generation's `mappings_mutex` inside
`translate()`.

Domain support replaced that with a registry lookup. The same miss now pays:

| Step | Added by the refactor |
| --- | --- |
| `prune_retired_thread_state()` | four thread-local structures, each an acquire load |
| `AddressSpaceRegistry::current()` | `shared_lock` on a process-global `shared_mutex`, `unordered_map` find, `shared_ptr` copy, two retirement-sequence loads |
| `AddressSpaceAccess` constructor | a **second** `shared_lock`, on `lifecycle_mutex_`, held for the whole operation |
| `AddressSpace::translate()` | unchanged - `shared_lock` on `mappings_mutex` plus the find |

Three `shared_mutex` acquisitions where there was one. libc++'s `shared_mutex` is
not a cheap reader-writer lock - `lock_shared()` takes a real internal
`std::mutex` - so "shared" readers serialize on one cacheline instead of
proceeding in parallel. Ten vCPU threads missing concurrently queue, which is
why the cost is a multi-core cost.

### The change

The resolved owner is cached per thread and validated by one comparison against
the global retirement sequence, which `retire_domain()` advances **before** it
touches the registry, so a cache hit proves no retirement has begun since the
entry was published. A hit still returns an owning reference: dropping to a raw
pointer would rest on the lifecycle lock alone to keep the object alive, and the
refcount round-trip is far cheaper than the lock and hash it replaces.

`test_software_resolution_cache_drops_a_retired_domain()` pins the invalidation.
It reuses the **same domain id** after retirement, so a cache validated on the
domain alone hands back the retired space, which then reports `ESHUTDOWN` and
creates no replacement. **Mutation-tested:** deleting the sequence comparison
fails `memory-domain-lifetime`, and it passes again when restored.

### A probe had to be written first

No existing probe reaches the miss path. `guest_tlb_128m_alias_loop` touches
exactly two pages, which sit in the software TLB from the second iteration
onward - it was built to expose set aliasing in the direct index, not to
generate misses - and `guest_memory_site_loop` hammers a single site. Measured
on both builds the alias probe reports 0.101 s against 0.101 s: it cannot see
this change at all.

`tests/perf/guest_mmu_miss_walk_loop.S` walks 65,536 pages, twice the 32,768
entries of the 16,384-set two-way TLB, so a linear pass evicts each entry before
reaching it again and every access is a miss. `BENCH_THREADS` threads share the
address space through `CLONE_VM`, the shape the multi-core phase produces; each
thread keeps its own TLB, so sharing the region does not turn their misses into
hits. Every pass sums the first word of each page against what the parent
seeded, so a run reporting OK has proved all 65,536 translations resolved to the
right host page, for every thread and every pass.

| median of 3, P-core | candidate | baseline |
| --- | ---: | ---: |
| 10 threads, pass A | 5.117 s | 6.325 s |
| 10 threads, pass B (reversed) | 4.769 s | 6.140 s |
| 1 thread, pass A | 0.603 s | 0.648 s |
| 1 thread, pass B (reversed) | 0.623 s | 0.642 s |

About 21% at ten threads and 4% at one, with host CPU falling from 44-50 s to
36-38 s on the ten-thread run, so this is less work rather than better
scheduling. E-core runs at `--cpus 4` were inconclusive in both directions
(candidate 15.691 s and 13.432 s, baseline 14.818 s and 15.598 s): ten probe
threads on four efficiency cores makes wall time a scheduling result. Recorded
as measured rather than argued into a conclusion.

### Result

Four runs, two matched pairs, orders reversed, 10 vCPUs, `--engine gadgets`,
zero RCU stalls, all 32 workloads in every run.

| | baseline | candidate |
| --- | ---: | ---: |
| pair 1, baseline first | 79 / 479 (result) | 77 / 492 (result) |
| pair 2, candidate first | 78 / 486 (result) | 79 / 488 (result) |
| **mean** | **78.5 / 482.5** | **78.0 / 490.0** |

**Single-core is unchanged**: the ranges overlap completely, 78-79 against
77-79. **Multi-core improves 1.55%, and the two sets do not overlap** - the
candidate's worst run, 488, beats the baseline's best, 486, and both candidate
runs beat both baseline runs. One aggregate improves while the other stays
inside its noise, which is acceptance condition 1.

Eleven of sixteen multi-core sections improve, led by Photo Filter +8.5%,
HDR +5.8%, Object Remover +5.2% and Horizon Detection +4.2%.

**One section regresses and is not explained: Navigation multi-core, 1115 and
1133 against 1089 and 1083, about -3.4% with no overlap.** That is the section
that led the post-refactor multi-core regression, so the expectation was that it
would gain most. It is recorded as an open follow-up rather than waved away; the
likely place to look is that Navigation's miss pattern is dominated by one
generation's `mappings_mutex`, which this change does not touch, so removing the
registry lock may only have moved the queue onto it. B-1 step 2 replaces exactly
that lock.

### Method notes, which cost this trial two extra runs

**A cross-day comparison is not safe.** The first candidate run read 78/487 and
was compared against G100's 80/495 from the previous day, which looked like a
2-point single-core regression. Rerunning the *unchanged* accepted build the next
day gave 79/479 - the same code moved 80 to 79 single-core and 495 to 479
multi-core between days. Every trial from here compares against a control run
back to back with it, never against a number already in this log.

**A single pair overstates.** Pair 1 alone put the multi-core gain at +2.7%; with
the reversed repeat it is +1.55%. Pair 1 alone also showed single-core 79 to 77,
which the repeat erased - Clang single-core went 48, 45, 49, 46 across the four
runs, an 8.9% swing between two runs of the *same* candidate binary. The G96
band's zero single-core spread describes three runs of one build in one sitting
and does not survive across sittings.

**Host condition is now recorded rather than assumed.** Each run samples total
host busy cores every 60 s; the pairs above ran at medians of 4.40 and 4.43
cores. A Time Machine backup overlapping the first run was initially treated as
grounds to discard it. That was wrong: the guest uses 10 of 20 host cores, the
backup was network-bound to a remote volume at roughly 290 KB/s, and one core of
background work cannot move a single-threaded phase. Record host condition, do
not reject results for it.

**Phase wall time is an independent check.** The console log timestamps each
phase and the two agreed throughout - in pair 1 the candidate's single-core phase
took 41 minutes against 37 and its multi-core phase 12 against 16.

### Code layout, measured and set aside

`__text` grew 820 bytes, so every object linked after the MMU shifted. Comparing
the two images by symbol: **9,576 of 9,663 gadget entries are at identical
addresses**, and the 87 that moved are the C++-defined gadgets, all displaced by
exactly +752 bytes, which changes their 64-byte cache-line phase. This was the
leading hypothesis for the apparent single-core regression until the repeats
showed there was nothing to explain. It is recorded because the effect is real
and unpinned - G35's padding rule and `gadget-bank-layout-snapshot` cover the
assembly bank, and nothing covers compiled code - so a future trial that does
show an unexplained single-core move on a size-changing build should check it.
Where a mechanism can be switched at run time, measuring both arms from one image
removes the question entirely.

## G102: B-3 step 1, ISB in place of the no-op YIELD - rejected

Rejected on a **8.5% JIT multi-core regression** with no overlap between four
runs. Reverted. The architectural premise is correct and the conclusion drawn
from it is not.

### The premise

Arm gives YIELD an effect only on hardware with simultaneous multi-threading,
which no Apple Silicon part has, so `yield` on this host is a plain NOP and the
four spin loops in this tree have no backoff at all:
`jit_gate_value_changed` (jit_publication.hpp), the `has_published_jit_store`
drain loop (jit_publication_state.cpp), `ExclusiveMemoryMutex`
(load_store_memory.hpp) and `Ls64Lock` (complex_memory_helpers.cpp). ISB is a
pipeline restart rather than a hint, so it costs the spinner real latency and
gives the machine back, and it is always safe where a hint was safe.

### What was measured

A `platform/spin_wait.hpp` with `spin_hint()` replaced `yield` at all four
sites. Gates passed: 572/572 unit, 45/45 tooling.

**The measurement has no layout confound at all.** ISB and YIELD are both
four-byte instructions and the header is header-only, so the candidate's
`__text` came out **byte-identical to the baseline at 3,867,900**, and zero of
9,663 gadget entries moved. Whatever moved the score is the instruction itself.

Screening first found that no probe in `tests/perf` reaches the two gadget-path
sites: `guest_atomic_add_loop` uses `ldaddal` and
`guest_reservation_store_fallback_loop` uses `ldxr`/`stxr`, and both lower to
native host instructions rather than to the `exclusive_memory_mutex` fallback.
The measurable site is therefore the JIT publication gate, which G52 put at
44-56% of sampled emulator time for four multi-core workloads. Measured with
`--engine jit`, 10 vCPUs.

| JIT | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 237 | **790** |
| baseline (result) | 235 | **811** |
| ISB at all four sites (result) | 241 | 728 |
| ISB except the two helper waits (result) | 243 | 737 |

The baseline's **worst** run beats both candidates' best, so the sets do not
overlap: -8.5% on the mean. Host load was measured throughout and does not
explain it - the first candidate ran on a *quieter* host than its baseline
(median 4.78 against 5.56 cores).

The damage concentrates exactly where G52 said the gate dominates:
Object Remover -28.5%, Ray Tracer -23.2%, Horizon Detection -19.9%, Asset
Compression -17.8%, Object Detection -10.4%, HDR -10.1%.

### The diagnosis, including the part that was wrong

The first hypothesis was that backoff belongs to a waiter contending for a
resource but not to one whose own latency is on the critical path, and that the
two waits inside `JitExclusiveOperationScope` are the latter - the helper holds
the turn while every other thread is blocked behind it, so slowing its
observation of "drained" multiplies across all ten. A second variant removed
the backoff from both helper waits.

**That variant did not recover: 737 against 728.** The hypothesis was wrong, or
at least not the main term. What remains between it and the baseline is the ISB
in `jit_gate_value_changed`'s default path, used by `JitBlockPublication` while a
store waits for the phase to clear - and that wait is *also* on the critical
path, because a waiting store holds `jit_waiting_store_count_value` above zero
and the next helper cannot start until it reaches zero. Every wait in this
protocol feeds back into every other one, so there is no "free" place to spend
latency.

The conclusion is not that ISB is a bad spin hint in general. It is that this
gate has no slack anywhere: G47 already rejected a `std::mutex` turn here on
measured regressions, and now a 15-cycle backoff costs 8.5%. That is strong
evidence for C-2's specified replacement - a FIFO ticket turn whose contended
waiters park with atomic wait/notify - rather than any further tuning of how the
waiters spin.

### Disposition

- The ISB substitution at the two JIT gate sites is **rejected**, with numbers.
- `ExclusiveMemoryMutex` and `Ls64Lock` are **not** rejected and **not**
  accepted: they were carried along in both candidates, but no probe reaches
  them and they are cold in Geekbench, so neither run says anything about them.
  Reopening them requires first writing a probe that reaches the
  `exclusive_memory_mutex` fallback - the guest sequences that get there are the
  ones the native lowering declines, not ordinary LSE atomics or plain
  LDXR/STXR pairs.
- B-3's second half - hoisting the mutex and vector scan out of the
  `has_published_jit_store` loop - was not measured. Note that the natural
  implementation, an atomic count of non-idle participants, would put a
  contended global read-modify-write on `JitBlockPublication`'s deliberately
  lock-free direct-store path to speed up a rare waiter, which is the shape
  G33 and G34 were rejected for. It belongs with C-2's redesign, not before it.

## G103: B-2, the per-thread execution object resolved once

Accepted under condition 2: the focused control improves 20% at ten workers and
8% at one, with no Geekbench regression in either aggregate or any of the
thirty-two per-workload sections.

### The cost

Every guest entry calls `Context::thread_execution_state()` **twice**, once from
`darwin_uml_static_guest_interrupt_sequence()` and once from
`run_common_guest()`, to compute an answer that is per thread and changes only
at a retirement. Each call took:

| Step | Cost |
| --- | --- |
| `thread_execution_state()` | `shared_lock` on `execution_lifecycle_mutex_`, held for the whole guest run |
| | `lock_guard` on `states_mutex_` plus a `shared_ptr` copy |
| `ThreadStateStore::acquire()` | a third mutex, a `std::thread::id` hash lookup, a second `shared_ptr` copy |
| `ThreadStateRegistrations::bind()` | an erase-remove over a vector whose predicate dereferences a `weak_ptr` control block per entry, then a `weak_ptr` construction and `owner_before` comparisons |

Six mutex acquisitions and two hash lookups per syscall, on process-global
cachelines shared by all ten vCPU threads. The `bind()` scan is not in the
plan's description of this site and is not the smallest part of it.

### The change, and two things that were removed from it

The resolved object is cached in a thread-local keyed on the `Context`.

**The entry is weak, not owning, and that is a correctness requirement rather
than a preference.** `ThreadExecutionState` carries the engine slots and the
guest memory view, and retirement destroys it *before* retiring the memory
domain those caches are built on - `test_context_destruction_retires_state_before_domain`
pins that order. An owning cache defers the destructor to the caching thread's
next resolution or to its exit, which moves it after the retirement it is
ordered before. The first implementation held a `shared_ptr` and was wrong in
exactly that way.

**An explicit validity epoch was written and then deleted.** It advanced inside
`retire_thread_execution_states()` and was compared on each hit. Mutation
testing showed no test could be made to fail without it, and the reason is that
it is redundant: retirement clears the store, which drops the only strong
reference - it runs under `execution_lifecycle_mutex_` held exclusively, so no
`ThreadExecutionStateAccess` can be outstanding to hold another - so the object
is destroyed and `lock()` fails on its own. The weak reference *is* the
invalidation. A mechanism that no test can exercise is worse than no mechanism,
so it was removed rather than kept as insurance.

The `Context` comparison that remains is not redundant, and now has a negative.
`test_execution_state_is_not_shared_between_contexts` resolves from two live
contexts on one thread and requires each to get its own object. **Mutation-tested:**
replacing the comparison with `true` fails `runtime-context-lifecycle`.

### The probe

Nothing in `tests/perf` measured the guest entry boundary - every other probe
measures guest code once the emulator is already inside it.
`tests/perf/guest_syscall_boundary_loop.S` loops on `getpid`, the cheapest
syscall the guest kernel offers, so what it measures is almost entirely the
crossing rather than the work behind it.

| `guest_syscall_boundary_loop` | candidate | baseline |
| --- | ---: | ---: |
| 10 workers | 25.1 s, 24.9 s, 26.3 s | 32.9 s, 30.3 s |
| 10 workers, host CPU | 208-220 s | 253-275 s |
| 1 worker | 1.031 s | 1.124 s |

About 20% at ten workers and 8% at one, with a fifth less host CPU consumed, so
this is less work rather than better placement. The baseline exceeded the
runner's 30-second default command timeout where the candidate finished in 25,
which is how the size of the effect first showed up.

### Result

Matched pair, back to back, 10 vCPUs, `--engine gadgets`, zero RCU stalls, all
32 workloads, host load medians 2.78 and 3.00 cores.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 79 | 493 |
| candidate (result) | **80** | **494** |

**No single-core section regresses at all.** Six improve - Background Blur
+2.9%, Clang +2.0%, HTML5 Browser +1.4%, Structure from Motion +1.3%,
Navigation +1.2%, Object Remover +0.8% - and ten are unchanged.

Multi-core gains are led by Navigation +3.2%, Photo Filter +2.4% and File
Compression +1.9%. The largest multi-core losses are Clang -1.8%, PDF Renderer
-0.8% and Ray Tracer -0.7%, all far inside those sections' band spreads of 5.6%,
6.1% and about 2%.

Geekbench is compute-bound rather than syscall-bound, so a 20% improvement at
this boundary was never going to move the aggregate much; that it moved both by
a point while regressing nothing is the result. The saving is proportionally
much larger for syscall-heavy guest work than these workloads represent.

### Not done

The plan's second half for this item - replacing the run-long `shared_lock` on
`execution_lifecycle_mutex_` with a published per-thread active flag that
teardown scans - is untouched. The lock is still taken and released once per
guest entry and held across the run. That is a smaller remaining cost than what
was removed, and unlike the cache it changes the retirement protocol rather than
just short-circuiting a lookup, so it belongs in its own trial.

## G104: G-8, BFI for MOVK and NEG for the EXTR shift complement

Accepted under condition 2. **It is also the first direct evidence in this
campaign that G-3's central hypothesis holds: the same change is worth 4.1% on
performance cores and about 11% on efficiency cores.**

### The two changes

`move_wide_keep` implemented guest MOVK as materialise-mask, shift it, shift the
immediate, `bic`, `orr` - three to five instructions with a `bic`->`orr` chain
three deep. BFI is precisely MOVK's semantics, insert 16 bits at a fixed
position and leave the rest, in one instruction with no chain. For the 32-bit
entries it also subsumes the `mov w14, w14` zero-extend, because BFI on a w
register writes bits 63:32 as zero on its own. That is 2 to 5 instructions
removed per entry across six entries.

`extract_register` computed the left shift amount as `mov w17, #64` then
`sub w17, w17, w12`. LSL takes its amount modulo the register width and the
`shift_nonzero` variant guarantees 1..width-1, so the complement is just
`neg w17, w12`: for 64-bit, `(2^32 - shift) mod 64 == 64 - shift` because 2^32
is a multiple of 64, and the same argument gives the 32-bit case. One
instruction instead of two, and the chain no longer waits on a constant
materialisation.

### The layout is provably held constant

This is the point G17 and G36 failed on, and the reason G96 made pinning a
standing rule. Both entries shrink, and `gadget` aligns to 16 bytes, so a
4-byte reduction moves every later entry whenever the old body length was
exactly 4 past a multiple of 16. Trailing `.space` after the unreachable
`next_gadget` restores each entry's original length.

Verified rather than asserted: comparing the two linked images by symbol,
**0 of 9,663 gadget entries moved**, and `__text` is **byte-identical at
3,868,296**. The A/B therefore differs only in the instructions under test.

### Measurement

`guest_logical_move_loop` and `guest_add_immediate_loop` use `movk` only to
materialise their iteration count, so neither reaches these entries from its
measured loop. `tests/perf/guest_move_keep_extract_loop.S` puts MOVK at all four
64-bit halfword positions and both 32-bit ones, EXTR at both widths, and the ROR
alias into the loop body, and checks the accumulated sum against a total derived
from one iteration - so a wrong insert position, shift complement or rotate
fails the run rather than just running faster.

| median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| P-core, `--cpus 10 --workers 1` | 3.414 s, 3.455 s | 3.574 s, 3.595 s |
| E-core, `--cpus 4 --workers 1` | 13.216 s, 11.991 s | 14.565 s, 13.795 s |

**4.1% on P-cores and about 11% on E-cores**, the candidate winning in both
orders on both core classes. E-core wall time is noisier - host occupancy varied
0.54 to 0.76 cores across those four runs - so treat 11% as "substantially more
than 4.1%" rather than as a precise figure. The direction and the ordering are
what matter, and they are exactly what G-3 predicted: a narrower core with a
smaller reorder window pays more for gadget-body instruction count, and that is
the core class iOS runs on.

### Result

Matched pair, back to back, 10 vCPUs, `--engine gadgets`, zero RCU stalls, all
32 workloads, host load medians 2.77 and 2.84 cores.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 80 | 493 |
| candidate (result) | 80 | **498** |

Single-core is identical and multi-core is +1.0%, which is **inside** the 1.7%
band and therefore not itself evidence. The acceptance rests on condition 2: the
targeted control improves unambiguously on both core classes, and no Geekbench
aggregate or section regresses beyond run-to-run variation. The largest section
move against the candidate is single-core PDF Renderer, 86 to 84; that section
read 83, 85, 85, 86, 86, 86 across the day's other gadget runs, so a 2-point
move is inside its own spread. Multi-core sections are mostly up, led by Photo
Filter +4.6%, HTML5 Browser +3.5%, Object Detection +2.2% and PDF Renderer
+2.1%.

### What this reopens

G8, G17, G23, G23b and G36 were all rejected as instruction-count reductions
measured on P-cores, and G96 recorded the suspicion that the P-core result may
not hold for the platform that matters. This trial is the first to measure one
of these on both classes with the layout provably pinned, and the E-core figure
is roughly two and a half times the P-core one. That does not un-reject any of
them - each still has to be re-measured on its own - but it removes the reason
to assume they would come out the same way, and it makes the E-core half of
those re-measurements the interesting half.

### Not done

The third candidate in the G-8 table, a `_same` variant of `extract_register`
lowering the guest `ROR #imm` alias to a single `ror` when Rn == Rm, is not
implemented. Unlike these two it adds a bank entry rather than shrinking an
existing one, so it needs translator support in `extract_register_gadget` and
spends from the G-7 bank budget. `guest_move_keep_extract_loop` already contains
the ROR alias, so the control for it exists.

## G105: B-7, the host's real issue ratios

No runtime change. This measures what the campaign has been assuming, on both
core classes, so G-1, G-3, G-5 and B-5 can argue from a model rather than from
trial and error. `tests/perf/host_issue_ratio.cpp`, built with
`c++ -std=c++20 -O2 -o host_issue_ratio tests/perf/host_issue_ratio.cpp` and run bare for
performance cores and under `taskpolicy -c background` for efficiency cores.

### Method, and two mistakes it had to be corrected for

Everything is reported in operations per cycle, and the cycle is **measured**:
a dependent chain of `add` retires exactly one per cycle, which calibrates
whichever core the process is actually on. That matters because the two classes
do not run at the same clock, and a published frequency would be wrong for one
of them.

Two errors were made and fixed, both of which would have produced confident
nonsense:

1. **Accumulator count is a correctness property.** The first vector kernel used
   eight accumulators, each `fadd` depending on its own previous result. Eight
   chains against a three-cycle FADD latency sustain 8/3 = 2.67 per cycle, so it
   reported 2.596 and looked like a two-wide vector unit. Sixteen chains report
   3.99. The eight-chain kernel is kept in the output as a permanent reminder -
   it still reads 2.670 on P-cores, and the gap between the two rows *is* the
   FADD latency.
2. **The scheduler migrates a process between core classes mid-run.** One
   version calibrated at 2.06 GHz and then reported 6.06 vector ops per cycle,
   which no Apple core can issue: it had calibrated on an efficiency core and
   measured on a performance one. Fixed by requesting
   `QOS_CLASS_USER_INTERACTIVE`, and by calibrating against *each* kernel so the
   frequency is printed beside every result and a migration is visible rather
   than silent.

### Results

| kernel | P-core (3.02-3.08 GHz) | E-core (2.04-2.06 GHz) |
| --- | ---: | ---: |
| 128-bit loads only | **2.99 - 3.01** | **1.99 - 2.00** |
| vector ALU, 16 chains | **3.99 - 4.02** | **2.00** |
| vector ALU, 8 chains | 2.67 | 2.00 - 2.01 |
| mixed 1 load : 1 valu | 2.67 | 2.65 - 2.67 |
| mixed 2 load : 1 valu | 4.00 - 4.02 | 2.96 - 2.97 |
| mixed 3 load : 2 valu | **5.00 - 5.02** | **3.07 - 3.09** |
| mixed 1 load : 2 valu | 4.02 - 4.07 | 3.00 |

### What this says

**The video's ratio does not transfer, and it inverts.** Cortex-A7xx mid-cores
issue three 128-bit loads per clock against two vector operations, which is why
its advice is to spend vector ops to save loads. Apple performance cores are
three loads against **four** vector operations: the vector unit is the wider
port and the load unit is the scarcer one. On efficiency cores the two are
equal at two apiece. Any rebalancing argument copied from the talk would have
been pointed the wrong way on this hardware.

**Mixing does not buy both peaks.** The best combined rate is 5.0 per cycle on
P-cores and 3.1 on E-cores, well below the 7 and 4 that adding the isolated
peaks would suggest. A gadget body that is already mixed cannot be modelled as
"loads are free because the vector unit is busy".

**The two classes differ by about 2.4x in real throughput**, not by the 1.48x
their clocks differ by: 5.0 ops/cycle at 3.04 GHz is 15.2 Gop/s, against 3.09 at
2.05 GHz for 6.3 Gop/s. E-core FADD is also not latency-limited at eight chains
where the P-core is, so its narrower width is the binding constraint rather than
its window.

**This corroborates G104 independently.** That trial removed instructions from
two gadget bodies and measured 4.1% on P-cores against about 11% on E-cores, a
ratio of roughly 2.5. The issue-width ratio measured here is 2.4. The consistent
reading is that gadget bodies are issue-bound on efficiency cores while on
performance cores something else - G8's threaded indirect dispatch - is the
limit, so instruction count converts to time on one class and largely does not
on the other. **G8's conclusion is a P-core conclusion, and this is the
mechanism for why it should not be assumed to hold on the platform that matters
for iOS.**

### Consequences for the remaining items

- G-1 removes a **load** per gadget, which is the scarcer port on both classes -
  three-wide on P, two-wide on E. That is the right resource to attack, and the
  prediction is that it pays on both classes rather than only on E.
- G-5's internal `LDP`/`STP` pairs two loads into one instruction, which relieves
  the same port. Same reasoning.
- B-5's NEON compare-and-accumulate trades loads for vector ops. On Apple
  hardware that is trading a three-wide port for a four-wide one on P-cores and a
  two-wide for a two-wide on E-cores, so the video's 22-38% is not a number to
  expect here. It is still worth doing structurally - one bulk fetch instead of
  64 virtual ones - but the vectorisation half should not be assumed to pay.
- Bank-growing trials (G-2, G-4) are not constrained by these ratios and remain
  governed by G-7's I-cache budget instead.

**Not measured:** a representative A-series part. The plan asks for one; there is
no iOS device in this loop, and the XCFramework has no in-repo app target to run
it from. The E-core column here is an M1 Ultra Icestorm cluster, which is the
closest available stand-in and is the right shape of core, but a phone's E-cores
and its thermal envelope are not measured and should not be claimed.

## G106: G-5, folding the vector-bank base into the load displacement - rejected

Rejected as a performance change: **3,968 instructions removed from the gadget
bank and no measurable effect on either core class.** Reverted. The result is
worth recording because it is the first prediction G105's model made that was
then tested, and it came out the way the model said.

### The change

`UML_GADGET_CPU_V0` is 272, a multiple of 16, so it is encodable as a scaled
immediate on a 128-bit load. 43 SIMD gadget bodies formed the vector-bank base
and then indexed off it:

```
add x11, _cpu, #UML_GADGET_CPU_V0
add x12, x11, x10, lsl #4
ldr q0, [x12]
```

which folds to two instructions:

```
add x12, _cpu, x10, lsl #4
ldr q0, [x12, #UML_GADGET_CPU_V0]
```

Every access through a re-based register has to move with it, including the ones
that already carry a displacement - `str xzr, [x13, #8]` becomes
`[x13, #UML_GADGET_CPU_V0 + 8]`. Sites where a derived register reaches an
`LD1`/`ST1` are excluded, because the structure loads have no immediate-offset
form; two sites were skipped for that and for the base escaping into arithmetic.

Across the expanded bank this removed **3,968 instructions**, shrinking `__text`
by 15,872 bytes.

### Measured with the layout pinned, and it is flat

Padding after each affected entry's terminating `next_gadget` restored every
length: **0 of 9,663 entries moved and `__text` came out byte-identical at
3,868,296**. Without that padding 7,622 of 9,663 entries move, which is the
confound in its natural state and worth recording as a second calibration of
G96's finding.

| median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| P-core `guest_simd_three_same_add_loop` | 2.550 s, 2.553 s | 2.556 s, 2.553 s |
| P-core `guest_simd_basic_fadd_loop` | 2.558 s, 2.561 s | 2.562 s, 2.561 s |
| E-core `guest_simd_three_same_add_loop` | 6.829 s, 6.784 s | 6.834 s, 6.764 s |

Every pair overlaps. No Geekbench run was spent on it: a change that cannot be
distinguished from its control on the probe built to expose it has nothing for
the aggregate to confirm.

### Why it is flat, and why G104 was not

G105 measured Apple P-cores at 3 loads and 4 vector operations per cycle and
E-cores at 2 and 2, with gadget bodies issue-bound on E-cores and dispatch-bound
on P-cores. The instruction removed here is an `add` that feeds address
generation, which has slack ahead of the vector load that consumes it - it is
not on the critical path, and neither core is short of the integer ALU port it
uses. G104 removed instructions from **dependency chains** instead: `bic`->`orr`
at depth three became a single `bfi`, and `mov`->`sub` at depth two became a
`neg`. That is the distinction G8 drew in the first place - "the threaded
indirect dispatch, rather than the removed integer adds, limits this workload" -
and it now has a mechanism and a matched pair of examples on both sides of it.

**The operative rule for the rest of Part A: count dependency depth, not
instructions.** An instruction-count reduction that leaves the chain depth
unchanged should be expected to measure flat even on E-cores, and should be
screened on a probe before any Geekbench time is spent on it.

### What is left of it

The unpadded fold is 15,872 bytes smaller, about 0.4% of `__text`. That is a
binary-size and I-cache question, not a speed one, and it belongs to G-7, which
asks for L1I and iTLB miss rates and a budget before any bank-size trial is
judged. It is recorded here so G-7 has a ready candidate: the transform is
mechanical, the correctness risk is understood, and the unit suite covers it -
an earlier version of this transform with a fixed-line-count scope silently left
`str d0, [x13]` un-rebased in one gadget and was caught immediately by
`arm64-simd-shift-immediate-left`.

## G107: G-1, hoisting the successor fetch

The original family trial below was neither rejected nor accepted. A
representative family was converted to establish what the mechanism was worth
before paying for the bank-wide version, and its load-throughput result was much
smaller than the plan assumed. That trial was reverted and left the
polymorphic-branch claim open. The later default-off bounded closure is recorded
separately below; it is focused-accepted without changing the production
default.

### What was converted

`add_sub_immediate_fast`, chosen because it is hot, has a dedicated control in
`guest_add_immediate_loop.S`, and is the only shape where the change is
unambiguously safe:

```
-	ldr x8, [_stream], #8      ...body...   next_gadget   (ldr x8; br x8)
+	ldp x8, x14, [_stream], #16  ...body...   br x14
```

Three properties make it safe here that do **not** hold bank-wide, and they are
the real cost of the full change:

1. **It is never the last entry in a block.** `Builder::build()` always appends a
   terminal gadget, so the word after the operand exists. A terminal gadget that
   prefetched would read past the end of `program.words()`, and
   `program_contains_stream()` bounds `stream` to `[begin, end)`, so it would
   also stop finding its own program. The bank-wide version needs either an exact
   list of gadgets that can end a block or a sentinel word appended to every
   program.
2. **It touches no memory, so it has no fault path.** Prefetching advances
   `_stream` past the successor before the body runs, so any gadget that can
   fault mid-body would be observed at a stream position one word further along
   than today, which the cold-exit and checkpoint machinery reads.
3. **The body leaves x14 free.** There is no spare register bank-wide - the body
   here already uses x8, x9, x10, x11, x15, x16 and x27 - so the full version
   needs a reserved successor register and an audit that no gadget clobbers it.

Layout held constant: **0 of 9,663 entries moved, `__text` byte-identical.**
572/572 unit.

### Measured

| `guest_add_immediate_loop`, median of 3 | candidate | baseline |
| --- | ---: | ---: |
| P-core, both orders | 3.360 s, 3.353 s | 3.372 s, 3.373 s |
| E-core, round 1 | 8.895 s, 8.593 s | 8.395 s, 8.478 s |
| E-core, round 2 (reversed) | 8.449 s, 8.420 s | 8.451 s, 8.392 s |

**P-core: +0.55%**, small but clean - the candidate's worst run beats the
baseline's best. **E-core: inconclusive.** Round 1 looked like a 3.5%
regression and round 2 overlaps completely, so round 1's two high candidate
samples were noise. Recorded that way rather than picking the half that suits
either story.

### Why so little, and what the trial could not test

The plan expected "one fewer load and one fewer stream update per gadget" to be
a direct throughput gain because loads are the scarce port. G105 confirms loads
are scarce - three per cycle on P-cores, two on E-cores - but the gadget body is
not short of load slots, it is waiting on the indirect branch. LDP also very
likely cracks to two micro-operations on these cores, so "one fewer load" is one
fewer *instruction* rather than one fewer load slot, which is the same shape as
G106 and comes out the same way.

**The trial cannot test the mechanism's other half.** The plan's second claim is
that resolving the branch target at the top of the body lets a mispredict be
detected and repaired earlier, which pays on polymorphic gadget sequences.
`guest_add_immediate_loop` predominantly repeats the selected ADD successor,
with one SUB transition per loop; it is low-polymorphism rather than a deliberate
branch-prediction stressor. Testing that claim needs a mixed successor workload
to run prefetched gadgets throughout - which was then treated as the whole
change, including the reserved register and terminal-gadget work above.

### Disposition

At that point G-1 stayed open, with the balance of evidence changed: the
load-throughput half of its rationale was measured at about half a percent on
the family's own control, and the mispredict half was untested. A future attempt
needed to treat the reserved successor register and the program sentinel as the
first deliverables, not as details, and measure against a deliberately
polymorphic probe rather than a single-gadget loop. At that point, no such probe
existed yet.

### Bounded closure prototype and matched result (2026-08-12)

The incremental boundary is now explicit and default-off. Configuring
`UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST=ON` changes only the 32
`add_sub_immediate_fast` entries: their operand load becomes
`ldp x8, x14, [_stream], #16`, their unchanged body ends in `br x14`, and four
unreachable bytes restore each entry's exact prior size. Ordinary builds keep
the option off and retain the original bank bytes. This is deliberately not a
bank-wide successor register, sentinel, or fault-cursor rewrite.

The bounded family already has the three safety properties the wider proposal
lacked: it cannot fault, x14 is unused by every body, and Builder appends a
complete two-word terminal after a final fallthrough instruction. The focused
structure test pins all 32 spans, both selected dispatch shapes, x14 ownership,
and the terminal stream. A terminal-count case executes an ADD-immediate as the
last translated instruction and reaches Builder's synthetic terminal with one
retirement.

`guest_gadget_polymorphic_successor_loop.S` supplies the missing polymorphic
payload. Eight
copies of the exact same `add x20, x20, #1` entry rotate its single host branch
site through NOP, MOVZ, MOVK, EOR, REV, CLZ, EXTR/ROR, and ADC successor gadget
families. A structural assertion requires one predecessor pointer and eight
distinct successor pointers before timing. `guest_add_immediate_loop` is the
same-family load-throughput control. It is low-polymorphism rather than
literally monomorphic: the selected ADD branch sees itself 15 times and the SUB
entry once per loop. It therefore controls broad same-family throughput, but is
not a direct hardware-mispredict oracle.

#### Correctness gates and run configuration

The comparison used the same working checkout after outer HEAD
`1d28525afce272ac27473b4f3fabfbc6200a5b41`; its revision, dirty state, and
configure options were recorded with the run. Separate Release build trees
passed `arm64-gadget-successor-hoist-structure`,
`arm64-gadget-terminal-count`, plus `architecture-audit`, at 16 KiB OFF and ON
and at 4 KiB OFF and ON. A separate 16 KiB ON ASan/UBSan tree passed the same
focused tests.

The measured products are ordinary Release integrations with translation
sharing ON, x8/x9 carry OFF, all profiling OFF, and only G107 selected OFF or
ON. Product, object, archive-member, UUID, build-ID, and hash equality are not
part of this comparison.

#### Matched P/E campaign

The campaign used the gadgets engine, 16 KiB guest, 1 GiB memory, one worker,
P10 and E4, shell init, the EROFS image directly, and no retries or deleted
samples. Eight one-repetition preflights ran P
`pA mB mA pB` then E `pB mA mB pA`. The scored schedule ran 16 invocations on
P, then 16 on E, with three printed samples per invocation. P used
`pA mB mA pB pB mA mB pA` twice; E used
`pB mA mB pA pA mB mA pB` twice. Here `A` is OFF, `B` is ON, `p` is the
polymorphic probe, and `m` is the low-polymorphism control. The primary cell
statistic is the median of each invocation's three samples followed by the
median of its four invocation medians. All 40 guest/emulator runs exited zero
after orderly shutdown, chronology proves no guest overlap, and all 96 scored
samples remain in the result.

Positive percentages below mean that ON is faster. Ranges are the four
invocation medians, not the 12 individual printed samples:

| class / probe | OFF wall median (range), s | ON wall median (range), s | wall improvement | OFF / ON host CPU median, s | CPU improvement |
| --- | ---: | ---: | ---: | ---: | ---: |
| P / polymorphic | 1.9820 (1.981–1.983) | 1.7100 (1.709–1.713) | **13.723512%** | 2.160 / 1.860 | **13.888889%** |
| P / control | 3.3895 (3.380–3.392) | 3.3720 (3.360–3.376) | 0.516300% | 3.685 / 3.665 | 0.542741% |
| E / polymorphic | 6.0685 (5.763–6.405) | 5.6280 (5.310–5.754) | **7.258795%** | 5.215 / 4.710 | **9.683605%** |
| E / control | 8.7750 (8.643–9.420) | 8.2940 (7.994–8.458) | 5.481481% | 7.545 / 7.080 | 6.163022% |

All four OFF/ON invocation-median ranges are disjoint, and wall and host-CPU
directions agree. The polymorphic-minus-control difference-in-differences is
13.207211 percentage points on P and 1.777314 points on E. The counterbalanced
halves remain beneficial:

| class | polymorphic halves | control halves | difference-in-differences halves |
| --- | ---: | ---: | ---: |
| P | 13.723512%, 13.673058% | 0.502216%, 0.545642% | 13.221296 pp, 13.127415 pp |
| E | 10.634451%, 5.693334% | 9.847115%, 4.469016% | 0.787336 pp, 1.224318 pp |

#### Acceptance and limitations

The user's prospective dynamic cross-core policy uses the polymorphic wall
result as primary: P must improve by strictly more than `X=2%`, and E may
regress only by strictly less than twice the measured P improvement. It accepts
G107: P improves 13.723512%, while E improves 7.258795%, making its signed
regression -7.258795% against a strict 27.447023% ceiling. The independently
preserved stricter preregistration also passes: both classes clear 2% on the
polymorphic probe, ranges do not overlap, both halves are beneficial, neither
control nor CPU regresses by more than 2%, and every class and half has positive
difference-in-differences. The stronger preference that every class and half
also reach 2 percentage points does **not** pass because E reaches only
1.777314 points overall and 0.787336/1.224318 points by half.

The background-load gate was explicitly user-waived. Material unrelated load
persisted: the pre-gate snapshot included ReportCrash at about 99% of one CPU
and Spotlight at about 88%; the closing snapshot included Time Machine
`backupd` at about 119%, ReportCrash at about 98%, and Spotlight at about 74%.
No process was mutated. E-core timing spread is materially wider than P-core
spread. One E ON/polymorphic raw sample took 7.490 seconds at only 0.57 observed
host cores and remains included. E runs use a process-wide QoS clamp, not hard
hardware affinity. The E control itself improves 5.481481%, so the
1.777314-point E difference-in-differences is only modest evidence for an
additional polymorphism-specific effect even though the focused gate passes.

The commands and raw measurements are recorded at
`.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/`.
All runs shut down cleanly and the chronology shows no overlap. G107 is
therefore **focused-accepted**. At that checkpoint the selector remained OFF
pending a representative workload; the following current-tree confirmation
supplies that missing boundary.

### Representative Geekbench confirmation and default enablement (2026-08-13)

A current production baseline completed the full Geekbench 6.7.1 CPU suite on
the gadgets engine at 10 vCPUs and 8 GiB. Repeating the focused comparison on
the current OFF and ON products retained the mechanism result: the polymorphic
probe improved from 2.010 to 1.745 seconds on P and from 7.094 to 6.743 seconds
on E; the matched low-polymorphism control moved from 3.381 to 3.430 seconds on
P and from 9.312 to 9.881 seconds on E. The resulting control-normalized wall
gains are about 14.4% P and 10.4% E. A targeted multi-core Clang/Object
Detection check improved 421/455 to 423/459, with its combined score moving
438 to 441.

The full candidate run improved Single-Core 94 to 97 and its Integer component
83 to 87; Floating Point moved 119 to 118. Multi-Core improved 564 to 567,
with Integer 483 to 487 and Floating Point unchanged at 753. The full run's
early File Compression and HTML5 values initially looked low, so those two
workloads were rerun immediately as a compact matched check: baseline 607/605
versus candidate 605/597, only -0.3%/-1.3% rather than the full run's isolated
HTML5 outlier. Every command and emulator shutdown completed successfully.
Raw console logs are under
`.build/results/gadgets-optimization-20260813/`.

This representative boundary closes G107: the focused effect is large on both
core classes, targeted real workloads do not show a material regression, and
both full aggregate scores improve. The bounded successor fetch is now ON by
default. The option remains available for diagnosis; it does not authorize a
bank-wide successor register, a sentinel rewrite, or prefetch across a
fault-capable gadget.

## G108: the JIT multi-core noise band, and a retraction of G102

**The JIT engine's multi-core aggregate varies by 19% between consecutive runs of
an unchanged binary.** Every JIT conclusion in this log that rests on a single
matched pair is therefore unsupported, including G102's rejection of B-3. This
section records the band and retracts that rejection.

### The measurement

The same image, `--engine jit`, 10 vCPUs, two runs back to back with nothing
changed between them and host load medians of 4.76 and 4.57 cores:

| run | single-core | multi-core |
| --- | ---: | ---: |
| (result) | 242 | **701** |
| (result) | 245 | **834** |

**701 to 834 is 19.0%.** That same image had scored 829 half an hour earlier as
another trial's baseline, so three runs of one binary span 701, 829, 834.

Single-core is far steadier: 235, 237, 241, 242, 242, 243, 245, 250 across the
day, a 6.4% range, and no single-core conclusion here moved by more than that.

### Why, and what it should have been read as

The variance is not spread evenly. Object Detection multi-core reads 212, 239,
242, 243, 270, 283, 374, 378 across these runs - a bimodal split around 250 and
370 rather than a distribution around a mean. Asset Compression, Object Remover
and Horizon Detection do the same. That is a scaling collapse that either
happens in a run or does not, which is the signature of a lock convoy rather
than of measurement noise, and it points at the same publication gate G52
measured at 44-56% of sampled emulator time and G47 already failed to fix.

There was also an order artifact hiding in plain sight. Across three JIT pairs I
ran the candidate first and the baseline second, and the first run scored lower
every single time - 728 before 790, 737 before 811, 771 before 829. Three
unrelated changes producing the same 8% deficit should have been read as a
property of the harness, not of the code.

### Retraction: G102 does not establish that ISB spin hints regress the JIT

G102 rejected B-3 on "a 8.5% JIT multi-core regression with no overlap between
four runs", candidates at 728 and 737 against baselines at 790 and 811. Every one
of those four numbers is inside the 701-834 band an unchanged binary produces.
**The rejection is withdrawn.** What G102 established stands only in part:

- Still valid: the change is layout-neutral (`__text` byte-identical, no gadget
  entry moved), it passes every correctness gate, and the two gadget-path sites
  are unreached by any probe.
- No longer supported: that ISB backoff costs anything on the JIT engine, and
  the whole diagnosis built on top of that - including the claim that the second
  variant "did not recover", which compared two numbers that were never
  distinguishable in the first place.

B-3 returns to open. Re-measuring it needs a protocol that can resolve a few
percent on this engine, which a single pair cannot: either many repetitions per
arm, or a focused control that reaches the gate directly, or C-2 landing first so
the bistability is gone. The change itself remains reverted, because it was never
shown to help either.

### Standing rule for JIT measurements

A JIT multi-core comparison needs the same treatment the gadget engine got in
G96: a band established from repeated runs of the accepted build, before any
trial is judged against it. Until that exists, judge JIT changes on focused
controls and on single-core, and treat any multi-core aggregate move under about
20% as no information at all.

The gadget engine is not affected. Its measurements across the day span 77-80
single-core and 479-498 multi-core, and it has no equivalent of this gate.

## G109: B-5 step 1, bulk block validation

Accepted on its focused control. `validate_stale_block()` is **23% faster** at
ten workers with the candidate winning both orders and no overlap. The JIT
Geekbench aggregate could not adjudicate it and is reported as such rather than
dressed up either way - see G108.

### The change

`validate_stale_block()` runs on every stale hit in production builds, and G44
measured 245,339 of 315,215 misses as stale-sequence retranslations of an
already-cached PC, so this is a hot path rather than a corner. It was written in
the slowest available shape: up to 64 iterations, each a full virtual
`StaticGuestMemory::fetch()` through the software TLB plus a four-byte `memcmp`,
for at most 256 contiguous bytes.

It now fetches page-bounded runs and compares each run once, reusing the
page-splitting loop `fetch_instruction_window()` already uses in the gadget
executor.

**Fault reporting is preserved exactly, and that is the whole correctness
argument.** The old loop returned `mismatch` as soon as a word differed and
`fault` only for an unreadable word it actually reached, so a span containing a
mismatch *before* an unreadable word reports mismatch. A bulk fetch that fails
cannot tell which case it is in, so a failed run falls back to the
word-at-a-time path over that run rather than reporting either outcome for the
whole span. The plan called for exactly this and it is the reason the change is
low risk.

### Measured

`guest_syscall_boundary_loop`, `--engine jit`, 10 vCPUs, 10 workers, median of 3:

| | candidate | baseline |
| --- | ---: | ---: |
| pass A | 26.638 s | 34.006 s |
| pass B (reversed) | 25.180 s | 32.965 s |

About 23%, no overlap. Syscall-heavy guest work advances the change counters
constantly, so nearly every lookup arrives as a stale hit and this function
dominates - which is why the effect is so visible here and would not be on a
compute-bound workload.

### What the aggregate says, which is nothing

| `--engine jit` | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 250 | 829 |
| candidate (result) | 242 | 771 |

Read on its own this looks like a 7% multi-core regression. It is not
interpretable: G108 measured 701 and 834 from consecutive runs of one unchanged
image, so this pair sits inside the band, and the single-core gap of 250 to 242
is inside the 235-250 range the same engine produced all day. **Acceptance
therefore rests on the focused control and on the structural argument - strictly
less work for identical semantics - not on the aggregate.** Recorded that way
deliberately: this is the same shape of evidence that produced G102's withdrawn
rejection, read correctly this time.

### Not done

Step 2, the video's NEON compare accumulating with `MLA` or a `UABA` digest, is
deliberately not attempted. G105 measured Apple P-cores at three loads against
four vector operations per cycle, the inverse of the Cortex-A7xx ratio the
video's 22-38% came from, so the vectorisation half of B-5 has no reason to pay
here and would be spending vector width to relieve a port that is not the
constraint. Step 3, keeping the loop rolled, is satisfied by construction.

This also remains largely moot if C-1's page-granular code epochs land, since
most stale hits would stop happening at all.

## G110: G-7, the gadget bank's size budget, measured rather than assumed

No runtime change kept. G-7 asked for an I-cache and binary-size budget before
any bank-growing trial is judged. Rather than assume a cost, the question was
posed as an experiment using an artifact G106 already produced: does making the
bank **smaller** by a realistic amount help?

### The experiment

G106's fold removes 3,968 instructions from the bank. Padded, it holds the
layout and measures flat. **Unpadded** it shrinks `__text` from 3,868,312 to
3,852,440 - **15,872 bytes, 0.41%** - and moves 7,622 of 9,663 entries. That is
exactly the change a bank-size question wants: same work, less code, different
layout.

Matched pair, back to back, 10 vCPUs, `--engine gadgets`, host load medians 4.70
and 4.91 cores.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline, full bank (result) | 78 | 487 |
| candidate, 15.9 KiB smaller (result) | 78 | 479 |

**Single-core is identical and multi-core is -1.6%, inside the 1.7% band.**
Shrinking the bank by 0.41% and relocating three quarters of its entries
produced nothing measurable, in either direction.

### The budget this establishes

The video's "~2% from code-cache pressure alone" came from rolling up an
unrolled loop in a much smaller code footprint. It does not reproduce here at
this scale, and the reason is visible in the shape of the bank: 9,663 entries
across 3.8 MB is far larger than any L1I regardless, and a translated block
executes a scattered handful of them, so the working set is set by which gadgets
a workload uses rather than by how large the bank is in total.

**Consequently the constraint G-7 was written to impose is much looser than the
plan assumed.** A bank-growing trial of this magnitude - G-2's fused pairs,
G-4's duplicated SIMD variants - should not be expected to pay an I-cache
penalty at 0.4%, and should not be scoped down on that fear. What remains worth
watching is a growth large enough to change the *number of distinct entries a
hot loop touches*, which is a different quantity from total bank size and is not
measured by this experiment.

**Not measured:** L1I and iTLB miss rates directly. That needs performance
counters, which on macOS means private kperf interfaces or Instruments rather
than anything scriptable from this harness. The end-to-end experiment above
answers the question G-7 was actually asked - whether to constrain bank growth -
without them, and is recorded in their place rather than pretending they were
collected.

### Binary size

The 15,872 bytes are real and the fold is correct - it passes 572/572 with the
same semantics. It is not kept, because its only measured effect on speed is a
multi-core move in the wrong direction inside the noise band, and shipping a
change on that basis is what this log exists to prevent. It is recorded here as
available if shipped iOS binary size ever becomes a binding constraint, where
0.4% of `__text` would be judged on size rather than on speed.

## G111: G-2, guest CMP-immediate fused with a following B.cond

Accepted under condition 2, and it is the first change in this campaign to move
a gadget probe by more than a few percent on **both** core classes.

### Why fusion and not more instruction removal

The campaign has now measured the same lesson three times from different angles.
G106 removed 3,968 instructions from the bank and measured flat. G107 removed a
load and a stream update per gadget and measured 0.55% on P-cores and nothing on
E-cores. G105 explains both: the bodies are not short of issue slots, they are
waiting on the threaded indirect branch. G8 said exactly this in the first place.

Fusion is the only lever that attacks that directly, because it removes a
dispatch rather than an instruction. Trial 5 measured it at +0.83% composite and
was reverted in July as "marginal and workload-dependent" - **before** G8
established what the limiter was.

### The change

Unfused, guest `CMP #imm` followed by `B.cond` costs a dispatch between two
entries that share nothing but the flags, and the flags make a system-register
round trip in each direction: the compare ends `mrs x27, nzcv` and the branch
begins `msr nzcv, x27`, purely because they are separate entries.

The fused entry removes the dispatch and the `msr`. **The `mrs` stays**, and that
is a correctness point rather than an oversight: the guest's NZCV is
architecturally live after the branch, so the block has to leave it in x27
whichever way the branch goes. `MRS` does not disturb PSTATE, so it sits between
the `SUBS` and the `CSEL` that reads its flags.

Counted against the pair it replaces, the body is 12 instructions where the two
entries were 16, and one of the four removed is the indirect branch.

`DecodeContext` gains the instructions following the current one and the builder
loop becomes indexed, so a fusing decoder can see what comes next and advance
`compiled` past everything it consumed.

**Deliberately narrow about what it will fuse.** Only the plain `B.cond` form -
`BC.cond` shares the top byte but carries a different hint. Only `Rn != 31`,
because 31 means SP for this instruction class. Only the fourteen real
conditions, because the always forms have no fused entry and the selector returns
nullptr for them. Every excluded case falls through to the unfused pair
unchanged, which is why the differential suite passes without a single new
allowance.

### Measured

`tests/perf/guest_compare_branch_pair_loop.S` had to be written: every existing
loop counter is `subs xN, xN, #1` with a real destination, which is not the fused
shape, so no existing probe exercises this path at all.

| median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| P-core | 4.811 s, 4.816 s | 5.108 s, 5.111 s |
| E-core | 11.689 s, 11.540 s | 12.461 s, 12.363 s |

**5.8% on P-cores and 6.5% on E-cores**, no overlap in either order on either
class. Unlike G104 the two classes agree, which is the signature of removing a
dispatch rather than instructions: both are limited by it.

### Result

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 78 | 480 |
| candidate (result) | 78 | **483** |

Single-core identical, multi-core +0.6% and inside the band, so the acceptance
is condition 2 on the focused control. The largest section move against the
candidate is single-core Clang, 48 to 45; that section has read 45 to 50 across
the day's gadget runs, so it is inside its own spread.

**Why the aggregate barely moves, stated plainly:** this is one instruction pair
out of many. Conditionals in real code also arrive as `SUBS` with a live
destination, as `CBZ`/`CBNZ`, as `TBZ`/`TBNZ`, and as `CMP` of two registers,
none of which this fuses. The probe is entirely the fused shape and moves 5.8%;
Geekbench contains a few percent of it and moves accordingly.

The bank grows 3,840 bytes for 56 entries. G110 measured a 0.41% size change
moving 7,622 entries as producing nothing, so this is inside the budget by an
order of magnitude.

### The generalization this earns

G-2's step 3 asks for a measured superinstruction pass rather than one hand-
picked pair, and this result is the argument for it: a single narrow pair is
worth 5.8% of the shape it covers, so the return comes from **coverage**. The
next pairs to take, in the order the profile suggests, are `SUBS`-with-
destination + `B.cond` (the loop-counter shape every probe here uses), `CMP`
register + `B.cond`, and `CBZ`/`CBNZ`, which is already a single instruction but
ends a block and so pays the same boundary cost. The translation-time pair
histogram the plan describes is worth building before guessing further, and the
lookahead plumbing it needs is now in `DecodeContext`.

## G112: G-2 generalization, SUBS-with-destination + B.cond - rejected

Rejected and reverted. G111's fusion covers guest `CMP`, which is `SUBS` with an
XZR destination. The obvious next pair is `SUBS` with a **live** destination
followed by `B.cond` - the shape every counted loop ends on, and the reason none
of this tree's own probes exercised G111's path. It measures well on the probe
and badly on Geekbench, and the probe does not win the argument.

### The change

A second fused family identical to G111's except that the difference is written
back to Rd before the branch resolves. Ordering is the same and is not
incidental: neither `MRS` nor the `AND`/`STR` that write Rd disturb PSTATE, so
the `CSEL` still reads the flags `SUBS` produced. 56 further entries, 4,632 more
bytes of bank. 572/572 unit.

### Measured

| P-core, median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| `guest_compare_branch_pair_loop` | 4.688 s, 4.709 s | 4.799 s, 4.817 s |
| `guest_add_immediate_loop` | 3.402 s, 3.413 s | 3.405 s, 3.406 s |

A further **2.3%** on the probe that ends its loop this way - which stacks with
G111 for 5.11 s unfused, 4.81 s with CMP fusion, 4.69 s with both, **8.2%
total** - and nothing on `guest_add_immediate_loop`, whose loop edge is a small
fraction of a large unrolled body.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline, G111 only (result) | 78 | 485 |
| candidate (result) | 77 | 476 |

**Both aggregates fall**, multi-core by 1.9% which is just outside the 1.7%
band. That fails condition 1, and it fails condition 2 as well, which requires
no Geekbench regression at all beyond run-to-run variation. A focused-probe gain
does not buy an aggregate loss - that is the rule this log exists to enforce,
and it is the rule the retracted G102 was decided under too.

**The pairing is weaker than it should be and this is recorded rather than
glossed:** the first baseline attempt wedged (below) and had to be killed, so
the surviving baseline ran about two hours after its candidate rather than back
to back. The direction is consistent across both aggregates, so the conclusion
is not rescued by that, but a re-measurement is the honest first step if anyone
wants to reopen this.

### Why the loop-counter shape may genuinely be worse than the CMP shape

Worth recording, because it is not obvious that adding coverage to a winning
mechanism could lose. The fused CMP entry replaces two entries with one and
removes a dispatch outright. The fused SUBS entry does the same but is longer -
it carries the write-back - and, unlike the CMP case, the entries it adds are
ones a hot loop executes **every iteration**. G110 measured a 0.41% bank size
change as immaterial, but that experiment shrank the bank uniformly; it says
nothing about adding 56 entries that land in the hottest loops' working set.
That is the distinction G110 explicitly flagged as unmeasured: the number of
distinct entries a hot loop touches, rather than total bank size.

### Disposition

G111 stands. The generalization is not simply "more pairs is more win", and the
pair histogram G-2's step 3 asks for should be built and read before another
pair is added by hand - it would rank candidates by how often they occur rather
than by how obvious they look, and this pair looked obvious.

### A wedge, recorded because it did not reproduce

The first baseline run of this trial sat on multi-core File Compression for
**44 minutes** at about 2 host cores, with no console progress, no RCU stall and
no guest kernel warning, on a host at its usual 4.5-6 core background load. Every
other run that day, including the same binary an hour earlier and the retry
afterwards, completed that workload in a couple of minutes at 6-8 cores.

It was killed without a stack sample, which was the wrong call - it was the only
instance available. The run scripts now carry a stall watchdog: when the console
log stops advancing for six minutes while the emulator is alive, it takes two
`sample(1)` call-graphs a minute apart and leaves them beside the log without
disturbing the run. If this recurs the samples will say whether it is the
publication gate, the MMU miss path, or a guest scheduler loop.

## G113: G-2 step 3, the adjacent-pair histogram

No runtime change. G-2's remaining step asks for a translation-time histogram of
adjacent gadget pairs so fusion candidates are chosen by frequency instead of by
eye. G112 is the argument for it: the pair that looked like the obvious follow-on
to G111 lost, and there was no way to know that in advance.

### The instrument

The first word a decode emits is its gadget pointer, so the record needs no
separate pass and no knowledge of operand shapes. Counting is per thread and
merged at dump time, so translation stays lock-free; names resolve through
`dladdr`.

Compiled out unless `UML_GADGET_PAIR_HISTOGRAM=ON`, and **verified rather than
asserted**: the production emulator built with the option absent is byte-identical
to the previous commit's, same SHA-256.

Two things the first attempt got wrong, both recorded because they would cost
the next person the same hour. `atexit` alone produces an empty file - the
harness ends the guest with `poweroff -f` and then SIGTERM, and a run that exits
on a signal never runs exit handlers - so the file is now rewritten
periodically. And `dladdr` on every entry at every dump is far too slow once the
table reaches twenty thousand entries; it is tolerable at the current interval
but should be given a name cache before anyone captures a longer workload.

### What it says

Captured over guest boot and userland start-up: **39,845,888 pair occurrences
across 20,324 distinct pairs.**

| occurrences | share | first | second |
| ---: | ---: | --- | --- |
| 1,067,306 | 2.7% | `pc_relative` | `load64_unsigned_fast_r` |
| 657,985 | 1.7% | `integer_pair_load64_0_rr_sp` | `integer_pair_load64_0_rr_sp` |
| 611,469 | 1.5% | `pc_relative` | `add_sub_immediate_x_add_n_rr` |
| 560,852 | 1.4% | `logical_shifted_x_orr_p_n_zr_r` | `logical_shifted_x_orr_p_n_zr_r` |
| 545,122 | 1.4% | `logical_shifted_x_orr_p_n_zr_r` | `branch_link_linked` |
| 533,708 | 1.3% | `nop` | `return_cached` |
| 502,387 | 1.3% | `integer_pair_load64_2_rr_sp` | `nop` |
| 483,785 | 1.2% | `load64_unsigned_fast_r` | `add_sub_immediate_x_add_n_rr` |
| 457,770 | 1.1% | `nop` | `integer_pair_store64_1_rr_sp` |
| 429,000 | 1.1% | `add_sub_immediate_x_add_n_sr` | `integer_pair_store64_0_rr_sp` |
| 347,382 | 0.9% | `load64_unsigned_fast_r` | `compare_branch_x_z_linked` |

Cumulative share: top 10 is 14.7%, top 25 is 25.1%, top 50 is 34.8%, top 100 is
46.2%, top 200 is 57.7%. A long tail, so no single fusion is transformative and
coverage is what pays - which is what G111 and G112 between them already showed
from the other direction.

### The finding that matters

**Address materialisation dominates, and the conditional shapes do not appear at
all.** `ADRP` + `LDR` is the single most common pair in translated code at 2.7%,
`ADRP` + `ADD` is third at 1.5%, and `MOV` register + `MOV` register and `MOV` +
`BL` follow. The `CMP` + `B.cond` pair fused by hand in G111 is nowhere near the
top.

That retrospectively explains G111 exactly. It won 5.8% on a probe made entirely
of the pair it fuses and moved the Geekbench aggregate by almost nothing,
because the pair is rare in real translated code. The mechanism was right and
the target was picked badly - which is precisely the failure mode this histogram
exists to prevent, and it was picked by eye because the histogram did not exist
yet.

### Ranked candidates for the next fusion trial

1. `pc_relative` + `load64_unsigned_fast_r` - 2.7%, and the easiest of the set:
   both halves are simple, neither is terminal, and no flags are involved.
2. `pc_relative` + `add_sub_immediate_x_add_n_rr` - 1.5%, the same shape with an
   `ADD` instead of a load.
3. `logical_shifted_x_orr_p_n_zr_r` twice - 1.4%, two register moves.

Taken together the first two are 4.2% of all adjacent pairs, against whatever
share `CMP`+`B.cond` holds, which is below the top twenty.

### Limitation, stated plainly

This capture covers guest boot and userland start-up, **not the Geekbench
workloads**. The intended Geekbench capture died on `curl: (35) TLS connect
error` - a network failure in the guest, unrelated to the instrument - and was
not retried. Start-up code is real compiled code and the top pairs are ordinary
compiler output rather than anything boot-specific, so the ranking is unlikely to
be upended, but a compute kernel's mix is not the same as a dynamic loader's and
the capture should be repeated over Geekbench before the ranking is trusted for
anything beyond choosing the next trial.

## G114: folding guest ADRP + ADD into one entry

Accepted under condition 2, and it is the largest focused-probe result of this
campaign - **15% on P-cores and 19% on E-cores** - for a change that adds no
gadget to the bank at all.

**This is G113 paying for itself immediately.** The histogram ranked
`ADRP` + `ADD` third among adjacent pairs in real translated code at 1.5%, well
ahead of the conditional shape G111 fused by eye. It would not have been chosen
otherwise.

### Why it costs nothing

The translator already folds `ADRP` to a constant - `pc_relative` is a three-word
entry that stores a precomputed address into Rd. So the
`adrp xN, sym` / `add xN, xN, #:lo12:sym` idiom that names everything in
position-independent code was two entries and two dispatches computing something
translation already knew.

When the `ADD` rewrites the same register it reads, the page value it consumes is
dead the moment it is produced, so the pair collapses into the **existing**
`pc_relative` entry carrying the final address. No new gadget, no bank growth,
no selection table - 116 bytes of decoder, against the 3,840 bytes G111 spent on
56 entries.

**Restricted to the exact idiom:** 64-bit, no flags, no shifted immediate, and
`Rd == Rn ==` the ADRP destination. Anything else leaves the page value
architecturally observable and is left alone, which is why a re-capture shows the
pair halving rather than vanishing. Neither instruction can fault, so nothing can
be delivered between them and retiring both together is sound.

### Measured

`tests/perf/guest_pc_relative_address_loop.S`, four independent addresses per
iteration so no single dependency chain dominates, summed rather than
dereferenced so it measures address materialisation and not the memory path.

| median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| P-core | 1.392 s, 1.394 s | 1.638 s, 1.641 s |
| E-core | 4.193 s, 4.246 s | 5.191 s, 5.265 s |

**15% and 19%**, no overlap in either order on either class. As with G111 the two
classes agree, because what is removed is a dispatch.

### Result

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 79 | 491 |
| candidate (result) | 79 | **493** |

Single-core identical, multi-core +0.4% and inside the band, so acceptance is
condition 2 on the focused control. Multi-core gains are led by File Compression
+4.0%, Navigation +3.8% and HTML5 Browser +3.5%; the largest losses are Horizon
Detection -4.8% and Object Remover -3.2%, both inside their own spread across the
day's gadget runs (790-830 and 660-692 respectively).

The aggregate again moves far less than the probe, for the same reason as G111:
the probe is made entirely of the folded idiom, and Geekbench contains it at the
1.5% the histogram measured.

### What this suggests next

`ADRP` + `LDR` is the **top** pair at 2.7%, nearly twice this one, and the same
argument applies - the address is a translation-time constant, so a fused entry
would load from a known address without materialising the base into `CpuState`
and reloading it. It is deliberately not attempted here: that fusion lands inside
the software-MMU translate, cross-page and fault paths, which the plan calls the
correctness-critical host boundary, and it deserves its own trial rather than
being appended to this one.

## G115: eliminating NOP entries - blocked on the lowering-evidence contract

Not measured. Recorded because the candidate is a good one, the blocker is
specific, and the next person should not rediscover it.

### The candidate

G113's histogram put `nop` on one side of three of the ten most frequent
adjacent pairs - `nop`+`return_cached` at 1.3%, `integer_pair_load64`+`nop` at
1.3%, `nop`+`integer_pair_store64` at 1.1%, about **3.7% of all adjacent pairs
between them**. A `nop` entry costs a full threaded indirect dispatch to do
nothing, and there are 26 sites where the translator emits one for an
instruction it has already folded away: `MOVZ` into XZR, a discarded
system-register read, an architectural `NOP`.

Emitting nothing looked sound, and as far as the *engine* goes it is: a gadget is
not the unit of retirement. `compiled` counts guest instructions and the builder
advances it either way, blocks are entered at word zero and always end with a
terminal entry, and nothing maps a stream offset back to an instruction index. A
block whose every instruction folds away would simply be its terminal entry.

### Why it stopped

Sixteen unit tests fail, all with the same shape - "discarded PC-relative result
was not folded to NOP". They assert **which entry each instruction lowers to**,
which is this project's instruction-evidence discipline rather than an incidental
expectation. "Folded to a NOP entry" is the currently documented lowering for
these instructions, and "folded away entirely" is a different claim that has to
be made deliberately: sixteen lowering assertions and the corresponding evidence
ledger entries, across the engines that share the ledger.

That is a contract change, not a translator tweak, and it is not something to
push through at the end of a long session on the strength of a histogram
estimate. The change itself is a one-line flip once the contract is updated - the
sites were converted to a named `emit_nop()` helper precisely so the decision
lives in one place - so the cost is entirely in the evidence, not in the code.

### What it is worth doing first

Re-capture the histogram over the Geekbench workloads before spending that
effort. The 3.7% figure comes from guest boot and userland start-up, where
`nop`-adjacent pairs may be inflated by dynamic-loader and start-up code that
does a lot of register shuffling. If the share holds on compute kernels the work
is clearly justified; if it collapses, it is not.

## G116: the pair histogram over a full Geekbench run, and two corrections

G113 captured the histogram over guest boot and start-up and said plainly that
the ranking should be re-taken over the workloads before being trusted. It has
been, and it changes the answer twice. **3,451,912,192 pair occurrences across
47,494 distinct pairs**, all 32 workloads, with the `dladdr` name cache that made
a capture this long practical.

| occurrences | share | first | second |
| ---: | ---: | --- | --- |
| 121,044,458 | 3.51% | `integer_pair_load64_0_rr_sp` | `integer_pair_load64_0_rr_sp` |
| 101,905,829 | 2.95% | `integer_pair_store64_0_rr_sp` | `integer_pair_store64_0_rr_sp` |
| 101,893,236 | 2.95% | `logical_shifted_x_orr_p_n_zr_r` | `logical_shifted_x_orr_p_n_zr_r` |
| 91,484,735 | **2.65%** | `add_sub_register_x_sub_s_sh_n_rr_d` | `branch_eq_linked` |
| 72,893,029 | 2.11% | `logical_shifted_x_orr_p_n_zr_r` | `branch_link_linked` |
| 48,274,998 | 1.40% | `integer_pair_store64_0_rr_sp` | `add_sub_immediate_x_add_n_sr` |
| 47,618,568 | 1.38% | `load64_unsigned_fast_r` | `load64_unsigned_fast_r` |
| 35,568,790 | 1.03% | `load64_unsigned_fast_r` | `add_sub_register_x_sub_s_sh_n_rr_d` |
| 28,877,040 | **0.84%** | `add_sub_register_x_sub_s_sh_n_rr_d` | `branch_ne_linked` |

### Correction 1: NOP elimination is not worth doing

G115 recorded that `nop` sat on one side of three of the ten most frequent pairs,
about **3.7%**, and that acting on it was blocked behind sixteen lowering
assertions and the instruction-evidence ledger. Over Geekbench, pairs involving
`nop` are **0.24%** of the total.

The boot figure was start-up code - dynamic loading and initialisation, where
folded-away instructions cluster - and it does not survive contact with compute
kernels. **G115 is closed rather than merely deferred: the evidence-contract work
it needs is not justified by a quarter of a percent.** This is exactly the check
G113 asked for, and it saved the effort rather than confirming it.

### Correction 2: the valuable conditional pair is the register form

G111 fused guest `CMP`-**immediate** with `B.cond`, chosen by eye before the
histogram existed, and moved the aggregate by almost nothing. The register form -
`CMP Xn, Xm` then `B.cond`, which appears here as
`add_sub_register_x_sub_s_sh_n_rr_d` followed by `branch_eq_linked` or
`branch_ne_linked` - is **3.49% combined**, the fourth and ninth most frequent
pairs in real workloads. The immediate form does not reach this table at all.

So the mechanism G111 proved was right and it was pointed at the rarer half of
its own instruction class. The register form is the next fusion to build, and it
reuses G111's machinery directly: same flag round-trip to remove, same
`follow_block_link` exit, same narrow decode conditions.

### What else the workloads rank

`LDP`+`LDP` at 3.51% and `STP`+`STP` at 2.95% are the top two - consecutive
register-pair transfers, which is prologue and epilogue code. Fusing a pair of
pair-transfers would move four registers in one entry. Register `MOV`+`MOV` at
2.95% and `MOV`+`BL` at 2.11% are argument-setup shapes.

Note also what is **absent**: `ADRP`+`LDR` and `ADRP`+`ADD` led the boot capture
and do not appear in the top sixteen here. G114 folded the latter and was
accepted on a matched pair - single-core flat, multi-core +0.4% - so it stands,
but its 15% and 19% probe figures should be read as the value of the idiom where
it occurs rather than as a share of these workloads.

The distribution is flatter than boot's: the top pair is 3.51% and no single
fusion is transformative, which is the same conclusion from a much larger sample.

## G117: fusing guest CMP of two registers with B.cond, and G-2's ceiling

Accepted under condition 2, and it settles what superinstruction fusion is worth
in this engine.

### The change

G116 ranked `CMP Xn, Xm` followed by `B.cond` fourth and ninth of 47,494 pairs
over a full Geekbench run - 2.65% with `B.eq`, 0.84% with `B.ne`, **3.49%
together** - while the immediate form G111 fused by eye does not reach the table.
This is the same mechanism aimed at the half of the instruction class that
actually occurs: the dispatch between the two entries goes, and with it the `msr`
that reloads the flags the compare just wrote.

56 entries, 4,276 bytes. Only the plain unshifted register-register form with an
XZR destination; an extension, a shift or a real destination falls through to the
unfused pair unchanged.

| median of 3, both orders | candidate | baseline |
| --- | ---: | ---: |
| P-core | 4.827 s, 4.811 s | 5.090 s, 5.073 s |
| E-core | 11.220 s, 11.147 s | 11.707 s, 13.442 s |

**5.2% on P-cores** with no overlap. E-core is directionally the same but one
baseline sample sits at 13.4 s against another at 11.7 s, so it is reported as
consistent rather than precise.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 80 | 489 |
| candidate (result) | 80 | 489 |

**Both aggregates identical.** No section moves outside its own spread - the
largest, Navigation multi-core at -3.5%, sits inside today's 1077-1144 range, and
single-core has six sections up, eight flat and two down. Condition 2 is
satisfied: the focused control improves and nothing regresses beyond run-to-run
variation.

### The ceiling, which is the real result

This is the best-targeted fusion available - the fourth most frequent pair in
real workloads, chosen from a 3.45-billion-sample histogram rather than by
intuition - and it moves the aggregate by **zero measurable points**. The
arithmetic is unforgiving and worth writing down:

> 3.49% of adjacent pairs, times roughly 5% saved on the pair, is about 0.17% of
> translated execution. That is an order of magnitude below what a Geekbench
> aggregate can resolve.

G116 measured the distribution as flat - the single most common pair is 3.51%,
and it takes a hundred pairs to reach 46% of the total. **So superinstruction
fusion cannot be a large win in this engine unless it is applied across dozens of
pairs**, and each pair costs 56 bank entries plus a decode condition. Three
fusions are now accepted (G111, G114, G117) and together they are worth a few
tenths of a percent on the aggregate while each is worth 5-15% on its own shape.

That is not a reason to revert them - they are real, they cost almost nothing,
and they compound - but it retires the expectation that G-2 was a large lever.
Trial 5's original +0.83% composite now looks like the right order of magnitude
for the whole idea rather than a floor, and the plan's framing of that figure as
"a floor rather than a ceiling" is not supported by what the histogram shows.

**What would change that conclusion:** a fusion pass that generates entries for
the top N pairs mechanically rather than one hand-written family at a time, so
that N can be fifty rather than three. The histogram, the lookahead in
`DecodeContext` and three worked examples now exist; the missing piece is
generation. That is a different and larger piece of engineering than the plan's
step 3 describes, and it should be costed against the few tenths of a percent
that three pairs bought before it is started.

## G118: B-1 step 2 may not need a radix table at all

No change made. This records a structural finding that, if its one open premise
holds, replaces the plan's largest and riskiest item with a much smaller one. It
is written down rather than acted on because the premise needs verification that
belongs at the start of a session, not the end of one.

### What the plan assumed

B-1 step 2 proposes replacing the per-generation `unordered_map<page, Mapping>`
and its `shared_mutex` with a lock-free three-level radix table, so a translate
becomes three atomic loads with no lock. That is a substantial rewrite of the
host security boundary - 36 `mappings` call sites, the mapping-change history
`Tlb::reconcile_changes` depends on, and G3's additive versus destructive batch
classification all have to survive it.

### What the code actually does

**Every mapping mutation already quiesces execution.** `map`, `protect`, `unmap`,
`destroy`, `rebind` and `end_batch` each construct an `ExecutionQuiesce`, which
closes the execution gate and then waits for `execution_reader_mask` to drain
before touching the table:

```
	ExecutionQuiesce quiesce{state, generation};
	std::lock_guard lock{state->mappings_mutex};
```

So a writer cannot run concurrently with anything inside an execution-reader
window. If `translate()` only ever runs inside such a window - and its two
production callers, the software TLB miss path in `tlb.cpp` and
`StaticGuestMemory`, are both reached from guest execution - then **the
`shared_lock` it takes on `mappings_mutex` is excluding something that the
execution gate has already excluded.** Removing it would be a few lines rather
than a rewrite, and would deliver most of what step 2 was for.

### The premise that has to be verified first, and one thing that already
### complicates it

The mutex cannot simply be deleted: `copy_from_buffer` is a **kernel-side** reader
that takes it and is explicitly designed to run in parallel with guest execution -
its comment says so - so it is not inside an execution-reader window and writers
must keep excluding it. The proposal is therefore narrower than "drop the lock":
it is "drop the lock *in `translate()` only*", leaving every other reader and all
writers exactly as they are. That is sound if and only if:

1. every production caller of `translate()` is inside an execution-reader window,
   for the whole call, on every path including the fault and cross-page ones; and
2. the unit tests that call `darwin_uml_static_vm_translate` directly - there are
   several, in `native_mmu_tests`, `static_mmu_batch_tests` and
   `arm64_jit_cache_validation_tests` - either establish that window or are
   single-threaded with respect to mapping mutation.

Point 1 is the real work. It is an invariant the code currently does not state or
enforce anywhere, and making it load-bearing means asserting it - a debug-build
check that `translate()` observes a non-zero reader count would turn an argument
into something the test suite proves on every run.

### Why this is worth doing before the radix table

If the invariant holds, the cheap change removes one of the two remaining locks
on the miss path for a few lines and no new data structure, and the radix table's
remaining value is the hash lookup and the `shared_ptr<MappedBacking>`
dereference rather than the lock. That is a much smaller prize than the plan
assumed, and it should be measured before committing to the rewrite - the same
mistake G-2 made by assuming fusion was a large lever before G116 measured the
distribution.

Also worth noting: the Navigation multi-core regression from G101 is still open
and was hypothesised to be contention on exactly this `mappings_mutex`. If the
cheap change is valid, it tests that hypothesis directly and far sooner.

### G118 addendum: the premise is false, measured

The hypothesis above was tested rather than left standing, by instrumenting
`translate()` to count how often it observes a non-zero execution-reader count
and running the miss-heavy probe:

| | translates |
| --- | ---: |
| inside an execution-reader window | 10,682,369 |
| **outside** | **131,587** |

**About 1.2% of translates run outside a reader window**, so the execution gate
does *not* already exclude every translate from a concurrent mapping mutation,
and dropping the `shared_lock` in `translate()` would be a data race on the host
security boundary for roughly one call in eighty. **The cheap alternative is
withdrawn as stated.**

This is worth the instrumentation it cost. The argument was structurally sound -
every mutation really does quiesce, and the two production callers really are
reached from guest execution - and it was still wrong, because some path reaches
`translate()` outside the window. An argument of that shape is exactly what gets
acted on without checking.

**What remains actionable.** The invariant is not true, but it may be
*establishable*: 1.2% is small enough that the offending callers are likely one
or two specific paths rather than a pervasive pattern. Identifying them - by
splitting the counter per call site across `tlb.cpp` and `StaticGuestMemory`, and
across the fault, cross-page and translation-time fetch paths - would say whether
they can be brought inside a reader window or must keep the lock. If they can,
the cheap change becomes available again and the assertion proposed above turns
it into something the suite enforces. If they cannot, B-1 step 2 needs the radix
table after all, and this measurement is the justification for spending that
effort rather than an assumption that it was needed.

### G118 second addendum: the exception is a single named caller

The 1.2% was attributed by bucketing out-of-window translates on
`__builtin_return_address(0)` and resolving the buckets with `dladdr`. There is
exactly one:

| out-of-window translates | caller |
| ---: | --- |
| 131,072 | `darwin_uml_static_vm_retry_fault` |

Nothing else appears. Every translate from the software-TLB miss path and from
`StaticGuestMemory` is inside a reader window, as the argument predicted; the
fault-retry path is the sole exception, and it reaches `translate()` through
`with_address_space` from `darwin_uml_vm_retry_fault` in `static_api.cpp` -
the host-side fault handler, which by construction runs when guest execution has
already stopped rather than inside it.

**That makes the cheap change available again in a narrower form.** The hot path
does not need the lock; one cold caller does. Two shapes fit:

1. Split the entry points - an unlocked `translate()` for the execution path and
   a locked `translate_unsynchronised()` for `retry_fault` - so the fault handler
   keeps excluding writers while the miss path stops paying for it. Smallest
   change, and the asymmetry is explicit in the names.
2. Bring `retry_fault` inside a reader window, restoring a single uniform
   invariant that the proposed debug assertion could then enforce everywhere.
   Cleaner, but it changes when the fault handler can run against a quiescing
   writer, which needs its own thought.

Either way the measurement now says which locks are load-bearing rather than
leaving it to argument, and B-1 step 2's radix table is not needed to remove the
`mappings_mutex` acquisition from the miss path. What the radix table would still
buy is the hash lookup and the `shared_ptr<MappedBacking>` dereference, which is
a smaller and separately measurable prize.

**Sequence for the next attempt:** take shape 1, keep every other reader and all
writers unchanged, add the debug assertion for the execution path only, measure
with `guest_mmu_miss_walk_loop` and a matched gadget pair, and check Navigation
multi-core specifically - G101 left it 3.4% down and hypothesised contention on
exactly this mutex.

## G119: removing the mapping lock from the miss path - measured, and it buys nothing

Implemented, measured, reverted. This closes out B-1 step 2's central premise.

### The change

Following G118's attribution, `translate()` skipped the `shared_lock` on
`mappings_mutex` whenever the calling thread already held its own execution
reader window on that generation - which the instrumentation put at 98.8% of
translates - and kept the lock otherwise, so `retry_fault` and any future
out-of-window caller stay correct by falling through the branch rather than by
an invariant someone has to remember.

The test is deliberately about *this* thread rather than the gate's reader
count: a non-zero count belonging to other threads proves nothing, because a
writer that has closed the gate proceeds the moment the count reaches zero and a
caller outside a window would still be reading.

572/572 unit and 8/8 TSan.

### Measured

`guest_mmu_miss_walk_loop`, ten threads sharing one address space so every one
of them contends for the same `mappings_mutex`, medians of 3 and then of 5, in
alternating order:

| | candidate | baseline |
| --- | ---: | ---: |
| round 1 | 5.340 s, 4.871 s | 4.962 s, 4.837 s |
| round 2 | 4.978 s, 5.142 s | 5.035 s, 5.053 s |

**Every pair overlaps.** Removing the lock from 99% of the acquisitions on the
probe built to maximise contention for it produces no measurable change, so it
is reverted: a change on the host security boundary that buys nothing is pure
risk.

### What this settles

B-1's premise was that "two mutex acquisitions, two hash lookups, and contended
atomic refcount round-trips per miss" cost real time, and that libc++'s
`shared_mutex` is expensive because `lock_shared()` takes a real internal
`std::mutex`. G101 confirmed half of that - removing the **registry** lock and
its hash lookup was worth 21% on this same probe and 1.55% of Geekbench
multi-core. G119 now shows the other half is worth nothing: the per-generation
`mappings_mutex` is not where the time goes.

**So B-1 step 2's radix table is not justified by lock removal.** What remains of
its case is the hash lookup and the `shared_ptr<MappedBacking>` dereference on a
hit - a much narrower claim than the plan makes, and one that should be measured
on its own before a lock-free three-level table is written across 36 call sites
on the correctness-critical host boundary.

The difference between the two locks is worth naming, because it is the
generalisable part: the registry lock was **process-global**, taken by every vCPU
thread for every miss regardless of which guest process they were in, while
`mappings_mutex` is **per generation**, so it is only contended by threads inside
the same guest address space and is already sharded by exactly the thing that
varies. G42's 16,384-set TLB in front of it means the surviving traffic is low
enough that the acquisition does not show up.

Navigation multi-core, still 3.4% down since G101 and hypothesised to be
contention on this mutex, is therefore **not** explained by it. That hypothesis
is withdrawn and the section needs a different explanation.

## G120: the miss path's remaining cost is the lifecycle lease, worth 31%

No change kept. This locates what B-1 was actually looking for, after G119
eliminated the candidate the plan named.

### First, the probe is sound

Before trusting another conclusion from `guest_mmu_miss_walk_loop`, its shape was
checked by varying the pass count on one build:

| passes | wall |
| ---: | ---: |
| 1 | 0.659 s |
| 4 | 1.553 s |
| 16 | 4.955 s |

Marginal cost is 0.298 s per pass between 1 and 4 and 0.284 s between 4 and 16,
with about 0.37 s of fixed set-up. At the default of 16 passes **93% of the time
is the measured walk**, so the probe is not dominated by first-touch faults or
boot, and the G101 and G119 readings taken from it stand.

### What actually dominates

Every call through `with_address_space` constructs an `AddressSpaceAccess`, whose
constructor takes a `shared_lock` on the address space's `lifecycle_mutex_` and
holds it for the whole operation. That lock is **per address space**, so all ten
vCPU threads in one guest process share it, and libc++'s `lock_shared()` takes a
real internal `std::mutex` - so the "shared" readers serialize.

Measured by removing it in a throwaway build - unsafe, since retirement is then
unguarded, and never intended to ship:

| `guest_mmu_miss_walk_loop`, 10 threads | without the lease | baseline |
| --- | ---: | ---: |
| both orders, median of 3 | 3.530 s, 3.388 s | 4.973 s, 5.114 s |

**About 31%**, with host cores rising from 7.4 to 7.9 as the serialization goes
away. That is larger than G101's registry-lock win on the same probe.

### Where this leaves B-1

The plan's three steps aimed at the wrong two of the three locks on this path:

| lock | scope | measured worth |
| --- | --- | ---: |
| registry `mutex_` (G101, step 1) | process-global | **21% probe, 1.55% Geekbench MC** |
| `lifecycle_mutex_` lease (**unattributed by the plan**) | per address space | **~31% probe** |
| `mappings_mutex` (G119, step 2's target) | per generation | **0%** |

The pattern is scope. The two that pay are shared by every thread that touches
the same address space; the one that does not is already sharded per generation
and sits behind G42's 16,384-set TLB. Step 3's flattening - removing the
`shared_ptr<MappedBacking>` dereference on a hit - is three ordinary loads, strictly
less than the mutex G119 measured at zero, so it is closed on the same evidence
rather than measured separately.

### The safe replacement, which is the next piece of work

The lease cannot simply be deleted: it is what makes `retire()` wait for
in-flight operations before the address space is torn down. The plan already
describes the replacement, for B-2 rather than B-1: *a published per-thread
active flag that teardown scans*, the pattern
`jit_publication.hpp` uses for `jit_store_participants`.

Concretely - and this is a store-buffer problem, so the fences are the design
rather than a detail: an access publishes its address space into a thread-local
slot, issues a `seq_cst` fence, then reads `retired_`; `retire()` stores
`retired_`, issues a `seq_cst` fence, then scans every registered slot until none
names it. Either the access sees the retirement or the scan sees the access, and
never neither. It needs a slot registry with thread-exit unregistration, and TSan
over the domain-lifetime suite is the gate that matters.

That is a self-contained piece of concurrency work with a 31% probe result
waiting behind it, and it is the single highest-value item left in Part B.

## G121: the lifecycle lease replaced with a published per-thread flag

Accepted. This is what B-1 was looking for, and the plan did not name it.

### The change

Every call through `with_address_space` constructed an `AddressSpaceAccess`,
whose constructor took a `shared_lock` on the address space's `lifecycle_mutex_`
and held it for the whole operation - the mechanism by which `retire()` waited
for in-flight work. That lock is **per address space**, so all ten vCPU threads
of one guest process serialize on it.

It is replaced by the pattern `jit_publication.hpp` already uses for
`jit_store_participants`: each thread publishes the address space it is operating
on into its own slot, and teardown scans the slots instead of excluding everyone.

**The fences are the design, not a detail.** An access stores its slot and then
reads `retired_`; retirement stores `retired_` and then reads the slots. Both
sides are `seq_cst`, so the two stores are totally ordered and at least one side
observes the other - an access the scan does not see is guaranteed to see the
retirement and back out. Relaxing either side reopens exactly the window the
lease closed. The slot registry takes a mutex for registration and for the scan,
but an access clears its own slot from its destructor without it, so a scan
always makes progress even while an exiting thread waits to unregister.

### Measured

| `guest_mmu_miss_walk_loop`, 10 threads, median of 3 | candidate | baseline |
| --- | ---: | ---: |
| both orders | 3.544 s, 3.462 s | 4.905 s, 4.797 s |

**About 28%**, against 31% for the throwaway build in G120 that simply deleted
the lock - so the slot store and load cost roughly a tenth of what they replace.

| | single-core | multi-core |
| --- | ---: | ---: |
| baseline (result) | 81 | 507 |
| candidate (result) | 81 | **515** |

Single-core identical, multi-core **+1.6%**, with **13 of 16 multi-core sections
improving** - led by Object Remover +10.1%, Photo Filter +5.2%, PDF Renderer
+3.9% and Structure from Motion +3.6%. Only Horizon Detection -1.5% and Object
Detection -0.4% move against it, both inside their spread.

The candidate ran on the **busier** host of the two - load medians 3.41 against
1.93 cores - so the comparison is if anything against it. Gates: 572/572 unit,
45/45 tooling, **8/8 TSan**, which is the gate that matters for a change that
replaces a lock with fences.

### Where the campaign now stands against the refactor

G100 recorded the pre-refactor build at 81 single-core and 514 multi-core, and
the post-refactor regression that started this campaign at 77 and 477.

| | single-core | multi-core |
| --- | ---: | ---: |
| pre-refactor (G100's record) | 81 | 514 |
| post-refactor band | 77 | 477 |
| now | **81** | **515** |

**Stated carefully:** the 514 was measured on a different day, and G101 is the
entry that established cross-day comparisons are not safe here - the same binary
read 80/495 and 79/479 on consecutive days. Today's host is quieter than most of
this campaign's runs, which lifts the whole level. So the right claim is that the
matched pairs show the regression closed by the changes recorded here, not that
515 and 514 are directly comparable numbers.

### What this says about B-1 as the plan wrote it

The plan proposed three steps and named two locks. Measured:

| lock | scope | worth |
| --- | --- | ---: |
| registry `mutex_` (step 1) | process-global | 21% probe, +1.55% Geekbench MC |
| `lifecycle_mutex_` lease (**not in the plan**) | per address space | **28% probe, +1.6% Geekbench MC** |
| `mappings_mutex` (step 2's target) | per generation | 0% |

The predictor is scope, not the lock's type or the cost of libc++'s
`shared_mutex`. Both locks that paid are taken by every thread touching the same
address space on every miss; the one that did not is sharded per generation and
sits behind G42's TLB. A future reader of this plan should take from B-1 that the
question to ask of a lock is how many threads share it and how often, and that
the answer is cheap to get by deleting it in a throwaway build and measuring
before designing a replacement.

## G122: C-2, the FIFO ticket turn - neutral, and the bistability was already gone

Implemented, measured against a matched control, reverted. The measurement also
answers a question left open since G108.

### The change

G47 rejected a `std::mutex` turn for the reservation publication gate on measured
regressions and specified the replacement: a FIFO ticket turn whose contended
waiters park with atomic wait/notify. That was built - a ticket taken on arrival,
waiters parked on a single `serving` counter, release advancing it by one - in
place of the `compare_exchange` loop on a bool, where every queued helper raced
for the same flag and the winner was whichever thread the scheduler favoured.

572/572 unit, 45/45 tooling, 8/8 TSan.

### Measured

Three runs of each build, `--engine jit`, 10 vCPUs, because the question was
about variance rather than a single score.

| | multi-core | spread | mean |
| --- | --- | ---: | ---: |
| control, HEAD without C-2 | 822, 808, 791 | **3.9%** | 807 |
| C-2 ticket turn | 789, 811, 786 | 3.2% | 795 |

**Neutral.** The ranges overlap, the spreads are the same to within a run, and
C-2's mean is 1.4% lower. There is nothing here to accept, so it is reverted.

### The finding that matters more

G108 measured this engine at **701, 829 and 834** on an unchanged binary - a 19%
spread, with per-section values bimodal rather than distributed, which is what
made the JIT aggregate useless for deciding anything and forced G102's rejection
to be retracted.

**The control above spreads 3.9%.** The bistability is gone, and it was gone
*before* C-2 - so the ticket turn did not fix it, and neither did anything about
the gate's fairness. What changed between G108 and now is the set of accepted
work: G109 removed most of the stale-block validation the JIT was doing on every
lookup, and **G121 removed the per-address-space lifecycle lease that every
thread took on every MMU operation**. A convoy needs a contended serialization
point; G121 removed the one every engine shares.

That is a satisfying resolution of a loose end - the gate was blamed for a convoy
that was mostly somewhere else - but it is inference from the sequence, not a
controlled attribution. Confirming it would mean re-running G108's binary today,
which was not done.

### Consequences

- **C-2 is closed as measured-neutral.** G52's 44-56% of sampled time in
  `wait_for_jit_store_phase` remains the strongest argument that this gate
  matters, but two attempts on it - G47's mutex turn and this ticket turn - have
  now both failed to produce a measurable improvement, and the third possibility
  in the plan, B-3's spin-hint change, was itself retracted. The gate should not
  be attempted again without a profile taken *now*, since the surrounding code has
  changed substantially.
- **B-3 becomes measurable.** It was reopened in G108 because a 19% band could
  not resolve an 8% claim. At 3.9% it can, and re-measuring it is now an ordinary
  trial rather than a blocked one.

## G123: serialize Darwin kernel mappings and repair 4 KiB slots as a unit

Accepted as a correctness repair. The matched hot-path screen shows no
performance regression.

### The regression and why the old implementation appeared to work

The post-refactor 4 KiB build reached systemd and then repeatedly took a native
translation fault in kernel vmalloc memory. It failed under both JIT and gadgets,
while the 16 KiB build passed. An unmasked reproduction placed the fault in the
SLUB allocation path with a Linux PTE still present but its 16 KiB Darwin host
slot absent.

The ordering defect predates the refactor. Two `init_mm` walkers could overlap:
an older walker could snapshot an absent PTE, a newer walker could publish and
map a present PTE, and then the older walker could unmap last. In the 4 KiB guest
profile, applying an absent lower quarter before a present upper quarter could
also remove the whole 16 KiB host slot between those two operations. The fault
recovery path then saw an empty pending-range set and retried the same missing
host mapping indefinitely.

A complete substitution of the pre-refactor native-MMU implementation made one
bounded boot pass, but it did so by restoring untracked global state and stale
mapping behavior. That rollback removed per-run ownership and retirement,
direct/shadow transition atomicity, alias publication, checked error boundaries,
and the tests that protect them. Smaller publication, singleton, and shadow-loop
rollback trials did not restore a correct boot. The rollback was therefore a
diagnostic, not a shippable fix.

### The forward repair

- Every Darwin `init_mm` synchronization is serialized from before the pending
  range is snapshotted and cleared until the walk has applied or requeued every
  range, including internal `-EAGAIN` retries. A same-task owner marker converts
  a nested host fault into a bounded error instead of self-deadlocking.
- The 4 KiB profile reads the four PTEs belonging to one Darwin host slot,
  flushes dirty shadow bytes first, maps every present quarter, and only then
  removes absent quarters. An error preserves the complete pending range for
  retry.
- Kernel vmalloc fault repair explicitly republishes the exact Linux page and
  resumes only when the host bridge proves that the requested mapping and
  permission now exist. Direct mappings are checked against both ownership
  metadata and the actual Mach VM region.
- Publishing a dirty `PROT_NONE` shadow temporarily adds read permission only
  for the copy and restores the exact tracked protection on every success and
  error path.

The broader fact that generic vmalloc writers do not publish all four PTEs as
one atomic operation is not overclaimed here. A racing writer queues a later
serialized replay; exact fault repair republishes a missing page. Temporary
sibling permission granularity remains a pre-existing host-page constraint, not
something this repair claims to eliminate.

### Correctness acceptance

- Canonical Linux replay: tree
  `92749de0046c6c82e901db75f57eeec3500cf358`; 161 changed files, 12 outside
  the port; manifest, topology, policy, ABI, and exact import checks pass.
- Full production kernels build under LLVM 22.1.8 in both page profiles.
- Focused Release tests pass in both profiles; focused ASan/UBSan and the full
  eight-test TSan gate pass. The new policy fixture rejects 24 mutations of the
  serialization, retry, slot-ordering, and fault-validation invariants.
- Immutable-rootfs systemd matrix, 10 vCPUs and 8 GiB: 4 KiB JIT 3/3, 4 KiB
  gadgets 3/3, 16 KiB JIT 1/1, and 16 KiB gadgets 1/1. Every valid run reached
  systemd as PID 1, reported the expected page size, passed the tmpfs OverlayFS
  checks, shut down normally, and emitted no fatal, POSIX-fault, RCU-stall,
  panic, BUG, or Oops marker.
- `rootfs.img` remained mode 0444, size 8,589,934,592, SHA-256
  `501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.

One discarded boot attempt used a process-wide file-size limit and stopped
before guest output because the limit also covered the emulator's sparse RAM
backing. The accepted runs instead bounded only console output and repeated
fatal markers. Two discarded probe attempts were likewise harness-only: one
lacked host process-inspection permission, and one mistakenly multiplied the
payload's own ten threads by ten uploader workers. Neither contributes a timing
or correctness result.

### Performance boundary and measurement

No CPU engine, translated execution path, software-MMU lookup, or ordinary
user-MM synchronization implementation changed. In the 16 KiB build the 4 KiB
slot helpers compile out; normalized `__um_tlb_sync` remains 362 instructions
and its normalized disassembly is identical to the control. The added
checks execute only when kernel or user mappings actually need synchronization
or on fault recovery, not on a normal guest boundary with no pending PTE work.

The matched 16 KiB gadgets `guest_mmu_miss_walk_loop` control used 10 guest
CPUs, one process with ten internal threads, two counterbalanced orders, and
three repetitions per invocation:

| pooled median | pre-fix control | forward repair |
| --- | ---: | ---: |
| wall | 3.699 s | **3.609 s** |
| host CPU | 28.595 s | **28.410 s** |

Both order pairs favor the candidate. The approximately 2.4% wall difference
is not attributed to this cold-path repair, but it rules out a meaningful
regression on the MMU-miss workload that protected G101/G119/G121. At G123, a
full Geekbench comparison was still reserved for final refactor-campaign
acceptance rather than being spent on this isolated kernel correctness fix; the
current disposition above records its later user-directed skip.

Evidence is in `.build/results/forward-init-mm/`.

## G124: the post-migration Geekbench baseline, and a PSI result withdrawn

Two outcomes. The gadget baseline is re-measured on the unified EROFS image, and
a `CONFIG_PSI` regression this entry first claimed is withdrawn as an artifact of
run ordering. The withdrawal is the more useful half.

### Benchmarking was broken and no one had noticed

Every guest prompt matcher anchored on `(?:^|\r?\n)` and bounded the line with
`[^\r\n]*`. When bash execs a replacement shell it emits its bracketed-paste
reset and the terminal returns the carriage without a line feed, so the
transcript reads `\r\n<ESC>[?2004l\r# ` and the bounded run cannot cross that
bare `\r`. The last step of the systemd-overlay login is exactly that sequence,
so `exec sudo -n /bin/sh` timed out at 180 seconds with the prompt already on
screen, taking Geekbench, the command and link benchmarks, and LTP with it.

It survived because the EROFS migration introduced the `uml` login and the
`exec sudo` step, and the final Geekbench run after that migration was skipped by
user direction, so nothing exercised the path again. The covering test mocked
`_read_until` and compared the pattern string to itself, which passes equally for
a matcher that never matches anything. Fixed in `0f6a6fc`, with the patterns
pinned against real console bytes instead.

### The current baseline

16 KiB gadgets, 10 vCPUs, 8 GiB, P-cores, `rootfs.erofs`, image-local Geekbench
6.7.1 with `--no-upload`. Nine complete runs:

| single-core | multi-core |
| --- | --- |
| 80-81 | 503-517, mean 511 |

The multi-core band is **2.8% wide**, materially wider than this campaign has
generally assumed. Several past entries treated 1-2% aggregate differences as
signal; against this band they are not, and conclusions of that size drawn from
unreplicated aggregate pairs should be re-read with suspicion.

**The G121 reference of 81/515 is not comparable and cannot be made comparable.**
The `c98f003` emulator rejects the current invocation outright: its
`has_writable_primary_block_device` check refuses `ubd0c=`, and it forces
`root=/dev/ubda1` plus `ro` for a GPT ext4 image. It cannot boot a raw EROFS
root at all. No same-day control against that build is constructible, so no
regression against 515 is demonstrable in either direction. 515 also falls
inside the band above.

### The withdrawn PSI result

`f209f56` added `CONFIG_PSI=y`. A first experiment ran two default runs
(02:00-03:50) and then two `psi=0` runs (03:55-05:50) and reported 13 of 16
multi-core workloads faster without PSI, mean +0.82%, sign test p about 0.0009,
with single-core flat at +0.14% and a 4/3/9 split. That was accepted here as a
mechanism-consistent finding and a `CONFIG_PSI_DEFAULT_DISABLED=y` fix was
prepared.

**It does not replicate.** The design ran all of one arm and then all of the
other, so elapsed time and arm were perfectly confounded. Re-run as ABBA on one
binary, with both arms passing the parameter explicitly so the compiled default
is irrelevant:

| arm | multi-core | mean | range |
| --- | --- | ---: | ---: |
| `psi=1` | 503, 514 | 508.5 | 11 |
| `psi=0` | 510, 512 | 511.0 | 2 |

The `psi=1` range **fully contains** the `psi=0` range. The same per-workload
analysis that produced 13/1/2 now produces 11/4/1, p about 0.12, with individual
workloads swinging as much as -4.5% and +4.2% - an order of magnitude above the
0.5% being claimed. The kernel config change was reverted unbuilt into the
product.

Three lessons, all cheap to state and expensive to learn:

- **Per-workload consistency does not validate a block-ordered design.** 13 of 16
  agreeing is exactly what a systematic host-condition difference produces. The
  sign test measures consistency, not causation, and is blind to the confound.
- **Flat single-core alongside moved multi-core is not corroboration.** Single-core
  uses 1 of 20 host cores and is far less sensitive to host load than the
  10-core phase, so drift produces that same asymmetry unaided.
- **A rebuild between arms voids the comparison.** A confirmation run built from a
  fresh kernel landed at 509 and was read as evidence against PSI; it was not
  evidence either way, because the binary had changed.

`--kernel-argument` was added to the Geekbench runner for this, which is what
makes a same-binary boot-parameter A/B possible. Evidence is in
`.build/results/psi-abba-*` and `geekbench-head-gadgets-*`.

## G125: B-6, the guest feature surface was starving the vector workloads

Accepted, and the largest single result in this campaign. `virtual_hwcap()`
advertised nine features and `darwin_uml_guest_hwcap2()` returned zero
unconditionally, while both engines had implemented far more. Guest libraries
select their vector paths from those words, so every unadvertised family was
running a fallback the engines never needed it to run.

### The contract

A family is advertised only where an engine has committed to executing it, on
the **identical host predicate its native paths already use**. DotProd rides
`ArmFeature::dotprod`, the same gate `gadget_memory_selection.cpp` and
`JitCompilerFamilies::emit_simd_dot` use; half precision rides the `fp16` probe
that `gadget_simd_selection.cpp` and `jit_compiler_floating.cpp` use; JSCVT
additionally requires `jscvt_subnormal_flags_correct`, because without it the
engines decline the native encoding. The interpreter implements all of these in
software through the dispatcher, so no engine can be handed an opcode it will
not execute. On this host I8MM and BF16 stay dark, correctly - the host lacks
them.

Added: HWCAP FPHP, ASIMDHP, ASIMDRDM, JSCVT, ASIMDDP, ASIMDFHM; HWCAP2 FRINT,
I8MM, BF16.

### Measured with targeted workloads, not the full suite

Geekbench 6.7.1 runs single workloads (`--section`/`--workload`), which
turns a 55-minute suite into a few minutes and reports rates at three
significant figures instead of a rounded integer score. Single-core, gadgets,
interleaved candidate/control against one frozen control binary, n=2 per arm:

| workload | control | candidate | change |
| --- | ---: | ---: | ---: |
| Object Detection | 0.96 img/s | 2.34 img/s | **+144.3%** |
| Photo Filter | 1.02 img/s | 1.88 img/s | **+83.4%** |
| Background Blur | 0.29 img/s | 0.47 img/s | **+63.8%** |
| Structure from Motion | 2.45 Kpx/s | 3.11 Kpx/s | **+27.2%** |
| Horizon Detection | 3.37 Mpx/s | 3.70 Mpx/s | +9.8% |
| HDR | 2.96 Mpx/s | 3.00 Mpx/s | +1.3% |
| Ray Tracer | 96.40 Kpx/s | 94.60 Kpx/s | -1.9% |

The features separate cleanly by workload, which is the evidence that these are
real mechanisms and not drift. An earlier DotProd-only build moved Object
Detection identically (+144%) but left Background Blur at 69 against 69; adding
half precision is what moved Blur and Photo Filter. Ray Tracer, which is scalar
FP, is flat to slightly down and is the one to watch in the full suite.

**These magnitudes are why targeted measurement mattered.** G124 put the
multi-core aggregate band at 2.8%; a +144% workload win is worth roughly 2% of
the aggregate once it is averaged with fifteen others, which the full suite
would have reported as barely more than noise.

### The rest of the surface is host-limited, not implementation-limited

MOPS would be the largest remaining general-computing win - glibc routes
`memcpy`/`memset` through CPY*/SET* when it is advertised - but this host does
not implement it, and emulating it in software would be slower than the guest's
existing NEON fallback. The same applies to CSSC, ECV, AFP, RPRES, WFxT, HBC and
SVE. What remains genuinely implementable is LRCPC/LRCPC2 and FCMA, which the
host and the JIT have but the gadget bank does not, then FLAGM/FLAGM2 and LSE2,
which are not yet in `ArmFeature` at all.

Gates: 621/621 unit. The fail-closed allowlist in `arm64-jit-capability` caught
every bit added here and was extended, not relaxed.

## G126: the guest feature surface, measured end to end

Accepted. Full Geekbench 6.7.1, HEAD against a frozen build of `d2970da`'s
`darwin/` tree - the state before any of this work - on one host, ABBA ordered,
no systemd, 16 KiB gadgets, 10 vCPUs, 8 GiB.

| aggregate | control | candidate | change |
| --- | --- | --- | ---: |
| single-core | 80, 80 | **94, 93** | **+16.9%** |
| multi-core | 506, 498 | **558, 542** | **+9.6%** |

Neither arm overlaps the other on either metric, and the control reproduced to a
single point single-core across both of its runs.

### Where it comes from

| workload | single-core | multi-core |
| --- | ---: | ---: |
| Object Detection | **+142.2%** | **+96.5%** |
| Photo Filter | **+80.5%** | +16.7% |
| Background Blur | **+61.7%** | +15.5% |
| Photo Library | **+44.0%** | **+35.0%** |
| Structure from Motion | **+30.0%** | +23.6% |
| Horizon Detection | +8.8% | +8.6% |

Every one of these is a workload that selects a vector path from HWCAP. Photo
Library was not in the targeted set and is the one result the full suite found
that the focused runs did not; the other five match their targeted measurements
closely, which is the check that the targeted method was measuring the right
thing.

The families added after the vector set - LRCPC, LRCPC2, FCMA, FLAGM, FLAGM2,
USCAT, SB, DIT - contribute nothing here and were never expected to. Disassembly
of Geekbench 5.5.1, 6.7.1 and 7.0.0 counts zero occurrences of all of them.
They are carried on the evidence of `guest_hwcap_sanity.S`, which executes each
one and checks its result, not on a benchmark score.

### What moved against it

Object Remover multi-core -6.5% is the largest regression and sits outside the
2.8% multi-core band G124 established, so it is probably real rather than noise.
PDF Renderer -3.2%, Ray Tracer -2.1% and Asset Compression -1.0% multi-core are
inside or near that band. Ray Tracer is scalar floating point and no advertised
family should reach it; it read -1.9% in targeted testing and -2.1% here, which
is consistent enough across two independent measurements to be worth a look
rather than dismissal. None of these offsets a +16.9% and +9.6% aggregate, but
they are recorded as open rather than explained.

### Method

The measurement rules this campaign paid to learn were all applied: one frozen
binary per arm built before the campaign so no rebuild happened between runs,
ABBA ordering so linear host drift cancels by position, and a guest shell with
no systemd - which also removes the 120-second quiescence phase and everything
it was there to stop. `tools/guest-shell.py` boots in about 5 seconds against
roughly 90 and streams the console live, so a long run's progress is visible
instead of arriving only at the end.

## G127: C-4, Text Processing does not scale on real hardware either

No change made. C-4 has been open since G3 on the premise that Multi-Core Text
Processing running at roughly 1.2-1.4 host cores "fails to scale in *every*
engine, so it is probably one fixable serialization - and that fix would help
gadgets too." **The premise is wrong, and two reference devices settle it.**

### The measurement

Gadgets, 10 vCPUs, 8 GiB, `tools/guest-shell.py` with no systemd, single
Geekbench workloads selected with `--section`/`--workload`. Multi-core Text
Processing reproduced across four runs at 61, 62, 62 and 61 - 4.90 to 4.99
pages/sec - so the number below is not one noisy sample.

| | single-core | multi-core | ratio |
| --- | ---: | ---: | ---: |
| Text Processing | 4.05 pages/s | 4.99 pages/s | **1.23x** |
| File Compression | 14.7 MB/s | 82.7 MB/s | **5.63x** |

**File Compression was measured in the same boot, immediately before Text
Processing.** That is the control the earlier flags never had: the same binary,
guest, host and topology reach 5.6x on one workload and 1.2x on the other, so
whatever limits Text Processing is not a serialization the emulator imposes on
all multi-core work.

Host CPU confirms it directly. File Compression's multi-core phase held
**4.6 to 6.3 host cores**; Text Processing held **1.1 to 1.7**, oscillating
between roughly 113% and 165% rather than climbing.

### What the guest itself sees

Sampling `procs_running` once a second inside the guest, then all thread states
system-wide with `ps -eLo stat=`:

| workload | runnable threads |
| --- | --- |
| Text Processing | **R=2 in 156 of 175 samples** |
| File Compression | reaches R=8, R=11 and R=12 |

One of those two is the sampling loop itself, so Text Processing presents about
**one runnable thread**. `procs_blocked` was 0 in every sample, so its other
threads are sleeping rather than waiting on I/O: the guest kernel has nothing to
place on the other vCPUs, and the emulator is idle for want of work rather than
serializing work it has.

### The reference devices

Two Geekbench Browser results, one of them the **same 6.7.1 for Linux
AArch64 binary this campaign runs**:

| device | cores | SC | MC | ratio |
| --- | ---: | ---: | ---: | ---: |
| iPhone 17 Pro Max (A19 Pro) | 6 | 290.2 pages/s | 348.2 pages/s | **1.20x** |
| Raspberry Pi 4 Model B | 4 | 24.9 pages/s | 29.6 pages/s | **1.19x** |
| this emulator, gadgets | 10 | 4.05 pages/s | 4.99 pages/s | **1.23x** |

Geekbench 6 Text Processing is near-serial by construction, and native silicon
gets 1.19-1.20x from it. **This emulator gets 1.23x - marginally more than
either reference.** There is no missing parallelism to recover.

The same comparison shows the multi-core path is healthy rather than merely
adequate. File Compression scales 5.63x here against 2.55x on the A19 Pro's six
heterogeneous cores and 1.13x on the Pi 4, which is memory-bandwidth bound.

### Disposition

**C-4 is closed as measured, with no defect found.** It should not be reopened
on the aggregate multi-core score alone. Reopening it needs a workload whose
scaling ratio here is materially below the same workload's ratio on comparable
native hardware - which is the comparison C-4 was missing for its whole life,
and the reason a workload property survived three separate flags (G3, G44, G94)
as a suspected emulator bug.

Limitations: single host, single engine, and the ratios above compare rates
across machines of very different absolute speed. The claim is only about the
**shape** of each workload's multi-core scaling, not about any absolute score.

## G128: G-4's prize, sized on the engine that already has the mechanism

No change kept. This measures what vector residency is worth before spending
any effort building it into the gadget bank, because G-4's whole history is a
change that was rejected on a measurement that could not see it.

### Why Trial 10's rejection proved nothing

Trial 10 mapped guest V0-V7 to host Q16-Q23 and was rejected after profiles
found "zero mapped operations" in File Copy, Dhrystone and Pipe Throughput -
three workloads with no SIMD in them. Its composite moved +0.10%, which the log
itself records as not causally attributable. That is a null measurement, not a
refutation, and repeating the implementation without first fixing the
measurement would have risked repeating the outcome.

### The JIT already implements what G-4 asks for

`JitCompilerFamilies` keeps eighteen host vector registers - q6, q7 and q16-q31
- as per-block resident slots with `loaded`/`dirty` tracking, and falls back to
`load_q` straight from `CpuState` when no slot is free. **That fallback is
exactly the gadget engine's permanent behaviour**, so turning residency off in
the JIT reproduces the gadget bank's vector cost model on an engine where the
mechanism can be switched rather than written.

The gadget engine has no equivalent: `simd_three_same_vector`
([floating_simd.inc:2675](darwin/cpu/arm64/gadgets/engine/floating_simd.inc:2675))
loads both operands and stores the result through `CpuState` on every single
guest SIMD instruction, and nothing in `darwin/cpu/arm64/gadgets/` mentions
residency at all.

### Measured

One binary for both arms, selected by an environment variable read once, so no
rebuild happened between runs and the arms cannot differ by code layout. JIT,
10 vCPUs, 8 GiB, no systemd, single-core, ABBA ordered, n=2 per arm.

| workload | residency on | residency off | residency is worth |
| --- | ---: | ---: | ---: |
| Ray Tracer | 359.3, 338.3 Kpx/s | 198.5, 215.9 Kpx/s | **+68.3%** |
| Photo Filter | 6.24, 5.71 img/s | 3.77, 3.68 img/s | **+60.4%** |
| Object Detection | 9.55, 9.41 img/s | 6.60, 6.59 img/s | **+43.7%** |
| Background Blur | 1.37, 1.31 img/s | 1.01, 1.02 img/s | **+32.0%** |

No arm overlaps the other on any workload, and each arm reproduces itself.

**Ray Tracer is the result that changes the picture.** It is scalar floating
point, so a "vector" bank should not reach it - except that AArch64 scalar FP
lives in the same register file, so residency covers it too. It gains the most
of the four. That also bears on G126's open Ray Tracer regression, which was
recorded as unexplained precisely because no advertised vector family should
touch a scalar FP workload.

### What this does and does not establish

It establishes that the `CpuState` round-trip on vector and scalar-FP operands
is a first-order cost on exactly the workloads G125 and G126 moved, and that
**G-4's prize is real and large** - the opposite of Trial 10's finding.

It does not predict the gadget engine's gain. The JIT allocates slots per block
and can keep a value in a register across many instructions; a gadget bank
cannot allocate, because each gadget is a separately compiled body, so it needs
a fixed guest-to-host convention plus per-variant gadget selection - the plan's
step 2. A fixed eight-register convention captures less than eighteen
per-block-allocated slots. These numbers are therefore an **upper bound on the
mechanism's value**, not a forecast of the trial's result.

Correctness under the off arm was checked rather than assumed: 264/264 JIT,
SIMD, floating and vector unit tests pass with residency disabled, so the slower
arm is a valid comparison and not a broken one. The diagnostic switch was
reverted and the tree rebuilt; it is not committed, because a runtime check in
the translation path is not something production should carry.

## G129: G-4's two gadget-shaped routes, one refuted and one blocked

No change kept. G128 established that vector residency is worth 32-68% on the
workloads that matter. This asks the question that decides whether the gadget
engine can collect any of it, because the JIT's mechanism does not port
directly.

### Why the JIT's design does not port

The JIT allocates a resident slot per block and emits `q6`, `q17` or whichever
host register it chose into the instruction it is generating. **A gadget body
cannot do that**: it is assembled once, ahead of time, and names its host
registers statically. A fixed guest-to-host bank therefore needs one gadget
variant per operand register combination - 32x32x32 for a three-operand shape -
which is not a bank anyone can build. Only two routes remain.

### Route 1: fuse adjacent SIMD instructions - measured, and it will not do it

If pairs of SIMD instructions were fused, the intermediate could stay in a
scratch vector register instead of round-tripping through `CpuState`, with no
fixed mapping needed at all. That requires a concentrated pair distribution, so
it was measured with the existing `UML_GADGET_PAIR_HISTOGRAM` build over
Object Detection, Background Blur, Photo Filter and Ray Tracer - the four
workloads G128 measured.

| | share of all translated adjacencies |
| --- | ---: |
| both sides FP or SIMD | **6.3%** (82,448 of 1,310,720) |
| exactly one side | 5.4% |

**There is no head to attack.** 2,677 distinct FP/SIMD pairs; the largest,
`floating_madd_single -> floating_madd_single`, is 0.63% of all pairs, and the
top eighteen together are 44% of FP pairs but only 2.8% of everything. Fusing
the best few cannot reach a mechanism worth tens of percent. This is the same
shape G117 found when it retired the expectation that G-2 was a large lever, and
it is a second independent confirmation of that lesson rather than a new one.

### Route 2: a fixed bank with per-variant selection - blocked on instrumentation

This remains open, and it needs the register-triple distribution: if real
translated code concentrates its SIMD operands on a few registers, a bounded set
of variants covers most accesses, and if it does not, the route is dead.

**That distribution is not cheaply measurable today, which is itself the
finding.** The obvious hook points, `simd_unary_operand` and
`simd_ternary_operand` in
[gadget_simd_selection.cpp:422](darwin/cpu/arm64/gadgets/gadget_simd_selection.cpp:422),
have **one call site each**. Almost all SIMD operand words are built inline,
across roughly twenty separate decode branches of
`program_builder_floating_simd_impl.ipp`, each recomputing
`offsetof(CpuState, vectors) + index * sizeof(VectorRegister)` by hand. An
attempt to instrument the two helpers was written, built and run, and captured
essentially nothing; it was reverted rather than committed, because dead
instrumentation that looks like coverage is worse than none.

Anyone taking route 2 should first fold that offset computation into one
shared helper the decode fragments call. That is a refactor worth doing on its
own terms - it removes twenty hand-inlined copies of one address calculation -
and it creates the single choke point this measurement needs.

### Where G-4 stands

The prize is measured and large (G128). Route 1 is refuted with numbers. Route 2
is neither refuted nor available: it is blocked on a small, well-defined
refactor rather than on a judgement call. G-4 stays open with a concrete next
step, which is a better position than the one Trial 10 left it in, where the
prize itself was believed to be zero.

## G130: G-4's fixed bank is worth 6-26%, and the shape it has to take

No runtime change kept. G129 left route 2 - a fixed guest-to-host vector bank -
blocked on a measurement that needed a choke point the code did not have. The
preceding commit created it by folding twenty-seven hand-inlined copies of the
vector operand offset into one helper, so both questions can now be answered.

### Translated code concentrates its vector operands

Captured with `UML_GADGET_SIMD_OPERAND_HISTOGRAM` over Object Detection,
Background Blur, Photo Filter and Ray Tracer, gadgets, 10 vCPUs.

| registers | share of vector operand accesses |
| --- | ---: |
| v0-v7 | **52.0%** |
| v8-v15 | **2.6%** |
| v16-v31 | 45.4% |

| bank width | coverage | uniform would be |
| ---: | ---: | ---: |
| 4 | 33.1% | 12.5% |
| 8 | **54.0%** | 25.0% |
| 12 | 68.3% | 37.5% |
| 16 | 80.8% | 50.0% |

The distribution is concentrated at roughly twice uniform, and **v0-v7 is
essentially the optimal eight**: the best eight by count are v0-v6 plus v16 at
54.0%, against 52.0% for the contiguous low eight. Trial 10 picked the right
registers. v8-v15 are nearly dead at 2.6%, which is what AAPCS64 predicts -
they are callee-saved, so compilers avoid them in leaf vector code.

Note the totals are snapshots at a 65,536-record dump boundary rather than
whole-run counts, which is fine for proportions and is all that is claimed.

### What a fixed eight-register bank would actually buy

Measured, not extrapolated, by restricting the JIT's residency to guest v0-v7 -
a fixed eight-register bank - and comparing against both its normal eighteen
per-block slots and no residency at all. One binary, three modes selected by
environment variable, palindromic order A C B B C A, n=2 per arm, single-core.

| workload | none | **fixed v0-v7** | full | fixed over none | share of the gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| Object Detection | 6.54 | **8.22** | 9.53 | **+25.6%** | 56% |
| Ray Tracer | 216.85 | **263.00** | 320.25 | **+21.3%** | 45% |
| Background Blur | 1.00 | **1.15** | 1.33 | **+15.5%** | 47% |
| Photo Filter | 3.74 | **3.96** | 6.12 | **+6.0%** | 9% |

No arm overlaps its neighbour on any workload. **A fixed eight-register bank is
worth 6% to 26%** and recovers 9% to 56% of what full per-block allocation gives.
Photo Filter is the outlier in both directions: it gains most from full
residency and least from a fixed bank, so its working set is wider than eight
registers.

### The design this forces

Residency only avoids a load if the value is already in a host register the
gadget body names statically, so the bank costs one gadget variant per resident
guest register **per operand position**. A three-operand shape with eight
resident registers is 9^3 = 729 variants, which is not buildable. Two
consequences follow, and they are what the next attempt should start from:

1. **Only one operand position can be resident.** The destination is the one
   worth taking, because the accumulate shapes read it, update it and write it
   back, so residency removes a load *and* a store from each instruction rather
   than one load. That is 8 variants per operation shape, not 729.
2. **The top FP pair in G129's histogram is exactly that shape.**
   `floating_madd_single -> floating_madd_single` is the largest FP adjacency at
   0.63% of all pairs; a repeated accumulate into one register stores and
   reloads it every iteration today.

What remains genuinely hard is not the bank but the flush protocol: any gadget
that is not residency-aware must observe the current value in `CpuState`, so the
translator has to track which guest register is live in which host register
across a block and emit a flush before every unaware access, and an exception
between blocks must not leave guest state live in a host register. That is
correctness-critical work on the host boundary, and it is the whole remaining
cost of G-4.

### Disposition

G-4 is no longer blocked and is no longer speculative. The prize is measured
twice (G128 bounds it above at 32-68%, this bounds the buildable version at
6-26%), the register set is chosen by data rather than by intuition, and the
variant explosion that made route 2 look impossible is resolved by restricting
residency to the destination. The diagnostic switches used here were reverted;
neither is committed.

### Later handoff disposition (2026-08-12)

G130 is an optional future optimization, not unfinished refactor or JIT-cache
acceptance work. Its JIT result bounds the prize but cannot close the gadget
mechanism: the current gadget engine has neither resident/dirty vector state nor
a flush protocol, so there is no valid existing arm to measure.

Reopen only as a default-off bounded gadget prototype. Start with one
nonfaulting accumulate family, destination-only v0-v7 variants, and no
cross-block residency. Dirty state must be materialized before every unaware
access, helper, terminal, fault, interrupt, link, and block boundary; exact
retirement and all unrelated gadget positions and 64-byte phases must remain
unchanged. Focused 16 KiB and 4 KiB correctness plus a sanitizer arm precede a
matched sensitive/control campaign. Screen P first on control-normalized wall:
it must improve strictly more than 2%; only then run E, whose regression must be
strictly less than twice the measured P gain. Even a focused pass leaves the
selector OFF until a representative gadget workload confirms it.

## G131: B-3, the profile G122 required, and why the score route failed

No runtime change kept. G122 reopened B-3 as "an ordinary trial rather than a
blocked one" and separately required that the gate not be touched again "without
a profile taken *now*". Both were attempted. The score comparison failed for a
reason worth recording, and the profile found something the score never could.

### The score comparison could not resolve it, and the failure is instructive

B-3 step 1 replaces the gate's `yield` - architecturally a no-op on a machine
with no SMT, so the loop has no backoff at all - with `isb`, a real backoff.
Measured as one binary with both arms behind an environment variable, JIT,
10 vCPUs, multi-core Clang, Object Detection, Photo Filter and Ray Tracer, ABBA.

| workload | by run position: A B B A |
| --- | --- |
| Clang | 407, 434, 430, 434 |
| **Object Detection** | **404, 437, 865, 840** |
| Photo Filter | 347, 360, 495, 380 |
| Ray Tracer | 861, 1530, 1318, 1068 |

**Object Detection doubles between position 2 and position 3.** That tracks run
order, not arm, and the cause is mundane: the host had been building and running
unit suites immediately before, and was still settling. Within-arm spread
reaches 2.08x, against the 3.9% band G122 established for the multi-core
*aggregate*. Individual multi-core workloads are far noisier than the aggregate
those 3.9% were measured on, so B-3 was never resolvable this way at n=2, and
running more arms would have bought precision on a confounded quantity.

ABBA is what made this visible rather than misleading: with A at positions 1 and
4 and B at 2 and 3, a monotonic drift largely cancels in the arm means, and the
by-position sequence exposes the drift directly. Reported as a failed
measurement, not as a result.

### The profile, which is decisive

Counting the mechanism instead of its score, over the same four multi-core
workloads:

| | count | per gate entry |
| --- | ---: | ---: |
| `JitExclusiveOperationScope` entries | 262,181 | - |
| spin hints executed in `jit_gate_value_changed` | **22,369,571** | 85.3 |
| iterations of the `has_published_jit_store` loop | **5,113,608** | 19.5 |

The gate is hot, so B-3's premise survives. But the second row is the finding.
`jit_gate_value_changed` spins at most 64 times before parking on
`atomic::wait`, and it averages 85 hints per entry, so waiters routinely
exhaust the spin and park - which is the behaviour the 64-iteration budget was
chosen for, and it bounds what any substitute hint can be worth.

**The third row is where the cost actually is.** Every one of those 5.1 million
iterations calls `has_published_jit_store`, which takes
`jit_store_participants_mutex` and runs `any_of` over the participant vector.
That is **5.1 million mutex acquire/release pairs and 5.1 million linear scans**
in a four-workload run, on a gate whose own protocol is otherwise lock-free.

### The fix, specified

G102 deferred this half because "the natural implementation, an atomic count of
non-idle participants, would put a contended global read-modify-write on
`JitBlockPublication`'s deliberately lock-free direct-store path to speed up a
rare waiter, which is the shape G33 and G34 were rejected for." That objection
is correct and is avoidable, because the waiter does not need a count - it needs
to read the states without a lock.

Move the observable state out of `JitStoreParticipant` and into a stable,
index-addressed slot array; the participant holds a slot index and its `state`
becomes an accessor onto that slot. Registration and release keep the existing
mutex, which is cold. The scan then reads a flat array of atomics with no lock
and no pointer chasing, and **the store path is unchanged** - a participant
still only stores its own state, at a different address, so nothing is added to
the lock-free path and G102's objection does not apply.

The lifetime hazard that forces the mutex today disappears with it: the scan
never dereferences a participant, so a released slot simply reads `idle`, which
is exactly what "no published store" means. A destroyed participant cannot be
observed at all.

### Disposition

- **B-3 step 1, the ISB substitution, is neither accepted nor rejected.** The
  measurement was confounded; the profile bounds its value by showing waiters
  exhaust the spin budget and park regardless. Re-measuring it needs a quiesced
  host and the multi-core aggregate, not per-workload scores.
- **B-3's second half is now the substantiated item**, with 5.1 million lock
  acquisitions behind it rather than an argument. It is a concurrency change on
  the store-publication protocol with 27 `state` call sites, so it wants TSan
  and a stress campaign of its own rather than being appended to another
  session's work. It was deliberately not started here.

## G132: B-3's second half, the store scan made lock-free

Accepted. G131 located the cost: 5,113,608 iterations of
`JitExclusiveOperationScope`'s wait loop over four multi-core workloads, each
one taking `jit_store_participants_mutex` and running `any_of` over a vector of
participant pointers - 5.1 million lock acquire/release pairs and linear scans
inside a protocol that is otherwise deliberately lock-free.

### The change

A participant's observable store state moves out of `JitStoreParticipant` and
into a slot in a static array; the participant holds a pointer to its slot and
`state` becomes an accessor. Registration and release keep the existing mutex,
which is cold - once per executor. The scan reads a flat array of atomics with
no lock and no pointer chasing.

**The store path is untouched.** G102 deferred this half because the obvious
implementation, an atomic count of non-idle participants, "would put a contended
global read-modify-write on `JitBlockPublication`'s deliberately lock-free
direct-store path to speed up a rare waiter, which is the shape G33 and G34 were
rejected for." That objection is correct and does not apply here: a participant
still only stores its own state, at a different address.

### Why the mutex was there, and why it is no longer needed

It was not ordering the states. Those are `seq_cst` on both the store and the
scan, which is what orders them against `jit_exclusive_operation_phase`; the
Dekker pattern between a publishing store and a helper announcing itself is
unchanged. The mutex was preventing the scan from dereferencing a participant
that had been destroyed - `unregister` says so directly.

Slots remove that hazard instead of guarding it. The scan reads static storage
that outlives every participant, never dereferences a participant, and a
released slot reads `idle`, which is exactly what "no published store" means.
Release stores `idle` before clearing the claim, so a scan racing a release sees
either the participant's final state or `idle`, and once the participant is gone
those mean the same thing.

Three further properties are deliberate rather than incidental:

- **Exhaustion cannot break correctness.** `jit_store_slot_capacity` is 1024. A
  participant that cannot claim a slot keeps publishing into its own storage and
  joins an overflow list the scan consults under the mutex, exactly as every
  participant did before. Exhaustion costs speed, never visibility.
- **The scan bound only grows.** Shrinking it would race a participant still
  publishing through a lower index. A released slot reads `idle`, so scanning
  one costs a load and cannot produce a false positive. Registration claims the
  lowest free slot, which keeps the bound tight across a long run.
- **Copying a participant is a compile error.** `published_` points into the
  object until a shared slot is claimed, so a copy or move would leave one
  aliasing the other's storage. All four are deleted.

### Measured

The multi-core score could not resolve this and is reported as unusable rather
than as a result: across eight arms, Object Detection ran 350 to 972 and Photo
Filter 340 to 846 - the bimodal per-workload behaviour G108 documented, which
G122 cleared only for the *aggregate*. `guest_reservation_store_loop` was also
tried and is too short at 0.074 s wall, with both arms fully overlapping.

So the mechanism was timed directly. One binary, the previous mutex scan behind
an environment variable, multi-core Object Detection and Photo Filter, ABBA.
The dump fires on a fixed wait count, so **both arms measured exactly 180,225
gate waits** and the times are directly comparable.

| arm | time in the wait loop | mean |
| --- | --- | ---: |
| mutex and vector scan | 272.37 ms, 272.50 ms | 272.4 ms |
| **lock-free slot scan** | **247.67 ms, 242.56 ms** | **245.1 ms** |

**10.0% less time in the gate wait loop**, with no overlap between the arms and
the control reproducing to 0.05%. No multi-core regression was observed; the
scores are simply too noisy to claim anything from, in either direction.

### Gates

TSan 8/8 including `arm64-jit-publication_gate`, unit 572/572 at 16 KiB,
573/573 at 4 KiB, ASan/UBSan 572/572, tooling 50/50. Both page profiles were run
because the change is in shared host code. The measurement switch and the wait
timer were removed before the gates above were re-run, so they cover exactly the
code that ships.

## G133: the block cache held 12% of the working set

Accepted. `cache_sets` 8192 -> 131072 and `arena_size` 64 MiB -> 512 MiB in
`darwin/cpu/arm64/jit/jit_runtime_contract.hpp`, pinned by
`the_cache_holds_more_than_twice_the_former_capacity` in
`tests/unit/arm64/arm64_jit_cache_validation_tests.cpp`. This is the largest
effect measured in this campaign and it supersedes the framing of G126 and the
segmentation and page-granular-invalidation trials, which all attacked
invalidation while the real constraint was capacity.

### What the guest actually translates

`UML_JIT_PROFILE_OVERLAP=1` records the distinct guest PCs each thread
translates and merges them, counting how many threads translated each PC. Over
multi-core Clang followed by Object Detection, JIT, 10 vCPUs:

| | |
| --- | ---: |
| total block translations | 109,056,465 |
| sum of per-thread distinct PCs | 2,095,646 |
| **union of distinct PCs** | **283,353** |
| PCs translated by exactly 10 threads | 197,859 (70%) |
| PCs translated by exactly 1 thread | 79,072 (28%) |

**385 translations per distinct block.** That factors into 52x within-thread
retranslation (109.0M / 2.10M) and 7.4x cross-thread duplication (2.10M /
283K). The within-thread factor is the larger by seven times, and it is pure
capacity: the cache held 8192 sets x 4 ways = 32,768 entries against a 283,353
block working set, about 12% of it.

### The cache and the arena are one resource

One binary shape per arm, same campaign, same host, JIT, 10 vCPUs. Scores are
Geekbench 6.7.1 workload scores; RSS is the emulator's peak resident size.

| sets | arena | Clang | Object Detection | peak RSS |
| ---: | ---: | ---: | ---: | ---: |
| 8192 | 64 MiB | 426, 431, 438 | 414, 441, 445 | 4682 MiB |
| 8192 | 256 MiB | 310, 312 | 359, 361 | - |
| 65536 | 64 MiB | 547, 528 | 1354, 1247 | - |
| 65536 | 256 MiB | 756, 714 | 1330, 1241 | 8053, 8034 MiB |
| 131072 | 64 MiB | 532, 529 | 1282, 1147 | 7015, 6902 MiB |
| 131072 | 256 MiB | 800, 795, 813 | 1280, 1321, 1310 | 8243, 8106 MiB |
| **131072** | **512 MiB** | **808, 779** | **1390, 1375** | 8164, 8294 MiB |
| 131072 | 1024 MiB | 796, 804 | 1360, 1336 | 8048, 8044 MiB |

Against the 8192/64 MiB baseline the accepted configuration is **+84% on Clang**
(432 -> 794 mean) and **+219% on Object Detection** (433 -> 1383 mean).

The arena saturates at 512 MiB. Clang is flat from 256 MiB upward (803, 794,
800) and Object Detection gains about 6% from 256 MiB to 512 MiB on
non-overlapping ranges (1280-1321 against 1375-1390) then gives it back at
1 GiB. Peak resident size is unchanged across all three, 8046 MiB to 8229 MiB,
because the arena is faulted in on use rather than on reservation, so the
larger size is headroom for a bigger guest working set at no measured cost.

Neither constant is sufficient alone, and the interaction has a sign change
worth recording: **enlarging the arena alone makes Clang worse** (432 -> 311),
because a large arena behind a small cache only holds more dead code, while at
65536 sets and above the same arena change is a large gain (538 -> 735). Arena
exhaustion calls `clear_cache()`, so a cache big enough to hold the working set
is worthless if a small arena keeps discarding it. Object Detection saturates
once the cache is large and is indifferent to the arena; Clang, with the larger
code footprint, needs both.

### What this cost, and what it did not fix

Peak emulator RSS rises from 4682 MiB to about 8229 MiB at 10 vCPUs, roughly
+355 MiB per executor. Most of that is the arena, since `block_cache_` is
value-initialised and therefore resident while `saved_guest_words_` uses
`make_unique_for_overwrite` and faults in only what is touched.

The 7.4x cross-thread duplication is untouched. Every executor still translates
the same guest code into its own cache, which is both wasted work and the
reason the memory cost multiplies by vCPU count. A shared cache in FEX's shape
(one `CodeBuffer` owning a `GuestToHostMap` shared across threads, per-thread
L1 in front) is the remaining route; it would reclaim most of the memory this
entry spends and remove the 7.4x, at the cost of a lock on the compile path and
a rework of the per-executor block-link ownership.

### Gates

Unit 572/572 at 16 KiB, 573/573 at 4 KiB, tooling 50/50. Both page profiles
were run because the change alters cache geometry that the 4 KiB profile
indexes differently. `cache_replacement_keeps_validation_metadata_bounded` kept
a private copy of `cache_index` hardcoding the 8192 mask, so its hand-picked
colliding offsets silently stopped colliding; it now derives the mask from
`cache_sets` and uses offsets re-derived for the current geometry, and its
static asserts fail loudly if the geometry changes again.

## G134: translations shared between executors, and what G133 left it

Implemented and kept, measured neutral to negative on the campaign that
motivated it. `SharedTranslationCache` in
`darwin/cpu/arm64/jit/jit_shared_translations.cpp`, pinned by
`a_second_executor_reuses_the_first_translation_safely` in
`tests/unit/arm64/arm64_jit_cache_validation_tests.cpp`. The switch
`share_translations_between_executors` in `jit_runtime_contract.hpp` selects
the arms and ships enabled in ordinary builds. The guest-GPR link-profile build
keeps it disabled: its generated code embeds executor-owned counter pointers,
and reusing those pointers after the compiling executor is destroyed would be
a use-after-free. The profile runtime test statically pins that diagnostic-only
exception rather than expanding the production relocation ABI.

### What made sharing possible

G133 measured 7.4x cross-thread duplication: ten executors translating the same
guest code into ten private caches. The obstacle was that a translation embeds
absolute pointers into the link tables of the executor that compiled it, so the
host code is not portable between executors.

Those pointers enter ordinary generated code at exactly two places,
`emit_direct_linked_exit` and `emit_indirect_linked_exit`, and both arrive
through `Arm64Emitter::load_literal_constant`, which parks the value in a
64-bit tail literal pool slot. Recording the slot offset makes reuse a two-word
rewrite. `load_relocatable_pointer` tags those literals, `finish()` records
where they land, and a reusing executor copies the words into **its own** arena
while `ExecutableArena::store_relocated` rewrites the slots to its own link
records before publishing the instruction cache.

Nothing executable, no arena and no link record is shared, and no lock exists
on any path execution can reach. What is shared is decode and emit. A
translation that had to materialize a pointer through a cold MOV-wide stub
instead of a pool slot, which happens only when the pool falls outside imm19
reach, is marked unshareable and reused by nobody.

The key is the guest PC plus the exact guest words, so an entry is a pure
function of its input and can never go stale. That deliberately decouples this
cache from generation and `code_changes` invalidation.

### It works, and it does not pay here

**Historical terminology clarification (2026-08-12).** The predecessor
counter described the 1.3 million events below as `reused`, but it did not yet
separate an exact shared-cache lookup from a completed private installation.
Read that dated observation as lookup hits, or `matched=` in the current
profile. Current `reused=` is narrower: it advances only after metadata
validation, private-link resolution, relocated arena storage, and block
installation all succeed. The retained predecessor aggregate cannot be
retroactively split, so its original number and wording remain as history.

Over multi-core Clang then Object Detection, JIT, 10 vCPUs: **262,144
translations published and 1.3 million reused against 291,000 misses**, an
81% reuse rate, with the published count landing near the 283,353 distinct
blocks G133 measured.

Same-session ABBA, four runs per arm, one constant between them:

| | shared | control | delta |
| --- | ---: | ---: | ---: |
| Clang | 754, 756, 759, 749 | 761, 763, 725, 763 | **+0.2%** |
| Object Detection | 1204, 1150, 1164, 1267 | 1308, 1301, 1274, 1202 | **-5.9%** |

**Sharing is free on Clang and costs about 6% on Object Detection**, with
overlapping ranges on the latter, so the true cost is somewhere between 3% and
9%. Avoiding 1.3 million compilations bought nothing measurable.

The reason is G133. Sizing the block cache to the working set cut total
translations from 109 million to under 4.2 million, so the compilation this
removes is no longer on the critical path, while the lookup and publication
cost falls on every local miss. Object Detection has the long FP and SIMD
blocks, so it carries the largest per-operation cost: up to a 256-byte guest
word comparison per lookup and the same again per publication.

The same-session control mattered. Measured against this morning's G133
numbers the loss looked like 10%; the control had itself drifted from 794 and
1383 to 753 and 1271 over the session, and the real effect is 6%.

### Rejected along the way

Two intermediate shapes were measured and discarded, both against the
morning baseline before the drift was known, so treat them as ordering only:

| | Clang | Object Detection |
| --- | ---: | ---: |
| one mutex for the whole cache | 721 | 1232 |
| sharded 64 ways | 745 | 1279 |
| sharded, single copy via `store_relocated` | 767 | 1245 |

A single mutex convoyed ten executors on every miss and insertion. Sharding by
guest PC recovered about a third of that, and having the arena apply the
relocations as it stores removed a second full copy of every reused
translation. Both are in the shipped form.

An earlier version published unconditionally and made the instruction evidence
suites pathological: a single-executor process copied and stored every one of
millions of mostly-unique translations for a reader that never arrives, turning
a 23-second test into more than 20 minutes. Publication is now gated on two or
more live executors. Lookups stay unconditional because an empty cache makes
them a hash probe.

### Gates

Unit 622/622 at 16 KiB including tooling, 573/573 at 4 KiB, ASan/UBSan 572/572,
TSan 8/8. ASan carries real weight here: the pinning test destroys the
compiling executor before the reusing one runs, so a missed relocation is a
use-after-free rather than a silent branch through another executor's link
table. The engine object boundary audit rejected a first attempt that put the
report's `getenv` in a production object; it is now inside
`UML_ENABLE_JIT_PROFILE`.

## G135: the JIT cache geometry is a budget, not a set of constants

Accepted, and it corrects G133. `darwin/cpu/arm64/jit/jit_cache_geometry.cpp`
derives every per-executor JIT structure from a whole-process budget that
defaults to half of guest memory, overridable with `--jit-cache-budget=`.

### The regression G133 shipped

G133 chose 131072 sets and a 512 MiB arena against multi-core Clang and Object
Detection, recorded peak RSS rising from 4682 MiB to 8229 MiB, and judged that
acceptable. It was measured on one campaign and never on a desktop, where the
guest's code footprint is far larger and more diverse, and a KDE boot was
reported using about 30 GB.

Sizes per executor, which multiply by the guest's CPU count:

| structure | bytes | resident |
| --- | ---: | --- |
| `block_cache_` | 52 MiB | value-initialised, so immediately |
| `saved_guest_words_` | 128 MiB | faulted on use |
| `arena_` | 512 MiB | faulted on use |
| direct link table | 128 MiB | 1,048,576 x 128 B |
| indirect link table | 56 MiB | 1,048,576 x 56 B |
| `tracked_store_arena_` | 32 MiB | faulted on use |
| `active_links_` reserves | 8 MiB | reserved |

**916 MiB per executor, 9.0 GiB at ten vCPUs**, against roughly 119 MiB and
1.2 GiB before G133.

The link tables are the part that was not a deliberate choice.
`maximum_block_links` was `cache_sets * cache_ways * 2`, so raising the set
count by sixteen raised the link tables by sixteen as well, and `JitBlockLink`
is `alignas(128)`. That is 184 MiB per executor that has nothing to do with
wanting a larger block cache. The reservation was also twice what is
reachable: a block the cache cannot hold cannot be a link target either, so
the geometry now allocates one link per cached block.

### What the budget buys

**Historical arithmetic clarification (2026-08-12).** The table and totals in
this G135 section are the accepted predecessor calculation from before the
bounded shared-cache carve and before complete eager and retained storage
accounting. They remain here as dated evidence rather than being rewritten to
look current. The current geometry reserves the shared tier first and charges
the full per-executor remainder described in G136.

The budget is split evenly across the guest's CPUs, and each executor spends
its share at 30% lookup structures, 45% arena, 8% tracked stores, with the set
count rounded down to a power of two and the link tables sized to the blocks
the cache can hold.

| guest | vCPUs | sets | arena | per executor | total |
| --- | ---: | ---: | ---: | ---: | ---: |
| 4 GiB | 10 | 32768 | 92 MiB | 177 MiB | **1.72 GiB** |
| 8 GiB | 10 | 65536 | 184 MiB | 352 MiB | **3.44 GiB** |
| 16 GiB | 10 | 131072 | 368 MiB | 673 MiB | 6.57 GiB |

Against the fixed 9.0 GiB, an eight-gigabyte guest is bounded at 3.44 GiB and
a four-gigabyte guest at 1.72 GiB, and the total now follows the guest instead
of ignoring it.

### Measured cost

Same workload and host as G133 and G134, two runs:

| | Clang | Object Detection | peak RSS |
| --- | ---: | ---: | ---: |
| fixed 131072 sets, 512 MiB arena | 753 | 1271 | 8229 MiB |
| budget derived, 8 GiB guest | 699, 691 | 1207, 1203 | 7521, 7617 MiB |

**About 8% on both scores for 660 MiB of resident memory on this campaign.**
The resident saving is much smaller than the 5.6 GiB the worst-case arithmetic
suggests, because these structures are faulted in on use and Clang plus Object
Detection never filled the 512 MiB arena. What changed decisively is the
bound, which is what a desktop reaches and this campaign does not. A guest with
more memory gets a larger budget and recovers the scores; a 16 GiB guest lands
back on G133's geometry.

The desktop that motivated this was not re-measured, so the 30 GB figure is
reported as the user observed it and the fix is claimed only against the
arithmetic and the campaign above.

### Still duplicated

Roughly 70% of a per-executor budget is shareable in principle and is not yet
shared. `saved_guest_words_` is a verbatim copy of guest instruction bytes,
identical in every executor and now stored a third time inside the G134
translation cache; replacing the 256-byte inline slot with a pointer to that
shared entry would collapse it. The arena is per-executor only because
generated code embeds absolute link pointers, which G134's relocation contract
rewrites per executor rather than removing; making generated code load the link
table base from a register would make the arena shareable at the cost of one
extra load per linked branch. The block cache genuinely resists sharing, since
72 of its 104 bytes per entry are executor-specific pointers and bookkeeping.

### Gates

Unit 572/572 at 16 KiB, 573/573 at 4 KiB, tooling 50/50 on both, ASan/UBSan
572/572, TSan 8/8. `cache_replacement_keeps_validation_metadata_bounded` now
searches for colliding offsets at run time instead of hardcoding offsets for
one set count, and the capacity pin is a runtime requirement for the same
reason.

## G136: bounded JIT-cache foundation and predecessor final-profile evidence

Recorded 2026-08-12 for the uncommitted working-tree checkpoint after outer
HEAD `1d28525afce272ac27473b4f3fabfbc6200a5b41`. This is not a new accepted
implementation commit and does not reattribute G133--G135's dated score or RSS
observations. It records the completed foundational mechanisms and the first
post-`1d28525` diagnostic that satisfies the final-profile evidence contract;
later publication/accounting follow-ups are distinguished below.

### One authoritative budget and fixed stable link storage

The host now derives the guest CPU count and memory size with the maintained
kernel's own `ncpus=` and `mem=` rules. The budget and expected executor count
publish as one first-valid pair: an identical repeat succeeds, while an
incompatible later pair fails instead of silently retaining geometry derived
for another run. The bounded shared-cache reservation is removed first and the
remaining whole-process budget is divided among the expected executors.

The per-executor charge covers eager implementation/TLB state, block-cache
backing, the complete fallback validation-word slab, fixed direct and indirect
link arrays and fixed indexes, active-record lists, retained relocation
scratch, and page-rounded code, tracked-store, and floating-transaction arenas.
Direct and indirect link records no longer grow after construction, so an
embedded link address remains stable for its arena's lifetime. Capacity
exhaustion takes the existing semantic unlinked path and is counted; it cannot
escape the budget through container growth. A compilation that misses a
requested link remains locally correct but is degraded and cannot occupy a new
canonical shared key.

### Bounded sharing and one owner for validation words

The process-global shared cache is independently bounded by entry count and
accounted bytes. Its key binds the guest PC, exact consumed instruction-word
prefix, and complete code-generation profile. An admitted entry owns one
immutable copy of that exact prefix, and every successfully materialized block
aliases it. A single-executor compilation, refused publication, or any other
noncanonical result aliases its executor's already-accounted fallback slab;
the full slab remains charged even though shared ownership avoids another
committed copy when admission succeeds.

The current counters deliberately distinguish stages. `matched=` means that an
exact canonical entry was found. `reused=` means the consumer subsequently
validated all retained metadata, resolved every executor-private link,
performed the only permitted relocation rewrite in
`ExecutableArena::store_relocated()`, stored the code in its private arena, and
installed the block. Invalid metadata, direct/indirect link capacity, and
private-arena exhaustion are materialization failures and fall back to local
compilation. Thus a match is evidence of a lookup, not automatically evidence
of saved compilation work.

The same-session saved-word `A B B A A B B A` campaign remains deliberately
modest evidence: candidate/control medians moved -0.34% for Clang, -2.16% for
Object Detection, and +0.62% for maximum RSS. The score deltas are within the
2.8% campaign band, the collapsed second candidate run was retained, and RSS
moved in the wrong direction for a memory-win claim. The ownership change is
kept for its single immutable source and bounded-accounting contract, not for a
claimed score or RSS improvement.

### Executor pool and the prepared-memory regression it exposed

One managed run owns at most its configured guest CPU count of JIT executors.
Lightweight task-carrier wrappers borrow them exclusively; an extra borrower
waits in FIFO order rather than constructing an unbudgeted executor, and a
queued borrower wins over a tight preferred-slot reborrow. The pool is bound to
one session, runtime memory domain, and code-generation profile, and drains
before that domain retires.

The first integration attempt exposed a real pooling bug. An executor moving to
a different task-carrier `StaticGuestMemory` could see the same generation and
mapping-change tuple as its prior borrower and skip `memory.prepare()`. The
fresh memory object's overlay was then unprepared even though the executor TLB
did not require a sequence refresh. The fix tracks the exact prepared-memory
identity separately, clears it at every begin/end boundary, and prepares the
first block of each distinct memory object while keeping executor-TLB
preparation conditional on mapping changes. Focused executor-pool tests passed
in both page profiles and in the ASan/UBSan and TSan configurations at the
pre-follow-up checkpoint; the diagnostic below also completed the previously
stalled real overlay workload. Those passes are correctness and structure
evidence, not throughput evidence.

### Executor-pool focused A/B and exact-token follow-up (2026-08-12)

The first current-tree, global-mask executor-pool comparison is recorded at
`.build/results/jit-cache-handoff/executor-pool-perf-20260812/README.md`.
The control preserves the prior wrapper-owned executor lifetime and bypasses
only the new process-wide live-executor ceiling; geometry, code generation,
shared-cache policy, probe bytes, and measurement logic remain matched. The
probe is `guest_syscall_boundary_loop` under shell init and JIT with 1 GiB guest
memory: P10/9 workers and E4/3 workers, ordered
`pool, control, control, pool, pool, control, control, pool`, with three
repetitions per invocation. The primary statistic is the median of each
invocation's three repetitions and then the median across four invocations.

| host class | pool wall | standalone wall | pool change | pool host CPU | standalone host CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| performance | 21.314 s | 25.619 s | **-16.80%** | 159.105 s | 184.700 s |
| efficiency | 18.489 s | 17.590 s | **+5.11%** | 36.240 s | 30.690 s |

The P-core raw ranges do not overlap and both counterbalanced halves agree
(-17.09% and -16.54%). The E-core ranges overlap and its halves reverse
direction (+8.06%, then -2.12%), but the primary wall result exceeds the 2%
review threshold and host CPU regresses 18.08%. The pool is therefore **not
accepted** on this evidence. No sample was removed and no direct contention or
stack profile was captured.

Every borrow and release in that measured candidate modifies one global
`free_mask_` RMW cache line. That is a plausible E-core mechanism, not an
observed attribution. The follow-up replaces it with one 128-byte-aligned
atomic session-token cell per slot. Preferred-slot acquisition remains first,
generic acquisition remains lowest-slot-first, and the FIFO waiter gate,
session/profile binding, lazy construction, exact cap, and pre-domain drain are
unchanged. Claim CASes only the requested session token to zero and then reads
the stable owner; release publishes that token and drain exchanges cells to
zero independently. Exact token identity prevents a borrower paused after an
old session check from claiming a restarted session's executor even if the
allocator reuses its address. Standalone leases are excluded from registry
reservation through managed teardown, while a private tagged pool factory may
construct only during an active, non-release-pending session. A separate
four-slot TSan fixture pins concurrent exclusivity, ordered waiters versus a
tight reborrow, mixed idle/active drain, stale-check restart, activation-window
lease exclusion, and fresh-session recovery.

The corrected exact-session-token repeat is recorded at
`.build/results/jit-cache-handoff/executor-pool-exact-token-measurement-perf-20260812/README.md`.
Its isolated `standalone-unlimited-lease` control selects the standalone
adapter path and bypasses only managed-session lease exclusion and the
live-executor ceiling; geometry, code generation, shared-cache policy, input
bytes, and measurement logic remain matched. It repeats the same P10/9-worker
and E4/3-worker shell-init/JIT probe, fixed
`pool, standalone, standalone, pool, pool, standalone, standalone, pool`
order, and three repetitions per invocation. The primary statistic remains
the median of the four invocation medians for each arm and host class:

| host class | exact-token pool wall | standalone wall | pool change | exact-token pool host CPU | standalone host CPU | pool CPU change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| performance | 22.7025 s | 22.2960 s | **+1.82%** | 167.400 s | 164.130 s | **+1.99%** |
| efficiency | 17.4315 s | 14.9345 s | **+16.72%** | 32.650 s | 25.675 s | **+27.17%** |

The corrected pool is therefore also **rejected as a performance
optimization**: it retained neither the prior P-core benefit nor the required
E-core wall result within 2%. Both invocation-median ranges overlap. P halves
are +4.25% and +0.32% wall (+3.53% and +1.04% host CPU); E halves reverse at
-7.92% and +16.93% wall while host CPU moves +0.25% and +32.16%. The host was
not idle: a long-lived `ReportCrash agent` occupied about one P core, with
variable `spotlightknowledged`, WindowServer, and application load. All samples
were retained and no guest was retried. The counterbalanced order limits simple
drift, but the overlapping ranges, split E halves, and recorded host load limit
causal precision. They do not establish a structural cause for the noisy E
result or confirm the earlier global-mask RMW hypothesis.

The pool remains required to enforce the configured guest-CPU executor cap and
the whole-process budget; this rejection is not permission to restore
unbounded wrapper-owned executors. The mechanism trace below closes the initial
acquisition-path diagnosis. It also closes sequence affinity without a code
arm: the interrupt-sequence pointer is a vCPU interrupt identity rather than a
task, address-space, or JIT-cache key, and the evidence does not connect its
lifetime slot fanout to borrow-weighted cache churn. The remaining fixed
per-entry loan cost has not been isolated.

#### Unscored executor-pool mechanism trace

The profiling-only follow-up adds `UML_JIT_PROFILE_EXECUTOR_POOL=1` to answer
the mechanism questions the rejected A/B could not. Each JIT wrapper records
its own borrow count, preference attempts/hits, fallback claims, availability-
cell probes and failed CASes, waiter enqueue/wake/FIFO handoffs, managed
construction and created/peak-live state, post-claim observed borrowed peak,
slot changes, interrupt-sequence changes and nulls, and same-sequence movement
to another slot. Each successful borrow also ORs its claimed slot into a
bounded local `{session, sequence, slot-mask}` association. A quiescent merge
keys by session and sequence, preserving distinct runs even if an address is
reused, and reports distinct/shared counts, extra wrapper associations,
wrapper fanout, multi-slot counts, extra slot associations, maximum slot fanout
and explicit overflow without printing either key. The consecutive movement
field is supplemental; it is not the sequence-to-slot stability measure. Cold
standalone lease construction and live peak are recorded under the same
private registry.

There is no global profiling atomic increment on the borrow path: a wrapper
updates ordinary fields, and only wrapper lifetime plus the existing
quiescent final takes the registry mutex. `peak-borrowed-observed=` is not a
linearizable global maximum; it is the largest post-claim scan observed by any
wrapper. An unset control passes null to the pool and skips that scan. The
source and every call compile only in the existing profiling product. That
product nevertheless contains diagnostic branches, and enabled traces add
post-claim cell scans, so every timing from it is deliberately unscored and
must not be compared with the accepted or rejected A/B measurements. The
deterministic final-report fixture drives the counter vocabulary, interleaved
and cross-wrapper slot masks, live-to-retired idempotence, and a pool-only
changed final; the existing real pool unit tests remain the FIFO and
concurrency authority.

The accepted trace is recorded at
`.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md`.
It contains one P-class invocation followed by one E-class invocation, no
retry, orderly guest shutdown, zero runner/pipeline/emulator status, two
complete 35-field pool finals, and no remaining guest or build process. The
exact mechanism totals are:

| observation | P: 10 vCPU / 9 workers | E: 4 vCPU / 3 workers |
| --- | ---: | ---: |
| wrappers / borrows | 21 / 18,002,647 | 15 / 6,002,142 |
| preferred attempts / returned remembered slot | 18,002,626 / 18,002,107 (99.9971%) | 6,002,127 / 6,002,116 (99.9998%) |
| fallback claims | 530 (0.0029% of borrows) | 22 (0.0004%) |
| cell probes / failed CAS | 18,004,098 / 1,461 (1.000081 probes/borrow) | 6,002,173 / 35 (1.000005 probes/borrow) |
| waiter enqueue / wake / FIFO handoff / maximum | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| construction attempts / created | 10 / 10 | 4 / 4 |
| peak live / peak borrowed observed | 10 / 9 | 4 / 3 |
| slot changes | 519 | 11 |
| sequence changes | 0 | 2 |
| distinct / shared / multislot sequences | 10 / 5 / 10 | 4 / 4 / 4 |
| null sequence observations | 0 | 0 |
| extra slot associations / maximum slot fanout | 64 / 9 | 8 / 4 |
| local/global association overflow | 0 / 0 | 0 / 0 |
| standalone constructions / live peak | 0 / 0 | 0 / 0 |

`preferred-hits` is the returned-remembered-slot count, not a first-CAS
success count; its shortfall from preferred attempts exactly equals the 519/11
slot changes. Direct cell probes and failed-CAS totals are the acquisition
evidence. They exclude frequent multi-cell acquisition scans, repeated CAS
failure, waiter queuing, and repeated construction as explanations for the
rejected A/B result: remembered-slot return exceeds 99.997%, probes per borrow
are effectively one, no waiter path ran, and construction was a failure-free
one-time fill to the configured CPU count. This trace does not by itself
identify what does cause the loss, and its raw timings are unscored because the
profiling build adds branches and the enabled diagnostic performs post-claim
scans. Recorded host load was also high and variable.

Sequence affinity is closed without a code arm at this checkpoint. Linux
returns `&guest_ipi_sequences[cpu]`, so the observed identity names a vCPU's
interrupt source rather than a task, address space, or executor-cache key.
Local JIT block reuse is indexed by generation and PC and matches generation,
code-change sequence, and PC; TLB and link preparation follow generation plus
the mapping and code-change sequences. The current adapter instead remembers
one executor slot per task-carrier wrapper. It returned that slot on
18,002,107 of 18,002,626 P attempts (99.997117%) and 6,002,116 of 6,002,127 E
attempts (99.999817%). The exact 519/11 shortfalls were same-sequence slot
changes, only 0.002883% of all P borrows and 0.000183% of all E borrows.

The lifetime evidence remains exact: all 10 P identities were multislot with
maximum fanout 9, and all four E identities were multislot with maximum fanout
4. The association field ORs every slot a sequence ever used, however; one
rare startup or fallback adds a slot without measuring borrow-weighted global
cross-wrapper churn or an executor-cache miss. A vCPU-to-slot map would retain
every per-entry session check, claim CAS, and release publication that
distinguishes the pool from standalone, while potentially displacing
wrapper-correlated address-space and PC locality. It therefore has neither a
mechanism attribution nor a plausible path to remove the fixed loan tax.

Reopen only if a profiling-only, zero-overflow trace measures borrow-weighted
global same-sequence slot transitions across wrappers and correlates them with
executor-local block misses or recompilation, validation failures, or link/TLB
reprepare work sufficient to explain at least a 2% host-CPU or wall-time
effect; a symbolized scheduling profile independently placing at least 2% of
carrier CPU in those post-handoff cold paths also qualifies. Lifetime fanout
alone does not. A later candidate's causal control must be the current
exact-token wrapper-affinity pool, not standalone alone. A fresh standalone
arm is additionally required before claiming recovery of the pool tax, and
the trace must be disabled for score collection.

The initial untagged pointer-cell source passed both executor-pool tests in the
ordinary 16 KiB and 4 KiB profiles, with `architecture-audit` in each build,
and passed the same 2/2 selection under ASan/UBSan and TSan. It was subsequently
rejected by the stale-check/restart audit described above. The exact-session
token and managed-lease repair pass cache geometry plus both pool fixtures in
the ordinary 16 KiB and 4 KiB profiles and under ASan/UBSan and TSan. Both
ordinary profiles also pass `architecture-audit`; focused source and runtime
tests cover test-control ownership. These are correctness and structure gates,
not performance evidence.

### Final reporting survives stdio locks and descriptor backpressure

The profiling product formats bounded records in private storage and writes
them directly to stderr's descriptor. Its writer resumes the exact unwritten
suffix after short writes or `EINTR`; on `EAGAIN`/`EWOULDBLOCK` it waits with
`poll(POLLOUT)` and then continues. It never takes or flushes stderr's shared
`FILE` lock. Long expansion reports use typed, numbered continuation records,
so concurrent producers remain reconstructible without one process-wide output
lock.

After the embedded ELF entry returns and all task carriers join, the emulator
drains retired and still-live recorder tails before destroying the executor
pool. The app repeats the same facade before its intentional `std::_Exit()`.
Compile/overlap and shared reporters retain normal-exit callbacks for other
embeddings. Pool reporting is intentionally explicit-only because its snapshot
requires that quiescent pre-drain boundary. Unchanged explicit repeats are
silent, as are later compile/overlap and shared component callbacks, while later
activity permits a new cumulative final. The focused `check-jitprof` workflow
passed 2/2. Its fixture
holds the stderr `FILE` lock, fills a nonblocking pipe to `EAGAIN`, proves the
writer waits until a reader drains it, byte-compares the complete record, and
exercises concurrent long records plus unchanged/changed final phases. The
associated architecture audit covered 56 targets, 50 layered targets, and 57
registrations with no forbidden edge or cycle.

### Accepted predecessor one-shot diagnostic artifact

The exact accepted record is
`.build/results/jit-cache-handoff/link-capacity-profile-backpressure-fix-20260812/README.md`.
It is one diagnostic run, started 2026-08-11 22:21:25 UTC and not repeated.
The benchmark pipeline and emulator returned zero. The guest reached its
overlay marker and stopped with `result=0 exitcode=0`.

| Workload/result | Observation |
| --- | ---: |
| Clang | 807; 3.97 Klines/s |
| Object Detection | 1242; 37.2 images/s |
| Multi-Core Score | 1001 |
| complete periodic compile records | 1,240 |
| complete periodic shared records | 2 |
| complete compile finals | exactly 1 |
| complete shared finals | exactly 1 |
| malformed profile records / records after shared final | 0 / 0 |

The newline-terminated compile final reports 5,131,093 blocks and 25,111,876
guest instructions. Direct/indirect branch-operand refusals are 43,477/0 and
own-target refusals are 33,728. Direct link occupancy reaches its exact
262,144-record capacity; indirect occupancy is 21,168 of 262,144. There are 27
primary-arena resets. Stale validation records 31,235,656 attempts, 31,227,083
hits, zero mismatches, zero faults, and 8,573 sequence races.

The shared final reports 161,881 entries/publications, 4,354,180 exact matches,
4,352,573 successful reuses, 5,052,816 misses, and 4,832,300 admission
rejections. It records 25,787 degraded and zero malformed rejections; all 1,607
materialization failures are explained by 1,597 direct-link-capacity failures
and 10 private-arena exhaustions, with zero indirect-capacity or invalid-metadata
failures. Retained payload is 119,438,792 bytes and accounted storage is
268,434,848 of the 268,435,456-byte cap.

A follow-up call-flow audit established that the 4,832,300 capacity rejections
in this artifact occurred only after the then-current publisher had allocated
and copied a complete transient `SharedTranslation`. The current tree performs
an exact-race and overflow-safe lower-bound capacity preflight before candidate
construction, retains `insert()` as the final admission authority, and converts
only opportunistic-publication `std::bad_alloc` to the existing private install.
Focused tests cover a full shard, the observed 608-byte global residual,
overflow, candidate and map-insertion allocation failures, private execution,
and stale validation-word survival. Both preflight and final insertion now
resolve an exact canonical owner before applying the two-live-executor gate, so
a compiler that overlapped a retiring peer can retain immutable canonical
validation ownership without admitting a new one-executor identity. Insertion
failures and the conservative accounted bytes retained after successful map
insertion followed by entry-vector failure are reported separately; the latter
is not measured heap usage. The current-source diagnostic below checks these
counter and reporting boundaries, but there is no matched post-change
throughput measurement.

The rejected pre-framing current-source diagnostic at
`.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md`
is preserved as one unscored attempt with no retry. Its statuses, workloads,
three finals, and terminal order passed, but exactly 39 unterminated shell
continuation prompts prefixed the first otherwise complete compile record on
physical line 188. Because emulator stdout and stderr already shared one PTY,
suffix parsing or outer-stream separation was rejected; the writer now places
the separating LF and profile body in the same descriptor write.

The sole post-framing current-source diagnostic is accepted at
`.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md`.
Every guest, pipeline, and emulator status is zero. The 39
prompts remain on their own line, and all 1,450 physical profile records begin
exactly with `Darwin JIT` and end in LF: 1,445 periodic compile, two periodic
shared, and one compile, pool, and shared final in post-stop order, with
nothing after shared. Direct/indirect occupancy is 262,144/262,144 and
21,469/262,144. Shared translation accounting is
268,434,608/268,435,456, with 5,131,797 matches, 5,127,800 reuses, 5,672,507
admission rejections, zero insertion failures or retained failure bytes, 3,986
direct-capacity failures, and 11 arena failures. Clang 758 and Object Detection
1257 are diagnostic and unscored because this is one run without a matched
control.

The predecessor artifact proves that the reporting boundary is complete, its
executor pool carried the real workload through orderly shutdown, capacity
refusal was observable, and matched/reused semantics were not conflated. Its
counters do not cover the later insertion fields or canonical-first admission.
They are not score evidence: the run has no matched control and was not repeated. The
global-mask executor-pool A/B is rejected on its E-core result, and the
corrected exact-session-token A/B is separately rejected after losing on both
host classes. The pool remains the required budget-correct lifetime boundary;
the supporting mechanism trace is complete and excludes frequent acquisition
scanning, CAS retry, waiter traffic, and reconstruction as the cause. Sequence
affinity is closed without code because lifetime vCPU-to-slot fanout is not
borrow-weighted cache-churn evidence and wrapper preference already misses on
only 0.002883%/0.000183% of P/E borrows. Reopening requires the correlated
borrow-weighted trace or symbolized scheduling profile defined above; the
pool's remaining per-entry loan cost has not been isolated. The shortened
sharing `ON/OFF/OFF/ON` full-Geekbench sequence produced median peak tree RSS
of 11,385,044,992 bytes ON versus 11,067,219,968 OFF: a 317,825,024-byte
(2.87%) increase, not a reduction. Its two-run-per-arm scores were visibly
unstable, so they are descriptive only; the user's significant-memory
exception does not apply and no longer-running sharing score campaign is
pending. The current 16 KiB host workflow passed 575/575 units and 47/47
tooling tests. The 4 KiB unit run passed 575/576; its sole failure was the
unrelated timing-sensitive executor-pool multislot test, which passed
immediately in an exact isolated rerun, and 4 KiB tooling passed 47/47. Every
gadget test passed in both page profiles. The Apple package workflow passes all
three slices and installed, raw, and Xcode consumer builds. Full sanitizer and
complete integration workflows remain outside this checkpoint's credited
evidence.

## G137: fixed x8/x9 cross-link carry closure

### Mechanism and correctness boundary (2026-08-12)

The default-off `UML_JIT_CARRY_X8_X9_ACROSS_LINKS` prototype gives guest x8
and x9 a fixed linked-block bank in host callee-saved x22 and x23. The enlarged
persistent-chain frame saves and restores those host registers, loads the
guest pair at the external entry, and retains it only across a successful
direct or PIC link. Ordinary exits, link-capacity fallback, helpers, faults,
PC-alignment rejection, and terminal paths publish canonical guest state;
continuing TLB helpers and constrained LS64 sequences reload the bank before
translated execution resumes. Terminal helpers cannot republish stale
pre-helper values. Tracked direct-store translations retain their existing
shape and do not enable the bank.

The carry selector is part of the complete shared-translation code-generation
profile, so OFF and ON products cannot reuse each other's code. The GPR-link
profiling build remains incompatible because its generated code embeds
executor-owned counter pointers; configuration rejects combining the two, and
that diagnostic profile source-forces translation sharing off.

Focused ordinary correctness evidence is green for 16 KiB OFF and ON and for
4 KiB OFF. Closure then used two fresh carry-ON trees. The 4 KiB Release tree
passed the focused PC-alignment, floating-trap, executor-pool, JIT-cache
validation, and chain-ABI runtime selection 5/5 plus `architecture-audit`. The
16 KiB Release ASan/UBSan tree passed the same focused selection 5/5. Together
they cover frame shape, host callee-saved restoration, direct and PIC links,
shared-translation relocation and compiler-first destruction, TLB miss
continuation, helper/fault/terminal priority, PC alignment, LS64, register
allocation, and x8/x9 memory operands. These are implementation and lifetime
checks only; they are not performance evidence.

### Predeclared matched carry campaign

The campaign below was declared before launch and then executed without
changing its order, statistic, or strict gates. It used separate Release OFF
and ON build directories with sharing explicitly ON, G107 explicitly OFF, and runtime/GPR
profiling OFF. The sensitive payload is
`guest_fixed_x8_x9_link_loop`; `guest_fixed_gpr_link_loop` is the x0/x1 entry,
exit, and link-overhead control. Both payloads self-check both members of their
pair.

Use the EROFS image directly, JIT, 1024 MiB, one worker, P cores with ten guest
CPUs, and E cores with four guest CPUs. Each invocation has four
repetitions; repetition one is warmup and the invocation sample is the median
of repetitions two through four. For each core class, use the fixed sequence
`A B C D  D C B A  B A D C  C D A B`, where A is OFF/control, B is
OFF/sensitive, C is ON/sensitive, and D is ON/control.

The predeclared microprobe threshold is at least a 2% sensitive improvement on
both core classes and at least a 2% improvement after normalizing by the
control, with nonoverlapping ranges, a beneficial sign in both counterbalanced
halves and every quartet, control within +/-2%, no normalized host-CPU
regression above 2%, and no unexplained wall/CPU disagreement. A failed or
ambiguous result keeps the option off. Even a passing microprobe does not by
itself change the production default; a representative real-workload or
Geekbench confirmation is a separate required disposition campaign.

### Matched result and two distinct dispositions (2026-08-12)

The result is recorded at
`.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`.
It retains the commands, eight one-repetition preflights, all 32 scored
invocations, every per-repetition wall/CPU value, and host/process observations.
Every guest and emulator exited cleanly and no sample was retried or deleted.

The primary values are nested medians: repetition 1 is warmup, repetitions
2--4 form an invocation median, and four invocation medians form each cell.

| class | OFF control | OFF sensitive | ON sensitive | ON control | raw sensitive | control | normalized wall | normalized CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| P10, one worker | 0.9530 s | 0.9565 s | 0.9140 s | 0.9595 s | **-4.443%** | +0.682% | **+5.091%** | **+5.614%** |
| E4, one worker | 6.2105 s | 6.0925 s | 5.9400 s | 6.0180 s | **-2.503%** | **-3.100%** | **-0.616%** | **+2.025%** |

P passes every predeclared gate: sensitive invocation-median ranges do not
overlap, all quartets and halves are beneficial, control stays within the band,
and wall/CPU agree. E fails the strict policy. Its sensitive ranges overlap,
control moves beyond +/-2%, quartet 1 is +1.396% raw and -7.489% normalized,
half 1 is -3.569% normalized, and normalized wall and CPU disagree in
direction. Therefore the original preregistered result is **P PASS, E FAIL,
overall FAIL**.

During the scored E campaign the user supplied a separate, explicitly post-hoc
cross-core tolerance: compare E regression with twice the measured P gain.
Under that policy the prototype is accepted. P's 5.091% normalized-wall gain
sets a strict 10.181% E regression ceiling, and E regresses only 0.616%. P's
4.443% raw sensitive improvement sets an 8.887% E raw-regression ceiling, and
E instead improves 2.503%. Host CPU is disclosed rather than substituted for
the primary metric: E improves 2.025%, within twice P's 5.614% CPU gain, but
its direction still conflicts with wall.

This post-hoc acceptance does not rewrite the strict failure or prove an E-core
carry-specific win. The E control confound, QoS-clamp-not-affinity topology,
wall/CPU disagreement, and user-waived unrelated host load remain limitations.
One worker isolates a single timed linked-loop workload from simultaneous
benchmark-worker contention, but it does not remove the guest topology,
executor pool, scheduler, whole-emulator CPU accounting, or ambient host work;
that is why both x8/x9 sensitive and symmetric x0/x1 control cells are required
in every arm. The production default remains **OFF**. Enabling it still needs a
representative real-workload or Geekbench disposition beyond this microprobe.

## G138: fixed 64 MiB private caches, lazy sharing, and compact link footers

Recorded 2026-08-12 against the current working tree. This investigation used
the known cache-sensitive Geekbench 6.7.1 Clang (201) and Object Detection
(301) workloads at 8 GiB, ten vCPUs, JIT, and P QoS. Each score below is one
complete observation under the user's accepted background-load boundary; it is
mechanism evidence rather than a multi-run benchmark campaign.

The production geometry now caps the complete retained private working set at
64 MiB per executor. `--jit-executor-cache-budget=` may lower but not raise
that cap. Once private caches are funded, only the global shared-cache budget
grows with guest memory. Shared shard/map backing is created on first insertion
and remains process-byte-bounded. Canonical entries contain zero in every
executor-pointer relocation cell and retain only neutral guest keys, emitted
code, metadata, and immutable validation words. Mutable activation state is
regenerated per executor and is not cached globally.

The initial 64 MiB observation scored 460 Clang, 856 Object Detection, and 628
combined. A 128 MiB diagnostic arm scored 532/669/597, so simply enlarging the
private tier was not a consistent answer; the requested 48 MiB arm was stopped
as a decision route. A one-executor diagnostic still exhibited cache/arena
churn, while the accepted executor-pool trace returned the remembered slot on
99.997117% of P attempts. The regression therefore did not require executor
migration or failed cross-executor reuse.

The more important fault was direct-link storage. The predecessor fixed table
filled and produced 63,941,694 direct-capacity materialization failures. A
first attempt moved link state beside code but retained ten activation pointers
in each 128-byte footer; it scored 332/975/569 and, in the profiling run,
caused 16,938 arena resets and 489,657,430 shared materializations. The retained
layout stores one 64-byte activation anchor after the first code block that
names each target PC, replaces ten pointers with one internal entry and a
ten-bit capability mask, and makes every later source reuse that anchor through
a fixed reverse index. Code and anchors are allocated and reset as one arena
resource.

The compact 64 MiB ordinary observation scored **542/1139/786**, improvements
of **17.83%/33.06%/25.16%** over the initial 64 MiB observation. Its profiling
run recorded 3,869 arena resets and 183,428,416 shared materializations, down
77.16% and 62.54% from the first footer, with zero direct-capacity failures.
The complete successful local compile/publication/install path averaged
16,322.84 ns; the complete shared lookup/materialization/private-footer/install
path averaged 2,424.81 ns. Shared reuse is therefore **6.73x faster** and uses
85.14% less time than recompilation, rejecting the hypothesis that relocation
and footer regeneration are slower than compiling again.

That compact observation still created one mutable record per source rather
than per target. Removing the reset-time scan reduced the representative
`arm64-base-control-compare-test` runtime from 688 to 171 seconds; making every
source for the same target reuse one arena-owned anchor reduced it again to
6.66 seconds. The final ordinary observation scored **613/1238/871**, another
**+13.10%/+8.69%/+10.81%** over 542/1139/786 and
**+33.26%/+44.63%/+38.69%** over the initial 64 MiB result.

A final live emulator sample was taken before shutdown. Resident memory peaked
at 6.932 GiB. Activation/deactivation was absent from its collapsed top-stack
list and only 48 recursive samples entered constant-time `activate_link`;
lookup, shared validation, generated execution, and code publication remained
visible. The earlier instrumented sample reported 2.8 GiB physical footprint.
Its final shared report contained 2,047,701 entries and 3,285,847,540 accounted
bytes against a
3,624,031,696-byte shared budget. Thus the large footprint was not caused by an
eager allocation of the nominal global limit: the workload populated most of
the lazy shared tier. Executor-local activation bytes are absent from the
canonical shared representation and are now cheap enough that the sample does
not support adding shared mutable state or an extra hot-path indirection.

Disposition at this checkpoint: retain the compact link representation,
growing shared remainder, lazy shards, neutral shared relocation cells, and
unique arena-adjacent direct target anchors. The 2026-08-13 G139 sweep below
confirms that production remains at the 64 MiB ceiling. Do not restore the 128 MiB
private diagnostic arm, per-source mutable copies, or the 128-byte footer.
Moving the remaining unique activation fields out of executable pages is
closed unless a new profile identifies them as material; the current sample
does not justify the added indirection.

## G139: private-cache floor sweep

Recorded 2026-08-13 with the same ordinary JIT product and the G138 workload:
Geekbench 6.7.1 Clang (201) plus Object Detection (301), 8 GiB guest, ten
vCPUs, and P QoS. Runs were sequential, unprofiled, and completed with orderly
guest and emulator shutdown. Time Machine, ReportCrash, and other unrelated
host load remained a disclosed limitation under the user's accepted
under-ten-core background-load boundary.

| private ceiling | Clang | Object Detection | combined | disposition |
| ---: | ---: | ---: | ---: | --- |
| 64 MiB | 613, 613 | 1238, 1247 | 871, 874 | reference |
| 62 MiB | 626, 615 | 1267, 1244 | 891, 875 | accepted |
| 60 MiB | 598 | 1126 | 821 | rejected |
| 56 MiB | 581 | 1293 | 867 | rejected |

The two-observation medians are 613/1242.5/872.5 at 64 MiB and
620.5/1255.5/883 at 62 MiB. The smaller tier changes those medians by
**+1.22%/+1.05%/+1.20%** and neither individual observation sacrifices a score
beyond the established 2.8% focused band. By contrast, 60 MiB regresses Object
Detection 9.70% and combined 6.06% against the fresh reference; 56 MiB
regresses Clang 5.22%. The non-monotonic constituent scores also show why a
single aggregate cannot establish the floor.

Disposition: keep the production private ceiling at **64 MiB per executor** as
directed by the user. Although 62 MiB is the smallest measured no-sacrifice
point, it moves only up to 20 MiB across ten executors, preserves the same
whole-process budget, and has only two observations under disclosed host load.
Do not lower the default to 60 MiB or infer a total-RSS reduction from this
capacity shift.

## G140: bounded gadget SIMD destination residency

Recorded 2026-08-13 after the G107 successor-fetch default was accepted. This
implements the smallest G130 route rather than a fixed whole register bank:
four or more consecutive nonfaulting integer vector MLA instructions with one
v0-v7 destination and sources that do not alias it load that destination into
q24, operate there, and flush it before the next unaware instruction or the
translated-block boundary. The run cannot cross a helper, terminal, fault,
interrupt, or link. Established gadget entries keep their positions because
the new load/MLA/flush entries are appended to the bank.

The focused 32-MLA sensitive loop and symmetric out-of-bank control used three
repetitions per arm:

| class | OFF control | OFF sensitive | ON sensitive | ON control | sensitive change | control change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P10 | 2.292 s | 2.291 s | 0.696 s | 2.317 s | **+69.6%** | -1.1% |
| E4 | 5.129 s | 5.495 s | 3.103 s | 5.421 s | **+43.5%** | -5.7% |

The targeted single-core SIMD set (Object Detection, Blur, Horizon, HDR,
Photo Filter, Ray Tracer, and Structure from Motion) changed aggregate score
91 to 92. Against the already-accepted G107 predecessor, the clean full
Geekbench run changed Single-Core **97 to 96** and Multi-Core **567 to 575**;
against the original pre-G107 baseline it is 94/564 to 96/575. No aggregate
regression is obvious.

One first full attempt was stopped rather than scored after Multi-Core Text
Processing emitted repeated RCU-stall reports. That attempt had already
completed six other multi-core workloads. The isolated Text Processing rerun
completed at 62 and the sole clean full rerun completed it at 63, exactly the
predecessor score, so the stall did not reproduce as a candidate semantic
failure. Both the stopped log and clean result remain under
`.build/results/gadgets-optimization-20260813/`.

Disposition: enable only this bounded family by default. The result does not
justify dirty vector state across a block/link/helper boundary or a 729-form
whole-bank cross-product. Exact stream-shape, threshold, alias/out-of-bank
fallback, pre-link flush, retirement, and next-instruction visibility remain
focused correctness contracts.

## G141: exact stale gadget-program validation

Recorded 2026-08-13 after G140. A same-generation code-change notification
formerly discarded every decoded gadget program even when the affected guest
code was unrelated or an instruction-cache maintenance operation left the
words unchanged. G141 first resets all direct and indirect link activations,
retains the program slots, and validates every guest word consumed by a stale
program at page-bounded fetch boundaries. An exact match reactivates the local
program. A mismatch or fetch fault discards it and decodes normally; a changed
guest generation still invalidates the entire cache. Validation storage is lazy
and grows only when a populated slot needs a longer buffer. This replaced the
first prototype's fixed 64-word slot member, which could add 4 MiB per fully
populated executor.

The focused unchanged-code IC-IVAU sensitive probe and DC-CVAU boundary control
used three repetitions per arm:

| class | OFF control | OFF sensitive | ON sensitive | ON control | sensitive change | control change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P10 | 0.047 s | 1.928 s | 0.724 s | 0.046 s | **+62.4%** | +2.1% |
| E4 | 0.147 s | 7.522 s | 3.715 s | 0.153 s | **+50.6%** | -4.1% |

The compact candidate changed targeted multi-core Clang/Object Detection from
394/461 to 383/464, or combined 426 to 422 (-0.9%). The superseded fixed-array
prototype scored full Single-Core/Multi-Core 95/564. After lazy validation storage,
the final full run scored **96/567**, versus 96/575 for the G140 predecessor and
94/564 for the original pre-G107 baseline. The final step is therefore flat on
Single-Core, -1.4% on Multi-Core relative to its immediate predecessor, and
+2.1%/+0.5% across the three accepted gadget changes. No aggregate regression
is obvious, and the exact-validation selector is enabled by default.

## G142: shared decoded gadget templates closed before a code arm

A live Clang/Object Detection sample of the final G141 product covered 5,607
time points with ten configured vCPUs. `Builder::build` was the collapsed top
frame 1,324 times, or **2.36% of the ten-vCPU capacity over that interval**.
Because unused vCPU capacity and sleeping task carriers are included, this is a
capacity observation rather than a precise share of consumed CPU. Executable
gadget bodies are already shared process text. The remaining per-block stream
embeds executor-local direct and indirect
link-cell pointers, so an immutable global template would still need a private
copy, relocation pass, link-cell regeneration, lookup synchronization, and
lifetime/session policy. Those costs cannot be removed by sharing the decoded
words and would consume most or all of the measured upper bound. G141 already
removes repeated decode after unchanged code invalidation without adding global
hot-path state. The template arm is therefore closed without implementation;
reopen only if a representative profile places materially more than 2% of
consumed carrier CPU in decode after G141 or a design can avoid the private copy and
relocation while preserving executor-local link ownership.

## G143: focused gadget-engine time breakdown

Recorded 2026-08-13 on the final G107/G140/G141 product with ten guest vCPUs,
8 GiB, performance QoS, and the gadgets engine. macOS `sample` covered the
active portions of Geekbench 6.7.1 Clang (201) and Object Detection
(301) in both core modes. The raw reports, exact commands, classifier, and
complete symbol table are retained under
`.build/results/gadgets-time-profile-20260813/`.

The table reports mutually exclusive top-of-stack leaves inside `uml-darwin`.
Gadget execution is the sum of memory, control, integer, FP/SIMD, and other
decoded assembly gadgets. Cache activation/link work includes cache
store/activate/deactivate, link-map creation, and cache preparation; it is not
a hot cache lookup.

| mode / workload | new decode | hot cache lookup | activation + links | gadget execution | memory gadgets | control | integer | FP/SIMD | MMU + engine |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single Clang | 8.73% | 1.22% | 16.13% | **71.52%** | 28.51% | 15.17% | 24.23% | 3.29% | 2.34% |
| multi Clang | 7.01% | 1.74% | 16.15% | **72.48%** | 29.61% | 16.80% | 25.56% | 0.08% | 2.62% |
| single Object Detection | 0.00% | 0.00% | 0.00% | **99.71%** | 36.05% | 11.60% | 14.24% | 37.83% | 0.29% |
| multi Object Detection | 0.00% | 0.51% | 0.00% | **99.07%** | 40.47% | 20.66% | 23.44% | 14.50% | 0.42% |

Clang gives the stable mechanism result: single and multi agree that loading a
hot cached program costs only 1--2%, while new decode costs 7--9% and cache
admission/eviction/link maintenance costs about 16%. Direct `LinkRecord` map
insertion alone is 6.23% single and 5.36% multi; activation plus deactivation
adds 7.11% and 7.19%. The next cache-side candidate should therefore target
link-record/cache churn, not introduce a global decoded-template lookup.

The sampled Object Detection windows are already hot and spend more than 99%
of active in-product leaves inside gadget assembly. Its likely opportunities
are the actual load/store and SIMD/dot-product gadget bodies. Host C++ MMU and
engine scaffolding remain below 0.7%; guest memory work is not absent, but is
already represented by the 28.5--40.5% assembly load/store share.

This is diagnostic profiling, not score evidence. `sample` suspends threads
and visibly reduced multi-core utilization while collecting stacks. Filtering
to active in-product leaves omits sleeping carriers, and inlined work is
charged to its visible caller. A zero means below the window's resolution, not
that startup translation never happened. The two five-second Object Detection
windows also cover different internal model phases, so their FP/SIMD mix is
not a core-scaling conclusion; the repeated Clang distribution is the stronger
signal.

## G144: first post-profile gadget candidates rejected

Recorded 2026-08-13. Ten default-off candidates were implemented and removed
after the focused-to-representative screen; none reached an E-core or full-suite
run. Raw focused and Geekbench logs remain under `.build/results/`.

- An entry-local last-page translation cache moved the GPR, SP, and SIMD
  focused probes by -0.38%, +0.60%, and +0.91%. The six worst-gap single-core
  Geekbench workloads changed aggregate score 74 to 75, with mixed constituent
  movement. This did not clear the greater-than-2% P threshold.
- Bounded integer SDOT destination residency first kept one accumulator, then
  up to eight independent accumulators in q24--q31. The final sensitive probe
  improved 1.009 s to 0.749 s (**25.8%**) while its symmetric control moved
  about 1%. Real Object Detection stayed 80 to 80 single-core and changed 469
  to 463 multi-core. The benchmark-only topology was rejected.
- Two-to-four monotonically increasing 64-bit scalar loads or stores shared one
  proven same-page translation and skipped intermediate gadget dispatch. GPR
  and SP probes both improved **34.9%** (2.317 to 1.508 s and 2.347 to
  1.529 s). The profiled Geekbench subset instead changed HTML5 Browser 75 to
  72, PDF Renderer 88 to 88, Text Processing 50 to 50, and aggregate 69 to 68.
  The candidate was rejected before a full run.
- Prepacking the complete `CpuState` vector-lane byte offset for single-register
  structure transfers reduced the dedicated lane load/store probe from 0.559
  to 0.551 s, only **1.43%**. It stopped before Geekbench because even the
  all-lane-transfer loop did not clear the P threshold.
- Replacing direct-link `unordered_map` allocation with lazy four-way stable
  slabs kept the direct-link control within the noise band (2.204 to 2.231 s)
  and improved the code-invalidation control by 1.6% (0.740 to 0.728 s).
  Targeted Clang nevertheless fell from 52 to 49 single-core (**-5.8%**;
  255.2 to 243.0 lines/s). The arm stopped before multi-core, E-core, or the
  full suite. Sparse-slab hashing and probing did not repay the measured cold
  insertion cost in the representative workload.
- Full-width vector FADD/FMUL/FDIV/FSUB gadgets briefly skipped the redundant
  `movi` that precedes an operation overwriting all 128 destination bits. The
  all-FADD probe changed only 2.564 to 2.562 s (**+0.08%**), so this hot-body
  reduction stopped before Geekbench and was removed.
- A bounded four-or-more-instruction vector FADD/FMUL/FSUB run retained one
  v0--v7 destination in q24 and activated the FP environment once. The
  repeated-FADD probe improved 2.315 to 0.702 s (**69.7%**) while its
  out-of-bank control changed 2.308 to 2.320 s. Targeted real workloads did
  not transfer the gain: Asset Compression changed 87 to 86, Ray Tracer 95 to
  96, and their single-core aggregate 90 to 89. The candidate was removed
  before a full-suite or E-core run.
- A profiling-only two-cell PIC audit counted 898,479,113 RET events across
  HTML5, PDF, and Clang: 46.80% first-cell hits, 30.93% second-cell hits, and
  22.27% misses. A return-only four-way arm therefore preserved the first two
  probes and checked cells three/four only after a former miss. Its four-target
  loop improved 2.435 to 2.059 s (**15.4%**) while the two-target control
  changed 1.313 to 1.333 s, about **16.7% normalized**. Real work stayed below
  the gate: HTML5 73 to 74, PDF 87 to 88, Clang 45 to 45, and aggregate 66 to
  66. The extra cells and comparisons were removed before a full or E run.
- The 64-bit logical-MOV alias then used the accepted successor-fetch shape
  without changing its 48-byte entry or later gadget phases. Its eight-target
  probe improved 1.882 to 1.608 s while the low-polymorphism control improved
  1.799 to 1.720 s (**10.64% normalized**). The representative path did not
  transfer: HTML5 stayed 72, PDF stayed 87, Text Processing fell 49 to 47, and
  their aggregate stayed 67. The arm was removed before full-suite or E-core
  measurement.
- The vector `FMLA .4S by element` stream then stored its three already-known
  `CpuState` offsets directly, removing `AND + UBFX + LSR` from the hot gadget
  at the cost of 16 more stream bytes. The sensitive probe regressed 2.571 to
  2.662 s (**-3.54%**) while its `.2S` control moved 2.570 to 2.567 s; the
  normalized result was **-3.66%**. The larger threaded stream was rejected at
  the probe gate, before Geekbench or E-core measurement.

These failures sharpen the profiling rule: a large loop-shaped probe gain is
not enough when real instruction adjacency, register aliases, and page
boundaries rarely admit the fast path. Subsequent arms need either a measured
coverage counter or a target whose optimization applies to the already-hot
individual gadget body, followed by the same targeted Geekbench gate.

## G145: common iOS-style memory and vCPU architecture requalification

Recorded 2026-08-14 after macOS adopted the same constrained address-space
shape and one-persistent-carrier-per-vCPU model used by the iOS products. The
current Release 16 KiB gadgets product ran Geekbench 6.7.1 directly
from the tmpfs OverlayFS shell: PID 1 was `/bin/sh`, systemd and its services
were never started, the guest had ten vCPUs and 8 GiB, and Geekbench used
`--no-upload`. The complete run shut down cleanly and scored **96 single-core /
565 multi-core**, versus the final pre-change G141 run at **96 / 567**. The
single-core result is identical and the multi-core difference is **-0.35%**.
The largest constituent movements have mixed signs—single-core Background
Blur -5.08%, multi-core File Compression -4.53%, and multi-core Photo Filter
+7.16%—rather than the uniform loss expected from a new carrier or memory
boundary. The former run booted systemd and stopped services before the
workload, so this is a requalification comparison rather than a strict binary
A/B; the direct-shell run is the current low-noise benchmark authority.

Separate intrusive, unscored `sample` captures covered single- and multi-core
Clang and Object Detection under the same direct-shell boot. The per-entry
carrier boundary reported zero stack-switch time. Activation plus synchronization
accounted for less than 1.2% of the corresponding accumulated guest plus
syscall time in all four captures, and neither `darwin_cpu_relax` nor
`darwin_ipi_poll` appeared as a material collapsed leaf. Clang retained the
pre-change shape: direct `LinkRecord` insertion was about 6% of active
single-core product samples and 5.76% multi-core, while activate/deactivate was
6.71%/7.07%. Object Detection remained assembly-bound: indexed SDOT occupied
32.32% single-core and 28.68% multi-core, and the leading 128-bit vector load
occupied 15.94%/14.68%. These are the same existing gadget costs identified by
G143, not overhead introduced by fixed carriers or the shared Apple memory
architecture.

Two new individual SDOT hot-body arms stopped at the focused probe gate. First,
pair-loading the first two operand-stream words removed one host instruction
from every native and portable dot-product gadget, but the self-checking
indexed-SDOT median remained exactly **2.298 seconds** with unchanged host CPU.
Second, packing all three `CpuState` offsets into one stream word reduced each
translation by two words and two stream loads; it moved **2.298 to 2.289
seconds (+0.39%)**, again without CPU improvement. Both were removed before
targeted Geekbench. G144's independently rejected SDOT residency arm is not
reopened: its 25.8% synthetic gain already failed to move real single-core
Object Detection and regressed its multi-core score.

Disposition: retain the common macOS/iOS address and persistent-carrier
architecture unchanged. The aggregate result is already at the former gadgets
level, measured carrier overhead is below the optimization threshold, and the
remaining large leaves are pre-existing gadget bodies whose first bounded
candidates have failed the focused-to-representative rule. No macOS-only
threading, address-space, or memory shortcut is justified. Raw console and
sample reports are under
`.build/results/gadgets-ios-architecture-20260814/`; the production emulator
was rebuilt from the accepted source after both rejected arms.

## G146: 4 KiB high-address Geekbench requalification

Recorded 2026-08-14 from source revision
`7a1640e07f497e8e0181dc163d251f011f5c4404`. The canonical
`macos-integration-4k` configuration rebuilt Linux 6.18.41 with
`CONFIG_PAGE_SHIFT=12` and rebuilt the gadgets product with a 4 KiB guest-page
contract. The emulator SHA-256 was
`0fe958889342e1c2ce71b103d6d4ab14a17dcec7d2e467d8244eaf2a1fd45361` and
the embedded 4 KiB `vmlinux` SHA-256 was
`483acc8d2fad30728fd0e283e5cbbce91f5e8c3cb675dea4e026a8b3fed7eb40`.
Production inputs matched the named revision; evolving test-only fixture edits
in the worktree were not linked into that product. The run used ten guest
vCPUs, 8 GiB, the local Geekbench 6.7.1 payload with `--no-upload`, and
the direct tmpfs OverlayFS PID-1 shell; systemd and desktop services were never
started. The host was on AC power and the clean scored process was not sampled,
profiled, or inspected.

The complete run finished in **53m21s**, emitted zero for the benchmark command
and emulator exit statuses, powered off cleanly, and scored **92 single-core /
417 multi-core**. G145's current 16 KiB run used the same guest size, vCPU
count, engine, benchmark payload, and no-systemd flow. Its 96/565 result is the
current page-profile comparison:

| Result | Current 4 KiB | Current 16 KiB | 4 KiB delta |
| --- | ---: | ---: | ---: |
| Single-core | 92 | 96 | -4.17% |
| Single-core integer | 82 | 85 | -3.53% |
| Single-core floating point | 114 | 119 | -4.20% |
| Multi-core | 417 | 565 | -26.19% |
| Multi-core integer | 350 | 487 | -28.13% |
| Multi-core floating point | 579 | 744 | -22.18% |
| Elapsed wall time | 53m21s | 48m48s | +9.32% |

The multi-core movement is not uniform host throttling: Ray Tracer is 937
versus 948 (-1.16%), while File Compression is 362 versus 590 (-38.64%),
Navigation is 379 versus 1198 (-68.36%), and Photo Filter is 277 versus 509
(-45.58%). This shape is consistent with the 4 KiB compatibility build's
required private 4 KiB shadows on a 16 KiB Darwin host, quarter-page stack
reconciliation, and shadow copy/flush boundaries, but one observation per
current profile does not prove that mechanism caused every score difference.

Against the historical G35 4 KiB result at 79/416, the current result is
**+16.46% single-core and +0.24% multi-core**, and elapsed time improved from
3,302 to 3,201 seconds (-3.06%). That comparison is historical rather than a
strict A/B because it crosses later source, rootfs, and launcher changes and
replaces the old upload flow with `--no-upload`.

Address placement was checked in a separate deliberately unscored run because
`vmmap` suspends the target. Its live ranges began executable kernel mappings
at `0x600000000000` (96 TiB) and mapped the 8 GiB physical backing at
`0x600001e08000-0x600200000000`. Thus both the kernel and guest backing used
the current high native window rather than the obsolete placement directly
above 64 GiB. The untouched scored log independently reports a Normal-zone
ceiling of `0x00006001ffffffff`.

The rebuild also exposed two stale validation assumptions rather than product
failures. The embedded-loader fixture still reserved the 48 GiB ELF link
address instead of the relocated runtime address, and the guest smoke oracle
knew only the original crypto/atomics HWCAP subset even though later accepted
runtime changes advertise additional host-gated features. The loader fixture
now derives and reserves the active native window, while the guest smokes check
the complete current `AT_HWCAP`/`AT_HWCAP2` contract and keep the kernel's
separate `/proc/cpuinfo` subset explicit. The first canonical workflow attempt
is rejected because all three engine smokes stopped at that stale oracle before
their workloads. After correction, `check-integration-4k` passed all 53 tooling
checks and all five fast guest integrations in one uninterrupted workflow.
The same five fast guest cases passed at 16 KiB, and
`check-loader-asan-ubsan` passed its focused 1/1 loader gate.

Raw evidence is retained under
`.build/results/gadgets-ios-architecture-20260814/`. The scored console is
`current-full-4k-nosystemd-clean.log`, SHA-256
`78bc6888aa30524eef064d800d7bbba8037fde31cadb1adee5f858af18182598`;
the intrusive address record is `current-full-4k-high-va.vmmap.txt`, SHA-256
`7608fcc8cc484f49c8fbf9737c7a121ec3c8c959abf9386a8f73dbb57d8888ac`.
The benchmarked implementation predates the later test and documentation-only
commit that records this evidence.

## G147: physical-iPad completed-workload profile

Recorded 2026-08-14 on the connected iPad mini (6th generation), iPadOS 18.5,
from the Release Kernarium app with a standard development signature and no
debugger. The app used the canonical 16 KiB kernel, gadgets engine, 1 GiB of
guest memory, all six physical CPU cores as vCPUs, the existing EROFS image,
and its recreated 8 GiB ext4 OverlayFS disk. The opt-in
`KERNARIUM_GEEKBENCH=1` bootstrap kept PID 1 as `/bin/sh`, mounted only the
filesystems required by the local Geekbench 6.7.1 payload, and invoked
it with `--no-upload`; systemd and the desktop were never started.

The run was stopped during Photo Filter at the user's direction because that
unfinished phase was dominated by 1 GiB guest-memory pressure, which is not an
optimization target. The forwarded interrupt ended the app with status 130
and no core dump. Thirteen preceding single-core workloads had completed, but
Photo Filter produced no score and the multi-core phase never began, so this
campaign has **no aggregate Geekbench score**. The three Photo Filter captures
(`009`, `010`, and `011`) are excluded from every result below. The completed
workload scores were File Compression 108, Navigation 181, HTML5 Browser 75,
PDF Renderer 95, Photo Library 110, Clang 55, Text Processing 49, Asset
Compression 80, Object Detection 99, Background Blur 90, Horizon Detection
55, Object Remover 125, and HDR 79.

Eight physical-device Time Profiler windows cover only workloads that finished.
Percentages are mutually exclusive top-of-stack weights over active app
samples; the small unlisted remainder is system-library or unclassified work.

| Trace / completed workload | Active carrier CPU | gadget assembly | emulator C++ | native guest kernel | non-carrier CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| 001 File Compression | 10.109 s | 98.89% | 0.39% | 0.03% | 0.030% |
| 002 HTML5 Browser | 10.093 s | 91.42% | 7.00% | 0.14% | 0.010% |
| 003 Clang | 15.102 s | 78.69% | 19.61% | 0.04% | 0.026% |
| 004 Clang, later window | 15.102 s | 77.70% | 20.49% | <0.01% | 0.026% |
| 005 Text Processing | 15.108 s | 95.54% | 3.47% | 0.04% | 0.033% |
| 006 Asset Compression | 10.108 s | 98.60% | 0.32% | <0.01% | 0.010% |
| 007 Object Detection | 10.397 s | 73.93% | 0.55% | 23.03% | 0.019% |
| 008 Object Remover | 10.094 s | 98.58% | 0.39% | 0.01% | 0.040% |

More than 99.6% of active samples in each retained window were scheduled on
the A15 performance cores. The first and last retained traces both report a
Nominal thermal state. Non-carrier CPU never exceeded 0.040%, so UIKit, Metal,
input, networking, app lifecycle, and host carrier scheduling are excluded as
material explanations for the measured execution rate. This agrees with
G145's macOS proof that the common persistent-carrier architecture is not the
performance problem.

The finished captures rank the next investigations as follows:

1. **Code-change link reset.** HTML5 charges 4.94% directly to
   `GadgetExecutor::prepare_cache`. On every same-generation code change,
   `ProgramCache::invalidate_links()` currently scans and resets all direct
   and indirect records accumulated by that executor. V8 makes this visible.
   First instrument change frequency and active-versus-allocated populations;
   then test a bounded active-record index that resets every live activation
   without scanning historical inactive records. G141's exact requirement to
   clear all active direct and indirect links before validation remains
   non-negotiable, and no branch-gadget hot-path check should be added.
2. **Clang cache/link churn.** The independent Clang windows repeat the same
   19.61%/20.49% C++ share. Direct `LinkRecord` unordered-map insertion alone
   is 4.05%/3.96%; `Builder::build`, activate/deactivate, decode, cache lookup,
   and block entry form the remainder. This reproduces G143 on physical iOS
   and is not a new platform cost. Instrument record creation, activation, and
   reuse before choosing another stable-address container; G144's four-way
   slab replacement already regressed representative Clang 52 to 49 and stays
   rejected.
3. **Scalar register-offset address formation.** File Compression is 98.89%
   gadget assembly, and `scalar_other_load32u_x_1_r` alone is 12.47%. The
   current gadget performs extension and optional scaling separately before
   ADD. A low-risk focused arm can fold those operations into AArch64's
   extended/shifted-register ADD forms, but it must first clear a dedicated
   shifted-register-offset probe and then a matched File Compression A/B.
4. **Profile-keyed untagged vector-pair gadgets.** `vector_pair_load128_0`
   contributes 10.5% of Object Detection and 7.3% of Object Remover. Its
   ordinary path loads and branches on `CpuState::mte_enabled` for every pair.
   Production neither advertises nor enables MTE, while unit coverage does
   exercise it. A code-generation-profile-keyed MTE-disabled symbol can avoid
   that per-pair check without deleting the checked MTE path or weakening the
   shared-cache profile key. The likely two-instruction saving still needs a
   focused probe because its end-to-end upper bound may be below 2%.
5. **Cold EROFS decompression is a separate artifact question.** Object
   Detection's 23.03% native guest-kernel share resolves principally to
   `HUF_decompress4X1_usingDTable_internal` (14.45%) and
   `FSE_decompress_wksp_bmi2` (4.39%). About 19.3% of that window is therefore
   zstd demand decompression rather than emulator translation. A warm repeat
   or matched LZ4/uncompressed-image experiment can quantify the storage/start
   tradeoff; it is not evidence for changing the emulator CPU, MMU, carrier,
   or iOS application layers.

Text Processing, Asset Compression, and Object Remover are broad
assembly-bound mixes rather than single host bottlenecks. Indexed half-precision
FMLA is visible in Object Detection, but A15 lacks AFP and an unchecked arm
must preserve dynamic FPCR.AH semantics; G144 also showed that a large generic
FP run-fusion probe did not transfer to Asset Compression. It ranks below the
four measured candidates above.

Disposition: accept the profile as a diagnosis only. No product hot-path
change is authorized by an intrusive, unmatched trace. Each candidate must
pass a focused correctness/performance probe and matched representative A/B
under the existing greater-than-2% prospective gate. Raw traces, XML exports,
device and launch records, exact commands, score boundary, and the complete
analysis are retained under
`.build/results/kernarium-ipad-geekbench-20260814/`.

## G148: four-vCPU multi-core-only page-profile comparison

Recorded 2026-08-14 with the already-qualified Release gadgets products from
G145 and G146. The 16 KiB emulator SHA-256 was
`770e2f023c16c70be4ca023eb50cce129702c5f54a5ffb79282bbdff5e9801dd`
and its `vmlinux` SHA-256 was
`aa2fa9d0d9fcfdb3f9816d41e80fd047725ea16c391cfa3a315d3a838c466aae`.
The 4 KiB emulator SHA-256 was
`0fe958889342e1c2ce71b103d6d4ab14a17dcec7d2e467d8244eaf2a1fd45361`
and its `vmlinux` SHA-256 was
`483acc8d2fad30728fd0e283e5cbbce91f5e8c3cb675dea4e026a8b3fed7eb40`.
Their kernel configurations reported `CONFIG_PAGE_SHIFT=14` and 12,
respectively, and both guests reported Linux 6.18.41-gd1d324b1c263.

Both runs used four guest vCPUs, 8 GiB, the gadgets engine, the unified EROFS
image, and the direct tmpfs OverlayFS PID-1 shell. Systemd and the desktop were
not started. The exact guest command was:

```sh
/opt/geekbench/release/geekbench6 \
  --multi-core --no-upload
```

A separate
unscored five-second probe entered `Multi-Core` directly and ended only when
the outer timeout returned 124. Both scored runs then reported `1 Processor,
4 Cores`, emitted a `Multi-Core` section with all 16 workloads, emitted no
`Single-Core` section, returned benchmark status zero, and powered the
emulator off with result and exit code zero. This supersedes only the current
availability assumption; the older campaigns' recorded behavior
remains historical.

| Result | 16 KiB | 4 KiB | 4 KiB delta |
| --- | ---: | ---: | ---: |
| Multi-core | 309 | 269 | -12.94% |
| Integer | 263 | 226 | -14.07% |
| Floating point | 415 | 373 | -10.12% |
| Elapsed wall time | 25m42.47s | 30m12.94s | +17.53% |
| Maximum resident set | 5,682,151,424 B | 5,702,057,984 B | +0.35% |

| Multi-core workload | 16 KiB | 4 KiB | 4 KiB delta |
| --- | ---: | ---: | ---: |
| File Compression | 342 | 271 | -20.76% |
| Navigation | 639 | 492 | -23.00% |
| HTML5 Browser | 274 | 257 | -6.20% |
| PDF Renderer | 263 | 170 | -35.36% |
| Photo Library | 434 | 390 | -10.14% |
| Clang | 167 | 156 | -6.59% |
| Text Processing | 61 | 57 | -6.56% |
| Asset Compression | 329 | 313 | -4.86% |
| Object Detection | 261 | 239 | -8.43% |
| Background Blur | 406 | 401 | -1.23% |
| Horizon Detection | 437 | 395 | -9.61% |
| Object Remover | 406 | 369 | -9.11% |
| HDR | 373 | 346 | -7.24% |
| Photo Filter | 550 | 376 | -31.64% |
| Ray Tracer | 391 | 385 | -1.53% |
| Structure from Motion | 366 | 346 | -5.46% |

The 4 KiB loss is workload-dependent rather than uniform. Background Blur and
Ray Tracer remain within 1.6%, while PDF Renderer and Photo Filter lose more
than 31%. The 4 KiB process also used 88.84 host-system CPU seconds versus
30.49 seconds at 16 KiB and reported 969,789 versus 143,455 voluntary and
11,196,831 versus 2,717,230 involuntary context switches. Those counters are
consistent with materially more host boundary activity, but they include
scheduling effects and do not identify its cause. Peak resident size was
effectively unchanged, and neither guest swapped.

This four-vCPU aggregate gap is narrower than G146's 26.19% multi-core gap at
ten vCPUs. That is consistent with the 4 KiB compatibility cost becoming more
visible with concurrency, but it is not a topology-scaling proof: G146 ran the
complete suite, these runs began directly at multi-core, and each point has
only one observation. The order was 16 KiB then 4 KiB, not randomized. The
host remained on AC power, neither scored process was sampled or suspended,
and read-only process checks showed about 398% and 396% emulator CPU. The host
was also in active interactive use; a 4 KiB snapshot included transient
CMake/Ninja work and other desktop CPU load. Treat score percentages as the
requested current directional comparison, not a low-noise causal A/B.

The 16 KiB console is
`.build/results/gadgets-ios-architecture-20260814/multicore-4vcpu-16k.log`,
SHA-256
`81b26fb4defc82b2cdf83b20781857f08bb113ba114763500aac1bcc79f9622c`.
The 4 KiB console is
`.build/results/gadgets-ios-architecture-20260814/multicore-4vcpu-4k.log`,
SHA-256
`9191b32bd875382f04d4fe464a2eae86f88494d3f45efdbe1f93d33e7cf90cd2`.
No product change is accepted or proposed from this measurement alone.

## G149: physical-iPad slow-workload confirmation

Recorded 2026-08-14 on the same iPad mini (6th generation), iPadOS 18.5, as
G147. The standard-signed Release app used the canonical 16 KiB kernel,
gadgets engine, 1 GiB, six vCPUs, the preserved EROFS image, and a fresh 8 GiB
ext4 OverlayFS disk. Systemd and the desktop were not started. The product was
built from base `ece2bd839694e5bf337297131aa75049df17c2a7` plus a validated
`KERNARIUM_GEEKBENCH_WORKLOADS` selector. The app executable SHA-256 was
`d7b4c92da30a4a20838ac4290c71fd1a111f193d00976bf8f449367da0c18ece`;
the canonical kernel remained
`aa2fa9d0d9fcfdb3f9816d41e80fd047725ea16c391cfa3a315d3a838c466aae`.

G147's Asset Compression 80, Background Blur 90, Horizon Detection 55, and
HDR 79 were respectively 10.11%, 19.64%, 54.17%, and 25.47% below the current
clean macOS references at 89, 112, 120, and 106. Two unprofiled physical runs
selected exactly those four single-core workloads. A third run selected only
Horizon Detection to resolve the split in the first two. Every attempt emitted
`KERNARIUM_GEEKBENCH_END:0`, powered the guest off cleanly, and returned zero
from the managed emulator.

| Workload | macOS | clean iPad run 1 | delta | clean iPad run 2 | delta | tie-break | delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Asset Compression | 89 | 84 | -5.62% | 83 | -6.74% | -- | -- |
| Background Blur | 112 | 96 | -14.29% | 93 | -16.96% | -- | -- |
| Horizon Detection | 120 | 51 | -57.50% | 143 | +19.17% | 51 | -57.50% |
| HDR | 106 | 79 | -25.47% | 75 | -29.25% | -- | -- |

Asset Compression therefore does **not** reproduce a greater-than-10% gap.
Background Blur and HDR do. Horizon has three slow observations and one fast
observation when G147 is included. G42 previously recorded the same workload
alternating between Geekbench adaptive-duration steps under an otherwise
unchanged build. The slow mode is repeatable, but the 51/143 split is not a
stable emulator-throughput delta by itself.

Separate one-workload launches then attached Time Profiler only after the
single-core phase began, for 15 seconds each. The contextual profiled scores
were 99 Background Blur, 52 Horizon Detection, and 76 HDR; profiler-attached
scores are intrusive and are not clean confirmation evidence. All three
thermal tracks remained Nominal throughout. Percentages are mutually exclusive
top-of-stack weights over active application samples.

| Workload | Gadget assembly | Emulator C++ | Native guest kernel | Host/system | Non-carrier | P / E carrier samples |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Background Blur | 98.828% | 0.410% | 0.013% | 0.722% | 0.026% | 99.987% / 0.013% |
| Horizon Detection | 88.962% | 0.265% | 8.775% | 1.980% | 0.018% | 99.398% / 0.602% |
| HDR | 88.789% | 9.718% | 0.060% | 1.406% | 0.027% | 99.980% / 0.020% |

These captures exclude thermal throttling, efficiency-core placement,
UIKit/Metal, input, networking, and host carrier scheduling as material causes.
They rank the next focused work as follows:

1. **Profile-keyed MTE-disabled vector memory.** No-SP vector-memory symbols
   account for 34.905% of Background Blur and 17.751% of HDR. The eligible
   pair, other, and unsigned gadget families load and branch on
   `CpuState::mte_enabled` on every ordinary access even though the production
   guest neither advertises nor enables MTE. G147's pair-only proposal should
   be broadened to a code-generation-profile-keyed untagged variant of all
   eligible families. Keep the checked symbols and MTE unit semantics. The
   maximum local saving is one load and one branch per hit, so a focused probe
   and matched Blur/HDR physical A/B must establish an end-to-end gain.
2. **Checked indexed floating point in Background Blur.** Checked indexed
   operations total 34.243%, with `simd_indexed_fmla_checked_8h_0` alone at
   19.386%. A15 lacks AFP, so dynamic FPCR.AH/FIZ semantics prohibit replacing
   these with unchecked operations. Measure checked-operation density per
   decoded block and software-fallback incidence; only then test one
   block-level ordinary-FPCR guard or multiversion. Do not repeat G144's larger
   indexed-FMLA stream, which regressed its focused control.
3. **HDR block boundaries and links.** HDR's 9.718% C++ share includes
   `ProgramCache::find` at 3.191%, block entry at 2.136%, `LinkRecord` hash
   insertion at 1.221%, translated-loop overhead at 0.915%, FP-environment
   setup at 0.663%, `prepare_cache` at 0.398%, indirect-link learning at
   0.391%, and signal return at 0.199%. Instrument block-exit causes,
   cache hit/miss/eviction, record creation, and direct/indirect link hits
   before changing one leaf. G144's four-way stable slab regressed Clang and
   remains rejected.
4. **Horizon byte-store locality.** Byte-memory gadgets total 55.260%; the
   `store8_unsigned_fast_z` and `scalar_other_store8_sxtw_0_r` bodies alone
   total 49.182%. First count adjacent-store run length and same-page locality.
   Only a concentrated result would justify a bounded run that preserves exact
   fault priority and retirement. G144's synthetic 64-bit memory fusion gained
   34.9% in its probe yet regressed real workloads, so blind fusion is not a
   candidate.

Horizon's 8.775% native-kernel region resolves to compaction, folio migration,
LRU accounting, and locks: `isolate_migratepages_block` is 1.408%,
`mem_cgroup_update_lru_size` 1.137%, and `__buffer_migrate_folio` 1.101%.
That is the 1 GiB guest-memory-pressure class the user explicitly excluded;
no swap, reclaim, or storage-redirection workaround is proposed.

Disposition: accept the targeted selector and confirmation/profile evidence,
but no product hot-path change. The MTE-disabled vector-memory arm is the
strongest cross-workload candidate. Every candidate still requires focused
correctness and performance probes, then a matched representative physical
A/B under the existing greater-than-2% prospective gate. Raw traces, XML
exports, launch records, exact commands, hashes, and the live-console boundary
are documented under
`.build/results/kernarium-ipad-targeted-20260814/`.

## G150: physical-iPad 1.5 GiB fit check

Recorded 2026-08-14 on the G147/G149 iPad mini (6th generation), iPadOS 18.5.
The standard-signed Release app was built from base
`89e3bf411a4352e09b53dbee6957cb8a215127bb` plus the worktree's in-progress
configurable launch/settings changes; this measurement does not commit or
attribute those source changes. Its executable SHA-256 was
`a75a7e0f1b9ff71b71d021bb461e0a79d46c86e2049e72ccd33bc2cd2d15ee04`
and embedded runtime SHA-256 was
`589e7b4bca2568a765e4542eba276cfceb1665625e8e485924e8bc77ed4ce3df`.

One unprofiled run selected Asset Compression, Background Blur, Horizon
Detection, and HDR under the same no-systemd gadgets shell, six vCPUs, existing
EROFS root, and recreated 8 GiB ext4 OverlayFS disk as G149. A command-line
UserDefaults override supplied 1536 MiB without changing the persisted app
setting. The guest confirmed `mem=1536M ncpus=6`, brought all CPUs online,
reported 1,521,152 KiB available from 1,572,864 KiB during initialization, and
Geekbench reported 1.46 GB.

| Workload | 1.5 GiB score | Rate | G149 clean 1 GiB observations |
| --- | ---: | ---: | ---: |
| Asset Compression | 82 | 2.54 MB/s | 84 / 83 |
| Background Blur | 96 | 0.40 images/s | 96 / 93 |
| Horizon Detection | 115 | 3.57 Mpixels/s | 51 / 143 / 51 |
| HDR | 76 | 2.22 Mpixels/s | 79 / 75 |

The selected-workload summary was 86 single-core, 82 integer, and 94 floating
point. Geekbench emitted `KERNARIUM_GEEKBENCH_END:0`; the guest powered down;
Darwin UML reported `result=0 exitcode=0`; and the managed runtime returned
zero. No guest OOM, iOS memory termination, or benchmark error occurred, so
the requested fallback to a smaller guest was not taken. After every success
marker, the operator closed the intentionally persistent idle SwiftUI process;
the launch JSON's later signal 9 with no core dump is therefore not jetsam.

The three stable-shaped workloads remain inside or immediately adjacent to
G149's 1 GiB observations. Horizon 115 is between its earlier 51/143 modes and
only 4.17% below the macOS reference at 120, reinforcing the existing
adaptive-duration diagnosis. This single run establishes that the four
requested workloads fit at 1.5 GiB on this device. It does not prove that
additional guest memory improves emulator throughput or that a 1.5 GiB full
desktop/Aquarium campaign fits. Exact launch arguments, installation and
rootfs-preservation records, hashes, and the live-console boundary are under
`.build/results/kernarium-ipad-1_5g-20260814/`.

## G151: matched macOS and physical-iPad regression traces

Recorded 2026-08-14 for the two G149 workloads with a confirmed regression of
more than 10% on the physical iPad: Background Blur and HDR. Asset Compression
did not reproduce that threshold, and Horizon Detection remained bimodal, so
they were not promoted into this comparison. Fresh macOS controls used the same
canonical 16 KiB kernel and EROFS rootfs, gadgets engine, no-systemd launcher,
1 GiB guest, and six vCPUs as the iPad. Background Blur scored 113 and HDR 105,
matching the existing 112/106 macOS references despite replacing macOS's
ordinary 8 GiB/ten-vCPU defaults. Both guests powered down normally. Time
Profiler attached after `Single-Core` appeared for the same 15-second duration
as the G149 captures.

| Workload / platform | Gadget assembly | Emulator C++ | Native guest kernel | Host/system | Non-carrier |
| --- | ---: | ---: | ---: | ---: | ---: |
| Blur / macOS | 98.849% | 0.515% | 0.032% | 0.605% | 0.000% |
| Blur / iPad | 98.828% | 0.410% | 0.013% | 0.722% | 0.026% |
| HDR / macOS | 87.476% | 11.753% | 0.019% | 0.739% | 0.013% |
| HDR / iPad | 88.789% | 9.718% | 0.060% | 1.406% | 0.027% |

Blur has an exact-symbol total-variation distance of 0.063993. Its checked
indexed-FP share is 32.787% macOS versus 34.252% iPad, no-SP vector memory is
34.536% versus 34.861%, and the same checked half-precision FMLA is the largest
leaf at 19.098% versus 19.391%. The iPad therefore does not add C++, kernel,
system, or scheduling work to the confirmed Blur regression.

The whole-window HDR distance of 0.351159 is not a platform comparison: the
windows cover different internal phases. Five equal-time slices show three
macOS slices of one stable kernel followed by two of another. The iPad starts
with the second kernel, covers two additional kernels, and ends with the first.
Comparing only observed matching phases reduces the distances to 0.125608 and
0.148472.

| Matched HDR phase | Platform | Gadget assembly | Emulator C++ | Guest kernel | Host/system |
| --- | --- | ---: | ---: | ---: | ---: |
| A | macOS | 88.298% | 11.139% | 0.000% | 0.563% |
| A | iPad | 88.495% | 11.107% | 0.000% | 0.398% |
| B | macOS | 86.279% | 12.672% | 0.048% | 1.001% |
| B | iPad | 82.477% | 17.024% | 0.000% | 0.499% |

Phase A has no material category difference. Phase B's +4.353 percentage
points of iPad C++ time is almost entirely `ProgramCache::find`, at 5.968%
versus 1.598% (+4.370 points). Within that function, 51.811% of iPad exclusive
samples land at offset `+0xbc`, the result check reached in cache way 2;
macOS instead concentrates in the initial/way-0 probe. This shows a deeper
lookup on iPad in that phase. It does not yet distinguish stable platform
behavior from different pre-capture replacement history, so require per-way
hit counters before changing the cache. Even removing the complete observed
excess bounds this phase's speedup near 4.6%, far short of the clean 25--29%
HDR score deficit.

The trace UUID products contain identical raw instructions for the dominant
checked FMLA, vector-pair load, vector 32-bit load, scalar FMADD, and
`ProgramCache::find`. `GadgetExecutor::run_block` has 388 instructions in both;
its only mnemonic-order difference is one independent `lsr`/`sub` scheduling
swap. Both captures run essentially entirely on performance cores. There is no
visible iOS-specific engine, feature-selection, UIKit/Metal, kernel, system,
or carrier-scheduling tax.

Disposition: no product change. The one emulator-local difference worth a
counter experiment is HDR's later-way lookup. The dominant cross-device gap
is below a percentage Time Profiler's resolution: the same machine-code paths
consume more wall time on the A15 performance core than on M1 Ultra. Separate
frequency, IPC, cache, and instruction-latency effects with matched hardware
counters or focused native gadget probes rather than adding an iOS-only path.
Raw traces, exact commands, XML hashes, the reference-resolving classifier,
phase matrices, PC-offset tables, product UUIDs, and instruction-byte checks
are under
`.build/results/macos-ios-regression-traces-20260814/`.

## G152: physical-iPad versus macOS instruction throughput

Recorded 2026-08-14 at base `42d781210a8265955f9f23edb9b0ec70b69f3180`
without modifying or attributing the worktree's in-progress Kernarium settings,
runtime, tests, and kernel configuration. The physical iPad mini (6th
generation) and M1 Ultra Mac Studio both ran the canonical 16 KiB kernel and
EROFS root, gadgets engine, no-systemd launcher, 1 GiB guest, and six vCPUs.
CPU Profiler sampled `CORE_ACTIVE_CYCLE` at a 1,000,000-event threshold. A
separate CPU Counters pass supplied 1 ms timer samples and scheduler state.
[Apple's Processor Trace support](https://developer.apple.com/documentation/xcode/analyzing-cpu-usage-with-processor-trace)
does not include the A15 or M1, so exact retired-instruction streams were not
available.

Background Blur scored 113 on macOS and 99 on iPad in the active-cycle
captures. Its stable 15-second windows execute the same machine-code and
symbol mix established by G151.

| Blur metric | macOS | iPad | iPad residual |
| --- | ---: | ---: | ---: |
| carrier active-cycle rate | 2.906492 Gcycle/s | 2.638408 Gcycle/s | mac/iPad 1.101608 |
| cycles per equal image | 1.000000 | 1.036134 | +3.613% |
| `vector_pair_load128_0` cycle share | 14.883782% | 16.281608% | +1.986151 equal-work points |
| four checked indexed `FMLA.4S` gadgets | 13.410106% | 14.776012% | +1.899828 equal-work points |
| all CpuState loads | 45.417900% | 46.243959% | +2.497054 equal-work points |

The 1.101608 active-cycle-rate ratio alone predicts an iPad score of 102.58.
The remaining 3.613% cycles per image closes the observed 113/99 ratio. The
whole-gadget excess is concentrated in serial CpuState load-use chains: the
checked FMLA path loads FP state and three vectors, executes native `fmla`,
stores the result to CpuState, and dispatches; the pair-load path loads the
guest address, checks MTE/page crossing, probes the software TLB, executes
`ldp q0, q1`, stores both registers to CpuState, and dispatches. Threaded-stream
loads add only 0.467432 equal-work point, while host-memory/software-TLB loads
are slightly lower on iPad. Precise-PMU skid differs across M1 and A15, so this
finding belongs to the complete sequences and grouped loads, not the one
successor PC on which a sample lands.

Complete CPU Counter runs then scored HDR 104 on macOS and 79 on iPad. Their
timer profiles expose a wall-time phase that active-cycle weighting nearly
hides:

| HDR timer metric | macOS | iPad |
| --- | ---: | ---: |
| sample span | 102.532999 s | 147.596001 s |
| carrier CPU time | 96.001 s | 135.113 s |
| `vector_store32_unsigned_fast` | 1.991 s | 11.636 s |
| `vector_other_store32_x_0` | 0.645 s | 10.116 s |
| two-store total | 2.636 s / 2.745805% | 21.752 s / 16.099117% |
| `ProgramCache::find` | 2.904 s / 3.024968% | 3.880 s / 2.871670% |

The two 32-bit store gadgets account for 19.116 of 39.112 extra iPad
carrier-seconds, or 48.875% of the observed difference. Only the iPad has a
continuous 16.0-second interval in which those two leaves exceed 80% of timer
samples. Across its 2.5-second bins, the carrier remains Running
95.220--96.902%, Interrupted 2.827--4.763%, Blocked at most 0.250%, and
Runnable at most 0.008%; samples remain on A15 performance cores. The phase is
therefore repeated guest-store execution, not iOS leaving a runnable carrier
unscheduled, efficiency-core placement, UIKit/Metal work, or enforcement of
Instruments' CPU-budget diagnostic.

Both paths perform full translated-store work around the native `str s0`:
decode the stream operand, load guest address inputs from CpuState, test MTE
and page crossing, probe the write software TLB, load `q0` from CpuState,
store four bytes to guest host memory, and dispatch. This is the largest HDR
instruction-phase difference observed, but it is not yet a per-instruction
latency result. Geekbench's adaptive internal phases do not expose a retired
guest count; the iPad may execute an additional initialization or calibration
run. First count exact guest PCs, retired occurrences, consecutive store-run
length, and same-page locality, then use an identical-work M1/A15 gadget probe.
Do not infer an eightfold `str` latency or restore G144's rejected blind memory
fusion from timer duration alone.

The active-cycle profiles also confirm G151's later-way iPad
`ProgramCache::find` behavior, but the complete timer pass gives it only 0.976
extra second and a lower iPad percentage. It is a secondary localized target;
per-way hit counters remain required before changing cache placement or
replacement policy.

Disposition: no product change. G152 supersedes G151's statement that the
dominant gap is wholly below percentage-profiler resolution. Blur divides into
a 10.2% active-cycle-rate difference and a 3.613% serial-chain cycle residual.
HDR's largest observed wall-time outlier is the two translated 32-bit vector
store paths, subject to the adaptive-work-count caveat; the sequential
ProgramCache probe is secondary. Exact commands, exported profiles, hashes,
the compressed scheduler-state evidence, and reference-resolving analysis are
under `.build/results/macos-ios-instruction-counters-20260814/`. The transient
3.2 GiB CPU Counters bundles and unused context-switch export were removed
after retaining the compact evidence.

## G153: equal-work 32-bit vector-store probes reject the HDR latency hypothesis

Recorded 2026-08-15 at base `24a9c8ae` while preserving unrelated in-progress
Kernarium, kernel-configuration, and rootfs-test changes. The M1 Ultra Mac and
physical A15 iPad mini both ran the gadgets engine, canonical 16 KiB kernel
`Linux 6.18.41-g2df7a2f2d5ec`, 1.5 GiB guest memory, six vCPUs, and one guest
worker. The iPad used a Release app and its signed 16 KiB runtime carrier. A
minimal EROFS image made the probe PID 1 there; macOS used the ordinary guest
shell runner. Boot and shutdown are outside every in-guest timing interval.

Two new freestanding probes differ only in the encoding of their 32 target
stores. One uses `STR S0,[Xn]`, selecting `vector_store32_unsigned_fast`; the
other uses `STR S0,[Xn,X28]` with a zero index, selecting
`vector_other_store32_x_0`. Both repeat the same 16 cache-line address sequence
twice. Each of five `CLOCK_MONOTONIC_RAW` samples executes a fixed five million
iterations: 160,000,000 target stores and 170,000,000 guest instructions in
the measured loop. The exact probe binaries used on both hosts have SHA-256
`ac0124cf080d659928dab5f02f01afe4da2cb1595fbf408c028d0845848d6cf9`
and `d6242b9b472c4bc79bddc0829f3792db6f33c41d6251d257e5d29cf83c212508`
respectively. Disassembly confirmed 32 intended store encodings followed by
the same `SUBS`/`B.NE` loop tail in each payload.

| Store form | macOS median | iPad median | macOS ns/store | iPad ns/store | iPad/macOS time |
| --- | ---: | ---: | ---: | ---: | ---: |
| unsigned offset | 284.711 ms | 264.669 ms | 1.779 | 1.654 | 0.929607 (-7.039%) |
| zero register offset | 286.117 ms | 264.511 ms | 1.788 | 1.653 | 0.924485 (-7.552%) |

The unsigned-offset medians correspond to 597.097 and 642.311 million guest
loop instructions per second on macOS and iPad. The register-offset medians
correspond to 594.162 and 642.695 million. The result therefore rejects a
physical-iOS per-occurrence throughput regression for these hot, same-page
32-bit vector-store gadget paths. In this controlled workload the iPad is
about 7% faster, not multiple times slower.

Disposition: accept the probes and negative result; make no emulator hot-path
change from G152's timer-symbol durations. G152's long iPad HDR store phase
must reflect a different number or pattern of guest-store occurrences, such as
adaptive calibration or phase behavior, rather than an eightfold latency for
the two store gadgets. This probe does not model HDR's complete cache/locality
mix. The five samples were consecutive within one launch per platform rather
than randomized across launches, but their tight spread is ample to reject
the reported order-of-magnitude hypothesis. Exact commands, raw samples,
product hashes, and disposable EROFS hashes are under
`.build/results/vector-store32-cross-host-20260815/`.

## G154: iOS KDE-desktop bring-up is limited by the vCPU count, not translation

Recorded 2026-08-15 at base `943f9dcc` while preserving unrelated in-progress
Kernarium UI, test, and documentation changes in the working tree; those files
are not attributed to this experiment. Every arm ran the **gadgets** engine, as
instructed, because that is the only engine a physical iOS device can use. All
arms booted the same `rootfs.erofs` KDE image on the canonical 16 KiB kernel
`Linux 6.18.41-g2df7a2f2d5ec` with `mem=1536M`.

Hosts: an Apple M1 Ultra Mac Studio (16 performance, 4 efficiency cores,
macOS 26.3) and a physical iPad mini 6 (A15, **2 performance, 4 efficiency
cores**, iPadOS 18.5). The Mac carried background `screensharingd`,
`fseventsd`, and `mds_stores` activity throughout, with load average 7--10 of
20 cores; runs were interleaved rather than blocked on a quiet host.

Two console markers bound the phases. `Started sddm.service` ends the
CPU/IO-bound service boot. The once-only `Native display: direct ...` record
from `native_display.mm` and `native_display_ios.mm` marks the **first
accelerated frame**, that is, the first scanout presented from a GPU texture
rather than the CPU fallback. Both are reported relative to the kernel banner,
so host launch and app startup are excluded.

| Arm | vCPUs | to sddm | to first frame |
| --- | ---: | ---: | ---: |
| macOS native | 6 | 6.953 s | 22.555 s |
| macOS native | 2 | 9.987 s | 30.089 s |
| iPad | 6 | 7.135 s | 63.505 s |
| iPad (repeat) | 6 | 7.132 s | 65.401 s |
| iPad | 2 | 8.682 s | 44.559 s |
| iPad | 2 | 9.319 s | 55.558 s |
| iPad, 90 s cooldown | 2 | 8.661 s | 39.624 s |

At the shipped six-vCPU policy the iPad reaches sddm in 7.135 s against
macOS's 6.953 s, a 1.03 ratio, so the service boot is at parity. The same run
needs 63.505 s to present its first accelerated frame against macOS's
22.555 s, a 2.82 ratio. The regression is therefore confined to the phase
after the display manager starts rendering.

A 100-second device Time Profiler capture of the six-vCPU boot
(`xctrace`, Time Profiler, iPad mini 6) explains why. The process consumed
211.335 s of CPU over 100.383 s of wall time, or **2.11 cores**, spread over
exactly six carrier threads at 13.00--23.28% each. Samples landed **73.52% on
performance cores and 26.48% on efficiency cores**: the two A15 P-cores are
close to saturated while the four E-cores stay largely idle. Under 5% of
sample weight resolved to a named symbol because the gadget bank and the
embedded guest kernel are not images Instruments can symbolize, so this
capture is evidence about thread count, core class, and utilization, not about
a hot function.

Cutting the guest to two vCPUs, matching the A15's performance-core count,
moved first frame from 63.505/65.401 s to 39.624--55.558 s. Every two-vCPU run
beat every six-vCPU run despite the six-vCPU runs being earlier in the session
on a cooler device, which makes the comparison conservative. The service boot
moves the other way, 7.13 s to 8.66--9.32 s, because early boot does benefit
from more runnable CPUs; the desktop phase dominates the total.

The matched two-vCPU pair is the decisive number: macOS 30.089 s against the
iPad's best 39.624 s is a **1.32** ratio. Most of the apparent 2.82x is
therefore the vCPU policy interacting with a heterogeneous core layout, not a
per-instruction translation deficit and not the ANGLE/Metal presentation path.
This is consistent with G153, where the same device was about 7% *faster* than
this Mac on an equal-work gadget store probe.

Two further observations, neither of them a performance result:

- The iPhone 17 Pro Max Simulator could not be used as a KDE arm at all. It
  reaches `graphical.target` (sddm 7.505 s, session 10.955 s) but never leaves
  fbcon and never prints a presentation record, in both the 440x860/160 DPI
  point mode and the 1320x2580/480 DPI Retina mode. The same guest geometry
  reaches autologin normally under the macOS launcher, so this is specific to
  the Simulator graphics stack rather than to the image or the density.
- `ExecutableArena::available()` in `darwin/cpu/arm64/jit/jit_arena.cpp`
  reports JIT support on the iOS Simulator because its probe only checks that
  the `MAP_JIT` `mmap` succeeds, which it does there. The subsequent `store()`
  faults, because the W^X toggle is compiled out for every iOS target under
  `#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)`. A direct probe on the
  booted Simulator showed `mmap` succeeding, an unguarded write raising
  SIGBUS, and both a `dlsym`-resolved `pthread_jit_write_protect_np` and a
  plain RW-then-RX `mprotect` succeeding. Selecting the JIT engine on the
  Simulator today would crash rather than refuse.

Limitations. The iPad preference could be written only once: `cfprefsd`
caches the value after first use and ignored every later file write, so a
four-vCPU arm was not measured and the six-vCPU state was not restored. Two of
the three two-vCPU runs had no enforced cooldown and their 16 s spread is
consistent with thermal drift, so the two-vCPU figure is a range, not a point.
One device, one image, and one page profile were used, and the Mac carried
background load. Nothing here measures steady-state desktop or WebGL frame
rate, which remains the open question for the ANGLE/Metal path.

Disposition: no product change in this commit. The evidence supports changing
Kernarium's guest CPU default from every processor `NSProcessInfo` reports to
the performance-core count, but that default should not be changed until it is
validated on more than one device family and against the Simulator and macOS
products.

## G155: the image's tty0 getty collides with SDDM's VT and stalls KDE bring-up

Recorded 2026-08-15/16 at base `46f757cf` while preserving unrelated in-progress
Kernarium UI, test, and documentation changes. All arms ran the gadgets engine,
the canonical 16 KiB kernel, `rootfs.erofs`, and `mem=1536M`.

KDE desktop bring-up failed intermittently: the guest reached
`graphical.target`, SDDM authenticated `uml`, and then nothing rendered, leaving
the VT text console on screen. The failure is **not** iOS-specific. It
reproduces on macOS, most readily at four vCPUs, which is how it was diagnosed
at all; the iOS Simulator merely showed it first.

The image enables `getty@tty0.service`. `systemctl show` reports
`UnitFileState=enabled` with an empty `SourcePath`, and
`/run/systemd/generator/getty.target.wants/` contains only
`serial-getty@ttyS0.service`, so this is a real enablement symlink in the image
rather than generator output. `/sys/class/tty/console/active` is `tty0 ttyS0`.

`/dev/tty0` is the *active VT alias*, not a fixed console. SDDM logs
`Using VT 2` and `Jumping to VT 2`; the tty0 getty follows the compositor onto
VT 2, both claim the same VT, and the Wayland session dies at once:

```
sddm-helper[518]: Jumping to VT 2
sddm-helper[518]: VT mode didn't need to be fixed
sddm[320]: Session started true
sddm-helper[336]: [PAM] Closing session
sddm[320]: Auth: sddm-helper exited with 1
```

Both KDE launchers now mask `getty@tty0.service` and start
`getty@tty1.service`, keeping the image's text login prompt on the VT that boot
already leaves it on while removing the collision. Interleaved macOS A/B at
four vCPUs, alternating arms to remove the host-load confound:

| Arm | Desktop reached |
| --- | ---: |
| `getty@tty0` relocated to tty1 | 12/12 |
| unchanged | 3/10 |

Fisher's exact p = 0.0031. Four Simulator runs with the fix reached first
accelerated frame in 23.559--24.883 s, against two of seven before it. Four
macOS runs through the unmodified launcher reached it in 23.531--25.472 s.

A separate defect was found and fixed while diagnosing this: neither KDE
overlay created `~/.local/share/sddm`, so SDDM's redirect of the Wayland
session's stderr failed and every reason a failed bring-up would have reported
was discarded. That is a diagnostics fix only; with the directory present the
collision still reproduced, and the captured session log was empty because the
session dies before writing anything.

Two claims made earlier in this investigation were wrong and are corrected
here. The failure is not Simulator-specific, and the missing session-log
directory is not its cause: a control rerun of the arm that had scored 5/6
failed 2/2, so that apparent improvement was host-condition variance rather
than the change.

The physical A15 iPad mini 6 was then re-measured with the same fix, using a
Release build installed over USB. At the shipped six-vCPU policy, first
accelerated frame moved from 63.505/65.401 s before the fix to 54.079 s after
it, about 16% faster, because a collided session costs SDDM a failed attempt
and a retry even when the next one succeeds. Device runs at four vCPUs with the
fix reached 42.040 s and 51.325 s. The best observed device configuration is
therefore four vCPUs with the relocation at 42.040 s, against 64.5 s mean for
the shipped six-vCPU pre-fix default: a 35% reduction. The device vCPU count
could only be changed by writing the preference plist and reinstalling the app,
because `cfprefsd` caches the value and ignores later file writes, so pre- and
post-fix device arms are not matched at every count and the two four-vCPU runs
differ by 9 s, consistent with the thermal drift already noted in G154.

Disposition: accept the launcher relocation and the `AGENTS.md` carve-out that
authorises it. The underlying defect belongs to the image, which should enable
`getty@tty1.service` rather than `getty@tty0.service`; the launcher exception
should be retired when a rebuilt image ships. This experiment measures bring-up
reliability and first-frame latency only, not steady-state desktop or WebGL
frame rate. Raw logs are under the session scratchpad; the macOS host carried
background `screensharingd`/`fseventsd` load throughout at load average 7--10
of 20 cores.

## G156: WebGL Aquarium is about half rate on the iOS Simulator at matched resolution

Dated 2026-08-16. The first Aquarium measurement taken on any product. Both
arms ran the *same* guest workload: `webglsamples.org/aquarium` with
`numFish=500`, a 440x860 guest desktop at 160 DPI, `mem=1536M`, `ncpus=4`, and
the gadgets engine. 440x860 is what an iPhone 17 Pro Max Simulator produces in
Kernarium's default point-resolution mode, and the macOS arm was pinned to the
same geometry with `--gpu-width=440 --gpu-height=860 --gpu-dpi=160`. Both used
the native Metal display, not VNC.

Everything inside the guest was byte-identical. The macOS runner lifted the
extension manifest, verifier, Chromium launcher flags, and systemd unit
verbatim out of the heredocs in `KernariumRuntime.mm`, so the only difference
between the arms is the host product. The verifier was temporarily instrumented
to log one `UML_AQUARIUM_FPS|T=<ms>|FPS=<v>` record per sample for 150 samples;
that instrumentation was reverted afterwards and the shipped verifier still
reports its ten-sample average.

Both arms were reduced by one rule, so no per-platform judgement enters the
comparison. Plateau is the mean of the final twenty readings inside a common
140 s post-load window; stable is the earliest reading after which a trailing
ten-sample mean never again leaves +/-10% of that plateau. Smoothing is
required because the page reports whole-number FPS: at four frames per second a
raw reading alternates between 4 and 5, which straddle the plateau and can
never sit inside a +/-10% band.

Runs were interleaved ABBA (Simulator, macOS, macOS, Simulator) so host drift
could not be read as a platform effect.

| Arm | Plateau fps | Stable after first frame |
| --- | ---: | ---: |
| Simulator 1 | 3.95 | 60.0 s |
| Simulator 2 | 3.95 | 52.8 s |
| macOS 1 | 7.65 | 38.3 s |
| macOS 2 | 7.45 | 35.3 s |

macOS sustains 7.55 fps against the Simulator's 3.95, a factor of 1.91, and
settles in 36.8 s against 56.4 s, a factor of 1.53. The two platforms' arms do
not overlap on either metric, and the ordering survives every combination of
+/-10%/+/-15% tolerance and a 120 s/140 s window. The verifier's own average
over all 150 samples gives the same picture: 4.30 fps Simulator against 7.53
macOS.

The gap is confined to sustained WebGL throughput. Every other stage matched:
guest boot to first accelerated desktop frame was 30.6 s Simulator against
28.5 s macOS, Chromium start was 19.6 s against 20.8 s, and page load, which is
network-dominated, overlapped at 103.9 s against 96.7 s.

Both arms rendered through hardware Virgl rather than a software fallback,
which is what makes the comparison meaningful:

```
Simulator  RENDERER=ANGLE (Mesa, virgl, OpenGL ES 3.0)
macOS      RENDERER=ANGLE (Mesa, virgl (Apple M1 Ultra), OpenGL ES 3.0)
```

Both reported `CANVAS=1024x1024` and `WebGL 2.0 (OpenGL ES 3.0 Chromium)`. The
Aquarium canvas is fixed at 1024x1024 independently of the desktop geometry, so
the two arms submitted identical GPU work; the matched 440x860 desktop affects
compositing and presentation only.

The Simulator runs native arm64 on the same M1 Ultra as the macOS arm, at the
same vCPU count, memory, engine, resolution, and canvas. Translation is
therefore not a candidate explanation, and neither is the vCPU policy of G154,
which is held equal here.

**Correction.** This entry originally attributed the gap to the host graphics
stack -- virglrenderer on CGL desktop GL against virglrenderer on
ANGLE-over-Metal GLES -- reasoning from the Simulator's
`No suitable EGL configs found` log line and its shorter renderer string. G157
profiled both arms and refuted that: host GL and Metal account for under half a
percent of self time on either side. The attribution below is superseded; the
measurements above stand.

Disposition: no change is made on this evidence. The measurement establishes
the ratio and rules out vCPU count, resolution, canvas size, guest
configuration, boot, and browser startup. Localising the cost required the
profiles in G157. Limitations: two runs per arm, Simulator only, no
physical-device arm, and the macOS host carried background
`screensharingd`/`fseventsd`/`ReportCrash` load throughout at load average
6--7 of 20 cores, affecting both arms equally by construction. The
physical-device arm is excluded by policy on the 1.5 GiB acceptance device.

## G157: the Simulator Aquarium gap is guest-CPU throughput, and the JIT is not the fix

Dated 2026-08-16. Follow-up to G156, which measured a 1.91x Aquarium deficit on
the iOS Simulator against macOS at matched geometry but misattributed it to the
host graphics stack. Both arms were profiled with `sample(1)` for 40 s, attached
once the verifier had produced 45 readings so the window sits inside the
plateau rather than averaging over the ramp.

`sample` reports the same tick count for every thread, so the useful quantity is
the share of ticks whose deepest frame is real work rather than a blocking
syscall. Per-thread, over 40 s:

| Thread | Simulator busy | macOS busy |
| --- | ---: | ---: |
| four vCPU carriers, combined | 39482 (41.7%) | 55947 (55.3%) |
| `uml-gpu` | 1950 (8%) | 893 (4%) |

The host graphics stack is idle on both. Self time attributes 31408 samples to
the Kernarium runtime against 64 for ANGLE's `GLESv2` on the Simulator, and
48819 to `uml-darwin` against 162 for `AGXMetalG13X` plus 84 for
`AppleMetalOpenGLRenderer` on macOS. Under half a percent of self time is
host GL or Metal on either arm, which refutes G156's host-GL attribution
outright. `uml-gpu` is 92% blocked on the Simulator and 96% on macOS: it is
waiting for guest work, not producing frames slowly.

The hot frames are the same on both arms and are dominated by translation and
cache machinery rather than gadget execution -- `ProgramCache::activate` is the
single largest self-time frame on each (2682 Simulator, 4383 macOS), ahead of
`Builder::build`, `decode_load_store`, `ProgramCache::find`, and the link-record
hash table. The distribution is not merely similar; the profile shape is the
same and macOS simply runs about 1.4x more of everything. Aquarium in this
configuration is bound by guest-CPU emulation throughput, and the Simulator
gets less of it.

One platform-specific difference did show up: the Simulator's vCPU carriers
spend 12393 samples blocked in `read` against 3446 on macOS, 13% of their wall
time against 3.4%. That is guest block-device traffic against the same 3.8 GB
EROFS image, and it accounts for roughly half the busy-time gap. It is not
explained here.

Because translation overhead led the profile on both arms, the obvious
candidate was to run the Simulator on the JIT rather than the data-only gadget
engine. That required two fixes. `ExecutableArena::available()` probed only
that the `MAP_JIT` mapping succeeded, so it could promise an engine whose first
store would fault; it now also requires a publication route. And the W^X toggle
was compiled out for every target with `__IPHONE_OS_VERSION_MAX_ALLOWED`
defined, which silently included the Simulator -- yet the Simulator runs on
macOS and does enforce MAP_JIT W^X. A standalone probe confirmed the symbol is
present there and reachable through `dlsym`:

```
dlsym_pthread_jit_write_protect_np=PRESENT
routeA_dlsym_toggle=OK result=42
routeB_rw_then_rx=OK result=42
routeC_jit_mprotect=RW_FAILED
```

With those in place the Simulator runs the JIT, and it boots faster: first
accelerated desktop frame at 23.7 s against 29.2--32.0 s on gadgets. It is
nonetheless far worse at the workload. Against a gadget arm that loaded assets
and held a 3.95 fps plateau, the JIT arm loaded **zero** Aquarium assets and
produced **zero** FPS readings in 900 s, never reaching page load. Chromium
stayed alive and logging throughout, so this is pathological slowness rather
than a crash. The plausible mechanism is that V8 writes guest code continuously
and each write invalidates translations, but that is a hypothesis this
experiment did not test.

Disposition: reject the JIT on the Simulator outright, not merely as its
default. The measurement above is the performance reason, but the standing
reason is stronger and independent of it: a physical device can never publish
into a `MAP_JIT` mapping, so a Simulator that translates through the `dlsym`
route would stop representing the product it exists to stand in for. The
`dlsym` route is therefore removed rather than left available behind an
explicit selector, and the measurement here is retained as the record of what
it would have bought. Both Apple products keep the gadget engine and
`UML_DEFAULT_EXECUTION_ENGINE_JIT` stays macOS-only.

The arena fix is kept, and under this policy it is what enforces the decision:
an engine is advertised only when it can actually publish, so both iOS products
now report the JIT unavailable and an explicit request fails rather than
faulting on its first translated block. `ExecutionEngine::automatic` falls back
to the gadget engine when the platform default is unavailable rather than
failing outright, and Kernarium passes `auto` with a
`KERNARIUM_EXECUTION_ENGINE` override so a gadget/interpreter A/B needs no
rebuild.

Reproducing the JIT arm requires locally restoring the `dlsym` publication
route in `jit_arena.cpp`; the probe output above is the evidence that it works.

The Simulator deficit is therefore localised but not closed: it lives in guest
CPU throughput, with the excess `read` blocking as the one concrete lead. Two
untested candidates remain -- the guest idle and timer-wakeup path, since both
arms idle heavily in `__pselect` and the Simulator idles more, and the block
I/O difference itself. Limitations: one profile per arm, Simulator only, and
the JIT arm is a single run, which is enough for a result this large but not for
a ratio.

## G158: memoising the gadget link record removes a hash lookup from every reactivation

Dated 2026-08-16. G157 found the Aquarium workload bound by guest-CPU
emulation on both Apple products, with `ProgramCache::activate` the single
largest self-time frame on each arm (2682 samples Simulator, 4383 macOS) and
the `links_` link-record hash insert accounting for a further 865 and 1776.
Together that is 9.4% of attributed self time on the Simulator and 10.8% on
macOS. This entry acts on that.

The cost is structural rather than incidental. `prepare_cache` calls
`invalidate_links` whenever the guest code-change sequence advances at the same
generation, which resets every link activation as G140/G141 requires. Every
block executed afterwards therefore takes the slow path in
`ProgramCache::find`: revalidate the cached program word by word, then
`activate` it again. A workload whose guest writes code continuously -- V8 in
Chromium is the case that matters -- never leaves that cycle, so reactivation is
the steady state rather than a transient.

`activate` resolved its link through `links_.try_emplace(pc, pc)` on every one
of those passes, hashing a guest PC to reach the same node it reached last
time. `links_` is node-based, so that address is stable until `invalidate()`
clears the map, and `invalidate()` also advances `epoch_`. The slot now
memoises the record pointer alongside the epoch it was resolved in and reuses
it while that epoch holds, which removes the hash from the reactivation path
without changing which record is used.

Measured with the probe pair built for this mechanism.
`guest_gadget_code_invalidation_loop` executes IC IVAU against unchanged code
every iteration, advancing the code-change sequence and forcing exactly this
revalidate-then-reactivate path. `guest_gadget_code_invalidation_control_loop`
substitutes DC CVAU, keeping the same cache-maintenance execution boundary
without advancing the sequence. Separate emulator binaries from the same
configure, run ABBA at one vCPU and one worker, seven repetitions per run,
medians reported:

| Arm | Sensitive probe | Control probe |
| --- | ---: | ---: |
| without memoisation | 0.7208 s mean, 0.714--0.733 | 0.049 s |
| with memoisation | 0.6977 s mean, 0.689--0.709 | 0.049 s |

3.21% faster on the sensitive probe over six runs per arm with no overlap
between the two arms' ranges, and the control does not move at all, which is
what makes the improvement attributable to the reactivation path rather than to
boot or probe overhead.

Disposition: accept. Both page profiles pass, 575 + 49 and 576 + 49.

Two larger candidates were examined and not taken. `activate` also calls
`index_stream` unconditionally, and on a revalidation whose program and
activation are unchanged that re-derives entries it already holds; skipping it
under a per-slot indexed-activation guard would capture more of `activate`'s
self time, but a stream index entry evicted by another program would then never
be reinstated and `find_stream` would fall back to walking every slab, trading a
bounded win for an unbounded loss. The larger prize is `invalidate_links`
itself: because the code-change sequence is global, a write anywhere resets
every link, including blocks on untouched pages. Scoping invalidation to the
changed range, or replacing the eager reset with the activation-epoch scheme the
JIT link table already uses, would attack the reactivation rate rather than the
per-reactivation cost. Both are worth more than this change and both need
range-carrying invalidation the guest ABI does not currently supply.

Correction, same date. Both candidates in this paragraph were wrong, for
different reasons.

The `invalidate_links` mechanics are wrong: range is not the only way to shrink
that reset, and scoping it by activation needs no ABI change at all. G159 built
that, measured it, and rejected it because the cost removed is too small a share
of the workload to matter -- though G160 later made the same scoping worthwhile,
because keeping several address spaces resident grows `links_` enough that the
reset does become expensive.

The `index_stream` rejection is wrong outright. An evicted stream index entry
would not be stranded, because the eviction is observable where it happens: the
evicting pass clears the victim's marker and the victim rebuilds its entries at
its own next activation, which is exactly when an unconditional walk would have
rebuilt them. Index coverage is identical, so nothing is traded. Measured on
`guest_gadget_reindex_loop`, which reactivates a full-size translated block
every iteration, ABBA over four runs an arm at seven repetitions:

| Arm | Sensitive probe | Control probe |
| --- | ---: | ---: |
| unconditional re-index | 0.46025 s, 0.459--0.461 | 0.13725 s |
| skip when unchanged | 0.40975 s, 0.408--0.411 | 0.13725 s |

10.97% on the probe with no overlap between arms, 15.6% once the control is
subtracted, and the control identical to the millisecond across all eight runs.
Targeted single-core Geekbench was flat, as expected for a workload that
reactivates little. The guard shipped with the cache work in G160.

## G159: scoping the gadget link reset by activation -- rejected

Dated 2026-08-16. Measured and reverted. The mechanism improvement is real and
the workload improvement does not exist; the gap between those two statements is
the entry.

G158 set `invalidate_links` aside on the reasoning that attacking it needed
range-carrying invalidation the guest ABI does not supply. That reasoning was
wrong. The reset can be scoped by *activation* instead of by range, with no ABI
change, because only a record that was activated since the last reset can differ
from the reset state. `links_` accumulates a record for every branch target the
translator ever named, whether or not it was executed, while only a resident
block holds an activation, so the two scopes are far apart.

Temporary instrumentation on a disposable build counted both populations at
every call, on macOS with the gadget engine:

| Workload | Calls | `links_` mean | activated mean | peak `links_` | indirect mean | activated mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Aquarium, 1536 MiB, 4 vCPU, 175 s | 143360 | 3719 | 324 | 70958 | 372 | 47 |
| Geekbench single-core preamble, 2 vCPU, ~7 min | 2048 | 12704 | 98 | 21431 | 1188 | 13 |

Aquarium pays about 820 resets a second at a ratio near eleven; Geekbench's
ratio is 130 but it advances the sequence about seventy times less often.

`guest_gadget_code_invalidation_loop` cannot see this at all -- its guest names a
handful of targets -- and an ABBA over four runs an arm confirmed that, at
0.699/0.708/0.707/0.697 against 0.703/0.708/0.711/0.703, fully overlapping. The
probe pair added here does see it. `guest_gadget_link_population_loop` walks a
chain of 4096 distinct single-branch blocks once, leaving that many named
targets behind, then runs the identical IC IVAU loop;
`guest_gadget_link_population_control_loop` builds the same population and
substitutes DC CVAU. ABBA, four runs an arm, seven repetitions a run, medians:

| Arm | Sensitive probe | Control probe |
| --- | ---: | ---: |
| walk every record | 0.2355 s mean, 0.233--0.240 | 0.0190 s |
| walk activated records | 0.0867 s mean, 0.086--0.087 | 0.0187 s |

2.71x on the probe with no overlap, and 3.18x on the reset alone once the
control is subtracted. Both unit profiles passed with the change applied,
575 + 49 and 576.

None of that reached the workload. WebGL Aquarium on the macOS native display at
the matched 440x860 and 160 DPI geometry, ABBA, gadgets, 1536 MiB, four vCPUs:

| Run | Arm | Plateau | Stable after page load |
| --- | --- | ---: | ---: |
| 1 | without | 8.10 fps | 90.2 s |
| 2 | with | 8.35 fps | 130.5 s |
| 3 | with | 8.10 fps | 75.1 s |
| 4 | without | 8.20 fps | 40.5 s |

8.15 against 8.225 on the plateau, a 0.9% difference smaller than the spread
between the two runs of either arm, and time-to-stable varies by about 50 s
within an arm, which is larger than any between-arm difference. The page reports
whole-number FPS, so nothing below a few percent is resolvable here in any case.

The arithmetic explains the null result and should have been done first.
143360 resets removing roughly 3400 node visits each is about 487 million visits
over the run; at a couple of nanoseconds apiece that is around one second of CPU
against the roughly 700 core-seconds the measured window spends on four vCPUs.
The change makes 0.14% of the workload three times cheaper. A probe that
isolates a mechanism reports the mechanism's own cost as 100% of its runtime,
which is exactly why an isolated probe result must be converted into a share of
the target workload before it justifies anything.

Disposition: reject, reverted. The probe pair and
`an_unrelated_code_change_resets_an_untouched_link` are kept. The probes make
the mechanism measurable for anyone who returns to it, and the test pins a
G140/G141 property that had no coverage -- that a code change resets links on
pages it never touched -- which passes on unmodified code and is independent of
the rejected change. The standing lesson is that `invalidate_links` is cheap in
absolute terms no matter how badly it scales, so link-table work is not where
gadget-engine time goes; `activate` and its callees, which G157 ranked as the
largest self-time frame on both products, remain the evidenced target.

## G160: keeping several address spaces resident in one tagged gadget cache

Dated 2026-08-16. Accepted. The gadget program cache was keyed by guest PC
alone, so a translation could not survive a switch to another address space and
`prepare_cache` emptied the cache on every one. Counting that on a browser guest
found 297--320 whole-cache flushes a second, each discarding about 3000
translations that had just been built, and a plateau lookup hit rate near 30%
with 900K--1.5M retranslations a second. Translation and cache work together
were about 30% of plateau CPU, and `miss_invalid` stayed at zero throughout, so
almost none of that retranslation followed an actual change to guest code.

Slots, link records and indirect sites now carry the address space they
describe, so one pool serves all of them and a switch retains rather than
discards. Residency is bounded: measured reuse returns to one of the last eight
distinct address spaces 94.5% of the time, and only 1.6% of switches are to an
address space never seen before, so an LRU bound of eight keeps capacity for
what is running at almost no cost in returns.

Two intermediate designs were built and rejected against measurement.

Partitioning the existing slot budget between N separate caches keeps memory
flat and does not work: at eight caches of 2048 slots, 97.6% of insertions
evicted a live entry, and at 4096 slots 92.9%, because one browser process needs
about 3000 slots on its own. Startup then became slow enough that Chromium's
child processes missed their fifteen-second IPC deadline and the page never
loaded at all. The occupancy that suggested partitioning would fit -- 39--64%
-- had been measured while the cache was flushed hundreds of times a second and
never allowed to fill, so it understated the real working set.

Tagging without a residency bound is worse still. Nothing retires a dead address
space, so a guest that starts and exits processes accumulates all of them: 467
address spaces held translations in a 16384-slot pool, about 35 slots each, and
99.8% of insertions evicted a live entry. Flushing on every switch was wasteful
but self-limiting; removing the limit without replacing it is not an
improvement.

Measured on WebGL Aquarium over VNC, macOS, gadgets, 4 vCPUs, 3 GiB of guest
memory, differenced across the plateau:

| design | hit | flush/s | retranslations/s | memory |
| --- | ---: | ---: | ---: | ---: |
| flush on switch | 29.7% | 479.8 | 1,463,895 | ~90 MB |
| tagged, 16384 slots, 8 resident | 46.1% | 0 | 839,247 | 176 MB |
| tagged, 32768 slots, 8 resident | 58.6% | 0 | 502,254 | 238 MB |
| tagged, 16384 slots, 16 resident | 45.0% | 0 | 742,712 | 400 MB |
| tagged, 65536 slots, 8 resident | 76.2% | 0 | 255,000 | 382 MB |

65536 slots with eight resident is the shipped default. Sixteen resident is
worse on every axis at fixed capacity, which is the same dilution partitioning
produced. Eviction rate is not comparable across these rows: a full cache
displaces something on essentially every insertion by definition, and the tagged
pools run full while the flushing one never fills.

Frame rate moved from 8.00 to 9.10 on single runs, which is not evidence of a
frame-rate gain. G157 found this workload bound by round-trip latency with the
vCPU carriers only 39--55% busy, so removing emulation work is not expected to
convert proportionally into frames. The defensible claims are the cache ones.

Memory is the cost and it is linear in vCPU count, because each vCPU carries its
own executor and the gadget engine shares no translations between them. Measured
at 16384 slots and eight resident: 185.3 MB over four executors and 385.0 MB
over eight, 104% of linear, with per-executor cost flat at 46.3 and 48.1 MB.
Both of the components that dominate the footprint scale per executor rather
than being divided -- streams 71.6 to 143.9 MB and links 93.0 to 199.1 MB --
which is duplication, not division. On Kernarium's policy of taking every
processor the device reports, eight cores at the shipped default projects to
roughly 800 MB against a 1.5 GiB guest, so cross-executor translation sharing,
which the JIT already has as G134, is the next thing this design needs.

## G161: sharing gadget translations between executors

Dated 2026-08-17. Built, correct, and measured; it removes 90% of translation
work and does not make WebGL Aquarium faster. Whether it earns its place is a
memory question, measured separately below. G160 left the gadget cache costing
memory linear in vCPU count, because every vCPU carries its own executor and its
own cache and the engine shared nothing between them. Counting the duplication
on the browser guest at the shipped default found that almost none of the
translation work is new:

| vCPUs | translations | first sight | another executor had built it | own repeat |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 179,306,496 | 5.1% | 69.1% | 25.8% |
| 8 | 144,703,488 | 6.2% | 79.7% | 14.1% |

Each distinct translation was built 16--20 times, and the cross-executor share
rises with vCPU count, which is what it should do if the duplication is real:
more executors means more chances somebody else got there first.

### The relocation contract

A gadget program cannot simply be copied between executors. Its stream embeds
pointers to `BlockLink` and `IndirectLink` records, and those belong to one
executor and one address space. Everything else in the stream is a gadget entry
point, a process-global table, or a value derived from the words being
translated, so two executors translating the same words at the same address emit
identical streams apart from exactly those cells.

`LinkRelocations` records each such cell by the guest PC it names rather than the
pointer it holds, and every builder site that emits one goes through `emit_link`
or `emit_indirect_link` so the record cannot drift from the stream. A block ends
at its first control-flow instruction and no terminal emits more than two link
cells, so the record is a fixed two-entry array with an overflow flag; an
overflowed record is never published.

A canonical entry retains zero in those cells, which is the fail-safe direction:
a consumer that failed to resolve one faults on a null stream rather than
branching into another executor's state. Identity is the guest address, every
guest word the build could have consumed, and the complete code-generation
profile, settled by comparing the words rather than by the hash that places the
entry, so a collision costs a publication rather than a wrong translation.
Publication is gated on two or more live executors. Nothing may retain a pointer
into the store past its shard lock, because publication reclaims.

`tests/unit/arm64/arm64_gadget_shared_translation_tests.cpp` destroys the
producing executor before the consumer executes a single instruction, so a missed
relocation is a sanitizer failure rather than a silent wrong answer, and it
requires the consumer's linked run to retire three instructions, which is only
possible if the relocated cell reached the consumer's own activated record.

Sharing is not confined to one address space. Two address spaces running the
same bytes at the same address are the same translation, because only the link
cells are per-address-space and the consumer resolves those against whichever
address space it is bound to, so a guest running one program in several
processes shares across all of them.

### What it does, and does not do

WebGL Aquarium over VNC, macOS, gadgets, 4 vCPUs, 3 GiB of guest memory, 65536
slots and 8 resident, four runs per arm in a counterbalanced ABBA/BAAB order,
differenced across the plateau. Both arms embed kernel build #78 and carry the
same measurement instrumentation, so the comparison isolates the store.

| | frame rate | builds/s | adoptions | shared hit | translation memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| no sharing | 9.04 | 303,290 | -- | -- | 368.7 MB |
| shared store, 128 MiB | 8.76 | 30,826 | 89.1% | 88.9% | 488.1 MB |

Builds fall 89.8% and the store answers 88.9% of the questions put to it. The
frame rate does not follow: 9.04 to 8.76 is -3.0% at p = 0.114 over all 70
splits, which does not reach the 0.029 floor this design can produce. The arms
overlap -- 9.30/9.10/8.70/9.05 against 8.60/8.90/8.80/8.75 -- so the defensible
reading is flat, possibly slightly negative, at 32.4% more translation memory.

Two candidate explanations were tested and rejected. Adopted blocks are not
failing to link: they enter the C++ boundary less often than built ones,
1.19M against 1.31M lookups a second, and every other rate falls by the same
9%, which is a guest making less progress rather than one crossing the boundary
more. And the cost is not the eviction sweep, though that was a real defect:
`reclaim()` originally granted reprieves bounded by the shard's entry count, and
with an 89% hit rate most entries carry a recent adoption, so each publication
rotated through thousands of deque nodes while holding the lock every adoption
needs. Bounding it to eight reprieves is correct and is retained, but it moved
the frame rate from -4.9% to -3.0%, which is within the noise of each other.

The likely explanation is that the gadget builder is already fast. G99's ThinLTO
and G100's textual composition of the decode families exist to make decode cheap,
and a browser basic block is 6--7 instructions, about 16 stream words. Rebuilding
that is not clearly dearer than hashing 256 bytes of window, taking a shard lock,
comparing 256 bytes, and copying the result out of a randomly addressed 128 MB
region. This is the opposite of the JIT's position in G134, where compilation is
expensive enough that sharing plainly pays.

### The store is additive, not a replacement

| | index | slabs | streams | links | indirect | per-executor | shared | all |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| no sharing | 18.0 | 44.0 | 201.1 | 90.6 | 5.3 | 359.0 | 0 | 359.0 |
| shared store | 18.0 | 44.0 | 194.3 | 99.3 | 5.8 | 361.4 | 128.0 | 489.4 |

Sharing removes the work of rebuilding, not the space of holding: the
per-executor caches are unchanged because they are what the hot lookup path
hits. At a fixed per-executor size the store can therefore only cost memory,
which is why the frame-rate result above settles nothing about the design's
purpose.

The store holds 267,132 entries in 128 MiB, which is 502 bytes each and is an
exact figure: unlike the pool, it accounts what it actually allocated. Taking
off the entry's own bookkeeping and its copy of the fetched guest window, which
the executor caches do not keep, leaves roughly a hundred bytes of gadget
stream, or about fourteen words -- consistent with the five or six instructions
a browser basic block runs before it branches, though the subtraction is
derived rather than separately measured. Against that the per-executor
`streams` figure is 194 MB over 252,247 occupied slots, about 770 bytes a slot.
`CountingResource` measures what the pool requested from upstream rather than
what is live, so most of that gap is pool slack and retained free chunks. That is
a finding about G160's memory rather than about this change, and it is a more
promising target than the translations themselves.

### Can sharing buy back the memory? No -- adoption costs more than building

The frame-rate result above settles nothing on its own, because at a fixed
per-executor size the store can only add memory. The design's purpose was to let
the per-executor caches shrink, a miss costing a copy instead of a build. Three
arms, four runs each, interleaved in a counterbalanced Latin arrangement, all
three embedding kernel build #80; same guest, resolution and memory as above.

| arm | frame rate | builds/s | adoptions | cache | shared | total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 65536 slots, no sharing | 9.31 | 298,358 | -- | 364.8 MB | 0 | 364.8 MB |
| 16384 slots, no sharing | 7.96 | 888,580 | -- | 186.4 MB | 0 | 186.4 MB |
| 16384 slots + 128 MiB shared | 6.28 | 24,257 | 96.5% | 193.0 MB | 128.0 MB | 321.0 MB |

Both differences from the 65536 baseline reach p = 0.0286, the floor for four
against four. Sharing made the small cache worse, not better: 6.28 against 7.96,
21% slower while removing 97% of its build work. That is the result stated as
plainly as it can be -- the store does its job almost perfectly and costs frame
rate for it.

It also prices the mechanism. Per frame, at the plateau:

| arm | lookups | builds | adoptions |
| --- | ---: | ---: | ---: |
| 65536, no sharing | 135,756 | 32,038 | 0 |
| 16384, no sharing | 209,685 | 111,596 | 0 |
| 16384 + shared | 213,155 | 3,866 | 112,421 |

The two 16384 arms are within 1.7% on lookups, so between them the only thing
that changed is that 107,730 builds a frame became 112,421 adoptions -- and the
frame got 27% dearer. Were adoptions free, that arm would have been the faster
of the two. Reading frame rate as emulator throughput and using the 65536 arm to
pin what a build costs, an adoption comes out at two to three times a build,
the spread covering what one assumes a lookup costs, which the three arms cannot
separate on their own. The reading depends on frame rate tracking throughput,
which G157 qualifies, but the direction does not: an arm doing a thirtieth of
the build work of its control cannot be a fifth slower unless what replaced that
work costs more than it did.

Nothing about the sizes involved makes this surprising. Adoption hashed 256
bytes of fetch window, took a shard lock, compared 256 bytes, and copied about
110 bytes, to avoid rebuilding a program roughly fourteen words long that the
builder produces in five or six decode dispatches. G134 pays for the JIT because
compilation there is expensive. Gadget translation is not.

The two no-sharing arms are worth keeping separately from any of this, as a
memory and performance curve for G160: 65536 to 16384 slots costs 14.5% frame
rate and saves 48.9% of translation memory.

### The cheaper key: better, and still not close

The obvious answer to "an adoption costs more than a build" is to make adoption
cheaper. The window key was the wasteful part: it hashed 256 bytes of fetch
window and compared 256 bytes, to identify a program built from about fourteen
words. Keying on the guest address and profile alone, and settling identity
against the words the build actually consumed -- the rule the per-executor cache
already revalidates retained programs by -- makes both proportional to the block
rather than to the window. Entries then need only the consumed prefix, not the
whole window. One consequence is required rather than optional: once the key no
longer covers the window, a publication must displace a stale incumbent at the
same address, or a guest regenerating code at one address would lock that
address out of the store forever.

Four runs an arm, counterbalanced, kernel build #80, against a fresh
same-configuration control.

| arm | frame rate | builds/frame | adoptions/frame | entries | bytes/entry |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16384, no sharing (control) | 8.06 | 110,714 | 0 | -- | -- |
| 16384 + shared, window key | 6.28 | 3,866 | 112,421 | 266,954 | 503 |
| 16384 + shared, cheap key | 6.70 | 2,089 | 115,295 | 476,511 | 282 |

Every intended effect appeared. Per-entry cost fell from 503 to 282 bytes, so
the same 128 MiB holds 78% more translations, the store's hit rate rose from
96.4% to 98.2%, and the frame rate improved 6.7% over the window key. The
control at 8.06 against the previous campaign's 7.96 is 1.3% apart, so the two
campaigns are comparable and that improvement is real.

It is also nowhere near enough: 6.70 against 8.06 is -16.9% at p = 0.0286.
Refitting as above puts an adoption at roughly two to two and a half times a
build, down from two and a half to three. The hash and the compare were never
the dominant term. What is left -- a shard lock, a pool allocation, a
hundred-byte copy, and a cold touch into a 128 MiB region -- is irreducible for
any design that keeps translations outside the executor that runs them.

That makes this a property of the gadget engine rather than of the
implementation. Its blocks are about fourteen words and the builder emits them
in five or six decode dispatches, because G99's ThinLTO and G100's textual
composition of the decode families were built to make exactly that cheap. There
is no version of "fetch it from somewhere else instead" that beats it. G134 pays
for the JIT because compilation there is expensive; the same reasoning does not
transfer, and the 69--80% duplication the opening measurement found is real but
not worth removing.

### How long is a gadget block?

The natural response to "an adoption costs more than a build" is to make the
build bigger, so the adoption's fixed costs amortize. Measured over 4,417,749
published blocks on the browser guest, the mean block is **4.93 instructions and
13.03 stream words**:

| length in instructions | share of blocks | cumulative |
| --- | ---: | ---: |
| 1--2 | 31.6% | 31.6% |
| 3--4 | 30.4% | 62.0% |
| 5--8 | 25.1% | 87.1% |
| 9--16 | 10.3% | 97.4% |
| 17--63 | 2.6% | 99.93% |
| 64, the fetch-window limit | 0.07% | 100% |

`block_instruction_count` is the fetch window, not a length limit: a block ends
at its first control-flow instruction, and only 0.07% of blocks run far enough
to be cut off by the window instead. Enlarging the window therefore cannot
lengthen blocks. It only makes every cache miss fetch a larger span of guest
memory -- 4 KiB rather than 256 bytes at 1024 -- and, until this was fixed, made
`validate()` value-initialize a buffer of that size on every revalidation. The
window is now `UML_GADGET_BLOCK_INSTRUCTIONS` and the validation buffer is left
uninitialized, since it is written before it is read.

Getting longer translation units out of this engine means not stopping at the
branch: following a direct branch into its target rather than emitting a link.
That is a real option and it would move the build/adopt ratio the right way, but
it is not a parameter change. It would alter block boundaries, retirement
accounting, link topology, and specifically the G140/G141 validation contract,
which assumes a program's consumed words are a contiguous run from its start
address. It should be evaluated on its own merits rather than as a way to
rescue sharing.

## G162: translating a conditional branch's fall-through inline behind it

Dated 2026-08-17. Accepted, and the largest single gain measured in this
sequence. G161 established that a gadget block is 4.93 instructions and that
nothing which merely moves translations around can pay at that size. The
distribution of *why* blocks end says what to do about it instead:

| block ends at | share | can it be continued? |
| --- | ---: | --- |
| conditional (CBZ/CBNZ 22.4%, fused CMP+B.cond 16.0%, TBZ/TBNZ 7.4%, B.cond 7.0%) | 52.8% | yes, and the fall-through is the next address |
| BL, a direct call | 24.7% | only by inlining the callee |
| BR/BLR/RET, indirect | 14.0% | no, the target is not known at translation time |
| B, unconditional direct | 8.1% | yes, but it leaves a hole in the address range |
| exception, window limit | 0.4% | no |

Conditional branches are both the largest category and the only large one whose
continuation keeps the guest addresses contiguous. That matters more than the
share does: `correct_terminal_instruction_count`, its FPCR counterpart, and the
G140/G141 validation contract all convert a guest PC to a position with
`(pc - base) / 4` and all assume a program's consumed words are a contiguous run
from its start. Continuing through a fall-through preserves every one of those
untouched. Continuing through an unconditional `B` would break all three, which
is why it is not what this does.

The taken edge keeps its link; the not-taken path falls into the next gadget and
stops being an indirect jump through a link record at all. The stream is the
same size, because the word that held the fall-through link now holds the number
of instructions translated behind the branch.

That count is the one thing the change had to add. Entering a block precharges
its whole translated length into the accumulator the engine returns as the
retired count, and until now every exit was a block's last instruction, so
nothing ever had to give any of it back. A taken branch now leaves from the
middle and subtracts its suffix before following its link. The length is not
known while the branch is being emitted, so each site is recorded and `build`
fills it in once the block ends.

WebGL Aquarium over VNC, macOS, gadgets, 4 vCPUs, 3 GiB, 65536 slots and 8
resident, sharing off, four runs an arm counterbalanced, kernel build #80:

| | frame rate | translations/s | cache hit | translation memory |
| --- | ---: | ---: | ---: | ---: |
| block ends at the branch | 9.00 | 297,908 | 76.4% | 366.9 MB |
| fall-through inlined | 11.31 | 168,819 | 85.9% | 347.0 MB |

+25.7% at p = 0.0286, the floor for four against four, with no overlap between
the arms: 11.55/10.75/11.50/11.45 against 8.80/8.60/9.40/9.20. Translations fall
43% because each block covers about 1.75 times as many instructions, the hit
rate rises because there are fewer and better-amortized entries, and memory
falls 5.4%.

Extending the same shape to the fused CMP+B.cond pairs, which take two guest
instructions rather than one and so record a retire count of two, brings
coverage to 52.8% of terminations and more than doubles the gain. Same
conditions, same control, four runs an arm:

| | frame rate | translations/s | cache hit | translation memory |
| --- | ---: | ---: | ---: | ---: |
| block ends at the branch | 9.19 | 297,390 | 76.4% | 361.0 MB |
| all conditional forms inlined | 14.71 | 101,010 | 91.9% | 388.1 MB |

+60.1% at p = 0.0286, again with no overlap: 14.05/14.70/15.05/15.05 against
9.50/9.15/8.85/9.25. Translations fall 66% and the cache hit rate reaches 91.9%.
Memory rises 7.5% here rather than falling, because the blocks are long enough
that larger streams outweigh holding fewer of them; against 60% more frames that
is not a difficult trade.

`arm64-gadget-inline-fallthrough` pins the part no conformance test can see: the
guest registers are identical whether or not the suffix arithmetic is right, and
only the retirement count differs. It requires the not-taken path to retire the
whole block, a taken branch to retire only what ran before it -- two of six, so
four given back -- and a fault past the branch to be counted from its own
address.

### What is left, and why the last of it was rejected

Re-measuring the shape afterwards shows the change absorbed its whole category:
conditional terminations fall to 0.00% and the mean block goes from 4.93 to
10.61 instructions. What ends blocks now:

| block ends at | share |
| --- | ---: |
| BL, a direct call | 48.93% |
| BR/BLR/RET, indirect | 29.28% |
| B backward | 11.77% |
| B forward | 8.43% |
| the fetch window | 0.89% |
| exception | 0.67% |

A call cannot be continued through -- control reaches the instruction after it
only by returning through an indirect branch -- an indirect branch has no target
at translation time, and a backward branch would translate its own loop forever.
That leaves the forward `B`, of which 80.5% land inside the fetch window.

It was implemented and rejected. Continuing at the target does not require a run
list after all: the instructions jumped over can never execute, so they need
only be *counted*, not translated, which keeps the block one contiguous run and
leaves both instruction-count corrections and the validation contract working
unchanged. One gadget hands back the precharge for the counted-but-skipped
instructions and dispatches, with no branch at all -- cheaper than the
`follow_block_link` it replaces.

Measured against the same configuration with only the conditional inlining, four
runs an arm: 14.29 to 13.89 frames a second, -2.8% at p = 0.31. No gain.

The reason is in the same property that made it cheap. `validation_words` is
`instruction_count`, so counting the skipped instructions also saves and
re-compares them on every revalidation, and revalidation runs at a 91% hit rate.
Trading one `follow_block_link`, paid once when the block is entered, against
comparing the skipped words every time the block is revalidated does not come
out ahead at the reaches forward branches actually have -- a median of about
nine instructions. Bounding the reach would shrink the cost and the yield
together: reaches of four or fewer are 25.7% of forward branches, which is 2.2%
of blocks.

It also changes the premise of G161 rather than vindicating it: sharing lost
because a build was too cheap to be worth avoiding, and this makes builds both
larger and rarer. Whether that moves the adopt/build ratio enough to matter is
now a different measurement from the one G161 took.

## G163: translating a direct call's callee inline behind it

Dated 2026-08-17. Accepted at one call site per callee. G162 left 48.9% of
blocks ending at a direct call and 29.3% at an indirect branch, with 103M
indirect-link retrainings measured over the run -- each an indirect branch that
missed its two-entry `IndirectLink` and left translated code for the C++
boundary, at roughly seven times the translation rate. A return is one of those,
so inlining a callee removes both the call boundary and the return.

### What it costs, measured before building it

| | |
| --- | ---: |
| distinct call sites | 3,044,417 |
| distinct callees | 888,455 |
| duplication if every site inlines | 3.43x |
| callees with exactly one call site | 386,151 (43.5%) |
| callees with three or more sites | 347,006 (39.1%) |

### The block stops being contiguous

A callee is at an arbitrary address, so unlike G162's fall-through there is no
counting trick: the block genuinely spans caller, callee, and the continuation
after the return. `Program` therefore carries up to three `GuestRun` entries,
and the three things that assumed contiguity walk them -- both instruction-count
corrections, and the G140/G141 validator, which compares each run against the
addresses that run covers. With one run this is behaviourally identical, which
is what makes the change reviewable.

Whether that walk is affordable was measured before any of the inlining was
built, by splitting every block's single range into three adjacent runs:
identical addresses, words and behaviour, only the validator's walk differing.
That came out at +0.3% (p = 0.89). Range count is free. What sank the forward
branch in G162 was volume -- dead words added to `validation_words` -- and an
inlined callee adds words that are genuinely executed, exactly as the
conditional fall-through did.

A call emits `call_inline`, which keeps only the link register the call sets,
and translation continues into the callee. The callee's `RET` emits
`return_guarded`, which falls through into the continuation only while X30 still
holds the address the call pushed, and otherwise gives back the precharge and
falls into the ordinary cached return whose operands follow it. A build that
cannot open the next run -- an unreadable window, or a call in the continuation
that would need a fourth -- must still leave the stream with an exit, because
both gadgets assume their run follows them; it emits a link to where the block
would have continued, which is the ordinary un-inlined call or return.

### One site per callee, not all of them

| variant | frame rate | change | p |
| --- | ---: | ---: | ---: |
| calls end the block | 13.79 | -- | -- |
| callee inlined at one site | 14.38 | +4.3% | 0.0023 |
| callee inlined at every site | 13.75 | +0.0% | 1.00 |

Eight runs an arm, two counterbalanced campaigns pooled, against a control
carrying the same gadgets in the same file positions but never emitting them, so
both arms share gadget phasing and the difference is the mechanism alone.

Inlining at every site is a perfect wash: the call boundaries and indirect
returns it removes buy exactly what its 3.43x duplication costs. Admitting each
callee once -- the first site to reach it inlines it, the rest keep the ordinary
link -- keeps a fraction of the coverage and none of the duplication, and is
worth 4.3%. Duplication was the larger half of that cancellation.

Two limits on the accepted arm. `maximum_inlined_callees` is 65536 against
888,455 distinct callees, so admission stops once the set fills within an epoch;
coverage is bounded by that as well as by one site per callee. And this is an
increment on top of G162's 60.1%, against a control that already inlines
conditional fall-throughs, not a second independent gain of that size.

The 103M indirect retrainings remain the largest unaddressed cost signal.
Widening the two-entry `IndirectLink` attacks the same return cost as a change
to one data structure, with none of this machinery, and has not been tried.

## G164: the gadget cache and block-length work on the iOS Simulator

Dated 2026-08-17. G160 and G162 were both accepted on macOS. This checks them on
the product they were meant for, against G156's Simulator Aquarium baseline,
which was taken before either landed.

Both sides are the same configuration, read out of the logs rather than assumed:
iPhone 17 Pro Max Simulator, `numFish=500`, a 440x860 guest at 160 DPI,
`mem=1536M`, `ncpus=4`, the gadgets engine, and the 16 KiB kernel profile. The
baseline numbers below are recomputed from G156's own logs with the same script
as the new runs, so the comparison does not depend on how that entry phrased its
plateau: its table records 3.95 under a different definition, while the
last-ten-reading figure used here reproduces the 4.30 verifier average it
reports separately.

| | plateau fps | first frame, guest | first frame, launch | verifier avg |
| --- | ---: | ---: | ---: | ---: |
| before, G156 run 1 | 4.40 | 51.9 s | 107.0 s | 4.50 |
| before, G156 run 2 | 4.20 | 53.5 s | 103.6 s | 4.10 |
| after, run 1 | 6.00 | 34.3 s | 68.2 s | 6.00 |
| after, run 2 | 6.00 | 31.6 s | 64.7 s | 5.95 |

Plateau 4.30 to 6.00, a factor of 1.40. Time to first frame 52.7 s to 32.9 s
measured inside the guest, a factor of 1.60, and 105.3 s to 66.5 s measured from
app launch, a factor of 1.58. Neither metric's arms overlap.

Two cautions on reading this.

The delta is not the gadget work alone. Eleven commits separate the two
measurements, and while most are gadget-engine or unrelated, `37bca53c` changes
GLSL instancing and the virglrenderer patch moved with it. That is a rendering
path Aquarium exercises. The frame-rate share attributable to G160 and G162
specifically is therefore not isolated here; what is established is that the
shipping Simulator product is substantially faster than the recorded baseline,
in the same direction as the macOS A/Bs that did isolate those changes.

"Stable after first frame", the third metric G156 reports, is not usable. Its
own two arms give 141.9 s and 50.0 s for the same quantity, and the new runs
give 136.6 s and 27.8 s: the run-to-run spread is larger than any effect. The
definition is also asymmetric under whole-number FPS readings -- at a 6 fps
plateau a reading of 5 falls outside the +/-10% band, while at 4.3 a reading of
4 sits inside it -- so a faster build is mechanically harder to call stable.


## G165: where the Simulator Aquarium's time and gadget cache actually go

Dated 2026-08-17. G164 established that the shipping Simulator product reaches a
6.00 fps Aquarium plateau against G156's 4.30. This entry asks what the gadget
cache is doing there and where the host's cycles go, using a measurement-only
build plus `sample(1)`. The short answer is that boot and plateau are different
machines: boot is guest-CPU-bound and pays a third of its vCPU time to the gadget
cache, while the plateau leaves the guest's vCPUs idle two thirds of the time.

Configuration, read out of the logs rather than assumed: iPhone 17 Pro Max
Simulator on iOS 26.5, Kernarium, Linux 6.18.41, the 16 KiB profile, a 440x860
guest at 160 DPI, `mem=1536M`, `ncpus=4`, the gadgets engine, SDDM autologin into
Plasma with `chromium-aquarium.service`, `numFish=500` on a 1024x1024 canvas
through ANGLE on Mesa virgl. Shipping gadget options: 65,536 cache slots four
ways, eight resident address spaces, exact validation on, conditional
fall-through inlining on (G162), callee inlining off (G163), cross-executor
sharing off (G161). Host: 20-core arm64 macOS at load 7-9 with a macOS 4 KiB
integration guest of another session running throughout; recorded rather than
treated as grounds to discard.

The measurement build adds counters to `GadgetExecutor` and its `ProgramCache`
and writes one bounded record per executor to the stderr descriptor every 2^21
lookups. It is not committed. Its cost is visible but small: time to first frame
is unchanged at 33.7-34.7 s against G164's 34.3 s and 31.6 s, while the plateau
reads 5.00 rather than 6.00. Because the page reports whole-number FPS, that gap
bounds the instrumentation plus co-tenant cost at somewhere between about 1% and
17%, and no conclusion below rests on it.

### Cache efficiency

Four runs, each the full boot-to-verifier-completion interval, summed over the
four executors.

| | run 1 | run 2 | run 3 | run 5 |
| --- | ---: | ---: | ---: | ---: |
| lookups | 245,366,784 | 211,812,352 | 262,144,000 | 264,241,152 |
| hit rate | 89.09% | 89.58% | 89.67% | 89.43% |
| fast hits | 74.36% | 76.83% | 75.49% | 75.79% |
| revalidated hits | 14.72% | 12.75% | 14.18% | 13.64% |
| misses | 10.89% | 10.40% | 10.31% | 10.55% |
| evictions, share of stores | 62.68% | 63.75% | 64.80% | 64.39% |
| slot occupancy at exit | 90.9% | 84.3% | 80.4% | 83.4% |
| host bytes, all executors | 426.8 MiB | 426.5 MiB | 406.4 MiB | 411.2 MiB |

Run 1 was unprofiled, run 2 profiled at a 1 ms interval, runs 3 and 5 at 10 ms.
Run 2 fell to 2-3 fps under its sampler, so that its ratios still land within
half a point of the other three is the strongest evidence here that these are
workload properties rather than timing accidents.

The decompositions matter more than the hit rate.

**Misses are churn, not cold start.** Every miss reaches `translate()`, so the
translation count is the exact miss count, while the separately counted `miss`
covers only two identifiable sub-causes: an entirely unallocated slab, or a
revalidation that failed. Those account for 12,138 of run 1's 26,725,251
misses. The other 99.95% found an allocated set in which no way held the block
-- meaning it had been evicted or retired, not that it had never been seen. Two
mechanisms empty those ways.

Eviction: the cache runs full. Occupancy peaks at 98.5% of slots in run 1, 99.7%
in run 3 and 99.4% in run 5, and 63-65% of stores displace a live block. That is
not a start-up transient; across a run's records the eviction rate climbs from
6.75% to a steady 80-90% and stays there.

Retirement: whenever an executor first binds a ninth address space it retires
the least recently current one whole. Run 1's per-executor occupancy falls from
its 64,968-slot peak to 18,699 late in the run -- a single cache losing over two
thirds of its contents -- and every one of those blocks has to be rebuilt when
its address space returns.

**Revalidation almost never fails.** 36,139,584 attempts produced 3 mismatches
and 791 fetch faults. Exact validation therefore runs on 14.7% of lookups as
pure insurance: the guest essentially never rewrote code under a cached block.
That is the G140/G141 contract behaving as intended, and it also means the
revalidation path is overhead in the common case rather than work that saves a
rebuild.

**Space.** 426.8 MiB across four executors for a 1.5 GiB guest: streams 260.7,
link records 86.4, slabs 54.0, the fixed stream index 18.0, indirect sites 7.7,
and about 1,878 bytes per resident block. The four caches are private, so most
of that is four copies of the same guest code -- which is what made G161's
sharing attractive before it was rejected on frame rate.

**Address-space residency is pinned at its cap.** Every executor reports eight
resident address spaces, the configured `UML_GADGET_RESIDENT_GENERATIONS`, for
the whole run. Run 1 saw 3,632 first-time binds against 251,973 binds total, so
roughly 3,600 whole address spaces were retired. A desktop guest running
systemd, Plasma and multi-process Chromium simply has more live address spaces
than eight.

### What the guest is executing

| | run 1 |
| --- | ---: |
| guest instructions retired | 69,314,774,010 |
| engine entries | 245,377,188 |
| instructions per entry | 282.5 |
| instructions translated | 310,766,985 (0.448% of retired) |
| mean built block | 11.63 instructions |
| retired per instruction built | 223x |

Blocks end almost entirely on three things: `BL` 47.46%, an indirect branch
27.92%, a plain `B` 22.92%. The fetch window ends only 0.91% of them and
conditional branches have all but disappeared -- 238 `CBZ` and no `B.cond` in
26.7 million blocks -- which is G162 confirmed on the product rather than on
macOS. `BL` is now the single largest boundary, which is exactly the ground
G163 tried and failed to make pay.

### Where the host's cycles go

Profiles are `sample(1)` at a 10 ms interval, restricted to the four
`uml-vcpu-N-carrier` threads, with self time charged to the nearest caller that
is not a libsystem shim so the categories name emulator components rather than
`pthread_getspecific`. Two windows sit inside one run's frame-rate plateau, 60 s
and 25 s; a third covers 30 s mid-boot, before the first frame.

| carrier on-CPU self time | boot | plateau A | plateau B |
| --- | ---: | ---: | ---: |
| gadget bodies, guest userspace | 48.36% | 44.82% | 42.71% |
| UML kernel executing natively | 9.22% | 33.12% | 37.37% |
| cache maintenance | 22.41% | 9.77% | 7.88% |
| other runtime: memory, device, emulator | 8.97% | 5.48% | 5.18% |
| translation | 5.09% | 2.52% | 2.38% |
| cache lookup and validation | 4.70% | 1.48% | 1.86% |
| engine entry and exit | 1.03% | 0.45% | 0.26% |

Inclusive figures for the paths of interest, as a share of carrier on-CPU time:

| | boot | plateau A | plateau B |
| --- | ---: | ---: | ---: |
| `gadgets::execute` | 44.45% | 38.25% | 35.51% |
| `ProgramCache::prepare` | 13.27% | 5.32% | 3.65% |
| of which `retire` | 10.29% | 3.50% | 2.99% |
| of which `invalidate_links` | 2.84% | 1.76% | 0.59% |
| `Builder::build` | 7.60% | 3.76% | 3.06% |
| `ProgramCache::find` | 7.01% | 2.11% | 2.21% |
| `ProgramCache::store` | 5.12% | 2.00% | 2.31% |

The two plateau windows come from the same run 40 s apart and agree within a few
points on every row, which is the reproducibility check for this method.

The `???` frames in the unsymbolicated carrier stacks are the Linux kernel
executing natively out of the signed image carrier, which `sample` cannot
attribute; they are counted as UML kernel time above, together with the TLS and
mutex calls the kernel makes through libsystem.

**Boot and plateau are two different regimes, and that is the finding.** During
boot the four carriers run at 72-76% occupancy and spend only 25.9% of their
wall time off-CPU: the guest is genuinely CPU-bound, and gadget-cache overhead --
maintenance, lookup, translation together -- is 32.2% of carrier on-CPU time
against 48.4% spent executing translated code. At the plateau the same carriers
run at 32-42% and spend 63.4% of their wall time off-CPU, 71% of that wait in
`__pselect`, UML's idle and event wait, and 26.6% in a blocking `read`. The
`uml-gpu` thread is 87.5% off-CPU there, waiting in `mach_msg2_trap` (56%) and
`poll` (37%), and the whole process draws about 1.6 CPU cores. Cache overhead
falls to 13.7% of a much smaller busy share.

So the plateau is not limited by the guest's CPUs: its own four vCPUs are idle
roughly two thirds of the time, waiting on something other than themselves. Boot
is limited by them, and boot is what time-to-first-frame measures.

### Methodological caveat, and it is not small

The sampling interval changes the plateau answer, though not the boot answer.
At 1 ms, plateau `retire` reads 7.56% of carrier on-CPU time against 3.50% at
10 ms, and `invalidate_links` 3.69% against 1.76%; the 1 ms plateau run itself
fell to 2-3 fps. The boot profile is stable across intervals -- 21.24% cache
maintenance and 10.58% `retire` at 1 ms, 22.41% and 10.29% at 10 ms -- which is
what makes the boot figures the ones to trust.

The mechanism fits: suspending the process to walk several dozen thread stacks
adds wall time while the guest's timer deadlines keep accruing, so on resume the
guest takes a burst of interrupts, scheduling and address-space switches. During
boot the guest is already saturated and that injection is proportionally small.
At the plateau the guest is mostly idle, so the same injection is a large
relative addition, and the profile drifts toward looking like the boot profile.
Profiling a time-virtualised emulator biases toward whatever the virtual clock
drives, and the bias is largest exactly where the guest has slack. The 10 ms
plateau numbers still cost 5 fps to 4, so treat them as upper bounds too.

### What this changes

**The plateau is not the place to optimise the engine.** G157 concluded that the
Simulator Aquarium gap was guest-CPU throughput. That predates G160 and G162,
and at the plateau it no longer holds: with the carriers idle 63% of the time in
`__pselect` and `read`, the frame rate is set by the round-trip chain from guest
Chromium through virtio-gpu, the `uml-gpu` thread, virglrenderer, ANGLE and
Metal. Gadget bodies are 44.8% of carrier on-CPU time and the carriers are 36.6%
busy, so executing translated code is about 16% of a carrier's wall time: making
it infinitely fast returns at most that, and only to the extent the freed time
becomes frames rather than more `__pselect`, which on a chain whose other links
are already waiting it largely will not. G157's reasoning stands for the regime
it measured; the balance moved, plausibly because G160 and G162 moved it.

**Boot is the place, and retirement is the target there.**
`ProgramCache::retire` walks every slab and all 65,536 slots, then `erase_if`s
the whole link and indirect maps, to remove one address space. Its own comment
justifies this on the grounds that retirement is rare. On this guest it happens
roughly 3,600 times and costs 10.3% of carrier on-CPU time during boot -- the
one phase that is CPU-bound, and the phase time-to-first-frame measures.
Indexing slots and link records by generation would make the sweep proportional
to what is retired rather than to the cache, needs no extra memory, and is worth
its own A/B against time to first frame rather than against plateau fps.

The miss decomposition says the lever on the 10.9% miss rate is churn, not
warm-up or validation, and it has two independent halves. Four private caches at
426.8 MiB for a 1.5 GiB guest hold four copies of the same code while evicting
live blocks on 63% of stores -- the observation that motivated G161, whose
rejection was about the price of adoption rather than about the diagnosis.
Separately, an eight-address-space cap on a guest running systemd, Plasma and
multi-process Chromium discards two thirds of a cache at a time; whether raising
the cap pays depends on host memory the Simulator does not obviously have, so it
wants measuring rather than assuming.

### Limitations

The instrumented build is not committed and was not run through the canonical
workflows; it exists to produce these counters. A macOS integration guest from
another session ran throughout, and the FPS figures here are consequently not
comparable to G164's -- only the counter ratios and profile shares are being
claimed. Whole-number FPS reporting means the instrumentation cost is bounded
rather than measured. `sample` cannot symbolicate the natively executing Linux
kernel, so its share is attributed as a block rather than broken down. The boot
window is a single 30 s sample from one run; the two plateau windows come from
one run 40 s apart, not from independent runs.

One measurement was discarded rather than reported. `xcrun simctl launch` over a
live instance attaches to the running one, prints nothing on the console, and
returns the old pid, so a profile taken from a run started before the previous
one had exited is silently a second window on the previous run. That is how the
first attempt at a boot profile was produced; it is reported above as the second
plateau window, which is what it actually was. The driver now terminates and
waits for any live instance before launching.


## G166: a 512 MiB shared gadget tier, appended and paid for -- both rejected

Dated 2026-08-18. G161 rejected cross-executor translation sharing at a 128 MiB
budget because an adoption cost two to three times the build it replaced. Two
things have changed since: G162 lengthened the mean built block from 4.93 to
11.76 instructions, so a build is now roughly 2.4x the work it was, and G165
found that gadget-cache churn -- not cold start -- is what produces the 10%
miss rate, with four private caches holding four copies of the same code. Both
argue for another look, so the store was remeasured at 512 MiB in the two shapes
the question naturally takes.

| arm | private slots | shared tier |
| --- | ---: | ---: |
| A | 65536 | none (shipping default) |
| B | 65536 | 512 MiB appended |
| C | 16384 | 512 MiB, paid for by the smaller private cache |

macOS, `.build/sharecache` at one revision, three binaries differing only in
those two cache variables. WebGL Aquarium through the Kernarium guest setup on
the VNC display: 4 vCPUs, 3 GiB, a 440x860 guest at 160 DPI, gadgets, 16 KiB
profile. Twelve runs in the counterbalanced order A B C C B A A B C C B A, four
an arm, a fresh port each; none wedged. Plateau is the mean of the last ten
readings.

### Frame rate

| arm | n | plateau | sd | time to first frame | sd |
| --- | ---: | ---: | ---: | ---: | ---: |
| A | 4 | 13.975 | 0.411 | 61.1 s | 4.1 |
| B | 4 | 13.850 | 0.311 | 63.6 s | 1.5 |
| C | 4 | 10.300 | 0.183 | 73.4 s | 2.8 |

B is -0.89% on plateau, t = -0.49; not distinguishable from the control. C is
-26.30%, t = -16.33, and 20.3% slower to the first frame.

### Cache efficiency

One instrumented run an arm, the same counter build as G165 plus the shared
store's own counters, summed over the four executors.

| | A | B | C |
| --- | ---: | ---: | ---: |
| lookups | 310.4 M | 304.1 M | 320.9 M |
| private hit rate | 90.27% | 90.27% | 72.67% |
| private misses | 9.71% | 9.72% | 27.32% |
| evictions, share of stores | 61.5% | 60.1% | 96.8% |
| shared lookups | -- | 29.6 M | 88.5 M |
| shared adoptions | -- | 16.6 M (56.1%) | 54.5 M (61.6%) |
| blocks actually built | 30.1 M | 13.0 M | 33.7 M |
| instructions built | 354.6 M | 217.5 M | 659.4 M |
| shared entries | -- | 1.62 M | 1.60 M |
| private host bytes | 406.1 MiB | 366.5 MiB | 167.5 MiB |
| shared host bytes | 0 | 512.0 MiB | 512.0 MiB |
| guest instructions retired | 115.9 G | 115.2 G | 99.5 G |

The mechanism works exactly as designed. Appending the tier leaves the private
cache untouched -- 90.27% hit rate in both A and B, to two decimal places -- and
converts 56% of what escapes it into adoptions, cutting blocks built by 57% and
instructions decoded by 39%. The store fills its budget: 1.62 M entries at 332
bytes each, having seen 3.77 M distinct blocks and evicted 2.15 M.

### Why appending it buys nothing

Sixty-second `sample(1)` windows inside each plateau, restricted to the vCPU
carriers, self time charged to the nearest non-libsystem caller. Inclusive
shares of carrier on-CPU time:

| | A | B | C |
| --- | ---: | ---: | ---: |
| `gadgets::execute` | 57.92% | 58.84% | 43.48% |
| `Builder::build` | 4.66% | 2.32% | 9.80% |
| `SharedProgramStore` | 0.01% | 2.31% | 9.96% |
| `ProgramCache::find` | 2.32% | 2.31% | 1.31% |
| `ProgramCache::store` | 2.83% | 2.28% | 8.85% |
| `ProgramCache::prepare` | 6.87% | 5.41% | 5.07% |

B is an almost exact swap: `Builder::build` falls 2.34 points and
`SharedProgramStore` rises 2.30, with `gadgets::execute` unmoved. So adoption is
no longer two to three times a build, as G161 measured on shorter blocks -- on
the current code it costs very nearly the same. That is the mechanism working
and still not paying.

The reason it cannot pay is the size of the prize rather than the price of
adoption. Building is 4.66% of carrier on-CPU time in the control, and the
carriers are 54% busy, so the entire translator is about 2.5% of a vCPU's wall
time. A tier that adopted every single miss for free would return at most that,
against a measurement resolution of roughly +/-1.5% on four-run means. Raising
the budget above 512 MiB moves adoption toward its 75% ceiling -- one of four
executors must always build first -- and cannot raise the ceiling on savings.
512 MiB of host memory is not a reasonable price for under 2.5%, and on the
1.5 GiB iOS guest it is not available at all.

### Why paying for it is much worse

Shrinking the private cache to a quarter does not trade capacity between tiers
at par. Private misses go 9.71% to 27.32% and 96.8% of stores displace a live
block, so the private cache is thrashing outright at 99.98% occupancy. The
shared tier absorbs 61.6% of that, but the miss volume nearly triples, and 33.7 M
blocks still have to be built -- more than the control's 30.1 M despite 54.5 M
adoptions. Instructions decoded rise 86%.

The profile shows where the frame rate went. Translation, the shared store, and
private stores together take 28.6% of carrier on-CPU time against 7.5% in the
control, squeezing `gadgets::execute` from 57.92% to 43.48%; allocator time
roughly doubles as well, at 16.80% against 6.84%, from storing 87.7 M programs
instead of 30.1 M. Guest instructions retired in the same wall window fall 14%.
A shared tier cannot substitute for private capacity because a private hit is a
tagged slot lookup while a shared hit is a hash, an exact word comparison, a
stream copy and a relocation pass.

### Disposition

`UML_GADGET_SHARED_BUDGET_MIB` stays 0, and `UML_GADGET_CACHE_SLOTS` stays
65536. The CMake rationale for the shared budget is updated, since G161's
"two to three times the cost of a build" no longer describes the current code
even though its conclusion still holds.

The useful negative result is the bound: on this workload no translator or
translation-cache change can be worth more than about 2.5% of a vCPU's wall
time, because that is all the translator costs. G165 said the same thing from
the other direction for the plateau. Cache work that is worth chasing is the
maintenance side -- `ProgramCache::prepare` is 6.87% here, nearly half of it the
retirement sweep, which is larger than the whole translator.

### Limitations

Cache efficiency is one run an arm rather than four; G165 established that these
ratios reproduce within half a point across runs, and the three arms' lookup
counts agree within 5%, but the frame-rate arms are the replicated ones. The
counter build is not committed. Frame-rate and counter runs are separate builds,
so the counter arms' plateaus (13.60, 14.30, 10.70) are consistency checks on
the clean arms rather than additional samples. The profiles are one 60 s window
an arm and cost about 1 fps while running, which is why they are used for shares
between arms and not for absolute frame rate. Time to first frame is reported
but has a 4.1 s spread in the control, so only C's 20% regression exceeds it.


## G167: per-address-space cache state, so retiring one is dropping it

Dated 2026-08-18. G165 and G166 both ended pointing at the same thing:
`ProgramCache::prepare` was 6.87% of vCPU on-CPU time on macOS and 13.27% during
the Simulator's boot phase, more than the whole translator, and nearly all of it
was two walks that scale with the cache rather than with the work.

`retire()` removed one address space by walking every slab and all 65536 slots,
then `erase_if`-ing the complete link and indirect maps and both live lists.
`invalidate_links()` walked the single shared `live_links_` vector filtering for
one address space, so with eight resident it discarded seven eighths of the
walk. The code's own comment justified the sweep on the grounds that retirement
is rare; G165 measured roughly 3,600 of them in a run and 4,753 on macOS,
because a desktop guest running systemd, Plasma and multi-process Chromium has
far more than eight live address spaces.

### The change

Everything belonging to one address space now lives in one `Generation`: its
code-change mark, its own link and indirect maps keyed by guest PC alone, its
live-link and live-site lists, and the flat indices of the slots it has stored
into. `generations_` holds at most nine of them.

Retiring an address space is then dropping its `Generation` and clearing the
slots on its own list, both proportional to what it owned. `invalidate_links`
walks only the owner's live lists, with no filtering. Three incidental
consequences: the link key is a single word rather than a (generation, PC) pair,
which also makes `link_record` cheaper on the translation path; the reset cells
of an indirect site now name the owner's own empty record rather than whichever
address space happened to be current, which was the same thing in practice
because `prepare` is the only caller; and retirement no longer calls
`deactivate` per slot, because the records those calls would reset are about to
be destroyed and the cleared epoch already retires the slot's stream-index
entries.

The slot list is a hint, not an authority. It may name a slot another address
space has since taken, and may name one twice, so retirement re-checks every
entry against the slot's own generation and epoch. It can only *miss* a slot by
overflowing, which is capped at the cache's slot count and recorded in
`slots_complete`; retirement answers that with the original whole-cache sweep.
That keeps the fallback exact rather than relying on the list being perfect.

`tests/unit/arm64/arm64_gadget_cache_validation_tests.cpp` gains
`retiring_an_address_space_keeps_the_others`, which fills residency exactly,
confirms nothing has been retired, then runs one more address space and requires
that every survivor still answers from the cache while the retired one has to
translate again. Making `release()` a no-op fails it, so it has teeth.

### What it costs and what it removes

macOS, WebGL Aquarium through the Kernarium guest setup on the VNC display, 4
vCPUs, 3 GiB, 440x860 at 160 DPI, gadgets, 16 KiB profile. P is HEAD, Q is the
change; P's binary hashes identically to G166's control arm.

Sixty-second `sample(1)` windows inside each plateau, restricted to the vCPU
carriers, inclusive shares of carrier on-CPU time:

| | P | Q |
| --- | ---: | ---: |
| `ProgramCache::prepare` | 5.35% | **0.40%** |
| of which `retire` | 3.87% | 0.28% |
| of which `invalidate_links` | 1.46% | 0.00% |
| `ProgramCache::store` | 2.46% | 2.82% |
| `ProgramCache::find` | 2.21% | 2.24% |
| `Builder::build` | 4.63% | 5.26% |
| `gadgets::execute` | 60.37% | 62.06% |

`prepare` falls by a factor of thirteen and the cache-maintenance category with
it, 10.09% to 6.61% of carrier on-CPU self time, while gadget bodies rise 69.60%
to 72.42%. `store` rises 0.36 points, which is the one `push_back` per store the
slot list adds.

Cache behaviour is unchanged, which is the point: this makes retirement cheaper
without making it select differently. One instrumented run an arm, against the
+/- 0.5 point run-to-run band G165 established:

| | P | Q |
| --- | ---: | ---: |
| hit rate | 90.13% | 90.86% |
| misses | 9.85% | 9.13% |
| evictions, share of stores | 60.73% | 63.11% |
| host bytes, all executors | 401.5 MiB | 419.4 MiB |
| bytes per resident block | 2012 | 1966 |

The memory line is the honest cost: the slot lists add about 4 MiB across four
executors, bounded by construction at one 32-bit index per slot per resident
address space, so 2 MiB per executor at 65536 slots and eight resident. Most of
the 17.8 MiB difference above is Q simply holding 6.9% more blocks at the
sampled instant; bytes per resident block went down, not up.

### Frame rate

Sixteen runs in the counterbalanced order P Q Q P Q P P Q P Q Q P Q P P Q, eight
an arm, a fresh port each; none wedged.

| metric | P | Q | change | Welch t |
| --- | ---: | ---: | ---: | ---: |
| plateau fps | 14.287 +/- 0.383 | 14.025 +/- 0.713 | -1.84% | -0.918, df 10.7 |
| time to first frame | 59.71 s +/- 2.35 | 56.63 s +/- 2.52 | **-5.16%** | -2.529, df 13.9 |

Time to first frame improves by 5.16%, p about 0.024. The plateau does not move,
and its arms are not distinguishable.

That split is the prediction G165 and G166 made, now tested. Boot is CPU-bound:
the carriers run 72-76% busy and pay a third of their time to the gadget cache,
so removing cache work shows up directly. At the plateau the carriers are idle
around 47% of the time waiting on the virtio-gpu round trip, so time handed back
to them becomes more `__pselect` rather than more frames. A cache change should
therefore be judged on time to first frame, and this entry is the first in the
series to do that as its primary metric.

Q's plateau spread is nearly twice P's (0.713 against 0.383), driven by one 12.90
reading against a 13.4-15.2 range elsewhere. With eight runs an arm that is not
enough to call a variance change, and the mean difference is not significant
either way; it is recorded rather than explained.

### Limitations

Cache counters are one run an arm, not eight; the frame-rate arms are the
replicated ones, and the counter claim is only that behaviour did not change,
against a band established in G165. Profiles are one 60 s window an arm and cost
about 1 fps while running, so they are used for shares between arms rather than
for absolute frame rate. The Simulator, where G165 measured `prepare` at 13.27%
during boot and where time to first frame matters most to a user, is remeasured
in G168.

## G168: G167 on the iOS Simulator, where the cost it removes was largest

Dated 2026-08-18. Measurement only; no code changed. G167 accepted
per-address-space gadget cache state on macOS and left its own limitation open:
the Simulator, where G165 measured `ProgramCache::prepare` at 13.27% of
boot-phase carrier time and where time to first frame is what a user actually
waits through, had not been remeasured. This entry closes that. P is
`12eca831`, the whole-cache sweep; Q is `a50b8362`, G167. The two installed app
bundles are byte-identical except the two runtime framework binaries -- the
`Kernarium` executable itself hashes the same in both -- so the only difference
is `darwin/cpu/arm64/gadgets/executor.{cpp,hpp}`. Each run reinstalls its arm.

Configuration as G165: iPhone 17 Pro Max Simulator on iOS 26.5, Kernarium,
Linux 6.18.41, the 16 KiB profile, a 440x860 guest at 160 DPI, `mem=1536M`,
`ncpus=4`, the gadgets engine, SDDM autologin into Plasma with
`chromium-aquarium.service`, `numFish=500` on a 1024x1024 canvas through ANGLE
on Mesa virgl. Host: 20-core arm64 macOS at load 8-9 with a Chrome Remote
Desktop host taking about 130% and `fseventsd` about 100% throughout. That
co-tenancy is why absolute time to first frame here is around 63 s against
G165's 34 s; it is recorded rather than treated as grounds to discard, and every
comparison below is between interleaved arms measured under it.

### Where the time went

`sample(1)` at a 10 ms interval, restricted to the four `uml-vcpu-N-carrier`
threads. One 20 s window from 12.4 s after launch, inside boot, and one 30 s
window 40 s after the first frame. Inclusive shares of carrier on-CPU time.

| boot | P | Q |
| --- | ---: | ---: |
| `ProgramCache::prepare` | 12.77% | **1.32%** |
| of which `retire` | 9.89% | 0.93% |
| of which `invalidate_links` | 2.81% | 0.00% |
| `ProgramCache::store` | 3.54% | 4.78% |
| `ProgramCache::find` | 5.62% | 7.49% |
| `Builder::build` | 5.51% | 8.98% |
| `gadgets::execute` | 46.01% | 52.78% |

| plateau | P | Q |
| --- | ---: | ---: |
| `ProgramCache::prepare` | 6.79% | **0.11%** |
| of which `retire` | 4.11% | 0.08% |
| of which `invalidate_links` | 2.68% | 0.00% |
| `ProgramCache::store` | 2.83% | 2.64% |
| `ProgramCache::find` | 2.83% | 1.84% |
| `Builder::build` | 4.48% | 4.23% |
| `gadgets::execute` | 37.21% | 41.12% |

P reproduces G165's independently taken 13.27% at 12.77%, which is the control
this comparison needs. In self time by symbol, `ProgramCache::retire` is the
single most expensive symbol in P's whole boot profile at 9.32% -- ahead of
every gadget body and ahead of the unsymbolicated native kernel -- and does not
appear in Q's top ten at all.

By category, boot cache maintenance falls 19.37% to 11.44% of carrier on-CPU
self time while gadget bodies rise 49.27% to 56.88%; at the plateau the same
pair moves 12.52% to 5.70% and 43.39% to 48.36%. Carrier off-CPU wall time
during boot falls 35.9% to 29.6%.

Two figures in those tables are artefacts of a fixed wall-clock window rather
than regressions. `store`, `find` and `build` rise during boot because Q reaches
further into the boot sequence within the same 20 s and therefore translates
more. And Q's residual 5.70% of plateau cache maintenance is `store` and the
link hash table, not `prepare`.

### Frame rate

Sixteen runs in the counterbalanced order P Q Q P Q P P Q P Q Q P Q P P Q, eight
an arm, each a fresh install and launch; none failed, and all sixteen produced
the full 150 readings.

Time to first frame splits cleanly at the point the verifier's own clock starts,
and the split is the whole result. `launch -> script` covers guest boot,
systemd, SDDM, Plasma and the Chromium launch, which is CPU-bound guest work;
`script -> first frame` is Chromium and WebGL warm-up, which is not.

| | P | Q | change | Welch |
| --- | ---: | ---: | ---: | ---: |
| launch -> verifier script start | 32.24 s +/- 0.96 | 29.92 s +/- 1.12 | **-7.19%** | t = -4.443, df 13.7, p = 0.0006 |
| verifier script start -> first frame | 31.99 s +/- 1.13 | 32.38 s +/- 2.00 | +1.22% | t = +0.481, df 11.0, p = 0.64 |
| launch -> first frame | 64.22 s +/- 1.74 | 62.30 s +/- 2.22 | -3.00% | t = -1.931, df 13.2, p = 0.075 |
| plateau fps, verifier's last 20 | 5.800 +/- 0.089 | 5.644 +/- 0.390 | -2.69% | t = -1.104, df 7.7, p = 0.30 |
| plateau fps, all readings 40 s after first frame | 5.552 +/- 0.121 | 5.496 +/- 0.306 | -1.00% | t = -0.479, df 9.1, p = 0.64 |

The CPU-bound phase improves 7.19% at p = 0.0006, and the separation is nearly
complete: seven of Q's eight runs are faster than seven of P's eight. The
browser phase does not move, and the plateau does not move. The total is a
weighted average of a phase that improved and a phase that could not, so its
-3.00% at p = 0.075 understates the effect on the part of the run this change
can reach; the total is reported because it is what a user waits through, not
because it is the sharper measurement.

That is the same shape as G167 found on macOS, and a sharper version of the same
prediction G165 and G166 made: cache work is worth removing where the carriers
are busy, and at the plateau they are idle around 60% of the time waiting on the
virtio-gpu round trip, so time handed back there becomes `__pselect`.

`launch -> script` is derived per run as the host timestamp of a console line
minus the page timestamp inside it. Two independent markers give it -- the
verifier's `EVENT=load` and its `EVENT=first-fps` -- and they agree within 47 ms
on every one of the sixteen runs, so the derivation is not carrying the result.

The plateau needs one methodological note. The page reports whole-number FPS and
the verifier averages only its last twenty readings, which at five frames a
second is too coarse to estimate from: run 16 and run 3 have nearly the same
whole-run distribution, 86 fives and 58 sixes against 82 and 57, yet the
verifier scores them 5.100 and 5.900. Averaging every positive reading in a
common window instead -- 40 s after the first frame, about 118 readings a run --
is the same reduction applied identically to both arms and cuts P's spread from
0.089 to a comparable 0.121 while pulling Q's from 0.390 to 0.306. Both
estimators agree that the plateau did not move. Q's remaining spread is one run
that was genuinely slow throughout, run 8 at 4.856 with 120 fives and 31 fours;
with eight runs an arm that is not enough to call a variance change.

### Limitations

Profiles are one 60 s window an arm a phase, not replicated, and they cost frame
rate while running, so they are used for shares between arms rather than for
absolute rate. No cache counters were collected on the Simulator for this entry;
the claim that cache behaviour is unchanged rests on G167's macOS counters and
G165's Simulator baseline, not on a Simulator A/B of the counters themselves.
Only the 16 KiB profile was measured; the 4 KiB Aquarium campaign was not run on
either product for G167 or G168, though both page profiles pass the unit suites.
The `launch -> script` and `script -> first frame` split is a decomposition of
one measurement, not two independent ones, so its two rows are not independent
tests.

## G169: the Darwin current-vCPU read is on Linux's interrupt-masking path

Dated 2026-08-18. This entry began as an investigation into why the iOS
Simulator's Aquarium plateau is about 5.5 fps against macOS's 14, and it ends
with two separate findings. The first is that the comparison was confounded and
the deficit is not what it appeared; that is G170. The second, independent of
it, is a real host-primitive cost that this entry fixes.

### What the comparison eliminated

macOS, everything matched except the named variable, gadgets, 16 KiB, 440x860
at 160 DPI, `numFish=500`:

| variable | control | arm | verdict |
| --- | ---: | ---: | --- |
| guest memory 3 GiB vs 1536M | 13.58 fps | 13.52 fps | no effect |
| tmpfs upper vs Kernarium's ext4 on `ubd1` | 13.93 fps | 13.77 fps | 1.2%, noise |

Three runs an arm each. Guest geometry was identical on both products, a
1024x1024 canvas on a 440x860 guest at 160 DPI, and a microbenchmark built for
both targets measured the shipping current-vCPU sequence at 4.32 ns on the
Simulator against 4.56 ns on macOS -- the Simulator's `pthread_getspecific` is
not slower per call. Decomposing carrier CPU per frame, guest *userspace*
emulation costs 0.109 core-seconds on macOS against 0.130 on the Simulator, a
factor of 1.2. Gadget execution was never the problem.

Guest *kernel* CPU per frame was 0.011 core-seconds against 0.089, a factor of
eight, and that is what this entry is about.

### The mechanism

`sample(1)` at 10 ms restricted to the four `uml-vcpu-N-carrier` threads, with
the guest kernel's unsymbolicated frames resolved against `vmlinux` by load
bias. Self time inside the guest kernel:

| | macOS | Simulator |
| --- | ---: | ---: |
| `uml_curr_cpu` | 13.6% | 40.2% |
| `pthread_once@plt` | 3.9% | 16.5% |
| `pthread_getspecific@plt` | 3.0% | 10.5% |
| `_raw_spin_lock_irqsave` | 1.8% | 9.0% |

Two thirds of the Simulator's guest-kernel time was the current-vCPU read. The
PLT rows are why: they sit in the symbol gap between `_einittext` and `_stext`
and only resolve once you notice that `0xc00292ed0` and `0xc00292ee0` are the
stubs themselves. Counting the libsystem bodies those stubs call,
`uml_curr_cpu` is 22.3% of all carrier on-CPU time on the Simulator and 2.3% on
macOS, and 89% of the carriers' `libsystem_pthread` samples are reached through
it.

`uml_curr_cpu()` is UML's `smp_processor_id()` on Darwin, and it is hotter than
a per-thread lookup normally is because `local_signal_state()` resolves the
per-CPU signal state through it. `block_signals()` and `unblock_signals()` are
Linux's `local_irq_disable()` and `local_irq_enable()`, so every spinlock
acquisition and every kernel entry reaches it. `cpu_relax()` pays it twice per
spin iteration, once directly in `darwin_cpu_relax()` and once inside
`um_get_signals()`. The Linux host port uses a plain `__thread int` for the same
value; the Darwin port needs the pthread key because its destructor is the
fixed carrier's one lifetime boundary.

### The change

Series topic 0019 publishes a release-ordered ready flag from the key
constructor and takes an acquire load of it on the read path, instead of
`pthread_once`. The setters keep the once-guard, so the key and its carrier
destructor are still created exactly once. A reader that observes the flag clear
has no carrier at all, and CPU0 is already what `pthread_getspecific` would have
returned it.

Disassembly is the direct check: `uml_curr_cpu` went from three calls --
`pthread_once@plt`, `panic` on its error path, and `pthread_getspecific@plt` --
to one. The microbenchmark puts that at 4.32 ns to 2.13 ns on the Simulator and
4.56 ns to 2.18 ns on macOS.

Adding a topic forced one piece of unrelated churn: `verify-series-policy.py`
requires every patch's mail subject to agree with the series count, so all
fifteen existing patches moved from `[PATCH nn/18]` to `[PATCH nn/19]`. No diff
content changed and every stable patch-id is unchanged; only the file hashes in
the manifest moved.

### What it actually bought

Both products rebuilt against the landed series, Q = topic 0018, S = topic 0019.
Same 10 ms `sample(1)` windows as above, one run an arm a phase.

Boot, where the guest kernel's work is bounded, is where the saving lands:

| carrier on-CPU, Simulator boot | Q | S |
| --- | ---: | ---: |
| UML kernel (native), charged category | 12.12% | 9.02% |
| guest kernel self time | 7.14% | 5.29% |
| `libsystem_pthread` | 3.61% | 2.50% |
| gadget bodies | 55.59% | 56.85% |

A quarter of the guest kernel's boot cost is gone and the carriers spend it on
guest instructions instead.

The plateau is the interesting negative result. `pthread_once` disappears from
the profile entirely, as intended -- and `pthread_getspecific` *rises*, from
14.21% and 512 samples to 20.40% and 780. Total carrier on-CPU rises with it,
4.20% of wall to 4.61%, and the frame rate does not move. The guest kernel at
the plateau is not doing bounded work that got cheaper; it is spinning against
the host GPU, and a cheaper `cpu_relax()` buys more spins rather than less time.
That is worth stating plainly: on an unbounded wait, making the wait cheaper
cannot save anything.

Frame rate, eight counterbalanced Simulator runs and six macOS runs:

| | Q | S | change |
| --- | ---: | ---: | ---: |
| Simulator plateau fps | 5.750 +/- 0.467 | 5.713 +/- 0.477 | -0.65%, p = 0.9 |
| Simulator launch -> first frame | 62.58 s | 63.08 s | +0.81%, p = 0.83 |
| macOS plateau fps | 13.775 (n=2) | 14.333 (n=3) | +4.05% |

Nothing here is significant. The Simulator is bound by the virtio-GPU round
trip at its plateau, so freed carrier CPU becomes `__pselect`; that is the same
conversion failure G167 measured, and it was the prediction before these runs
rather than after. The macOS arms point the right way and are consistent with
the boot profile, but at two runs against three they are not evidence. One macOS
control run stalled in its typed bootstrap and produced no readings; it is the
*control* arm, so it cannot flatter the change, and it is recorded rather than
replaced.

Disposition: accepted on the profile, not on frame rate. The change removes a
call from the hottest host primitive the guest kernel has, it is measurably
cheaper wherever the kernel's work is bounded, and no supported default,
guest-visible behaviour, or campaign result moves. It is not a frame-rate
optimization and must not be cited as one.

### Validation

Fresh authenticated replay of all nineteen topics from the release package,
canonical tree `930696044f4ca953f7ee63f1514585f9fd1e06b6`; delta audit unchanged
at 160 changed files and 7 outside-port; both LLVM 22 production page-profile
builds; `check-integration` and `check-integration-4k` both 53 tooling and 5
guest tests green; `kernel-series-topology`,
`kernel-release-source-preparer`, `kernel-darwin-host-compat`,
`darwin-vcpu-carrier-policy` and `darwin-init-mm-tlb-policy` green.

## G170: decoupling guest rendering from host presentation, and two corrections

Dated 2026-08-18. This entry does three things: it records a display change
that removes a real pathological path, it reports that the change is inert on
both products, and it retracts the measurement that motivated it.

### The retraction first

G169's investigation compared macOS Aquarium at about 14 fps against the iOS
Simulator at about 5.5, and along the way measured macOS on its own native
Metal display at 2.48, 2.57 and 2.59 fps against 13.1-13.6 on VNC. Three
consistent runs, and the conclusion drawn was a 5.2x native-display deficit.

That number is wrong. Running the identical binary and configuration again in a
six-run interleaved isolation produced 13.600 fps on one run and 2.900 on
another, the same arm about thirty-five minutes apart. No code separates them.
The native window's frame rate is bimodal on whether macOS considers it
occluded, and the three original runs all landed in the occluded mode. The
caveat was stated when the number was first reported and should have blocked
the conclusion rather than accompanying it.

With visibility controlled, native and VNC are the same: native 13.600, 14.100,
14.350, 14.450, 14.450 against VNC 13.09-14.55. There is no native-display
deficit, and any future macOS native measurement has to account for this
bimodality or it will measure the window manager.

### The change

`presentOpenGl` and `presentMetal` returned a bool, and the device answered
`false` with a full `readResourceRectangle` GPU-to-CPU readback, a whole
pixel-buffer copy and a CPU blit -- on the guest's scanout-flush path. That
conflated two unlike things: a display that cannot express this scanout at all,
which genuinely needs the readback, and a display whose host presentation is
simply behind, which needs nothing.

`PresentOutcome` now separates them into `unsupported`, `presented` and
`dropped`. Only `unsupported` reaches the readback. A dropped frame is success
for the guest, which keeps its own pace; the next flush carries newer contents
anyway. macOS drops when every IOSurface slot is in flight and when it
overwrites a slot the host never drew; Kernarium already overwrote its pending
texture but did so silently, and now reports it.

`DisplayStatistics` counts `guestFrames`, `presentedFrames`, `droppedFrames`
and `cpuFallbackFrames` as relaxed atomics. `UML_GPU_FRAME_STATS=<seconds>`
prints one bounded line per interval to stderr, mirrored into the macOS window
title; unset keeps it off. Presented frames are counted in the Metal completion
handler rather than in `finishPresentation`, which the encoder-failure paths
also reach.

### What it measures

macOS native, six runs, and the iOS Simulator, eight counterbalanced runs with
statistics on the decoupled arm:

| | macOS native | Simulator |
| --- | ---: | ---: |
| guest frames offered | 550-3341 a run | about 1780 a run |
| dropped | 0 | 1, 0, 0, 0 |
| cpu-fallback | 0 | 0 |
| guest rate vs presented rate | equal | equal, about 6.0/s |

The decoupling is therefore **inert on both products**: neither guest ever
outruns host presentation, so the path this change fixes is never taken. Frame
rate confirms it. macOS decoupled 14.450/14.450 against coupled 14.100/14.350;
Simulator plateau 5.713 +/- 0.480 against 5.938 +/- 0.075, and time to first
frame 60.06 s against 62.41 s, p = 0.35. Nothing here is significant in either
direction.

`presented` exceeds `guest` by roughly 90 frames a Simulator run because CPU
framebuffer frames -- the boot console -- are presented but are not scanout
flushes. That asymmetry is intended and documented rather than normalised away.

### Disposition

Kept, on three grounds that are not frame rate. The readback-on-backpressure
path is real and pathological whether or not this workload triggers it, and a
display that refuses a frame must not be able to force a GPU sync onto the
guest. The counters are the instrument that disproved two hypotheses in one
afternoon -- slot exhaustion, then the native-display deficit itself -- and a
nonzero `dropped` or `cpu-fallback` rate is now a finding rather than an
invisible event. And `check-gpu` covers the new contract with a mutation-checked
test: making a dropped frame fall back to CPU readback fails
`gpu-device-command-atomicity`.

It must not be cited as a performance improvement. On this workload, on both
products, it changes nothing measurable.

## G171: why the iOS Simulator renders Aquarium at 6 fps and macOS at 14.5

Dated 2026-08-18. G168 through G170 kept measuring the Simulator against macOS
without ever asking why the two differ by 2.4x on the same guest. This entry
answers that, separates the part that is the Simulator's nature from the part
that is a guest defect, and records one fix that works on macOS and provably
cannot work on the Simulator.

### The gap, with the guest held constant

Both products boot the same 6.18.41 kernel, the same `rootfs.erofs`, the same
gadget engine and the same geometry. The Simulator's own boot line reads
`440x860 guest at 160 DPI with mem=1536M ncpus=4`, which is exactly what the
macOS harness passes.

| | macOS | Simulator |
| --- | ---: | ---: |
| guest clock, page load to first FPS reading | 26 581 ms | 27 629 ms |
| Aquarium plateau | 14.45 fps | 6.00 fps |

The guest reaches its first frame in the same time on both, so the emulated CPU
is not the difference. Everything below is host-side.

### Where the host time goes

`sample` at 1 ms over about 20 s of plateau, `uml-gpu` thread, divided by the
frames actually produced in the window:

| per frame | macOS | Simulator | ratio |
| --- | ---: | ---: | ---: |
| frame time | 90.8 ms | 248.8 ms | 2.7x |
| serving guest GPU commands | 17.9 ms | 164.5 ms | 9.2x |
| - host-to-guest readback | 2.0 ms | 106.8 ms | 53x |
| - `draw_vbo` | 2.0 ms | 43.0 ms | 21x |
| fence wait, where GPU execution shows up | 12.5 ms | 12.7 ms | 1.0x |
| synchronous XPC to `MTLSimDriver` | 0 ms | 131.8 ms | - |

The fence wait is the same on both. It is the same M1 Ultra and it does the
same work in the same time. What differs is the cost of asking it, and 132 of
the Simulator's 165 ms a frame is `sendXPCMessageWithReplySync` blocked in
`mach_msg2_trap`.

The two products do not run the same host GL. macOS gives virglrenderer CGL
contexts on Apple's OpenGL driver; iOS device and Simulator give it ANGLE GLES3
contexts on Metal, and in the Simulator Metal is `MTLSimDriver`, a proxy that
serialises calls over XPC to a host-side GPU process. The Simulator's readback
tail is `rx::mtl::Texture::getBytes` -> `-[MTLSimTexture getBytes:]` ->
`dispatchOperationWihReply` -> `sendXPCMessageWithReplySync` ->
`mach_msg2_trap`.
macOS has no equivalent tail; its leaf is `gldReadFramebufferData` in
`AppleMetalOpenGLRenderer`, in process.

### The guest asks both hosts for exactly the same thing

A temporary counter over the virgl command stream in `VirglRenderer::submit`,
counting `VIRGL_CCMD_COPY_TRANSFER3D` with `READ_FROM_HOST`. It was reverted
after the measurement and is not in the tree.

| interval | commands | reads | pixels per read |
| --- | ---: | ---: | ---: |
| macOS, 2000 batches | 174 796 | 82 | 1 048 576 |
| Simulator, 2000 batches | 49 567 | 18 | 1 048 576 |
| Simulator, next 2000 | 444 339 | 201 | 1 048 576 |

1 048 576 is exactly 1024x1024: one full readback of the WebGL canvas per
rendered frame, on both. Every bit of the difference is what the host charges
to answer.

### That the charge is the Simulator's, not ours

An independent microbenchmark, no emulator involved: one BGRA8 shared
`MTLTexture`, cleared on the GPU each iteration, then read to CPU memory.

| region | Simulator `getBytes` | macOS `getBytes` | ratio |
| --- | ---: | ---: | ---: |
| 16x16 | 0.153 ms | 0.002 ms | 77x |
| 256x256 | 0.117 ms | 0.014 ms | 8.4x |
| 1024x1024 | 0.260 ms | 0.073 ms | 3.6x |

The Simulator's floor is about 0.13 ms whatever the size and macOS's is about
0.002 ms: the cost is per call, not per byte, because it is one synchronous XPC
round trip. At 132 ms a frame of blocked XPC that is roughly a thousand
reply-requiring Metal calls per Aquarium frame.

### The obvious fix, measured and rejected

ANGLE's Metal backend has a second readback path: with a `GL_PIXEL_PACK_BUFFER`
destination it blits the texture into an `MTLBuffer` on the GPU rather than
calling `getBytes`, and `MTLBuffer` contents are genuinely shared memory in the
Simulator. Routing virglrenderer's readback through a PBO therefore looked like
it would take the payload out of the round trip.

| region | Simulator blit+map | macOS blit+map |
| --- | ---: | ---: |
| 16x16 | 0.439 ms | 0.279 ms |
| 256x256 | 0.402 ms | 0.252 ms |
| 1024x1024 | 0.476 ms | 0.288 ms |

Worse than `getBytes` at every size on both products, because the blit needs its
own command-buffer commit and `waitUntilCompleted` -- another round trip, and a
dearer one. Rejected before any dependency patch was written. Do not retry it.

### Why the guest reads back at all

The readback is not intrinsic to Aquarium. Chromium's GPU process logs, on both
products and on every run examined:

```
ERROR:ui/gl/gl_surface_egl.cc:277] No suitable EGL configs found.
ERROR:ui/gl/gl_surface_egl.cc:380] No suitable EGL configs found for
initialization.
```

The harness launches Chromium with `--use-gl=angle --use-angle=gles-egl`, and
that backend finds no EGL config it can use for a window surface on the guest's
Mesa virgl. Chromium loses accelerated presentation, composites the page in
software, and therefore reads its accelerated 1024x1024 canvas back once a
frame. `--ozone-platform=x11` does not change the error.

`--use-angle=gl` does. On macOS the EGL errors disappear, Chromium reaches
`gl_surface_presentation_helper`, the guest reports `RENDERER=ANGLE (Mesa,
virgl (Apple M1 Ultra), OpenGL 4.1)` instead of `OpenGL ES 3.0`, and the
readback counter stays at **0 reads over 14 000 batches and 2.88 million virgl
commands**. Plateau median of samples 30-110 of each 150-sample run:

| arm | median | mean |
| --- | ---: | ---: |
| `gles-egl`, four baseline runs | 14.0, 14.0, 14.0, 15.0 | 13.70-14.72 |
| `gl` | **17.0** | 16.51 |

The same flag on the Simulator removes the EGL errors and the readback as well
-- `reads=0` -- and then renders nothing, 0 fps for every sample:

```
ANGLE Display::initialize error 12289: Could not create a backing OpenGL
  context.
Exiting GPU process due to errors during initialization
"Uncaught TypeError: Cannot read properties of null (reading 'getExtension')"
```

The guest's GL API is inherited from the host's. macOS gives virglrenderer
desktop CGL, so the guest advertises OpenGL 4.1 and ANGLE's desktop backend has
something to sit on. The Simulator gives it ANGLE GLES3, so the guest can only
offer GLES and the desktop backend has nothing to create. The Simulator is
confined to exactly the `gles-egl` path that cannot find a window config.

### Answer, in order of size

1. **The Simulator's Metal is out of process.** Every reply-requiring Metal call
   is a synchronous XPC round trip, about 0.13 ms against 0.002 ms in process,
   independent of payload. This workload makes on the order of a thousand a
   frame. Not ours to fix.
2. **Chromium has no accelerated presentation surface in the guest**, on either
   product, so it software-composites and reads the canvas back once a frame:
   2 ms a frame on macOS, 107 ms on the Simulator, because it is the largest
   single round trip in the frame. Worth about 19% on macOS; the only fix found
   is unavailable on the Simulator.
3. **The emulated CPU and the GPU itself are not involved.** Time to first frame
   differs by 3.9%, and the fence wait is 12.5 ms a frame against 12.7 ms.

The macOS win is not adopted here. The flag lives in the Aquarium launcher in
`Kernarium/Runtime/KernariumRuntime.mm`, which carried an unrelated concurrent
edit throughout this work, and it must become conditional rather than absolute
because it breaks the Simulator outright. Repairing the `gles-egl` config
failure would help both products and is the open item.
