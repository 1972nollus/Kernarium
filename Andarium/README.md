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
AndariumCore/         Swift package: the model and the adb client, no UIKit
AndariumTests/        hosted tests of the device app
Config/               entitlements, Info.plist, privacy manifest, bridging header
scripts/              system staging, the App Store bundle check, acknowledgements,
                      screen capture
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
the runtime runs one guest at a time, and needs the increased memory limit
and extended virtual addressing entitlements. The model and the runtime belong
to the process (`AndariumProcess`): a scene that connects again shows the same
ones.

`AndariumPreview` is a Simulator-only target built from `Shared` and
`Preview` with mock data. It embeds no runtime and boots nothing. Its launch
arguments (`-screen <name>`, `-play YES`, `-lowSpace YES`, `-resetTips YES`,
`-bootStage <step>` for a frozen boot screen) open any screen or state directly
so a Simulator driven only by `simctl` reaches every flow (`-screen bug-report`
and `-screen install-apk` play the two guest operations on the overlay).
`scripts/capture-screens.py` launches every screen on a Simulator and saves a
labelled PNG set with a `manifest.json` (device, orientation, appearance, screen
and what to look at; `--list` prints the screens):

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

Home has one Start button for a normal boot. Below its Storage card come the
System section (vCPUs and RAM as a summary; Edit opens the sheet that changes
them), Display (Retina mode), Capabilities (Microphone, Gyroscope) and Backup
(Back Up Guest Data); the Recovery and Erase section groups Start in Recovery and
Erase Guest. Setting a guest up asks only for its storage and backups. A fresh guest
opens the system's setup wizard on its first normal boot.

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

The iPhone interface is portrait-only. On iPad, Home follows the host's
orientation. During a guest session, the scene's root hosting controller
requests an interface orientation lock while the scene fills its screen, so
Android alone rotates its display. A window, or a full-screen scene whose
lock iPadOS releases, uses host-driven rotation: the guest is held at display
rotation zero and the guest's display always follows the window.
Returning to a locked full-screen scene restores the guest's previous rotation
preferences. The saved preferences live in the guest until restored, so an app
termination or guest restart cannot replace them with the temporary override.
Sensor axes stay fixed during each period of guest-driven rotation; entering
that mode mounts them to the newly locked interface orientation and discards
the previous stream's pending samples. During host-driven rotation the guest
is drawn upright in the window, so the axes follow the window's interface
orientation and each change restarts the stream (`MotionMount`).

`ITSAppUsesNonExemptEncryption` is NO in `Config/Info.plist`.

## Acknowledgements

