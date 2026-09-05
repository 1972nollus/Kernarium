# Optimization history

This is the repository's single history of attempted performance work. Entries
retain only the hypothesis, disposition, and durable lesson. They do not define
current behavior; current contracts live in architecture and subsystem docs.

`Accepted` means the useful mechanism remains or its result became product
policy. `Rejected` means the candidate was removed or disabled. `Measured`
means the work improved understanding without justifying a product change.

## Early gadget and MMU trials

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| Trial 1 | Preserve gadget caches across address-space switches | Rejected; mixed results did not improve the aggregate. |
| Trial 2 | Share immutable gadget programs across vCPUs | Rejected; shared-miss synchronization overwhelmed short parallel work. |
| Trial 3 | Fixed set-associative cache with slab storage | Accepted; stable bounded storage removed full-cache clear cliffs. |
| Trial 4 | Data-only successor links | Accepted; cached blocks can link without executable publication. |
| Trial 5 | Specialized CMP/branch gadgets | Rejected after a marginal, workload-specific result. |
| Trial 6 | Reduce syscall-boundary state copying | Accepted; persistent per-vCPU state removed redundant frame work while preserving migration semantics. |
| Trial 7 | Batch cold fetches and pool translated programs | Accepted; cold process and translation workloads improved. |
| Trial 8 | Larger gadget cache and translation window | Rejected; capacity was not limiting and the larger window did extra work. |
| Trial 9 | Asynchronous Darwin UBD dispatch | Rejected as a performance change; correctness worked but cached benchmark work did not benefit. |
| Trial 10 | Keep guest NEON values in host registers | Rejected; the target workloads rarely used the selected regions. |
| Trial 11 | Vectorize bulk sparse-MMU translation | Rejected; sparse lookup and cached cross-page work dominated the vector setup. |

## Geekbench-guided gadget work

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G12 | One translation for same-page SIMD pairs | Accepted. |
| G13 | One translation for same-page integer pairs | Accepted. |
| G14 | Native host SIMD pair transfers | Accepted. |
| G15 | Native host integer pair transfers | Accepted. |
| G16 | Remove impossible scalar-byte page crossing | Accepted. |
| G17 | Direct ADD/SUB-immediate gadget addressing | Rejected; instruction reduction alone did not improve runtime. |
| G18 | Inline immutable direct-link records | Accepted. |
| G19 | Inline indirect-link cells | Accepted. |
| G20 | Pool link-map nodes | Rejected; added ownership cost without a useful gain. |
| G21 | Longer linked-chain checkpoint | Rejected. |
| G22 | Lightweight production builder callbacks | Rejected. |
| G23 | Combine scalar page and TLB checks | Rejected. |
| G24 | Predecode one-table SIMD lookup offsets | Accepted. |
| G25 | Predecode indexed floating-point vector offsets | Accepted. |
| G26 | Predecode basic vector floating-point offsets | Accepted. |
| G27 | Predecode three-same SIMD offsets | Accepted. |
| G28 | Predecode pairwise-long SIMD offsets | Rejected. |
| G29 | Predecode widening and high-narrow SIMD offsets | Accepted. |
| G30 | Dispatch-scoped native floating-point environment | Accepted. |
| G31 | Single link-map lookup | Accepted. |
| G32 | Bounded initial program reserve | Accepted. |
| G33/G33b/G33c | Cache active-link pointers | Rejected; lifetime and invalidation costs outweighed lookup savings. |
| G34 | Retain direct-link map nodes | Rejected for the same lifecycle pressure. |
| G35 | Branch-free logical MOV alias | Accepted. |
| G36 | Reuse ADD-immediate register-bank base | Rejected. |

The workload profile behind these trials showed that memory translation was
the broadest gadget cost, cold translation mattered most for compiler-style
work, and several image workloads were dominated by pair-vector transfers.
The detailed per-workload sample matrix was intentionally discarded.

