# Android 17 iOS cold-boot CPU profile (2026-08-25)

## Host-OS trace isolates guest-physical-memory churn (2026-08-26)

Later Xcode System Trace, Virtual Memory Trace, and Activity Monitor captures
supersede the earlier provisional interpretation that the iPhone simply
executes the gadget/MMU mix much less efficiently. The matched two-vCPU run is
not starved of P-core service. Its distinguishing cost is host VM and file
pager activity against UML's sparse, shared, file-backed physical-memory
mapping while the 8 GB phone is under severe memory pressure.

The uninstrumented comparison remains the timing baseline: 136.298 seconds on
the actively cooled iPhone 15 Pro Max versus 58.867 seconds in the M1 Ultra
Simulator, or 2.315x. The new traces use the same accepted release, gadgets,
two vCPUs, 2 GiB, 16 KiB pages, normal boot, hardware Virgl, and a fresh COW.
They explain the gap; their instrumented wall times do not replace it. Virtual
Memory Trace moved the iPhone run to 147.701 seconds and the Simulator run to
117.170 seconds, so the profiler penalized Simulator much more heavily.

### The host scheduler is not withholding the P cores

The corrected physical-device System Trace retained the final 27.757 seconds
through `sys.boot_completed=1`. Its rolling detail buffer overwrote the first
133.79 seconds, so these percentages apply only to the retained late-boot
window. That is nevertheless the window where the uninstrumented device has
its largest convergence penalty.

| Combined two-carrier state | Seconds | Capacity share |
| --- | ---: | ---: |
| Running | 44.272 | 79.749% |
| Blocked | 8.913 | 16.055% |
| Interrupted | 1.385 | 2.496% |
| Preempted | 0.492 | 0.886% |
| Runnable | 0.448 | 0.807% |

The window has 55.515 carrier-seconds of capacity. Runnable plus preempted
time is only 1.693%; runnable p95 is 1.959 microseconds and preempted p95 is
9.333 microseconds. Of the 44.272 running seconds, 43.860 seconds (99.07%) are
on CPU 4 or CPU 5, the two P cores. This directly rules out host run-queue
starvation and meaningful E-core placement for the matched iPhone result.
Blocked time is real, but it is a wait within the guest/host execution path,
not evidence that iOS had a runnable carrier and refused to schedule it.

The same retained window contains 86,747 carrier syscalls, about 3,125 per
second. The largest explicit wait totals are 2.169 seconds in `read`, 0.420
seconds in `pselect`, and 0.106 seconds in `poll`; mapping calls themselves are
not a direct syscall bottleneck (`mmap` totals 5.44 milliseconds and
`mach_vm_map_trap` 0.199 milliseconds). The missing cost is visible in the VM
fault stream rather than as one long blocking syscall.

### The iPhone repeatedly drops and re-faults the guest-RAM mapping

Matched rolling Virtual Memory Trace windows capture the final 16.230 seconds
on iPhone and 17.336 seconds in Simulator. Counts below include only
`uml-vcpu-0-carrier` and `uml-vcpu-1-carrier`, so unrelated application and
system activity is excluded.

| Carrier VM operation | iPhone | Simulator | Device / Simulator |
| --- | ---: | ---: | ---: |
| Page Out | 207,714 / 3.461 GB | 14 / 229 KB | 14,837x events |
| Page Out aggregate duration | 1.097 s | 0.000809 s | 1,356x |
| Page Cache Hit | 153,579 / 2.516 GB | 26,170 / 0.429 GB | 5.87x events |
| Page Cache Hit aggregate duration | 6.526 s | 0.0964 s | 67.68x |
| Page Cache Hit median / p95 | 46.375 / 56.625 us | 2.041 / 8.500 us | 22.72x / 6.66x |
| File-backed Page In median / p95 | 5.625 / 271.458 us | 0.750 / 35.750 us | 7.50x / 7.59x |
| Zero Fill | 17,884 / 0.0220 s | 125,822 / 0.1720 s | Simulator-dominant |

The iPhone's page-cache faults land directly in translated guest memory
operations. Leading stacks include `.Lscalar_unsigned_untagged58111`,
`uml_arm64_gadget_exclusive_store_release_w_rr`, pair stores, and other gadget
loads and stores. They are therefore not incidental UI or profiler faults.
Simulator sees mostly cheap zero-fill work and almost no page-out activity;
the phone continually revisits file-backed pages that are no longer resident.

Activity Monitor independently correlates the VM events with process I/O.
Across the comparable sampled interval, iPhone process logical writes rise by
3,322,937,344 bytes, close to the 3,460,628,480 Page Out bytes despite slightly
different sample boundaries. Simulator writes rise by exactly 229,376 bytes,
the same 14 times 16 KiB reported by its 14 Page Out events. The iPhone process
ledger reaches 37.247 GB (34.69 GiB) of logical writes by the end of this boot
capture, compared with 4.866 MB in Simulator. Instruments' `Page Out` and
logical-write counters do not prove that every byte reached physical flash;
hole deallocation and pager accounting can charge logical I/O without a
same-sized device write. They do prove a radically different VM/file-system
path between the hosts.

Host memory state explains why that path differs. During the iPhone window,
only 1,726--7,064 host pages are free: 28.3--115.7 MB at the measured 16 KiB
host page size. The system has 1.905 GB in its compressor, 665 MB of swap in
use, and 7.91--8.00 GB represented by the used-page count. Kernarium's mapped
resident size oscillates around 2.50--2.60 GB. In Simulator it remains around
4.305--4.344 GB, even though the process physical-footprint counter is only
0.378--0.402 GB. Resident size includes shared mappings and aliases and is not
a unique-RAM charge, but the cross-host difference is exactly the expected
signature: the much larger Mac host keeps substantially more of the mapped
guest-physical-memory working set resident.

