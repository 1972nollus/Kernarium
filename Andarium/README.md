# Andarium

Andarium is the iOS application that runs one Android (LineageOS 24, Android
17) guest on the Darwin runtime. Android ships inside the app; what the user
creates, sizes and erases is the guest. It is an iOS device product like
[Kernarium](../Kernarium/README.md) and runs on an Apple silicon Mac in
Designed for iPad mode for the same reason
([`graphics.md`](../docs/graphics.md#current-path)). Kernarium stays the
developer application for Linux guests and for Android bundles.

## Ownership

```text
Andarium/Shared/      screens and model both targets build
Andarium/Preview/     mock screens, data and launch arguments (Simulator)
Andarium/Live/        the runtime, storage and session of the device app
AndariumCore/         Swift package: the application's model, no UIKit
AndariumTests/        hosted tests of the device app
Config/               entitlements, Info.plist, bridging header
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
```

`../GuestHost/Sources` holds the host code Kernarium and Andarium share to
run a guest: the runtime loader, the display and touch view, the disk and zstd
utilities and the shader cache.

The project file is the JSON format Xcode 27 reads and Xcode 27.2 writes
(`project.xcproj`), kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Kernarium keeps its `.pbxproj`.

## Targets

`Andarium` is the device application (`com.imxieyi.andarium`, iOS and an
Apple silicon Mac in Designed for iPad mode). It embeds the Android runtime
frameworks and carries the system image; Kernarium stays beside it on a
device. It has no background mode: the guest runs while Andarium is in the
foreground and is paused when it leaves, and iOS ending a paused Andarium
shows a notice on the next launch. It runs one scene, because the runtime boots
one guest per process, and needs the increased memory limit and extended
virtual addressing entitlements.

`AndariumPreview` is a Simulator-only target built from `Shared` and
`Preview` with mock data. It embeds no runtime and boots nothing; the iOS
Simulator has no Metal 4 device for the guest's graphics. Its launch arguments
(`-screen <name>`, `-play YES`, `-lowSpace YES`, `-resetTips YES`) open any
screen or state directly so a Simulator driven only by `simctl` reaches every
flow:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'generic/platform=iOS Simulator' build
```

## Build

Packaging the Android runtime alone, and staging the system from the accepted
release, need no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
Andarium/scripts/stage-system-image.py
```

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares,
and the system to `.build/andarium-system`. Then the device application builds
from the `Andarium` scheme:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'generic/platform=iOS' build
```

The `AndariumTests` bundle runs hosted in the application on an iPad or, as an
iPad app, on an Apple silicon Mac (`xcodebuild test`).

## Model

`AndariumCore` holds everything the screens decide with, so it is tested on
the host (`andarium-core`, [`testing.md`](../docs/testing.md#andarium)): the
limits and display formats, the memory policy, the guest and its life
(`Lifecycle`: create, start, boot, erase, how a run ends), the system's
description read from the release manifest, boot milestones found in the
console, and the guest's storage. The application adds only what a screen
holds: the open sheet, display options and the session's state.

A screen shows a control only when the capability behind it exists
(`Capabilities`). The Simulator preview has them all; a build whose runtime
cannot do a thing leaves its control out instead of offering it.

### Memory

Guest memory runs from 1 GB in steps of 256 MB. The top of the ruler is one
step below the recommended maximum, which is the largest guest memory that
leaves the guest's graphics budget at its floor on that device; so no
position on the ruler is one iOS is likely to end the application for. The
default is 1.5 GB, or the ruler's top where that is lower. The budget terms
(`MemoryTerms`) are measured provisional values; replacing them changes only
that value.

### Storage

A guest's files live in `Library/Guest/` of the application's container,
behind `GuestStorage`. They are not in `Application Support`: the runtime
names the disk in a kernel argument that cannot carry a space, a comma, a
colon or a quotation mark, so a guest is never created at such a path
(`DiskPath`). The first release keeps the whole system and the guest's data in
one GPT image (`SingleImageStorage`), written sparse from the system image the
application carries: Create and Erase take tens of seconds and each guest
holds its own copy of the system, and a new system reaches an existing guest
only by erasing it. That layout is interim; a layout with the system apart
from the data replaces it behind the same protocol.

The guest is left out of iCloud and computer backups unless the user turns on
Include in Backups for it: Android can set itself up again, and the files take
several gigabytes.

## Runtime

The device application drives the Android runtime through the same loader as
Kernarium. A run starts when the guest's display has its size and the disk is
ready, on a thread of its own, and lasts the launch: Android's power-off ends
it, and starting again means opening Andarium again. The boot screen follows
the guest's console for its milestones (the kernel, Android's init, its
graphics service and `sys.boot_completed`, or Recovery's service) until the
runtime reports boot events.

The side keys send Android's keys as Linux key codes: Power 116 (held for the
power menu), volume 115 and 114, Back 158 and Home 172.

Resolution and Follow Window Size apply at the next start, because the runtime
takes them at boot, so the session's Settings leaves them out
(`Capabilities.liveDisplayOptions`). Microphone applies at once and asks iOS
for access when it is turned on.

## Automation

A harness launch (`xcrun devicectl device process launch`, environment given
as `DEVICECTL_CHILD_` variables) reaches a state without touching the screen.
The `KERNARIUM_` names mean what they mean in Kernarium; the automation
Android runtime (`package-ios-frameworks.sh --runtimes android
--android-automation`) additionally reads `KERNARIUM_ANDROID_ADB_AUTH`.

```text
KERNARIUM_VCPU_COUNT, KERNARIUM_GUEST_MEMORY_MIB, KERNARIUM_RETINA_MODE,
KERNARIUM_GPU_AUTO_RESIZE, KERNARIUM_ANDROID_BOOT_MODE (recovery starts
Recovery), KERNARIUM_ANDROID_DISK_SIZE_GIB (the guest's storage in GB),
KERNARIUM_HOST_FORWARD, KERNARIUM_FOOTPRINT_LOG_MS, KERNARIUM_EXECUTION_ENGINE,
KERNARIUM_GUEST_HIBERNATION, KERNARIUM_ANDROID_ADB_AUTH
ANDARIUM_START=normal|recovery   create the guest if there is none, then start
ANDARIUM_ERASE=1                 erase the guest's data before the start
ANDARIUM_SYSTEM_IMAGE=<file>     create or erase from this raw GPT .img or
                                 .img.zst staged in the container's Documents
                                 instead of the bundled system
```

On an Apple silicon Mac, `xcodebuild test -scheme Andarium -destination
'platform=macOS,variant=Designed for iPad' -only-testing:AndariumTests/AutomationHoldTests`
is the launcher, with each setting given as a `TEST_RUNNER_`-prefixed variable
(`docs/testing.md`). The test run ends the application, so
`TEST_RUNNER_ANDARIUM_HOLD_SECONDS` keeps the host up for that long.

The console carries the runtime's own `Kernarium:` lines and these of
Andarium's: `Andarium: runtime returned <n>` when the run ends (0 is a power
off), `Andarium thermal state: phase=start|change|finish state=<state>
uptime=<seconds>`, and `Andarium: application inactive|active` when a system
alert takes the focus and gives it back. The first launch of a bundle makes iOS
ask once to allow local network access, which only a person answers; the guest
keeps running meanwhile, and the harness waits for the focus
(`run_ios_device_android.py --wait-active`).

The container's `Documents` is the harness's staging area; Andarium never
deletes a file it did not create. The guest is under `Library/Guest/`
(`guest.img`, `settings.json`, `instance.json`) and is not the
harness's to write.