Info lists every project the application carries, and a tap shows the
copyright holders, the license and its full text. The list is the bundled
resource `Andarium/Shared/Acknowledgements.json`, generated by
`scripts/acknowledgements.py` from the verified dependency sources and the
kernel's license files (never fetched; `--verify` compares the resource with
them). Its table names, per project, the frameworks and prefix components it
covers: the system (Android Open Source Project, LineageOS), the Linux kernel
inside the runtime, and the GPU, network, storage and compression libraries of
the iOS device prefixes. A tooling test fails when the Andarium project embeds
a framework, or an iOS device prefix contract declares a component, that no
entry covers ([`docs/testing.md`](../docs/testing.md#andarium)). Adding a
library therefore means adding its entry to the script's table and running it.

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
console lines the iPad harness reads. AppStore builds as Release does and
leaves out everything that exists for automation or only pretends:

- `APP_STORE` is a Swift compilation condition of the configuration. It removes
  the launch environment (`Live/LaunchEnvironment.swift`, so no `ANDARIUM_` or
  `KERNARIUM_` name is in the application or in `AndariumCore`), and the
  harness's console lines (`Shared/ConsoleReport.swift`, with every call to it,
  and the thermal state and uptime reads behind them). The type is in the
  application, so no such line is in `AndariumCore` either.
- It embeds the App Store runtime, packaged by
  `Kernarium/scripts/package-ios-frameworks.sh --app-store --runtimes android`
  from the `kernarium-ios-device-android-app-store` preset, which reads no
  environment knob ([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
- It is not offered to Macs, and its Archive action uses it.
- Its last build phase runs `scripts/check-app-store-bundle.py` on the
  application, which fails the build on a missing key in Info.plist,
  an environment knob or a trace of the build host (the
  linker's debug map, a home-directory path) in the application or a framework,
  a console line of the harness in the application's code, or a privacy
  manifest that does not cover a binary
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
cannot do a thing leaves its control out instead of offering it. The device
application has `bugReport`, `apkInstall` and `adbOverNetwork`, which the
runtime's adb channel provides.

### Memory

A guest has at most the shared
[vCPU limit](../docs/architecture.md#memory-and-executable-code) (the device's
processors, at most 8), which a new guest takes and the processor ruler and
`KERNARIUM_VCPU_COUNT` stop at; a stored count above it comes down on load.

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
states (`KERNARIUM_GUEST_MEMORY_MIB`) stays as stated. Setting a guest up asks
for neither vCPUs nor memory: a new guest takes every vCPU the device offers
(`min(8, physical cores)`) and the bound of that count rounded down to a half
GiB and held within 1.5 to 4 GiB, never above the bound itself
(`MemoryPolicy.defaultMemoryMiB`; 4 GiB on a Mac, which sets no limit).

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
the moment Android itself reports `sys.boot_completed` in that run, which
deletes the snapshot; Recovery never reaches it. A newer release after that
updates from the data as it is, and nothing restores the old snapshot.

Start on an update that did not finish tries it again: the same update, the
snapshot kept. Home's one other way back is Restore, shown only while the
snapshot exists: it puts the data back to what the update started from and
does nothing more, so Home then shows the ordinary pending update and the next
Start updates afresh from that data. Restore ends in the same state as the
rules' restore followed by a normal start. Any older release can update directly
to any newer release. If the carried system is newer than an unfinished update,
Start automatically restores the pre-update data, then starts the newer system
with a fresh snapshot; no intermediate release needs to finish. A missing
snapshot prevents that restoration. Erase Guest deletes the snapshot too.
A guest the Android in this build is older than, or whose files do not match it, is blocked: Start is dimmed and erasing
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
Back Up Guest Data (Home's Backup section): Android can set itself up again,
and the files take several GiB.

## Runtime

The device application drives the Android runtime through the same loader as
Kernarium. A run starts when the guest's display has its size and the disk is
ready, on a thread of its own. Android's clean power-off stops the device
services and releases the kernel's resources and guest RAM, then returns to Home;
Start can boot again in the same app process. An unclean shutdown, a host stop,
or cleanup that fails keeps the failure screen and requires reopening Andarium. The boot screen follows
the guest's console for its milestones (the kernel, Android's init and
`sys.boot_completed`, or Recovery's service) until the runtime reports boot
events. A normal boot has four stages, and the last, Startup, spins until
`sys.boot_completed`; the guest's display is laid out from the first frame and
shows dimmed behind the screen, which a plain translucent fill dims and flat
panels sit on, so the display costs what a visible one costs and the render
server samples no backdrop for it. Recovery keeps its fifth stage, Screen, on an
opaque screen.

The side keys send Android's keys as Linux key codes: Power 116 (held for the
power menu), volume 115 and 114, Back 158 and Home 172.

The guest display supports up to ten simultaneous fingers, including pinch
and other gestures. The shared [GuestHost input](../GuestHost/README.md) maps
each finger independently onto the guest's visible display.

Retina mode applies at the next start, because the runtime takes it at boot, so
the session's Settings leaves it out (`Capabilities.liveDisplayOptions`). The
guest's display always follows the window; there is no option for it, and a
value an earlier version stored is ignored.

Capabilities apply at once. Turning Microphone on asks iOS for access at that
moment; if iOS refuses, the switch goes back off, the row dims with "Not allowed
in Settings", and turning it on again says so and offers Andarium's page in
Settings. The same happens when access is taken away in Settings, which Andarium
sees whenever it becomes active (and at launch), and the guest hears nothing
while access is out of reach; allowing it in Settings after being offered the
page turns the switch on. Gyroscope has no permission: raw accelerometer and
gyroscope updates (`CMMotionManager`) need no iOS access and no purpose string,
so it is a plain switch.

Retina uses the screen's native pixels. On a 2× display, that makes each
full-screen colour or depth buffer four times the size at point resolution;
apps can keep several such buffers. These host graphics allocations count
against the same iOS process limit as guest RAM, so the RAM setting alone does
not bound Andarium's memory use.

Gyroscope is off by default. Its switch in Home's Capabilities (and the
session's Settings) shares the device's accelerometer and gyroscope with the
guest. The choice is saved with the guest.
A device with neither sensor leaves the switch unavailable. Capture starts
after a normal boot finishes, while the app is active, and stops when the
switch turns off, the app becomes inactive, or the guest ends or restarts.
Recovery never captures motion. Only the newest samples are sent, at up to
100 Hz, over private vsock port 5556; nothing listens on the host’s network.
The system image must include the [motion sub-HAL](../android/lineage-24/README.md#motion-sensors);
a missing service is reported in Capabilities and Settings.

### Statistics

The capsule over the session and the Stats rows of its Settings show the
guest's CPU, memory and frame rate from the runtime's own readings
(`kernarium_runtime_loader_read_statistics` and `..._read_guest_memory`,
[`Kernarium/README.md`](../Kernarium/README.md#statistics)). `StatsMeter`
(`AndariumCore`) turns them into figures and `AppModel` paces them:

- Nothing is read unless a view shows the figures, the application is active,
  the guest runs and the runtime offers statistics (`StatsSampling.isWanted`).
  One loop serves both views. It takes two readings a quarter second apart, so
  figures show within a third of a second, then one a second, and asks the
  guest for its memory after the second reading and with every other one.
  Leaving the foreground ends the loop; coming back starts a new baseline.
- CPU is the share of the guest's vCPUs the readings' interval used, 0 to 100,
  and the sparkline holds the last ten readings against the same 100%. Frame
  rate is the frames the display presented. Memory is the guest's own memory in
  use (`MemTotal` less `MemAvailable`, from its kernel), in the one size
  formatter. A figure with no reading is an en dash. A memory the guest cannot
  give, as when its display is off and it may be asleep, keeps its last value.
- A restarted guest (a new boot index) starts all of them over. A guest whose
  screen is off is not dimmed: its figures are as true as any.

## Android operations

Andarium is the adb host for its one guest. The runtime opens a connected stream
socket to the guest's vsock port 5555
(`kernarium_runtime_loader_connect_vsock`), so adbd is reached by no other
process and through no listening socket of the device's, unless ADB over
Network is on. `AndariumCore/Adb` speaks the
protocol on it: the message codec, the handshake, shell protocol v2, the `sync:`
reads, the `bugreportz -p` lines and the encoder that turns a PKCS#1 public key
into adb's 524-byte structure. It has no install verb. Every operation opens a
connection of its own on a thread of its own and ends it when it finishes;
cancelling the task shuts the descriptor down. A read waits with `poll` first,
because on Darwin a blocking `read` on a socket pair can miss the end the
other side announces with `shutdown(SHUT_WR)`, which the runtime does when the
guest closes a stream, and then never wakes. Every limit counts the time the
guest runs (`GuestRunClock`): it stops while `PauseController` holds the guest
paused, so Andarium in the background never times an operation out.

adbd restarts once when the guest's USB and wireless debugging both turn off,
which ends every connection in use. The next operation connects to the new
adbd (a handshake the restart cut is tried again, and a refusal meanwhile is
retried to the connect limit); a bug report or an install under way says the
guest's debugging service restarted and asks to try again, and the bug
report first stops the dump that went on in the guest. A connection the
runtime cannot make at all within its timeout is retried the same way, and at
the limit it says the guest is not answering yet, as against its debugging
service not being ready.

**Key.** `AdbKeyStore` keeps one RSA-2048 key per guest in the Keychain (the
Secure Enclave has no RSA): this device only (`AfterFirstUnlockThisDeviceOnly`),
not synchronized, in the application's own access group. It is made off the main
thread while a start prepares, when none exists, and its public key goes in each
boot request (`android_adb_public_key`), which adbd authorizes for that boot on
the vsock port whatever the guest's USB debugging setting is, so no operation
depends on it. Finding the key and making it is one locked step, so the start
that prepares and any other caller get the same key. Erase Guest deletes it;
an isolated instance has a key of its own, tagged with its name, which only its
Erase Guest deletes. The client signs adbd's AUTH token
and never offers its key: a second token means adbd refused it, and the
operation says so.

**Create Bug Report** is in every build, in a normal session. Its row in
Settings has a footer saying what a report holds, and a tap starts it at once.
Settings dismisses and the full-screen overlay shows the guest's own progress
(nine tenths of the bar) and then the copy, with a Cancel button that stops the
dump in the guest (`setprop ctl.stop dumpstatez`) and removes what it left. The
zip is staged in `tmp/BugReports/` after a check of the free space
(`volumeAvailableCapacityForImportantUsage`), is offered in the system share
sheet (a centred popover on an iPad), and is deleted on every outcome of the
sheet; each launch sweeps the folder. The guest's copy and any earlier ones are
removed after a good pull.

**Install App** is in every build, in a normal session: Settings, Apps,
Install App... picks a single `.apk` in Files (a file of another kind is
answered by a row, a `.apks`, `.xapk` or `.apkm` bundle by one of its own, and
Settings stays open). For an `.apk` Settings dismisses and the overlay shows the
file streamed in 256 KiB reads to `abb_exec:package install -r -t -S <size>` for
the first eight tenths of the bar; the guest's own install holds it there, with
the Cancel button gone, and a success closes the overlay and adds an Installed
row. A failure is an alert and a Failed row, in the words of
`PackageInstallMessages`.

**ADB over Network** lets a computer on the same network use adb on the guest.
It is off at every run, never persisted, and available only while the guest's
USB debugging is on: the toggle is dimmed with a line saying so otherwise, and
it goes off by itself, closing the listener and every connection already made
(the guest's adbd ends its non-host sessions too, and this is the second layer),
when the setting turns off, when the run ends and when the guest restarts. The
setting is read through the channel (`settings get global adb_enabled`,
`UsbDebugging`) every 3 seconds while Settings is open and every 10 while the
toggle is on; each read opens a connection, which wakes a guest that suspended
itself.

The listener is `AdbNetworkBridge` in `AndariumCore`: a BSD socket on every
IPv4 address at TCP 5555 and, for each computer that connects, a fresh stream
from `kernarium_runtime_loader_connect_vsock(5555)` with a thread for each
direction doing a blocking read and then a blocking write. A slow side holds
back its sender through the socket buffers, nothing is held beyond one 64 KiB
block per direction, an end of one side passes on as a half close, a failure
ends both, and TCP keepalive finds a computer that vanished. At most 8
computers are served at once, so the runtime's 32 open streams leave room for
the app's own operations. Plain descriptors on both sides are why it is not
Network.framework: the runtime hands back a descriptor, and the same code is
exercised on the host with socket pairs and loopback. The bridge understands no
byte of adb, so the guest's adbd authenticates the computer: its key is not the
host's, and the guest's own prompt approves it (the prompt is the guest's, and
"Always allow" there is what lets the same computer reconnect without it). The
guest pauses with the application, which has no background mode: while Andarium
is in the background a computer's adb commands wait, the kernel still completes
new connections into the listener's backlog, and a transfer in flight resumes
when Andarium returns. iOS may end a paused Andarium under memory pressure,
which closes the listener and every connection. The row shows
`adb connect <address>:5555`, with the address from `getifaddrs` (`en0` first,
then other `en` interfaces; never cellular, tunnels or link-local), read again
with the setting; without one it says the device has no Wi-Fi address. iOS
asks for Local Network access once, and the question carries
`NSLocalNetworkUsageDescription`, whose text also covers the guest's own
connections to hosts on the network. Nothing starts a listener before the
toggle. macOS reaches the guest with the launcher's `--net-forward` instead.

Privacy: the adb features add no required-reason API. The picked file's size is
the `fileSize` resource value, the limits use Swift clocks, the space check is
the disk-space API with reason E174.1 (the start's own space check already uses
it), and neither the Keychain, sockets nor `getifaddrs` is a required-reason
API.

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
KERNARIUM_ANDROID_BOOT_MODE (recovery starts Recovery), KERNARIUM_ANDROID_DISK_SIZE_GIB (the guest's storage in GiB),
KERNARIUM_HOST_FORWARD, KERNARIUM_FOOTPRINT_LOG_MS, KERNARIUM_EXECUTION_ENGINE,
KERNARIUM_GUEST_HIBERNATION, KERNARIUM_ANDROID_ADB_AUTH
ANDARIUM_START=normal|recovery   create the guest if there is none, then start
ANDARIUM_ERASE=1                 erase the guest's data before the start
ANDARIUM_STATS=1                 show the statistics over the session and
                                 print each reading
ANDARIUM_GUEST_INSTANCE=<name>   use Library/TestGuests/<name> instead of Library/Guest
ANDARIUM_ADB_TESTS=1             let AdbOverVsockTests drive the running guest
ANDARIUM_ADB_KEY_FILE=<path>     where eraseGuestMakesANewKey records the key
ANDARIUM_ADB_REARM_TESTS=1      also run the tests that need an image raising a dead dialog's prompt again
ANDARIUM_ADB_NET_HOLD_SECONDS=<n> keep the bridge up for a real adb (and write the key
                                 as tmp/adbkey; tmp/adb-net-done ends the hold)
```

An isolated instance name contains 1–64 ASCII letters, digits, hyphens or
underscores. Use it for compatibility tests that need a fresh guest while the
ordinary guest or its pending update must be preserved. The instance keeps its
own disks, settings, adb key and pause marker across launches. An invalid name
cancels the automatic start and erase requests.

For a Recovery display check, run only `AndariumTests/RecoveryDisplayTests`
with `ANDARIUM_RECOVERY_TESTS=1`, `ANDARIUM_GUEST_INSTANCE=recovery-display`
and `KERNARIUM_RETINA_MODE=0` or `1`, without `ANDARIUM_START`. It checks the
guest's display density, provides a 30-second capture window, then powers off
through Recovery's own menu. Its rendered guest frame is saved as
`tmp/recovery-points.png` or `tmp/recovery-retina.png` in the app's container.

`AdbOverVsockTests/aFreshGuestOpensTheSetupWizard()` checks a fresh guest's
unprovisioned flags and setup welcome activity, captures `tmp/setup-welcome.png`,
then powers off. Run it with `ANDARIUM_ADB_TESTS=1`, `ANDARIUM_OOBE_TESTS=1`,
`ANDARIUM_START=normal` and the fresh isolated `recovery-display` instance.
The product's first boot stays in the wizard; completing it, and harnesses that
skip it, are checked by `android-oobe` (`docs/testing.md`).

On an Apple silicon Mac, `xcodebuild test -scheme Andarium -destination
'platform=macOS,variant=Designed for iPad' -only-testing:AndariumTests/AutomationHoldTests`
is the launcher, with each setting given as a `TEST_RUNNER_`-prefixed variable
(`docs/testing.md`). The test run ends the application, so
`TEST_RUNNER_ANDARIUM_HOLD_SECONDS` keeps the host up for that long.

The console carries the runtime's own `Kernarium:` lines and these of
Andarium's: `Andarium: runtime returned <n>` when the run ends (0 is a power
off), `Andarium thermal state: phase=start|change|finish state=<state>
uptime=<seconds>`, `Andarium: application inactive|active` when a system
alert takes the focus and gives it back, and with `ANDARIUM_STATS=1` a line
per reading, `Andarium stats: cpu=<n>% fps=<n> memory-used=<MiB> memory=<MiB>
held=<MiB> boot=<n>` (`-` for a figure not read yet; `memory` is the guest's
size, `held` what the device holds of it). iOS asks to allow local network access
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