### The calls come from ACK free-page reporting

UML creates guest physical memory as a sparse, unlinked temporary file and
maps it `MAP_SHARED`. The host memory layer maps the same descriptor, so gadget
loads and stores ultimately touch that file-backed mapping. ACK topic 31 lets
the guest buddy allocator return free pages by calling `F_PUNCHHOLE` on ranges
of the physical-memory file; the current 16 KiB build reports at order 5, or
512 KiB minimum ranges.

The System Trace resolves this exact call chain. All 28 carrier `sys_fcntl`
calls in the retained window use descriptor 31 and command `0x63` (Darwin
`F_PUNCHHOLE`). Their kernel caller is runtime address `0xe802d90c4`.
Subtracting the `0x280000000` kernel load bias gives linked address
`0xc002d90c4`, 0x1c bytes into `os_discard_file_range` at `0xc002d90a8` in the
accepted release's `System.map`. This excludes UBD COW discard and ties the
host deallocation traffic to free guest physical pages.

The resulting mechanism is a memory-capacity tradeoff, not simply a bad
syscall to delete:

1. Android allocation/free churn reports large runs of free guest pages.
2. `F_PUNCHHOLE` releases those pages from the shared sparse backing so the
   phone does not retain the guest's peak working set.
3. On the memory-constrained iPhone, the host VM and APFS path accounts large
   page-out/deallocation traffic and removes file-backed pages from residence.
4. Android soon reuses part of that physical memory; gadget loads and stores
   then incur the measured slow page-cache faults. The M1 Ultra host can retain
   far more mapping residency, so the same guest observes cheap zero-fill and
   page-cache service with almost no page-out traffic.

### Disabling reporting is a decisive negative control

A fresh uninstrumented iPhone boot added only
`initcall_blacklist=uml_page_reporting_init` to the same guest command line.
The live command line contained it, and both the free-page-reporting enable
messages were absent. The result got much worse:

| Milestone | Reporting enabled | Reporting disabled |
| --- | ---: | ---: |
| APEX activated | 5.933 s | 9.325 s |
| Zygote started | 7.368 s | 29.500 s |
| Boot animation started | 18.534 s | 56.042 s |
| Codec generator started | 54.613 s | 253.987 s |
| Codec generator exited | 99.707 s | 304.665 s |
| Boot complete | 136.298 s | 448.561 s |

The disabled run is 3.291x slower than the enabled control, remains at Nominal
thermal state, and has no RCU stall, panic, watchdog, or OOM. It also starts a
late `artd` job that runs for 157.741 seconds, so the 312-second delta is not a
clean per-fault timing decomposition: extreme memory pressure changes Android
service behavior too. It is nevertheless a decisive product result. Retaining
every touched guest page is substantially worse, and removing free-page
reporting is not a fix.

### Accepted diagnosis and implication

The primary physical-device penalty is the interaction between a
memory-intensive Android boot, UML's sparse shared file backing for guest
physical memory, the ACK free-page reclamation mechanism, and the iPhone's
limited host-memory capacity. It is not explained by the A17 Pro clock,
thermal state, P/E placement, runnable-thread scheduling, extra retired guest
work, translation, or a missing vCPU. The carriers are already using both P
cores; host VM/file-pager work on one shared physical-memory backing cannot be
turned into parallel guest compute by adding vCPUs.

Future optimization should preserve the ability to return free guest pages
while reducing discard/re-fault churn, for example by qualifying batching,
hysteresis, or a different device-suitable backing/reclaim contract. It must
not merely disable page reporting. No emulator, Android image, kernel, or
runtime product changed during this investigation.

The corrected System Trace and its reduced scheduler/syscall analyses are
under
`.build/android/host-os-profile-20260825/iphone15promax-system-trace-2vcpu-window180`.
Matched VM captures and process/system summaries are under
`.build/android/host-os-profile-20260825/iphone15promax-vm-final10-2vcpu` and
`.build/android/host-os-profile-20260825/ios-simulator-vm-final10-2vcpu-restored`.
The no-reporting control is under
`.build/android/host-os-profile-20260825/iphone15promax-no-page-reporting-2vcpu/uninstrumented-cold`.

## Matched two-vCPU P-core control

