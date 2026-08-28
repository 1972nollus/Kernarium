#!/system/bin/sh
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

partition=/dev/block/by-name/userdata
partition_sectors=$(/system/bin/blockdev --getsz "$partition") || exit 1
read_ext4_value() {
    requested_key=$1
    /system/bin/tune2fs -l "$partition" 2>/dev/null |
        while IFS=: read -r key value; do
            [ "$key" = "$requested_key" ] || continue
            set -- $value
            printf '%s\n' "$1"
            break
        done
}
filesystem_blocks=$(read_ext4_value "Block count") || exit 1
filesystem_block_size=$(read_ext4_value "Block size") || exit 1

case "$partition_sectors:$filesystem_blocks:$filesystem_block_size" in
    *[!0-9:]* | :* | *: | *::* )
        echo "grow-userdata: could not determine ext4 or partition size" >&2
        exit 1
        ;;
esac

filesystem_block_sectors=$((filesystem_block_size / 512))
if [ "$filesystem_block_sectors" -eq 0 ] ||
    [ $((filesystem_block_sectors * 512)) -ne "$filesystem_block_size" ]; then
    echo "grow-userdata: unsupported ext4 block size" >&2
    exit 1
fi
filesystem_sectors=$((filesystem_blocks * filesystem_block_sectors))
if [ "$filesystem_sectors" -gt "$partition_sectors" ]; then
    echo "grow-userdata: ext4 exceeds its userdata partition" >&2
    exit 1
fi
if [ "$filesystem_sectors" -eq "$partition_sectors" ]; then
    exit 0
fi

set +e
/system/bin/e2fsck -pf "$partition"
status=$?
set -e
if [ "$status" -gt 1 ]; then
    echo "grow-userdata: e2fsck failed with status $status" >&2
    exit "$status"
fi
exec /system/bin/resize2fs "$partition"
