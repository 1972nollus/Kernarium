# Kernarium

Kernarium is the iOS application that runs one Linux or Android guest on the
Darwin runtime. It is an iOS device product only;
[`graphics.md`](../docs/graphics.md#current-path) gives the Metal 4 reason and
how the same device slice runs on an Apple silicon Mac in Designed for iPad
mode. Build, launch, and test commands are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium).

## Ownership

```text
Kernarium/            SwiftUI application: settings, guest selection, disk
                      capacity, decompression, console, shader cache
Runtime/              runtime framework target, one per kernel flavour
KernariumTests/       hosted application and runtime tests
KernariumUITests/     UI tests
KernariumAutomation/  long-lived UI-automation host for a connected device
scripts/              launch, packaging, bundle assembly, and automation tools
```

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

Kernarium defaults to Linux, the processor count `NSProcessInfo` reports, and
4 GiB of guest memory where the process is given the host's memory (Designed
for iPad on a Mac) or 1.5 GiB on a physical device, whose jetsam limit a 4 GiB
guest would cross and a 2 GiB guest crossed on a 4 GiB iPad mini 6. The device
value stays until the graphics-memory reductions land, and a value the user
stored is kept.

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

## Guest storage

A Linux guest boots an image file in the application's Documents directory:
the one the settings name, or, while they name none, the accepted KDE desktop
image `kde-20260929-r2.erofs`
([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)). A name the
settings already store, `rootfs.erofs` included, is kept, and every image in
Documents stays selectable. An Android guest is a `.kern`
bundle directory in Documents; the
[Android README](../android/lineage-24/README.md#apple-consumers-and-guest-bundles)
owns its layout. A bundle holds the guest's persistent, directly writable image,
and every launch reopens that same image, so rebuilding the application or a
runtime framework must not erase it and a bundle is never recreated merely to
relaunch it. When copying a bundle to a device, target the complete
`Documents/<name>.kern` destination; never copy into or replace the Documents
directory itself, which can flatten the bundle or destroy other guests.

Launch does not compare kernel, runtime, or rootfs release identities; missing
or malformed inputs fail at their ordinary file or image boundary. Disk
capacity and compressed delivery are application concerns, so runtime
frameworks receive only an ordinary disk image and no capacity policy.
`DiskUtil` owns a raw file's capacity before launch and dispatches any
format-specific partition-table update; Android GPT growth is in the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract).
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
loopback, because authorization off also makes `adb root` available to
whatever reaches the forward. A host on USB reaches a loopback forward through
`iproxy`; a forward on every interface or on a network address is refused.
The packaging script refuses a release runtime that carries the switch.

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

## Devices and display

Kernarium attaches:

- an absolute touchscreen and evdev keyboard;
- a signed-16-bit 48 kHz playback stream (stereo) and capture stream
  (mono or stereo) through Virtio-Sound, whose capture records silence unless
  the user turns on Microphone;
- an Android-only Virtio battery reflecting the Apple host's percentage and
  charging state, with a 100-percent charging fallback when the host has no
  readable battery;
- SLiRP networking from the verified iOS prefix;
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
waiting up to eight seconds for the guest's suspend reply, which follows its
file system sync. A return to the foreground resumes the guest once that reply
has arrived or the wait has ended.

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
the guest is running, as it returns. A screen that stays on keeps Android busy;
with it off the guest suspends itself and only its wakelock holders and alarms
run, as on a phone, so a job that should make progress has to hold a wakelock,
as Android's own downloads and jobs do. A guest in the Picture in Picture mode
keeps its screen, because its display is on show.

The Picture in Picture mode needs the `audio` background mode in `Info.plist`,
which iOS requires for any such window, and an audio session of category playback
that mixes with other audio; the window sets the session as the application
resigns active, because guest audio claims and drops it on its own terms. The
application plays no sound of its own.

## Scripts

Each script documents its options in `--help` or its header:

- `run-ios-mac.sh` builds, installs, provisions, and launches the application
  on an Apple silicon Mac in Designed for iPad mode;
- `package-ios-frameworks.sh` packages the selected runtime and graphics slices
  and the static GPT fdisk and zstd XCFrameworks for Xcode, with
  `--automation` the automation Linux runtime, with `--android-automation`
  the automation Android runtime, and the host battery and video encoder
  archives only `KernariumTests` links;
- `make-guest-bundle.py` assembles an Android `.kern` bundle from an installed
  release;
- `ipad-automation.py` drives UI automation on a connected iPad through the
  `KernariumAutomation` host; and
- `verify-signed-kernel-carrier.py` checks that a kernel image is safe for the
  signed carrier.
