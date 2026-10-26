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
builds, verification, image assembly, and publication are offline.

The producer has three independent outputs:

1. the `lineage_darwin` target files of the configured build variant, which
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

`overlay/` owns the complete `device/kernarium/darwin` product tree and is
synchronized as one verified unit. The Vulkan HAL's Mesa source has its own
pinned source and generated series under `third_party/mesa-venus/`.

`third_party/fdroid/` pins the F-Droid client and its Privileged Extension by
size and SHA-256. Both are upstream-presigned APKs the build installs
unchanged: the client as an ordinary product app and the extension in
`priv-app` with a permission allowlist of only `INSTALL_PACKAGES` and
`DELETE_PACKAGES`. `tools/fdroid_source.py` materializes them into the
workspace during source preparation (the only networked step, refusing any
size or digest difference) and the build verifies the result. Their detached
`.asc` signatures verify only against the 2018 F-Droid release key, which a
current keyring may list as expired, so the pinned digests and the APK
signing-certificate pin in `metadata.env` are the checked identity.

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
`arm64-v8a` and `armeabi-v7a`, in that order. The 32-bit side is ARMv8-A
AArch32 on a cortex-a53, built as the softfp `armeabi-v7a` ABI; the image lists
no `armeabi`, and `ro.zygote` is `zygote64_32`. Every daemon and HAL stays
64-bit (`IGNORE_PREFER32_ON_DEVICE`), so 32-bit code runs only in applications,
the two zygotes and the WebView RELRO process. Recovery, `init_boot` and the
vendor ramdisk are 64-bit only. The kernel runs 4 KiB pages, because Android's
32-bit libraries are 4 KiB aligned; the 64-bit userspace stays page-agnostic (a
16 KiB maximum ELF alignment and no compile-time bionic page-size macro), so it
is valid on a kernel of either size, and `ro.product.page_size` reports 4096.

A `device/google/cuttlefish` topic (route `p0001`) adds the
`UML_ANDROID_MULTILIB` switch, which makes the page-agnostic phone inherit
`core_64_bit.mk` instead of `core_64_bit_only.mk`; `overlay/lineage_darwin.mk`
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
talks to a 64-bit renderer. The same project's route also carries a LineageOS
correction: its WebView service drops the configured providers that are not
installed, asking the package manager for every user, and does so under the
identity of the calling application, which the package manager refuses without
`INTERACT_ACROSS_USERS`. The topic runs that availability check as the system,
so any application reads the provider list, as on AOSP.

The build variant is the single `LINEAGE24_BUILD_VARIANT` setting in
`metadata/lineage.env`, `user` or `userdebug`. Lunch, the target-files verifier,
the release manifest and the accepted-release descriptor all follow it, so a
release is accepted only as the variant the tracked tree builds. A `user` build
keeps the stock shell privilege boundary; a `userdebug` build is the
development variant (see the ADB paragraph below).

The product removes Cuttlefish virtual hardware and services that have no
Darwin UML implementation while preserving Android framework APIs required by
ordinary applications.

The product is `lineage_darwin`: the Cuttlefish base with LineageOS's
`common_full_tablet_wifionly` product chain on top, so the tablet configuration
and the LineageOS platform services (LineageParts, LineageSettingsProvider and
the hardware, live-display, trust and global-actions services) are present.
`overlay/lineage_darwin.mk` sets the LineageOS switches before inheriting it
and names what the product leaves out. The image provides Settings, Trebuchet,
Files, Package Installer, the LineageOS WebView, LatinIME, ThemePicker, the
patched Jelly browser and a curated application set: Glimpse (gallery), Twelve
(music), Etar (calendar), ExactCalculator, Camelot (PDF viewer) and Seedvault
(backup), plus the F-Droid client and Privileged Extension. It has no Google
or microG services. The LineageOS updater and setup
wizard, Canvas, Aperture, Profiles, Recorder, QuickAccessWallet, the charger
animation,
fastbootd and AudioFX are excluded, as are DeskClock and EmergencyInfo; the
build and verifier assert both lists. QuickSearchBox and its home-screen
search surface are deliberately omitted. Bluetooth and Wi-Fi
device implementations and feature declarations are also omitted. The Network
dashboard exposes the stock Ethernet interface editor alongside Data Saver,
VPN, and Private DNS. On this radio-free product, the editor enables AOSP's
full Ethernet path and hides its inapplicable Wi-Fi groups. The radio-only
Connected devices entry is hidden, as is System update because the synced AOSP
tree has no activity implementing that intent. The modular framework API
containers remain part of base AOSP so ordinary applications can still resolve
the Android SDK classes; excluding them also removes required system-server
dexpreopt inputs. Telephony,
GNSS, NFC, UWB, Thread, camera, biometric, sensor, vibrator, secure-element,
USB, and Trusty hardware is not advertised. Ethernet uses the real
Virtio-net/SLiRP path.

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
declaration and exactly this input. Settings omits its Bluetooth media-output
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

