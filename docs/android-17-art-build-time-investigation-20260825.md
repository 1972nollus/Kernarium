# Android 17 build-time ART investigation (2026-08-25)

## Result

The current Android product already enables AOSP dexpreopt, but inherited
`system_other` policy prevents most ordinary application artifacts from being
installed in any image that this product publishes. The minimal local fix is
to clear `BOARD_USES_SYSTEM_OTHER_ODEX` in the project-owned device
`BoardConfig.mk`, keep `PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false`, rebuild
incrementally with the existing Ninja output, and require the resulting
`odex`/`vdex` files beside their APKs in `system`, `system_ext`, and `product`.

This should move all 26 observed first-boot verification jobs for ordinary
partition APKs from the guest to the x86_64 builder. It will not, by itself,
move the ten observed jobs for APKs contained in updatable APEXes: pinned AOSP
17 deliberately disables dexpreopt for non-system-server APEX variants. That
is a separate, materially larger problem and is not part of the first fix.

No AOSP source, product file, build script, output tree, image, or accepted
release was changed during this investigation. No Android build was started.

## Exact current evidence

The inspected source is the pinned `android-17.0.0_r1` workspace and its
preserved `out-kernarium-android17` Ninja output that produced accepted release
`android17-ack6.18-16k-20260825-8c5881ab-ext4-apex`.

The generated Soong global dexpreopt configuration says:

```text
DisablePreopt=false
DisablePreoptBootImages=false
OnlyPreoptArtBootImage=false
IsEng=false
HasSystemOther=true
PatternsOnSystemOther=app/%, priv-app/%, system_ext/app/%,
                      system_ext/priv-app/%, product/app/%,
                      product/priv-app/%
DefaultCompilerFilter=""
SystemServerCompilerFilter=""
```

This rules out the tempting but incorrect diagnoses that dexpreopt is globally
disabled, that the product is an `eng` build, or that only the ART boot image
is precompiled. The product is the intended `userdebug` build and the boot and
framework ART artifacts are present.

The accepted target-files archive contains 67 APK entries below the ordinary
partition `app` and `priv-app` directories, but only six adjacent ARM64
application `odex`/`vdex` pairs:

- `KeyChain`, `WallpaperBackup`, `FusedLocation`, `InputDevices`, and
  `SettingsProvider` under `system`; and
- `CuttlefishService` under `vendor`.

There is no `SYSTEM_OTHER` tree and no `system_other.img`. The first five
packages are `PRODUCT_SYSTEM_SERVER_APPS`, so AOSP deliberately exempts them
from `system_other`; the vendor APK does not match the inherited system
patterns. This explains why precisely those existing artifacts survive.

CredentialManager proves that this is an install-placement defect rather than
a missing compiler rule. Soong generated its exact ARM64 dex2oat command with
the complete boot class path, stored class-loader context, Cortex-A53 target,
`--compiler-filter=speed-profile`, and
`--compilation-reason=prebuilt`. Its intermediates exist:

| Artifact | Bytes |
| --- | ---: |
| `package.odex` | 66,392 |
| `package.vdex` | 107,832 |
| `package.art` | 16,416 |

None is installed beside
`SYSTEM/priv-app/CredentialManager/CredentialManager.apk` in target-files.

The retained first-boot Android log from
`.build/android/measurements/platform-compare-ext4-apex-4vcpu-2g-20260825/macos-first/hvc0-wall.log`
contains 36 unique dex2oat input locations. Every runtime command requests
`--compiler-filter=verify --compilation-reason=first-boot`. They divide into:

- 26 ordinary APKs below `/system`, `/system_ext`, or `/product`; and
- ten APKs below active `/apex` mounts.

Conversely, ART reports the existing adjacent system artifacts as
`kOatUpToDate` with filter `speed` and does not recompile them. That is direct
runtime evidence that a matching build artifact satisfies the first-boot
request.

The current partition staging trees also have ample capacity for the first
placement fix: approximately 1.2 GiB is present in a 4 GiB `system`, 321 MiB in
a 1 GiB `system_ext`, and 256 MiB in a 2 GiB `product`. Capacity must still be
checked by the normal image builder because dexpreopt will make previously
unreachable outputs part of the incremental build and will increase the
compressed transfer artifact.

## Root cause in pinned AOSP 17

The product inherits `device/google/cuttlefish/shared/BoardConfig.mk`, which in
turn includes `build/make/target/board/BoardConfigMainlineCommon.mk`. The latter
sets:

```make
BOARD_USES_SYSTEM_OTHER_ODEX := true
```

The Kernarium product independently sets:

```make
PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false
```

Those switches control different decisions:

1. `build/make/core/board_config.mk` honors the product switch and does not set
   `BUILDING_SYSTEM_OTHER_IMAGE`.
2. `build/make/core/dex_preopt_config.mk` nevertheless copies the inherited
   board switch into Soong as `HasSystemOther=true` with the default app
   patterns.
3. `build/soong/dexpreopt/dexpreopt.go` prefixes matching app artifact install
   paths with `system_other/`.
4. `build/soong/java/dexpreopt.go` intentionally skips those installs when it
   constructs the normal partition images. Its source comment says the skip
   exists because Soong creates only the system image there.

The pinned tree already contains the correct precedent. AOSP's
`build/make/target/board/BoardConfigGsiCommon.mk` inherits the same mainline
board policy and then clears it with an empty assignment because GSI does not
want `system_other` odex placement.

## Minimal local implementation

The first implementation should remain entirely in the checked local product
and producer tooling:

