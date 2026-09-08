#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
case "${1:-}" in
run) shift; exec sudo -n podman run --network=none --pull=never "$@" ;;
*) printf '%s\n' 'Only offline container execution is supported' >&2; exit 2 ;;
esac
