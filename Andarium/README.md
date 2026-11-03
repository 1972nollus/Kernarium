# Andarium

Andarium is the iOS application that runs one Android (LineageOS 24, Android
17) guest on the Darwin runtime. Android ships inside the app; what the user
creates, sizes and erases is the guest. It is an iOS product like
[Kernarium](../Kernarium/README.md) and, in its development configurations,
runs on an Apple silicon Mac in Designed for iPad mode; the App Store
configuration is not offered to Macs. It also builds for the iOS Simulator,
where the guest renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).
Kernarium stays the developer application for Linux guests and for Android
bundles.

## Ownership

```text
Andarium/Shared/      screens and model both targets build
Andarium/Preview/     mock screens, data and launch arguments (Simulator)
Andarium/Live/        the runtime, storage and session of the device app
AndariumCore/         Swift package: the application's model, no UIKit
AndariumTests/        hosted tests of the device app
Config/               entitlements, Info.plist, privacy manifest, bridging header
scripts/              system staging and the App Store bundle check
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
```

`../GuestHost/Sources` holds the host code Kernarium and Andarium share to
run a guest: the runtime loader, the display and touch view, the zstd utility
and the shader cache.

The project file is the JSON format Xcode 27 reads and Xcode 27.2 writes
(`project.xcproj`), kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Kernarium keeps its `.pbxproj`.

## Targets

`Andarium` is the device application (`com.imxieyi.andarium`, iPhone and iPad,
and in Debug and Release an Apple silicon Mac in Designed for iPad mode). It
embeds the Android runtime frameworks and carries the system image; Kernarium
stays beside it on a device. It has no background mode: the guest runs while
Andarium is in the foreground and is paused when it leaves, and iOS ending a
paused Andarium shows a notice on the next launch. It runs one scene, because
the runtime boots one guest per process, and needs the increased memory limit
and extended virtual addressing entitlements.

`AndariumPreview` is a Simulator-only target built from `Shared` and
`Preview` with mock data. It embeds no runtime and boots nothing. Its launch
arguments (`-screen <name>`, `-play YES`, `-lowSpace YES`, `-resetTips YES`)
open any screen or state directly so a Simulator driven only by `simctl` reaches
every flow:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

## Devices

The guest is drawn with Metal 4, which the A14 and later chips have. No
capability key names the chip, so `UIRequiredDeviceCapabilities` names `metal`
and a device without Metal 4 still installs the application. At launch the
application asks the GPU for the Metal 4 family (`HostDevice.runsGuestGraphics`).
Without it Home shows "Needs an A14 Chip or Later" and dims Start (and Set Up
Guest, when there is no guest yet); `Lifecycle` answers `needsNewerChip` to any
start. The Simulator draws on the CPU and always passes.

iPhone and iPad are supported (`TARGETED_DEVICE_FAMILY` 1,2); the App Store
configuration sets `SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD` to NO.

`ITSAppUsesNonExemptEncryption` is NO in `Config/Info.plist`.

## Build

Packaging the Android runtime alone, and staging the system from the accepted
release, need no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
Andarium/scripts/stage-system-image.py
```

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares.
The staged system, `.build/andarium-system`, is the accepted release as
installed: the read-only system disk `guest/system.img`, its
`guest/partition-layout.json`, `boot/kernel-command-line.txt` and
`release-manifest.json`, checked against the manifest and the layout (size,
sha256, 4 KiB pages) before they are copied. `--release <directory>` stages
another installed release. The application bundles the directory as it stands.
Then the device application builds from the `Andarium` scheme:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'generic/platform=iOS' build
```

The `AndariumTests` bundle runs hosted in the application on an iPad, in the
iOS Simulator or, as an iPad app, on an Apple silicon Mac (`xcodebuild test`).
For the Simulator, package its slices first (below), then:

```sh
xcodebuild test -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' \
    -only-testing:AndariumTests
```

### Configurations

The project has Debug, Release and AppStore. Release is the development and
harness build: it reads the automation environment below and prints the
thermal reports the iPad harness reads. AppStore builds as Release does and
leaves out everything that exists for automation or only pretends:

