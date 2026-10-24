#
# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Select the UML-safe branches in the inherited Cuttlefish product before it
# is evaluated. The Darwin machine has no nested Trusty VM, modem, NFC, sensor,
# Bluetooth, Wi-Fi, GNSS, biometric, secure-element, or camera transport.
TRUSTY_SYSTEM_VM := disabled
UML_ANDROID_SECURITY_HALS := nonsecure
UML_ANDROID_BUILTIN_KERNEL := true
UML_ANDROID_STATIC_STORAGE := true
UML_ANDROID_DISABLE_PROJECT_QUOTAS := true
UML_ANDROID_DISABLE_AVF := true
UML_ANDROID_DISABLE_IDENTITY := true
UML_ANDROID_DISABLE_REBOOT_ESCROW := true
UML_ANDROID_DISABLE_LEGACY_VNDK := true
UML_ANDROID_MULTILIB := true
BOARD_HAVE_BLUETOOTH := false
CF_VENDOR_NO_UWB := true
CF_VENDOR_NO_THREADNETWORK := true
LOCAL_ENABLE_LIGHT := false
LOCAL_ENABLE_OEMLOCK := false
LOCAL_ENABLE_NEURALNETWORKS := false
LOCAL_ENABLE_SENSORS := false
LOCAL_ENABLE_NFC := false
LOCAL_ENABLE_FRP := false
LOCAL_GNSS_PRODUCT_PACKAGE :=
# Select the inherited HIDL primary HAL explicitly so Cuttlefish does not also
# append its MMAP-capable product-property defaults.
LOCAL_AUDIO_PRODUCT_PACKAGE := \
    android.hardware.audio.parameter_parser.example_service \
    com.android.hardware.audio
LOCAL_USE_VENDOR_AUDIO_CONFIGURATION := true
# The Vulkan driver of this product is Mesa's Venus HAL below, not the
# Cuttlefish com.google.cf.vulkan APEX. That APEX carries the gfxstream and
# SwiftShader drivers, their feature declarations, and an init script that
# sets ro.vulkan.apex; the platform Vulkan loader then resolves
# vulkan.<ro.hardware.vulkan>.so only inside that APEX linker namespace and
# never reaches /vendor/lib64/hw. Dropping it also drops SwiftShader's
# insecure executable-memory requirement.
TARGET_VULKAN_SUPPORT := false
TARGET_DISABLE_BIOMETRICS_FACE := true
TARGET_NO_TELEPHONY := true
TARGET_USES_CF_RILD := false
TARGET_DEDICATED_RECOVERY := true
TARGET_NO_RECOVERY := false

# LineageOS switches, read while its configuration is inherited below. The
# machine has no camera, no audio capture device, no charger or fastboot
# transport, and no hardware to apply effects to, so the matching defaults of
# the LineageOS product chain stay out. Debug builds carry `su` for the
# development tooling; a user build never does.
PRODUCT_NO_CAMERA := true
TARGET_EXCLUDES_AUDIOFX := true
# The Android UML ELF is built and published independently; the board stages it
# from the kernel artifact, so LineageOS's own kernel build stays off.
TARGET_NO_KERNEL_OVERRIDE := true
TARGET_DISABLE_FASTBOOTD := true
WITH_LINEAGE_CHARGER := false
ifneq ($(TARGET_BUILD_VARIANT),user)
WITH_SU := true
endif

$(call inherit-product, device/google/cuttlefish/vsoc_arm64_pgagnostic/phone/aosp_cf.mk)
$(call inherit-product, hardware/interfaces/audio/aidl/default/audio_effects.mk)
$(call inherit-product, vendor/lineage/config/common_full_tablet_wifionly.mk)

# The inherited desktop product installs the complete historical AOSP sound
# library. Keep its compact alarm, notification, and UI defaults, but omit the
# legacy ringtone banks: this product has no telephony stack and the files
# otherwise crowd useful app payloads out of the fixed product image.
PRODUCT_COPY_FILES_EXCLUDE += \
    frameworks/base/data/sounds/newwavelabs/% \
    frameworks/base/data/sounds/ringtones/%

