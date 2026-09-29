# x86-64 instruction unit corpus

`x86_64_v3.pack` is the self-contained instruction input for the ordinary
`x86-64-instruction-corpus` CTest. It contains compact per-instruction traces
derived from the pinned K x86-64 semantics tests, complementary handwritten
FEX assembly programs, and the repository's own cases in `cases/`. CMake,
CTest, and the test executable do not need either upstream checkout, an
assembler, Python, or a network connection. The x86-64 guest exists only in
4 KiB builds, so the test runs in the 4 KiB unit tree.

The profile is portable x86-64-v3 userspace plus the AES, PCLMUL, XSAVE,
FSGSBASE, and ADX extensions already owned by the product contract. Intel-only
facilities, privileged instructions, obsolete vendor extensions, and
post-AVX2 extensions are outside this corpus. `excluded.tsv` records every
producer input omitted by the profile, rejected by upstream, invalid as
x86-64, or removed as an exact duplicate. An included illegal instruction is
always a test failure.

`manifest.tsv` binds every packed record to its origin, input hashes, semantic
forms, and case count. `coverage.tsv` is the merged form inventory and
`provenance.json` binds the pack to the pinned producer commits and Intel SDM
revision. FEX and native-Intel records are bit-exact. K-only NaN payload and
sign choices are compared by IEEE NaN class only for the explicitly classified
floating-point programs where Intel permits multiple quiet-NaN results; every
other K trace remains bit-exact. Focused unit cases retain exact x86
indefinite-NaN checks.

x87 is emulated only at full 80-bit precision. The producer drops FEX's
`FEX_X87REDUCEDPRECISION` request, which only FEX's own translator honours,
and keeps those programs' expected registers: FEX's x86-64 host runner checks
every ASM program natively, so they are full-precision hardware results. The
test runner rejects a case that still carries the request.
The upstream license notices are retained alongside the pack.

## Repository cases

x86 instruction correctness cases that neither upstream covers belong in
`cases/`, not in new focused tests; a focused test stays only where it needs
an engine hook the runner cannot give a guest program, such as a second
executor or engine-internal state. Each case is a GNU-syntax `.s` file whose
`_start` is linked at `0x10000` and ends at a `HLT`. One `/* CONFIG ... */`
block holds FEX's `MemoryRegions` (page-aligned address to size) and
`RegData` (RIP, the GPRs, and `XMM0`-`XMM15` as two or four 64-bit lanes)
with the same meaning FEX gives them. A case starts from zeroed registers
with IF set and may rely on no mapping but its code and its regions. The only
system call a case may make is `time(NULL)` (RAX 201, RDI 0), which the runner
answers with 1 and the native run with the time, so a case that makes it
leaves RAX undeclared.

A case's expectation has the authority of the other origins: the producer
accepts it only when a native Intel run of the case, from the same state to
its `HLT`, ends with exactly the declared values. The runner dispatches these
cases through the executor's program cache on the product's code-page model,
where every page may hold code and only a publication moves a page's stamp,
so a case can also hold the engine to its code-publication contract. Upstream
FEX programs keep building each block afresh. Run one case with
`uml_x86_64_fex_asm_runner --repository BINARY CONFIG`.

Regenerate only from verified producer inputs:

```sh
dependencies/prepare.sh --product x86-conformance \
  --root "$CASE_SENSITIVE_DEPENDENCY_ROOT"
python3 -B tools/fex-tests/build-asm-corpus.py \
  "$CASE_SENSITIVE_DEPENDENCY_ROOT/fex" .build/fex-tests/corpus-x86-64 \
  --mode x86-64
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --native-request .build/x86-tests/native-k-request.bin \
  --repository-request .build/x86-tests/repository-request.bin
clang++ --target=x86_64-unknown-linux-gnu -std=c++20 -O2 \
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
  -fno-builtin -fno-pie -mno-red-zone -nostdlib -static -fuse-ld=lld \
  -Wl,-e,_start -Wl,--build-id=none -Wall -Wextra -Werror \
  tools/x86-tests/native-k-trace.cpp -o .build/x86-tests/native-k-trace
```

Run `native-k-trace` on an Intel Linux host once with each request on
standard input, saving its standard output as `native-k-response.bin` and
`repository-response.bin`. The helper verifies the required x86-64-v3
CPU/XSAVE profile and fails on `SIGILL` or on any case that does not reach its
`HLT`. A new or edited repository case needs only a fresh repository response.
Complete the pack locally with:

```sh
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --fex-corpus .build/fex-tests/corpus-x86-64 \
  --native-response .build/x86-tests/native-k-response.bin \
  --repository-response .build/x86-tests/repository-response.bin
```

Use the final command with `--check` for a byte-for-byte freshness check.
