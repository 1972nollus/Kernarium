#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "${script_dir}/.." && pwd -P)"

emulator="${UML_EMULATOR:-${repo_root}/.build/presets/macos-integration/bin/uml-darwin}"
rootfs="${UML_ROOTFS:-${repo_root}/rootfs.erofs}"
rootfs_wrapper="${repo_root}/tools/rootfs/with-verified-rootfs.sh"
root_device="${UML_ROOT_DEVICE:-/dev/ubda}"
memory="${UML_MEMORY:-8G}"
systemd_unit="${UML_SYSTEMD_UNIT:-multi-user.target}"
dry_run=false
extra_kernel_arguments=()

usage()
{
	cat <<EOF
Usage: $(basename "$0") [options] [-- kernel-argument ...]

Boot Darwin UML with an EROFS image, a tmpfs-backed OverlayFS root,
and systemd as PID 1.

Options:
  --emulator PATH     Emulator executable
                      (default: .build/presets/macos-integration/bin/uml-darwin)
  --rootfs PATH       EROFS root filesystem image (default: rootfs.erofs)
  --root-device PATH  Guest root block device (default: /dev/ubda)
  --memory SIZE       Guest memory (default: 8G)
  --unit UNIT         Initial systemd unit (default: multi-user.target)
  --dry-run           Print the command without booting
  -h, --help          Show this help

Environment overrides:
  UML_EMULATOR, UML_ROOTFS, UML_ROOT_DEVICE, UML_MEMORY, UML_SYSTEMD_UNIT

The launcher starts the image's /bin/sh as a temporary PID 1, feeds it the
overlay setup, pivots into the new root, and execs the systemd binary. After
the bootstrap commands, host standard input remains connected to the guest
console.
EOF
}

validate_kernel_argument()
{
	argument=$1
	case "$argument" in
	ubd0*=*|root=*|rootfstype=*|init=*|mem=*|console=*|systemd.unit=*)
		echo "$0: kernel argument is controlled by the launcher: $argument" >&2
		exit 2
		;;
	esac
}

while (($# != 0)); do
	case "$1" in
	--emulator)
		(($# >= 2)) || {
			echo "$0: --emulator requires a path" >&2
			exit 2
		}
		emulator="$2"
		shift 2
		;;
	--rootfs)
		(($# >= 2)) || {
			echo "$0: --rootfs requires a path" >&2
			exit 2
		}
		rootfs="$2"
		shift 2
		;;
	--root-device)
		(($# >= 2)) || {
			echo "$0: --root-device requires a path" >&2
			exit 2
		}
		root_device="$2"
		shift 2
		;;
	--memory)
		(($# >= 2)) || {
			echo "$0: --memory requires a size" >&2
			exit 2
		}
		memory="$2"
		shift 2
		;;
	--unit)
		(($# >= 2)) || {
			echo "$0: --unit requires a systemd unit" >&2
			exit 2
		}
		systemd_unit="$2"
		shift 2
		;;
	--dry-run)
		dry_run=true
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	--)
		shift
		extra_kernel_arguments+=("$@")
		break
		;;
	-*)
		echo "$0: unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	*)
		extra_kernel_arguments+=("$1")
		shift
		;;
	esac
done

[[ -x "$emulator" ]] || {
	echo "$0: emulator is not executable: $emulator" >&2
	exit 1
}
[[ -f "$rootfs" ]] || {
	echo "$0: rootfs image does not exist: $rootfs" >&2
	exit 1
}
[[ -n "$root_device" && -n "$memory" && -n "$systemd_unit" ]] || {
	echo "$0: root device, memory, and systemd unit must be non-empty" >&2
	exit 2
}
for kernel_argument in "${extra_kernel_arguments[@]}"; do
	validate_kernel_argument "$kernel_argument"
done

