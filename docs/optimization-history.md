# Optimization history

This is the repository's single history of attempted performance work. Entries
retain only the hypothesis, disposition, and durable lesson. They do not define
current behavior; current contracts live in architecture and subsystem docs.

`Accepted` means the useful mechanism remains or its result became product
policy. `Rejected` means the candidate was removed or disabled. `Measured`
means the work improved understanding without justifying a product change.

## General gadget forwarding

| Attempt | Disposition and lesson |
| --- | --- |
| Template-owned adjacent scalar result forwarding | Accepted as the default policy, with an explicit opt-out for controls and code size. Shared assembly operand contracts cover arithmetic, logical, bitfield, select, shift, and ordinary memory inputs without an opcode-pair whitelist. Matched no-helper-exit controls improved dependent scalar throughput by roughly 2–3x, with independent controls neutral; full Geekbench and Aquarium coverage established broad applicability, not a general application speedup. Larger code/data banks and mixed full-workload timings remain tradeoffs. Both code layout and execution-data addresses must match in mechanism controls; disabling forwarding and coverage preserves the original gadget text. |
| Template-owned adjacent SIMD/FP result forwarding | Accepted as an independently configurable default. Natural-v2 contracts extend shared arithmetic, comparison, accumulator, widening/narrowing, reduction, and shape templates without new hot-path producer copies. Default-iteration, four-vCPU Geekbench 6 repeats averaged about 0.4 percent single-core and 1.6 percent multi-core over scalar-only forwarding. Both multi-core pairs were positive, but workload effects were mixed and gains small relative to shared-host variation. Default enablement does not turn template coverage and correct forwarding into a broad application speedup claim. |
| Combined scalar/SIMD dependency coverage | Measured with both forwarding policies enabled on four vCPUs. Full one-iteration Geekbench 5/6/7 single-core sets, Geekbench 6 multi-core, and steady-state Aquarium showed that broad contract coverage does not imply frequent adjacent dependencies: SIMD added about 0.7–3.3 percentage points of entered-instruction coverage in Geekbench and 0.2 points in Aquarium. Scalar and SIMD counters are disjoint and execution-weighted; conditional source opportunities and entered prefixes are not retired reads, total program dependencies, or speedup estimates. |
| Preserve results across opposite-domain gadgets | Measured, not enabled. A conservative shadow model preserving scalar/SIMD results only across audited nonclobbering opposite-domain entries added about 0.12 percentage points of entered-instruction coverage in both full Geekbench 6 single-core and steady-state Aquarium. It reused the existing dependency contracts and did not change forwarding selections. Template-owned preservation metadata is a bounded next candidate, but the aggregate opportunity is modest and is not a speedup estimate; memory/helper paths and undeclared result shapes require separate contracts. |

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
| G189 | Mirror the retained arm64 cache hierarchy in x86 gadgets | Accepted after desktop-boot profiling and cache A/Bs: bounded four-way program storage, a two-way successor tier, and direct and indirect links reduced dispatch work without weakening exact source validation. |
| G190 | Bulk-own x86 cache-generation allocations and reuse validation buffers | Rejected; matched desktop boots were neutral to slightly slower. |
| G191 | Inline exact validation for tiny x86 source spans | Accepted; more than 90 percent of profiled validations were at most 32 bytes, and the change reduced full smoke-boot time by about five percent while the earlier queue milestone remained neutral. |
| G192 | Measure realistic scalar and SIMD gadget ceilings | Measured on the default 16 KiB profile with two new no-exit workloads whose bodies the guest probes share word for word. A browser-like scalar mix (loads of every width and offset form, a pair, three stores, flag-setting ALU, selects, a never-taken check and two data-dependent skips) sustained 13.0% of native throughput, and an FP inner loop (vector pair and register-offset transfers, indexed and vector FMLA, converts, an FP compare branch) 6.9%, beside 46.8% for the dependent ADD chain in the same hour, itself up from G178's 15.4% since template forwarding. A one-vCPU production-gadgets guest reached 97% of the scalar ideal and 91% of the SIMD ideal with identical checksums and path counts. The lesson is that the engine spends about four host cycles per scalar word and three and a half per SIMD word of realistic code, where the native core retires the same mixes at 1.9 and 4.3 instructions per cycle, so the ADD-chain ceiling overstates the reachable ratio four to seven times; the SIMD guest's wider gap to its ideal is consistent with the FP environment work at each executor round trip and is the first place to look for whole-workload FP gains. |
| G193 | Attribute the realistic ceiling cost with hardware counters | Measured on the default 16 KiB profile with CPU Counters samples of the ideal bench and freestanding host microbenchmarks. The scalar mix executes about 12 to 13 host instructions per guest word at a host IPC of 3.0, with 37% of cycles in back-end stalls and 15% discarded to mispredictions (native discards 56% on the same data-dependent branches); the ADD chain runs at IPC 3.8 and the SIMD mix near 4 by static count, the tracer itself slowing that run. On this core threaded dispatch costs about 1.4 cycles per gadget whether or not sites are shared, a block-link follow adds about 4.6 cycles, a scalar result round trip through CpuState costs 6.6 cycles and a 128-bit vector round trip 12.3 against 1 and 3.2 in registers, and a register move is eliminated at rename so forwarding adds no chain latency. Adjacent forwarding covers 31% of the scalar mix's and 20% of the SIMD mix's read-after-write edges. Lesson: above the dispatch floor the mixes are bound by exposed dependency latency through CpuState and by misprediction refill, not by instruction count, so a reduction pays only when it shortens a chain on the critical path, by fusing dependent pairs or keeping a result live across more than one gadget. |
| G194 | Keep write-through results in a bank of forwarding slots instead of one adjacent register | Rejected at one and two slots; the work is kept on a local branch as the base for link-surviving residency. Two scalar slots (`x3`, `x4`) and two vector slots (`v16`, `v17`) let a value outlive every declared entry between its producer and a later consumer; vector loads and stores, PC-relative moves and inline branches joined the contracts, and a conservative register-set decoder invalidates a slot only when its guest register is rewritten. The two-slot bank grew the emulator from 41 to 52 MB. The ideal ceilings did not move at any slot count (add-immediate 1.62 G/s, scalar-mix 0.76, SIMD-mix 0.89-0.91) because the values the mixes wait on are loop-carried across block links, where every slot dies. In a same-hour three-arm comparison the one-slot bank was within half a percent of the control on all three guest probes, equal on Geekbench single-core (130 versus 130) and 3 percent lower on Aquarium inside that workload's run-to-run spread; two slots cost 1.7 percent on the SIMD probe, gained one Geekbench point (131) and lost 7 percent on Aquarium (12.7 against 13.7 fps), consistent with the larger variant text on a browser workload. Two defects surfaced only outside the unit fuzz: the native CSSC count entries encoded a hard-coded `x0` result while storing the slot register, found by comparing every base entry's disassembly with the control under a consistent register renaming; and a decoder-oracle audit against capstone over every distinct kernel and Geekbench instruction found the accumulating SIMD forms (`saba`, `uaba`) decoded without their destination read, harmless to slot invalidation. Lesson: intra-block residency is not where the remaining hop cost is; the hops that bound a five-instruction block sit at its entry, so only residency that survives a direct link could remove them, and more variant text is paid for on every workload whether or not it forwards. |
| G195 | Reorder a block's instructions before translation so consumers follow producers | Rejected at both levels and removed. A list scheduler over the same dependency decoder, strict (only instructions that cannot fault or exit move, among themselves) and relaxed (re-executable instructions also hoist across faulting ones). Ideal ceilings unchanged within 1%, but the translation-free guest probes ran 5 to 8 percent slower at either level and slot count, Geekbench single-core lost 2 percent, and Clang lost 27 percent: a sample of the emulator showed the scheduler itself at 17 percent of that run, since every block scheduled the whole 64-word fetch window. The Aquarium bootstrap also stalled under scheduling, which turned out to be the GPU runner losing console input when the guest falls behind its unpaced 128-byte-per-10-ms feed; the same run passed with the writer slowed five-fold, and the stall is recorded there rather than in the engine. Two correctness rules were learned from Geekbench before the rejection: an idiom-folding decoder must see only an instruction's guest successors, or a pair the schedule made adjacent is folded and the branch addressed from the wrong instruction; and a hoisted instruction must not write anything an instruction between the crossed one and itself reads, because a fault at the crossed instruction resumes in program order and re-executes those instructions with the clobbered input (a `subs` size computation read a shifted copy of itself and `operator new` threw). Lesson: the host core already overlaps independent chains across gadgets, so packing consumers behind producers lengthens the critical path it sees; ordering is not a lever for a data-only engine, hop count per guest instruction is. |

