# FEX instruction conformance corpus

The x86 gadget conformance campaign consumes the handwritten x86-64 and i386
assembly tests from the pinned `x86-conformance` dependency. FEX is a test
input only; no FEX source or runtime library is linked into an emulator
product.

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
status, and content hashes. The producer rejects a dirty or incorrectly pinned
FEX checkout and publishes the output directory only after every source has
assembled successfully.

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
