# Architecture

The enforced first-party dependency direction is:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

`darwin/CMakeLists.txt` composes owning subdirectories; source manifests and
targets reside with their modules. The concrete interpreter, gadget, and JIT
adapters are under `cpu/arm64/{interpreter,gadgets,jit}`. The neutral
`cpu/engine_factory.cpp` selects one adapter without containing an engine
loop. Runtime aggregates those owned objects in order and supplies the single
guest-entry boundary.

`cpu/arm64/layout/assembly_offsets.h` is the neutral assembly-visible
CPU/TLB layout contract. C++ static assertions in
`cpu/arm64/layout/assembly_layout.hpp` and
the assembly consumers verify it. The JIT does not include gadget
implementation files.

## Executable-memory policy

Interpreter state, gadget program buffers, software-MMU backing, and 4 KiB
native shadow aliases are sandbox-safe data. The precompiled gadget
implementation stays in ordinary signed RX text. Neither engine contains an
executable-allocation or code-publication primitive or performs an RW-to-RX
transition. Guest execute permission in the software MMU is metadata used by
instruction fetch, not a Darwin VM permission.

Dynamic code belongs only to the explicitly selected JIT. All `MAP_JIT`, JIT
write-protection, and instruction-cache publication logic is owned by
`cpu/arm64/jit/jit_arena.cpp`. The JIT stays in iOS products for entitled
debugger or jailbreak deployments, while sandboxed consumers can select the
interpreter or precompiled gadget engine without entering that path.

The direct native-VM branch and ELF loader are macOS-only and are compiled out
for iOS; the shared bridge object retains only software-MMU entry points there.
Apple packaging rejects an iOS device or simulator archive when an
executable-memory primitive is referenced by any object other than the JIT
arena.

`architecture-audit` combines four checks. Documentation paths in this file
are validated by the same target, so a moved contract cannot leave a stale
architecture reference behind:

- CMake File API direct-link layer rules and dependency-cycle detection;
- CPU/memory/device implementation include rules, including assembly and
  `.inc` files;
- unowned implementation, empty-directory, retired-terminology, and
  implementation-fragment checks; and
- the explicit CTest manifest and advisory source-size review records.

The audit does not prove generated-code shape, performance, or a kernel boot.
Those remain separate measured acceptance gates. The 800-line production,
600-line test/support, and 300-line CMake guidelines are advisory. Crossing a
guideline is always reported and never fails the audit; large cohesive units
may remain without an exception. Optional owner, reviewed-size, date, and
rationale metadata lives in
[`source-size-reviews.tsv`](source-size-reviews.tsv). Malformed, duplicate, or
stale review records still fail because they would make the report misleading.
The report covers first-party code and support sources below `cmake`, `darwin`,
`dependencies`, `kernel`, `scripts`, `tests`, and `tools`; recognized
declarative configuration, manifests, and patches are included as
informational rows without a numeric guideline.

The canonical host/kernel C ABI is in [`../kernel/abi`](../kernel/abi). Its
version/fingerprint and host/kernel layout assertions fail compilation when a
guest-register boundary drifts.