## Host instructions per guest instruction

| Attempt | Disposition and lesson |
| --- | --- |
| Measure host instructions per guest instruction | Measured with per-carrier hardware counters (xctrace CPU Counters bursts joined to the runtime dispatch profile) and an exact execution histogram. The Geekbench 6 single-core set costs about 16.3 host instructions per guest instruction (12.9 File Compression to 21.7 HDR) at IPC 3.1; Aquarium about 19.8 (14 to 32 per carrier). Gadget entries hold 89–90 percent of host instructions, MMU helpers 1–4 percent, executor and link training 3–4 percent, translation up to 1 percent. Weighting exact entry counts by each entry's shortest helper-free path models 14.1 and 13.4; memory transfers are a quarter of guest instructions but 40 percent of fast-path cost at 22–28 host instructions each, SIMD/FP 13–15, ALU 8–11, inline conditional branches 9. The 807 hottest entries (71 KiB of a 1.6 MiB bank) cover 99 percent of executions. |
| Count why linked chains return to the executor | Measured. 89 percent of Geekbench single-core block entries and 73 percent of Aquarium's are indirect-site misses through the two cells; 80 percent of the Geekbench misses are returns, and 85 percent of misses happen at sites with five or more distinct targets, which no small cell array can cover. Each round trip costs about 350 host instructions; HDR breaks a chain every 132 guest instructions and spends 12 percent of its host instructions there. |
| Return-record ring for cached returns | Accepted as the default (`UML_GADGET_RETURN_STACK`). Linked calls push {return PC, return BlockLink} on a private sixteen-record ring; a cached return whose cells miss pops a matching record and trains the cell as the cold helper would. Exact counters over the Geekbench 6 single-core set: executor block entries −59 percent, return misses −84 percent; HDR, Photo Filter and File Compression lose every return miss, and a 64-record ring changed nothing, so the residue is not depth. A first form that skipped cell training lost 6 percent on Clang and Photo Library because every monomorphic return then paid two empty cells plus the ring. Quiet-host confirmation of the trained form, alternating control and ring: Geekbench single-core +2.7 percent score and −2.6 percent guest CPU time over nine valid runs, ring ahead in four of six adjacent pairs by score (the losses −0.7 and 0.0 percent) and five of six by CPU time, with Clang +13, HDR +10, HTML5 Browser +4, Photo Library +4, PDF Renderer +3 and Photo Filter +2.5 percent and Object Remover −2 percent; Aquarium +2.8 percent FPS over eight runs, ahead in three of four pairs. The push costs seven host instructions per linked call and 8 bytes of stream; returns from sites more polymorphic than their cells are where it pays. |
| Tagged TLB keys | Rejected. Keying the software TLB by the address as presented removes the two-instruction top-byte check from every scalar transfer, but Geekbench single-core measured −1.2 percent score and +1.3 percent guest CPU time, and Aquarium −2.2 percent, none positive. As in G186 and G187, the check overlaps the TLB dependency chain rather than extending it; MTE guests would also key several tags of one page into the same set. |
| Fused transfer translation | Accepted as the default (`UML_GADGET_FUSED_TRANSLATE`). Same-page transfers key the software TLB by the page of the transfer's final byte while indexing by its first, which folds the three-instruction cross-page check into the tag compare: with 16 KiB pages n and n + 1 never share a direct-mapped set, so a crossing transfer can only miss and is recognized on the cold path. The hit path falls through with the second way, crossing and miss handling behind the dispatch, keys are page numbers, and pair transfers translate in place. A 64-bit unsigned load's hit path drops from 23 to 21 host instructions and an SP pair's from 28 to 24, about 5 percent of the modeled fast-path instructions of the Geekbench single-core set and 4 percent of Aquarium's. The first form compared the shifted key with a shifted-operand `cmp`: 2 percent slower on transfer-bound probes (independent loads, byte and pair read-modify-write) although 1.5 percent faster on dependent pointer chains, and a matched Geekbench wash (−0.4 percent score). A separate shift and plain compare, one word longer, made every probe neutral or 1 percent faster, and restoring the taken-branch shape around the shifted compare reproduced the loss, so the operand form of the compare was the cost, not the branch shape. Matched-layout Geekbench single-core on the plain form, host shared with other campaigns: +1.4 percent score and −2.0 percent guest CPU time over three valid adjacent pairs, ahead on CPU time in all three; Aquarium on a quiet host is neutral: 14.45 against 14.34 average FPS over eight alternating runs (+0.8 percent) with the candidate ahead in one of four adjacent pairs, inside the run-to-run spread of one arm; three earlier campaigns under other sessions' GPU load were unusable. A quiet-host repeat of Geekbench (load 10 to 17) confirmed +1.2 percent score and −1.0 percent guest CPU time over four adjacent pairs, ahead in three and four of them, with thirteen of sixteen workloads up and none down more than 2 percent. A dependent guest load costs about 15.5 host cycles with a forwarded base and about 19 through CpuState, of which the TLB index, load and add are about 6: the remaining transfer lever is latency, not count. |
| Four indirect cells and NOP elision | Measured only by exact counts; not timed. Four cells cover only the 15 percent of misses at three- and four-target sites. NOP entries (BTI and PAC hints on a profile without pointer authentication) are 2.5 percent of Aquarium's guest words and 0.5 percent of Geekbench's, two host instructions each; elision is a runtime switch (`DARWIN_UML_GADGET_ELIDE_NOPS=1`) left for a future matched timing. |

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

