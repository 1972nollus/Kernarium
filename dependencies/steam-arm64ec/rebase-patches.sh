#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export UML_DEPENDENCY_MANIFEST="$here/manifest.tsv"
export UML_DEPENDENCY_METADATA_ROOT="$here"
exec "$here/../rebase-patches.sh" "$@"
