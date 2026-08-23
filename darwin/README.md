# Darwin UML host runtime

The host implementation is organized by ownership:

```text
api/       public API contract
platform/  Apple capability and platform primitives
memory/    guest address-space, MMU, and TLB contracts
cpu/       interpreter, gadgets, JIT, and the neutral engine factory
runtime/   guest-entry adapter and runtime composition
device/    network, input, GPU, audio, and provider implementations
emulator/  options and product composition
app/macos/ CLI and embedded-kernel process adapter
```

On macOS the Virtio-GPU display provider supports `vnc` and `native` modes.
The native provider samples the `MTLTexture` exported by the common
virglrenderer/ANGLE Metal path directly in MetalKit. AppKit and UIKit share one
device-owned newest-frame-wins presentation timeline with at most one Metal
command buffer in flight; only their window, input, sizing, and lifecycle
shells differ. Presentation reports accepted, dropped, or unsupported. A
superseded frame is retired as guest success without coupling rendering to the
host display timeline. Unsupported resources may take the counted CPU
diagnostic/VNC readback path, but zero sustained fallback is required for
native qualification. `UML_GPU_FRAME_STATS` reports both timelines, drops,
and fallback; see `docs/testing.md`. The native AppKit shell releases every key
it still believes is held whenever the window stops being key or the view
resigns first responder, because AppKit delivers the matching `keyUp` and
`flagsChanged` to whichever application took focus. It also reads the
device-dependent modifier bits rather than `NSEventModifierFlagShift` and its
siblings, which stay set while either side of a pair is held and would
otherwise report a release as a press. AppKit remains on the process
main thread while the synchronous emulator run moves to a worker whose signal
mask preserves UML's SIGALRM timer contract.

macOS, iOS Simulator, and iOS device now use one product-fixed host GL facade:
UTM libepoxy resolves EGL/GLES only from the authenticated flat ANGLE
frameworks, `HostGlProvider` creates an ES 3 hardware display with
`EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`, and virglrenderer exports the native
Metal texture. The provider is an opaque owner inside a fixed-layout
`VirglRenderer`; no backend macro, CGL context, OpenGL texture resource,
OpenGL.framework link, desktop libepoxy/virglrenderer family, or runtime GL
selector remains. Provider construction, context topology, reset, and teardown
stay on the GPU transport thread. VNC availability is independently fixed by
the macOS product and cannot select another renderer; iOS deliberately omits
VNC.

Kernarium also composes two in-process Virtio-input services over the generic
vhost-user transport: an absolute touchscreen whose coordinate maxima match
the safe-area Metal view in its selected pixel mode and an evdev keyboard fed
by UIKit text input plus the app's Power, Volume Up, Volume Down, Escape, Tab,
and Home tray. Every tray action emits an ordinary press/release pair. By
default the view maps safe-area points directly to
guest pixels and supplies 160 DPI through Virtio-GPU EDID. Optional Retina mode
multiplies the bounds by `nativeScale` and supplies `160 * nativeScale`; touch
points are scaled into the same selected range. Point density selects VGA8x16
fbcon text, while Retina density selects TER16x32. Its audio service exposes
one standard Virtio Sound playback stream backed by AVAudioEngine. The guest
sees signed 16-bit interleaved stereo at 48 kHz; capture, jacks, mixer controls,
and the legacy UML hostaudio relay remain absent. PCM periods stay owned by the
device until AVAudio reports them consumed. iOS uses the playback audio-session
category and restarts the engine after route/configuration changes or
interruptions. The macOS CLI attaches the same service only when passed the
host-only `--audio` option. The standard KDE launcher passes that option by
default, while Kernarium attaches audio for every normal launch.

Desktop guest images own ALSA discovery and application routing and must ship a
supported PipeWire session manager such as WirePlumber. The macOS KDE overlay
and Kernarium bootstrap do not synthesize a sink or routing policy when that
package is absent.