## JIT multicore and browser working sets

| Attempt | Disposition and lesson |
| --- | --- |
| Isolate JIT store-publication state at the host cache-line size | Accepted. Adjacent 64-byte vCPU slots shared an Apple arm64 cache line. Using 128-byte slots and separating the shared gate fields improved four-worker HDR throughput without weakening the publication memory orders. |
| Replace unused shared JIT translations within the process budget | Accepted. Bounded replacement prevents startup code from permanently crowding out later working sets. The shared index retains its high-water allocation charge, and live validation words continue to pin their storage. |
| Fund larger private JIT working sets within the process budget | Accepted. A 256 MiB private ceiling reduces browser startup cache resets while the total budget remains tied to guest RAM. Smaller process budgets reduce private geometry before allocating the shared remainder. |
| Retain address-space link activation in writable data | Accepted. Generation-tagged direct and indirect links retain trained targets across unchanged address-space switches, while bounded context eviction and code publication deactivate the affected context. Link updates no longer open executable write windows. |
| Lower bounded compare/exchange retry regions natively | Accepted. Native exclusives preserve width, ordering, status, flags, and retired prefixes; read-only mismatches and tagged accesses retain their semantic behavior. Concurrent native and semantic retries share the existing publication protocol. |
| Remove speculative GPR residency recompilation from runtime misses | Accepted. Cold translation profiles exposed a second lowering pass on runtime misses. Keeping one lowering pass removes this duplicate compilation work. |
| Admit repeated ordinary blocks to native caches | Rejected as the runtime default; retained as an explicit standalone option. Initial throughput attribution was withdrawn because a PAuth gate left production admission inactive. Configuration-matched tests corrected that gate, but enabling admission substantially regressed single-core File Compression with little multicore benefit. Preserve eager runtime compilation. |
| Reuse peer translations before cold admission | Retained for the explicit admission option. An executor uses an exactly validated shared translation on its first local visit instead of repeating semantic execution. Initial throughput attribution was withdrawn because production admission was inactive during those trials. |
| Release the MMU execution reader during native compilation | Rejected. Reentry version checks and concurrent code mutation tests passed, but paired four-worker photo subtests showed no meaningful throughput gain. Cold compilation was not the remaining shared-MM bottleneck. |
| Plan single-page MMU batches without temporary containers | Rejected. Both page-profile MMU suites passed, but additional matched photo trials did not reproduce the initial throughput gain. Container allocation was not the dominant cost of the shared-MM waits. |
| Remove redundant kernel fault mapping walks | Rejected. Removing the initial walk gave inconsistent photo-subtest gains, and checking mappings after draining pending changes added more variability. Preserve the existing fault repair until a repeatable throughput improvement justifies the shared kernel change. |
| Enable generic kernel optimistic spinning | Rejected. Shared-MM and SMP checks passed, but reversed-order photo subtests produced mixed multicore throughput and no repeatable scaling gain. Preserve the existing lock policy. |
| Drain IPI pipes using completed-write counts | Rejected. Mainline checks passed in both page profiles, and Android boot, wakeup, and shared-memory checks passed. Removing poll calls did not produce a repeatable four-worker photo gain in alternating trials without compiler overlap. Preserve the existing drain protocol. |
| Isolate kernel carrier flags and counters on 128-byte host cache lines | Rejected. The adapted mainline and Android source policies passed, and the 16 KiB SMP and shared-memory screen passed. Alternating photo trials reduced aggregate multicore throughput; packed carrier state was not the remaining scaling bottleneck. |
| Publish data permission increases without stopping execution | Rejected. Both page-profile barrier and rollback tests passed, but alternating four-worker photo subtests showed mixed results and no meaningful scaling gain. Avoid weakening the general mapping barrier without a workload benefit. |
| Reuse pinned state for TLB history reads | Rejected. Active-generation reuse and direct execution-context reads passed both page-profile lifetime and invalidation checks, but matched photo trials did not establish a meaningful gain. Visible registry contention in diagnostic samples did not translate into a useful throughput improvement. |
| Skip translated-cache reconciliation for first page publication | Rejected. Both page-profile unit, rollback, cache, SMP, and shared-memory checks passed. Alternating photo and Aquarium trials did not show a repeatable throughput gain, so retain the existing translation invalidation policy. |
| Use an exclusive mutex for address-space mapping metadata | Rejected. Both page-profile unit suites and guest checks passed, as did Android boot, wakeup, and shared-memory checks. Short photo trials suggested a small gain, but the full comparison without compiler or other-emulator overlap left scaling unchanged and reversed the Photo Filter result. Retain concurrent metadata readers. |
| Defer contended MMU replay at ordinary userspace returns | Rejected. Both engines passed smoke, SMP, and shared-mapping checks in both page profiles; Android source replay and build also passed. Alternating photo trials gave a small, inconsistent throughput gain while scheduler traces retained the fault-path mutex contention. Forced fault repair remained the dominant source of waits. |
| Replace MMU shared mutexes with native pthread reader/writer locks | Rejected before product changes. An isolated primitive comparison improved uncontended access but substantially reduced four-thread read-only and mixed read/write throughput. |
| Avoid code-version changes for first page publication | Rejected. Existing-code, remap, execute-fault, and allocation rollback tests passed in both page profiles, but paired photo subtests did not improve throughput. Retain conservative code publication semantics. |
| Require a third visit before native admission | Rejected. Correctness checks passed, but production admission was inactive during the timing trials, so those scores do not justify changing the threshold. Keep native admission on the second local visit. |
| Increase JIT dispatches from 64K to 256K guest instructions | Rejected. Engine contract checks passed in both page profiles, but paired photo subtests showed no multicore throughput gain. Keep the current kernel-entry budget. |
| Increase native JIT chains from 4K to 16K guest instructions | Rejected. Focused photo controls showed a small multicore gain with single-core variation, while full runs overlapped other host work. Preserve the 4K fairness checkpoint until an isolated comparison justifies longer preemption intervals. |
| Reuse an already-owned FP environment from scalar chain entries | Rejected. Direct and indirect capability checks passed in both page profiles, including missing-state fallbacks. Alternating photo trials showed a small HDR gain but slower Photo Filter throughput in both candidate runs, so broader chaining did not improve multicore scaling. |
| Publish store eligibility from read-only JIT chain entries | Rejected. Focused checks passed in both page profiles after excluding reservation-establishing helpers, but alternating photo trials showed no reliable scaling gain. Broader chains also require explicit handling of a successor's deferred reservation exit; retain the existing publication boundary. |
| Shorten JIT dispatches from 64K to 4K guest instructions | Rejected. Chromium still missed its ordinary startup deadline, and paired photo subtests lost multicore throughput. |
| Enable linked x8/x9 carry or isolate the MMU execution counter further | Rejected. Linked register carry regressed paired photo subtests. Separating MMU sequence fields from its execution counter produced mixed paired results, so the original layout remains. |
## x86-64 gadget engine campaign

