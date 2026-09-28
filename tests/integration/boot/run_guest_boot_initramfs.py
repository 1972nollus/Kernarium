#!/usr/bin/env python3
"""Boot a guest through the base archive and a per-run initramfs.

The launcher appends a per-run segment (its built-in /init, /boot.env and
/payload files) to the base archive it embeds. A run passes only when the
generic /init mounts the EROFS image beneath a tmpfs overlay, installs the
payload, replacing a file the image carries, leaves both layers visible and
switches root into the payload's entry program as PID 1. The
remaining cases require every broken boot to stop with a named reason and a
nonzero exit instead of reaching the image's own init: the base archive alone,
a segment appended without the base's padding, an initrd the kernel never
loads, and an invalid /boot.env. The contract is
tools/rootfs/boot-ramdisk/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
import uuid


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERIC_INIT = REPOSITORY / "tools" / "rootfs" / "boot-ramdisk" / "init"
MISSING_INIT = "init=/uml-per-run-initramfs-did-not-run"
BOOT_TIMEOUT_SECONDS = 180
PROBE_PATH = "/uml-boot-probe"
TOKEN_PATH = "/opt/uml-boot-initramfs-test/nested/token"
# A file every supported image carries, which the payload must replace.
REPLACED_PATH = "/etc/hostname"

# Runs as PID 1 in the new root, with the image's own shell.
PROBE = f"""#!/bin/sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
failures=
check()
{{
	name=$1
	shift
	"$@" || failures="$failures $name"
}}
check pid-1 [ "$$" = 1 ]
check overlay-root grep -q \\
	'^overlay / overlay .*lowerdir=/lower,upperdir=/state/upper,workdir=/state/work' \\
	/proc/mounts
check erofs-lower [ -d /sys/fs/erofs/ubda ]
check lower-visible grep -q '^/dev/ubda /.uml-lower erofs ro[, ]' /proc/mounts
check state-visible grep -q '^tmpfs /.uml-state tmpfs ' /proc/mounts
check initramfs-gone [ ! -e /boot.env ]
token=$(cat {TOKEN_PATH} 2>/dev/null)
check replaced-file [ "$(cat {REPLACED_PATH})" = "$token" ]
echo "UML_BOOT_PROBE_CMDLINE:$(cat /proc/cmdline)"
if [ -z "$failures" ]; then
	echo "UML_BOOT_PROBE_OK:$token"
else
	echo "UML_BOOT_PROBE_FAILED:$failures"
