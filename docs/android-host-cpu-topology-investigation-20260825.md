# Android guest use of Apple asymmetric CPU topology

## Outcome

The proposed topology contract was implemented locally, qualified on the
physical iPad, rejected on performance, and completely reverted. The current
product again presents every configured vCPU symmetrically and leaves every
carrier at the default host QoS. The qualification campaign explicitly
configured four vCPUs. No experimental ACK topic, host topology source,
runtime capacity argument, or differentiated release remains in the accepted
product.

The prototype proved that the emulator *can* expose Apple performance-core and
efficiency-core classes to Android, but also proved that doing so is harmful
for this boot workload. It implemented the complete proposed contract:

1. discovering the host's ordered performance levels with the public
   `hw.nperflevels` and `hw.perflevelN.logicalcpu` sysctls;
2. assigning every fixed UML vCPU an immutable virtual performance level;
3. giving Linux a normalized capacity for each vCPU, so its ordinary
   capacity-aware scheduler can prefer the stronger virtual CPUs for work that
   does not fit on a weaker one; and
4. giving each persistent Darwin vCPU carrier a QoS request corresponding to its
   virtual level.

The physical device reported two level-0 and four level-1 cores. With the
`1024,1024,512,512` guest profile, an Instruments trace showed vCPU0/1 spending
97.5% of their CPU time on P cores and vCPU2/3 effectively 100% on E cores, so
the best-effort QoS placement worked. The same trace averaged only 1.719
running carriers, however. An interleaved warm-boot sweep measured uniform
controls at 147.644 and 158.415 seconds (153.029-second mean), while capacities
512, 640 and 768 completed in 173.188, 179.015 and 186.556 seconds: regressions
of 13.2%, 17.0% and 21.9%. The single 512 cold boot took 360.523 seconds versus
the nearby 355.008-second uniform observation and supplied no cold win.

The topology policy therefore made Android avoid useful work on the two lower
capacity vCPUs without creating more P-core capacity. Its accepted limit was
already known: A15 has only two P cores, and exact numbered-core binding is not
available through supported Apple APIs. The implementation was removed in
commit `8b143c28`; this document retains the design and evidence only as a
rejected experiment.

Subsequent uniform-product counters found only 5.1--5.8% more retired guest
user work on iPad than Simulator, but 3.26--4.13 times more carrier CPU per
recorded instruction and 16--28% lower effective carrier concurrency. Even the
maximum sparse-report tail leaves at least a 2.99-fold penalty. The device gap
is therefore dominated by host execution rate and scheduling service for the
full Android gadget/MMU working set, not by missing virtual topology metadata.

## Current emulator boundary

After the revert, the current topology is symmetric at both scheduling layers:

- `EmulatorConfiguration.defaultVCPUCount` uses only
  `ProcessInfo.processorCount`.
- The Kernarium runtime accepts only a total `guest_processor_count` and emits
  `ncpus=N`.
- ACK topic 13 creates one named, persistent pthread per UML CPU, including
  CPU 0. It knows the vCPU index at thread creation but applies no QoS or
  affinity policy.
- The UML topology header gives each CPU a distinct core ID in one package,
  but does not override `arch_scale_cpu_capacity()`. Linux therefore sees
  capacity 1024 for every vCPU.

That model explains the physical-profile boundary recorded in G175. Four
carriers were alive and balanced, but Linux had no way to know that two A15
P-cores and four E-cores sat behind them, and XNU had no indication that a
carrier held a guest task Linux considered performance-critical.

The current fixed-carrier design is a good implementation seam. A vCPU keeps
the same pthread for its lifetime, so a performance-level assignment remains
stable even while Linux tasks migrate between vCPUs.

## What Apple exposes publicly

Apple documents the following read-only topology discovery sequence for
`sysctlbyname(3)`:

- `hw.nperflevels` is the number of general-purpose core types;
- level 0 is the highest-performance type;
- `hw.perflevelN.logicalcpu` is the number of currently enabled logical cores
  of level N; and
- `hw.perflevelN.logicalcpu_max` is the maximum number at that level.

The same selectors are declared by the macOS and iPhoneOS SDK 26.5 headers.
The current M1 Ultra acceptance host returned two levels, with 16 logical CPUs
at level 0 and four at level 1. The prototype recorded two level-0 and four
level-1 cores on the target iPad. Discovery remained dynamic; no device-name
table was introduced.

Apple also supports setting a pthread QoS at creation with
`pthread_attr_set_qos_class_np()` or on the running pthread with
`pthread_set_qos_class_self_np()`. XNU's published Edge scheduler design says
that QoS participates in per-cluster placement recommendations on asymmetric
systems. It is nevertheless a workload-importance request, not a physical
core selector.

