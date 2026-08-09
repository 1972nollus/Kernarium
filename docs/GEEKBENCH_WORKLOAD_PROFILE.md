# Geekbench 6 workload profile

> Historical G9 profile analysis. The current runner uses the Geekbench 6
> payload from `rootfs.erofs` under `/opt` with `--no-upload`; the
> user-directed final scoring run was skipped, so this capture is not evidence
> for the EROFS migration.

## Capture

The accepted G9 run captured one five-second macOS stack sample at the start
of every Geekbench 6 CPU workload in both phases: **32/32 sections**. Its raw
profile is `/private/tmp/geekbench-g9-retry-profile`; the console is
`/private/tmp/geekbench-g9-retry-console.log`.

`tests/perf/analyze_geekbench_workloads.py` converts `workloads.tsv` and the
sample files into a per-workload hotspot matrix. The generated detailed
matrix for this run is `/private/tmp/geekbench-g9-workload-hotspots.tsv`.
Percentages below are shares of active emulator top-of-stack samples, after
excluding sleeping host-library threads. A five-second sample identifies the
dominant steady instruction sites but may not cover every internal phase of a
long workload.

## Single-core matrix

| Workload | Memory | Control | Integer | FP/SIMD | Runtime | Leading site |
|---|---:|---:|---:|---:|---:|---|
| File Compression | 25.2% | 8.4% | 63.1% | 1.1% | 0.1% | register SUBS |
| Navigation | 50.3% | 16.6% | 24.2% | 1.7% | 0.0% | 64-bit load |
| HTML5 Browser | 39.8% | 17.4% | 28.0% | 1.0% | 5.4% | 64-bit load |
| PDF Renderer | 43.3% | 11.0% | 42.9% | 0.2% | 0.6% | 32-bit load |
| Photo Library | 42.0% | 17.2% | 29.5% | 0.0% | 3.3% | 64-bit load |
| Clang | 31.5% | 14.3% | 28.2% | 0.0% | 19.8% | 64-bit load |
| Text Processing | 40.3% | 16.9% | 24.2% | 0.0% | 10.0% | 64-bit load |
| Asset Compression | 29.1% | 7.0% | 30.9% | 27.2% | 0.3% | register SUB |
| Object Detection | 34.8% | 6.4% | 22.7% | 34.0% | 0.4% | 128-bit load |
| Background Blur | 43.7% | 4.9% | 9.3% | 40.4% | 0.3% | pair 128-bit load |
| Horizon Detection | 40.8% | 9.2% | 34.4% | 11.4% | 0.6% | pair 128-bit load |
| Object Remover | 34.5% | 26.8% | 37.0% | 0.5% | 0.0% | indexed byte load |
| HDR | 36.4% | 8.7% | 29.2% | 23.1% | 0.4% | indexed 32-bit store |
| Photo Filter | 39.0% | 26.6% | 31.4% | 1.3% | 0.2% | compare-and-branch |
| Ray Tracer | 48.0% | 17.6% | 19.9% | 2.4% | 2.9% | 64-bit load |
| Structure from Motion | 35.1% | 8.6% | 14.8% | 38.0% | 0.3% | pair 128-bit load |

## Multi-core matrix