The current iOS 26 comparison limits the guest to two vCPUs to test whether
the physical-device gap was caused by third and fourth carriers spilling onto
E cores. The connected device named `Google Pixel 8 Pro` is an iPhone 15 Pro
Max (`iPhone16,2`) running iOS 26.5; the Simulator runs on the 16-P-core,
4-E-core M1 Ultra Mac Studio. Both uninstrumented runs used the accepted
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1` release, gadgets, two
vCPUs, 2 GiB, 16 KiB pages, normal boot, point-mode 160 DPI, hardware Virgl,
a fresh COW, and host launch through init's first `sys.boot_completed=1`
action. The iPhone was under user-confirmed active cooling.

| Host | Uninstrumented cold boot | Device / Simulator |
| --- | ---: | ---: |
| iPhone 15 Pro Max, iOS 26.5 | 136.298 s | 2.315x |
| iOS 26.5 Simulator, M1 Ultra host | 58.867 s | 1.000x |

Both logs bring up exactly two guest CPUs with 2,048 MiB and the same ACK
6.18.21 kernel. The device surface is 430 x 839 and the Simulator surface is
744 x 1081, so the physical run renders fewer pixels and that difference does
not explain its longer time. Both runs started at Nominal thermal state and
contain no thermal-change, RCU-stall, watchdog, panic, or crash record.

Two vCPUs limit the number of carrier threads but do not create host-core
affinity. Separate Xcode 26.6 CPU Profiler cold boots therefore measured the
actual physical core for every vCPU cycle sample. These profiles are placement
checks, not replacements for the uninstrumented table:

| Host | Profiled guest boot | P-core cycle share | E-core cycle share | P-core weighted mean |
| --- | ---: | ---: | ---: | ---: |
| iPhone 15 Pro Max | 135.761 s | 98.776% | 1.224% | 3.094 GHz |
| M1 Ultra Simulator host | 61.900 s | 99.982% | 0.018% | 2.937 GHz |

The device trace remained Nominal for its complete 137.083-second interval,
with no public thermal-state transition, while the phone was actively cooled.
The Simulator trace was likewise Nominal throughout. Neither trace reports
lost events or run issues. The iPhone's P-core mean is 5.35% higher than the
M1 Ultra value, and its 1.224% E-core spill is far too small to explain a
2.315x wall penalty. This directly rejects E-core placement, lower P-core
frequency, and thermal headroom as explanations for the matched gap.

The wall-time divergence starts after early Android userspace. The iPhone is
faster through SurfaceFlinger, then loses time in every material convergence
stage:

| Uninstrumented interval | iPhone | Simulator | Device / Simulator |
| --- | ---: | ---: | ---: |
| Launch through SurfaceFlinger | 8.730 s | 13.610 s | 0.641x |
| SurfaceFlinger to boot animation | 9.805 s | 2.384 s | 4.112x |
| Boot animation to codec generator start | 36.078 s | 21.425 s | 1.684x |
| Codec-list generator | 45.095 s | 8.834 s | 5.105x |
| Codec exit to boot-animation exit | 18.951 s | 10.253 s | 1.848x |
| Boot-animation exit to boot complete | 17.639 s | 2.360 s | 7.473x |
| SurfaceFlinger through boot complete | 127.568 s | 45.257 s | 2.819x |

CPU Profiler observes 180.733 billion total vCPU cycle weight on the device
versus 213.381 billion in Simulator, only 84.7% as much cycle weight despite
2.193x profile wall time. The accepted adjacent-full-PMI intervals cover a
similar fraction of vCPU cycles on each host (67.81% and 66.36%) but amount to
only 0.292 sampled on-core carrier-seconds per device boot second versus 0.779
in Simulator. This comparison points to much lower carrier on-core duty or
service during the device's late Android path, not to slower P-core clocks.
CPU Profiler alone cannot divide that off-CPU term between host preemption,
guest dependency idleness, and host waits, and its cycle weight is not a
retired guest-instruction count.

Raw summaries, wall-prefixed consoles, CPU Profiler traces, and reduced
core-residence analyses are under
`.build/android/boot-2vcpu-20260825`. No emulator, Android image, kernel, or
runtime product changed for this measurement.

## Result

The accepted Android 17 guest was CPU-profiled through a fresh-COW cold boot
on the physical iPad and the iOS Simulator. Both products were Release builds
of the fixed host code, used the gadgets engine, 4 vCPUs, 2 GiB of RAM, 16 KiB
guest pages, normal boot, point-mode 160 DPI, and hardware Virgl.

| Host product | Profile wall | Process CPU | Average process cores | Attribution confidence |
| --- | ---: | ---: | ---: | --- |
| iPad mini 6, iOS 18.5 | 303.145 s | 573.213 CPU-s | 1.891 | Time Profiler; 95.916% of carrier CPU classified |
| iOS 26.5 iPad mini Simulator | 153.688 s | 251.115 CPU-s | 1.634 | `sample(1)` fallback; approximate, 0.810 active-sample/CPU cross-check |

## iOS 26 CPU-frequency and throttling follow-up

The connected device named `Google Pixel 8 Pro` is an iPhone 15 Pro Max
(`iPhone16,2`) running iOS 26.5. It was measured with Xcode 26.6 against the
same accepted Android release, gadgets engine, 4 vCPUs, 2 GiB, 16 KiB pages,
normal boot, point-mode 160 DPI, hardware Virgl, and a fresh COW. CPU Profiler
was attached while the app was stopped, the guest was then resumed, and the
recording was stopped at init's first `sys.boot_completed=1` action. Guest boot
at that intrusive boundary took 355.376 seconds; it is not a replacement for
the uninstrumented timing baseline.

The Xcode 26 Power Profiler was tested first. It now works on this iOS 26
device and supplies device thermal state, modeled system power, per-process
subsystem impact, CPU instruction count, and QoS execution. Its export schema
does not contain CPU frequency or cycle data, so a Power Profiler value was
not relabeled as frequency. The Xcode 26 CPU Profiler instead records
`CORE_ACTIVE_CYCLE`, the physical core, one-million-cycle PMI samples, and the
same device thermal track. Frequency was derived only between adjacent full
PMI samples for one `uml-vcpu-*` thread on the same physical core. Scheduler
partials, migration, intervals outside 0.1--5 ms, and implausible rates were
excluded.

| Boot interval | P-core median | P-core p90 | E-core median | E-core p90 |
| --- | ---: | ---: | ---: | ---: |
| First third | 3.56 GHz | 3.73 GHz | 2.08 GHz | 2.11 GHz |
| Middle third | 2.54 GHz | 3.26 GHz | 2.08 GHz | 2.11 GHz |
| Last third | 2.72 GHz | 3.08 GHz | 2.07 GHz | 2.11 GHz |

The P-core median therefore falls 28.7% from the first to middle third and
23.6% from the first to last third. Its p90 falls 12.6% and 17.4%. Stage
results show the same boundary: before APEX the P-core median/p90 is
3.68/3.73 GHz, during `artd` it is 2.54/3.26 GHz, during codec generation it is
2.72/3.26 GHz, and from codec exit to boot-animation exit it is 2.72/3.08 GHz.
Ten-second windows reach an observed 3.77 GHz P-core p90 early, fall mainly to
3.07--3.44 GHz during sustained ART work, and are predominantly 2.90--3.08 GHz
after codec exit. The E-core p90 remains at 2.10--2.11 GHz. CPU Profiler
attributes 83.02% of all vCPU cycle weight to P cores and 16.98% to E cores,
so the P-cluster reduction affects the majority of the measured work.

The filtered set contains 186,728 intervals and 186.232 billion cycles,
covering 51.71% of all vCPU cycle weight. Xcode reported no lost events or run
issues. The observed maxima, 3.7769 GHz on P cores and 2.1092 GHz on E cores,
also bound the calculated distributions. Instruments reports one continuous
Nominal thermal interval over the complete 356.666-second trace, and the app's
public `ProcessInfo.thermalState` record starts Nominal with no change event.

The supported conclusion is therefore two-part. Sustained Android boot does
measurably suppress P-core burst frequency and this reduces performance, but
iOS does not classify the run as thermally constrained. Call it sustained
DVFS/power-envelope behavior, not a Fair/Serious/Critical thermal throttle.
The frequency loss can plausibly add tens of percent to clock-bound P-core
intervals; it cannot by itself explain the prior multi-fold iPad/Simulator
CPU-per-work gap. The local trace, summary, console, TOC, and reduced JSON are
under
`.build/android/cpu-frequency-20260825/iphone15promax-ios26-cold-4vcpu-2g`.

## Retired-work and carrier-CPU follow-up

A later low-overhead run closes the work-volume question left open by the
sampling profiles. A separate `UML_RUNTIME_PROFILING=ON` runtime counted
retired arm64 guest-user instructions and dispatch exits exactly at each
record. Every 4,096 dispatch entries it also sampled the carrier's thread CPU
clock. The latter includes
both emulated-user execution and native UML work performed by that persistent
carrier between guest entries; it is not a hardware-cycle or native-kernel
instruction counter. Native ACK instructions are not included in the retired
guest-instruction count.

Both platforms again used the same accepted release, gadgets, 4 vCPUs, 2 GiB,
16 KiB pages, normal boot, 160 DPI and hardware Virgl. Cold runs removed only
the selected bundle's COW; warm runs immediately reused the resulting COW.
Both runtime slices were built with `-O3`, ThinLTO, and the same
`armv8.2-a+fp16+dotprod+crypto+fcma` ISA target. A focused comparison found the
1,621,680-byte assembled gadget-engine text identical after excluding Mach-O
platform metadata, so this is not a device build using weaker optimization or
instruction selection.

| Boot | Host | Wall s | Retired guest instructions | Carrier CPU-s | Average carrier cores | Carrier ns / guest instruction |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Cold | Simulator | 72.520 | 34,601,170,828 | 183.541 | 2.531 | 5.304 |
| Cold | iPad | 435.979 | 36,370,137,341 | 795.954 | 1.826 | 21.885 |
| Warm | Simulator | 40.854 | 14,035,766,767 | 85.319 | 2.088 | 6.079 |
| Warm | iPad | 167.272 | 14,846,695,715 | 293.979 | 1.757 | 19.801 |

Each row uses the last complete cumulative record from every carrier before the
boot-complete milestone. A record is exact, but sparse emission can omit fewer
than 4,096 later dispatches per carrier. Even if every omitted dispatch retired
the full 65,536-instruction budget, the total residual is below 1.074 billion
instructions. Giving that entire bound to the device and the full per-thread
record-age CPU bound to Simulator still leaves at least a 3.98-fold cold and
2.99-fold warm device carrier-CPU-per-work penalty. The conclusion does not
depend on treating the sparse milestone as a perfect final flush.

The recorded wall ratios have a direct accounting identity. Cold
iPad/Simulator is
`6.012 = 1.051` times the retired work, times `4.126` carrier CPU per retired
instruction, divided by `0.721` relative carrier utilization. Warm is
`4.094 = 1.058 * 3.257 / 0.842`. Only 5.1--5.8% more emulated guest work was
retired on the device. The dominant term is therefore 3.26--4.13 times more
carrier CPU time per stable guest-work unit; 16--28% lower effective carrier
concurrency compounds it.

The engine's cumulative elapsed service time is also much larger than carrier
CPU: cold 1,253.381 versus 795.954 carrier-seconds, and warm 422.491 versus
293.979. Because carrier CPU includes work outside the engine, those
differences are conservative lower bounds: at least 457.427 seconds (36.5%)
of cold and 128.512 seconds (30.4%) of warm summed engine service elapsed while
the corresponding carrier was off-CPU. Physical scheduling/preemption is
material, but the CPU-per-work ratio proves it is not the primary term.

The stage boundary remains workload-sensitive. Warm device and Simulator are
effectively equal through zygote. Afterward, device carrier CPU per instruction
is 2.45 times Simulator through boot-animation start, 3.67 times through codec
generation start, 4.05 times through codec exit, and 3.08 times through boot
completion. This aligns with the earlier symbol profile: real gadget bodies
plus memory/MMU own 79.65% of physical CPU, while translation/cache is 6.46%,
host synchronization 0.20%, and on-CPU I/O 1.01%. At this stage the provisional
interpretation was lower sustained A15 throughput for the gadget/MMU and
large-working-set mix, plus lower carrier concurrency and E-core spill. The
later host-OS trace above supersedes that interpretation: expensive host VM
fault and file-pager service is charged while the carrier executes gadget
memory operations, so an on-CPU symbol profile does not classify it as an
ordinary emulator I/O function.

One diagnostic run that read `CLOCK_THREAD_CPUTIME_ID` before and after every
dispatch is explicitly discarded: it moved iPad cold boot from the prior
355-second range to 411.898 seconds while barely changing Simulator. The
accepted method reads the clock once at carrier baseline and once per 4,096
entries. Its raw summaries and reduced stage comparisons are under
`.build/android/cpu-split-20260825`; the preceding exact retired-work-only runs
are under `.build/android/retired-work-20260825`.

These wall values delimit the profiled samples; they are not replacements for
the uninstrumented boot-timing results. Simulator sampling is particularly
intrusive: the same uninstrumented product previously completed cold boot in
77.500 seconds, while this sampled run took 153.688 seconds. The physical
profile also represents one cold run and has ordinary first-boot variance.

Real gadget execution is the largest CPU owner on both products. It consumes
356.384 CPU-s (62.17%) on the iPad and approximately 128.402 CPU-s (51.13%)
on Simulator. The iPad's next owner is memory/MMU runtime at 100.187 CPU-s
(17.48%). Translation plus software translation-cache lookup and maintenance
consume only 37.035 CPU-s (6.46%) on the iPad. The corresponding approximate
Simulator subtotal is 35.977 CPU-s (14.33%), driven by a larger late-boot
program-cache-maintenance share rather than by translation itself.

From `artd` start through boot completion, the iPad consumes 414.117 CPU-s
(72.24% of its whole-boot CPU) and Simulator consumes approximately 182.575
CPU-s (72.71%). The single largest interval on each product is `artd` start to
codec-list generation start: 228.461 CPU-s (39.85% of total) on the iPad and
94.301 CPU-s (37.55%) on Simulator. The next largest interval is codec exit to
boot-animation exit: 130.100 CPU-s (22.70%) and 50.630 CPU-s (20.16%). The
post-ART Android convergence path, not partition mounting or APEX activation,
therefore owns most cold-boot CPU on both products.

## Whole-boot CPU attribution

| Category | iPad CPU-s | iPad % | Simulator CPU-s | Simulator % |
| --- | ---: | ---: | ---: | ---: |
| Gadget compute/control | 245.990 | 42.91% | 87.477 | 34.84% |
| Gadget memory instructions | 110.394 | 19.26% | 40.925 | 16.30% |
| Translation/decode/build | 19.635 | 3.43% | 7.469 | 2.97% |
| Translation-cache lookup/validation | 9.767 | 1.70% | 8.287 | 3.30% |
| Translation-cache maintenance | 7.633 | 1.33% | 20.221 | 8.05% |
| Memory/MMU runtime | 100.187 | 17.48% | 41.888 | 16.68% |
| I/O/device paths on CPU | 5.775 | 1.01% | 15.357 | 6.12% |
| Native guest-kernel slice | 37.170 | 6.48% | 3.797 | 1.51% |
| Host synchronization | 1.138 | 0.20% | 3.860 | 1.54% |
| Other emulator runtime | 0.999 | 0.17% | 5.251 | 2.09% |
| Unresolved carrier | 23.152 | 4.04% | - | - |
| Host-system active work | 5.081 | 0.89% | 16.430 | 6.54% |
| Support threads | 6.292 | 1.10% | 0.154 | 0.06% |

The rows are mutually exclusive and sum to each product's process CPU. The
following definitions matter when interpreting them:

- Real gadget time is the sum of gadget compute/control and gadget memory
  instructions. These are samples whose leaf instruction is inside a generated
  `uml_arm64_gadget_*` body or its local assembly label; they are not time in
  the executor's outer dispatch loop.
- Gadget memory is the execution of guest load, store, atomic, exclusive, and
  prefetch instruction bodies. Memory/MMU runtime is the separate host work in
  guest address translation, TLB preparation, mapping batches, slow memory
  paths, and bulk memory operations. Their diagnostic memory-related subtotal
  is 210.581 CPU-s (36.74%) on the iPad and approximately 82.813 CPU-s
  (32.98%) on Simulator, but gadget memory remains part of the real-gadget
  subtotal too.
- Translation is instruction decode and program construction. Cache lookup is
  `ProgramCache::find`, stream lookup, validation, and shared-translation
  lookup. Cache maintenance is software program-cache indexing, publication,
  activation, deactivation, retirement, and link-record bookkeeping. It does
  not mean hardware data-cache or instruction-cache flushing.
- I/O/device time is CPU spent executing UBD, Virtio, serial, network, GPU, and
  host `read`/`write`/`pread`/`pwrite` paths. Blocked I/O wait is off-CPU and
  cannot appear as CPU time in this table.
- Native guest-kernel time is execution inside the signed ACK kernel mapping.
  Host-system work is active libc, libsystem, allocation, and similar work not
  assigned to a more specific emulator owner. Support threads cover the app,
  GPU service, console/I/O support, and UI outside the vCPU carriers.

The physical trace records 566.921 of 573.213 CPU-s on the four named vCPU
carrier threads. Thus 98.90% of all app CPU is on the carriers. The profile
averages only 1.891 active process cores despite a 4-vCPU guest, so CPU hot
functions alone do not account for all wall time; dependency chains, idle
periods, and off-CPU waits remain relevant.

## Why the iPad uses more CPU time without using four cores

The 2.283 whole-process CPU-time ratio is almost exactly the product of a
1.972 wall-time ratio and a 1.157 average-active-core ratio:

```text
573.213 / 251.115 = (303.145 / 153.688) * (1.891 / 1.634)
       2.283       =          1.972       *        1.157
