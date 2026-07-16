#!/bin/sh

exec </dev/console >/dev/console 2>&1

run_mount()
{
	echo "MOUNT BEGIN: $*"
	/bin/mount "$@"
	echo "MOUNT END ($?): $*"
}

run_mount -t proc proc /proc
/bin/mkdir -p /dev/pts /dev/shm /tmp /run /sys
run_mount -t devpts -o gid=5,mode=620,ptmxmode=0666 devpts /dev/pts
run_mount -t tmpfs -o mode=1777 tmpfs /dev/shm
run_mount -t tmpfs -o mode=1777 tmpfs /tmp
run_mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run
run_mount -t sysfs sysfs /sys

echo "MOUNT TEST COMPLETE"
exec /bin/sh