| Workload | Memory | Control | Integer | FP/SIMD | Runtime | Leading site |
|---|---:|---:|---:|---:|---:|---|
| File Compression | 26.3% | 8.5% | 51.2% | 4.8% | 6.2% | immediate ADD |
| Navigation | 53.4% | 16.9% | 22.9% | 1.4% | 1.1% | 64-bit load |
| HTML5 Browser | 36.1% | 16.4% | 29.4% | 1.3% | 7.7% | 64-bit load |
| PDF Renderer | 39.1% | 11.8% | 40.2% | 2.4% | 3.1% | 32-bit load |
| Photo Library | 42.7% | 18.0% | 28.5% | 0.0% | 3.5% | 64-bit load |
| Clang | 31.8% | 11.8% | 29.5% | 0.0% | 19.2% | 64-bit load |
| Text Processing | 38.8% | 15.8% | 24.6% | 2.8% | 9.7% | 64-bit load |
| Asset Compression | 29.8% | 7.8% | 27.2% | 28.4% | 0.9% | immediate ADD |
| Object Detection | 35.4% | 7.1% | 25.0% | 27.7% | 2.5% | register ADD |
| Background Blur | 44.0% | 5.2% | 10.4% | 37.2% | 1.2% | pair 128-bit load |
| Horizon Detection | 40.0% | 8.3% | 38.2% | 7.4% | 1.4% | byte load |
| Object Remover | 36.3% | 25.8% | 35.9% | 0.6% | 0.1% | indexed byte load |
| HDR | 30.0% | 2.9% | 17.4% | 30.8% | 4.3% | engine leave boundary |
| Photo Filter | 38.9% | 25.5% | 33.2% | 0.7% | 0.0% | compare-and-branch |
| Ray Tracer | 48.5% | 17.7% | 19.3% | 2.0% | 3.3% | 64-bit load |
| Structure from Motion | 29.6% | 6.4% | 14.3% | 42.2% | 4.4% | SIMD table lookup |

## Findings and trial order

1. Memory is the broadest bottleneck. It leads 13 of 16 single-core
   workloads and reaches 53.4% in multi-core Navigation. G11 therefore moved
   TLB entries to offset zero and removed one address-generation instruction
   from every production memory lookup.
2. Pair-vector translation is the largest concentrated opportunity.
   `vector_pair_load128_0` alone accounts for 29.7% of single-core and 27.6%
   of multi-core Background Blur. The existing gadget proves both transfers
   are in one guest page, then performs the identical TLB lookup twice.
3. Clang is the cold-translation outlier. Builder, cache linking, and
   `run_block` together explain roughly one fifth of both phases, so
   translation allocation/decode remains a separate target after memory.
4. File Compression is dominated by integer arithmetic and flags rather than
   the memory subsystem. It needs an integer-specific profile before another
   safe change is selected.
5. Photo Filter and Object Remover are control-heavy. Their leading linked
   compare/conditional branches are already specialized, limiting low-risk
   branch work.
6. Multi-core HDR's 12.8% `uml_arm64_gadget_leave` share is unique and should
   be investigated with full call trees before changing the global fairness
   or interrupt boundary.
7. Structure from Motion spends 9.0% of its single-core samples and 9.4% of
   its multi-core samples in one-table SIMD lookup gadgets. G24 predecodes
   their three vector offsets at translation time, improving focused
   single-worker and ten-worker controls by 2.32% and 3.27%, respectively,
   without regressing the untouched two-table form.

## Optimization status

The accepted workload-driven stack through G35 contains the entries-first
TLB, same-page pair translation reuse, native host pair transfers, scalar-byte
specialization, inline direct and indirect link records, and predecoded
one-table SIMD lookup, indexed-FP, and basic vector-FP offsets. G21 through
G23 were rejected after extended controls found no gain or measurable
regressions. G25 improves the indexed FMLA/FMUL family in Background Blur;
G26 improves the basic vector `FADD`/`FMUL`/`FSUB` family seen in Asset
Compression, Background Blur, and Structure from Motion. Remaining
workload-local targets need separate investigation: G27 covers Object
Detection's same-width integer SIMD add, while G28 found no gain from
predecoding its pairwise-long addresses. G29 covers Object Detection's
widening multiply path with 2.61% single-worker and 3.02% ten-worker focused
gains. G30 addresses multi-core HDR's unique engine-leave behavior by retaining
the native guest FP environment across internal block exits; its exit-heavy
ten-worker control improves 31.0% while a normal single-worker FP control
regresses 0.70%. G31 removes the duplicate link-map hash on cold misses, and
G32 caps the Builder's initial reserve at 34 words. On the standalone
Clang-shaped `/bin/true` fork/exec control they improve the ten-worker path by
0.92% and 0.64%, respectively; G32 also improves the matched single-worker
control by 6.39%. Attempts to retain active link pointers or map nodes
(G33/G34) regressed single- or multi-worker controls and were removed. G35
then addresses the common integer MOV alias found across browser, compiler,
and text workloads: its exact control improves 25.3% with one worker and
25.1% with ten, while a non-alias logical control remains neutral.