Measured on macOS with matched ABBA runs: focused static x86-64 probes in the
guest for the mechanism, then Geekbench 6.7.1 single-core for the workload.
`--iterations 1` reproduces the default run's per-workload rate at roughly a
fifth of the wall time and is what these entries used.

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| X1 | Declare the ARMv8.5-A host baseline | Accepted as an enabler with no performance claim. The engine is macOS-only, so every host implements LSE, FlagM, FlagM2 and FRINTTS; the assembled object is byte-identical. |
| X2 | Drop result normalization the host already performed | Accepted. A W-form host instruction writes the whole 64-bit register, so width normalization was a no-op at 32 and 64 bits. 32-bit register arithmetic improved 12%. |
| X3 | Fold away the page-crossing test for single-byte accesses | Accepted. A one-byte access cannot cross a page. Byte memory operations improved 4%, and forcing the check to emit reproduced the previous engine instruction for instruction. |
| X4 | Write the deferred-flag tail as one constant | Accepted. Operation and width are macro parameters and the rest of the tail is zero, so eight bytes are one link-time constant. Compare, set-condition and locked paths improved 4%. |
| X5 | Interprocedural optimization for the translation path | Accepted on workload evidence against probe evidence. Steady-state execution is 0.5% to 2.3% slower, but four Geekbench workloads gained up to 2.75% with none regressing. Probes cannot see a change that only affects translation. |
| X6 | Resolve effective addresses without branching on the descriptor | Accepted. Every address field is a translation-time constant. An always-zero state slot lets an absent base or index be added unconditionally and lets the segment selector index a table; truncation distributes over addition, so the narrow-address mask applies once to the sum. Eight branches become none, the bank loses 13%, and memory operations improved 2% to 12%. |
| X7 | Extract byte sign masks with NEON | Accepted. Four shift-and-accumulate steps replace a loop that ran once per lane. The instruction is 78% faster; it is the core of glibc's SSE2 string routines, which a static histogram of a dynamically linked benchmark cannot see. |
| X8 | Skip the store-side publication and overlap check | First attempts rejected: hoisting the check before the store, gating it on the write-code flag, and gating it on the executable-page bit all measured no gain, because Linux grants READ_IMPLIES_EXEC to binaries without a non-executable stack marking and every data page then qualifies as executable. |
| X8b | Call code publication only when a store can publish something | Accepted. Publication is 54% of every guest store and its call is a lookup that misses whenever the page holds no cached translation. Recording that in the write translation, and retiring the translation when a page gains code, turns it into one bit test. Stores 47% to 54% faster; File Compression +3.5%, Text Processing +2.1%. The lesson is that the expensive part of a sequence can be the call it guards rather than the instructions that are visible. |
| X9 | Evaluate branch conditions from the deferred record | Rejected. The premise was that a conditional branch pays full flag materialization, but that routine exits early and is already cheap; adding a pre-check ahead of it made conditional branches 57% slower in two independent implementations. |
| X10 | Fold the recorded operation and width into flag materialization | Accepted. Both keys are constants at the recording site, but the materializer rediscovered them through two run-time chains that reached the commonest cases last. One bounded table branch now selects a body with both folded in, so the width masks, the sign-bit index and the clear mask are immediates. INC 22% faster, SETcc and ADC 18%, CMP/Jcc 15%; File Compression +3.1%, PDF Renderer +3.1%, Horizon Detection +2.9%. |
| X11 | Discard the deferred record in the bit-count family | Accepted. TZCNT, LZCNT and POPCNT define every arithmetic flag and cleared all six immediately after paying to materialize them, and every override any gadget records lies inside that same clear mask. Two stores replace the call, and a TZCNT after an ADD is 69% faster. |
| X12 | Evaluate carry conditions from the deferred record | Rejected, reproducing X9 from the opposite direction. This version replaced the call instead of adding a check in front of it, and carry-condition branches were still 70% slower. The zero-condition shortcut it imitates works because a recorded result is already width-masked and one compare answers it; carry needs the recorded operation, so the shortcut reintroduces the dispatch it exists to avoid, in a gadget rather than in a shared routine. |
| X13 | Stop advertising ERMS | Accepted at the time, superseded by X15. CPUID leaf 7 set the bit that tells glibc's ifunc resolver REP MOVSB is the fast way to copy, while this engine implements REP as a per-element loop. Withholding it moves string work onto the vector gadgets: PDF Renderer +8.6%, Horizon Detection +8.4%, File Compression +1.5%, every distribution non-overlapping. A capability claim is a scheduling hint to the guest's own dispatchers, so it is a performance decision, not only a compatibility one -- and withdrawing a false claim is only one of the two ways to settle it. |
| X14 | Move a repeated string operation in page-bounded runs | Accepted. REP MOVS and REP STOS resolved an address, probed the software TLB and settled a store's publication once per element, which for the MOVSB and STOSB a libc reaches for is once per byte. Those costs belong to the page, so a run confined to one source page and one destination page now pays them once and moves its bytes in 64-byte blocks; the element loop keeps everything a forward block cannot express and the two rejoin each other freely. REP MOVSB is 71x faster at a 4 KiB call size and 112x at 32 KiB, REP STOSB 58x and 98x, REP MOVSQ 9.8x, and 32-bit-addressed REP MOVSB 44x. The first version instead fell back to the element loop whenever the destination page could hold code, and gained nothing whatsoever. |
| X15 | Advertise ERMS again | Accepted, superseding X13. ERMS tells glibc's ifunc resolver that REP MOVSB is the fastest way to copy, which runs make true here: at a 4 KiB call size REP MOVSB is 7.7x the SSE2 copy routine and 6.0x the AVX2 one, REP STOSB is 7.1x and 5.9x their fill counterparts, and REP still leads both at a 256-byte call size where per-instruction setup dominates. Over the three workloads X13 moved -- ABBA, four runs per arm, withheld against advertised -- File Compression is -0.97 percent, PDF Renderer -0.33 and Horizon Detection +0.41, a geometric mean of -0.30 with every pair of distributions interleaving. Withholding no longer buys anything measurable, so the bit goes back for the guests that assume it exists. |