# LineageOS's recovery also trusts the public LineageOS release certificate, so
# the project's own builds can be sideloaded. This product is signed with its
# owner's key alone: the recovery and the system trust that key and no other.
PRODUCT_EXTRA_RECOVERY_KEYS :=

PRODUCT_NAME := lineage_darwin
PRODUCT_DEVICE := darwin
PRODUCT_BRAND := Android
PRODUCT_MANUFACTURER := Kernarium
PRODUCT_MODEL := Darwin UML

# LineageOS makes its debug variant not debuggable (any non-empty value of this
# variable counts as set) and always demands ADB authorization. This product
# is debuggable like the AOSP debug build it replaces: userdebug has
# ro.debuggable=1. adbd's boot policy (ro.boot.kernarium.adb_auth) decides
# authorization per boot, and the root service starts with rooted debugging
# enabled only when that policy turns authorization off.
PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG :=

# The LineageOS updater polls a server for a device this project does not
# publish, the setup wizard would block automation, and the remaining apps
# have nothing to work with: no recorder input, no NFC wallet, no fastboot
# transport, and Canvas is the largest app for the least use. The Profiles app
# has no hardware triggers to act on here. DeskClock and EmergencyInfo leave
# with the other absent-hardware packages below.
PRODUCT_PACKAGES_EXCLUDE += \
    Canvas \
    fastbootd \
    LineageSetupWizard \
    Profiles \
    QuickAccessWallet \
    Recorder \
    Updater
PRODUCT_COPY_FILES_EXCLUDE += \
    vendor/lineage/prebuilt/common/etc/init/init.lineage-updater.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.lineage-updater.rc

# The desktop Cuttlefish base chooses EROFS APEX payloads. Keep every APEX on
# the uncompressed ext4 path so this guest never depends on EROFS at runtime.
PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE := ext4

# The kernel runs 4 KiB pages. The userspace stays page-agnostic, so a 16 KiB
# maximum alignment keeps every 64-bit ELF valid on a kernel of either size.
# BoardConfig.mk clears the inherited TARGET_BOOTS_16K, because the parent
# product assigns it after this file is read.
PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 16384
PRODUCT_NO_BIONIC_PAGE_SIZE_MACRO := true
PRODUCT_CHECK_PREBUILT_MAX_PAGE_SIZE := true
PRODUCT_16K_DEVELOPER_OPTION := false

PRODUCT_BUILD_VENDOR_DLKM_IMAGE := false
PRODUCT_BUILD_ODM_DLKM_IMAGE := false
PRODUCT_BUILD_SYSTEM_DLKM_IMAGE := false
PRODUCT_USE_DYNAMIC_PARTITIONS := false
PRODUCT_USE_DYNAMIC_PARTITION_SIZE := false
PRODUCT_BUILD_SUPER_PARTITION := false
PRODUCT_BUILD_SUPER_EMPTY_IMAGE := false
PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false
PRODUCT_VIRTUAL_AB_OTA := false
PRODUCT_VIRTUAL_AB_COMPRESSION := false
PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD :=
PRODUCT_VIRTUAL_AB_COMPRESSION_FACTOR :=
PRODUCT_OTA_FORCE_NON_AB_PACKAGE := false
AB_OTA_POSTINSTALL_CONFIG :=
PRODUCT_BUILD_RECOVERY_IMAGE := true

# Replace generic Cuttlefish boot policy modules with files owned by this
# exact-synced product overlay. The inherited product init script declares only
# tombstone_transmit, which this product does not install. ueventd reads vendor
# rules only from /vendor/etc/ueventd.rc.
PRODUCT_PACKAGES_EXCLUDE += \
    device_google_cuttlefish_shared_config_init_product_rc \
    device_google_cuttlefish_uml_config_init_vendor_rc \
    device_google_cuttlefish_uml_config_ueventd_rc \
    fstab.uml \
    fstab.uml.vendor_ramdisk
