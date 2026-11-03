#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Embed a framework that exists for the iOS device alone: the Run Script
# phase of an application target that also builds for the iOS Simulator calls
# it in place of an Embed Frameworks entry.
#
# usage: embed-device-framework.sh XCFRAMEWORK
#
# KosmicKrisp is the Vulkan driver of the device's GPU stack and requires
# Metal 4, which the Simulator does not have, so its XCFramework has a device
# slice alone. Xcode resolves a slice of every XCFramework in an Embed
# Frameworks phase for the SDK it builds, and a platform filter cannot name
# the device apart from the Simulator, so a device-only framework cannot sit
# in that phase of a target the Simulator also builds. This script embeds it
# as the phase does for a device build, with no headers or modules and the
# target's signing identity, and for any other SDK embeds nothing: the
# Simulator's GPU stack has no Vulkan driver to carry.
#
# Xcode runs it with the target's build settings in the environment.
set -eu

fail()
{
	printf '%s\n' "embed-device-framework.sh: $*" >&2
	exit 1
}

[ "$#" -eq 1 ] || fail "usage: embed-device-framework.sh XCFRAMEWORK"
XCFRAMEWORK=$1
NAME=$(basename -- "$XCFRAMEWORK" .xcframework)

case "${PLATFORM_NAME:-}" in
iphoneos) ;;
*)
	printf '%s\n' "$NAME is a device-only framework: not embedded for ${PLATFORM_NAME:-an unknown SDK}"
	exit 0
	;;
esac

SLICE="$XCFRAMEWORK/ios-arm64/$NAME.framework"
[ -d "$SLICE" ] || fail "$XCFRAMEWORK has no device slice; package it with Kernarium/scripts/package-ios-frameworks.sh"
[ -n "${TARGET_BUILD_DIR:-}" ] && [ -n "${FRAMEWORKS_FOLDER_PATH:-}" ] ||
	fail "run it from an Xcode build phase"

DESTINATION="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH/$NAME.framework"
mkdir -p -- "$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH"
rm -rf -- "$DESTINATION"
/usr/bin/rsync -a \
	--exclude .DS_Store --exclude Headers --exclude PrivateHeaders \
	--exclude Modules --exclude '*.tbd' \
	"$SLICE/" "$DESTINATION/"

# Sign it on copy, as the Embed Frameworks phase's CodeSignOnCopy does.
if [ "${CODE_SIGNING_ALLOWED:-YES}" = YES ] &&
	[ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]; then
	# shellcheck disable=SC2086
	/usr/bin/codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" \
		${OTHER_CODE_SIGN_FLAGS:-} \
		--preserve-metadata=identifier,entitlements,flags \
		--generate-entitlement-der "$DESTINATION"
fi
printf '%s\n' "embedded $NAME for the device"