| X16 | Keep the native chain alive across a code publication | Accepted, and the largest result of the campaign. Counting stream entries against retired instructions showed the engine leaving the translated stream every 5.8 guest instructions in Text Processing, 6.7 in Clang and 8.5 in HTML5 Browser -- the three lowest-scoring workloads -- with the block it returned for already resident 99.7 to 99.98 percent of the time. A link record is reset for the whole address space whenever the code sequence moves, and came back only on translation or on the one branch of a cache hit that revalidated, so after a single publication the links stayed dead. That closed a cycle: the path that settles a run's own publication without cooling unrelated links only runs when a block follows a link. Re-arming a record whenever its slot is returned, and settling a publication when the run leaves the stream as well as when it follows a link, opens both ends. Instructions per stream entry 5.8 to 38.5, 6.7 to 40.3 and 8.5 to 43.5; Text Processing +133.8 percent, HTML5 Browser +87.1, Clang +77.1, File Compression +16.3, Horizon Detection +12.9, a geometric mean of +59.0 with no overlap on any workload. |
| X17 | Materialize only the carry for the operations that need it | Accepted. INC and DEC preserve CF and define the other five arithmetic flags from their own record; ADC and SBB read CF as an operand and then define all six from theirs. Each opened with an unconditional call to the full materializer, whose zero, sign, parity, auxiliary and overflow work the caller's own record overwrites a few instructions later. A carry-only entry point computes the one live bit through the same operation-and-width table: INC and DEC compute nothing, a logical operation clears it, the rest is one compare. Only the register forms use it, because the five stale bits are architecturally visible until the caller records and a read-modify-write form can fault in that window. ADC register-register 1.78x, INC register 1.76x, DEC after ADD 1.68x, INC after ADD 1.66x, ADC after ADD 1.61x, INC after CMP 1.35x; on the workloads PDF Renderer +3.99 percent, Horizon Detection +2.10, File Compression +1.56, a geometric mean of +2.54. The gap between 1.7x on a probe and 2.5 percent on a workload is the share these operations hold in a real instruction mix, and is the reason the probe alone could not have settled it. |
| X18 | Read a condition from the flags the compare already set | Accepted. A 32- or 64-bit CMP, SUB, TEST or AND computed with the flag-setting host instruction leaves NZCV holding exactly the x86 flags it defined, and nothing between producer and consumer disturbs them: the deferred record is an address and three stores, threaded dispatch a load and a branch. Producer and consumer stay separate gadgets and the builder pairs them, which costs one variant per condition rather than one per combination. ARM reading a subtraction's carry as the inverted borrow is what lines the unsigned conditions up with x86's. Parity has no NZCV form; after a logical operation ARM reads the cleared carry as borrowed where x86 reads it as no borrow, so the four conditions that consume CF keep the deferred path; and the narrow widths arrive zero-extended, so their host flags describe the wrong operation. The deferred record is still written, which keeps a later PUSHF or ADC correct and makes the test exact without a reference model -- two identical SETcc in consecutive slots must agree across both paths. CMP feeding SETcc 2.15x; CMP feeding a conditional jump 11 percent slower in a probe that is sixteen two-instruction blocks per iteration, where only block transition is measured; on the workloads File Compression +12.61 percent, Horizon Detection +8.61, PDF Renderer +5.46, a geometric mean of +8.85 with every distribution disjoint. The probe that regressed measures the one thing the change does not address, and the workloads outvoted it. |
| X19 | Advertise fast short REP MOVSB | Accepted. ERMS claims REP MOVSB is the fastest copy; FSRM narrows the claim to the short sizes where per-instruction setup rather than throughput decides, and glibc reads the two bits separately, so a resolver seeing only ERMS still sends short copies to a vector routine. Both claims hold once X14 made a page-bounded run a block move: REP MOVSB leads the SSE2 copy 7.7x and the AVX2 copy 6.0x at 4 KiB, and still leads both at 256 bytes. The workloads neither gain nor lose -- PDF Renderer +1.35 percent, File Compression -0.12, Horizon Detection -0.19, a geometric mean of +0.35 with two of three pairs interleaving. As in X15, the burden falls on withholding: a neutral measurement is no reason to route the guest around a path that is not slow. |
| X20 | Widen the indirect-branch site cache from two targets to four | Rejected. A RET is an indirect branch whose site belongs to the callee, so every caller of a shared function competes for the same ways, and after X16 indirect misses were the largest single remaining exit cause at 14 to 30 percent. Four ways cost 32 bytes per site and four instructions on the path to a miss, and the mechanism does respond: a probe calling one callee from four distinct sites runs about 2.4x faster, while one calling from eight -- beyond either capacity -- does not move. The workloads do not. On the three call-heavy ones, where a shared callee's site should be widest, Clang +2.22 percent, HTML5 Browser 0.00 and Text Processing -0.23, a geometric mean of +0.65 with every distribution overlapping; on the flag-axis three, +0.32. Sites that miss with two ways and hit with four are rare enough in real code that the mechanism working is not the same as the change paying, which is the same gap X17 crossed in the other direction. |
| X21 | Keep a call in the stream when it publishes a code write | Accepted, and the largest result since X16. Counting stream exits by the site that performed them -- rather than by the five causes an earlier diagnostic guessed at, which put 61 percent into "other" -- found one instruction responsible for 72.5 percent of Geekbench 6's: a linked CALL leaving because its own return-address push had published a code write. Direct and conditional jumps exited 0.00 percent of the time, so the link machinery worked; calls were not allowed to use it. The ordinary store path already publishes and continues, leaving only when the write lands inside the running block, and a return-address push is an ordinary store. Publication deactivates every cached block whose source bytes the write overlapped and resets the links reaching it, and the link still has to pass the same safety test a jump relies on, so the distinction that matters is "an executable page was written" against "the instructions I am about to run were written". Call probes 1.60x to 2.43x with every distribution disjoint; Geekbench 6 single-core 49.0 to 51.5, also disjoint, with HTML5 Browser +20.5 percent, Text Processing +18.6 and Clang +17.5. The lesson is that a per-site exit census finds a cause that a per-category one cannot, because the categories were chosen before the answer was known. |
| X22 | Link the block end the fetch window created | Accepted. With calls no longer leaving, running off the end of a block became the largest remaining cause at 48.1 percent of Geekbench 6's exits. A block ends at its first branch, so reaching the terminator means the 64-instruction fetch window cut a straight run short, which unrolled kernels do constantly; the successor is the next address, as statically known as a direct jump's target, and it was the last static transfer left unlinked. Only a block that actually falls through asks for a link, because resolving one for a branch-ended block would allocate a record per block for an address never reached. Straight-line probes -3.7 to -8.8 percent, all disjoint, landing on the time of a control whose block ends at its loop branch -- which is the check that the probes measure the boundary rather than the loop. A later exit census confirms the mechanism directly: block_exit falls from 370.7 million of Geekbench 6's exits to 2.25 million, and total exits fall 46.7 percent. Geekbench score 51.5 to 52.0 and the suite 309s to 305s, with Clang 1.4 percent slower and consistently so: it has much the largest code footprint, so the extra link record per truncated block is a real cost, outvoted by the other five subtests and by both aggregates. |
| X23 | Keep indirect-site training across a link cooling | Rejected. A code change anywhere cools every link and every indirect site in the address space, so each site had to be retaught after any of them -- 920 thousand coolings in one Geekbench 6 run. Retaining the learned targets is safe, because an entry names a guest address and points at that address's link record, records share their generation's lifetime with the sites pointing at them, and a record reset to its fallback returns with the indirect target in RIP, which is the outcome a miss produces anyway. That last property is also why it does not pay: the retained entry hits and then follows a dead link, so the exit still happens, one category over. An exit census shows indirect misses falling 2.4 percent while link_exit rises 5.3 and the link-follow exits rise 50, with the total unchanged. The focused probe agrees at zero, and the workload comparison was unusable -- twelve unrelated emulators put the host between load 7 and 159 across eight runs, and the arms drew different halves of that. The lesson is that moving an exit between causes is not removing it, and that a census of causes settles such a change without needing a quiet machine. |