fi
sync
poweroff -f
"""


class CaseFailure(RuntimeError):
    pass


def launch(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    engine: str,
    scratch: pathlib.Path,
    extra: list[str],
) -> tuple[int, str]:
    command = [
        str(emulator),
        f"--execution-engine={engine}",
        *extra,
        f"ubd0c={rootfs}",
        "mem=512M",
        "ncpus=1",
    ]
    environment = dict(os.environ, TMPDIR=str(scratch))
    try:
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=environment,
            timeout=BOOT_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.output or b"").decode(errors="replace")
        raise CaseFailure(
            f"the guest did not stop within {BOOT_TIMEOUT_SECONDS} seconds\n"
            + output[-8192:]
        ) from error
    return result.returncode, result.stdout.decode(errors="replace")


def require(condition: bool, message: str, output: str) -> None:
    if not condition:
        raise CaseFailure(message + "\n" + output[-8192:])


def require_probe(status: int, output: str, token: str, cmdline: str) -> None:
    require("UML_BOOT_ERROR:" not in output, "the per-run /init failed", output)
    require(
        "UML_BOOT_BASE_ERROR:" not in output,
        "the base archive's /init ran instead of the per-run one",
        output,
    )
    require(
        f"UML_BOOT_SWITCH_ROOT:{PROBE_PATH}" in output,
        "the per-run /init did not switch root into the payload entry",
        output,
    )
    require(
        f"UML_BOOT_PROBE_OK:{token}" in output,
        "the new root is not an overlay on the EROFS image, with both layers "
        "visible and this run's payload",
        output,
    )
    require(
        f"UML_BOOT_PROBE_CMDLINE:" in output and cmdline in output,
        f"the kernel command line lacks {cmdline}",
        output,
    )
    require("Kernel panic" not in output, "the guest panicked", output)
    require(status == 0, f"the guest exited with status {status}", output)


def require_loud_failure(status: int, output: str, marker: str) -> None:
    require(marker in output, f"the guest did not report {marker!r}", output)
    require("Kernel panic" in output, "the guest did not stop at a panic", output)
    require("UML_BOOT_PROBE" not in output, "the payload entry ran", output)
    require(status != 0, "a broken boot exited successfully", output)


def run(arguments: argparse.Namespace) -> None:
    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    base = arguments.base.resolve()
    tool = arguments.initramfs_tool.resolve()
    engine = arguments.engine
    with tempfile.TemporaryDirectory(prefix="uml-boot-initramfs-test-") as name:
        work = pathlib.Path(name)
        scratch = work / "launcher-tmp"
        scratch.mkdir()
        token = uuid.uuid4().hex
        probe = work / "probe"
        probe.write_text(PROBE)
        probe.chmod(0o755)
        token_file = work / "token"
        token_file.write_text(token + "\n")
        boot_env = work / "boot.env"
        boot_env.write_text(f"UML_BOOT_ENTRY={PROBE_PATH}\n")
        guest_files = [
            f"--guest-file=/boot.env={boot_env}",
            f"--guest-file=/payload{PROBE_PATH}={probe}",
            f"--guest-file=/payload{TOKEN_PATH}={token_file}",
            f"--guest-file=/payload{REPLACED_PATH}={token_file}",
        ]

        # The kernel never loads the launcher's initrd, so only the
        # launcher's init= stands between the guest and the image's init.
        status, output = launch(
            emulator, rootfs, engine, scratch, [*guest_files, "initrd_offset=x"]
        )
        require_loud_failure(
            status,
            output,
            "Requested init /uml-per-run-initramfs-did-not-run failed",
        )
        require(
            any(scratch.iterdir()),
            "an aborted run left no scratch directory to exercise its removal",
            output,
        )
        print("ok: an initrd the kernel never loads stops at the launcher's init=")

        status, output = launch(emulator, rootfs, engine, scratch, guest_files)
        require_probe(status, output, token, MISSING_INIT)
        leftovers = sorted(path.name for path in scratch.iterdir())
        require(
            not leftovers,
            f"the launcher left scratch directories behind: {leftovers}",
            output,
        )
        print("ok: the launcher's initramfs boots the payload on the EROFS overlay")

        # A host that drives the guest elsewhere writes the same initrd with
        # the host tool and names it itself.
        composed = work / "composed.cpio"
        subprocess.run(
            [
                str(tool),
                f"--output={composed}",
                f"--base={base}",
                f"--init={GENERIC_INIT}",
                f"--file=/boot.env={boot_env}",
                f"--file=/payload{PROBE_PATH}={probe}",
                f"--file=/payload{TOKEN_PATH}={token_file}",
                f"--file=/payload{REPLACED_PATH}={token_file}",
            ],
            check=True,
            timeout=60,
        )
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [f"initrd={composed}", MISSING_INIT],
        )
        require_probe(status, output, token, f"{MISSING_INIT} ")
        print("ok: uml-initramfs writes an initrd that boots the same payload")

        status, output = launch(
            emulator, rootfs, engine, scratch, [f"initrd={base}"]
        )
        require_loud_failure(status, output, "UML_BOOT_BASE_ERROR:")
        print("ok: the base archive alone stops at its own /init")

        segment = work / "segment.cpio"
        subprocess.run(
            [
                str(tool),
                f"--output={segment}",
                f"--init={GENERIC_INIT}",
                f"--file=/boot.env={boot_env}",
            ],
            check=True,
            timeout=60,
        )
        base_bytes = base.read_bytes()
        require(
            len(base_bytes) % 4 != 0,
            "the base archive is already four-byte aligned, so an unpadded "
            "append cannot be told apart from a padded one",
            "",
        )
        unpadded = work / "unpadded.cpio"
        unpadded.write_bytes(base_bytes + segment.read_bytes())
        status, output = launch(
            emulator, rootfs, engine, scratch, [f"initrd={unpadded}"]
        )
        # The kernel looks for the next header at the base's exact length,
        # finds "070701" at an unaligned offset and takes it for an unknown
        # compression; it keeps what it unpacked before that point.
        require(
            "invalid magic at start of compressed archive" in output,
            "the kernel accepted a segment appended without padding",
            output,
        )
        require_loud_failure(status, output, "UML_BOOT_BASE_ERROR:")
        print("ok: a segment appended without padding stops at the base's /init")

        invalid_env = work / "invalid.env"
        invalid_env.write_text("UML_BOOT_ENTRY=/sbin/init\nUML_BOOT_TYPO=1\n")
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [f"--guest-file=/boot.env={invalid_env}"],
        )
        require_loud_failure(
            status, output, "UML_BOOT_ERROR:unknown /boot.env parameter: UML_BOOT_TYPO"
        )
        print("ok: an invalid /boot.env stops the per-run /init")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--base",
        required=True,
        type=pathlib.Path,
        help="the accepted base archive the emulator embeds",
    )
    parser.add_argument("--initramfs-tool", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    try:
        run(arguments)
    except (CaseFailure, subprocess.SubprocessError) as error:
        print(f"boot initramfs check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
