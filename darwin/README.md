# Darwin UML host runtime

The host implementation is organized by ownership:

```text
api/       public API contract
platform/  Apple capability and platform primitives
memory/    guest address-space, MMU, and TLB contracts
cpu/       interpreter, gadgets, JIT, and the neutral engine factory
runtime/   guest-entry adapter and runtime composition
device/    network, VNC input, GPU, and provider implementations
emulator/  options and product composition
app/macos/ CLI and embedded-kernel process adapter
```

On macOS the Virtio-GPU display provider supports `vnc` and `native` modes.
The native provider keeps Virgl scanout pixels on the GPU: it blits the CGL
texture into one of two IOSurfaces and presents the same surface through
MetalKit. The two slots protect OpenGL/Metal producer-consumer ownership; they
are not a queued swapchain. Only the newest ready slot is retained and at most
one Metal command buffer is in flight. The CAMetalLayer is limited to two
drawables. Unsupported resource shapes fall back
to the shared CPU scanout conversion used by VNC. AppKit remains on the process
main thread while the synchronous emulator run moves to a worker whose signal
mask preserves UML's SIGALRM timer contract.

The opt-in Kernarium iOS device and Simulator runtime uses the same Virtio-GPU
device and virglrenderer command path. Its platform display is UIKit/MetalKit: the pinned
UTM libepoxy build loads EGL and GLES from ANGLE frameworks, ANGLE renders
through Metal, and the native resource texture is sampled directly into the
application-provided `MTKView`. VNC is deliberately not built for iOS.
Unsupported resource layouts use the shared CPU scanout fallback. Platform
source selection is exclusive, so the existing macOS AppKit, CGL, IOSurface,
and LibVNCServer implementation is unchanged.

Kernarium also composes two in-process Virtio-input services over the generic
vhost-user transport: an absolute touchscreen whose coordinate maxima match
the safe-area Metal view and an evdev keyboard fed by UIKit text input. Its
network callback is the ordinary SLiRP provider compiled against the private
iOS GLib/libslirp prefix. The application disables the UIKit idle timer while
the managed guest run is active and restores it when that run finishes.
Simulator runs retain four guest CPUs. Physical-device runs derive `ncpus=`
from `NSProcessInfo.processorCount`, matching the device's physical CPU-core
count.

Both iOS products carry the same complete ET_DYN kernel ELF in a 16 KiB-aligned
`__TEXT,__uml_elf` section of `KernariumRuntime.framework`. The standard Xcode
framework signature covers that storage. The embedded ELF loader verifies the
carrier byte-for-byte, derives the native window from guest RAM plus a 4 GiB
auxiliary allowance below the 62 GiB ceiling, biases the image as one unit, and
aliases immutable executable pages from the already signed Mach-O mapping with
`vm_remap`; it never copies code into anonymous memory or publishes RW pages as
executable. ELF data and BSS retain the ordinary copy path, and relocations into
signed executable pages are rejected. Simulator and device therefore use the
same signed-kernel and address-layout behavior. The macOS
AppKit/CGL/IOSurface/Metal, VNC, loader, and 96 TiB address-space paths are
unchanged.

Darwin schedules stackful Linux task contexts on exactly one persistent host
carrier per configured guest CPU, including vCPU0. Task creation initializes a
saved PC and aligned stack pointer but never creates a pthread; normal Linux
scheduling and migration can therefore use every vCPU without approaching
iOS's process thread ceiling. CPU hotplug reuses the existing carrier, and
shutdown first returns to its native pthread stack. The common mechanism is
used on macOS, Simulator, and device, with carriers named
`uml-vcpu-N-carrier` for profiles. Shipped kernels use a 100 Hz tick and
`NO_HZ_IDLE`, suppressing periodic carrier wakeups while a guest CPU is idle.
Darwin host signals remain wake edges rather than asynchronous Linux interrupt
execution. An IRQ-enabled `cpu_relax()` checkpoint polls pending IPIs so two
carriers waiting on synchronous cross-CPU calls cannot strand each other; an
IRQ-disabled section continues to defer the work until interrupts are enabled.

Each fixed-vCPU carrier owns its engine adapters for the life of its host
thread. JIT adapters are lightweight: a JIT-capable emulator run retains
at most one persistent executor per configured guest CPU in a process-global,
session-owned pool, and each adapter borrows one exclusively for its guest
timeslice. This lets cache and code state follow scheduled guest work across
more host task threads without multiplying retained memory beyond the
whole-process JIT budget. The managed pool belongs to one active runtime
memory domain: executors may switch guest generations and backings within that
domain, but teardown drains them before the domain retires and no executor
crosses into a later run or domain. Cacheline-separated availability cells
publish the exact owning session token, so a stale claimant cannot consume a
restarted session's executor. Standalone executor admission is closed from
managed-session reservation through teardown; only the pool's tagged lease
path may construct while that session is active. `CpuEngine::run()` is a
synchronous,
non-throwing-by-contract guest timeslice boundary: implementations return
`ExecutionResult` for guest exits; allocation or host failures are reported as
`fatal`. The virtual call occurs once at the guest-entry boundary, never in an
interpreter step or translated block loop. Gadgets and JIT retain independent
caches and do not fall back to one another.