X1 through X13 were settled together with a matched ABBA over the full
single-core suite, sixteen workloads per run, warm-up discarded, four runs per
arm. Every workload improved or held and none regressed: the geometric mean of
per-workload rates rose 3.94 percent, with Photo Filter +13.5, Structure from
Motion +12.9, Horizon Detection +10.1, PDF Renderer +10.0 and File Compression
+5.9 percent. X14 onward were each measured against their own parent on the
workloads their mechanism reaches rather than by repeating that suite-wide
campaign, so the entries above carry the evidence for each and there is no
combined figure for them.

The opening profile retired most of what the plan still had open, because it ranks
candidates by what executes rather than by what a static instruction histogram
counts. Truncating FP conversion and scalar FP compare -- the `frint32z` and
`axflag` candidates -- appear nowhere in any workload's profile. Locked
read-modify-write is under 0.01 percent, so LSE atomics have nothing to win.
Software-TLB misses are 0.17 percent, leaving gadget-TLB associativity nothing
to fix. Every shift form together is 1.6 percent and PUSH with POP 1.3 percent,
so specializing shifts on a constant count and fusing adjacent stack operations
are each worth well under one percent. Narrow-width flag extraction was
overtaken by X10, which removed the width dispatch it targeted.

The 37 percent in stream re-entry was the campaign's largest remaining item,
and the plan classified it as study-before-building: inlining the conditional
fall-through, a return-address stack for RET, and a linear host window. None of
those was the answer. Counting exits by cause first, rather than building any
of them, found instead that the chain machinery already present was being
switched off wholesale by an unrelated event and never switched back on, which
X16 fixed without adding a structure. Sampling could not have found this: it
attributes time to symbols, and every symbol involved was doing legitimate
work. What it could not show was how often the work ran.