PRODUCT_COPY_FILES += \
    device/kernarium/darwin/config/init.vendor.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.kernarium.rc \
    device/kernarium/darwin/config/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    device/kernarium/darwin/config/fstab.uml:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.uml \
    device/kernarium/darwin/config/fstab.uml:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/first_stage_ramdisk/fstab.uml \
    device/kernarium/darwin/config/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    device/kernarium/darwin/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml

# Remove obsolete first-party applications and packages backed by hardware
# that the Darwin UML machine does not expose. Providers and framework APIs
# useful to ordinary third-party applications remain installed, and so does
# sgdisk: vold executes /system/bin/sgdisk to scan and partition any disk it
# manages, such as the virtual disk of `sm set-virtual-disk`.
KERNARIUM_REMOVED_PACKAGES := \
    BasicDreams \
    BluetoothMidiService \
    BookmarkProvider \
    Browser2 \
    CFSatelliteService \
    CuttlefishTetheringOverlay \
    CuttlefishWifiOverlay \
    Calendar \
    CallLogBackup \
    Camera2 \
    CameraExtensionsProxy \
    CarrierConfig \
    CarrierDefaultApp \
    CellBroadcastLegacyApp \
    Contacts \
    CuttlefishService \
    DeskClock \
    DeviceAsWebcam \
    DynamicSystemInstallationService \
    Dialer \
    EasterEgg \
    EmergencyInfo \
    EyesFreeVidService \
    Gallery2 \
    ImsServiceEntitlement \
    MmsService \
    Music \
    MusicFX \
    ONS \
    PhotoTable \
    QuickSearchBox \
    SatelliteClient \
    SimAppDialog \
    Stk \
    TeleService \
    Telecom \
    TelecomShim \
    TelephonyProvider \
    android.hardware.biometrics.face-service.default \
    android.hardware.biometrics.fingerprint-service.default \
    android.hardware.motioncontext-service.default \
    androidx.camera.extensions.impl \
    com.android.cellbroadcast \
    com.android.compos \
    com.android.hardware.authsecret \
    com.android.hardware.biometrics.face.virtual \
    com.android.hardware.biometrics.fingerprint.virtual \
    com.android.hardware.cas \
    com.android.hardware.graphics.composer.ranchu \
    com.android.hardware.contexthub \
    com.android.hardware.gnss \
    com.android.hardware.input.processor \
    com.android.hardware.net.nlinterceptor \
    com.android.hardware.rebootescrow \
    com.android.hardware.secure_element \
    com.android.hardware.security.authgraph \
    com.android.hardware.security.secretkeeper \
    com.android.hardware.tetheroffload \
    com.android.hardware.thermal \
    com.android.hardware.usb \
    com.android.hardware.vibrator \
    com.android.hardware.wifi \
    com.android.vndk.v31 \
    com.android.vndk.v32 \
    com.android.vndk.v33 \
    com.android.vndk.v34 \
    com.google.emulated.camera.provider.hal \
    com.google.emulated.camera.provider.hal.v4l2 \
    com.google.cf.confirmationui \
    com.google.cf.health \
    com.google.cf.bt \
    com.google.cf.identity \
    com.google.cf.ir \
    com.google.cf.wifi \
    com.google.cf.wpa_supplicant \
    cuttlefish_telephony_satellite_configs \
    gsi_tool \
    gsid \
    android.hardware.health-service.cuttlefish_recovery \
    messaging \
    pmg_daemon \
    sample_camera_extensions.xml \
    snapshotctl \
    snapuserd \
    snapuserd_ramdisk \
    socket_vsock_proxy \
    suspend_blocker \
    tombstone_transmit \
    update_engine \
    update_engine_client \
    update_verifier \
    wificond \
    wificond_compat_symlink_module
PRODUCT_PACKAGES_EXCLUDE += $(KERNARIUM_REMOVED_PACKAGES)

