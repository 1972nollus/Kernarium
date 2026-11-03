# Kernarium

Kernarium is the iOS application that runs one Linux or Android guest on the
Darwin runtime. It targets the iOS device, whose slice also runs on an Apple
silicon Mac in Designed for iPad mode, and the iOS Simulator, where the guest
renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)). Build,
launch, and test commands are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium).

## Ownership

```text
Kernarium/            SwiftUI application: settings, guest selection, console,
                      microphone setting, background running and picture in
                      picture
Runtime/              runtime framework target, one per kernel flavour
KernariumTests/       hosted application and runtime tests
KernariumUITests/     UI tests
KernariumAutomation/  long-lived UI-automation host for a connected device
AudioInterrupter/     test-support application that takes the audio session
scripts/              launch, packaging, bundle assembly, and automation tools
```

The runtime loader, the guest display and its input, the zstd utility, the
microphone permission and the shader-cache bound are not in this directory:
[`GuestHost/`](../GuestHost/README.md) holds them, compiled into this
application and into Andarium. `ZstdUtil` below is that utility.

## Runtimes and runs

Kernarium uses `6.18`-series runtime frameworks for Linux and Android, both
with 4 KiB guest pages. Their names stay stable across patch releases and kernel
commits, and each carries its kernel ELF in signed Mach-O `__TEXT` (see the
[architecture](../docs/architecture.md#memory-and-executable-code)). The
application embeds every framework, links none at launch, and loads exactly
one for the process lifetime.

The library runs one guest per process, so a launch makes one run. A guest that
restarts itself (a reboot, `adb reboot`, a recovery or bootloader round trip) is
booted again inside that run, in the same process, and the run returns only when
the guest is over: it powered off, the user stopped it, a boot failed, or it
kept restarting. The application then says how it ended and asks to be
relaunched; it never starts a second run in the same process. The guest runs on
the named `uml-launcher` thread, which leaves the main thread to UIKit and
MetalKit. [Restarts](#restarts) describes what a restart keeps and what the
application shows.

Kernarium defaults to Linux, the
[vCPU limit](../docs/architecture.md#memory-and-executable-code) (the processor
count `NSProcessInfo` reports, at most 8, which is also the most the settings
and `KERNARIUM_VCPU_COUNT` offer), and 4 GiB of guest memory where the process
is given the host's memory (Designed for iPad on a Mac) or 1.5 GiB on a
physical device, whose jetsam limit a 4 GiB guest would cross and a 2 GiB guest
crossed on a 4 GiB iPad mini 6. The device value stays until the graphics-memory
reductions land, and a value the user stored is kept.

## Restarts

The runtime makes the machine once: the emulator handle, the user network with
its forwards, the GPU service with the view it draws on, the microphone switch
and the footprint log. Each boot makes its own Virtio services, GPU connection,
console bridge, input registrations and command line. When the guest asks to
restart, the runtime stops the boot's services, resets the network link so the
old guest's connections end, and boots again; an Android guest boots in `auto`
mode on every restart. The loop and its guarantees (the retry of a run the
library refuses, the stop request checked before every boot, the loop guard) are
in [`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle); the
host test `kernarium-restart-loop` runs the loop against a recording fake.

The application does not announce a restart the guest asked for. While the next
boot is under way it shows a transient Restarting badge, which goes when the new
guest's console answers. The runtime reports both edges through
`set_restart_callback` (runtime API 14), and `BackgroundController` applies the
page-cache trimming and the vCPU throttle again to a guest it keeps running,
since both belong to a kernel and the new one boots with neither. A guest that
restarts five times in a row within 15 s of booting is not booted again: the
run returns `KERNARIUM_RUNTIME_RESULT_RESTART_LOOP` and the notice says so
instead of printing a status. The restart notice for iOS ending the process in
the background is a different event and is unchanged.

The next boot waits while the guest's GPU requests are held or the guest is
paused. A guest that boots behind a closed gate finds its GPU unanswered: the
`virtio_gpu` probe times out on each capset and drops them all for that boot, so
it would run without 3D after the user came back. A `keepRunning` job holds the
GPU for its whole stay, so a guest that restarts during a job boots when the
user returns; a Picture in Picture window releases the gate and the guest boots
at once. The old guest still stops while the gate is closed: the report of its
restart makes the gate complete its GPU requests without running them, which its
shutdown waits for, and the gate holds the next guest as before.

`KERNARIUM_FOOTPRINT_LOG_MS` also writes `Kernarium: memory` lines (`tick`,
and for a restart `ended` and `booted`): the footprint, the lifetime peak iOS
acts on, the wired bytes, the address space, and the default malloc zone's
bytes in use and held: the figures a restart must return to the first boot's
level.

## GPU memory budget

The host GPU's budget is fixed at launch from the guest's memory, the process's
memory limit and three launch parameters
([architecture](../docs/architecture.md#memory-and-executable-code) owns the
formula; [graphics](../docs/graphics.md#device-memory-budget) what it bounds).
The application states the parameters in the boot request
(`gpu_budget_application_mib`, `gpu_budget_engine_mib`,
`gpu_budget_headroom_mib`) and, before it starts the run, asks the runtime for
the plan (`plan_gpu_budget`, which takes the guest's memory and vCPU count as
the run will, runtime API 18), which evaluates exactly what the run will. When
the limit leaves the GPU less than its 384 MiB floor, the application shows a
Memory May Run Short notice in the style of the ended and restarting notices:
a tap dismisses it, it holds nothing back, and the guest boots with the floor.
Nothing refuses a launch for lack of memory.

`KERNARIUM_GPU_BUDGET_APP_MIB`, `KERNARIUM_GPU_BUDGET_ENGINE_MIB` and
`KERNARIUM_GPU_BUDGET_HEADROOM_MIB` (each 0 to 16384) state the parameters for
one launch, for a measurement run. They come from the environment only and are
never stored. All three default to the launch set of
`runtime/guest_memory_budget.hpp`, whose values are zero until the product
owner chooses them; the loader reports the set, so the application keeps no
copy.

### Safe guest RAM

The settings offer no more guest RAM than the memory limit leaves once the vCPU
count's share of the process is counted
([architecture](../docs/architecture.md#memory-and-executable-code) owns the
formula, `G_max(v)`). Each added vCPU holds more of the process's memory (a
larger gadget cache and its slot tags), so the maximum falls as vCPUs rise.
The runtime framework is loaded once per process for the flavour chosen at
launch, so the settings do not ask it: the loader (`KernariumRuntimeLoader`)
includes the same header, reads the limit from the process (`TASK_VM_INFO`) and
answers `kernarium_runtime_loader_maximum_guest_memory_mib`. Where the system
sets no limit, on a Mac included, the answer is 4096 MiB and nothing changes.

- The Guest RAM stepper runs from 512 MiB to the maximum of the chosen vCPU
  count and updates as the count changes; a footnote states the maximum.
- Raising the vCPU count takes the RAM down to the new maximum, in 64 MiB steps.
  Lowering the count leaves the RAM as it is.
- Loading a stored size takes it down to the maximum of the stored vCPU count.
  With nothing stored the default is 1536 MiB on a device and 4096 MiB on a Mac,
  or the maximum where that is less.
- `KERNARIUM_GUEST_MEMORY_MIB` states a size as is, for automation; the launch
  notice above covers a size the limit cannot afford.

## Guest storage

A Linux guest boots an image file in the application's Documents directory:
the one the settings name, or, while they name none, the accepted KDE desktop
image `kde-20260929-r2.erofs`
([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)). A name the
settings already store, `rootfs.erofs` included, is kept, and every image in
Documents stays selectable. An Android guest is a `.kern`
bundle directory in Documents; the
[Android README](../android/lineage-24/README.md#apple-consumers-and-guest-bundles)
owns its layout. A bundle holds the release's read-only system disk
(`guest/system.img`, `guest/partition-layout.json`) and the guest's persistent
data disk (`guest/data.img`), which the runtime creates at the first launch and
every later launch reopens, so rebuilding the application or a runtime
framework must not erase it and a bundle is never recreated merely to relaunch
it. When copying a bundle to a device, target the complete
`Documents/<name>.kern` destination; never copy into or replace the Documents
directory itself, which can flatten the bundle or destroy other guests.

Launch does not compare kernel, runtime, or rootfs release identities; missing
or malformed inputs fail at their ordinary file or image boundary. The
application chooses the Android data disk's capacity and passes it with the
boot request; the runtime creates the disk or grows it to that capacity, never
shrinks it, and attaches exactly the system disk and the data disk (the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns the layout and the partition tables the host writes). Compressed delivery
is an application concern.
`ZstdUtil` streams a zstd source into an adjacent temporary file, optionally
representing zero regions as sparse holes, and publishes the destination only
after a complete decode.

## Guest boot

A Linux run boots the base archive the runtime embeds
([`tools/rootfs/boot-ramdisk`](../tools/rootfs/boot-ramdisk/README.md))
followed by a segment holding the generic per-run `/init` and its
`/boot.env`. The runtime writes that initrd to its temporary directory as
`kernarium-initrd-<pid>.cpio`, removes it when the run returns, and removes
any an earlier, killed process left. The parameters mount the image read-only
on `ubd0` beneath an overlay whose writable layer is an ext4 filesystem
formatted afresh on `ubd1`: the sparse `kernarium-overlay.ext4` the
application recreates in its temporary directory at each launch, at the size
the overlay setting names. Docker and containerd state is bound from that
filesystem beside the overlay, because neither accepts an overlayfs backing.
A sealed ext4 image is attached read-only as well. `/init` then switches root
into the image's own `/sbin/init`, which boots `graphical.target`.

Nothing is typed on the console and the runtime edits no image: accounts,
device groups, getty and display-manager policy, network and resolver belong
to the image. The KDE, Phrog and postmarketOS images Kernarium boots carry
that setup ([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)); the
KDE image masks the VT gettys, since SDDM runs the session on a VT of its own.
The unified `rootfs.erofs` carries none of it and boots to its own defaults:
SDDM without autologin, beside the image's `getty@tty0`.

## Automation runs

The release runtime accepts no host payload and carries no test campaign. The
`kernarium-ios-device-automation` preset builds the Linux runtime with
`UML_KERNARIUM_AUTOMATION_HARNESS`, and
`scripts/package-ios-frameworks.sh --automation` packages it in place of
the release one. Only it reads:

- `KERNARIUM_BOOT_PAYLOAD`, a plain file name in Documents: a padded newc
  segment of at most 64 MiB, written by `uml-initramfs`, that the runtime
  appends after its own, so its files, `/boot.env` included, replace the
  runtime's;
- `KERNARIUM_SYSTEMD_UNIT`, the `*.target` the guest boots instead of
  `graphical.target`; and
- `KERNARIUM_PAYLOAD_REMOVE=1`, which makes the runtime delete the payload
  file once it holds the bytes and print `Kernarium: removed the boot payload
  Documents/<name>` (or one line naming why it could not, which does not stop
  the boot). A host copies a payload for one launch and sets it; a launch that
  reuses a copied payload leaves it unset. `devicectl` cannot remove a file in
  an application container, so this is how the device runners avoid leaving
  one payload per run behind.

A payload also attaches the artifact relay, a Virtio serial port whose
acknowledged frames the runtime prints on standard output. The runtime prints
`Kernarium: automation boot with a <n>-byte payload and target <unit>`; a
host that does not see that line is talking to the release runtime.
`tests/integration/support/kernarium.py` writes payloads for the conformance
lanes, which boot `uml-cts.target`, the WebGL Aquarium probe, which boots
`uml-aquarium.target`, and the device workload runner, whose script the
harness session runs before powering the guest off. The lanes are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium) and
[`tests/perf/README.md`](../tests/perf/README.md).

The Android runtime has an automation variant of its own, the
`kernarium-ios-device-android-automation` preset packaged with
`scripts/package-ios-frameworks.sh --android-automation`. The release runtime
always starts the guest with `androidboot.kernarium.adb_auth=1`, so a fresh
bundle's first ADB connection waits for the "Allow USB debugging" prompt, and
a driver without a person at the screen cannot answer it. Only the automation
runtime reads `KERNARIUM_ANDROID_ADB_AUTH`: `0` starts one launch with
authorization off, as the macOS launcher does, and `1` or unset keeps the
prompt. It prints `Kernarium: ADB authorization is off for this launch`, and
it refuses the setting unless `KERNARIUM_HOST_FORWARD` is unset or listens on
loopback, because authorization off gives whatever reaches the forward an
unauthenticated shell (and `adb root` on a userdebug image). A host on USB
reaches a loopback forward through `iproxy`; a forward on every interface or on
a network address is refused.
The packaging script refuses a release runtime that carries the switch.

How an Android guest's adbd is reached is told to the guest on its kernel
command line, by the launcher alone: a release's command-line file cannot set
either argument. The boot request can carry the adb host key the guest
authorizes for that boot without asking its user (`android_adb_public_key`,
runtime API 19): the base64 of adb's `RSAPublicKey` record, 1 to 1024
characters of `A-Za-z0-9+/=`, or null. A request that carries anything else
fails with `-UML_EMULATOR_INVALID_ARGUMENT`. The runtime passes it as
`androidboot.kernarium.adb_key=<key>`. A launch with no
`KERNARIUM_HOST_FORWARD` has no use for a TCP listener, so the runtime also
passes `androidboot.kernarium.adb_tcp=0` and adbd listens on vsock alone; a
launch with a forward passes no `adb_tcp` argument, and adbd keeps listening on
TCP for the host that reaches it through the forward. adbd reads both as
`ro.boot` properties itself. The kernel's command line holds 4095 bytes in all,
counting what the emulator adds, and the runtime refuses a launch whose
arguments exceed it with a line that says so, because the kernel would end the
process.

Every automation runtime also reads `KERNARIUM_BACKGROUND_JOB_AT_START=1`
(`0` or unset asks for nothing; anything else is reported and ignored). A guest
in the keep-running mode (`KERNARIUM_BACKGROUND_MODE=keepRunning`) then gets
the background job as soon as it runs, the request the job button makes, and
the application prints `Kernarium: automation starts the background job`
followed by the job's own `submitted` or `refused` line. iOS accepts the
request only from the application in front, so the driver launches it there and
sends it to the background afterwards; a driver that cannot get the job
submitted still needs the UI automation host. The application learns of the
setting from `kernarium_runtime_automation_job_at_start`, an export outside the
versioned table that only an automation runtime has, and the packaging script
refuses a release runtime that contains the setting's name.

`KERNARIUM_BACKGROUND_HOLD_DELAY_MS`, read by the same runtimes and refused in
release the same way, holds the guest's GPU that many milliseconds (a whole
number up to 30000) after the application enters the background instead of at
the edge, on the same off-main timer the production bound uses, so a driver can
measure how long iOS leaves the host GPU usable there. It moves only the hold of a job, which keeps its guest drawing for the
measurement, and an Android guest's screen is still turned off without waiting
for it, so measure on a guest whose screen stays on. A return before the delay
ends cancels the hold. The application learns it from
`kernarium_runtime_automation_hold_delay_ms`, another export outside the table.

The App Store Android runtime is a third variant, the
`kernarium-ios-device-android-app-store` preset (`UML_APP_STORE_RUNTIME`)
packaged with `scripts/package-ios-frameworks.sh --app-store --runtimes android`
for Andarium's App Store configuration. It conflicts with the automation
harness. The option defines `UML_APP_STORE_RUNTIME` for the whole tree, and every
environment variable that tunes, observes or drives a run is read as unset:
`KERNARIUM_HOST_FORWARD`, `KERNARIUM_FOOTPRINT_LOG_MS`,
`KERNARIUM_EXECUTION_ENGINE`, `KERNARIUM_GUEST_HIBERNATION`, and the `UML_` and
`DARWIN_UML_` diagnostics and switches of the CPU, GPU, video, network and
platform layers (`TMPDIR` stays). The packaging script refuses an App Store
runtime whose host code carries the name of one of them
(`Andarium/scripts/check-app-store-bundle.py --runtime`). The release and
automation runtimes read them as they did.

## Devices and display

Kernarium attaches:

- an absolute touchscreen and evdev keyboard, which advertises Linux key codes
  1 through 127 and, above them, Back (158) and Home (172) for Android's
  navigation keys; the guest ignores any other code;
- a signed-16-bit 48 kHz playback stream (stereo) and capture stream
  (mono or stereo) through Virtio-Sound, whose capture records silence unless
  the user turns on Microphone;
- an Android-only Virtio battery reflecting the Apple host's percentage and
  charging state, with a 100-percent charging fallback when the host has no
  readable battery;
- SLiRP networking from the verified iOS prefix;
- a Virtio vsock device, the last Virtio device, through which the application
  opens streams to guest ports (`connect_vsock`, runtime API 19; Andarium opens
  adbd's port 5555). It is
  [the device](../darwin/device/vsock/README.md), one per boot, and the guest
  cannot open a connection to the application;
- a Virtio-GPU Metal framebuffer; and
- the guest's `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI; Retina
mode multiplies both geometry and density by `nativeScale`. The GPU's EDID and
Android's logical density on the kernel command line use that launch geometry.
The touchscreen's absolute range is fixed at 0 through 32767 on both axes, as
the VNC touchscreen's is, and Kernarium maps the guest's visible rectangle in
the view onto it, so touch resolution does not depend on the launch size. The
Metal shader cache policy is in
[`graphics.md`](../docs/graphics.md#metal-shader-cache).

The guest view lies within the content the system leaves clear of the window
controls: below them in a window, a tile and a Slide Over window, and at the
safe area where the scene covers its screen, which is Full Screen Apps and a
window zoomed to the screen, whose controls move to the menu bar. The system's
corner offset keeps describing a window after Multitasking switches the
running application to Full Screen Apps, so the application applies it only
while the scene is smaller than its screen, and follows the scene's geometry
as Multitasking, zoom, tiling, Slide Over and rotation change it.

Kernarium runs one guest per process and so has one window: its scene
manifest does not support multiple scenes, and iPadOS offers no second window.
Tiling and Slide Over, from the Window menu of a window zoomed to the screen,
only arrange that window, and the guest display follows its size as it does a
resize.

### Microphone

Emulator Settings has a Microphone switch, off by default. While it is off, or
iOS has not granted access, a guest that records gets silence in real time
and nothing opens the microphone. Turning it on is the one place iOS is asked
for access: the application asks (`MicrophoneSetting`), as the direct result of
the switch, and then tells the runtime (`set_microphone`), which never asks and
never shows a prompt. The switch applies at once, unlike the rest of the form,
which is why Cancel does not undo it. A launch that finds it on carries it into
the guest without asking, so a user who dismissed the question is not asked
again at launch; iOS's Settings is where to change the answer, and the
application picks the change up when it becomes active.

The microphone is opened only while a program in the guest records, so iOS's
recording indicator is lit only then, and it is closed when the program stops.
The audio session is play-and-record, still mixing with other audio, only for
that time; the Picture in Picture window keeps that category when it finds it
rather than replacing it with the playback category, which its movie mode needs.
A guest kept running in the background may keep recording while the program
does, under the `audio` background mode; iOS's indicator shows it.
[`../darwin/device/audio/README.md`](../darwin/device/audio/README.md#microphone)
owns the device side.

## Lifecycle

Kernarium asks the guest to write back when the application loses focus,
quiesces it before the application is backgrounded or terminated, and resumes
it on foregrounding. A key press also wakes a guest that suspended itself,
such as Android once its display is off, as a phone wakes to its keyboard;
a touch does not. Power wakes it at every press, any other key at most once a
second. The runtime side of sync, quiesce,
resume, and wake is in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle).

iOS revokes the host GPU within seconds of `didEnterBackground`, and a guest
request that reached Metal afterwards would make the guest's Vulkan device
lost for good. The application therefore holds the guest's GPU requests inside
`didEnterBackground`, before it quiesces, and releases them when it returns to
the foreground. The hold and the quiesce share the handler's four-second
budget, and the hold itself waits at most one second for a request already
running. Requests the guest queues meanwhile wait in their rings, through the
suspend, and run in order after the release. The handler stops waiting at that
deadline, but the quiesce carries on under the `guest-sync` background task,
waiting up to ten seconds for the guest's suspend reply, which follows its
file system sync, and then aborting a suspend that has not replied. A return
to the foreground resumes the guest once that reply has arrived or the wait has
ended.

Guest page-cache trimming stays off while the application is in the
foreground, because the guest then pays a host read for every evicted page its
user's programs read again. Quiescing trims once by itself, before the suspend:
the guest takes its cold page cache for up to a second, compacts its memory for
up to another, and hands what is free to the host, and the quiesce waits up to
three seconds for that. On a loaded Mac the pass took 1.2 s for a 4 GiB KDE
guest and 1.8 s for Android, so with an immediate hold and a prompt suspend
reply the quiesce usually ends within the handler's budget. Trimming only there
is the user's choice: paused after the pass, a 4 GiB guest held 141 MiB (KDE)
and 54 MiB (Android) more than one that trimmed all along, measured on macOS.
A guest kept running in the background trims as it runs; see below.

On a device the quiesce ends by hibernating the guest's RAM: the runtime holds
every thread that can touch it, writes it to a file in the application's
temporary directory and takes it off the footprint, so that a paused Kernarium
no longer carries the guest's wired gigabyte; the resume restores and rewires it
before it wakes the guest
([`../docs/architecture.md`](../docs/architecture.md#hibernation)).
`KERNARIUM_GUEST_HIBERNATION=off` keeps the RAM wired while paused, and `on`
forces the hibernation on the Simulator. The `guest-sync` queue serializes the
resume behind a quiesce, so a quick return waits for the write to end.

iOS may still end a suspended application, and the guest goes with it. The
application records the background edge in a marker file and removes it once
it is back in the foreground and the resume of a quiesced guest has returned;
a launch that finds one says the guest restarted because Kernarium was closed
in the background, once, until it is dismissed. A resume that cannot restore the
hibernated RAM ends the process inside the call, and the marker it leaves says
the same.

### Keeping a guest running in the background

A guest pauses in the background unless its owner chose otherwise. Settings
holds one mode per guest, stored under the guest's root filesystem name, and
`off` is the default and the only mode of a guest that never chose: running
costs battery, so it is opt-in. `BackgroundController` decides what the
lifecycle edges do and acts only through `BackgroundEffects`, which the
unit tests replace.

The mode `pictureInPicture` keeps the guest running while iOS shows its
display in a Picture in Picture window, which iOS keeps alive for as long as the
user leaves it open. The window shows the guest's real display: the runtime
mirrors the display it already renders to a sink the application sets
(`kernarium_runtime_loader_set_frame_sink`, see
[`graphics.md`](../docs/graphics.md)), and the application feeds those frames
to an `AVSampleBufferDisplayLayer`. While a guest has the mode, the mirror runs
at a trickle in the foreground so the layer always holds a current picture, and
iOS starts the window by itself as the user leaves the application; it runs at
a glanceable rate while the window is up. A guest without the mode renders no
mirror frames.

The edges, in order, with the host GPU held from the first so no request
reaches it while it is revoked:

- Leaving the application holds the GPU and writes the guest back. A window
  that is up, or that iOS has begun to show, keeps the guest running and the
  GPU is released; one that does not appear within three seconds, fails to
  start, or is closed in the background quiesces the guest as an unchosen mode
  would.
- Returning releases the GPU, resumes a guest that was quiesced, and closes the
  window if it is still up.
- While a guest is kept running its vCPU threads run at the system's background
  priority (`set_background_throttle`): low priority, the efficiency cores, and
  throttled I/O for those threads only. It returns them to full priority before
  anything else is asked of the guest, so a quiesce is not throttled.
- While a guest is kept running it also trims its page cache periodically
  (`set_page_cache_trimming`), and stops when it returns to the foreground or
  is quiesced, which trims once by itself. The switch runs on the `guest-sync`
  queue behind any write-back or quiesce already asked for, since it may wait
  for the guest's console; one made while the guest is quiesced reaches it on
  resume.

The throttle moves the work to the efficiency cores rather than removing
cores: on an A15 iPad, a KDE guest with six vCPUs running a six-thread load
for 240 s behind the window drew about a tenth of the estimated power it
drew unthrottled (about 0.2 W against 2.0 W, with no performance-core time),
and ran at about a third of its foreground rate. Process CPU time fell only
about 16%. Over that window iOS showed no reaction: the process kept its
foreground-focal CPU role, the thermal state stayed nominal, and no
background CPU limit fired. Taking guest CPUs offline (hot-unplug) would cap
the core count outright and is the fallback if a hotter device or a longer
window shows thermal pressure.

The mode `keepRunning` keeps a guest working for a time its owner chose (15
minutes to 2 hours, 30 by default) without a window. A timer button, offered
only to a guest with the mode, starts the job on the user's tap: iOS wants a
continued processing task to follow the user's own action, so nothing starts it
by itself. `BackgroundJob` submits one `BGContinuedProcessingTask`, whose
progress is the time used against the time chosen, and the system's progress
card shows the time left and lets the user stop it. iOS grants this task no GPU
on any device, so the guest's GPU requests stay held for the whole stay: a
guest that only computes carries on, and one that draws waits for the return.
The vCPUs are not throttled further, because iOS already runs the continued task
on the efficiency cores at background priority: a computing guest there ran at
about a third of its foreground rate for a fifth of a watt. If iOS ends the
process during a job, its card reports the job as failed, and the next launch
shows the restart notice. The job ends when its time is up, when the user stops
it, when iOS expires it, or when the guest's run returns, whether it powered
off, failed to start or kept restarting; in the background that quiesces the
guest, and in the foreground nothing changes. A job belongs to the mode, so
choosing another mode ends it. `KERNARIUM_BACKGROUND_LIMIT_MINUTES` sets the time for one launch.

An Android guest that is kept running this way has its screen turned off with
its power key as the application leaves, and on again, after the GPU is back and
the guest is running, as it returns. The screen goes off before the GPU is held,
because a visible app that is drawing when the hold lands has its render and
upload threads waiting on a GPU that does not answer, so it cannot handle the
framework's pause or the screen-off broadcast, and Android kills it as not
responding. The edge therefore presses the key, waits up to 1.5 seconds
(`gpuHoldBound`, within the four-second budget) for the guest's display to go
off, then holds the GPU and writes the guest back. The display is off when the
guest has no primary scanout, which the guest's disabled CRTC unbinds
(`kernarium_runtime_guest_display_active`, answered without a request reaching
the device, so it works as the hold lands). A display that is still on at the
bound is held anyway, because iOS revoking the GPU matters more than the
screen. A screen that is off already is neither woken nor turned on at the
return. Every other mode holds first.

The hold does not wait behind anything else. `GuestGPUHold` arms the bound as
a timer of its own, off the main thread, before the edge handler can wait, and
the screen going off or the bound, whichever comes first, takes the hold: a
main thread that is waiting for the screen, or for the guest's storage to
settle, cannot hold the hold back. iOS has denied the app the GPU 2.73 to
3.26 s after the edge in every measured edge (five, on an iPad; 4.1 to
4.7 s after the app resigned active), and a request that reaches the GPU then
is rejected. A bound of 1.5 s lands the hold 1.2 s ahead of the earliest of
those. The hold is not taken when the app resigns active, which is 0.8 to
1.7 s before the edge: the app resigns active for a pulled-down shade or a
call banner too, and freezing the guest's graphics for a user who stays would
be wrong. A screen that stays on keeps Android
busy; with it off the guest suspends itself and only its wakelock holders and
alarms run, as on a phone, so a job that should make progress has to hold a
wakelock, as Android's own downloads and jobs do. A guest in the Picture in Picture mode
keeps its screen, because its display is on show.

The Picture in Picture mode needs the `audio` background mode in `Info.plist`,
which iOS requires for any such window, and an audio session of category playback
that mixes with other audio; the window sets the session as the application
resigns active, because guest audio claims and drops it on its own terms. The
application plays no sound of its own.

## Simulator

The Simulator build is for UI tests of the application and its Android guest.
Configure and build a `kernarium-ios-simulator*` runtime preset, package it with
`scripts/package-ios-frameworks.sh --sdk simulator` (`--sdk device` beside it
gives XCFrameworks that both SDKs build against) and build the scheme for a
concrete Simulator, or run its tests with `scripts/run-ios-simulator-tests.sh`.
The Simulator product is arm64 alone, like its packaged slices: a concrete
destination builds that device's architecture, and the project excludes x86_64
for the Simulator SDK (`EXCLUDED_ARCHS[sdk=iphonesimulator*]`); a generic
Simulator destination needs `EXCLUDED_ARCHS=x86_64` as well
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)). The
GPU prefix is the software one, at
`.build/dependencies/apple-gpu-vulkan-experiment/ios-simulator-arm64` unless
`KERNARIUM_SIMULATOR_GPU_PREFIX` names another (`KERNARIUM_DEVICE_GPU_PREFIX`
does the same for the device's). The application selects Mesa's software device
(`LIBGL_ALWAYS_SOFTWARE`), which Mesa answers with llvmpipe, and embeds no
KosmicKrisp; the guest has no Vulkan and the runtime holds no GPU budget.

## Scripts

Each script documents its options in `--help` or its header:

- `run-ios-mac.sh` builds, installs, provisions, and launches the application
  on an Apple silicon Mac in Designed for iPad mode;
- `package-ios-frameworks.sh` packages the selected runtime and graphics slices
  for `--sdk device` (the default) and `--sdk simulator`, and the static GPT
  fdisk and zstd XCFrameworks for Xcode, with
  `--automation` the automation Linux runtime, with `--android-automation`
  the automation Android runtime, with `--runtimes android` the Android
  runtime alone (for Andarium; the output directory is shared, so package the
  full set again before building Kernarium), and the host battery and video
  encoder archives, a slice for each SDK, which only `KernariumTests` links;
- `embed-device-framework.sh` is the Run Script phase that embeds the
  device-only `KosmicKrisp.xcframework` for an `iphoneos` build and nothing for
  the Simulator, because an XCFramework in an Embed Frameworks phase needs a
  slice for every SDK the target builds and a platform filter cannot name the
  device apart from the Simulator;
- `run-ios-simulator-tests.sh` builds `KernariumTests` and `KernariumUITests`
  for the iOS Simulator and runs them in one iPad Simulator (`--help`);
- `make-guest-bundle.py` assembles an Android `.kern` bundle (system disk,
  layout, kernel command line) from an installed release;
- `repro-android-hold-mac.py` sends an Android guest through the background
  edge on a Mac and checks it through adb, with `AndroidHoldReproTests` as the
  application's side (`docs/testing.md`); `run-ios-mac.sh` takes
  `--android-automation`, `--only-testing` and `--test-env` for it;
- `ipad-automation.py` drives UI automation on a connected iPad through the
  `KernariumAutomation` host;
- `audio-interrupter.py` builds, installs, and launches `AudioInterrupter`, a
  test-support application that plays digital silence on a non-mixable audio
  session so that a run can interrupt Kernarium's audio without UI
  automation (`launch` starts it in the foreground, `terminate --pid` ends it,
  and `--help` lists its modes); and
- `verify-signed-kernel-carrier.py` checks that a kernel image is safe for the
  signed carrier.