## Page size and native JIT campaign

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G37 | Make 16 KiB the default guest page profile | Accepted; multi-core gains outweighed a small single-core cost, with 4 KiB retained for compatibility. |
| G38-G41 | Smaller, one-way, wider, and folded 16 KiB TLBs | Rejected; direct-index conflicts or extra index work remained. |
| G42 | Two-way 16 KiB gadget TLB | Accepted; it removed deterministic alias conflicts while keeping a short primary hit path. |
| G43 | Strict native ARM64 JIT with host-safe feature exposure | Accepted. Pair-memory fast paths were separately rejected after instability or regression. |
| G44 | Reduce generated-code expansion and validate stale blocks exactly | Retained; block-local register reuse and exact saved-word validation became the durable direction. |
| G45 | Stateful native links and compact block boundaries | Accepted. |
| G46 | Deferred architectural state with one-to-one native shapes | Accepted. |
| G47 | Replace reservation serialization with a mutex | Rejected; convoy behavior worsened the gate. |
| G48 | Native remapping and cold translation cleanup | Accepted where code shape and semantics stayed exact. |
| G49 | Exact constants and constrained vector prebinding | Accepted. |
| G50 | Plan consecutive vector-list registers at translation time | Accepted. |
| G51 | Select resident GPRs from observed block use | Accepted. |
| G52 | Compare JIT and gadgets under matched Geekbench | Measured; the JIT won serial work but exposed a global publication bottleneck in several parallel workloads. |
| G53-G79 | Scale publication and recover gadget regressions | Accepted; ordinary stores became lock-free while uncommon reservation work retained synchronization. |
| G80-G84 | Shared templates and alternate flat JIT caches | Rejected; ownership and working-set costs were not justified. |
| G85 | Diagnose a JIT layout regression | Measured; large inline tables, not vCPU scheduling, caused the regression. |
| G86-G88 | Restore compact cache hashing and descriptors | Accepted. |
| G89 | Limit translated blocks to eight instructions | Rejected; short blocks sharply regressed compiler work. |
| G90-G92 | Heap-backed tables and construction-time verification selection | Accepted; large per-vCPU initialization and production polling were removed. |
| G93 | Always fetch the full translation window | Rejected; early termination remained valuable. |
| G94 | Replace direct opcode checks with a lookup table | Rejected by a matched control; direct checks stayed faster. |
| G95 | Final direct-check and desktop graphics baseline | Measured; confirmed the retained JIT state and removed the earlier scheduling concern. |