Fresh settings databases also enable Developer options and ADB. The radio-free
product starts adbd on TCP port 5555 and retains the stock authorization prompt
for a newly connected host. The immutable `ro.boot.kernarium.adb_auth` launch
property can disable that check for host-side diagnostics: iOS fixes it to one,
while the standalone macOS product fixes it to zero. Bundle command lines may
not override this host security choice. LineageOS makes its debug variant
non-debuggable and always authorized; the product undoes the first
(`PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG` is empty) and the boot policy decides
the second. Under `userdebug` the build is debuggable and carries `su`, and
`adb root` goes through LineageOS's `adb_root` service, whose "Rooted
debugging" switch is off on a fresh data partition. A boot with authorization
off starts that service with the switch on, so on macOS, which authorizes
nothing, any local process that reaches the loopback ADB port can become root
in the guest, an accepted property of a development host. iOS keeps
authorization and the default-off switch.
SELinux stays enforcing in both variants; only the host-owned kernel command
line could change that. Android's wireless-debugging setting
uses the active Virtio-net Ethernet interface as its trusted-network identity,
monitors that transport for reconnect and disconnect, and publishes its mDNS
services without requiring a Wi-Fi manager or multicast lock. Fresh
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

The guest has no hardware video decoder, and the software decoders of the
swcodec APEX advertise less than they decode: VP9 up to 2048x2048 and HEVC at
2,000,000 8x8 blocks per second (1080p at 60 frames per second). Both decode
3840x2160, which the Mainline media CTS expects some component of every device
to accept. `overlay/config/media_codecs_c2.xml` updates the two entries (the
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
repeats the checks on the built policy (only the userdebug `su` may be
permissive).

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
list that drops one of them also fails the `lineage24-builder-lane` fixtures.

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
three groups without one: the applications
packaged inside updatable APEXes, which it never dexpreopts, the presigned
F-Droid client and Privileged Extension, whose prebuilt modules opt out, and
the second instruction set of any package, since Soong dexpreopts an
application for the first only.
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
`LINEAGE24_FIRST_BOOT_OAT_MK`, which `overlay/lineage_darwin.mk` includes, and
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
loads no vendor init script and stays expedited for its short run.

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

`third_party/mesa-venus/` pins that source as three independent inputs: the
upstream Mesa revision, the guest Vulkan series under
[`../../tools/rootfs/vulkan/`](../../tools/rootfs/vulkan/README.md), and the
Android port series beside the metadata. The port series carries the generated
Soong port and the few source changes only Android needs, such as advertising
`EGL_EXT_pixel_format_float` only with a float config: HWUI takes the extension
as the promise of a half-float window config, and the Android visuals are all
fixed-point. A guest replay assigns new
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

Presentation is `VK_ANDROID_native_buffer`: the platform loader allocates the
swapchain images from gralloc and the driver imports them. Those blobs belong to
the renderer's GL context, and an Apple host has no dma-buf to hand across, so
the renderer gives such a resource a host shared-memory segment and exports
that; the contract is in
[`../../docs/architecture.md`](../../docs/architecture.md). SurfaceFlinger then
takes the presented layer on the scanout as a device composition. The product
ships `kernarium_vk_present`, which owns a layer, wraps it through
`VK_KHR_android_surface` and presents a swapchain, as the pass-or-fail check of
that path.

The product declares `android.hardware.vulkan.version` 1.4, `level` 1 and
`compute` 0, and deliberately declares no `android.software.vulkan.deqp.level`:
this driver has not been qualified against Android's Vulkan dEQP must-pass list.

The Android hardware buffer handle type a swapchain needs is guest Mesa work,
so it arrives with the guest Vulkan epoch pinned in
`third_party/mesa-venus/metadata.env`. The HAL and the host renderer must also
speak the same Venus protocol revision: a change to the protocol series or the
Vulkan-Headers pin leaves an older release's HAL unable to talk to a newer
renderer. `tools/verify-mesa-venus-source.py` therefore refuses a protocol or
registry pin that differs from its owner, and a port codec whose first line
does not name the recorded pins; such a change needs a regenerated codec, a
port replay and a new release.

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
is connected. This machine's adb host arrives over the network, which cannot
wake a suspended guest, so the host's connection would vanish for the minute or
more until the next alarm. adbd's p0011 topic therefore sets
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

`support/image/partition-layout.json` owns a compact 3,123 MiB seed GPT with
separate Android payload, metadata, `init_boot`, `misc`, recovery, and a
128 MiB userdata filesystem. The payload partitions hold both userspaces:
system is 1,600 MiB, system_ext 512, product 576 and vendor 256. Userdata is
the final partition. Before its first
boot, the Apple app's generic Swift `DiskUtil` expands the bundle-local raw
file to the selected whole-GiB capacity from 6 through 64 GiB, which leaves
userdata about 3 GiB at the smallest, and uses the
statically linked GPT fdisk core to relocate the backup GPT and extend
userdata. The standalone macOS wrapper performs the same transactional
host-side operation with the system `truncate` utility and GPT fdisk's
`sgdisk`, found on `PATH` or named with `--sgdisk`. Runtime launch has no
disk-capacity parameter and rejects a GPT that does not match the file. The
resized image is opened directly as writable storage; Android launch never
constructs or accepts a UBD COW pair. Normal Android asks the ext4
`resize2fs` tool to fill userdata before mounting `/data`; the tool is
idempotent when the host has not enlarged the partition. The image carries
8,192 inodes (`BOARD_USERDATAIMAGE_EXTFS_INODE_COUNT`, which a `build/make`
topic, route `p0002`, forwards to the image builder as it does for the
read-only partitions), one per 16 KiB, ext4's default ratio. `resize2fs` keeps
the inodes per group, so mke2fs's own choice for so small a filesystem, one per
4 KiB, would leave the grown `/data` with more inodes than blocks, which
Android's storage tests reject; the verifier requires the count. Recovery does
not grow userdata: it mounts the filesystem at its current extent, and its
factory reset formats the whole partition. Neither guest rewrites or refreshes
a live partition table. There is no `super`,
dynamic partition, Virtual A/B, AVB, OTA slot, or DLKM partition.

The image keeps `/system/bin/sgdisk`, which `vold` runs for every disk it scans
or partitions. The fstab has no `voldmanaged` entry, so `vold` never manages the
system disk; it reaches sgdisk only for the loop-backed virtual disk of
`sm set-virtual-disk`.

The immutable Android payload filesystems and APEX payloads are ext4. Userdata
is an ordinary unencrypted ext4 filesystem in the directly writable image; this
emulator does not expose a hardware-backed KeyMint service and the guest bundle
is already protected by the host application sandbox. Both userdata fstab
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
that `lineage_darwin` builds, with no separate recovery source or build.
Neither ramdisk is duplicated as a standalone release file. Boot mode `auto`
follows the standard command in the directly writable `misc` partition;
explicit normal/recovery selection is diagnostic.

The recovery ramdisk is an LZ4 legacy frame of the product's page-agnostic
AArch64 ELFs, so it runs on the 4 KiB kernel unchanged. Two product changes
adapt the stock recovery to the virtual hardware:

- `overlay/recovery/` builds `librecovery_ui_darwin`, selected by
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

The recovery partition is 20 MiB. The stock LineageOS recovery image measures
15.07 MiB (15,802,368 bytes), and the size rule is that image plus 25%
headroom, rounded up to a 4 MiB multiple: 18.8 MiB becomes 20 MiB, which leaves
4.9 MiB (32%) of the partition free. Like every partition it starts on the
layout's 1 MiB alignment, and `BOARD_RECOVERYIMAGE_PARTITION_SIZE` must equal
the layout, so the build fails on an image that does not fit. Resize the
partition only when the image outgrows 80% of it, and a resize moves
userdata, so it changes the seed GPT that the Apple runtime validates.

`tools/verify-lineage-recovery.py` checks the image the assembler installs: a
kernel-less v4 image whose LZ4 or gzip ramdisk holds only target ELFs aligned
to 16 KiB or better, a closed dependency set, no loadable modules, the checked
fstab, and the `eth0` interface policy. `build/assemble-image.sh` owns
`init_boot` ramdisk composition, disk assembly, and verification. It refuses
unsafe paths, partition overflow, module payloads, or replacement of an
existing output.

## Release installation

`support/release/release-manifest.schema.json` defines the compact rootfs
release. It binds the fixed GPT, boot ranges, command line, layout, target-file
support, and source provenance. The checked
`metadata/accepted-release.env` selects one reviewed release without recording
a builder path.

After copying a reviewed candidate to the workstation, install it with:

```sh
android/lineage-24/tools/install-release.py \
  --source /absolute/local/path/to/release
```

The installer validates the candidate, expands the compressed GPT as a sparse
local file, revalidates the staged tree, and installs it atomically below
`.build/android/releases`. It never replaces an existing release. Apple CMake
does not import the rootfs release and does not invoke Repo, Soong, Kbuild,
image assembly, publication, or transport.

The release manifest retains the kernel build that qualified its target files,
but that value is historical provenance. Apple runtime selection uses an
independent compatible Android kernel artifact and performs no rootfs, kernel
commit, or emulator release comparison at launch. A bundle's expanded disk
remains usable across compatible kernel updates within the `6.18` line. A
kernel-only update therefore advances only
[`accepted-android-4k.env`](../../kernel/mainline-6.18/artifacts/accepted-android-4k.env)
and keeps the accepted release, which then runs a later kernel than the one it
records.

### Release signing

A release is signed with a private key set that never enters the repository, a
log or a release. `tools/publish-release.py --signing-keys DIR` takes the
unsigned target files, copies `DIR` (owner-only, no group or other access) to a
private temporary directory, signs with the AOSP `sign_target_files_apks`,
assembles the disk from the signed archive, and deletes the copy and checks the
deletion whether or not signing succeeded. `--disk` then names the new signed
disk, and `--mkbootfs`, `--mkbootimg` and `--first-boot-artifacts` are
required; the last names the first-boot output of
`tools/precompile-first-boot-apps.py`, and publishing runs that tool's `verify`
on the signed tree, so re-signing cannot leave first boot compiling. The set
holds the platform keys (`releasekey`, `platform`, `media`, `shared`,
`networkstack` and the others the tree names) as `.pk8` and `.x509.pem`, and
one 4096-bit key per APEX container with its `.pem` payload key. Signing fails
when any file stays signed with a key outside the copy, so no AOSP public test
key survives. Signing also replaces the OTA certificates, and the tool then
requires the system's and the recovery's `otacerts.zip` to trust the release
certificate and no public AOSP or LineageOS certificate; the product clears
`PRODUCT_EXTRA_RECOVERY_KEYS`, which would add LineageOS's release certificate
to the recovery. The presigned F-Droid APKs keep their upstream signature and
are named in the build receipt, which also records the SHA-256 of each platform
certificate. The AOSP host tools need `sign_target_files_apks` (built with
`otatools`). The key directory, its backup and the transfer to the builder are
the maintainer's responsibility.

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

## Apple consumers and guest bundles

The macOS Android product is configured with the `macos-android17` preset. Its
build requires a compatible Android kernel artifact; launch takes any rootfs
directory with the expected payload layout and one directly writable GPT image.
It does not alter the normal Linux executable or search for `rootfs.erofs`.

[`../../tools/boot_android.py`](../../tools/boot_android.py) is the ordinary
macOS launcher. It selects the accepted installed release, copies its seed GPT
to a persistent writable image on first boot, expands that copy to 8 GiB,
exposes guest ADB only on host loopback, waits for the transport, and disconnects
it when the guest stops. Later boots reuse the same image directly. When the
selected release identity changes, the launcher replaces the persistent image
with a newly expanded copy of that release. The default session uses
the existing `macos-android17` executable with native Metal display, 10 vCPUs,
8 GiB of RAM, the gadgets engine, and a 720-by-1280 portrait display. Pass
`--build` to refresh that executable incrementally or `--recovery` to boot
recovery; run `tools/boot_android.py --help` for other overrides.

Kernarium represents one Android guest as a `.kern` directory containing:

- `release-manifest.json`;
- `boot/kernel-command-line.txt`;
- exactly one directly writable `guest/*.img`.

The kernel is supplied by the runtime framework and is not bundle payload. The
manifest is provenance rather than a runtime compatibility token, and legacy
bundles without it remain selectable. Missing files and malformed disk images
fail when opened or parsed. Android appends the host-owned display density,
ADB authorization policy, and tagged-pointer option at launch. Launches write
persistent guest changes directly into the image inside the `.kern` bundle.

How Kernarium keeps, relaunches, and copies a bundle is in
[`../../Kernarium/README.md`](../../Kernarium/README.md#guest-storage).

## Validation boundary

Producer verification covers pinned source/patch identity, target files,
VINTF, SELinux policy, executable alignment, module exclusion, partition
layout, boot ramdisks, and release inventory. Runtime boot, persistence,
recovery, graphics, input, networking, connected-device memory limits, and
application compatibility are separate Apple-product gates.

Historical boot, ART, memory, and topology experiments are summarized in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