- `APP_STORE` is a Swift compilation condition of the configuration. It removes
  the launch environment (`Live/LaunchEnvironment.swift`, so no `ANDARIUM_` or
  `KERNARIUM_` name is in the application or in `AndariumCore`), and the
  thermal and uptime console reports.
- It embeds the App Store runtime, packaged by
  `Kernarium/scripts/package-ios-frameworks.sh --app-store --runtimes android`
  from the `kernarium-ios-device-android-app-store` preset, which reads no
  environment knob ([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
- It is not offered to Macs, and its Archive action uses it.
- Its last build phase runs `scripts/check-app-store-bundle.py` on the
  application, which fails the build on a missing key in Info.plist,
  an environment knob in the application or a framework,
  or a privacy manifest that does not cover a binary
  ([`scripts/check-privacy-manifests.py`](../scripts/check-privacy-manifests.py)).

```sh
cmake --preset kernarium-ios-device-android-app-store
cmake --build --preset kernarium-ios-device-android-app-store
Kernarium/scripts/package-ios-frameworks.sh --app-store --runtimes android
Andarium/scripts/stage-system-image.py
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -configuration AppStore -destination 'generic/platform=iOS' build
```

The output directory of the packaged frameworks is shared, so package the
runtime a build needs just before it. The privacy manifests of the application
(`Config/PrivacyInfo.xcprivacy`) and of the frameworks
(`Kernarium/PrivacyManifests`) declare exactly what the App Store build and the
runtimes use; a release or harness build also reads the system uptime for its
thermal report, which the application's manifest does not declare because only
an App Store build is uploaded.

The `Andarium` scheme builds for the Simulator too, with the software renderer:
build the Simulator runtime, package its slices (add `--sdk device`, with its
own runtime preset built, for XCFrameworks that both SDKs build against), and
build for a concrete Simulator. The device-only KosmicKrisp is left out by
`Kernarium/scripts/embed-device-framework.sh`. The Simulator product is arm64
alone, as its packaged slices are: a concrete destination builds that device's
architecture, and the project excludes x86_64 for the Simulator SDK
(`EXCLUDED_ARCHS[sdk=iphonesimulator*]`). A generic Simulator destination needs
`EXCLUDED_ARCHS=x86_64` as well
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)).