## Gadget-first and cache-budget campaign

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G96 | Define matched performance and code-layout controls | Accepted as campaign policy; later conclusions use focused controls rather than one noisy score. |
| G97 | Hash page addresses across four batch-planner buckets | Accepted. |
| G98 | Lower guest YIELD to host ISB in gadgets | Rejected; neutral aggregate and negative focused result. |
| G99 | Enable interprocedural optimization for gadget code | Accepted. |
| G100 | Compose five decode families in one translation unit | Accepted; this remains the audited `.ipp` exception. |
| G101 | Resolve the address space once per thread | Accepted. |
| G102 | Add ISB at JIT gate sites | Initial rejection withdrawn by G108 because the benchmark band was not established. |
| G103 | Resolve the per-thread execution object once | Accepted. |
| G104 | Use BFI/NEG for two gadget lowering patterns | Accepted on focused controls. |
| G105 | Measure host issue ratios | Measured; used to rank later changes. |
| G106 | Fold the vector-bank base into load displacement | Rejected; smaller code did not become faster. |
| G107 | Hoist successor fetch | Accepted for the bounded safe family; broader polymorphic claims were not made. |
| G108 | Establish the JIT noise band | Measured; retracted G102's unsupported rejection. |
| G109 | Validate stale blocks in bulk | Accepted. |
| G110 | Measure gadget-bank size headroom | Measured; prevented size assumptions from driving changes. |
| G111 | Fuse CMP-immediate with B.cond | Accepted on a focused probe. |
| G112 | Generalize fusion to destination-writing SUBS | Rejected. |
| G113 | Measure adjacent instruction pairs | Measured; used to choose later fusions. |
| G114 | Fold ADRP plus ADD | Accepted. |
| G115 | Remove NOP entries | Not implemented; the evidence contract did not justify it. |
| G116 | Profile instruction pairs over a full workload | Measured; corrected the NOP and conditional-pair priorities. |
| G117 | Fuse register CMP with B.cond | Accepted; further fusion had a small ceiling. |
| G118 | Replace the MMU miss structure with a radix table | Rejected before implementation after measurement disproved the premise. |
| G119 | Remove the mapping lock from the miss path | Rejected; it produced no useful change. |
| G120 | Isolate lifecycle-lease cost | Measured; identified the actual miss-path target. |
| G121 | Replace the lease with a published per-thread access slot | Accepted. |
| G122 | Use a FIFO ticket turn | Rejected as neutral. |
| G123 | Serialize Darwin kernel mappings and repair whole host slots | Accepted as a correctness repair; no performance claim. |
| G124 | Establish a post-migration baseline | Measured; an earlier PSI conclusion was withdrawn. |
| G125 | Expose the host-safe guest feature surface | Accepted; vector workloads had been feature-starved. |
| G126 | Requalify the feature surface end to end | Accepted. |
| G127 | Investigate Text Processing scaling | Measured; the workload itself exposes limited parallelism, so no emulator change followed. |
| G128 | Bound the possible value of a fixed vector bank | Measured in the JIT. |
| G129 | Try gadget-shaped fixed-bank routes | One route was rejected and one required better instrumentation. |
| G130 | Measure a fixed gadget vector bank | Measured; retained only as a possible bounded default-off prototype. |
| G131 | Profile the JIT store gate | Measured; score-only evidence could not settle the ISB idea. |
| G132 | Make the publication store scan lock-free | Accepted. |
| G133 | Greatly enlarge JIT cache and arena | Superseded; it fixed thrash but used an unbounded per-executor shape. |
| G134 | Share JIT translations between executors | Retained for bounded reuse and ownership; the measured throughput result was neutral to negative. |
| G135 | Derive JIT cache geometry from one process budget | Accepted; corrected G133. |
| G136 | Bound shared storage, reporting, and executor-pool state | Cache and reporting foundations retained; executor-pool performance A/Bs were rejected. |
| G137 | Carry guest x8/x9 across JIT links | Focused-accepted but left default-off; no broad workload claim. |
| G138 | Cap private JIT state and use compact link anchors | Accepted. |
| G139 | Sweep smaller private-cache ceilings | Rejected below the retained 64 MiB ceiling. |