# The inherited Cuttlefish product also asks for modules this tree does not
# define. AOSP ignores such requests; LineageOS requires every requested
# package to exist, so name them here instead of switching the check off.
PRODUCT_PACKAGES_EXCLUDE += \
    libGoldfishProfiler \
    metrics_helper \
    sleep

# The browser (Jelly), the keyboard (LatinIME) and ThemePicker come from the
# LineageOS product chain. resize2fs grows the compact userdata filesystem
# after the host expands its partition and before the first /data mount. The
# F-Droid client and its Privileged Extension are presigned prebuilts that
# source preparation materializes under vendor/kernarium/fdroid.
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    android.hardware.health-service.example_recovery \
    e2fsck \
    F-Droid \
    F-DroidPrivilegedExtension \
    resize2fs

# First boot compiles nothing. Soong preopts the applications it installs, but
# never an application inside an APEX, and the presigned F-Droid prebuilts opt
# out. tools/precompile-first-boot-apps.py compiles the rest after a first build
# pass; build-aosp.sh hands its PRODUCT_COPY_FILES fragment to the second pass
# through this variable, a path relative to the workspace root.
ifneq ($(LINEAGE24_FIRST_BOOT_OAT_MK),)
include $(LINEAGE24_FIRST_BOOT_OAT_MK)
endif

# Replace the handheld feature declaration and remove optional feature files
# for absent hardware. This keeps PackageManager truthful for sideloaded apps.
PRODUCT_COPY_FILES_EXCLUDE += \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml \
    frameworks/native/data/etc/android.hardware.biometrics.face.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.biometrics.face.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml \
    frameworks/native/data/etc/android.hardware.camera.concurrent.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.concurrent.xml \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.camera.full.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.full.xml \
    frameworks/native/data/etc/android.hardware.camera.raw.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.raw.xml \
    frameworks/native/data/etc/android.hardware.consumerir.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.consumerir.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.uwb.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.uwb.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.passpoint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.passpoint.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.software.verified_boot.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.verified_boot.xml
PRODUCT_COPY_FILES += \
    device/kernarium/darwin/config/kernarium_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml

# The Vulkan HAL is Mesa's Venus driver built from this project's pinned guest
# Mesa source. Its generated Soong module names are scoped with a venus_
# prefix so they cannot collide with the AOSP external/mesa3d modules. The
# feature declarations the removed Cuttlefish Vulkan APEX used to install
# come from the device tree instead, at the level this driver supports. No
# Vulkan dEQP level is declared: the driver has not been qualified. The
# loader finds the module by ro.hardware.vulkan, whose one writer is the boot
# command line's androidboot.hardware.vulkan, as for ro.hardware.egl below.
PRODUCT_PACKAGES_EXCLUDE += com.google.cf.vulkan
PRODUCT_PACKAGES += vulkan.virtio

# The EGL and GLES libraries are Mesa's Virgl driver from the same source and
# generated port. The platform EGL loader finds them in /vendor/lib64/egl by the
# ro.hardware.egl suffix; the gallium driver they link lies in /vendor/lib64.
# The inherited Cuttlefish Virgl configuration would add the AOSP
# external/mesa3d libGLES_mesa instead. The boot command line's
# androidboot.hardware.egl is the property's one writer: the product init
# script copies it, like the Vulkan and gralloc selections, at early-init, and
# that setprop fails if a vendor property has already set it. No vendor
# property may name any of the three.
#
# That script replaces the inherited Cuttlefish init_graphics.vendor.rc, which
# also copies the goldfish composer mode and the ANGLE feature overrides. This
# product has neither, their androidboot sources never exist, and each copy
# failed at every boot.
PRODUCT_PACKAGES_EXCLUDE += \
    device_google_cuttlefish_shared_config_init_graphics_vendor_rc \
    libGLES_mesa
PRODUCT_PACKAGES += \
    libEGL_kernarium \
    libGLESv1_CM_kernarium \
    libGLESv2_kernarium