```

CPU seconds integrate active cores over wall time; they are not a count of
guest instructions. The iPad therefore accumulated more than twice the CPU
time because the profiled boot stayed alive almost twice as long while using
slightly *more*, not fewer, average cores. The same identity explains the
largest stage ratios:

| Stage | iPad/Simulator wall | Average-core ratio | CPU-time ratio |
| --- | ---: | ---: | ---: |
| Zygote -> boot animation | 5.175 | 0.860 | 4.452 |
| `artd` -> codec generation start | 2.192 | 1.105 | 2.423 |
| Codec exit -> boot-animation exit | 1.925 | 1.335 | 2.570 |
| Whole profile | 1.972 | 1.157 | 2.283 |

These ratios describe the two profiled observations, not an equal-work
microbenchmark. Simulator sampling almost doubled its uninstrumented wall
time, and neither profile counts retired guest instructions. A slower critical
path also keeps time-dependent Android work such as boot animation, service
convergence, and polling alive longer, so equal Android work volume must not be
inferred from CPU seconds.

The physical trace excludes a missing or starved carrier. CPU time is balanced
across the four fixed vCPU threads:

| Carrier | CPU-s | P-core CPU-s | E-core CPU-s |
| --- | ---: | ---: | ---: |
| `uml-vcpu-0-carrier` | 142.597 | 110.012 | 32.585 |
| `uml-vcpu-1-carrier` | 146.976 | 114.892 | 32.084 |
| `uml-vcpu-2-carrier` | 136.898 | 105.599 | 31.299 |
| `uml-vcpu-3-carrier` | 140.450 | 107.372 | 33.078 |

The maximum/minimum ratio is only 1.074. Simulator call-graph sampling is
similarly balanced at 38.056--40.669 active sample-seconds per carrier, and
roughly half of each carrier's snapshots end in the guest idle `pselect` path.
There is no lost vCPU, permanently idle carrier, or single carrier receiving
all guest work.

The actual four-vCPU utilization limit is the runnable work exposed by the
Android critical path:

| Simultaneously running carriers | Physical-iPad wall | Share of profile wall |
| ---: | ---: | ---: |
| 0 | 15.880 s | 5.24% |
| 1 | 100.487 s | 33.15% |
| 2 | 113.846 s | 37.55% |
| 3 | 52.986 s | 17.48% |
| 4 | 19.946 s | 6.58% |

One or two carriers run for 70.70% of the profile; three or four run for only
24.06%. The APEX-to-Zygote interval is the clearest serial boundary: one
carrier runs for 86.79% of its wall time, 97.17% of its carrier CPU lands on a
P-core, and its average is only 1.084 carriers. The most parallel material
interval, `artd` start to codec generation start, averages 2.175 carriers and
has three or more running for 34.23% of its wall time. The approximate
Simulator activity analysis has the same shape, rising from 1.307 sampled
active carriers in APEX-to-Zygote to 2.582 in the corresponding ART interval.

This is not a host global-lock bottleneck. Host synchronization accounts for
only 1.138 CPU-s, or 0.20% of physical process CPU. Translation plus all
software translation-cache work is 6.46%, and on-CPU I/O/device work is
1.01%. The inactive carrier stacks and the stage boundaries instead point to
guest idleness and Android service, Binder, package, and compilation
dependencies that rarely make four independent critical-path tasks runnable
at once.

The A15's heterogeneous topology then makes the available parallelism less
valuable than it is on the M1 Ultra. The iPad has two P-cores and four E-cores;
the Simulator host has sixteen P-cores and four E-cores. Physical carrier CPU
lands 437.875 CPU-s (77.24%) on P-cores and 129.046 CPU-s (22.76%) on E-cores.
When three carriers run, the trace averages 1.946 P-cores and 1.054 E-cores;
when four run, it averages 1.987 P-cores and 2.013 E-cores. Darwin is therefore
filling both P-cores almost exactly when the guest exposes three or four
runnable vCPUs. The third and fourth carriers necessarily spill to E-cores.

Linux sees four symmetric virtual CPUs and cannot communicate which guest task
is on the boot critical path to Darwin. Darwin independently schedules the
four permanent host carriers, so a critical guest task may migrate onto a vCPU
whose carrier is on an E-core. Neither Kernarium's `uml-launcher` Foundation
thread nor the ACK carrier `pthread_create` path sets an explicit QoS or host
affinity. That absence can contribute to the 33.070 E-core CPU-s observed while
only one or two carriers run, but this is just 5.83% of whole carrier CPU. It
does not explain the dominant ratio, and the trace does not support a QoS or
affinity product change without a matched A/B.

ART is also not accidentally configured as single-threaded. The generated
`aosp_kernarium_darwin` product contains none of
`dalvik.vm.{boot-,}dex2oat-{threads,cpu-set}`. `artd` adds `-j` only when one of
those properties is present, while `art/dex2oat/dex2oat.cc` defaults its thread
count to `sysconf(_SC_NPROCESSORS_CONF)`. With four online guest CPUs, the
default is therefore four dex2oat threads. That is a ceiling for the compiler's
parallel phases, not proof that four independent Android critical-path tasks
exist throughout boot.

A focused fresh-COW two-vCPU control does not justify matching the guest to the
two A15 P-cores. Simulator completion moved from 77.500 seconds at four vCPUs
to 102.213 seconds at two, a 31.9% regression, proving useful work exists above
two CPUs on the M1 host. The physical two-vCPU control completed in 321.370
seconds, between the prior 303.145-second profiled and 384.418-second
uninstrumented four-vCPU observations. That spread is dominated by
instrumentation and first-boot variance, so it is neither a speedup nor a
regression result. Both controls brought up exactly two CPUs and completed
without a stall or crash.

This sampling profile originally left work volume versus execution rate open.
The later sparse counters at the top of this document close it: device work is
only 1.051--1.058 times Simulator, while carrier CPU per retired guest-user
instruction is 3.257--4.126 times larger. Android's mostly one- or two-task
critical path, the A15's two P cores, E-core spill, and host preemption lower
effective concurrency further. The supported diagnosis is therefore slower
A15 execution of the full gadget/MMU and memory-intensive Android mix,
compounded by scheduling service—not a missing carrier or extra guest work.
Without matched hardware counters, the evidence cannot attribute that on-CPU
term to one cache level or microarchitectural event.

## Physical-iPad stage breakdown

Percentages below are of each row's process CPU, not of whole-boot CPU.
`Gadgets` combines compute/control and memory gadget bodies. The remainder of
each row is host, support, synchronization, or unresolved work.

| Stage | Wall s | CPU-s | Avg cores | Gadgets | Translation | Cache lookup | Cache maintenance | Memory/MMU | I/O/device | Kernel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Profile start -> kernel banner | 1.300 | 0.647 | 0.498 | 0.0% | 0.0% | 0.0% | 0.0% | 7.9% | 0.5% | 45.3% |
| Kernel banner -> system partitions | 0.126 | 0.074 | 0.585 | 6.8% | 8.1% | 4.1% | 0.0% | 16.2% | 9.5% | 2.7% |
| System partitions -> APEX activated | 5.731 | 10.900 | 1.902 | 39.5% | 4.3% | 1.9% | 1.7% | 32.1% | 2.1% | 6.4% |
| APEX activated -> zygote | 9.985 | 11.165 | 1.118 | 60.1% | 2.1% | 0.8% | 0.5% | 24.9% | 1.0% | 4.7% |
| Zygote -> boot animation | 40.802 | 69.243 | 1.697 | 58.9% | 1.8% | 0.5% | 0.5% | 23.2% | 0.8% | 7.2% |
| Boot animation -> `artd` | 47.622 | 67.067 | 1.408 | 65.9% | 2.6% | 1.9% | 1.5% | 14.9% | 0.7% | 4.9% |
| `artd` -> codec generation start | 104.543 | 228.461 | 2.185 | 67.8% | 2.0% | 1.3% | 1.1% | 16.8% | 1.7% | 4.1% |
| Codec generation start -> exit | 18.506 | 29.030 | 1.569 | 46.7% | 10.0% | 4.6% | 3.4% | 19.0% | 0.4% | 9.5% |
| Codec exit -> boot-animation exit | 60.945 | 130.100 | 2.135 | 59.4% | 5.6% | 2.3% | 1.7% | 14.6% | 0.3% | 9.3% |
| Boot-animation exit -> boot complete | 13.585 | 26.526 | 1.953 | 55.3% | 4.8% | 1.9% | 1.6% | 17.8% | 0.2% | 11.9% |

The iPad's gadget share is 58.9--67.8% through the four long initialization
stages. Translation is only 2.0% in the largest CPU interval. It rises to
10.0% during codec-list generation, but that service is only 5.06% of
whole-boot CPU. Software cache maintenance never exceeds 3.4% in a material
stage. I/O/device execution peaks at 2.1% after partition mounting and is only
1.01% across the whole boot, so it is not a dominant on-CPU iPad cost.

## iOS Simulator stage breakdown

The Simulator percentages are approximate. Five-second sampling windows were
intersected with the wall stages and scaled by 250-millisecond process-CPU
telemetry. Stages shorter than one sampling window, especially the first two
rows, do not have independent category resolution.

| Stage | Wall s | CPU-s | Avg cores | Gadgets | Translation | Cache lookup | Cache maintenance | Memory/MMU | I/O/device | Kernel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Profile start -> kernel banner | 1.573 | 1.069 | 0.680 | 33.1% | 0.7% | 0.8% | 1.3% | 22.5% | 6.9% | 12.0% |
| Kernel banner -> system partitions | 0.144 | 0.093 | 0.647 | 33.1% | 0.7% | 0.8% | 1.3% | 22.5% | 6.9% | 12.0% |
| System partitions -> APEX activated | 9.210 | 12.096 | 1.313 | 33.9% | 2.0% | 1.4% | 3.1% | 25.9% | 6.5% | 7.2% |
| APEX activated -> zygote | 6.045 | 5.613 | 0.929 | 32.5% | 2.3% | 2.0% | 4.3% | 30.3% | 6.0% | 3.9% |
| Zygote -> boot animation | 7.884 | 15.554 | 1.973 | 30.3% | 3.3% | 2.9% | 7.3% | 29.9% | 6.0% | 3.1% |
| Boot animation -> `artd` | 27.576 | 34.114 | 1.237 | 59.3% | 1.9% | 3.7% | 6.6% | 13.0% | 6.7% | 1.2% |
| `artd` -> codec generation start | 47.697 | 94.301 | 1.977 | 65.9% | 1.6% | 2.5% | 4.4% | 10.8% | 6.9% | 1.0% |
| Codec generation start -> exit | 11.609 | 20.870 | 1.798 | 49.9% | 4.4% | 4.5% | 11.9% | 17.0% | 3.2% | 0.5% |
| Codec exit -> boot-animation exit | 31.662 | 50.630 | 1.599 | 37.0% | 5.1% | 4.5% | 14.4% | 20.3% | 5.1% | 1.0% |
| Boot-animation exit -> boot complete | 10.289 | 16.773 | 1.630 | 34.6% | 5.5% | 3.9% | 13.4% | 22.1% | 6.8% | 0.7% |

Simulator's translation share remains close to the physical result: 2.97%
whole-boot and 1.6% in the largest stage. Its program-cache-maintenance share
increases after codec generation begins, reaching 11.9--14.4% in the late
stages. A preliminary Debug-app capture showed the same pattern; the accepted
Release rerun excludes build configuration as its cause. Because Simulator
and device use different sampling mechanisms, however, the 8.05% versus 1.33%
whole-boot cache-maintenance delta is a direction for a future matched
Time-Profiler capture, not yet a precise cross-platform regression metric.

## Measurement setup and method

Both profiles consumed release
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1` and host gadget-cache fix
`051260e3`. Later outer commits before this campaign changed documentation
only. Each `.kern` bundle was copied from the same verified source bundle and
was recursively checked to contain its manifest, command line, and one guest
image with no `userdata.cow` before launch. Each profile created its own COW.
Existing Simulator and physical-device bundle siblings were preserved.