The SLiRP provider accepts host forwards. Each `--net-forward` specification is
recorded by the provider rather than by the SLiRP instance, so a stop and reopen
re-establishes the same set, and an individual forward that cannot claim its
host port is reported without failing the guest's only interface. Forwards
default to binding loopback and targeting the single DHCP lease; see
`docs/testing.md` for the grammar.

Kernarium's network callback is the ordinary SLiRP provider compiled against
the private iOS GLib/libslirp prefix. The application disables the UIKit idle
timer while the managed guest run is active and restores it when that run
finishes. It records the public `ProcessInfo.thermalState` at managed-run
start, on every iOS state-change notification, and at clean finish. Those
stable markers let signed-app qualification distinguish nominal, fair,
serious, critical, and unknown states without private APIs. Before each
physical-device matrix shard, the adapter uses a probe-only app launch that
does not initialize the guest and waits in one-minute intervals until that
public state is nominal. Thermal changes after the shard begins remain
observational: they do not reject a passing CTS shard unless they produce an
ordinary test, performance-budget, app-termination, or lifecycle failure.
Both iOS products default to
all processors reported by `NSProcessInfo`; Simulator defaults to 4 GiB of
guest RAM and physical devices
default to 2 GiB. The settings sheet can persist one through that processor
count, 512 MiB through 4 GiB of RAM in 64 MiB steps, a 1--32 GiB overlay image, the
selected `.erofs` filename from Documents, and either a 4 KiB or 16 KiB kernel
page profile, plus Retina mode; a saved configuration is snapshotted on the
next app launch. 16 KiB is the default kernel profile and Retina is off by
default. The standalone macOS app retains its larger
default of 8 GiB and ten vCPUs whenever the caller supplies neither `mem=` nor
`ncpus=`; explicit values continue to override those defaults independently.
The separate `uml-darwin-android17` composition defaults to 4 GiB and ten
vCPUs, accepts only the installed 16 KiB Android 17/ACK 6.18 release selected at
configure time, and requires a per-instance writable COW path. Its optional
host-only `--virtio-console-socket=/absolute/path` argument attaches one
standard single-port Virtio console through a reconnectable, mode-0600 Unix
stream socket. The console transport never waits for a client: it retains a
bounded recent output tail before attachment and drops backpressured output
with a diagnostic rather than blocking the guest. Its launcher does not search
for `rootfs.erofs` or alter the current `uml-darwin` product.
The ordinary macOS product additionally accepts the mutually exclusive
`--virtio-serial-socket=/absolute/path`. That mode negotiates the standard
multiport feature and publishes `org.uml.host-serial.0` at
`/dev/virtio-ports/org.uml.host-serial.0`; it does not turn the port into an
HVC console or interfere with an image-owned `/dev/hvc0` getty. Android keeps
the existing single-port attachment unchanged.

Every bound filesystem socket - the Virtio console, the Virtio serial line, and
each vhost-user device - is owned by `darwin/device/sockets`, which removes the
path when its guard unwinds and refuses to remove one another process has since
rebound at the same name. Ordinary teardown unwinds that guard, but a
terminating signal does not, so the same removal also runs from the
SIGHUP/SIGINT/SIGQUIT/SIGTERM handler the application installs and from
`darwin_uml_exit()`, the single point every guest-driven termination reaches.
The guest kernel installs its own SIGINT and SIGTERM handlers after the
application's, so those two arrive through the exit bridge while SIGHUP and
SIGQUIT stay with the application handler; both routes end in the same removal.
A path left by SIGKILL or a host power loss is not recoverable this way, and
because both socket servers refuse to replace an existing path, such a leftover
must be removed before reusing that name.

The opt-in Apple CTS adapters pass only an identifier from the checked
campaign allow-list. CMake generates that campaign's exact bounded guest
launcher from the common VK-GL-CTS driver into each page-profile runtime; the
app rejects an unknown identifier and makes CTS, Aquarium, and Geekbench modes
mutually exclusive. Adapter launches may select exactly 4096 or 16384 through
`KERNARIUM_KERNEL_PAGE_SIZE`, while ordinary launches continue to use the
saved setting. CTS runs use the same serial relay for checksummed artifact
egress and request guest poweroff; the connected-device wrapper refuses to run
before a matching passing Simulator artifact. This diagnostic composition
does not modify the EROFS image or advertise a new guest API. Both adapters
require the successful Darwin stop, guest-exit, and runtime-return markers and
reject the dirty DRM memory-manager teardown signature.

