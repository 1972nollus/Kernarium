# Darwin UML host runtime

## Ownership

```text
api/       installed C API and ABI identity
platform/  Apple capabilities, VM, files, signals, and sockets
memory/    guest address spaces, software MMU, and TLBs
cpu/       interpreter, gadgets, JIT, and neutral engine selection
runtime/   guest-entry composition
device/    block, console, GPU, input, audio, and network providers
emulator/  options and product composition
app/macos/ standalone process adapter
```

Targets and source lists belong to these directories. See
[`../docs/architecture.md`](../docs/architecture.md) for dependency direction.

## Products and defaults

The standalone macOS application supports embedded Linux kernels with 16 KiB
or 4 KiB guest pages. When omitted, `mem=` defaults to 8 GiB and `ncpus=` to
10; explicit kernel arguments override either value independently.

Kernarium uses `6.18`-series iOS runtime frameworks for 16 KiB Linux, 4 KiB
Linux, and 16 KiB Android. Framework names remain stable across patch releases
and kernel commits. It defaults to 16 KiB Linux, 4 GiB on Simulator, 2 GiB on
physical devices, and all processors reported by `NSProcessInfo`. It loads one
runtime for the process lifetime.

The Android macOS composition is a separate product. Its build consumes a
compatible ACK kernel artifact; launch accepts a rootfs directory and
per-instance COW without comparing rootfs release identity, and it does not
search for the Linux desktop image.

## Runtime and scheduling

Each public emulator handle owns its context, engine, stop state, and network
callbacks. One handle admits one run. Native 16 KiB callback runs may coexist
when mappings do not overlap; embedded and 4 KiB runs serialize through the
process address-space gate.

Every configured guest CPU has one persistent `uml-vcpu-N-carrier` pthread.
Linux tasks migrate on guest stacks rather than creating host threads. Host
signals wake carriers, and pending IPIs run only at IRQ-enabled checkpoints.
The early process-directed `SIGALRM` calibration signal remains blocked on
application and launcher threads and is temporarily owned by vCPU0.

Kernarium starts the synchronous guest run on the named `uml-launcher` thread.
The main thread remains available for UIKit and MetalKit.

## Guest memory and kernel loading

All products use the same 128 TiB sparse software-MMU task range. The native
kernel window contains guest RAM plus a page-rounded auxiliary allowance equal
to RAM, bounded from 1 GiB to 4 GiB. macOS places the window at 96 TiB; iOS
places it below its address ceiling.

Accepted mainline artifacts are shared by macOS and iOS within each page
profile. The Android product uses its independent accepted 16 KiB ACK artifact.
Apple CMake validates artifact manifests and never invokes Kbuild.

Kernarium carries each kernel ELF in a signed framework `__TEXT` section. The
loader aliases immutable executable pages from that mapping and copies only
data/BSS into anonymous storage. It rejects carrier mismatch, writable
executable segments, and relocations into signed text. macOS retains the
unsigned embedded-loader path.

Darwin UML guest RAM uses the kernel's linear physical window as its root. The
signed kernel image occupies the reserved prefix and the allocatable suffix is
created anonymously in place; software-MMU and host-service views borrow that
window rather than retaining a second full-span mapping. This applies to both
mainline page profiles and the Android kernel. Physical iOS commits and wires
the anonymous suffix, the loader-to-reserve tail, and any prefix pages as the
kernel releases them into guest RAM; a wire failure aborts startup. Simulator
and macOS retain demand allocation. Disk and image descriptors remain
file-backed.

## CPU engines

Engine selection is explicit. The interpreter is the reference oracle,
gadgets execute non-executable data streams of precompiled entries, and the JIT
publishes native code only through `cpu/arm64/jit/jit_arena.cpp`.

The gadget cache retains a bounded number of tagged address spaces. Retiring an
address space drops only its owned state, and stale same-generation programs
must match every consumed guest word before reuse. Shared gadget translations
contain no executor-owned link pointers after publication; consumers relocate
links into their own state.

The JIT derives private and shared cache geometry from one process budget. A
managed run keeps at most one executor per guest CPU. Shared translations own
immutable code and validation identity, while direct/indirect activation state
remains private and is regenerated during materialization.

## Devices

The generic vhost-user transport supports independent Virtio-GPU,
Virtio-input, Virtio-Sound, Virtio-console, and Kernarium battery services.
Deferred used-ring publication lets a device retain a descriptor until
asynchronous host work is complete.

The common Apple graphics stack uses virglrenderer and ANGLE/EGL on Metal.
macOS provides native AppKit and VNC display consumers; iOS provides UIKit
Metal presentation and deliberately excludes VNC. See
[`../docs/graphics.md`](../docs/graphics.md).

Kernarium attaches:

- an absolute touchscreen and evdev keyboard;
- one signed-16-bit stereo 48 kHz playback stream through Virtio-Sound;
- an Android-only Virtio battery reflecting the Apple host's percentage and
  charging state, with a 100-percent charging fallback;
- SLiRP networking from the verified iOS prefix;
- a Virtio-GPU Metal framebuffer; and
- the existing `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI. Retina
mode multiplies geometry and density by `nativeScale`. The GPU, touchscreen,
and Android logical-density command line use the same launch geometry.

The audio device owns a bounded period/completion ring. The callback path does
not allocate or lock. iOS owns AVAudioSession interruption and route recovery.
Capture and mixer controls are not exposed.

SLiRP host forwards are owned by the provider so they survive provider reopen.
An unavailable individual host port is reported without removing the guest's
network interface. Filesystem socket paths are removed only by their owning
guards or the common termination cleanup.

## Guest storage and lifecycle

The platform layer tracks every writable host descriptor opened for the guest.
`uml_emulator_sync()` asks the guest to write back through its management
console, then flushes those host descriptors. `uml_emulator_quiesce()` enters
full Linux system suspend and waits until devices, secondary CPUs, interrupts,
and timekeeping have reached the platform sleep boundary before flushing the
host. `uml_emulator_resume()` sends the console wake edge. A kernel without the
suspend command falls back to the legacy stop/go pair.

Every run receives a unique UML identity and management-console path. Guests
that need writeback requests boot with magic SysRq enabled. If the physical iOS
sandbox cannot bind the console, host-file flushing still runs but guest
writeback/freeze is unavailable.

Kernarium flushes when losing focus, quiesces before backgrounding or
termination, and resumes on foregrounding. A `.kern` bundle owns its persistent
COW; launch and app updates must not recreate or flatten an existing bundle.

## API and validation

The installed API is under `api/include/uml/`; its ABI identity is independent
of the host/kernel bridge under `../kernel/common/abi`. Follow the ABI README
for declaration changes.

Use the workflows in [`../docs/testing.md`](../docs/testing.md). The
architecture audit verifies source ownership, dependency direction,
executable-memory ownership, test registration, and platform isolation. Guest,
package, graphics, and performance behavior require their separate workflows.
