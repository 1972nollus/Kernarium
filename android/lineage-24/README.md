# LineageOS 24 UML guest producer

This directory owns the standalone LineageOS 24 guest producer for Darwin UML.
LineageOS 24.0 is Android 17: its tree is the AOSP `android-17.0.0_r1` release
with LineageOS's forks of the projects it changes. The exact source identity and
product name live in [`metadata/lineage.env`](metadata/lineage.env). The guest
kernel is not owned here: it is the mainline kernel line built with its Android
overlay, described in
[`../../kernel/mainline-6.18`](../../kernel/mainline-6.18/README.md#android-product).

The producer runs locally on an x86_64 Linux builder. Repository scripts accept
builder-local paths only; transport, machines, accounts, and credentials are
outside this repository.

## Producer boundary

`build/prepare-sources.sh` is the only producer command allowed to use the
network. It pins the Android source and the guest Mesa source. All patching,
builds, verification, image assembly, and publication are offline. A
`build/make` topic (route `p0002`) keeps that true of `lunch`: LineageOS's
`roomservice.py`, which `lunch` ran for a product it could not configure and
which reads a repository list from GitHub, is never called, so a product
configuration error stops the build with its own message.

The producer has three independent outputs:

1. the `lineage_andarium` target files of the configured build variant, which
   carry the kernel-less `init_boot` and LineageOS recovery images;
2. one static GPT guest disk containing both boot ramdisks; and
3. a compact rootfs release installed by Apple consumers.

Use the scripts under `build/` in that order. Each script's `--help` is the
canonical interface for builder paths and optional inputs. Ninja and the
preserved AOSP output directory own incremental builds; the wrapper does not
introduce a second build system.

The kernel is an input, not an output. It is the `android-6.18` artifact the
Mac builds from the mainline line with
`kernel/mainline-6.18/build/build-kernel.sh --product android` and publishes
with `publish-artifact.py --product android`; the builder receives the
published directory. Updating the artifact must not republish the GPT or
invalidate a guest bundle. The Android build, the disk assembly and the release
publisher take their kernel from such an artifact (`--kernel-artifact`) and
verify it against the tracked mainline source. Its `vmlinux` is the runtime
image; the build's own `vmlinux` keeps the DWARF pahole reads and does not fit
the product's 64 MiB boot image. A release ships the artifact's kernel files
unchanged, and its `kernel/config` must satisfy the Android required and
forbidden lists of the mainline line.

## Source and patch ownership

`metadata/base-manifest.xml` pins every Repo project; `tools/snapshot-source.py`
records a freshly synchronized workspace as that snapshot and its digests in
`metadata/lineage.env`. A project the manifest reads at a branch could move,
so `tools/render-pin-manifest.py` turns the snapshot into a Repo local
manifest that source preparation syncs with.
`patches/projects.tsv` maps each changed project, named with the manifest
remote that serves it, from its exact base to final tree. Generated patch
series under `patches/projects/` are the only maintained modifications to
upstream projects.

`overlay/` owns the complete `device/kernarium/andarium` product tree and is
synchronized as one verified unit. The synchronization stamps a file whose
bytes or mode differ from what the workspace held with the current time, and
restores the time of every file that did not change: Make treats an output newer
than its input as current, and a checkout keeps the time of the commit that
wrote a file, so an overlay that switches back to older content would otherwise
leave the installed copy of the newer content in place, while untouched files
keep the build from redoing work that depends only on them. The Vulkan HAL's
Mesa source has its own pinned source and generated series under
`third_party/mesa-venus/`.

Every series here uses the shared
[`scripts/patch_series.py`](../../scripts/patch_series.py) format and is
identified by the tree it produces: replay applies it to the pinned base and
requires the final tree recorded in `projects.tsv` or the owning
`metadata.env`. Release manifests compute patch counts and digests from the
series files when they are published.

Never edit generated patch files. Amend clean Git topic branches and run the
matching regenerator under `tools/`; it exports the series and records its
final tree. Export refuses a topic whose author is a placeholder; amend it as
described in [`kernel/mainline-6.18/README.md`](../../kernel/mainline-6.18/README.md).

## Android product

The product is a 64-bit system with a 32-bit application ABI, 4 KiB-only, and
built-in-kernel-only. Applications of both ABIs run from one image:
`arm64-v8a`, `armeabi-v7a` and `armeabi`, in that order. The 32-bit side is
ARMv8-A AArch32 on a cortex-a53, built as the softfp `armeabi-v7a` ABI; the
`armeabi` fallback runs older native applications through the same arm runtime,
and `ro.zygote` is `zygote64_32`. Every daemon and HAL stays
64-bit (`IGNORE_PREFER32_ON_DEVICE`), so 32-bit code runs only in applications,
the two zygotes and the WebView RELRO process. Recovery, `init_boot` and the
vendor ramdisk are 64-bit only. ABI compatibility does not change the platform's
minimum installable target SDK. An older APK can be tested with the explicit
`adb install --bypass-low-target-sdk-block` option; ordinary installs keep the
platform check. ABI support does not restore an older Android runtime: the
unmodified ZENONIA3 1.0.7 APK selects `armeabi` and loads its native library,
but its resource parser crashes on this ART runtime before gameplay.
The kernel runs 4 KiB pages, because Android's
32-bit libraries are 4 KiB aligned; the 64-bit userspace stays page-agnostic (a
16 KiB maximum ELF alignment and no compile-time bionic page-size macro), so it
is valid on a kernel of either size, and `ro.product.page_size` reports 4096.

A `device/google/cuttlefish` topic (route `p0001`) adds the
`UML_ANDROID_MULTILIB` switch, which makes the page-agnostic phone inherit
`core_64_bit.mk` instead of `core_64_bit_only.mk`; `overlay/lineage_andarium.mk`
sets it and `overlay/BoardConfig.mk` defines the second architecture. The
builder asserts that configuration, and the target-files verifier checks the
image: the ABI lists, the 32-bit runtime, the placement of every ELF (32-bit
files are 4 KiB aligned EABI5 softfp objects under `lib/`, `lib/arm` or
`bin/*32`, and 64-bit ones never sit there), and ART's per-ISA artifacts. The
LineageOS WebView declares `use32bitAbi`, which would make its 32-bit libraries
primary and every renderer a 32-bit process. A `frameworks/base` topic (route
`p0007`) lets a product disregard that attribute for system packages, and the
product sets `ro.config.ignore_use32bitabi`: renderers stay 64-bit, and a
32-bit application loads the 32-bit WebView library in its own process and
talks to a 64-bit renderer. The same route makes system_server preload the
secondary zygote only while an installed package has a 32-bit primary ABI, and
when the first one is installed later; without such a package the zygote keeps
about 13 MiB less, and it still forks the WebView RELRO process at boot, which
does not need the preload. system_server waits for that task, so the task never
throws: a failed package lookup, at boot or for an added package, preloads as
stock Android does, and an added or changed package is looked up by itself. The same project's route also carries a LineageOS
correction: its WebView service drops the configured providers that are not
installed, asking the package manager for every user, and does so under the
identity of the calling application, which the package manager refuses without
`INTERACT_ACROSS_USERS`. The topic runs that availability check as the system,
so any application reads the provider list, as on AOSP. A further topic
of the route lets a media router client set the volume of the built-in speaker
route: without a Bluetooth adapter the device route controller is the legacy
one, whose volume request did nothing, and the topic sets the music stream
volume as the system.

The build variant is the single `LINEAGE24_BUILD_VARIANT` setting in
`metadata/lineage.env`, `user` or `userdebug`. Lunch, the target-files verifier,
the release manifest and the accepted-release descriptor all follow it. `user`
is the default and the only variant that is released, signed with release keys,
published or accepted: `publish-release.py` and the release verifier refuse any
other (`require_release_build_variant`). A `user` build keeps the stock shell
privilege boundary: no `su`, no `adb root`, `ro.debuggable=0`, no permissive
SELinux domain, and the verifier rejects the debug-only payload (`su`,
`remount`, `procmem`), any application whose manifest sets
`android:debuggable`, and the in-tree test probes by name. `userdebug` is the
on-demand diagnostics variant: it carries `su`, is debuggable and may have the
permissive `su` domain, and the verifier requires exactly that. Each variant
builds in its own output directory (on the builder `out-andarium-user` and
`out-andarium-userdebug`), so switching between them is incremental;
`build-aosp.sh` records the variant in its output directory and refuses the
other one. The variant also decides the keys: every
`user` build, a release or a CTS candidate, is signed with the one release key
set, and a `userdebug` build keeps the AOSP test keys of the build itself and
is never re-signed
([Release signing](#release-signing)).

The product identity is Andarium's, not a Cuttlefish or "Darwin" one:
`PRODUCT_BRAND` Andarium, `PRODUCT_MANUFACTURER` Kernarium, `PRODUCT_MODEL`
"Andarium Virtual Device", `PRODUCT_DEVICE` andarium and `PRODUCT_NAME`
`lineage_andarium` (the makefile, `AndroidProducts.mk` and the device directory
follow LineageOS's `lineage_<device>` convention), so the lunch target is
`lineage_andarium-cp2a-user` and the product output directory is
`target/product/andarium`. The system partition stays AOSP's generic system
(`ro.product.system.*` is Android / generic / Generic System), so the identity is
in the product and vendor partitions (`ro.product.product.*`,
`ro.product.vendor.*`) and in the public `ro.build.fingerprint`, which `Build.*`
reports. The verifier checks the identity there and that the fingerprint starts
`Andarium/lineage_andarium/andarium:` and names the variant; the system
partition's own identity is not constrained. `darwin` stays the name of the
host platform.

Diagnostics on a `user` image start on the host and need no guest root: the
guest console the host captures for kernel logs, UML's mconsole (`sysrq`,
`proc`, `stack`, and `mem` for the memory counters, which it reads without
opening `/proc/meminfo`) for the kernel's state, and tracefs from the shell (the
image mounts it at `/sys/kernel/tracing`, readable and writable by `shell`). The
shell can also set `perf.drop_caches` (init writes `drop_caches`), the `debug.*`
properties and `powerctl`. Only what these cannot reach (`dmesg`, `setenforce`,
`ctl.*` service control, other uids' `/proc/<pid>/smaps`) needs a `userdebug`
build with `su`; build one on demand in its own output directory.

The product removes Cuttlefish virtual hardware and services that have no
Darwin UML implementation while preserving Android framework APIs required by
ordinary applications.

The product is `lineage_andarium`: the Cuttlefish base with LineageOS's
`common_full_tablet_wifionly` product chain on top, so the tablet configuration
and the LineageOS platform services (LineageParts, LineageSettingsProvider and
the hardware, live-display, trust and global-actions services) are present.
`overlay/lineage_andarium.mk` sets the LineageOS switches before inheriting it
and names what the product leaves out. The image provides Settings, Trebuchet,
Files, Package Installer, the LineageOS WebView, LatinIME, ThemePicker and a
curated application set: Glimpse (gallery), Twelve (music), Etar (calendar),
ExactCalculator, Camelot (PDF viewer) and Seedvault (backup). It has no browser
app (the WebView stays, so that a user can install one), no app store of its
own and no install restriction: Package Installer and every install path work.
It has no Google or microG services. LineageOS Setup Wizard handles the first
boot of a fresh guest; completing it provisions that guest (both settings 1 and
the wizard's HOME activity disabled), so subsequent boots go directly to the
launcher. Until then HOME returns to the wizard. The image has no trade-in
mode, and so adbd is one process through a first boot. AOSP's DeviceDiagnostics
asks Android's trade-in mode to start from a boot receiver on a first boot that
has not been set up, which turns USB debugging on, and the virtual Wi-Fi that
is already connected stops it at once, which turns it off; the framework
restarts adbd each time, twice a few seconds apart, some tens of seconds after
`sys.boot_completed` (about 230 s into the first boot on the iPad), and an
install or a bug report running over adbd was cut. TradeInModeService has no
configuration switch and starts only for a package that holds
`android.permission.ENTER_TRADE_IN_MODE`. DeviceDiagnostics is the one that
asks at boot (the shell package holds the permission too, for what a person
types over adb), so the product excludes DeviceDiagnostics
(`KERNARIUM_REMOVED_PACKAGES`, asserted by `build-aosp.sh`). The verifier
refuses its directory and any APK of the image partitions other than the
shell's that requests the permission; a manifest that declares the permission,
as framework-res.apk does, is not a request. A virtual device has no trade-in.
A harness that erases
a guest to measure it provisions the guest over adb after `sys.boot_completed`
([`tools/android_provision.py`](../../tools/android_provision.py), through
`tools/boot_android.py --provision`, `tests/perf/run_ios_device_android.py
--erase` and `tests/perf/android_launch_suite.py`): it sets both settings and
disables the wizard package for user 0 (the shell may not disable the wizard's
own components), so HOME resolves to the launcher, and it changes nothing in
the image. It returns only after adbd has stayed one process for six seconds
with no trade-in mode pending, which costs those six seconds on this image and
keeps a harness correct on an older release that still carries DeviceDiagnostics
(r15 and earlier, where provisioning at `sys.boot_completed` also keeps the
mode from starting: "Not starting trade-in mode, device is setup"). A measurement
of the first-boot experience itself keeps the wizard.
The `android-oobe` test completes the wizard through its pages and checks that
the helper reaches the same observable state. The LineageOS updater, Canvas,
Aperture, Profiles, Recorder, QuickAccessWallet, the charger animation,
fastbootd, AudioFX and the Jelly browser are excluded, as are DeskClock and
EmergencyInfo; the build and verifier assert both lists. SecureElement, the
OMAPI service for readers the machine does not have (no eSE, no UICC), is kept
but is not persistent (a `packages/apps/SecureElement` topic, route `p0024`,
drops `android:persistent`): nothing starts it at boot, so it costs no memory
at idle.
The first `SEService` client that binds it starts the process, and the bind
creates the service, which registers itself (the topic also drops the
application's own `startService`: the system refuses it in a process it started
for a bound service, and the application crashed on every bind). The service
lives while a client is bound and is destroyed with the last unbind, the
process stays until the system reclaims it, and nothing restarts it; a client
still connects, which CTS's OMAPI version test needs. The OMAPI features are not declared. QuickSearchBox and its
home-screen search surface are deliberately omitted. Bluetooth
device implementations and feature declarations are also omitted. Settings is
the stock application apart from its Bluetooth parts (a `packages/apps/Settings`
topic, route `p0009`): the Connected devices entry, which holds only Bluetooth,
the Bluetooth developer option and the media output switcher are gone. The
Network page is the stock one with its Wi-Fi page. The Wi-Fi hotspot and
wireless display pages are hidden, because the virtual Wi-Fi interface is a
station only, and so is System update, because the synced AOSP tree has no
activity implementing that intent. The modular framework API
containers remain part of base AOSP so ordinary applications can still resolve
the Android SDK classes; excluding them also removes required system-server
dexpreopt inputs. Telephony,
GNSS, NFC, UWB, Thread, camera, biometric, vibrator, secure-element,
USB, and Trusty hardware is not advertised.

### Motion sensors

The product installs Android’s AIDL multi-HAL service and the 2.1 motion
sub-HAL in `overlay/sensors/`. It advertises an accelerometer and gyroscope,
with continuous, non-wake-up delivery, 10 ms minimum periods and no hardware
FIFO, direct channels or data injection. Each sensor’s activation and sampling
period are independent; flush posts a completion after earlier delivered
events. The target-files verifier requires the service, its VINTF instance,
feature declarations and exactly this backend in `hals.conf`.

Andarium supplies samples only when the user enables Motion Sensors. The
sub-HAL accepts a single stream on vsock port 5556 from host CID 2; it accepts
no guest-originated sensor source. Its SELinux domain uses the platform’s
explicit virtual-device vsock classification, with only create, bind, listen,
accept, read, getattr and getopt on its own sockets; no connect or ioctl access
is granted. Each 20-byte record is little endian:
`ANS1`, a uint32 Android sensor type (1 accelerometer or 4 gyroscope), then
three float32 values in m/s² or rad/s. Axes match the mounted virtual display
(x right, y up, z out of the screen). Andarium keeps them fixed while Android
owns rotation; its [scene policy](../../Andarium/README.md#devices) describes
windowed operation and returning to a locked full-screen scene.
Core Motion acceleration is multiplied by −9.80665: a stationary,
face-up device reports +9.80665 on z. Gyroscope units and signs carry through.
Events use the guest’s CLOCK_BOOTTIME at receipt; this is a live input path,
not a synchronized recording of host acquisition times. Invalid records close
the stream. Disconnecting preserves no partial record or sample; a host can
reconnect after foregrounding or a guest reboot.

### Networking

The guest's one network device, `eth0`, is the host's user-mode network (SLiRP:
10.0.2.0/24, gateway 10.0.2.2, DNS 10.0.2.3), and it is the management NIC: adb
is reached through the host's forward to 10.0.2.15, so `eth0` keeps that address
and is never managed by the framework. `kernarium_net_setup`
(`overlay/tools`, a vendor binary that uses Cuttlefish's `cuttlefish_net`
netlink library) prepares it at `post-fs-data`, and init waits for it. After
init has set `arp_ignore` and `arp_announce`, so that no interface answers ARP
for another's address, the tool

- addresses `eth0` with 10.0.2.15/24 and sends the host's DHCP server one
  DHCPREQUEST for that address from `eth0`'s MAC, which makes the server
  reserve it before any other MAC exists;
- adds the priority-0 rule `from 10.0.2.15 lookup 5555` and the link route of
  that table, so the replies adbd sends leave `eth0` whichever network the
  framework makes the default; and
- stacks the private macvlan `buried_eth0` (MAC `02:15:b2:00:00:00`) on `eth0`
  and the `virt_wifi` device `wlan0` on that, the way Cuttlefish does. Both
  links are created without a link-flag mask, so they keep `IFF_MULTICAST`.

The tool fails closed: without the lease or the rule it creates no `wlan0`, so
adb keeps working and nothing else can take the management address. The kernel
needs `CONFIG_MACVLAN`, `CONFIG_CFG80211` and `CONFIG_VIRT_WIFI` for the last
step; a kernel without them leaves `eth0` configured and `wlan0` absent, and
apps with no network. The target-files verifier therefore requires all three
built in (`=y`) in the packaged kernel configuration.

The framework sees the Wi-Fi and only the Wi-Fi. The product declares
`android.hardware.wifi` and not `android.hardware.ethernet`, so the system does
not start Ethernet management and `eth0` never becomes a network of its own.
Apps get one network: Wi-Fi, the open SSID `VirtWifi`, unmetered, with the lease
10.0.2.16 and the host's DNS. Turning Wi-Fi off, or airplane mode, takes apps
offline while adb keeps working, since adb rides `eth0`. The Wi-Fi HAL
(`com.android.hardware.wifi`) and the supplicant
(`com.google.cf.wpa_supplicant`) are Cuttlefish's packages. `wificond` is not
shipped: the `cp2a` release configuration sets `RELEASE_DISABLE_WIFICOND`, which
keeps it out of the product, and the Wi-Fi service then sends nl80211 commands
itself (it falls back to that whenever `init.svc.wificond` is unset). The
product excludes `wificond` and its compatibility symlink by name, and the
target-files verifier rejects `SYSTEM/bin/wificond`. `BoardConfig.mk` offers the
HAL one station interface (`WIFI_HAL_INTERFACE_COMBINATIONS`),
because `virt_wifi` has no access-point or peer-to-peer interface type: there is
no SoftAP, Wi-Fi Direct, Aware or RTT, and Passpoint stays undeclared until the
Wi-Fi CTS runs show what it needs. `KernariumWifiOverlay` (`overlay/rro`) turns
MAC randomization off, because `virt_wifi` cannot change its MAC address and a
second MAC would need a second lease. No Connectivity overlay that keeps `eth0`
out of the Ethernet interface regex is needed: without the feature declaration
the Ethernet service does not run.

Once boot completes, `kernarium_wifi_network.sh` (`overlay/tools`, installed on
`system_ext` and run in a core domain of its own, because it drives the Wi-Fi
service through `cmd wifi`, which a vendor domain may not, and which hands the
service a binder to answer on, so the domain may transfer binders to
`system_server` and `system_server` may call the domain back) saves the open network `VirtWifi`, the one network `virt_wifi`
shows. It saves it once: the
marker `persist.sys.kernarium.virtwifi_saved` is set only after the network is
listed, so a boot that cannot save it tries again, and a network the user
forgets stays forgotten. It never turns Wi-Fi on or connects, so the user's
Wi-Fi switch decides whether the saved network is joined.

The vendor audio policy exposes the 48 kHz stereo primary playback path and one
built-in microphone, the Virtio sound capture stream (16-bit PCM at 48 kHz, mono
or stereo), supplies the AOSP software effects configuration, and disables
AAudio MMAP and exclusive modes. The product declares
`android.hardware.microphone` permanently, because one build fingerprint cannot
report a feature on only some boots (CDD 7.8.1 C-0-7). What the stream carries
is the host's setting: silence by default, so recording APIs always open and
return zeros, or the host's microphone when the user turns it on. The image
needs a host with the capture stream; on an older host an application can open
the input and the ALSA open then fails. The target-files verifier requires the
declaration and exactly this input. The product also declares
`android.software.companion_device_setup`, which the handheld declaration it
replaces carried: system_server starts the CompanionDeviceManager service only
with that feature, and the service runs without Bluetooth. The verifier
requires it. Settings omits its Bluetooth media-output
switcher so the remaining volume and vibration controls do not instantiate a
radio-backed media manager. A dedicated Virtio
battery device reports the Apple host's percentage and charging state through
the generic Android Health service; hosts without readable battery data report
an always-present battery at 100 percent and charging. The battery's sysfs path
carries its position among the Virtio devices each host composes. The
standalone macOS launcher and the Kernarium iOS runtime both pass it straight
after the GPU, and the vendor policy labels only that position, so a host that
places another Virtio device before the battery leaves it unlabelled.

Both the framework and SettingsProvider defaults disable the non-secure
keyguard, set the screen-off timeout to the maximum supported value, and keep
the display awake while the host supplies virtual AC power. LatinIME remains
eligible even while a host keyboard is attached. Fresh settings databases seed
every user-facing audio stream at its maximum and disable UI, lock, dial-pad,
charging, and power sounds; subsequent volume and sound settings remain owned
by the user.

Fresh settings databases keep Developer options and USB debugging off: the
overlay defaults Developer options off and the product sets no
`persist.sys.usb.config`, which AdbService reads at every boot to set
`adb_enabled`; the target-files verifier rejects the property in any build
property file. On a user build init's property service (stock
`update_sys_usb_config`) fills the empty property with `none` at every boot, so
"off" is an empty or `none` value, never `adb`; a debuggable build gets `adb`
from the same code. `ro.adb.secure` keeps the build type's value (1 on a user
build): the `device/google/cuttlefish` topic that `UML_ANDROID_KEEP_ADB_SECURE`
switches on (route `p0001`) drops the `ro.adb.secure=0` that the inherited
Cuttlefish user block adds to the product partition, and the target-files
verifier rejects a false value in any build property file, because Settings
hides "Revoke USB debugging authorizations" while the property is false. The
radio-free product starts adbd on TCP port 5555 and on vsock port 5555, and
retains the stock authorization prompt for a newly connected host. Three
immutable launch properties, which adbd reads itself (the p0011 topics), shape
that:

- `ro.boot.kernarium.adb_auth` can disable the authorization check for
  host-side diagnostics: iOS fixes it to one (only the automation runtime lets a
  test launch turn it off, on a loopback forward,
  [`../../Kernarium/README.md`](../../Kernarium/README.md#automation-runs)),
  while the standalone macOS product fixes it to zero.
- `ro.boot.kernarium.adb_tcp` is 0 when the launch has no host forward, as in
  the iOS release: adbd then listens on `vsock:5555` alone, so no application
  of the guest finds a TCP listener. Without the property, or with any other
  value, adbd listens on both, which the standalone macOS launcher and the test
  tooling rely on through their forwards. A `service.adb.listen_addrs` value
  still wins. adbd reads the launch property and not that one because
  `service.adb.listen_addrs` is a `system_prop`, which `vendor_init` may not
  set, whereas the `ro.boot` properties are `bootloader_prop`, which every
  domain reads: nothing in SELinux or init changes.
- `ro.boot.kernarium.adb_key` is the base64 adb public key that the host
  application, as the adb host of its guest, authorizes for the boot. adbd
  offers it first to every key check (the AUTH signature, the TLS client list
  and the TLS certificate check), so that host needs no prompt and no write to
  the framework's `adb_keys`, which the framework expires and empties. The
  guest's user cannot revoke it. The host owns the guest's disk, so the key
  gives it nothing it lacks, and the value is a public key.

AOSP's `neverallow` keeps untrusted applications from creating or connecting a
vsock socket, so only the host reaches the vsock listener. Three topics keep
adbd the host's channel whatever the user's debugging setting, and keep the
user's setting meaningful:

- The first `frameworks/base` topic (route `p0007`) records the USB debugging
  setting in `persist.sys.usb.config`: this product has no `UsbDeviceManager`,
  the property's only run-time writer (init writes it at boot only), so nothing
  recorded it, and AdbService, which sets `adb_enabled` from the property at
  every boot, forgot the user's choice at every reboot. `settings get global
  adb_enabled` (1 on, 0 off) is authoritative and follows the user's toggle
  within the settings observer's hop.
- The second restarts adbd (`ctl.restart`) where stock stops it when USB and
  wireless debugging are both turned off. Every session ends, as on stock, and
  adbd stays up for the host, whose own session drops once at that toggle and
  reconnects. Init also starts adbd again after any other stop
  (`on property:init.svc.adbd=stopped` in `overlay/config/init.vendor.rc`): a
  restart or a crash never raises that trigger, so the two do not fight.
- The adbd topic of route `p0011` is the key rule. The host's key
  (`ro.boot.kernarium.adb_key`) is accepted whatever the setting. Any other key
  authenticates, and may raise the stock "Allow USB debugging" prompt, only
  while USB debugging or wireless debugging is on (`persist.sys.usb.config`
  holds `adb`, or `persist.adb.tls_server.enable` is true). With both off, such
  a key is refused with no prompt, nothing to accept and nothing queued for a
  later prompt; the transport stays unauthorized. A computer on the LAN that
  the host bridges arrives over vsock from the host as well, so the rule is by
  key, not by origin. Authorization off (`ro.boot.kernarium.adb_auth`) skips the
  check and so the rule. adbd listens on guest TCP only when the launch has a
  host forward.
- The last `frameworks/base` topic keeps a dialog that dies unanswered from stranding adbd's
  prompt. The dialog (`UsbDebuggingActivity` of SystemUI) answers the framework through its
  buttons and through `onDestroy` when it is finishing. A process that dies under it, or a task
  that the system destroys before SystemUI has created the activity (SystemUI needs many seconds
  to do that in the first half minute after boot, and a foreground change such as the Setup
  Wizard's next step or the launcher's first start trims the task), sends no answer, and
  adbd's libadbd_auth then keeps that prompt dispatched, logging `prompt currently pending,
  skipping` for every later offer. `AdbDebuggingManager` therefore looks for the dialog's
  activity in the system's task list once, 15 s after each start of the dialog, and starts the
  dialog again when it is gone while the prompt is still awaited: at most three restarts per
  prompt, each but the last followed by one more look. A prompt stops being awaited at the user's allow or
  deny, a request to clear the keys, USB debugging turned off, a new request and the adbd socket
  closing. Nothing is scheduled while no prompt is awaited. A dialog that dies later than its
  look is not raised again; the prompt stays pending until adbd restarts (USB debugging off, then
  on in a separate command, because the restart ends the shell that asked for it).
  `AdbOverVsockTests/aPromptWhoseDialogDiedIsRaisedAgain()` kills SystemUI under the dialog, and
  `aPromptWhoseDialogIsUpIsNotRaisedTwice()` checks that a dialog that stays up is left alone.

The target-files verifier requires the adbd APEX's `bin/adbd` to carry the
launch property names, the debugging property names and the refusal line (the
payload hints store that binary uncompressed, so they are readable), the built
policy to let adbd read their labels, the dex files of `services.jar` (stored
uncompressed) to carry the first topic's log line, and the product init script
to start adbd at the stopped trigger. Bundle command lines may not override any
of these host-owned launch choices.
LineageOS makes its debug variant non-debuggable and always authorized; the product undoes the first
(`PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG` is empty) and the boot policy decides
the second. Under `userdebug` the build is debuggable and carries `su`, and
`adb root` goes through LineageOS's `adb_root` service, whose "Rooted
debugging" switch is off on a fresh data partition. A boot with authorization
off starts that service with the switch on, so on macOS, which authorizes
nothing, any local process that reaches the loopback ADB port can become root
in a userdebug guest, an accepted property of a diagnostics host. On a `user`
build the service is installed but built non-debuggable, so authorization off
still gives an unauthenticated shell and `pm install`, never root: the bypass
topic (route `p0011`) turns the switch on only when the build is debuggable, so
`adb root` is refused at once and the connection stays, where a switch that was
on would make adbd announce a restart as root that no init trigger of a user
build performs and leave the client waiting about 45 s. The target-files
verifier requires the topic's log line in `adb_root`, and the adb debugging
runner (`tests/integration/android/run_android_adb_debugging.py`) asks a user
guest for root. iOS keeps authorization and the default-off switch.
SELinux stays enforcing in both variants; only the host-owned kernel command
line could change that. Android's wireless-debugging setting
is the stock one: it trusts a Wi-Fi network by its SSID and publishes its mDNS
services on the guest's Wi-Fi interface. Fresh
notification state shows silent notification icons in the status bar. Active
DownloadProvider transfers use a silent low-importance channel so their
progress icon remains visible there; later notification changes remain owned
by the user.

The product uses software KeyMint/gatekeeper, SELinux enforcing policy, no
`system_other` image, and no loadable modules or DLKM partitions.

LineageOS's vendor policy lets `system_server` read the vendor security patch
level (`ro.vendor.build.security_patch`), which `SecurityStateManager` reports to
every application, while the platform policy denies applications the same read.
An application would see the level through the API and an empty property.
`overlay/sepolicy/vendor/appdomain.te` grants the read to application domains;
the value is public, and the target-files verifier requires the rule in the
packaged vendor policy.

The example audio service is two processes, the core HAL and the effects HAL, in
one SELinux domain, and the core HAL transacts with the effects HAL's binder.
The platform policy gives a HAL domain no binder call to itself, so every stream
open was denied (the audit rate limit hid how many).
`overlay/sepolicy/vendor/hal_audio_default.te` allows the call, and the
target-files verifier requires the rule.

The example Wi-Fi HAL reads the platform's `persist.vendor.debug.wifi.*`
properties at start-up, which its domain may not do, so every boot logged an
`hal_wifi_default` to `persist_vendor_debug_wifi_prop` denial. The read is the
AOSP HAL's own and harmless (an unset property reads as its default), so
`overlay/sepolicy/vendor/hal_wifi_default.te` allows exactly that read, never a
set and never a blanket `dontaudit`, and the target-files verifier requires the
rule in the packaged vendor policy.

The guest kernel has no ashmem, the product sets `sys.use_memfd`, and the
vendor policy enables `memfd_class`, so the shared buffers an application
creates are memfd files labelled with the application's domain. A MediaCodec
queues its encrypted input in such a buffer and the ClearKey DRM HAL maps it to
decrypt into it, which the platform policy refuses (it grants a HAL the buffers
of its declared clients, not those of applications). Every decrypt of a ClearKey
stream failed. `overlay/sepolicy/vendor/hal_drm_clearkey.te` lets that HAL get
the attributes of, read, write and map the memfd files of application domains
other than isolated apps. It names the ClearKey domain alone: `hal_drm_server`
also covers the Widevine HAL of the system. The target-files verifier requires
the rule in the packaged vendor policy.

The software decoders of the swcodec APEX advertise less than they decode: VP9
up to 2048x2048 and HEVC at 2,000,000 8x8 blocks per second (1080p at 60 frames
per second). Both decode 3840x2160, which the Mainline media CTS expects some
component of every device to accept. `overlay/config/media_codecs_c2.xml`, the
file that also lists the hardware codecs
([below](#hardware-video-encoding-and-decoding)), updates the two entries (the
vendor file is parsed after the APEX's), and the target-files verifier requires
the installed file to allow 3840x2160 at 30 frames per second.

Two CTS results that the image does not cause. LineageOS turns
`Settings.Global.restricted_networking_mode` on (lineage-sdk's settings
provider) and gives only applications that request INTERNET the network. A test
application that requests none, such as the one of the content resolver sync
tests, gets `POLICY_REJECT_ALL`, and the JobScheduler's connectivity constraint
of its sync job never holds: the product keeps this LineageOS privacy default,
and a CTS run sets the setting to 0 when the guest starts. A CTS test that
reboots the device (`PreferredActivityTests#testAddDuplicatedPreferredActivity`
expects the package manager to deduplicate preferred activities at the reboot)
cannot pass where reboots are disabled, as they must be when the emulator exits
on reboot.

LineageOS's firewall also gives every network, restricted ones included, the
allow-all UID range from 0 to `Integer.MAX_VALUE`. Netd lets a UID use a
network that a range includes whatever its permission bits, so any application
could bind a socket to a restricted network. A `packages/modules/Connectivity`
topic (route `p0003`) gives a restricted network no range, as on AOSP.
LineageOS also declares `Settings.Global.UIDS_ALLOWED_ON_RESTRICTED_NETWORKS`
`@hide` without `@Readable`, which makes the key unreadable to applications
that AOSP lets read it; a `frameworks/base` topic (route `p0007`) adds the
annotation.

MediaProvider's FUSE daemon kept its state on the stack of the session thread,
while the Java side keeps calling into the daemon until that thread deletes it.
A public volume that unmounts right after it mounts reached the volume's
leveldb setup once the state was gone, and MediaProvider aborted on a destroyed
mutex. A `packages/providers/MediaProvider` topic (route `p0019`) keeps the
state until the daemon is deleted and makes that setup return when the session
has ended.

LineageOS's SELinux policy serves an ext4 external SD card and addon.d updates
with grants that Android's neverallow tests refuse: `mediaprovider_app` is an
`mlstrustedsubject` (so it may fork and write `privapp_data_file`), `vold` may
relabel a filesystem to `sdcard_posix`, `fsck_untrusted` holds `sys_admin`, and
`backuptool` is permissive. The platform build lets them through because
LineageOS relaxed the matching neverallows in its fork. This product has no
removable card and no addon.d, so two topics undo them: `system/sepolicy`
(route `p0020`) reverts the `mediaprovider_app` change and restores AOSP's two
neverallows, and `device/lineage/sepolicy` (route `p0021`) drops the `vold` and
`fsck_untrusted` grants and the permissive `backuptool`. With the neverallows
back, a later such grant fails the policy build, and the target-files verifier
repeats the checks on the built policy (a user build has no permissive
domain; a userdebug build may have only `su`).

The guest boots straight to the home screen, and its swipe lock screen can be
turned on. `LockPatternUtils.isLockScreenDisabled` ORs
`config_disableLockscreenByDefault` with the stored `lockscreen.disabled`
value, so the overlay sets the flag false: a true flag keeps the lock screen off
whatever `locksettings set-disabled false` or the Settings app sets, and the CTS
keyguard tests need one that can be shown. The default is the product property
`ro.lockscreen.disable.default=true`: `LockSettingsService` stores
`lockscreen.disabled=1` for user 0 when it creates its database, which a fresh
`/data` does on the first boot, and the target-files verifier requires the
property. A `/data` that already holds a lock settings database has no such
value and shows the swipe lock screen; the standalone launcher gives every
release a new `/data`. Other users and work profiles start with the lock screen
on. The settings provider's `def_lockscreen_disabled` plays no part: nothing
reads `Settings.System.lockscreen.disabled`.

`mediaserver` loads the vendor's gralloc mapper to allocate decoder buffers, and
minigbm reads the option `vendor.minigbm.debug` when it initialises. That name
is a `vendor_default_prop`, which a core domain may not read; the lookup finds no
value and the mapper carries on. Android silences the same denial for
applications, SurfaceFlinger and the boot animation, and the system_ext policy
silences it for `mediaserver`, which the verifier requires.

The guest's virtual keyboards (the VNC keyboard, and the devices that `uinput`
and the virtual device manager create) belong to no display. LineageOS's swap
volume buttons port makes every keyboard that `EventHub` does not count as
external orientation aware, and it counts only USB and Bluetooth devices as
external. An orientation aware keyboard is also tied to the internal display,
while the virtual input device tests wait for a device that has no display, so
they timed out. A `frameworks/native` topic (route `p0022`) keeps LineageOS's
default for the keyboards of a built-in bus and AOSP's for `BUS_VIRTUAL`, which
virtio-input, `uinput` and virtual devices report; `keyboard.orientationAware`
in an input device configuration still decides either way.

The app-ops service records only the ops of a configured list into its
discrete history (`privacy/discrete_history_ops_cslist`). LineageOS replaces
AOSP's default with an Android 12 list through SimpleDeviceConfig, so every op
added since never reaches the history: the health and sensor reads, the trigger
audio and emergency location ops, the test op of the historical registry CTS
tests, and the ops AOSP records for security. SimpleDeviceConfig applies its
`configs_base` array (LineageOS's) and then `configs_device`, so
`overlay/overlay/packages/apps/SimpleDeviceConfig` sets the second to
LineageOS's list united with the ops of `DiscreteOpsRegistry`'s defaults. The
target-files verifier requires the generated overlay to cover the defaults; a
list that drops one of them also fails the `lineage24-target-files-fixtures`
test.

LineageOS's default wallpaper is a nearly detail-free square, and its `sw600dp`
and `sw720dp` variants, which every display of 600dp and more draws, are 1920 by
1920 pixels. SystemUI's `ImageWallpaper` fixes its surface to the bitmap, so a
1133 by 687 display held a 14 MiB wallpaper buffer that SurfaceFlinger scales
into a wallpaper window as wide as the display's long side, 1133 pixels. The
window, and with it the home screen's parallax range, comes from the display and
the launcher's page offsets, not from the bitmap, so the overlay
(`overlay/overlay-priority/frameworks/base/core/res/res`) replaces those two
variants with a 1024 by 1024 copy of the same image, 4 MiB. Against the original,
both resampled to a window of 687 to 2560 pixels, no pixel differs by more than
three of 255 levels (PSNR above 56 dB). The platform's own
`config_optimizeDefaultWallpaper` is no substitute: it crops to the display's long
side and scales up, 19.6 MiB at 2266 by 1374.

The wallpaper also sets the Monet palette. SystemUI quantizes a 112 by 112 mini
bitmap that `Bitmap.createScaledBitmap(bitmap, 112, 112, false)` takes with one
nearest-neighbour texel per sample, and a plain resize of the 1920 pixel image
moves the accent colour from LineageOS's #589EA8 to #78ACB9 although no pixel
differs visibly. `tools/default_wallpaper.py generate` therefore writes the
original's own sample into each texel the copy is sampled at (the positions were
read on a guest), 0.4% of the texels by at most five levels, so both images give
the quantizer the same pixels and the palette stays LineageOS's.

LineageOS's overlay names the same resource, and where two overlay directories do
that the one listed earlier in `PRODUCT_PACKAGE_OVERLAYS` wins (Soong gives the
earlier directory the higher priority). `inherit-product` expands the inherited
list where its call stands, so `overlay/lineage_andarium.mk` lists
`overlay/overlay-priority` before its calls and `overlay/overlay` after them,
where it ranks under LineageOS's and Cuttlefish's: a resource that both define is
theirs. That is why the wallpaper has a directory of its own.
`tools/default_wallpaper.py` checks the committed files and that order, and
regenerates the files from LineageOS's pinned file (`generate --source`); the
target-files verifier requires the built framework overlay's two variants to be
squares of at most 1024 pixels, which is where the order shows.

When applications update together while their tasks are in a desk (LineageOS
enables desktop windowing on tablets), WM Shell's `PackageUpdateController`
starts a `PackageUpdateActivity` placeholder in the task of each, once per
package and in an order that two package update threads decide. The placeholder
was `singleTop`, and the activity starter delivers a `singleTop` start to the top
activity of the focused root task when it is the same component. A desk is one
root task, so with the topmost task handled first the start for the next task
was delivered to the first task's placeholder as a new intent: the next
application kept running through its update and was killed by the package stop
timeout without saving its state (`CtsStopAndKillHostTest`). A `frameworks/base`
topic (route `p0007`) makes the placeholder a standard activity, so every
updating task gets its own.

The virtio-gpu display is paced by a vblank timer in the kernel, so the present
fences of drm_hwcomposer signal when a frame is shown. The inherited Cuttlefish
graphics configuration sets `ro.vendor.hwc.drm.present_fence_not_reliable=true`,
written for a host that sends frames as fast as it can. With it set,
SurfaceFlinger does not take present timestamps from the fences and the Vulkan
loader does not expose `VK_GOOGLE_display_timing`, which the CTS frame timeline
tests use. The product drops the property through the vendor build.prop denylist
(`PRODUCT_VENDOR_PROPERTY_BLACKLIST`: a filter over the product variable cannot
remove an inherited value, and a second assignment is a duplicate the build
refuses), and the target-files verifier rejects any partition that sets it true.
The guest needs a kernel with the vblank pacing.

Incremental installs (`adb install --incremental`, the Incremental FS service)
need the vendor property `ro.incremental.enable=1`, which the inherited
Cuttlefish configuration sets, and a kernel with Incremental FS. The
target-files verifier pins the property so a change in the inherited product
cannot drop it unnoticed.

A first boot compiles nothing. ART Service would otherwise compile every
package without a usable artifact, for each instruction set the package runs
in, on a fresh `/data` (reason `first-boot`, filter `verify`), and Soong leaves
two groups without one: the applications packaged inside updatable APEXes,
which it never dexpreopts, and the second instruction set of any package,
since Soong dexpreopts an application for the first only. A presigned prebuilt
can opt out as well; the image carries none.
`tools/precompile-first-boot-apps.py` compiles exactly those after a first
`target-files-package` pass, with the host `dex2oat`, Soong's own application
flags, filter `verify` and the class loader context ART Service derives on the
device (the manifest's libraries plus the implicit ones
`PackageBackwardCompatibility` adds). Soong records an application invocation
for the first instruction set only, so the tool takes the arm64 one and swaps
in the `arm` instruction-set flags Soong recorded for its libraries, which
share the boot image location and differ from their arm64 invocation in
nothing else; it checks that against every library recorded for both, and
refuses to derive the template when any of them disagrees. A package runs in arm64
unless native code puts it elsewhere: a multiArch package with libraries for
both ABIs runs in both, so the multi-ABI WebView needs `arm` artifacts too, and
a package with libraries for one ABI only runs in that ABI's instruction set.
The tool derives the set the way `PackageAbiHelperImpl` does, from the APK and
the library directories beside it, and records each artifact's ISA in its
manifest. It writes the artifacts where ART looks first:
`/system/framework/oat/<isa>/apex@<apex>@<path>@classes.{odex,vdex}` for an APK
under `/apex`, and `oat/<isa>` beside every other APK. A second pass
installs them through a generated `PRODUCT_COPY_FILES` fragment named by
`LINEAGE24_FIRST_BOOT_OAT_MK`, which `overlay/lineage_andarium.mk` includes, and
`build-aosp.sh` then runs the tool's `verify` against the packaged tree: every
APK with dex code must carry artifacts for its whole ISA set, and the tool's
own must match its manifest and the packaged APK's dex checksums. An unchanged result skips the
second pass. The artifacts are tied to the exact dex checksums and class loader
context, so a package that is later replaced (an APEX update, a different
class path) is recompiled by ART Service as usual; the image has no such
update channel.

The boot animation is off (`androidboot.enable_bootanimation=0`; the vendor
init script turns `debug.sf.nobootanimation` on for it), so SurfaceFlinger
draws nothing until the first real frame.

Boot runs with expedited RCU grace periods (`rcupdate.rcu_expedited=1` on the
command line), which shortens it by a few seconds. Once `sys.boot_completed`
is set, the vendor init script writes 0 to `/sys/kernel/rcu_expedited`, so a
running guest returns to normal grace periods and does not interrupt every
vCPU for each one. The target-files verifier requires that action. Recovery
loads no vendor init script and stays expedited for its short run. The command
line also sets `nbd.nbds_max=0`: Android uses no network block device, and the
kernel's built-in driver would otherwise create 16, each with a receiver thread
and its stack, about 7 MiB of guest memory. The Mac launcher and Kernarium both
read this one file from the release, so the setting reaches both.

The guest has a compressed swap of half its RAM. `fstab.uml` names one zram
entry (`zramsize=50%`) and the same init action that restores the RCU grace
periods runs `swapon_all` on it once `sys.boot_completed` is set, so boot
itself compresses nothing. The apps the system starts around boot completion
keep their anonymous memory in zram, which zswap fronts, rather than in guest
RAM that nothing but killing the app could reclaim: it compresses about 4:1 and
frees about 80 MiB of a 1536 MiB guest at idle, and the launcher workload
refaults about a quarter of the file pages it did. The verifier requires the one
fstab entry and the one `swapon_all`, at boot completion and nowhere else.

PinnerService does not lock the home app's files. It would otherwise keep the
launcher's APK, odex and vdex, about 54 MiB, resident whatever the memory
pressure; without the pin those pages are reclaimed like any app's, and the
time back to the home screen after a run of other apps did not grow.
`KernariumPinnerOverlay` (`overlay/rro`) sets `config_pinnerHomePinBytes` to
zero and leaves the WebView pin alone. That pin locks the first 20 MiB of
`webview.apk` (the ICU data, the dex and the start of the native library). A
cold start of a WebView app touches about 77 MiB of the 160 MiB library, spread
over its code, and only 9.5 MiB of what it touches lies inside the locked 20, so
a `pinlist.meta` of hot ranges would need either the whole touched set or a pin
half this size that does the same; dropping the pin outright costs about 5% more
major faults and disk reads on a cold WebView start. The pin sizes are not
overlayable, so the
overlay is built for SDK 28, the last level whose overlays are not held to the
target's overlayable declarations, and the target-files verifier reads the
built manifest for that: an overlay that targeted a later level would apply to
nothing and say nothing.

Several apps start at every boot and then sit cached, holding memory until the
low memory killer wants it: the privacy sandbox services (the ExtServices
`.adservices` process, FederatedCompute, OnDevicePersonalization), the backup
apps (Seedvault, LocalContactsBackup and the local transport), key provisioning
(`rkpdapp`), domain verification (`statementservice`) and the Health Connect
controller. Their boot receivers and the system's bindings stay, because each
schedules work or serves a feature; disabling them would lose it. Instead
`kernarium_reclaim_boot_apps.sh` (`overlay/tools`, system_ext, its own core
domain) waits 120 s after `sys.boot_completed`, reads `dumpsys activity lru` and
runs `cmd activity kill --user 0` for each listed package whose processes are
cached. That kills only what the system could have killed itself: alarms, jobs
and bindings stay, and the next use starts the app again. A package with a
process the system holds as a started service, the previous app or the home app
is logged to logcat (tag `kernarium_reclaim_boot_apps`) and left alone, as is a
package with no cached process; `kill-all` and `force-stop`, which cancel alarms
and jobs, are never used. The list is a reviewed table in the script and in
`tools/verify-target-files.py`, which require each other, and the script runs once
per boot.

### Time

The guest's wall clock is the host's: the runtime gives the guest the host's
wall clock at boot and its kernel then advances it with the host's monotonic
clock, and a resume corrects it by what the suspend left. The platform
therefore takes no network time. `NetworkTimeUpdateService` polls NTP whenever
a default network exists, whatever `config_autoTimeSourcesPriority` says, and
`NtpTrustedTime` reads its servers from `config_ntpServers`, so the overlay
sets that list to one entry that is not an `ntp://` URI, the only value that
leaves the client without a server (it rejects the list before any lookup; an
empty list makes `NtpConfig` throw in `system_server`), and LineageOS's own
list for mcc 460 is replaced in `overlay-priority`. The origin priorities drop
the network too: `telephony` and `external`, because an empty list means the
platform's default, which holds the network, and `telephony` alone would make a
platform without telephony hide "Set time automatically". The setting stays,
and a host clock step while the guest runs is corrected at its next resume or
boot, not at once. The target-files verifier reads the built framework overlay
and fails an NTP server, a network origin, an empty list or a missing list.

### Branding and diagnostics

The Setup Wizard names the system Andarium. The strings that carry the name
(`os_name`, `setup_services`, `update_recovery_title`, `update_recovery_setting`
and `update_recovery_description`) come from a runtime resource overlay in
`overlay/overlay/packages/apps/SetupWizard/app/src/main/res`, one file per
locale that translates them, because a locale that translates a string would
otherwise show LineageOS's text over the default. The application's resources
sit in a Gradle layout, which the overlay path follows. Settings, About, the
boot animation, the fingerprint and the build properties keep LineageOS's names.
Recovery's own text names no system. The target-files verifier requires the
wizard overlay's resource table to hold the Andarium strings and no Lineage name.

The artwork is the Andarium icon's three stacked tiles. In the Setup Wizard
the same overlay replaces the vector drawable `logo.xml` (the Finish screen,
and the services page header, where the layout tints it and only the three
silhouettes remain) and the Lottie animation `lottie_welcome.json` of the
Welcome screen. Recovery's header images (`logo_image.png`,
`logo_image_switch.png` and `fastbootd.png`, 256 by 160) and its 30 progress
frames (200 by 200) are mdpi pixel art in `overlay/recovery/res/images`: the
build copies a device's `recovery/res` over the stock resources, and recovery
scales the images to the host's density with nearest-neighbour sampling, so
they are drawn on the 1x grid and in greys that no byte order changes. The
recovery verifier requires each of those files byte for byte and no other
progress frame, and the target-files verifier requires the wizard's animation
byte for byte and every path of its vector.

LineageOS reports device data to its statistics server from LineageParts
(`ReportingService`, started by a boot receiver and a daily job) once
`stats_collection` is on, and the Setup Wizard asks for it on its services page.
The image has neither. Route `p0025` removes LineageParts' statistics feature
whole: the receiver and services, the preview screen, the part in the catalogue
that Settings lists and searches, the Trust entry, the endpoint and the strings.
LineageParts runs in the system uid, so a manifest cannot take its network
access away, and only removed code guarantees that nothing posts. Route `p0026`
removes the wizard's diagnostic data row, its privacy paragraph and policy URL,
and the code that wrote the answer to `stats_collection`, whose default stays
off. What is left of the services page is the on-screen navigation keys row,
which a device with a key disabler shows; this product has none, so the page
skips itself and disables its own activity. The target-files verifier searches
both applications' manifests, dex files and resources for the uploader's
components, endpoint and setting, and fails the image that still carries one.

## Graphics

Both graphics APIs come from one Mesa build of this project's own guest Mesa
source rather than from the AOSP Mesa snapshot, so the Android guest and the
Linux guest run the same drivers against the same renderer. GLES is Mesa's
Virgl gallium driver behind `libEGL_kernarium`, `libGLESv1_CM_kernarium` and
`libGLESv2_kernarium` in `/vendor/lib64/egl`, which the platform EGL loader
selects through `ro.hardware.egl=kernarium`; the three libraries link
`venus_gallium_dri.so` in `/vendor/lib64`. HWUI renders in the application
process, so a 32-bit application loads the 32-bit builds of the same libraries
from `/vendor/lib/egl` and `/vendor/lib`. The product excludes the inherited
Cuttlefish `libGLES_mesa`, and the AOSP `external/mesa3d` project stays at its
pinned revision. The Vulkan HAL is Mesa's Venus driver,
`VK_DRIVER_ID_MESA_VENUS` over Virtio-GPU, selected through
`ro.hardware.vulkan=virtio`.

The boot command line is the only writer of the three graphics HAL selections:
the product's vendor init script copies `androidboot.hardware.egl`,
`androidboot.hardware.vulkan` and `androidboot.hardware.gralloc` into their
`ro.hardware` properties at early-init, and that copy fails for a property a
vendor property has already set. The target-files verifier therefore refuses a
vendor `ro.hardware.egl`, `ro.hardware.vulkan` or `ro.hardware.gralloc`.

The command line also sets `androidboot.hw_timeout_multiplier=5`, which the
vendor init script copies to `ro.hw_timeout_multiplier`. The framework scales
its ANR and watchdog timeouts by it (`Build.HW_TIMEOUT_MULTIPLIER`), so the
slower engines, the gadget interpreters on the Mac and on the iPad, do not raise
ANRs that a phone-speed device would not. Cuttlefish uses 3 on its native
architecture and 50 when it emulates another one. Two CTS cases assume 3 or
less: `JobSchedulingTest#testAnr_onStartJob` and `#testAnr_onStopJob` wait 30 s
for the ANR that the job scheduler raises 8 s times the multiplier after an
unanswered `onStartJob` or `onStopJob`, which is 40 s here (24 s at 3, where
both pass). They are a timing deviation of the product, not a defect, and the
multiplier stays.

That script replaces the inherited Cuttlefish graphics init script. It expands
only `androidboot` properties the tracked command line sets, because a copy
from a missing one fails at every boot; the goldfish composer mode and the
ANGLE feature overrides have no source on this product and are not copied. The
one host-supplied value, `androidboot.lcd_density`, is copied to
`ro.sf.lcd_density` in an action gated on its presence. Every launcher passes
it, and a release's command line may not set it. Kernarium passes the density of
the EDID it advertises. The standalone macOS launcher passes its `--gpu-dpi`
value, which also sets the EDID, or 213, the density SurfaceFlinger itself
falls back to when a display has no physical size, so the property is never
empty (CTS `DisplaySizeTest` parses it as an integer).

The display follows the host view. When the host advertises a new size
([`../../docs/graphics.md`](../../docs/graphics.md#dynamic-native-geometry)),
the Virtio-GPU connector's mode list changes under an attached display, and
two routed projects carry that through Android. `external/drm_hwcomposer`
(route `p0017`) rebinds a connected display whose modes changed, so
SurfaceFlinger reconnects once at the new size with no headless step between.
`frameworks/base` (route `p0007`) keeps a display's state when a snapshot taken
between two quick reconnects names an active mode its own list lacks, which
otherwise crashes `system_server`. Density stays at its launch value. The
touchscreen's absolute range is fixed at 0 through 32767 on both axes, and the
host maps the guest's visible rectangle onto it
([`../../docs/graphics.md`](../../docs/graphics.md#dynamic-native-geometry)),
so touch stays exact at every size the display takes.

The iOS host's Virtio touchscreen supports ten independent contacts using Linux
type-B slots and tracking IDs. The product declares the multitouch, distinct
multitouch and jazzhand features, and target-files verification requires them.
VNC remains a single pointer; the recovery UI also accepts that input. Recovery
reads the host-supplied display density for its layout and scales its
fonts, localized text and graphic assets once at load time. Text stays readable
at Retina resolution throughout the Recovery screens.
Recovery init publishes `ro.boot.lcd_density` as the readable `ro.sf.lcd_density`,
as vendor init does in normal Android; the boot property is vendor-private.
The `recovery_minui_test` Soong target covers glyph boundaries and alpha/RGBA
scaling, including padded rows and fractional display densities.

`third_party/mesa-venus/` pins that source as three independent inputs: the
upstream Mesa revision, the guest Vulkan series under
[`../../tools/rootfs/vulkan/`](../../tools/rootfs/vulkan/README.md), and the
Android port series beside the metadata. The port series carries the generated
Soong port and the few source changes only Android needs, such as advertising
`EGL_EXT_pixel_format_float` only with a float config: HWUI takes the extension
as the promise of a half-float window config, and the Android visuals are all
fixed-point: RGBA8888, RGBX8888, BGRA8888 and RGB565, and gralloc offers no
10-bit or half-float colour-output buffer either, so a 10-bit render target is
unavailable and the CTS cases that use one skip on their own support query. The
port also offers a window config whose only depth-stencil plane is eight bits of
stencil, when the host renderer advertises that format
([`../../docs/graphics.md`](../../docs/graphics.md#guest-capability-boundary)):
HWUI asks for stencil and no depth for every window, `eglChooseConfig` sorts a
config without a depth plane first for such a request, and the window then holds
one byte per pixel where the smallest config with a depth plane held four. The
window's stencil plane is the drawable's depth-stencil attachment, shared by
every context that draws to the window, as a depth-stencil plane is. An
application that asks for no depth and uses one anyway no longer gets it, as the
EGL specification allows. The port also leaves out `VK_KHR_device_address_commands`,
which the CTS refuses on a device whose declared dEQP level does not list it. A guest replay assigns new
commit identities each time it is recreated, so the guest half is pinned by
tree. Moving to a newer guest epoch is a metadata bump, a replay, and a
regeneration; the verifier refuses a guest series that changed underneath the
recorded pin.

The port itself is generated. `tools/generate-mesa-venus-port.py soong` runs AOSP's
`meson2hermetic` converter with the checked `soong-port.toml` and the platform
description beside it, `soong-platforms.toml`, for two host platforms: arm64
and arm. The description answers Mesa's configure-time checks for each
instruction set (the converter runs no compiler), and the converter merges the
platforms into `select(arch())` expressions. Soong accepts a select only on a
property whose Go type is `proptools.Configurable`, so the tool keeps the
selects on `srcs` and `cppflags` and moves the values of a property like
`conlyflags` into an `arch` block; it refuses a select on any property that
its tables, taken from Soong's property structs, do not list. The converter
drops a generated source whose
command differs between platforms, and the OpenGL command marshalling bakes in
the pointer size, so the tool converts each platform alone as well, keeps the
first platform's generators under their names, adds the others as
`<name>_<arch>`, and has each library that uses one select it by arch. It also repairs the few
places where Soong needs more than Meson states, drops every module the
installed modules neither build from nor link, and then scopes every remaining
module name with a `venus_` prefix because `external/mesa3d` already defines
the same Mesa module names and Soong module names are global at the Make layer.
The installed modules keep the names the platform loaders resolve:
`vulkan.virtio` through `ro.hardware.vulkan`, and the three EGL and GLES
libraries through `ro.hardware.egl`. Soong makes every warning an error outside
`device/` and `vendor/`; a warning Mesa's own build tolerates therefore needs a
per-module `[[kernarium.warning_exception]]` entry, with its reason, in
`soong-port.toml`.

Meson compiles every C and C++ file with `-D_FILE_OFFSET_BITS=64`, and the
converter does not carry that compiler default over. The tool adds it, in the
defaults every compiled module uses, for each arch whose platform has
four-byte pointers. Without it a 32-bit module keeps a 32-bit `off_t`: the
Venus renderer's `mmap` of a DRM offset above 4 GiB then fails with `EINVAL`.
The 64-bit port is unchanged. The generator test refuses a committed port with
an arm module built without the define, and the target-files verifier refuses
a 32-bit library of the graphics stack that has a 64-bit twin and imports `mmap`
or `lseek` rather than `mmap64` or `lseek64`. The platform's other 32-bit
libraries import the plain functions for offsets that never leave 32 bits, so
the rule covers the Mesa and minigbm libraries alone.

Soong has no equivalent of the guest build's out-of-tree
`-Dvenus-protocol-dir`, so the port carries the driver half of the Venus
protocol codec at the tree path the upstream snapshot reserves for it.
`tools/generate-mesa-venus-port.py protocol` writes it by running
venus-protocol's own generator from clean replays of the two sources the
renderer and the guest rootfs generate their halves from: the protocol series
under [`../../tools/rootfs/vulkan/`](../../tools/rootfs/vulkan/README.md) and the
Vulkan-Headers registry its `sources.tsv` pins. `metadata.env` records both
trees, and the generator refuses a replay whose tree is not the recorded one.
The codec's first line names the two trees rather than venus-protocol's replay
commit, which changes whenever a replay is recreated, so the same pins always
reproduce the same bytes; `--check` compares a checkout's codec with that
generation.

The converter is a pinned builder input too:
`third_party/meson2hermetic/` records its upstream commit and a local series,
and the generator refuses a converter checkout whose tree is not the recorded
final tree. The series links a shared library as itself rather than as its
static closure, so each library links only its direct Meson dependencies, and
passes Meson's C arguments to C sources only.

`vulkan.virtio.so` is a same-process HAL: it is loaded into every application
that uses Vulkan, so the product labels it `same_process_hal_file`. The platform
labels `/vendor/lib64/egl` the same way, and the product adds the label for the
gallium driver those libraries link, in both library directories.

Mesa reads a buffer's DRM format, modifier and plane layout through gralloc, so
the product installs the allocator's own `gralloc.minigbm` module and selects
it through `ro.hardware.gralloc=minigbm`: the passthrough module answers none of those
queries and the driver would have to guess the layout of the buffers the window
system hands it. That module is labelled `same_process_hal_file` too. minigbm
allocates a GPU-only buffer as a virtio-gpu blob, which is the form the renderer
can share with the Vulkan render server, and keeps a buffer the guest itself
reads or writes on the classic transfer path, which a CPU mapping still needs.
The compositor's framebuffers are such GPU-only blobs; the host presents each
through the Metal texture that stores its GL texture, as
[`../../docs/graphics.md`](../../docs/graphics.md) describes.

Such a blob has no pages the guest can map, because the kernel refuses to map a
blob that is not mappable, so the guest's Virgl driver reaches it only through
copy transfers. It imports a host blob with staging on whenever the renderer
copies in both directions, and a second import of a handle it still holds
reports the blob's type as the first did. Hardware bitmap uploads and the
read-back of a window or an EGL image rely on both, and on the renderer reading
a blob texture back through its staging buffer's guest storage
([`../../docs/graphics.md`](../../docs/graphics.md#current-path)).

Presentation is `VK_ANDROID_native_buffer`: the platform loader allocates the
swapchain images from gralloc and the driver imports them. Those blobs belong to
the renderer's GL context, and an Apple host has no dma-buf to hand across, so
the renderer gives such a resource a host shared-memory segment and exports
that; the contract is in
[`../../docs/architecture.md`](../../docs/architecture.md). SurfaceFlinger then
takes the presented layer on the scanout as a device composition. The product
ships `kernarium_vk_present`, which owns a layer, wraps it through
`VK_KHR_android_surface` and presents a swapchain, as the pass-or-fail check of
that path. `kernarium_vk_ahb_import` is the check of the import side: it fills
hardware buffers of several formats and widths through the CPU, imports each
into Vulkan and compares what the driver reads back, which fails for a row
pitch the driver cannot import or the renderer lays out differently.
`kernarium_ahb_yuv_roundtrip` is its counterpart for YUV buffers: NV12, P010 and
YV12 of even and odd sizes are written whole and then by a partial lock and read
back through the host copy, which fails for a layout the guest and the renderer
read differently. `kernarium_ahb_rgba_lock` checks the lock of tall RGBA buffers
whose rows gralloc pads (widths 340 to 350 at 604 rows): a buffer the GPU wrote
and a buffer the CPU wrote are each compared at the stride, and a padded lock
that takes more than four times an unpadded one fails the run.
`kernarium_egl_configs` lists the EGL configs and picks the one HWUI's request
chooses (RGBA8888, no depth, eight bits of stencil); `--expect-stencil-only` fails
unless that config has no depth plane, and a pbuffer of it must stencil: a draw
that writes only stencil and a draw that passes only where the stencil matches
colour the two halves of the picture apart.

The product declares `android.hardware.vulkan.version` 1.4, `level` 1 and
`compute` 0, and deliberately declares no `android.software.vulkan.deqp.level`:
this driver has not been qualified against Android's Vulkan dEQP must-pass list.
A declared level makes the CTS run every list dated up to it, so a declaration
waits for those lists to pass whole. One gap is known in this
stack: `vkMapMemory` of host-visible memory exported as a hardware buffer fails
with `VK_ERROR_MEMORY_MAP_FAILED`, because gralloc backs such a buffer with a
classic 3D resource, which Venus refuses to map by design (its contents reach
the guest only through transfers), and a renderer-created blob in its place has
no guest mapping on this host. The two
`api.external.memory.android_hardware_buffer.*.host_visible.import_twice` cases
of the 2019 list are the ones that map it.

The Android hardware buffer handle type a swapchain needs is guest Mesa work,
so it arrives with the guest Vulkan epoch pinned in
`third_party/mesa-venus/metadata.env`. The HAL and the host renderer must also
speak the same Venus protocol revision: a change to the protocol series or the
Vulkan-Headers pin leaves an older release's HAL unable to talk to a newer
renderer. `tools/verify-mesa-venus-source.py` therefore refuses a protocol or
registry pin that differs from its owner, and a port codec whose first line
does not name the recorded pins; such a change needs a regenerated codec, a
port replay and a new release.

## Hardware video encoding and decoding

The host's VideoToolbox encoders and decoders reach the guest as V4L2 stateful
devices (Virtio video), and the product exposes them through Codec2. The vendor
service `android.hardware.media.c2-service-v4l2` of `external/v4l2_codec2`
serves `c2.v4l2.avc.encoder`, `c2.v4l2.hevc.encoder`, `c2.v4l2.avc.decoder` and
`c2.v4l2.hevc.decoder` beside the software `c2.android.*` components, and
MediaCodec selects them by name or by the platform's hardware preference. The patch series under
`patches/projects/p0018` adds the HEVC component and derives every encoder
parameter from the device rather than from constants: bitrate range and modes
(VBR, CBR, constant quality), QP bounds, B-frame depth, temporal layers,
profiles and levels, colour aspects (HLG too) and HDR10 static metadata. A capability the
device does not list is not advertised: Main 10 and HDR10 need a 10-bit raw
format, which the device publishes only when the host does. Packed RGB input
goes to the device unconverted when it lists that byte order and is converted
otherwise; a Main 10 session also takes 8-bit YUV and RGBA_1010102 input, which
the component converts to P010 by the stream's colour aspects. The device picks the level of a stream under the ceiling the client
asks for, so the component reports the level it reads from the first coded
headers.

The decoders take HEVC Main and Main 10 (with the High tier of every level the
device lists, which it lists by its Main tier) and H.264. They decode to the
raw format the client asks for: 8-bit YUV from a 10-bit stream unless the
client asks for P010, which the host converts, or renders to the input surface
of a video encoder, which reads all 10 bits. A client that decodes to byte
buffers is given pooled output blocks, and every output block is allocated with
CPU write usage so that the decoder can hand a picture to the host copy for a
consumer that reads it through the GPU; the device writes the guest pages of a
decoded picture, so minigbm never transfers a decoder's output from the host.
HDR content is shown as SDR: the host has no P010 sampler, so a P010 surface
cannot be shown correctly. A decoder reports its sessions and, once the
product states a decode pixel rate, its pixel rate through the same resource
interface as the encoders. The decoders keep the component's default rank, so
MediaCodec chooses them before `c2.android.avc.decoder` and
`c2.android.hevc.decoder` for a stream both take; a Main 10 stream has no other
decoder.

An encoder's input is a CPU-readable gralloc buffer (`COLOR_FormatYUV420Flexible`
as NV12, `COLOR_FormatYUVP010` as P010, or packed RGB): the host cannot sample a
YUV format natively, so minigbm allocates NV12, NV21, P010 and the planar 4:2:0
formats as bytes of an R8 resource, each plane a sub-image that a mapping
transfers on its own. P010 has the NV12 layout with two-byte samples, which is
what lets Main 10 and HDR10 input reach the device.

`overlay/config/media_codecs_c2.xml` is installed as
`vendor/etc/media_codecs_c2.xml` and adds the two encoders and the two
decoders, each with the performance points and measured frame rates CTS
requires of a hardware codec, identical width and height ranges, and (for a
decoder) the adaptive playback and dynamic colour aspects features it
implements. The numbers are what the codecs sustain through the guest-to-host
frame copy, which grows with the picture, so 4K HEVC encoding is published
below 30 frames per second; a decoder's lower bound is the rate that keeps up
with a 30 frames per second clip of every size at or below the point. The file
is the product's one vendor codec list, so it also carries any update
(`update="true"`) to the limits of a software decoder. The encoders start at
64x64 pixels and 20 kbit/s, the floors the host lists (the platform never lists a
video picture below 64 samples and the bit rate control starts at 20 kbit/s)
and the component clamps to: below them VideoToolbox's streams fail CTS
`VideoEncoderMinMaxTest`'s 20 dB PSNR bar, and the verifier requires the codec
list to state exactly these floors.
`ro.vendor.v4l2_codec2.encode_concurrent_instances` and
`ro.vendor.v4l2_codec2.decode_concurrent_instances` bound the instances of each
direction. The encoders allow eight. A decoder keeps its pool of decoded
pictures in the guest's memory, about 0.9 GiB for a 4K stream, so the guest's
8 GiB holds four: the decoders state four, which CTS creates at every
performance point's size without the low memory killer ending the process.
`ro.vendor.v4l2_codec2.video_pixel_rate` and
`ro.vendor.v4l2_codec2.video_decode_pixel_rate` are the pixel rates the host
sustains for encoding and decoding. The service reports them as the capacity
Android counts real-time instances against
(`MediaCodec.getGloballyAvailableResources()`), so no performance point may
exceed its direction's pixel rate and the largest must use at least 60% of it,
as CTS requires of the resources a point consumes; the target-files verifier
requires both. The service joins `camera`, which
`ueventd.rc` gives the `/dev/video*` nodes; its executable carries the
`mediacodec_exec` label; and `media_c2_extended_seccomp_policy` adds the calls
the V4L2 poller and the crash handler of debuggerd need beside the service's
default minijail policy, and `sepolicy/vendor/mediacodec.te` lets the service
map the message queue that a client's buffer pool shares in a memfd. The
target-files verifier requires each of these and refuses a hardware decoder
beyond the two (`VIDEO_DECODER_CODECS`).

Two CTS results are known deviations, because a published number must hold on
the hosts the product runs on (a busy Mac, an iPad) and not only on an idle one:

- **4K at 60 frames per second HEVC decoding.**
  `HevcVp9ClaimsPerformanceTest` at 3840x2160, 60 frames per second asks for a
  hardware point the host cannot sustain through the guest. The decoders publish
  4K at 49 to 55 frames per second. CTS's decoder performance test measures 62
  to 74 on an idle Mac and 50 to 56 on a busy one, and `VideoDecoderPerfTest`
  measures 53 to 56; publishing 60 made six of the HEVC 4K
  `CodecDecoderPerformanceTest` cases fail (50 to 56 against the 57 they then
  expect). The V4L2 path alone does about 250 frames per second at 4K and the
  host decoder 290, so the rest goes in the Android output path above V4L2 (the
  Codec2 output into a gralloc buffer and its consumer), which is a performance
  item of its own.
- **32-bit encode at the largest sizes.** On `armeabi-v7a`,
  `VideoEncoderDecoderTest#testPerf` at H.264 and HEVC 1080p and HEVC 4K reaches
  13.7 frames per second where the published point asks for 14.09: the test's
  own thread is pegged at 92 to 96% of a vCPU while the codec service is 90%
  idle, and the 64-bit run of the same case passes at 73 to 83%.

## Payload reduction

Payload reduction is done through product configuration and regenerated AOSP
patches, never by deleting files from a completed image. The product omits the
AuthSecret, reboot-escrow, AuthGraph, Secretkeeper, remote Identity Credential,
confirmation-UI, consumer-IR, USB, tether-offload, input-processor,
netlink-interceptor, and inactive Ranchu-composer HAL APEXes. None has
corresponding Darwin UML hardware. The active DRM composer, power and memtrack
integration, ClearKey DRM and CAS, dumpstate diagnostics, software
KeyMint/gatekeeper, and Credential Manager remain.

The thermal HAL APEX was first removed with these, and is installed again. It
is AOSP's example AIDL thermal service (`com.android.hardware.thermal`), which
reports fixed example temperatures and is what Cuttlefish ships, not a driver
for hardware the machine lacks. `system_server`'s thermal manager and the NDK thermal API need
a thermal HAL, and CTS `NativeThermalTest` fails with "HAL Ready: false"
without one. The module carries its own SELinux file contexts, and the platform
policy already has the `hal_thermal_default` domain and the service context.
The target-files verifier requires the vendor APEX to hold the service, the
VINTF fragment that declares `IThermal/default` and the init script.

The conditional-access (CAS) HAL APEX was removed with that group too and is
installed again. It is AOSP's default CAS service
(`com.android.hardware.cas`), which loads the ClearKey CAS plugin
(`libclearkeycasplugin`, installed for both ISAs under `/vendor/lib*/mediacas`)
and needs no device. Without it `MediaCas` has no `IMediaCasService`: CTS
`MediaCasTest` throws `UnsupportedCasException` for the ClearKey CAS system
id 63192. The vendor policy already has the `hal_cas_default` domain and the
platform has the service context. The target-files verifier requires the vendor
APEX to hold the service, the VINTF fragment that declares
`IMediaCasService/default` and the init script, and the plugin in both
`mediacas` directories.

The fixed image also omits CompOS and other AVF guest payloads, legacy VNDK
31--34 APEXes, OTA and DSU executables, the dynamic-system installer,
Cuttlefish host-monitoring services, process-memory guardian, and unused
vsock and tombstone forwarding helpers. These are excluded from the
resolved product package set before image construction. The build and
target-files verifiers reject any excluded module that reappears.

Android suspends itself like a phone when the screen is off, the battery is
unplugged and nothing holds a wakelock, and a phone stays awake while a USB host
is connected. This machine's adb host arrives over the network, which wakes a
suspend the guest started for only 500 ms per received frame (the
[mainline README](../../kernel/mainline-6.18/README.md) gives the kernel's
side), so a connected host would see the guest suspend again between its
exchanges. adbd's p0011 topic therefore sets
`sys.kernarium.adb.host_connected` to 1 while it has an adb host and to 0
when the last one leaves (and when it starts), and product init starts the
Cuttlefish `suspend_blocker`, a userspace wakelock, on 1 and stops it on 0. The
wakelock holds back only Android's own autosuspend: a suspend that the host
requests through the management console never goes through SystemSuspend, so it
still suspends the guest. adbd is a core domain, which the platform keeps from
setting a vendor-owned property, so the property is system-owned: its type,
label and the `set_prop` live in the overlay's system_ext sepolicy
(`overlay/sepolicy/system_ext/private`). Init parses the product's script as
`vendor_init` and drops a trigger on a property that domain may not read
("unexported property trigger"), and a `system_internal_prop` is closed to it, so
the type is a `system_public_prop` with `get_prop(vendor_init, ...)`.
`tools/check-overlay-sepolicy.py` keeps a core domain off the overlay's vendor
properties and a trigger off an unreadable property before any build, and the
target-files verifier repeats the platform neverallow on the built policy,
requires the service, both triggers, the property context and the rule, and
resolves every property that a trigger of the product script names to its
label and requires `vendor_init` to read it.

DeviceLock and Virtualization are retained with their boot and system-server
classpath fragments even though their hardware-backed functions are not
advertised. `SystemServiceRegistry` directly links their framework initializer
classes, so removing either APEX breaks Zygote and the platform SDK class
contract. Bluetooth, Wi-Fi, NFC, UWB, telephony, tethering, and RKPD modular
framework containers are retained for the same SDK/API reason. WebView, fonts,
ART and dexpreopt outputs, media codecs, SwiftShader, Mesa/virgl, Health
Connect, AdServices, and OnDevicePersonalization also remain.

Qualification starts with a fresh base expanded to 8 GiB and verifies direct
unencrypted ext4 userdata, absence of `/metadata/vold/metadata_encryption`, a truthful feature
inventory, VINTF and SELinux health, PIN/password changes, Android Keystore key
persistence across reboot, package installation, WebView, audio, networking,
and GLES. Only then are image size, resident processes, and cold and warm boot
time compared and the release accepted.

## Static disk and boot contract

The guest sees two disks, and a third would stall its boot: first-stage init
mounts a partition by name from any Android GPT disk it finds, so the host
attaches exactly these two.

- The **system disk**, `guest/system.img`, is read-only and ships with the
  release. One GPT holds `init_boot`, `recovery`, and the five EROFS partitions
  (`system`, `system_ext`, `product`, `vendor`, `odm`), each exactly its image
  rounded up to 4 KiB and packed back to back from LBA 40, the boot containers
  first and the filesystems largest first. The signed target files carry the
  five filesystems as Android sparse images; assembly expands them, so a
  partition's size and hash are those of the raw bytes the guest reads and the
  disk is raw. The two GPT tables are the only
  bytes that are not partition payload, and the bytes between a payload and
  its partition's end are zero. The disk is attached as `ubd0r`.
- The **data disk** is the guest instance's writable disk, a sparse GPT that
  the host creates (see [`darwin/device/block`](../../darwin/device/block/README.md)):
  `misc` (1 MiB at 1 MiB), `metadata` (16 MiB at 2 MiB), a reserved gap, and
  `userdata` from 64 MiB to the last MiB. The gap is room for a later layout
  to add partitions without moving userdata. The host writes no filesystem:
  the fstab's `formattable` entries make first-stage init format metadata and
  userdata on first boot. The disk is attached as `ubd1`, with discard.

`support/image/disk-policy.json` is the tracked policy: partition names,
image names, GUIDs, the packing rules, and the data disk template with its
capacity range (4 through 64 GiB, 8 by default). `tools/ubd-disk.py assemble`
turns the policy and the built images into `system.img` and the release's
`partition-layout.json` (schema 2), which records the system disk's size,
every partition's offset, size, image size and SHA-256, the SHA-256 of the
whole disk, and the data disk template. The host reads only that file, never
the policy, so a layout is a release property: changing a size or an offset
needs no host change. Partition names are unique across both disks.
`tools/ubd-disk.py verify --hash` checks a disk against its layout, optionally
against the tracked policy. The host does not hash at boot; the installer
verifies a release once.

The data disk only grows. The host raises its size and rewrites its GPT (the
`userdata` partition fills the disk); it never shrinks one. Normal Android
fills userdata on every boot, once `/data` is mounted and before the zygote
starts (`post-fs-data`): `resize2fs` asks the kernel's online resize to grow
the mounted filesystem to the partition, and does nothing when the host has not
enlarged it. The online resize is journaled and needs no `fsck`; an offline
`resize2fs` refuses a filesystem mounted since its last full check, and the
`e2fsck` that `mount_all` runs skips a clean one, so it would fill userdata on
the first boot only. The kernel adds the group descriptor blocks a larger
filesystem needs by converting it to `meta_bg`. `resize2fs` runs as root with
the group of `/data`'s owner, so it opens `/data` (mode 0771) without a DAC
capability, and in the `fsck` domain, which gets `sys_resource`, `open`,
`read` and `ioctl` on `system_data_root_file:dir` and the one ioctl
`EXT4_IOC_RESIZE_FS` (0x6610); the target-files verifier requires the command,
these rules, and no other ioctl for that domain on `/data`. Recovery does not
grow userdata: it mounts the filesystem at its current extent, and its factory
reset formats the whole partition. Neither guest rewrites or refreshes a live
partition table. There is no `super`, dynamic partition, Virtual A/B, AVB, OTA
slot, or DLKM partition. AOSP still builds its own metadata and userdata
images into the target files (the target-files verifier requires them and the
userdata inode count), but they are not shipped.

The image keeps `/system/bin/sgdisk`, which `vold` runs for every disk it scans
or partitions. The fstab has no `voldmanaged` entry, so `vold` never manages the
system disk; it reaches sgdisk only for the loop-backed virtual disk of
`sm set-virtual-disk`.

AOSP's EROFS tools (`mkfs.erofs`, `fsck.erofs`, `dump.erofs`) are built with
Zstandard. An `external/erofs-utils` topic (route `p0023`) compiles the encoder
and the decoder and embeds libzstd in `liberofs` itself (`whole_static_libs`,
because the `static_libs` of a static library are not linked into its users, so
`apex-ls` and the update engine's payload generator would otherwise miss the
symbols), and a second `external/zstd` topic (route `p0013`) lets that project
see the library. Without them `mkfs.erofs` rejects `-zzstd` and `fsck.erofs`
cannot read a Zstandard image.

The five read-only partitions are compressed EROFS: Zstandard level 15 with
16 KiB physical clusters on 4 KiB blocks. The two parameters are written once,
as `UML_ANDROID_EROFS_COMPRESSOR` and `UML_ANDROID_EROFS_PCLUSTER_SIZE` in
`overlay/lineage_andarium.mk`. `BoardConfig.mk` hands them to the image builder,
which otherwise uses `mkfs.erofs`'s defaults (no per-partition setting, no
extended option, no block size beyond the default), and `build/build-aosp.sh`
asserts that. The compress hints (`overlay/config/erofs_compress_hints.txt`)
store every `.apex` file uncompressed, so apexd's loop devices read it with
direct I/O and its pages are cached once. The target-files verifier reads the
parameters from the makefile and checks `misc_info`, the hints and the
superblock of each of the five images (4 KiB blocks, Zstandard alone, clusters
larger than a block, none of ztailpacking, fragments or dedupe), and that the
packaged kernel configuration has EROFS with Zstandard built in. Both fstabs
mount the partitions as `erofs ro`; the first-stage entries add
`wait,first_stage_mount`. The fixed GPT is unchanged: each image sits at the
start of its partition with zeros behind it, so the seed stays 3,123 MiB and
the compression shows in the images' own sizes. Decompression is the guest
kernel's native code, not emulated guest code. The five images hold 1,001 MiB
(system 524.5, system_ext 157.0, product 227.5, vendor 91.9, odm 0.3), of
which 257.5 MiB are the 52 `.apex` files. Against the ext4 release the
five-app cold launch sum is the same on a guest with 4 GiB (about 7.6 s
against 7.7 s) and about 20% longer on a quiet host with 1,536 MiB (13.9 s
against 11.6 s), where evicted pages are re-read and decompressed a whole
cluster at a time; that is why the clusters are 16 KiB and not 64 KiB.

Every APEX payload is compressed EROFS as well, with the same two parameters:
`PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE := erofs` and the three
`PRODUCT_DEFAULT_APEX_PAYLOAD_EROFS_*` settings in `overlay/lineage_andarium.mk`
reach apexer through Soong, not its lz4hc and 4 KiB defaults, and
`build/build-aosp.sh` asserts them. The `p0008` series no longer pins
`com.android.virt` to a payload of its own. The payload hints
(`overlay/config/erofs_apex_payload_hints.txt`) keep `.rc` and `.xml` files
and the adb daemon uncompressed, so the verifier still reads the example HALs'
service definitions and adbd's launch property names from the payload's bytes;
it finds each service binary through the payload's directories. The presigned CTS shim APEX is a prebuilt that keeps its
ext4 payload: the verifier allows exactly that one and rejects ext4 in any
other APEX. `tools/precompile-first-boot-apps.py` reads APKs out of an EROFS
payload with the host `fsck.erofs` (part of `otatools`), and out of an ext4 one
with `debugfs`.

Userdata is an ordinary unencrypted ext4 filesystem in the directly writable image; this
emulator does not expose a hardware-backed KeyMint service and the guest bundle
is already protected by the host application sandbox. The mount holds the
partition exclusively, so apexd cannot pin an updated APEX as a device-mapper
linear table over it (Early Activation, which exists to bypass dm-default-key);
the product sets `apexd.config.use_fiemap=false`, as GSI and DSU do, so staged
APEX installs go through `/data/app-staging`, init keeps two mount namespaces
(`ro.init.mnt_ns.count` 2), and the verifier requires the property. Both
userdata fstab
entries carry the fs_mgr `quota` flag, so the first mount runs
`tune2fs -Oquota -Qusrquota,grpquota,prjquota` (the seed filesystem's 256-byte
inodes allow project quota) and `installd` can answer StorageStatsManager from
the per-UID quota; the target-files verifier requires the flag. First-stage init mounts fixed GPT
partitions by name and does not use logical, slot-select, or module mount flags.
Writable metadata and userdata use continuous ext4 discard so deleted guest
extents are promptly returned to the sparse host file through UBD's batched
discard path.

`init_boot` and `recovery` contain kernelless boot-image-v4 ramdisks. The
kernel ELF remains outside the disk in the selected runtime artifact. Both images come
from the signed AOSP target files; `recovery` is the stock LineageOS recovery
that `lineage_andarium` builds, with no separate recovery source or build.
Neither ramdisk is duplicated as a standalone release file. Boot mode `auto`
follows the standard command in the directly writable `misc` partition and is
the launcher default; explicit normal/recovery selection is a diagnostic
override of the first boot only.

A guest reboot restarts the guest in the same host process, so `adb reboot`,
`adb reboot recovery` and `adb reboot bootloader` all return to a running
guest, and the launcher keeps waiting on the one process. Every restart reads
`misc` again and, in `auto` mode, boots the ramdisk it names: Android's
`boot-recovery` command boots recovery, which clears it when it leaves, and no
command boots Android. There is no bootloader to stop in, so the launcher
behaves as a bootloader that continues: a `bootonce-bootloader` command is
cleared, with a log line, and the guest boots on. It is cleared because a
command left in `misc` makes Android's init skip writing `boot-recovery`, and
the guest would then reboot into Android instead of recovery. A guest that
restarts within 15 s of booting five times in a row ends the launch with exit
status 3.

The recovery ramdisk is an LZ4 legacy frame of the product's page-agnostic
AArch64 ELFs, so it runs on the 4 KiB kernel unchanged. Two product changes
adapt the stock recovery to the virtual hardware:

- `overlay/recovery/` builds `librecovery_ui_andarium`, selected by
  `TARGET_RECOVERY_UI_LIB`. The stock Cuttlefish UI library addresses `eth1`;
  this one addresses the Virtio-net `eth0`, assigns the SLiRP address
  `10.0.2.15/24` itself (init cannot run `ifconfig` from recovery's SELinux
  domain), keeps Cuttlefish's always-connected USB policy, and removes the
  menu items for the bootloader, fastboot, and an ADB toggle that have no
  meaning here.
- The `bootable/recovery` route `p0016` lets the UI read the virtual
  touchscreen, a direct single-touch device (`BTN_TOUCH` with `ABS_X` and
  `ABS_Y`) that the stock UI, which reads only multi-touch axes, ignores.

Recovery shows the DRM display, takes VNC keys and touch, serves ADB over the
same loopback forward as normal Android (`adb sideload` checks packages against
the signing keys in the image's `otacerts`), and mounts the fixed GPT
partitions from the checked `overlay/config/recovery.fstab`. Recovery ADB
authentication follows the product's `ro.boot.kernarium.adb_auth` policy.
Recovery loads only its ramdisk's `prop.default`, which the build fills from the
product's properties, so `sys.use_memfd=true` reaches it and its ashmem users
fall back to memfd on a kernel without `/dev/ashmem`. The verifier requires
that line.

Recovery draws into a DRM dumb buffer, and the Virtio-GPU driver always creates
that buffer as `B8G8R8X8` whatever format the framebuffer declares afterwards.
`TARGET_RECOVERY_PIXEL_FORMAT := BGRX_8888` makes minui declare `XRGB8888`, the
buffer's own byte order, so every pixel appears exactly as stored. The
Cuttlefish default `ABGR_8888` declares `RGBA8888` for bytes nothing converts
and swaps red and blue; the kernel patch that offers the extra scanout formats
is meant for Android's composer, which scans out blobs that carry their own
format. The verifier requires `ro.minui.pixel_format=BGRX_8888` in the
ramdisk's `prop.default`. The correction is guest-side, so the iOS display path
(the same scanout code) needs none.

The stock LineageOS recovery image measures 15.07 MiB (15,802,368 bytes). Its
`BOARD_RECOVERYIMAGE_PARTITION_SIZE` of 20 MiB is the image builder's limit, 25%
headroom rounded up to a 4 MiB multiple, and the build fails on an image that
does not fit; the system disk packs the recovery image at its own size.

`tools/verify-lineage-recovery.py` checks the image the assembler installs: a
kernel-less v4 image whose LZ4 or gzip ramdisk holds only target ELFs aligned
to 16 KiB or better, a closed dependency set, no loadable modules, the checked
fstab, the `eth0` interface policy, and the checked `init.recovery.rc` as the
ramdisk's `init.recovery.cutf_cvm.rc`, the script that publishes the display
density before Recovery draws its UI (the inherited Cuttlefish script is kept
as `init.recovery.cuttlefish.rc`, which it imports). `build/assemble-image.sh`
owns `init_boot` ramdisk composition, system disk assembly, and verification.
It refuses unsafe paths, partition overflow, module payloads, or replacement of
an existing output.

## Release installation

`support/release/release-manifest.schema.json` defines the compact rootfs
release. It binds the system disk and its layout, boot ranges, command line,
target-file support, and source provenance. The Android section records the
security patch level (`ro.build.version.security_patch`) and the build number
(`ro.build.version.incremental`), which a release must set numerically
(`build-aosp.sh --build-number N`): the number is both the user-visible release
number and the monotonic key by which the host refuses a downgrade. It also
records the ABIs the image enables, in the image's own order
(`android.abi_list`, from `ro.product.cpu.abilist`), which Andarium's Info
sheet lists. The key is optional for validation: a release published before it
existed has none, and the host then shows no list. The checked
`metadata/accepted-release.env` selects one reviewed release without recording
a builder path.

After copying a reviewed candidate to the workstation, install it with:

```sh
android/lineage-24/tools/install-release.py \
  --source /absolute/local/path/to/release
```

The installer validates the candidate, including every system disk partition
hash, and installs it atomically below `.build/android/releases`. It never
replaces an existing release. Apple CMake does not import the rootfs release
and does not invoke Repo, Soong, Kbuild, image assembly, publication, or
transport.

The release manifest retains the kernel build that qualified its target files,
but that value is historical provenance. Apple runtime selection uses an
independent compatible Android kernel artifact and performs no rootfs, kernel
commit, or emulator release comparison at launch. A guest's data disk remains
usable across compatible kernel updates within the `6.18` line. A kernel-only
update therefore advances only
[`accepted-android-4k.env`](../../kernel/mainline-6.18/artifacts/accepted-android-4k.env)
and keeps the accepted release, which then runs a later kernel than the one it
records.

### Release signing

Every `user` build is signed with one private key set that never enters the
repository, a log or a release; there is no unsigned publication and no key
generated per build. A `userdebug` build keeps the AOSP test keys the build
uses, is not re-signed and is never published. `tools/publish-release.py
--signing-keys DIR` takes the unsigned target files and `DIR`, a directory
owner-only (no group or other access) that the caller deletes; the tool copies
no key, and it refuses to sign any other variant. It links `DIR` as
`keys/` in a private work directory, signs with the AOSP
`sign_target_files_apks` from there, assembles the system disk from the signed
archive, and deletes the work directory and checks the deletion whether or not
signing succeeded. The signer names the certificate in each `otacerts.zip`
after the path it is given, so it runs in the work directory with `keys` as its
default key directory: the member is `keys/releasekey.x509.pem` in every
release, with no build-host path or temporary name, and the tool refuses any
other member, and `verify-target-files.py --signed` makes the same check on
the signed archive. An unsigned build of either variant holds the AOSP test
certificate's file name, `testkey.x509.pem`, in both archives, and the verifier
requires exactly that of every archive not marked `--signed`
(`release_common.OTA_CERTIFICATE_MEMBERS`). `--disk-dir` then names
the new directory the signed `system.img` and `partition-layout.json` are
written to, and `--mkbootfs`, `--mkbootimg` and `--first-boot-artifacts` are
required; the last names the first-boot output of
`tools/precompile-first-boot-apps.py`, and publishing runs that tool's `verify`
on the signed tree, so re-signing cannot leave first boot compiling. The set
holds the platform keys (`releasekey`, `platform`, `media`, `shared`,
`networkstack` and the others the tree names) as `.pk8` and `.x509.pem`, and
one 4096-bit key per APEX container with its `.pem` payload key. Signing fails
when any file stays signed with a key outside `DIR`, so no AOSP public test
key survives. Signing also replaces the OTA certificates, and the tool then
requires the system's and the recovery's `otacerts.zip` to trust the release
certificate and no public AOSP or LineageOS certificate; the product clears
`PRODUCT_EXTRA_RECOVERY_KEYS`, which would add LineageOS's release certificate
to the recovery. A presigned file keeps its upstream signature and is named in
the build receipt (`none` when there is none), which also records the SHA-256
of each platform certificate. The AOSP host tools need `sign_target_files_apks`
(built with `otatools`). The key directory, its backup and the transfer to the
builder are the maintainer's responsibility.

### Superseded releases

After a replacement release is validated, remove the superseded Android build,
release, transfer, and guest-bundle artifacts from the local `.build` tree,
local temporary directories, and the Android build machine. Resolve releases
and bundles from their manifests and build outputs from their recorded
invocation and canonical output root, and delete only explicit Android paths.
Preserve the current accepted release and its active writable bundle, the
Android source and patch-producer checkouts, accepted kernel inputs, dependency
prefixes, non-Android guests, and unrelated files beside a bundle.

The second architecture adds a variant to nearly every module, so the first
multilib release is built in a fresh output directory, never over the 64-bit-only
one; that output is a superseded build once the multilib release is accepted.

## Storage accounting

The framework reports exact filesystem bytes for the static virtual disks.
`StorageManager`'s two primary-storage helpers sum `/data` and the five mounted
OS filesystems (`/system`, `/system_ext`, `/product`, `/vendor` and `/odm`).
`StorageStatsManager` uses that total and keeps its filesystem-available plus
reclaimable-cache calculation for free space. Settings derives its used space
and System category from those consistent byte counts. The `frameworks/base`
storage topic in route `p0007` owns this policy; the framework fix leaves Settings'
display calculations unchanged.

Retail capacity rounding does not apply to these virtual disks: it can report
less than the data filesystem contains, making the System category negative,
or inflate the total to the next advertised capacity. The configured sparse
disk capacity is independent of the Apple host's currently available storage.
Settings still imposes an upstream 1 GiB minimum on Temporary system files, so its
category sizes can sum to more than the displayed total used; exact filesystem
accounting does not remove that separate Settings calculation.

## Guest instance storage

A guest instance is the writable half of a guest: a directory holding the data
disk, `data.img` (the creation and growth rules are in
[`darwin/device/block`](../../darwin/device/block/README.md)), and
`instance.json`, which records the release the disk was last started with. The
system disk is not part of an instance. Every launcher that keeps instances,
the macOS script and the Andarium app, applies the same rules before each
start; [`tools/android_instance.py`](../../tools/android_instance.py) is the
reference implementation and
[`support/instance/update-decisions.json`](support/instance/update-decisions.json)
holds decision vectors and the record's transitions that any other
implementation must reproduce.

`instance.json` (schema 1) holds: `data_layout`, the data disk layout version of
the release's template; `release_id`, `build_fingerprint`, `build_incremental`
(the release's build number, as text) and `system_disk_sha256`, the release the
data disk was last started with; `created_with`, the release that created the
instance; `last_booted`, the time of the last start (RFC 3339, UTC);
`last_completed`, the last start that reached `sys.boot_completed` and shut down
cleanly (`release_id`, `build_incremental`, `at`) or `null`; and
`pending_update`, the update whose first start has not reached
`sys.boot_completed` or `null`: the release it starts (`to_release_id`), its
`snapshot` file, and the identity the disk was last started with before it
(`from`). The release's identity comes from its manifest and layout, never from
hashing the disk at launch.

The build number (`ro.build.version.incremental` of a release build) orders
releases; the rules compare nothing else. Before a start:

| Case | Action |
| --- | --- |
| no instance | create it; the host creates the data disk, which the guest formats on first boot |
| same release | start |
| newer build, same `data_layout` | snapshot, record the pending update, start; Android upgrades its own data |
| newer build, newer `data_layout` | snapshot, run the registered migration (none exists yet), start |
| older build | refuse: Android's key store, package manager and apexd do not go back |
| same build, another release or system disk, or a `data.img` without `instance.json` | refuse |
| an unfinished update, same build | retry with its snapshot kept, restore, or reset |
| an unfinished update, newer build | restore the pre-update snapshot, then update directly to the newer build |

An update is finished at the moment the host first sees the guest reach
`sys.boot_completed` (`booted`): `pending_update` becomes `null` and the
snapshot is deleted, so an unfinished update is one whose start never got that
far. Android has upgraded its own data by then, and the data is no longer what
the update started from. A newer release after that updates from the data as it
is, with a snapshot of its own, and nothing restores the old snapshot; the
shutdown that follows (`last_completed`) does not change this.

Any older build can update directly to any newer build; intermediate releases
need not boot. When an earlier update is unfinished, the launcher restores its
pre-update data before starting the newer release, which takes its own snapshot.
The existing snapshot must be available for that restoration.

The snapshot, `data.img.pre-<release_id>`, is an APFS clone, so it is instant
and costs only the blocks that later diverge; a volume without clones refuses
the update. It lives only while the update is unfinished, and the old system
disk is gone by then, so a restore puts back the data the new release started
from; it does not boot the previous release. An update whose first start fails
offers three ways out: start it again keeping the snapshot, restore the
snapshot and update afresh, or reset the instance (delete it and create it
anew), which is also the way out of a refused downgrade. A future data layout
changes the partition table only inside the reserved gap, never moving
`userdata`. The instance directory is one directory, so a backup of it is a
copy of it; the app, not the host layer, decides whether to exclude it from
backup.

## Apple consumers and guest bundles

The macOS Android product is configured with the `macos-android17` preset. Its
build requires a compatible Android kernel artifact; launch takes any rootfs
directory with the expected payload layout and one directly writable GPT image.
It does not alter the normal Linux executable or search for `rootfs.erofs`.

[`../../tools/boot_android.py`](../../tools/boot_android.py) is the ordinary
macOS launcher. It selects the accepted installed release and a guest instance
(by default `.build/android/instances/android-17`, see
[Guest instance storage](#guest-instance-storage)), applies the update rules to
it, and starts the emulator on the release's system disk and the instance's
data disk. A new instance has an 8 GiB data disk, `--data-gib` raises it, and a
disk never shrinks. The launcher exposes guest ADB only on host loopback, waits
for the transport and for `sys.boot_completed`, and disconnects ADB when the
guest stops. The default session uses
the existing `macos-android17` executable with native Metal display, 10 vCPUs,
8 GiB of RAM, the gadgets engine, and a 720-by-1280 portrait display. Pass
`--build` to refresh that executable incrementally or `--recovery` to boot
recovery for the first boot; run `tools/boot_android.py --help` for other
overrides. Pass `--audio-output null` to discard guest audio at the rate it
would play, for a run that must make no sound. The guest records silence unless
`--audio-input host` records from the Mac's microphone, which makes macOS ask
the app that runs the script for access (see the
[audio device README](../../darwin/device/audio/README.md#microphone)).

Kernarium represents one Android guest as a `.kern` directory containing:

- `release-manifest.json`;
- `boot/kernel-command-line.txt`;
- `guest/system.img`, the release's read-only system disk, and
  `guest/partition-layout.json`, which describes it; and
- `guest/data.img`, the guest's writable data disk, which the runtime creates at
  the first launch and only grows afterwards.

The kernel is supplied by the runtime framework and is not bundle payload. The
manifest is provenance rather than a runtime compatibility token. Missing files
and malformed disk images fail when opened or parsed. Android appends the
host-owned display density, ADB authorization policy, ADB host key, ADB TCP
policy, and tagged-pointer option at launch. Launches write persistent guest changes only to `guest/data.img`.

How Kernarium keeps, relaunches, and copies a bundle is in
[`../../Kernarium/README.md`](../../Kernarium/README.md#guest-storage).

Andarium carries the accepted release instead: `Andarium/scripts/stage-system-image.py`
checks it against its own manifest (accepted, 4 KiB pages, a LineageOS family,
the accepted release identity, file sizes, and the system disk's size and
sha256 against the partition layout) and stages the system disk, the partition
layout, the kernel command line and the manifest, at their release paths, for
the application to bundle. See [`../../Andarium/README.md`](../../Andarium/README.md#build).

## Validation boundary

Producer verification covers pinned source/patch identity, target files,
VINTF, SELinux policy, executable alignment, module exclusion, partition
layout, boot ramdisks, and release inventory. Runtime boot, persistence,
recovery, graphics, input, networking, connected-device memory limits, and
application compatibility are separate Apple-product gates.

Historical boot, ART, memory, and topology experiments are summarized in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