```sh
cmake --preset kernarium-ios-simulator-android
cmake --build --preset kernarium-ios-simulator-android
Kernarium/scripts/package-ios-frameworks.sh --sdk simulator --runtimes android
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

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

A guest has at most the shared
[vCPU limit](../docs/architecture.md#memory-and-executable-code) (the device's
processors, at most 8), which a new guest takes and the processor ruler and
`KERNARIUM_VCPU_COUNT` stop at; a stored count above it comes down on load, and
Info states the device's own processors.

Guest memory follows the shared vCPU-aware limit Kernarium uses
(`runtime/guest_memory_budget.hpp`). The application asks the loader
(`kernarium_runtime_loader_maximum_guest_memory_mib`) for the most guest RAM at
each vCPU count, under the memory limit the system sets the process (none on a
Mac) and the launch set `kernarium_runtime_loader_launch_memory_set` gives
(also what the boot request passes); `MemoryPolicy` holds those bounds and has
no formula of its own. The picker runs from 512 MiB to the bound of the chosen
vCPU count in the runtime's 64 MiB steps and follows the count live: raising
vCPUs takes memory above the new bound down to it, lowering them leaves it, and
a stored size above its bound comes down on load. A size an automation launch
states (`KERNARIUM_GUEST_MEMORY_MIB`) stays as stated. The default is 1.5 GiB or
the bound where that is lower, and 4 GiB on a Mac.

### Storage

A guest's files live in `Library/Guest/` of the application's container,
behind `GuestStorage`: `settings.json` (the guest's choices), `data.img` (its
writable data disk) and `instance.json` (the record of the Android that last
started on it). The system is Android's, read-only, and stays in the
application's release directory (`guest/system.img`); the runtime attaches the
system disk and the data disk, creates `data.img` at the first start sized by
the guest's capacity, and only grows it afterwards. Create and Erase therefore
write no disk and take no time.

`InstanceRules.decide` (`Instance.swift`, the rules and vectors of the
[Android README](../android/lineage-24/README.md#guest-instance-storage))
chooses what a start does with the carried release, and `StartPlan` is what
Home shows of it before Start (`GuestStorage.startPlan`, which changes
nothing): first start, ordinary start, update, an update that did not finish,
or a start that is blocked. An update first clones `data.img` to
`data.img.pre-<release id>` (an APFS clone), and an update counts as finished
only after Android itself reported `sys.boot_completed` in that run and the
run ended cleanly; Recovery never does.

Start on an update that did not finish tries it again: the same update, the
snapshot kept. Home's one other way back is Restore, shown only while the
snapshot exists: it puts the data back to what the update started from and
does nothing more, so Home then shows the ordinary pending update and the next
Start updates afresh from that data. Restore ends in the same state as the
rules' restore followed by a normal start. When the Android in this build is not
the one that was updating, Start is dimmed until Restore or Erase Guest. Erase
Guest deletes the snapshot too. A guest the Android in this build is older
than, or whose files do not match it, is blocked: Start is dimmed and erasing
is the way out. A guest of the first release (one `guest.img`, an
`instance.json` of schema 0) is not read: its start fails with the reason, and
Erase makes it a guest of this layout.

The runtime passes the data disk's and the release's paths to Linux in a
kernel argument that cannot carry a space, a tab, a newline, a comma, a colon
or a quotation mark. So `data.img` and the release directory stay at such
free paths, and a guest is never created at one (`DiskPath`): that is why the
guest is in `Library/Guest/` and not in `Application Support`, whose name has
a space.

Every size the application shows is binary, in the IEC units KiB, MiB and GiB,
from one formatter (`Formats.size`): the guest's capacity, data and free
space, the memory, the system, the host's free space and an installed package.
The guest sits beside the host's figures in one unit, so they differ from the
decimal gigabytes of iOS Settings (1 GiB is 1.074 GB). The first start needs
3 GiB free and later starts 1.5 GiB (`Limits`).

The guest is left out of iCloud and computer backups unless the user turns on
Include in Backups for it: Android can set itself up again, and the files take
several GiB.

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
The names are the application's, in `Live/LaunchEnvironment.swift`, which the
App Store configuration leaves out; `AndariumCore` knows the settings
(`LaunchOverrides.Setting`) and not the names.
The `KERNARIUM_` names mean what they mean in Kernarium; the automation
Android runtime (`package-ios-frameworks.sh --runtimes android
--android-automation`) additionally reads `KERNARIUM_ANDROID_ADB_AUTH`.

```text
KERNARIUM_VCPU_COUNT, KERNARIUM_GUEST_MEMORY_MIB, KERNARIUM_RETINA_MODE,
KERNARIUM_GPU_AUTO_RESIZE, KERNARIUM_ANDROID_BOOT_MODE (recovery starts
Recovery), KERNARIUM_ANDROID_DISK_SIZE_GIB (the guest's storage in GiB),
KERNARIUM_HOST_FORWARD, KERNARIUM_FOOTPRINT_LOG_MS, KERNARIUM_EXECUTION_ENGINE,
KERNARIUM_GUEST_HIBERNATION, KERNARIUM_ANDROID_ADB_AUTH
ANDARIUM_START=normal|recovery   create the guest if there is none, then start
ANDARIUM_ERASE=1                 erase the guest's data before the start
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
alert takes the focus and gives it back. iOS asks to allow local network access
only when the guest itself reaches the local network, such as a connection to a
host on the same network or a `.local` name; a boot and ordinary Internet use,
guest DNS included, raise no alert. The question is asked once per installation
and only a person answers it; the guest keeps running meanwhile, and a harness
run that provokes it waits for the focus
(`run_ios_device_android.py --wait-active`).

The container's `Documents` is the harness's; Andarium never deletes a file
it did not create. The guest is under `Library/Guest/` (`data.img`,
`settings.json`, `instance.json`) and is not the harness's to write. A launch
boots the system the build carries, so another release means another staged
build.