command=(
	"$emulator"
	"ubd0c=$rootfs"
	"root=$root_device"
	"rootfstype=erofs"
	"init=/bin/sh"
	"mem=$memory"
	"console=tty0"
	"ip=dhcp"
	"systemd.unit=$systemd_unit"
)
if ((${#extra_kernel_arguments[@]} != 0)); then
	command+=("${extra_kernel_arguments[@]}")
fi

if "$dry_run"; then
	printf '%q ' "${command[@]}"
	printf '\n'
	exit 0
fi

bootstrap_script="$(cat <<'GUEST_BOOTSTRAP'
set -eu
fail()
{
	echo "Unable to construct the tmpfs overlay root: $*" >&2
	exec /bin/sh
}
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt ||
	fail "tmpfs mount"
mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root ||
	fail "overlay workspace"
mount -o bind / /mnt/lower ||
	fail "EROFS lower bind"
mount -t overlay overlay \
	-o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work \
	/mnt/root ||
	fail "overlay mount"
mount -t devtmpfs devtmpfs /mnt/root/dev ||
	fail "devtmpfs mount"
mkdir -p /mnt/root/.uml-lower ||
	fail "old-root directory"
cd /mnt/root ||
	fail "new-root chdir"
pivot_root . .uml-lower ||
	fail "pivot_root"
rm -f /etc/resolv.conf ||
	fail "remove stale resolver configuration"
printf '%s\n' \
	'# Darwin UML user-mode network' \
	'nameserver 1.1.1.1' \
	'nameserver 8.8.8.8' \
	'nameserver 10.0.2.3' \
	'options timeout:1 attempts:3 rotate' > /etc/resolv.conf ||
	fail "install user-network resolver configuration"
mkdir -p /var/lib/docker ||
	fail "Docker state directory"
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker ||
	fail "Docker state tmpfs"
mkdir -p /var/lib/containerd ||
	fail "containerd state directory"
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd ||
	fail "containerd state tmpfs"
echo "Darwin UML root overlay: lower=erofs, upper=tmpfs"
if [ -x /lib/systemd/systemd ]; then
	exec /lib/systemd/systemd
fi
if [ -x /usr/lib/systemd/systemd ]; then
	exec /usr/lib/systemd/systemd
fi
fail "systemd executable not found"
GUEST_BOOTSTRAP
)"

echo "Booting $root_device from $rootfs as EROFS"
echo "Overlay upper/work: tmpfs; init: systemd ($systemd_unit)"
python3_executable="${PYTHON3:-$(command -v python3 || true)}"
[[ -n "$python3_executable" && -x "$python3_executable" ]] || {
	echo "$0: python3 is required for interactive console forwarding" >&2
	exit 1
}

export UML_GUEST_BOOTSTRAP="$bootstrap_script"
exec "$rootfs_wrapper" --rootfs "$rootfs" -- "$python3_executable" -c '
import os
import select
import signal
import subprocess
import sys
import time

bootstrap = os.environ.pop("UML_GUEST_BOOTSTRAP").encode() + b"\n"
process = subprocess.Popen(
    sys.argv[1:],
    stdin=subprocess.PIPE,
    bufsize=0,
    start_new_session=True,
)
stop_signal = None


def stop(signum, _frame):
    global stop_signal
    stop_signal = signum
    if process.poll() is None:
        process.terminate()


def write_all(data):
    view = memoryview(data)
    while view and process.poll() is None:
        try:
            written = os.write(process.stdin.fileno(), view)
        except BlockingIOError:
            select.select([], [process.stdin.fileno()], [], 0.25)
            continue
        except BrokenPipeError:
            return False
        view = view[written:]
    return not view


for handled_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(handled_signal, stop)

stdin_open = write_all(bootstrap)
try:
    while process.poll() is None:
        if not stdin_open:
            time.sleep(0.1)
            continue
        try:
            readable, _, _ = select.select([0], [], [], 0.1)
        except InterruptedError:
            continue
        if not readable:
            continue
        try:
            data = os.read(0, 65536)
        except BlockingIOError:
            continue
        if not data:
            process.stdin.close()
            stdin_open = False
        elif not write_all(data):
            stdin_open = False
    return_code = process.wait()
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

if stop_signal is not None:
    raise SystemExit(128 + stop_signal)
if return_code < 0:
    raise SystemExit(128 - return_code)
raise SystemExit(return_code)
' "${command[@]}"
