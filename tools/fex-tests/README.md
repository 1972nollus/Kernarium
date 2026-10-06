# FEX instruction conformance corpus

The x86 gadget conformance campaign consumes the handwritten x86-64 and i386
assembly tests from the pinned `x86-conformance` dependency, for which FEX is
a test input only. The macOS dependency prefix builds FEXCore from the same
row for the macOS-only x86-64 JIT; see
[`dependencies/README.md`](../../dependencies/README.md).

Prepare the exact upstream source with:

```sh
dependencies/prepare.sh --product x86-conformance
```

If NASM is not already available, build the pinned producer locally without
installing it system-wide:

```sh
tools/fex-tests/build-nasm.sh \
  "$PWD/external/sources/nasm" \
  "$PWD/.build/fex-tests/nasm"
```

With NASM available, materialize both raw test corpora and their FEX
configuration records with:

```sh
python3 tools/fex-tests/build-asm-corpus.py \
  external/sources/fex OUTPUT_X86_64_DIRECTORY --mode x86-64
python3 tools/fex-tests/build-asm-corpus.py \
  external/sources/fex OUTPUT_I386_DIRECTORY --mode i386
```

Set `UML_FEX_NASM` to the absolute producer path when it is not on `PATH`.

When the prepared checkout must cross a machine boundary, export only its
required files and their hashes first:

```sh
python3 tools/fex-tests/export-asm-source.py \
  external/sources/fex EXPORTED_SOURCE_DIRECTORY
```

The corpus producer accepts either the verified prepared Git checkout or this
reduced, content-verified export.

The output is disposable build data. `corpus.json` records the pinned FEX
commit, every source-relative test name, feature requirements, disabled-test
and known-failure status, and content hashes. The producer rejects a dirty FEX checkout and one
that is not based on the pinned commit or whose `LICENSE`, `Scripts`, or
`unittests` differ from it, so a prepared source carrying a maintained patch
series yields the pinned corpus. It publishes the output directory only after
every source has assembled successfully.

Build the test-only gadgets adapter from a 4 KiB unit configuration, then run
both complete corpora with:

```sh
cmake --build BUILD_DIRECTORY --target uml_x86_64_fex_asm_runner
python3 tools/fex-tests/run-asm-corpus.py \
  BUILD_DIRECTORY/bin/uml_x86_64_fex_asm_runner \
  OUTPUT_X86_64_DIRECTORY REPORT_X86_64.json
python3 tools/fex-tests/run-asm-corpus.py \
  BUILD_DIRECTORY/bin/uml_x86_64_fex_asm_runner \
  OUTPUT_I386_DIRECTORY REPORT_I386.json
```

The runner reports each case as one tab-separated line of `key=value` fields
that names the binary it ran; the campaign rejects a line naming another input.
The `x86-64-fex-asm-campaign-runner` tooling test runs the campaign over the
runner on generated inputs, so the two cannot drift apart.

The campaign executes every selected binary, including upstream-disabled
inputs, but excludes those upstream-disabled results from the applicable pass
rate. It never filters tests according to host CPU features: those annotations
describe guest instructions that the gadgets backend must implement. The JSON
report retains every per-test terminal state and aggregates decoder failures by
opcode map so implementation can proceed from measured coverage gaps.

FEX's generic `Disabled_Tests` entries are retained as explicit unsupported
upstream cases. Host-, simulator-, and FEX-JIT-specific exclusions do not
apply to the gadget backend.

Some FEX expectations are FEX's own behaviour where x86 hardware differs. The
report gives the raw pass rate over every applicable test and the rate under
the policy, which sets aside the tests upstream's `Known_Failures_host` names
as failing on native x86 and those the project records in
[`contradicted-expectations.tsv`](contradicted-expectations.tsv), each with the
native Intel evidence. A set-aside test still runs, its result stays in the
report, and one that passes is listed as matching FEX rather than hardware.
The campaign's status follows the policy, and a record row that names no test
fails it. An engine that follows x86 fails these tests in the raw count.

The engine emulates x87 only at full 80-bit precision, and the runner refuses
a case that asks for FEX's reduced-precision mode. The campaign therefore runs
those programs with `FEX_X87REDUCEDPRECISION` removed from the configuration it
hands the runner, as the unit corpus producer does: FEX checks every ASM
program natively, where the request has no effect, so their expected registers
are full-precision results. The report counts the removed requests.