| Property | iPad | Simulator |
| --- | --- | --- |
| App configuration | Release | Release |
| Guest bundle | `AndroidCpuProfileDevice4CPU2G051260e3.kern` | `AndroidCpuProfileSimulatorRelease4CPU2G051260e3.kern` |
| Guest surface | 1133 x 695 | 744 x 1081 |
| Thermal state at launch | nominal | nominal |
| Virgl provider | ANGLE/Metal hardware | ANGLE/Metal hardware |
| Boot completion | first init `sys.boot_completed=1` action | same |

The two surfaces differ by only 2.14% in pixel count, matching the preceding
platform-timing campaign. Both logs confirm 4 CPUs online, 2,048 MiB of guest
RAM, and the expected ACK 6.18.21 kernel.

For the physical device, the app was launched stopped through CoreDevice. A
Time Profiler trace attached to that PID before its first instruction, then the
process was resumed. The trace was exported, process and named-carrier CPU
weights were intersected with wall-prefixed `ttyS0` milestones, and leaf plus
call-path symbols assigned each CPU sample to one mutually exclusive category.
The symbolication gate classifies 95.916% of carrier CPU; 4.084% remains
unresolved and is retained explicitly rather than redistributed.

The current Xcode 26.6 / iOS 26.5 Simulator combination repeatedly disconnected
Time Profiler during the DTServiceHub file-descriptor handshake, including
after short attach gates and a Simulator restart. ETTrace CLI capture was not
available in this environment. The Simulator fallback therefore launched the
Release app stopped, attached `sample(1)` before resume, collected consecutive
five-second windows at a one-millisecond interval, and recorded cumulative
target-process CPU every 250 milliseconds. Known blocked/waiting top frames
were excluded; active symbol shares were scaled to the measured process-CPU
deltas. Active samples account for 0.810 of measured process CPU, which is why
Simulator categories are labeled approximate. The Release rerun replaced an
otherwise complete Debug-app preflight.