## iOS, graphics, and later gadget work

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G140 | Keep a bounded SIMD destination resident in gadgets | Accepted only for the proven nonfaulting family. |
| G141 | Reuse stale gadget programs after exact word validation | Accepted. |
| G142 | Share decoded gadget templates | Closed before implementation; the expected gain did not justify executable/code complexity. |
| G143 | Profile gadget-engine time by category | Measured; guided the next candidate set. |
| G144 | Ten post-profile gadget candidates | Rejected; none beat controls without unacceptable cost. |
| G145 | Requalify common iOS-style memory and persistent carriers | Accepted architecture; a separate SDOT residency arm remained rejected. |
| G146 | Requalify the 4 KiB high-address profile | Measured; no product change. |
| G147 | Profile completed workloads on physical iPad | Measured; host service rate, not missing carriers, dominated. |
| G148 | Compare page profiles with four vCPUs | Measured; no product change. |
| G149 | Repeat slow iPad workloads | Measured; confirmed device-specific throughput limits. |
| G150 | Check a 1.5 GiB physical-iPad workload subset | Measured as a fit result only; it did not establish desktop capacity or a speedup. |
| G151 | Compare macOS and iPad regression traces | Measured; no product change. |
| G152 | Compare equal guest instruction throughput across hosts | Measured; rejected a simple instruction-latency explanation. |
| G153 | Run equal-work vector-store probes | Measured; rejected the HDR vector-store hypothesis. |
| G154 | Vary vCPU count for iOS KDE bring-up | Measured; startup was limited by available guest CPUs rather than translation. |
| G155 | Move the KDE text getty off the compositor VT | Accepted as a narrow launcher compatibility fix. |
| G156 | Compare WebGL Aquarium at matched resolution | Measured; Simulator presentation was slower, with no immediate product change. |
| G157 | Try JIT for Simulator Aquarium | Rejected; it was slower and did not address the dominant wait. |
| G158 | Memoize gadget link records during reactivation | Accepted. |
| G159 | Reset only activated gadget links | Rejected; the removed work was too small to matter. |
| G160 | Keep several tagged address spaces in one gadget cache | Accepted; whole-cache switching was removed. |
| G161 | Share gadget translations across executors | Mechanism retained as bounded data sharing, but the speed/memory optimization claim was rejected: adopting tiny programs cost more than rebuilding them. |
| G162 | Inline a conditional branch's fall-through | Accepted and materially beneficial; broader noncontiguous continuation was rejected. |
| G163 | Inline one direct-call callee | Accepted with one site per callee. |
| G164 | Recheck cache and block-length work on Simulator | Measured; confirmed the retained direction. |
| G165 | Profile Simulator Aquarium cache and CPU time | Measured; boot was cache/CPU-sensitive while steady frames were host-round-trip-limited. |
| G166 | Add or fund a 512 MiB shared gadget tier | Rejected in both forms. |
| G167 | Give each address space owned cache state | Accepted; retirement became proportional to owned entries. |
| G168 | Recheck G167 on Simulator | Measured; confirmed startup benefits without a steady-frame claim. |
| G169 | Cache the Darwin current-vCPU lookup | Accepted on the hot-path profile, not as a frame-rate claim. |
| G170 | Decouple guest completion from host presentation | Retained as a correctness/latency boundary; it was performance-neutral and retracted an occlusion-biased native-display result. |
| G171 | Attribute the Simulator/macOS Aquarium gap | Measured; synchronous Simulator framework round trips dominated, and the obvious batching change was rejected. |
| G172 | Reduce UI-thread presentation work | Claim-before-drawable ordering accepted; fallback-buffer rotation and a separate presenter thread were not justified. |
| G173 | Bulk-own gadget generation allocations | Accepted; removed allocator-heavy Android boot stalls. |
| G174 | Rebind link state when a cache slot changes owner | Accepted correctness fix. |
| G175 | Profile Android boot carriers and dependencies | Measured; ART/dependency work and A15 host service dominated, not missing carriers. |
| G176 | Prototype asymmetric virtual CPUs | Feasible, but exact Apple core binding was unavailable through supported APIs. |
| G177 | Qualify asymmetric topology | Rejected; it reduced useful scheduling capacity and regressed boot. Uniform vCPUs remain current. |
| G178 | Measure a no-exit gadget ceiling | Measured on the default 16 KiB profile; a matched dependent ADD-immediate stream sustained 15.4% of native macOS throughput and 15.5% of direct arm64 Linux execution in the Debian VM, with no executor exits. By conservative outer wall timing, a real one-vCPU production-gadgets guest delivered approximately 98–100% of that rate across the repeated comparisons, while the matched JIT guest reached approximately 87–88% of native macOS. This is a best-case scalar ceiling, not a whole-engine or workload-independent ratio. |
| G179 | Hoist native FP-environment activation to program entry | Rejected. Removing the guard from every native-FP gadget cut about 61 KiB of executable code, but a 6.4-billion-operation focused run measured paired effects of opposite sign and effectively zero mean. Independent guard work overlaps the threaded-dispatch dependency chain, so neither instruction count nor bank shrinkage alone establishes a gain. |
| G180 | Bias linked-chain retirement accounting | Accepted. A signed counter bias replaced each linked edge's compare-and-branch with one sign-bit test and reduced gadget text by 272 bytes. Two seven-sample focused ABBA pairs improved a 300-million-link loop by 4.9% and 5.7%; full Geekbench 6 and four-vCPU Aquarium ABBA plus anomaly replays found no repeatable broad regression after excluding externally contended samples. |
| G181 | Add a generation-shared target-keyed tier after the two-way indirect-site cache | Rejected. A long focused polymorphic probe improved by about 45% while a monomorphic control was neutral, and four-vCPU Aquarium was neutral. Load-qualified Geekbench ABBA nevertheless found repeatable 4–7% single-core losses in general integer workloads. An extra lookup and 64 KiB data tier on residual misses is too broad a cost even when the primary two-way hit path and gadget-bank size stay effectively unchanged. |
| G182 | Retire only links whose translated code overlaps a sustained code-change range | Rejected. A bounded range history and demand-created reverse index improved repeated seven-sample invalidation probes by 14–15%, but the concurrency-safe implementation repeatedly lost about 6–8% in load-qualified four-vCPU Aquarium closing pairs and did not establish a clean Geekbench pass. Correctness requires coherent history publication and ring reuse; focused invalidation gains did not offset the broader cost. The conservative whole-reset behavior remains. |
| G183 | Outline repeated TLB-miss helper tails | Rejected. Sharing 3,268 cold tails cut gadget text by 88,976 bytes (5.45%), but two seven-sample hot-memory ABBA pairs had opposite signs and effectively zero mean, while a clean long all-miss pair regressed by 1.2%. Bank shrinkage alone did not pay for another taken call and return on misses. |
| G184 | Schedule conditional-branch NZCV restoration before independent stream loads | Rejected. Two seven-sample, three-billion-iteration linked-loop ABBA pairs measured -0.16% and +0.45% wall-time effects with no code-size or layout change. The system-register write did not expose useful overlap on the tested host. |
| G185 | Forward native NZCV into an adjacent conditional branch through shared interior entries | Rejected. The fast entries added no gadget text and correctly skipped one system-register write after a flag-producing ADD/SUB, but two seven-sample, three-billion-iteration ABBA pairs measured -0.21% and +0.73% wall-time effects, or a 0.26% regression on block means. Dispatch and stream-load latency hid the removed write even in the maximally favorable loop. |
| G186 | Keep the scalar top-byte address mask in one shared callee-saved register | Rejected. Computing the mask once removed one hot instruction per ordinary scalar transfer and shrank gadget text by 2,832 bytes without adding variants. Two load-clean seven-sample pairs of a four-billion-transfer GPR-memory probe measured 0.09% and 0.20% regressions. The address-state load and predicted branch overlapped the longer TLB dependency chain, so their removal did not raise throughput. |
| G187 | Put the scalar software-TLB way-zero hit in fall-through order | Rejected. The layout-only transform added no entries, instructions, or gadget-bank bytes, but two load-clean seven-sample pairs of a four-billion-transfer way-zero probe had opposite signs: a 0.82% regression and a 0.75% gain, leaving block means 0.03% slower. The existing predicted taken branch is not a throughput limiter. |
| G188 | Hoist scalar-memory successor fetch across address translation | Rejected. Fetching the successor before the address/TLB chain added no variants or bank bytes. Repeated seven-sample four-billion-transfer probes reduced runtime by 16–17%, including after G182 was removed. Full one-iteration Geekbench ABBA and targeted multi-core repetitions did not reproduce the apparent image-workload regressions, but load-qualified four-vCPU Aquarium comparisons repeatedly lost about 5–8% and further noisy replays did not establish a reliable broad pass. A hot-memory latency gain did not generalize merely by preserving bank size and entry layout. |