Five lessons generalize. Whether a page might hold code is not a condition this
engine can branch on to save work. An executable-stack declaration has Linux
mark every data page executable, and a probe whose data segment is plain
read-write with a non-executable stack still reported code-capable on every run,
while across the workloads publication actually fires on well under one percent
of self time. The predicate is true far more often than publication is needed
and cannot be predicted from the guest's own markings, so X8's first attempts
and X14's first version both measured nothing by skipping work when it was set.
The work has to become cheaper rather than conditional. Instruction count
inside a gadget is a weak predictor: the changes that paid removed a loop, a
call, or a branch, and those that only shortened a straight-line body did not.
A probe measures the mechanism it names and nothing else, so a candidate that
only affects translation, or that trades throughput for latency, needs a
workload to judge it. And laziness is worth paying for only where retrieving
the deferred value is cheaper than computing it: X9 and X12 failed at the same
boundary from opposite directions, while X10 succeeded by making the deferred
path itself cheaper rather than trying to avoid it. Finally, a profile ranks
what runs but not how often a structure is bypassed; where a fast path exists,
count how often it is taken before assuming its cost is the thing to reduce.
| X24 | Key the indirect-branch tier on the target rather than on the branch | Accepted. X20 widened the per-site tier from two targets to four and did not pay; a census split by the gadget family owning each site says why. Returns were 60.7 percent of Geekbench 6's stream exits and indirect jumps a further 13.8, and a return site belongs to the call that reached it, not to the RET that leaves it, so a function called from N places has N return targets competing at one site however many ways it has. One direct-mapped tier keyed on the target and shared across the address space holds all of them: a block publishes itself when the cache hands it out, so whatever ran most recently at a guest RIP is reachable from every indirect branch, and a RET enters a return site it has never returned to. An entry is a prediction only -- the follower compares the guest RIP it already computed before entering the link -- and entries live and die with their generation, which is what stops one naming a destroyed record. A RET drops its site tier and its stream word with it; indirect jumps and calls keep theirs as a first tier, because two compares still settle a monomorphic site and a private entry survives an eviction from the shared one. Total Geekbench exits fall 69.8 percent, return misses 96.2 and indirect-site misses 98.8. The probes separate the mechanism cleanly: one leaf called from sixteen sites runs 59.8 percent faster with disjoint distributions, while the monomorphic call shapes are 2 to 8 percent slower, which is the hashed lookup replacing a two-compare probe. Real code outvotes the probe -- HDR +11.8 percent with disjoint distributions, HTML5 Browser +5.4, Clang +3.9, Text Processing +3.0, Integer 49.0 to 50.0 -- because a monomorphic RET pays four instructions where a polymorphic one was paying an exit. |
| X25 | Retire only the translations a code change covers | Accepted. A code change anywhere cooled every link in the address space, so every direct edge afterwards cost a round trip to re-arm however far the write was from anything translated -- 69 percent of headless Chromium's exits and 53 percent of Wine's. The address space already hands a reader the mapping ranges it has not seen; recording the same for code lets a reader that is only a few changes behind replay exactly those ranges through the selective retirement the cache already performs for its own writes, and retire everything only when it cannot describe the whole span. Geekbench exits fall a further 38.8 percent and link_exit 75.3. Chromium 43.0s to 40.5s and Wine 195.5s to 185.5s; Geekbench itself does not move, Integer 50.0 to 50.0 and Floating Point 77.0 to 77.0, because after X21, X22 and X24 almost none of its remaining exits are cooled links. The durable lesson is about the first attempt, not the design: publication happens on the code-write path inside translated execution, and putting a `std::shared_mutex` there cost more than the change saved -- 933s against 764s -- because libc++ builds one from a mutex and a condition variable, so every unlock notifies and ten vCPUs serialize. A sample found it in one pass, with `pthread_mutex_lock` and `psynch_mutexwait` above every gadget. A sequence-stamped lock-free ring replaced it. |
| X26 | Reopen the instruction window instead of leaving at it | Rejected. After X25 the per-entry window was 58 percent of what remained: the engine left the stream every 4096 retired instructions purely to ask whether an interrupt had been raised or somebody had published a code change, and the answer is almost always no. Asking in the stream at the same checkpoints, and opening the next window when nothing has happened, keeps every bound the window enforced -- 94 percent of checks then continue, and Geekbench exits fall 52.7 percent, to a tenth of what they were before this campaign. It does not pay. Geekbench is flat (Integer 49.5 both ways, Floating Point 75.5 to 78.5 disjoint, whole-suite elapsed +0.2 percent) and Chromium is 5.4 percent slower with disjoint distributions across four runs of the changed arm. The exits removed were the cheapest ones: a window exit re-enters through the successor chain without revalidating anything, while the exits that remain miss the cache or cool links. A diagnostic build that dropped the in-stream code-change test to see whether eager checking was the cost ran 8.4 percent slower again, so that test is load-bearing rather than the problem. The lesson is that exits are not fungible: a census counts them equally and this campaign's three accepted changes removed 91 percent of them, but what is left is what was expensive all along. |

Host sampling profiles this engine without instrumenting it. The bank publishes
a symbol per gadget, so a sampled host PC attributes straight to the gadget that
owned it; assembler-local labels fold into their routine, and self time is a
frame's count less its children's. Threaded dispatch leaves no usable frame
chain -- x29 is the retired-instruction counter, not a frame pointer -- but the
deepest frame is the real PC, which is all a gadget profile needs, and a 5 ms
interval left the scored rate unchanged.

The profile that opened this campaign covered Geekbench 6.7.1 alone and put
roughly 37 percent of emulator time in leaving and re-entering the stream --
block selection, cache validation, link activation and the surrounding loop --
with flag materialization the largest single symbol at 9.8 percent, top-four in
every workload and peaking at 23 percent in Horizon Detection, and
self-modifying-code bookkeeping at 6.1 percent.

The current profile spans a wider set: Geekbench 5.5.1, 6.7.1 and 7.0.0
single-core, a headless Chromium rendering a local page whose script is the
timed work, and Wine, each workload set weighted equally and each sampled
across one boot running its whole suite. Stream re-entry is 21.7 percent, all
flag work together -- the materializer, its tables and the carry-only entry
point -- is 5.3 percent, address translation is 5.9, and self-modifying-code
bookkeeping is 6.9. No individual gadget exceeds about 2 percent.

So the two items this campaign attacked are the two that moved, and
bookkeeping for guest code writes did not: publishing a write, invalidating the
cached programs that overlap it and resolving the written page are now the
largest cluster after stream re-entry. That path calls into C++ on every guest
store whose page could hold code, and X8's lesson is that the predicate is true
too often to branch on, so the work has to become cheaper rather than
conditional.

