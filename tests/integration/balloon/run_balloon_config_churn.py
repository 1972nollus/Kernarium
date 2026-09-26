#!/usr/bin/env python3
"""Keep a guest running while its balloon reports a config change every 2 ms.

An external test backend serves the guest's Virtio balloon. Every 2 ms it
sends a config-change request on the vhost-user backend channel and waits for
the guest's acknowledgement; each change makes the guest's balloon worker read
the config again over the main socket. With one vCPU, that worker runs on the
only CPU that services the backend channel. A backend that waits for the
acknowledgement without answering the config read, while the guest waits for
that answer without draining the backend channel, stops the guest for good:
the command below never finishes and the kernel reports an RCU stall.

The guest must keep running commands and must have acknowledged and read many
of the changes, which shows they still reach the driver.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import socket
import subprocess
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


BALLOON_DEVICE_ID = 5
PERIOD_MILLISECONDS = 2
GUEST_MEMORY = "512M"
CHURN_SECONDS = 20
MINIMUM_CHANGES = 100
SERVER_EXIT_SECONDS = 20
SUMMARY = re.compile(
    rb"balloon churn: (\d+) config changes raised, (\d+) config reads answered"
)


def churn(guest: Guest) -> None:
    # The backend starts raising changes as soon as the guest connects, so
    # the boot can stop too.
    try:
        guest.boot_to_shell(180)
        # poweroff refuses to run without /proc.
        mounted = guest.run_command("mount -t proc proc /proc", 20)
        churned = guest.run_command(
            f"sleep {CHURN_SECONDS}; echo CHURN-$((6*7))",
            CHURN_SECONDS + 60,
        )
    except TimeoutError as error:
        raise RuntimeError(
            "the guest stopped while its balloon config churned: a "
            "config-change request and a config read waited for each other"
        ) from error
    if mounted.return_code != 0:
        raise RuntimeError("could not mount /proc:\n" + mounted.output)
    if churned.return_code != 0 or "CHURN-42" not in churned.output:
        raise RuntimeError("the churn command failed:\n" + churned.output)
    if not guest.shutdown(30):
        raise RuntimeError("the guest did not power off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    backend_end, guest_end = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    server = subprocess.Popen(
        [
            str(arguments.server.resolve()),
            "--fd", str(backend_end.fileno()),
            "--period-ms", str(PERIOD_MILLISECONDS),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(backend_end.fileno(),),
    )
    backend_end.close()
    output = b""
    try:
        try:
            guest = Guest(
                arguments.emulator.resolve(),
                arguments.rootfs.resolve(),
                GUEST_MEMORY,
                "tmpfs",
                (
                    "ncpus=1",
                    f"virtio_uml.device=fd={guest_end.fileno()}:"
                    f"{BALLOON_DEVICE_ID}",
                ),
                init_shell=True,
                pass_fds=(guest_end.fileno(),),
            )
        finally:
            guest_end.close()
        try:
            churn(guest)
        except BaseException:
            print(guest.output()[-4000:], file=sys.stderr)
            raise
        finally:
            guest.close()
        try:
            output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(
                "the balloon backend did not exit with the guest"
            ) from error
        summary = SUMMARY.search(output)
        if server.returncode != 0 or summary is None:
            raise RuntimeError("the balloon backend failed")
        changes, reads = (int(value) for value in summary.groups())
        if changes < MINIMUM_CHANGES or reads < MINIMUM_CHANGES:
            raise RuntimeError(
                "too few config changes reached the driver: "
                f"{changes} raised and {reads} config reads answered"
            )
    except BaseException:
        if not output:
            try:
                output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
            except subprocess.TimeoutExpired:
                server.kill()
                output, _ = server.communicate()
        print(output.decode(errors="replace")[-4000:], file=sys.stderr)
        raise
    print(
        f"{changes} config changes acknowledged and {reads} config reads "
        "answered while the guest ran"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