There is no supported `pthread_setaffinity_np()` or `sched_setaffinity()` in
the Apple SDKs. Mach's `THREAD_AFFINITY_POLICY` is documented in XNU's public
header as experimental and as a hint that equal tags share an L2 cache “if
possible.” It neither names a CPU nor selects a performance level. It cannot
satisfy the requested predictability and should not be used here.

Authoritative references:

- [Apple: Determining system capabilities](https://developer.apple.com/documentation/kernel/1387446-sysctlbyname/determining_system_capabilities)
- [Apple: Prioritize Work with Quality of Service Classes](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/PrioritizeWorkWithQoS.html)
- [Apple XNU: Clutch/Edge scheduler design](https://github.com/apple-oss-distributions/xnu/blob/main/doc/scheduler/sched_clutch_edge.md)
- [Apple XNU: thread affinity policy](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/thread_policy.h)

## Guest scheduler mechanism in ACK 6.18

ACK already contains the non-EAS capacity-aware scheduling path needed for a
first implementation. `drivers/base/topology.c` owns a per-CPU scale,
initializes every CPU to `SCHED_CAPACITY_SCALE` (1024), provides
`topology_set_cpu_scale()` and `topology_get_cpu_scale()`, and exports the
result as `cpuN/cpu_capacity` in sysfs.

The local UML topology header can map `arch_scale_cpu_capacity()` to
`topology_get_cpu_scale()`. An early UML command-line parser can then validate
and install one capacity per possible vCPU with `topology_set_cpu_scale()`
before scheduling domains are built. When ACK scans the completed domains it
detects unequal capacities and enables the asymmetric-capacity domain flags.
The fair scheduler's fit and misfit-load logic can then move a sufficiently
large task from a lower-capacity CPU to a higher-capacity CPU.

This path does not require CPUFreq, an Energy Model, schedutil, utilization
clamping, or loadable modules. Those mechanisms remain disabled. In
particular, Energy-Aware Scheduling predicts energy rather than merely
capacity and requires a real energy/performance model plus frequency behavior
that UML does not provide. Inventing those inputs would give the guest a false
hardware contract.

Linux documents the normalized capacity scale and the architecture hook in
[Capacity Aware Scheduling](https://docs.kernel.org/6.10/scheduler/sched-capacity.html).
The additional model and frequency requirements that make EAS unsuitable are
documented in
[Energy Aware Scheduling](https://docs.kernel.org/6.13/scheduler/sched-energy.html).

## Experimented local contract (rejected)

Represent topology as ordered performance levels rather than a P/E Boolean.
Apple already permits more than two levels, and level numbers carry the only
portable ordering guarantee.

At guest launch, the runtime would:

1. read the enabled logical-CPU count for every performance level;
2. validate that the values are nonzero where used, sum consistently, and can
   cover the requested guest vCPU count;
3. allocate vCPU IDs consecutively from the highest level down, always placing
   vCPU 0 in level 0;
4. pair each assigned level with an explicitly configured, measured capacity,
   normalized so level 0 is exactly 1024 and lower levels are in `1...1023`;
5. append an immutable versioned UML argument describing that per-vCPU
   capacity/profile alongside `ncpus=`; and
6. retain the same profile for the complete guest run.

For the current four-vCPU cases this policy produces:

| Host topology | Guest vCPU assignment |
| --- | --- |
| A15-class device, 2 highest + 4 lower | vCPU 0--1 highest, vCPU 2--3 lower |
| M1 Ultra, 16 highest + 4 lower | vCPU 0--3 highest; guest remains symmetric |
| One reported performance level | all vCPUs uniform at 1024 |

The runtime should use current enabled counts for the launch decision and may
record maximum counts for diagnostics. If the public queries fail, the levels
are inconsistent, or enabled capacity cannot cover the request, it must omit
the profile and retain today's uniform 1024/default-QoS behavior. It must not
guess from a machine identifier, cache size, or maximum-frequency key.

The ACK-side argument parser must be fail-closed and atomic:

- absent argument means every CPU is 1024;
- the vector length must equal `ncpus`;
- each value must be in `1...1024`, the maximum must be exactly 1024, and the
  vector must be nonincreasing by vCPU ID;
- any malformed value rejects the whole profile before changing a CPU; and
- the accepted vector becomes read-only before scheduling domains are built.

The same accepted profile would drive carrier QoS. A reasonable experimental
mapping is `QOS_CLASS_USER_INITIATED` for level 0 and `QOS_CLASS_UTILITY` for
lower levels. `USER_INTERACTIVE` is inappropriate for persistent emulator
compute, and `BACKGROUND` adds resource policy and throttling that confound
the capacity experiment. These QoS values are a prototype choice, not an
accepted performance policy.

CPU 0 exists before the ordinary secondary-CPU bring-up, so implementation
must explicitly verify initialization order. The early parser can apply CPU
0's QoS to the current carrier after it accepts the complete vector; each
secondary carrier can apply its indexed QoS before it enters Linux execution.
Before installing any capacity, the parser should preflight a pthread attribute
for every requested QoS class and set CPU 0's policy. A preflight or CPU 0
failure rejects the whole profile, leaving uniform capacity and default QoS.
After capacities become visible, a failure to apply an already-preflighted
policy to a secondary carrier must fail that CPU's bring-up instead of silently
running one half of a mismatched profile.

The direct Apple pthread imports can remain inside the Android ACK Darwin UML
topic. They do not require a host/kernel bridge-ABI revision. The Apple runtime
only constructs the argument from public sysctls; Android kernel changes stay
as generated local patches under `kernel/android-6.18`, and no AOSP image
rebuild is involved.

## Capacity calibration result

Apple exposes counts and ordering, not a normalized throughput ratio. Cache
size is not CPU capacity, and a nominal frequency—where available—would omit
microarchitectural work per cycle. The repository therefore has no defensible
automatic value for level 1. The measured 512/640/768 sweep rejected all three
tested values for Android boot rather than selecting one.

The prototype therefore made lower-level capacities an explicit local
experiment parameter and swept plausible values rather than accepting one by
inspection. Its Instruments capture correlated each carrier with the physical
core class and showed a strong placement separation. Retired guest work was
counted in the subsequent uniform-runtime investigation, after the topology
policy was already rejected; no tested topology arm warranted further
calibration.

A runtime self-benchmark is not recommended. It would delay every launch,
change thermal state before Android boot, and still could not force its worker
onto a particular core class. A per-model table is also rejected: it would be
an unmaintained private hardware database and conflicts with the project's
dynamic-capability policy.

## Expected benefit and observed limit

On the physical four-vCPU guest, a runnable task that Linux considers too
large for vCPU 2 or 3 can be migrated to vCPU 0 or 1, whose carrier asks XNU
for the higher service class. This addresses the two-scheduler mismatch found
in G175 when Android has at least three runnable carriers.

It cannot accelerate the serial part of boot. APEX-to-Zygote was one-carrier
for 86.79% of its wall time and that carrier already spent 97.17% of its CPU
time on a P-core. It also cannot reserve both P-cores for the app, prevent
other system work from competing, or guarantee that a high-level carrier
never runs on an E-core. The predictable contract is the vCPU's virtual class
and requested host policy, not its momentary physical CPU number.

## Historical implementation and acceptance gates

No implementation should be accepted without all of the following:

1. **Discovery tests.** Test a sysctl reader through injected one-, two-, and
   three-level fixtures, absent/malformed keys, changing enabled counts, and a
   request larger than the discovered enabled total. Tests must not depend on
   the developer Mac's topology.
2. **Parser tests.** Prove absent input preserves uniform capacity and malformed
   length, range, maximum, or ordering rejects the profile atomically.
3. **ACK replay/build.** Regenerate the local ACK topic, replay and build it on
   `x86-builder`, retain 16 KiB and `CONFIG_MODULES=n`, and verify the accepted
   direct Apple imports. Do not rebuild AOSP.
4. **Guest topology.** Verify every
   `/sys/devices/system/cpu/cpuN/cpu_capacity`, scheduling-domain asymmetric
   capacity flags, and a controlled misfit-task migration from a lower to a
   higher virtual CPU.
5. **Host placement.** Use a physical-device Instruments trace to show the
   actual P/E residence of every named carrier. Report all migrations and
   spill; do not describe QoS as affinity.
6. **Matched A/B.** Interleave topology-off and topology-on cold and warm boots
   on the iPad with the accepted 4-vCPU, 2-GiB guest. Repeat the same tuple on
   Simulator and macOS, where four vCPUs should remain uniform on the current
   16-P-core host.
7. **Correctness.** Require boot completion, post-boot dwell and orderly
   shutdown with no RCU stall, lockup, panic, Darwin signal, or timer/carrier
   regression. Record thermal state and actual carrier core-class residence.

Acceptance requires a repeatable physical-device improvement in the parallel
stages without a serial-stage regression or stability loss. If QoS does not
produce a useful placement separation, or the result is within run variance,
remove the differentiated policy and keep the current uniform behavior.

## Rejected alternatives

- **Exact numbered-core pinning:** no supported Apple API exists on the target
  products.
- **Mach affinity tags:** experimental L2-co-location hints, not CPU or cluster
  binding.
- **Private XNU thread-policy or processor-set calls:** unsupported and not an
  acceptable iOS product dependency.
- **Hard-coded device/core tables:** stale, model-specific, and unnecessary
  when public runtime topology discovery exists.
- **Cache-size or nominal-frequency capacity:** not a throughput measurement.
- **Fake CPUFreq, Energy Model, EAS, or uclamp:** supplies semantics the UML
  host cannot honor.
- **Changing carrier QoS without guest capacities:** leaves the two schedulers
  disagreeing about which vCPU is stronger.
- **Guest capacities without carrier policy:** advertises a distinction that
  XNU has no reason to preserve.