## Workload bottleneck disposition

The profile predates G11–G35, so its percentages are selection evidence rather
than claims about the final binary. Each row below accounts for both the
single-core and multi-core section of the named Geekbench workload.

| Workload | Profile bottleneck | Implemented or investigated | Remaining limit |
|---|---|---|---|
| File Compression | Integer add/sub, flags, scalar memory | G5 keeps NZCV resident; G35 removes MOV-alias work; G36 ADD base reuse measured neutral and was removed | Hot add/sub entries are already opcode-free native arithmetic; compare/branch fusion was explicitly reverted |
| Navigation | 50–53% memory, linked comparisons | G11, G13, G15, G16; linked compare branches already specialized | Safe 4 KiB TLB lookup and cross-page detection dominate |
| HTML5 Browser | Memory, control, runtime, MOV alias | G11–G19, G31/G32, G35 | Remaining sites are dispersed loads, pair transfers, and block boundaries |
| PDF Renderer | Scalar memory and integer multiply/add | G11, G13, G15, G16 | Native multiply/add is already branch-free; memory still requires TLB validation |
| Photo Library | Memory, linked comparisons, MOV alias | G11–G19, G35 | General load/store translation remains dominant |
| Clang | 19% cold translation/runtime plus memory | G9, G18/G19, G31/G32, G35 | Builder, cache link, and block-entry costs are distributed; retained-node/pointer variants G33/G34 regressed |
| Text Processing | Memory, linked comparisons, runtime, MOV alias | G11–G19, G31/G32, G35 | Remaining runtime share is block-boundary and safe lookup overhead |
| Asset Compression | Integer arithmetic and vector FP | G25/G26 and G35 | Remaining integer entries are native, specialized, and branch-free |
| Object Detection | SIMD widening/add/pairwise-long and 128-bit memory | G27/G29 retained; G28 measured and rejected | SADDLP predecode gave no gain; 128-bit memory needs safe page validation |
| Background Blur | Pair 128-bit memory and indexed FMLA | G12/G14 and G25/G26 | Pair path already reuses translation and native host LDP |
| Horizon Detection | Pair/scalar memory and integer arithmetic | G12–G16 | Software-MMU lookup remains the leading unavoidable cost |
| Object Remover | Indexed byte memory and control | G16 plus specialized linked branches | One-byte path already omits cross-page checks; further branch fusion is out of scope |
| HDR | Memory, FP/SIMD, engine exits | G11–G16 and G30 | Remaining exits enforce syscall/fault/fairness boundaries |
| Photo Filter | Linked compare/conditional branches and memory | Existing specialized compare/branch entries; G11–G16 | Data-only block chaining/fusion would be the next step but is deliberately not retained |
| Ray Tracer | 48% general memory | G11, G13–G16 | Safe TLB lookup dominates; combined-check experiments G23 regressed |
| Structure from Motion | Pair memory, TBL, vector FP | G12/G14, G24, G26 | Remaining SIMD and memory sites are already native specialized entries |

Across all 32 sections, the remaining broad cost is therefore the validated
software-MMU lookup, not guest opcode decoding: production memory gadgets
retain only the cross-page and TLB/fault decisions needed for architectural
correctness. The remaining concentrated compute sites either have a retained
specialization above or a measured rejection in `HOT_PATH_EXPERIMENTS.md`.
