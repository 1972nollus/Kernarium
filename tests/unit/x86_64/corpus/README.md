# x86-64 instruction unit corpus

`x86_64_v3.pack` is the self-contained instruction input for the ordinary
`x86-64-instruction-corpus` CTest. It contains compact per-instruction traces
derived from the pinned K x86-64 semantics tests and complementary handwritten
FEX assembly programs. CMake, CTest, and the test executable do not need either
upstream checkout, an assembler, Python, or a network connection.

The profile is portable x86-64-v3 userspace plus the AES, PCLMUL, XSAVE, and
FSGSBASE extensions already owned by the product contract. Intel-only
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
The upstream license notices are retained alongside the pack.

Regenerate only from verified producer inputs:

```sh
dependencies/prepare.sh --product x86-conformance \
  --root "$CASE_SENSITIVE_DEPENDENCY_ROOT"
python3 -B tools/fex-tests/build-asm-corpus.py \
  "$CASE_SENSITIVE_DEPENDENCY_ROOT/fex" .build/fex-tests/corpus-x86-64 \
  --mode x86-64
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --native-request .build/x86-tests/native-k-request.bin
clang++ --target=x86_64-unknown-linux-gnu -std=c++20 -O2 \
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
  -fno-builtin -fno-pie -mno-red-zone -nostdlib -static -fuse-ld=lld \
  -Wl,-e,_start -Wl,--build-id=none -Wall -Wextra -Werror \
  tools/x86-tests/native-k-trace.cpp -o .build/x86-tests/native-k-trace
```

Run `native-k-trace` on an Intel Linux host with the request on standard input
and save its standard output as `native-k-response.bin`. The helper verifies
the required x86-64-v3 CPU/XSAVE profile and fails on `SIGILL`. Complete the
pack locally with:

```sh
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --fex-corpus .build/fex-tests/corpus-x86-64 \
  --native-response .build/x86-tests/native-k-response.bin
```

Use the final command with `--check` for a byte-for-byte freshness check.