## Android boot and memory investigations

| Attempt | Disposition and lesson |
| --- | --- |
| Matched cold/warm platform timing | Early Android startup was not the bottleneck; first-boot ART and package verification dominated. Exact one-off timing tables were not retained. |
| macOS vCPU sweep at 4 GiB | Measured. Eight uniform vCPUs were the boot-time sweet spot on the M1 Ultra host: near-best typical cold boot with much tighter cold-run spread, the best warm median, and no benefit from increasing to ten. |
| Physical-iPad vCPU sweep at 1792 MiB | Measured. Four uniform vCPUs were the viable boot-time sweet spot on the 4 GiB A15 iPad: cold/warm medians were about 79.9/44.0 seconds versus 87.5/47.9 seconds with two. Six vCPUs was not viable at this RAM allocation; iPadOS terminated all four cold attempts under memory pressure before boot completion. |
| Build-time ART placement | Accepted. Ordinary application artifacts are installed beside their APKs instead of being routed to an unbuilt `system_other` image. APEX-contained apps remain a separate platform boundary. |
| ext4 Android payloads and APEXes | Accepted for the Android release; it removed EROFS-specific execution issues without changing Linux desktop rootfs policy. |
| Anonymous host VM for Android RAM | Accepted. It removed file-pager churn from cold boot while preserving ordinary file-backed disk behavior. |
| Full physical-device RAM wiring | Retained as fail-fast startup policy. A successful wire does not reserve all later framework/runtime headroom, so memory-limit qualification still uses real device outcomes. |
| Kernel physical window as the Android RAM root | Accepted. The software MMU and host services borrow the kernel's linear window, while protection-specific native mappings remain shared aliases. VM tracking confirmed that the separate full-size root mapping disappeared without creating another resident guest-RAM range; physical devices still require headroom for non-guest memory. |
| Apple asymmetric CPU topology | Rejected and reverted. Public topology/QoS hints could steer carriers but reduced useful Android capacity on the target device. |
| Physical-device boot profiling | Measured. After RAM changes, the remaining gap is dominated by host execution and scheduling service for the full gadget/MMU working set. |
| Batch sparse UBD discard | Accepted in both maintained kernel lines. A 512 MiB raw discard collapsed from 16,384 requests to four; matched controls measured about 19x lower wall time on macOS and 10.6x lower wall time on a physical iPad, while reclaiming the same sparse-file allocation. |