The first matched 16 KiB campaign passed the exact allow-listed GLES 3 case
1/1 on the iOS 26.5 iPhone 17 Pro Max Simulator and then on the connected iPad
mini 6. Both used gadgets, reported the hardware ANGLE/Metal provider, real
DRM/Xwayland/virgl, desktop GL 2.1, GLES 3.0, and guest/runtime status zero.
The first iPad poweroff warned that the DRM memory manager was not clean during
takedown. Maintained kernel topic 0020 now shuts Virtio-GPU atomic state down
before device unplug; three consecutive rebuilt Simulator runs and three gated
iPad runs subsequently passed 1/1 and powered off cleanly. This bounded repair
evidence is not the full sustained-lifecycle or conformance qualification.

Every Apple product derives the native window from guest RAM plus a
page-rounded one-eighth auxiliary allowance (with a 32 MiB floor) and biases
its ET_DYN image as one unit. Both iOS products top-pack that window below the
62 GiB ceiling. Kernarium embeds one versioned runtime framework for each
canonical 4 KiB and 16 KiB kernel. The app links neither at process launch;
its always-linked loader blocks the early UML timer signal before main, loads
exactly the saved profile, and validates the framework's release, page size,
and immutable C function table. Each complete kernel ELF occupies a
16 KiB-aligned `__TEXT,__uml_elf` section whose standard Xcode framework
signature covers that storage. The Darwin linker keeps executable boundaries
on 16 KiB host pages for both guest profiles. The embedded loader verifies the
selected carrier byte-for-byte and
aliases immutable executable pages from the already signed Mach-O mapping with
`vm_remap`; it never copies code into anonymous memory or publishes RW pages as
executable. ELF data and BSS retain the ordinary copy path, and relocations into
signed executable pages are rejected. macOS retains its established unsigned
loader and its AppKit native/VNC presentation choices, while using the common
ANGLE/Metal renderer and the same
128 TiB sparse guest range, scalable-window derivation, relocation, and
exclusion-hole handling as device and Simulator. It places the matching
page-profile kernel bytes at 96 TiB to avoid collisions in Darwin's crowded
low host range. Its
8 GiB default derives a 1 GiB auxiliary allowance and a 9 GiB active native
window.

At the 4 GiB Simulator default, the one-eighth auxiliary allowance is 512 MiB
and the active native window is 57.5--62 GiB. At the 2 GiB physical-device
default, the allowance is 256 MiB and the active window is 59.75--62 GiB.

Darwin schedules stackful Linux task contexts on exactly one persistent host
carrier per configured guest CPU, including vCPU0. Task creation initializes a
saved PC and aligned stack pointer but never creates a pthread; normal Linux
scheduling and migration can therefore use every vCPU without approaching
iOS's process thread ceiling. CPU hotplug reuses the existing carrier, and
shutdown first returns to its native pthread stack. The common mechanism is
used on macOS, Simulator, and device, with carriers named
`uml-vcpu-N-carrier` for profiles. Shipped kernels use a 100 Hz tick and
`NO_HZ_IDLE`, suppressing periodic carrier wakeups while a guest CPU is idle.
Darwin's early `ITIMER_REAL` calibration signal is process-directed. The
always-linked Kernarium loader blocks `SIGALRM` before application main and
the selected runtime blocks it again around the managed run, while the
existing launcher remains on its Swift worker; only
the vCPU0 carrier explicitly unmasks the signal until cooperative timer
deadlines take over.
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

Its cache holds several address spaces at once. Slots, direct links, and
indirect sites all carry the address space they describe, so a switch does not
empty the cache and a return to a recently-run address space finds its
translations intact. `UML_GADGET_CACHE_SLOTS` sizes the shared pool and
`UML_GADGET_RESIDENT_GENERATIONS` bounds how many address spaces may hold
translations; the least recently current one is retired whole when a new one
arrives.