X21, X22, X24 and X25 have since removed 81 percent of Geekbench 6's stream
exits, and X26 establishes that the remainder is not worth counting: removing
another 53 percent of them changed nothing. What is left of stream re-entry is
the exits that miss the program cache or re-arm a cooled link, so further work
belongs in translation and validation rather than in the count of returns.

## Graphics backend investigations

| Attempt | Disposition and lesson |
| --- | --- |
| Separate CGL/OpenGL and mobile renderer paths | Replaced by one authenticated host GL provider across Apple products; that provider is now Mesa Zink over KosmicKrisp. |
| Complete GLES 3.2 through existing APIs | Not viable as a single current Apple/Virgl path; the product claims only the qualified GLES boundary in [`graphics.md`](graphics.md). |
| Add Vulkan by pairing Venus with MoltenVK | Not implemented. MoltenVK does not supply the missing Virtio-GPU blob/shared-memory protocol, synchronization, or lifetime model. |
| CPU readback as native presentation | Retained only as a diagnostic fallback; sustained native qualification requires the Metal texture path. |
| Profile Chromium H.264 playback | Accepted. Host sampling showed one-millisecond fence polling and VNC scanout readback dominated the GPU thread while VideoToolbox decode was a small share. Packed damage readback, persistent staging, direct framebuffer rows, predicate-aware 100-microsecond fence polling, reusable codec buffers, and NV12 IOSurface/Metal copies raised matched 10-second Chromium callbacks from 85 to 128 and cut dropped frames from 126 to 63. Polling shared syncs from the active guest context was rejected because the host provider retired them more slowly; context-zero polling remains. |
| Defer Virgl fence completion | Accepted. Host-owned per-context timeline cookies allow multiple fenced submissions and transfers to remain in flight without blocking the GPU transport thread. Matched Chromium playback delivered more good frames with roughly flat host CPU use; renderer-state mutations remain barriers. |
| Post-deferred-fence Chromium H.264 candidates | Rejected after a fresh 1080p60 campaign with separate 30-second desktop and browser cool-downs and 60-second A-B-B-A measurement blocks. The gadget link front memo passed its first block, but four blocks averaged 0.72 percent, included negative pairs, and had a family-wise lower bound below zero; per-context detach bypass, dropping no-client scanout and flush commands, compositor removal, VideoToolbox decode-worker offload, threaded Virgl sync, and event-driven fence retirement failed the initial performance or correctness screen. VideoToolbox and fence waits remain mostly blocked wall time while vCPU execution dominates actual CPU. |
| VideoToolbox H.264 candidate sweep | Rejected under the same 1080p60 cool-down protocol. Bounded Metal-copy retry and depth-two asynchronous decode passed their first blocks, but four-block means were negative and included negative pairs; activation evidence confirmed that the asynchronous path executed. Descriptor narrowing, RBSP initialization, direct single-iovec input, framebuffer reuse, cache-flush frequency, parameter-set caching, and command-flush coalescing failed the initial screen. Direct decoder-output aliasing remains invalid at the resource-semantics gate; the synchronous decode and Metal-copy path remains the baseline. |
| Use the native JIT for Chromium H.264 playback | Rejected. Translation-cache churn stalled this browser workload before playback, so the qualified gadget engine remains the performance baseline. |
| Gadget-cache and TLB follow-up for Chromium H.264 | Rejected under the settled 1080p60 A-B-B-A protocol. Instrumentation found each vCPU's 65,536-entry program cache nearly full, about 122,000 translations and 1.57 million lookups per second, and a 92.25 percent hit rate. Stable link storage, alternate hashes and stream indices, lookup and deactivation memos, kiosk launch, cache capacity, associativity and replacement changes, fewer resident program generations, and a larger reverse index either regressed, produced mixed paired signs, or lost required decode evidence. Four resident TLBs produced eight positive pairings but failed the family-wise lower bound; two resident TLBs produced three positive valid blocks but later wedged early guest boot, so both were reverted. A non-video follow-up was neutral on eight-worker SHA-256 throughput, with two paired effects below one percent and flat wall time, but showed positive process/exec timings; one pair was dominated by a slow baseline while the other reduced time by 1.2 percent. Translation churn is real, but retaining address-space-local state requires a new correctness-preserving generation and lifetime design. |
| Extend VideoToolbox decode to HEVC and P010 | Accepted. A four-vCPU 1080p60 A-B-B-A qualification with separate 30-second desktop and browser cool-downs measured 15.7x HEVC Main and 30.1x HEVC Main10 decoder throughput over four-thread software decode, with 97.2 and 98.7 percent less guest decode CPU time. The upstream Debian Chromium build has no software HEVC decoder, so the same-file hardware/software comparison used upstream guest FFmpeg while separate 60-second Chromium runs qualified VA-API and VideoToolbox end to end. Chromium delivered about 49.3 good frames per second for both profiles, leaving presentation and guest GPU work, rather than decode, as the 60 fps bottleneck. |
| Extend stock-guest VideoToolbox formats | Accepted HEVC Main and Main10 through 8192x4320 at level 6.2 and JPEG Baseline/MJPEG through 4096x2304. A settled four-vCPU 1080p60 A-B-B-A comparison against four-thread software decode measured 13.0x, 24.4x, and 3.8x wall-time speedups for HEVC Main, HEVC Main10, and MJPEG, with 96.7, 98.4, and 72.3 percent less guest decode CPU time. Both 8-bit and 10-bit HEVC decoded correctly at 7680x4320 level 6.1, and MJPEG hardware output matched software within sub-LSB rounding error. H.264 High10 was rejected on macOS after native frame validation exposed a false-positive session probe; 4:2:2 and 4:4:4 were rejected because stock Debian libva and Mesa do not expose the required profiles and surfaces. No guest-library fork is required or supported. |
| Bound KosmicKrisp Metal retention by Vulkan lifetime | Accepted. Finite raster variants, owner-based pipeline retirement, command-allocator reset and completion-based release of one-time recordings prevent storage from accumulating with submission count. A sustained 2,465-case GL shard completed without failures and recovered memory after transient peaks. Guest RAM limits do not bound native graphics allocations. |
| Avoid raster expansion when rasterizer discard is enabled | Accepted. A dedicated discard class retains transform feedback and shader side effects while eliminating unused raster output. The previously timing-out geometry workload completes; discarded rasterization does not permit dropping earlier programmable stages. |