## Graphics backend investigations

| Attempt | Disposition and lesson |
| --- | --- |
| Separate CGL/OpenGL and mobile renderer paths | Replaced by one authenticated ANGLE/Metal path across Apple products. |
| Complete GLES 3.2 through existing APIs | Not viable as a single current Apple/Virgl path; the product claims only the qualified GLES boundary in [`graphics.md`](graphics.md). |
| Add Vulkan by pairing Venus with MoltenVK | Not implemented. MoltenVK does not supply the missing Virtio-GPU blob/shared-memory protocol, synchronization, or lifetime model. |
| CPU readback as native presentation | Retained only as a diagnostic fallback; sustained native qualification requires the Metal texture path. |
| Profile Chromium H.264 playback | Accepted. Host sampling showed one-millisecond fence polling and VNC scanout readback dominated the GPU thread while VideoToolbox decode was a small share. Packed damage readback, persistent staging, direct framebuffer rows, predicate-aware 100-microsecond fence polling, reusable codec buffers, and NV12 IOSurface/Metal copies raised matched 10-second Chromium callbacks from 85 to 128 and cut dropped frames from 126 to 63. Polling shared syncs from the active guest context was rejected because ANGLE/Metal retired them more slowly; context-zero polling remains. |
| Defer Virgl fence completion | Accepted. Host-owned per-context timeline cookies allow multiple fenced submissions and transfers to remain in flight without blocking the GPU transport thread. Matched Chromium playback delivered more good frames with roughly flat host CPU use; renderer-state mutations remain barriers. |
| Post-deferred-fence Chromium H.264 candidates | Rejected after a fresh 1080p60 campaign with separate 30-second desktop and browser cool-downs and 60-second A-B-B-A measurement blocks. The gadget link front memo passed its first block, but four blocks averaged 0.72 percent, included negative pairs, and had a family-wise lower bound below zero; per-context detach bypass, dropping no-client scanout and flush commands, compositor removal, VideoToolbox decode-worker offload, threaded Virgl sync, and event-driven fence retirement failed the initial performance or correctness screen. VideoToolbox and fence waits remain mostly blocked wall time while vCPU execution dominates actual CPU. |
| VideoToolbox H.264 candidate sweep | Rejected under the same 1080p60 cool-down protocol. Bounded Metal-copy retry and depth-two asynchronous decode passed their first blocks, but four-block means were negative and included negative pairs; activation evidence confirmed that the asynchronous path executed. Descriptor narrowing, RBSP initialization, direct single-iovec input, framebuffer reuse, cache-flush frequency, parameter-set caching, and command-flush coalescing failed the initial screen. Direct decoder-output aliasing remains invalid at the resource-semantics gate; the synchronous decode and Metal-copy path remains the baseline. |
| Use the native JIT for Chromium H.264 playback | Rejected. Translation-cache churn stalled this browser workload before playback, so the qualified gadget engine remains the performance baseline. |
| Gadget-cache and TLB follow-up for Chromium H.264 | Rejected under the settled 1080p60 A-B-B-A protocol. Instrumentation found each vCPU's 65,536-entry program cache nearly full, about 122,000 translations and 1.57 million lookups per second, and a 92.25 percent hit rate. Stable link storage, alternate hashes and stream indices, lookup and deactivation memos, kiosk launch, cache capacity, associativity and replacement changes, fewer resident program generations, and a larger reverse index either regressed, produced mixed paired signs, or lost required decode evidence. Four resident TLBs produced eight positive pairings but failed the family-wise lower bound; two resident TLBs produced three positive valid blocks but later wedged early guest boot, so both were reverted. A non-video follow-up was neutral on eight-worker SHA-256 throughput, with two paired effects below one percent and flat wall time, but showed positive process/exec timings; one pair was dominated by a slow baseline while the other reduced time by 1.2 percent. Translation churn is real, but retaining address-space-local state requires a new correctness-preserving generation and lifetime design. |
| Extend VideoToolbox decode to HEVC and P010 | Accepted. A four-vCPU 1080p60 A-B-B-A qualification with separate 30-second desktop and browser cool-downs measured 15.7x HEVC Main and 30.1x HEVC Main10 decoder throughput over four-thread software decode, with 97.2 and 98.7 percent less guest decode CPU time. The upstream Debian Chromium build has no software HEVC decoder, so the same-file hardware/software comparison used upstream guest FFmpeg while separate 60-second Chromium runs qualified VA-API and VideoToolbox end to end. Chromium delivered about 49.3 good frames per second for both profiles, leaving presentation and guest GPU work, rather than decode, as the 60 fps bottleneck. |
| Extend stock-guest VideoToolbox formats | Accepted HEVC Main and Main10 through 8192x4320 at level 6.2 and JPEG Baseline/MJPEG through 4096x2304. A settled four-vCPU 1080p60 A-B-B-A comparison against four-thread software decode measured 13.0x, 24.4x, and 3.8x wall-time speedups for HEVC Main, HEVC Main10, and MJPEG, with 96.7, 98.4, and 72.3 percent less guest decode CPU time. Both 8-bit and 10-bit HEVC decoded correctly at 7680x4320 level 6.1, and MJPEG hardware output matched software within sub-LSB rounding error. H.264 High10 was rejected on macOS after native frame validation exposed a false-positive session probe; 4:2:2 and 4:4:4 were rejected because stock Debian libva and Mesa do not expose the required profiles and surfaces. No guest-library fork is required or supported. |