# Mesa reads a buffer's DRM format, modifier and plane layout through the
# gralloc0 interface of minigbm, the product's allocator. The passthrough
# gralloc.default module cannot answer those queries, so Mesa would fall
# back to guessing and the driver could not describe the buffers the window
# system hands it. The module is a same-process HAL like the driver above,
# selected through ro.hardware.gralloc by androidboot.hardware.gralloc.
PRODUCT_PACKAGES += gralloc.minigbm
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level-1.xml \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute-0.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_4.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version-1_4.xml \
    frameworks/native/data/etc/android.software.opengles.deqp.level-latest.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.opengles.deqp.level-latest.xml

# A Vulkan window-system smoke test for this product. It owns a
# SurfaceFlinger layer, wraps it through VK_KHR_android_surface, and clears
# and presents a swapchain, so a run exercises the gralloc buffers the
# platform loader hands the driver through VK_ANDROID_native_buffer.
PRODUCT_PACKAGES += kernarium_vk_present
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += system/bin/kernarium_vk_present

# Hardware video encoding. The host's VideoToolbox encoders reach the guest as a
# V4L2 stateful encoder, and external/v4l2_codec2 exposes them through Codec2:
# its service is the vendor Codec2 store beside the software one. The XML lists
# the encoders alone, so hardware decoders stay out of Android's codec list, and
# the properties enable exactly those two components. The store admits as many
# instances as the host serves streams. The extended policy adds the calls the
# service's seccomp policy lacks for the V4L2 device, and ueventd gives the
# nodes to the group the service joins.
PRODUCT_SOONG_NAMESPACES += external/v4l2_codec2
PRODUCT_PACKAGES += \
    android.hardware.media.c2-service-v4l2 \
    libc2plugin_store
PRODUCT_COPY_FILES += \
    device/kernarium/darwin/config/media_codecs_c2_v4l2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    device/kernarium/darwin/config/media_c2_extended_seccomp_policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.v4l2_codec2.encoder.supported.h264=true \
    ro.vendor.v4l2_codec2.encoder.supported.hevc=true \
    ro.vendor.v4l2_codec2.encode_concurrent_instances=8

PRODUCT_PACKAGE_OVERLAYS += device/kernarium/darwin/overlay

# ADB is available over the inherited TCP port even though this machine has no
# USB controller. A user build retains the stock shell privilege boundary; a
# userdebug build also lets `adb root` restart adbd as root, so whoever can
# reach the port without authorization is root in the guest. Apple hosts select
# authorization through the immutable ro.boot.kernarium.adb_auth property on
# each boot, and that boot policy overrides the build type's ro.adb.secure
# default inside adbd.
PRODUCT_PRODUCT_PROPERTIES := $(filter-out \
    persist.sys.usb.config=%,$(PRODUCT_PRODUCT_PROPERTIES))
PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.usb.config=adb

# The WebView provider declares use32bitAbi. Left alone, that makes its 32-bit
# libraries primary, and every WebView renderer, including those of 64-bit
# applications, a 32-bit process. Keep the 64-bit ABI primary: 32-bit
# applications still load the 32-bit WebView library in their own process and
# talk to a 64-bit renderer.
PRODUCT_PRODUCT_PROPERTIES += \
    ro.config.ignore_use32bitabi=true

# The Virtio sound transport has no MMAP or exclusive hardware path. These
# read-only defaults seed a fresh Settings database; persisted user volume
# choices continue to take precedence after first boot.
PRODUCT_PRODUCT_PROPERTIES += \
    sys.use_memfd=true \
    aaudio.mmap_policy=1 \
    aaudio.mmap_exclusive_policy=1 \
    ro.config.vc_call_vol_steps=15 \
    ro.config.vc_call_vol_default=15 \
    ro.config.media_vol_default=15 \
    ro.config.alarm_vol_default=7 \
    ro.config.system_vol_default=7 \
    ro.config.assistant_vol_default=15 \
    audio.timecheck.timeout_duration_ms=300000 \
    audio.timecheck.second_chance_duration_ms=200000
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    ro.audio.ihaladaptervendorextension_enabled=true

PRODUCT_IGNORE_ALL_ANDROIDMK := true
