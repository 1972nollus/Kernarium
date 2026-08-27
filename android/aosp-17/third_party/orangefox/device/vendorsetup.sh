#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later

if [[ "${FOX_BUILD_DEVICE:-}" == darwin || "${TARGET_PRODUCT:-}" == twrp_darwin ]]; then
    export FOX_BUILD_DEVICE=darwin
    export FOX_BUILD_TYPE=Unofficial
    export FOX_VARIANT=Kernarium
    export FOX_VANILLA_BUILD=1
    export OF_DISABLE_MIUI_SPECIFIC_FEATURES=1
    export TW_DEFAULT_LANGUAGE=en
    export LC_ALL=C
    export ALLOW_MISSING_DEPENDENCIES=true
    export FOX_SETTINGS_ROOT_DIRECTORY=/data/recovery
    export FOX_MISCELLANEOUS_ROOT_DIRECTORY=/sdcard
fi
