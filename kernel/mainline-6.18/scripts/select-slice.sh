#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared selector for the normal arm64 kernel series and the 4 KiB x86_64
# userspace compatibility overlay. This file is sourced by producer scripts.

uml_kernel_select_slice()
{
	uml_select_line_root=$1
	uml_select_requested=$2
	uml_select_parent_mode=${3:-strict}
	case "$uml_select_parent_mode" in
	strict|allow-stale-parent) ;;
	*)
		printf '%s\n' "invalid kernel slice parent-selection mode: $uml_select_parent_mode" >&2
		return 1
		;;
	esac
	uml_select_common_metadata="$uml_select_line_root/patches/metadata.env"
	[ -r "$uml_select_common_metadata" ] || {
		printf '%s\n' "missing common kernel metadata: $uml_select_common_metadata" >&2
		return 1
	}

	# shellcheck disable=SC1090
	. "$uml_select_common_metadata"
	[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 3 ] || {
		printf '%s\n' "unsupported common kernel series schema" >&2
		return 1
	}
	UML_KERNEL_COMMON_PATCH_BRANCH=$UML_KERNEL_PATCH_BRANCH
	UML_KERNEL_COMMON_FINAL_TREE=$UML_KERNEL_FINAL_TREE
	UML_KERNEL_COMMON_PATCH_COUNT=$UML_KERNEL_PATCH_COUNT
	UML_KERNEL_COMMON_RELEASE_VERSION=$UML_KERNEL_RELEASE_VERSION
	UML_KERNEL_COMMON_UPSTREAM_REPOSITORY=$UML_KERNEL_UPSTREAM_REPOSITORY
	UML_KERNEL_COMMON_UPSTREAM_BRANCH=$UML_KERNEL_UPSTREAM_BRANCH
	UML_KERNEL_COMMON_UPSTREAM_COMMIT=$UML_KERNEL_UPSTREAM_COMMIT
	UML_KERNEL_COMMON_UPSTREAM_TREE=$UML_KERNEL_UPSTREAM_TREE
	UML_KERNEL_COMMON_BASE_REF=$UML_KERNEL_BASE_REF
	UML_KERNEL_COMMON_ABI_VERSION=$UML_KERNEL_ABI_VERSION
	UML_KERNEL_COMMON_ABI_FINGERPRINT=$UML_KERNEL_ABI_FINGERPRINT
	UML_KERNEL_COMMON_METADATA_PATH=$uml_select_common_metadata
	UML_KERNEL_COMMON_PATCH_DIR="$uml_select_line_root/patches/series"
	UML_KERNEL_COMMON_MANIFEST_PATH="$UML_KERNEL_COMMON_PATCH_DIR/manifest.tsv"

	case "$uml_select_requested" in
	arm64)
		UML_KERNEL_SLICE=arm64
		UML_KERNEL_POLICY_ROOT=$uml_select_line_root
		UML_KERNEL_METADATA_PATH=$uml_select_common_metadata
		UML_KERNEL_PATCH_DIR=$UML_KERNEL_COMMON_PATCH_DIR
		UML_KERNEL_MANIFEST_PATH=$UML_KERNEL_COMMON_MANIFEST_PATH
		UML_KERNEL_PARENT_BRANCH=''
		UML_KERNEL_PARENT_TREE=$UML_KERNEL_UPSTREAM_TREE
		UML_KERNEL_SLICE_CONFIG_FRAGMENT=''
		;;
	x86_64)
		UML_KERNEL_POLICY_ROOT="$uml_select_line_root/slices/x86_64"
		UML_KERNEL_METADATA_PATH="$UML_KERNEL_POLICY_ROOT/patches/metadata.env"
		UML_KERNEL_PATCH_DIR="$UML_KERNEL_POLICY_ROOT/patches/series"
		UML_KERNEL_MANIFEST_PATH="$UML_KERNEL_PATCH_DIR/manifest.tsv"
		UML_KERNEL_SLICE_CONFIG_FRAGMENT="$UML_KERNEL_POLICY_ROOT/config/userspace.fragment"
		[ -r "$UML_KERNEL_METADATA_PATH" ] || {
			printf '%s\n' "missing x86_64 slice metadata: $UML_KERNEL_METADATA_PATH" >&2
			return 1
		}
		# shellcheck disable=SC1090
		. "$UML_KERNEL_METADATA_PATH"
		[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 3 ] || {
			printf '%s\n' "unsupported x86_64 kernel series schema" >&2
			return 1
		}
		[ "${UML_KERNEL_SLICE:-}" = x86_64 ] || {
			printf '%s\n' "x86_64 metadata has the wrong slice identity" >&2
			return 1
		}
		[ "$UML_KERNEL_RELEASE_VERSION" = "$UML_KERNEL_COMMON_RELEASE_VERSION" ] &&
		[ "$UML_KERNEL_UPSTREAM_REPOSITORY" = "$UML_KERNEL_COMMON_UPSTREAM_REPOSITORY" ] &&
		[ "$UML_KERNEL_UPSTREAM_BRANCH" = "$UML_KERNEL_COMMON_UPSTREAM_BRANCH" ] &&
		[ "$UML_KERNEL_UPSTREAM_COMMIT" = "$UML_KERNEL_COMMON_UPSTREAM_COMMIT" ] &&
		[ "$UML_KERNEL_UPSTREAM_TREE" = "$UML_KERNEL_COMMON_UPSTREAM_TREE" ] &&
		[ "$UML_KERNEL_BASE_REF" = "$UML_KERNEL_COMMON_BASE_REF" ] &&
		[ "$UML_KERNEL_ABI_VERSION" = "$UML_KERNEL_COMMON_ABI_VERSION" ] &&
		[ "$UML_KERNEL_ABI_FINGERPRINT" = "$UML_KERNEL_COMMON_ABI_FINGERPRINT" ] || {
			printf '%s\n' "x86_64 metadata does not inherit the common kernel identity" >&2
			return 1
		}
		if [ "$uml_select_parent_mode" = strict ] &&
		   [ "${UML_KERNEL_PARENT_TREE:-}" != "$UML_KERNEL_COMMON_FINAL_TREE" ]; then
			printf '%s\n' "x86_64 slice parent tree does not match the common series" >&2
			return 1
		fi
		UML_KERNEL_PARENT_BRANCH=$UML_KERNEL_COMMON_PATCH_BRANCH
		;;
	*)
		printf '%s\n' "kernel slice must be arm64 or x86_64" >&2
		return 1
		;;
	esac
}
