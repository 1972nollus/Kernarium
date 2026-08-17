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
native shadow aliases are sandbox-safe data. This includes the process-wide
store of gadget translations executors adopt from one another: a gadget
program is a data stream of pointers into ordinary signed RX text, so sharing
one copies data between heap allocations and allocates nothing executable. The
precompiled gadget implementation stays in ordinary signed RX text. Neither engine contains an
executable-allocation or code-publication primitive or performs an RW-to-RX
transition. Guest execute permission in the software MMU is metadata used by
instruction fetch, not a Darwin VM permission.

Dynamic code belongs only to the explicitly selected JIT. All `MAP_JIT`, JIT
write-protection, and instruction-cache publication logic is owned by
`cpu/arm64/jit/jit_arena.cpp`. The JIT stays in iOS products for entitled
debugger or jailbreak deployments, while sandboxed consumers can select the
interpreter or precompiled gadget engine without entering that path.

The direct native-VM branch and ELF loader are compiled out of ordinary iOS
device and Simulator libraries; the shared bridge retains only software-MMU
entry points there. The opt-in Kernarium composition builds one versioned
runtime framework for each maintained 4 KiB and 16 KiB page profile, enabling
the existing native bridge and embedded loader for arm64 iOS device and
Simulator kernel runs. It selects the precompiled gadget engine and does not
alter the supported iOS package graph or authorize executable JIT publication.
The application links neither runtime at launch. Its always-linked C loader
masks the early process-directed timer before main, dynamically loads exactly
the saved framework, validates a versioned immutable function table, and
rejects a second page profile for that process lifetime.

On both iOS targets each kernel ELF is a 16 KiB-aligned section of its
framework's code-signed Mach-O `__TEXT`. The loader derives a RAM-scaled native
window below the iOS ceiling and uses `vm_remap` to alias only immutable,
relocation-free executable ELF pages from that signed mapping; non-executable
data and BSS remain ordinary anonymous mappings. The Darwin kernel link keeps
signed executable boundaries on 16 KiB host pages even for a 4 KiB guest.
Simulator, physical device, and macOS consume the exact same canonical bytes
within each profile: `/Volumes/<case-sensitive-volume>/build-16k/vmlinux` or
`/Volumes/<case-sensitive-volume>/build-4k/vmlinux`. Simulator and physical device carry
them through the same signed framework strategy. macOS retains its established
unsigned CLI embedding and uses the same guest range, RAM-scaled window
derivation, relocation, and exclusion-hole handling, but places that window at
96 TiB rather than inside iOS's constrained sub-62 GiB host range.
Product policy still lets
macOS default to 8 GiB and ten vCPUs while Kernarium defaults to 1.5 GiB and all
processors reported by iOS; the app can persist a smaller launch configuration
within its supported settings ranges. The common layout derives the appropriate
native window from each guest's RAM. Guest page size remains a compile-time
kernel and host-runtime choice, so the two profiles share architecture and
source but not a binary framework or kernel artifact. Kernarium defaults to
16 KiB and persists a 4 KiB compatibility selection for the next process
launch.
Executable-memory ownership is enforced in source
architecture and functional JIT publication tests, not by comparing or
attributing individual archive members.

## Darwin scheduling boundary

The maintained Darwin Linux port owns one persistent pthread per configured
vCPU, including vCPU0. Linux tasks retain their stackful runtime context on
guest kernel stacks and the ordinary scheduler resumes that context on the
selected carrier; task creation, exit, and migration do not create or retire
host threads. Secondary CPU hotplug reuses its existing carrier, while guest
shutdown transfers back to the carrier's native stack before pthread teardown.
This is one common macOS/iOS Simulator/iOS device mechanism, and the carrier
names are `uml-vcpu-N-carrier`. The kernel's 100 Hz plus `NO_HZ_IDLE` policy
reduces host wakeups without changing this ownership boundary.
The early Darwin `ITIMER_REAL` calibration signal is process-directed, so
Kernarium blocks `SIGALRM` on application and launcher workers and only the
vCPU0 carrier temporarily unmasks it. This signal ownership does not add or
move an application launcher thread.
Darwin host signals wake carriers but do not execute Linux IPIs asynchronously.
The arm64 spin-loop relaxation checkpoint services pending IPIs only while
Linux interrupts are enabled, which breaks synchronous cross-call wait cycles
without violating an IRQ-disabled critical section.

## iOS display composition

Kernarium preserves the real Virtio-GPU protocol and renderer rather than
substituting an application-owned framebuffer:

```text
Linux virtio-gpu -> vhost-user device -> virglrenderer -> libepoxy/EGL
    -> UTM ANGLE Metal backend -> MTLTexture -> MTKView
```

The application passes an existing UIKit `MTKView` down through the GPU
service. Virglrenderer exports the ANGLE-backed scanout as a native Metal
texture, and the iOS display owner presents that texture with a small Metal
draw pass. Unsupported resource shapes retain the existing CPU scanout
conversion as a compatibility fallback. The iOS target contains no AppKit,
CGL, or LibVNCServer dependency. Conversely, the macOS graph continues to
compile its existing AppKit/CGL/IOSurface native display and VNC providers; it
does not compile or link the UIKit implementation.

The vhost-user transport owns only connection and queue framing. The GPU and
Virtio-input devices independently implement their protocol operations over
that transport. Kernarium registers one absolute touchscreen and one keyboard,
while UIKit scales safe-area point coordinates into the configured guest
range—point-for-pixel by default or `nativeScale` Retina pixels—and converts
them with text input to Linux evdev events. The GPU device independently
advertises the matching 160 or `160 * nativeScale` effective density through
EDID.
`ContentView` keeps both that framebuffer surface and a SwiftTerm
`TerminalView` mounted, shows the framebuffer by default, and changes only
their visibility and input ownership when the user toggles surfaces. The
existing `ttyS0` relay publishes each ordered output chunk to both application
stdout and SwiftTerm; SwiftTerm delegate input returns through the same serial
socket. This is a view over the existing UML serial endpoint, not a synthetic
Virtio-console device, and hiding the framebuffer does not stop or recreate
the Virtio-GPU presentation path.
Networking remains the emulator's existing callback boundary backed by the
iOS static GLib/libslirp prefix. These iOS owners do not enter the macOS
application graph.

`architecture-audit` combines four checks. Documentation paths in this file
are validated by the same target, so a moved contract cannot leave a stale
architecture reference behind:

- CMake File API direct-link layer rules and dependency-cycle detection;
- CPU/memory/device implementation include rules, including assembly and
  `.inc` files;
- unowned implementation, empty-directory, retired-terminology, and
  implementation-fragment checks; and
- the explicit CTest manifest and advisory source-size review records.

Git-ignored OS and interpreter outputs (`.DS_Store`, `__pycache__`, `.pyc`,
and `.pyo`) are excluded from structural decisions. Their incidental presence
does not change the architecture graph or fail a build.

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