A block ends at its first control-flow instruction, which on a browser guest is
a mean of 4.93 instructions. `UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH`, on by
default, translates a conditional branch's fall-through inline behind it
instead: the taken edge keeps its link, the not-taken path falls into the next
gadget, and the word that held the fall-through link holds the number of
instructions translated behind the branch. Entering a block precharges its whole
length, so a taken branch leaving from the middle subtracts that suffix before
following its link. Guest addresses stay contiguous either way, which is what
keeps fault accounting and stale-program validation unchanged.

Each vCPU carries its own executor and its own cache, so several vCPUs
translate the same guest code once each. A process-wide store, bounded by
`UML_GADGET_SHARED_BUDGET_MIB` and off by default, lets one executor adopt
another's translation instead. It is retained as an experimental arm rather than
a supported default: it removes 90% of translation work and still costs frame
rate, because an adoption is dearer than the gadget build it replaces. What makes a gadget program
transferable is that its stream is identical between executors apart from the
cells naming `BlockLink` and `IndirectLink` records, which belong to one
executor and one address space: the builder records every such cell by the
guest PC it names, a canonical entry holds zero in them, and the consumer
resolves them against its own records while copying the stream into its own
storage. Executor addresses and mutable link state therefore never enter the
store. Identity is the guest address, every guest word the build could have
consumed, and the code-generation profile, compared exactly rather than by
hash, so a collision costs a publication rather than the wrong translation.
Publication is gated on two or more live executors, and entries are reclaimed
by second-chance FIFO within the budget.

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

`ExecutableArena::available()` reports the JIT engine only where the arena can
actually publish into a `MAP_JIT` mapping, which needs both the mapping and a
W^X toggle, not the mapping alone. macOS calls `pthread_jit_write_protect_np`
directly and is the only product with a publication route. A physical iOS
device cannot obtain the entitlement, so the toggle is a no-op and the arena
reports the engine unavailable rather than promising one whose first store
would fault.

The Simulator is held to the same answer on purpose. It runs on macOS and could
reach the toggle dynamically, but a Simulator that translates through a path
production devices cannot use would not represent the product it stands in for.
Both iOS products therefore run the data-only gadget executor, and
`UML_DEFAULT_EXECUTION_ENGINE_JIT` stays macOS-only.
`ExecutionEngine::automatic` resolves to the platform default when that engine
is available and falls back to the gadget executor when it is not, so the
automatic selection cannot be the one selection that fails outright.

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

The CLI and Kernarium consume validated, already-built mainline kernel bundles;
their CMake graphs do not prepare or compile Linux. The default 16 KiB CLI and
both 16 KiB Kernarium slices derive their ELF from the same accepted 16 KiB
artifact, while the 4 KiB macOS compatibility product and both 4 KiB
Kernarium slices derive it from the accepted 4 KiB artifact. Only host delivery
changes from the macOS CLI section to the iOS framework's signed `__TEXT`
carrier.
`UML_GUEST_16K_PAGES=ON` remains the default.

Guest integration uses the unified raw EROFS image `rootfs.erofs` at
`/dev/ubda`, with `rootfstype=erofs`. Shared guest campaigns map it with
`ubd0c=` to avoid a conflicting host-file lock. EROFS supplies the read-only
lower filesystem; workloads that need writable state use a tmpfs OverlayFS.
An explicitly selected derivative whose filename ends in `.ext4` is instead
accepted only after its host write bits have been removed. The macOS harness
and Kernarium map that lower with `ubd0cr=`, pass `rootfstype=ext4 ro`, and
retain the same disposable OverlayFS upper layer; Kernarium seals an imported
Documents copy to mode `0444` before entering the runtime. The original EROFS
image remains the default and is not rewritten by this opt-in path.
The image owns its VT and serial getty enablement, account credentials, and
login policy; host-fed OverlayFS launchers preserve them rather than installing
a transient getty or credential override, and mount separate tmpfs filesystems
at `/var/lib/docker` and `/var/lib/containerd` before systemd to keep Docker and
containerd state off the root overlay. The systemd integration smoke imports a local rootfs
archive containing a source-built freestanding arm64/Linux hello-world payload
into that disposable state and runs it with `--pull=never --network none`, with
no registry dependency. No KDE/VNC or WebGL launcher creates a desktop account:
both reuse the image's non-root `uml` user and never synthesize `plasma`. No
Unix-mode or read-only block-argument policy is needed. See
[`../docs/testing.md`](../docs/testing.md) for preparation and run commands.

