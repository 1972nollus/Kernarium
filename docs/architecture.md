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

Opt-in build-local JIT reporting follows the same dependency direction: the
app calls a private emulator facade, the emulator calls a runtime-owned bridge,
and only that runtime bridge includes the CPU reporting contract. The facade is
not part of the installed emulator ABI. The CPU-owned executor-pool profiler is
compiled only into that profiling graph. It aggregates fixed wrapper-local
records through a quiescent private registry; the borrow path never updates a
process-global profiling counter. Successful borrows update bounded wrapper-
local session/sequence slot masks. Quiescent aggregation keys them by session
and sequence, so address reuse across runs remains distinct, and emits only
fanout indicators rather than raw keys.

## Test ownership and taxonomy

The enforced test taxonomy is suite-level. Every first-party CTest registration
has exactly one primary label, `unit`, `tooling`, or `integration`, and that
label must match `tests/test-manifest.tsv`. Registration helpers and canonical
owner paths make the suite boundary mechanically auditable. Domain information
belongs in test names, manifest ownership, and source location; secondary
per-domain labels and any particular per-domain subdirectory layout are
optional organization rather than architecture or acceptance requirements.

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
for iOS; the shared bridge retains only software-MMU entry points there.
Executable-memory ownership is enforced in source architecture and functional
JIT publication tests, not by comparing or attributing individual archive
members.

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