1. In `android/aosp-17/overlay/BoardConfig.mk`, after the inherited Cuttlefish
   board include, add:

   ```make
   BOARD_USES_SYSTEM_OTHER_ODEX :=
   ```

   It must be empty, not the non-empty text `false`. Some Make-side placement
   logic tests whether the variable is non-empty rather than comparing it only
   with `true`.
2. Keep `PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false`. Do not add a
   `system_other` partition to the static GPT merely to recover artifacts that
   naturally belong beside their APKs.
3. Add a producer preflight assertion that the resolved
   `BOARD_USES_SYSTEM_OTHER_ODEX` is empty. After the build, validate the
   generated dexpreopt JSON has `DisablePreopt=false`,
   `DisablePreoptBootImages=false`, `OnlyPreoptArtBootImage=false`, and
   `HasSystemOther=false`.
4. Extend target-files verification to reject every `SYSTEM_OTHER` entry, pair
   every installed app `odex` with its `vdex`, and require adjacent ARM64
   artifacts for the version-scoped set of ordinary APKs observed compiling
   on first boot. Keep any deliberate exception explicit rather than using an
   artifact-count threshold.
5. Extend the focused builder fixtures for the resolved board variable, the
   forbidden `SYSTEM_OTHER` namespace, missing half-pairs, and missing required
   app artifacts.
6. Copy the reviewed repository snapshot to the dedicated Linux builder and
   reuse the exact existing `out-kernarium-android17` path. Continue using the
   repository's `build-aosp.sh` and AOSP's Ninja backend; do not clean the
   output and do not switch build systems.
7. Publish and install a new release only after the ordinary producer,
   target-files, VINTF, GPT, copied-release, and installed-release checks pass.

This changes only the project-owned exact-synced device overlay and local
producer checks. It requires no patch to an upstream AOSP project and does not
alter ACK, the static disk layout, userdata format, or Apple consumers.

## Compiler-filter decision

Do not combine placement repair with a compiler-filter experiment.

With the product's current empty default, Soong chooses `speed` for
system-server apps, `speed-profile` where a checked profile exists, and the
legacy name `quicken` for other apps. In this ART release, `quicken` is parsed
as the modern `verify` filter. Thus the existing policy already moves the
current first-boot verification work to the builder once its artifacts are
installed. A `speed` or `speed-profile` artifact is ordered above `verify` and
also satisfies the first-boot request.

If a later, separately measured goal is to put native AOT code in every
ordinary app, the local product can evaluate
`PRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER := speed`, with a narrowly
reviewed `PRODUCT_DEXPREOPT_SPEED_APPS` list only if profile-guided modules
also need forced full-speed compilation. That experiment will increase build
time and image/transfer size and may affect warm application performance; it
is not required to remove the current first-boot `verify` work.

## Residual APEX work

The ten observed APEX-contained APKs are:

- SdkSandbox, AppSearch, Bluetooth, DeviceLockController, ExtServices, and
  MediaProvider;
- OnDevicePersonalization, PermissionController, TetheringNext, and OsuLogin.

`build/soong/java/dexpreopt.go` explicitly returns dexpreopt-disabled for an
APEX variant unless it is an APEX system-server jar. Its own comments limit
current handling to ART boot-class-path jars, APEX system-server jars, and
prebuilt-APEX post-processing; ordinary apps inside an APEX are not covered.
Clearing the board switch therefore cannot create supported artifacts for
these APKs.

The first candidate should leave that behavior unchanged and measure the
residual. Do not claim that all first-boot ART work moved to build time. If the
ten jobs remain dominant, a later local-only investigation can define an APEX
artifact location plus update and invalidation semantics across APK version,
APEX versioned mount path, boot image, ART release, class-loader context, and
instruction-set features. That is not a one-variable product change.

The following shortcuts are rejected:

- do not set `pm.dexopt.first-boot=skip` or disable ART service dexopt; those
  suppress verification rather than moving it;
- do not seed `/data/dalvik-cache` in the release image; userdata is a blank,
  per-instance formatted and metadata-encrypted partition, and those artifacts
  are exact-build/context dependent; and
- do not manually copy intermediate files into an image outside AOSP's install
  graph; dexpreopt owns the APK, boot-image, class-loader-context, ISA, and ART
  identity coupling.

## Acceptance measurement

Before publishing, the candidate target-files must contain no `SYSTEM_OTHER`
tree or image and must contain adjacent ARM64 `odex`/`vdex` pairs for every
required ordinary app. CredentialManager is the representative profile-guided
check; Jelly, LatinIME, Settings, SystemUI, and ThemePicker are representative
user-facing checks.

Boot a fresh COW with the established gadgets, four-vCPU, 2 GiB, 16 KiB,
hardware-Virgl configuration. Capture the Virtio-console ART log separately
from `ttyS0` kernel/init logs. Acceptance requires:

- no first-boot dex2oat input below `/system`, `/system_ext`, `/product`,
  `/vendor`, or `/odm`;
- no ordinary partition APK reported as having no usable artifacts;
- each required adjacent artifact reported up to date with a filter at least
  as good as `verify`;
- any remaining first-boot jobs attributed only to the known APEX boundary;
- no ART crash, OOM, RCU stall, filesystem regression, or boot-completion
  regression; and
- clean persistence, shutdown, and a warm boot that does not invoke `artd`.

Measure the same fresh candidate on macOS, iOS Simulator, and the physical
iPad with the established matched setup. Compare total boot, `artd` lifetime,
first-to-last dex2oat interval, codec exit, boot-animation exit, and completion.
The placement fix is expected to reduce cold ART time. It is not expected to
fix the separate macOS warm post-codec/UI tail, and reducing `artd` exposure
does not by itself prove the previously observed macOS Binder `EFAULT` abort
is fixed.