Kernarium recreates one sparse image in the app temporary directory on each
launch (8 GiB by default, configurable from 1--32 GiB), formats it as ext4 in
the guest, and uses directories on that one filesystem for the OverlayFS
upper/work state plus Docker and containerd. Bind mounts keep those three
contents separate while sharing one free-space pool. The kernel has zswap
enabled by default with zstd, but that cache requires a backing swap device or
file. The image is writable guest state, not host-memory relief: Kernarium does
not create or activate backing swap, so zswap allocates no pool during the
normal desktop flow. The init shell mounts devtmpfs, pivots into the writable
root, applies the image-appropriate disposable desktop setup, and execs the
image's systemd or OpenRC init. KDE retains the image's `uml` account and
display manager. A Phosh image instead keeps its stock Phrog/greetd VT
session, image-owned VT and serial getties, and login credentials. Its
disposable ext4 overlay whiteouts only the image's mutually exclusive
development `graphical.target.wants/phosh.service` enablement, leaving
graphical target's normal display-manager alias to select stock greetd/Phrog.
Empty overlay copies of both `/etc/phrog/phoc.ini` and
`/etc/phosh/phoc.ini` make the greeter and authenticated user session retain
the Virtio-GPU preferred EDID mode instead of switching to the image's fixed
720x1440 mode. The immutable units remain untouched, the former wait helper and
greetd ordering drop-in remain absent, and Kernarium appends
`plymouth.enable=0` to systemd boots because a matched iOS Simulator A/B showed
this image blocking in `plymouth-start.service` before greetd. This launcher
policy skips only the boot splash; the disabled arm completed authentication
and retained the full EDID mode in the logged-in Phosh session. It does not
replace an image unit or affect the macOS launchers and direct Geekbench shell.
The `pmos-sxmo.erofs` image keeps its UID-10000 user, tinydm default session,
and Sxmo/Sway entry point. In its disposable overlay, Kernarium lets the
image's udev services run under UML, supplies the minimal postmarketOS
deviceinfo with swap disabled, classifies the Virtio touchscreen and keyboard,
and grants the `video` and `input` groups access after udev settles before
executing `/sbin/init`. No image account, OpenRC runlevel, or desktop session
is replaced.
Standard VT/fbcon renders early boot through Virtio-GPU. The legacy UML stdio
console and DRM log client are disabled. Kernarium does not attach the optional
macOS Virtio-console service, so `ttyS0` continues to carry its kernel console
bytes to application stdout. Kernarium also publishes the same ordered chunks
to its SwiftTerm terminal and writes SwiftTerm delegate input back to the
existing serial socket. The framebuffer remains the default app surface, and
switching views leaves both SwiftTerm scrollback and Virtio-GPU presentation
alive. The separate Android macOS product attaches the single-port Virtio
console over its explicit host Unix socket for the Android shell and `logcat`.
Its release command line keeps Linux printk exclusively on `ttyS0`; hvc0 is not
a kernel console.
`CONFIG_PRINTK_TIME` prefixes kernel records on both tty0 and ttyS0 with the
guest monotonic timestamp. Normal UML execution supplies that prefix from a
boot-relative host-monotonic nanosecond `sched_clock()` instead of 100 Hz
jiffies; time-travel modes retain their simulated clock, and the carrier tick
policy is unchanged. Both page profiles build VGA8x16 and TER16x32 so
Kernarium can select readable fbcon text for point and Retina densities.
The selected EROFS base remains read-only.
