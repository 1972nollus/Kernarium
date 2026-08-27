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
UML_ANDROID_SOURCE_MESA := true
UML_ANDROID_DISABLE_AVF := true
UML_ANDROID_DISABLE_IDENTITY := true
UML_ANDROID_DISABLE_REBOOT_ESCROW := true
UML_ANDROID_DISABLE_LEGACY_VNDK := true
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
TARGET_DISABLE_BIOMETRICS_FACE := true
TARGET_NO_TELEPHONY := true
TARGET_USES_CF_RILD := false
TARGET_DEDICATED_RECOVERY := true
TARGET_NO_RECOVERY := false

$(call inherit-product, device/google/cuttlefish/vsoc_arm64_pgagnostic/phone/aosp_cf.mk)
$(call inherit-product, hardware/interfaces/audio/aidl/default/audio_effects.mk)

# The inherited desktop product installs the complete historical AOSP sound
# library. Keep its compact alarm, notification, and UI defaults, but omit the
# legacy ringtone banks: this product has no telephony stack and the files
# otherwise crowd useful app payloads out of the fixed 256 MiB product image.
PRODUCT_COPY_FILES_EXCLUDE += \
    frameworks/base/data/sounds/newwavelabs/% \
    frameworks/base/data/sounds/ringtones/%

PRODUCT_NAME := aosp_kernarium_darwin
PRODUCT_DEVICE := darwin
PRODUCT_BRAND := Android
PRODUCT_MANUFACTURER := Kernarium
PRODUCT_MODEL := Darwin UML

# The desktop Cuttlefish base chooses EROFS APEX payloads. Keep every APEX on
# the uncompressed ext4 path so this guest never depends on EROFS at runtime.
PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE := ext4

PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 16384
PRODUCT_NO_BIONIC_PAGE_SIZE_MACRO := true
PRODUCT_CHECK_PREBUILT_MAX_PAGE_SIZE := true
TARGET_BOOTS_16K := true
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
# exact-synced product overlay.
PRODUCT_PACKAGES_EXCLUDE += \
    device_google_cuttlefish_uml_config_init_vendor_rc \
    device_google_cuttlefish_uml_config_ueventd_rc \
    fstab.uml \
    fstab.uml.vendor_ramdisk
PRODUCT_COPY_FILES += \
    device/kernarium/darwin/config/init.vendor.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.kernarium.rc \
    device/kernarium/darwin/config/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc \
    device/kernarium/darwin/config/fstab.uml:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.uml \
    device/kernarium/darwin/config/fstab.uml:$(TARGET_COPY_OUT_VENDOR_RAMDISK)/first_stage_ramdisk/fstab.uml \
    device/kernarium/darwin/config/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    device/kernarium/darwin/config/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml

# Remove obsolete first-party applications and packages backed by hardware
# that the Darwin UML machine does not expose. Providers and framework APIs
# useful to ordinary third-party applications remain installed.
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

# Jelly supplies the browser; ThemePicker remains part of the user-facing
# baseline, and LatinIME must remain visible even when a host keyboard exists.
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    android.hardware.health-service.example_recovery \
    Jelly \
    LatinIME \
    ThemePicker

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

PRODUCT_PACKAGE_OVERLAYS += device/kernarium/darwin/overlay

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