The gadget executor keeps decoded programs across a same-generation guest
code-change notification only when exact validation is enabled. It first resets
all executor-local direct and indirect link activations, then compares every
guest instruction word consumed by a stale program at the normal page-bounded
fetch boundary. An exact match reactivates that retained program; a mismatch or
fetch fault discards it and takes the ordinary decode path. A changed guest
generation still invalidates the whole cache. Saved-word buffers are allocated
only for populated slots and grow on demand rather than reserving a full block
in every cache slot.

The JIT process budget defaults to half of guest memory. Each pooled executor
has a complete private working-set ceiling of 64 MiB; after those working sets
are funded, only the process-global shared translation cache grows with guest
memory. Its shard backing is created on first insertion. Shared entries retain
canonical code with zeroed executor-pointer cells, neutral relocation keys,
and immutable validation words. Materialization copies code into the consumer's
private executable arena and regenerates only its executor-local link state.
The first block that names a direct target places one compact cacheline anchor
after its code; later sources reuse that target-PC anchor through the separately
budgeted fixed reverse index. Code and anchors retire together. Mutable
activation state and executor addresses never enter the shared cache.

The opt-in macOS JIT-profiling product uses a private emulator facade, not the
installed emulator ABI. That facade routes through a runtime-owned bridge to
the CPU reporters; the app does not include a CPU-private diagnostics header.
Once the embedded ELF entry has returned and retired its task carriers, the
emulator drains both retired and still-live thread-local profile tails before
releasing its executor pool. The CLI repeats that boundary after
`uml_emulator_run()` returns and before its intentional `std::_Exit()`. Each
changed cumulative report is written directly to stderr's descriptor. Its
bounded buffer starts with an LF in the same write, so an unrelated partial PTY
line cannot prefix the record schema; unchanged repeats are silent and require
no stdio flush. Compile/overlap and shared retain normal-exit callbacks for
non-CLI embeddings. Executor-pool reporting is intentionally explicit-only
because its snapshot requires the quiescent pre-drain boundary.
The dedicated `UML_JIT_PROFILE_EXECUTOR_POOL=1` mechanism trace is owned by the
CPU profiling graph. JIT wrappers record borrow, slot, waiter and interrupt-
sequence observations locally. Every successful borrow associates its captured
session and nonnull sequence with the claimed slot in bounded storage; only
wrapper lifetime and the quiescent final touch the registry mutex. The final
merges by session/sequence, ORs slot masks, and emits only aggregate fanout, not
raw keys. An unset control passes no profile into the pool and skips the
post-claim population scan. Trace-build branches and enabled post-claim scans
deliberately make those timings unfit for score comparisons. Ordinary products
compile out the source, environment control, wrapper storage and every call
site.

The CLI requires a replayed Linux source and embeds its Kbuild `vmlinux`.
`UML_GUEST_16K_PAGES=ON` is the default; set it to `OFF` for the independently
tested 4 KiB compatibility build.

Guest integration uses the unified raw EROFS image `rootfs.erofs` at
`/dev/ubda`, with `rootfstype=erofs`. Shared guest campaigns map it with
`ubd0c=` to avoid a conflicting host-file lock. EROFS supplies the read-only
lower filesystem; workloads that need writable state use a tmpfs OverlayFS.
The image owns tty0 getty enablement and login policy; host-fed OverlayFS
launchers preserve it rather than installing a transient getty override, and
mount separate tmpfs filesystems at `/var/lib/docker` and
`/var/lib/containerd` before systemd to keep Docker and containerd state off the
root overlay. The systemd integration smoke imports a local rootfs
archive containing a source-built freestanding arm64/Linux hello-world payload
into that disposable state and runs it with `--pull=never --network none`, with
no registry dependency. No KDE/VNC or WebGL launcher creates a desktop account:
both reuse the image's non-root `uml` user and never synthesize `plasma`. No
Unix-mode or read-only block-argument policy is needed. See
[`../docs/testing.md`](../docs/testing.md) for preparation and run commands.

Kernarium recreates one sparse 8 GiB image in the app temporary directory on
each launch, formats it as ext4 in the guest, and uses directories on that one
filesystem for the OverlayFS upper/work state plus Docker and containerd.
Bind mounts keep those three contents separate while sharing one free-space
pool. The image is writable guest state, not host-memory relief: Kernarium does
not create or activate a disk-backed swap file. The init shell mounts devtmpfs,
pivots into the writable root, installs
only the disposable KDE setup unit, and execs the image's systemd for
`graphical.target`. Kernel console bytes are relayed to application stdout.
The EROFS base remains read-only and the image's `uml` account and normal
display-manager units own the desktop session.