The two tools expose different stack detail and treat short intervals
differently. Absolute physical-device categories and broad Simulator owners
are useful; small cross-platform category deltas are not exact A/B evidence.
Elapsed CPU seconds also combine work volume with per-core speed and are not
guest-instruction counts.

Exact log scans found no Darwin signal, kernel panic, oops, RCU stall, lockup,
OOM, EROFS worker, unrecognized-userdata fallback, Virtio-GPU `0x1204` or
`0x1205` response, ART abort, or watchdog signature in either accepted run.

## Retained evidence

Ignored local evidence is retained under:

```text
.build/android/profiling/cold-boot-20260825/
  device-cold/
    cold-boot.trace
    time-profile.xml
    toc.xml
    summary.json
    ttyS0-wall.log
    analysis/{cpu-breakdown.json,cpu-breakdown.md,carrier-concurrency.json}
  simulator-release-cold/
    samples/window-*.sample.txt
    host-cpu.csv
    summary.json
    ttyS0-wall.log
    analysis/{cpu-breakdown.json,cpu-breakdown.md,carrier-activity.json}
  two-vcpu-control/
    device-cold-retry/{summary.json,ttyS0-wall.log}
    simulator-cold/{summary.json,ttyS0-wall.log}
```

For each accepted profile, stage CPU totals and category CPU totals independently
sum exactly to the recorded whole-process CPU. The earlier
`simulator-cold-2` directory is the excluded Debug-app preflight and is not a
reported result.
