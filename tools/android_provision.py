# SPDX-License-Identifier: GPL-2.0-or-later
"""Provision an erased Android guest from adb, for harnesses.

The product starts a fresh guest in LineageOS Setup Wizard: until a person
completes it, `device_provisioned` and `user_setup_complete` stay 0, HOME
resolves to the wizard, and the status bar and quick settings stay restricted.
That is the product's behavior and stays so. A harness that erases a guest and
then measures it (cold launches that press HOME, idle memory, CTS runs) would
measure the wizard instead, so it calls `provision` once the guest has reached
`sys.boot_completed`. A harness that measures the first-boot experience itself
does not call it.

`provision` leaves the guest provisioned the way the wizard's last step does:
both settings are 1, and HOME resolves to the launcher. The wizard disables its
own HOME activity and flow manager when it finishes, which the shell may not do
to another package's components, so the helper disables the wizard package for
user 0 instead. It changes nothing in the image, and it changes nothing in a
guest that is already provisioned.

The helper takes a `shell` callable: it runs one command string in the guest's
shell and returns its standard output, so each harness keeps its own adb
transport (a serial, a private server, a forward).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shlex
import shutil
import subprocess
import sys
from typing import Callable

SETUP_WIZARD = "org.lineageos.setupwizard"
HOME_INTENT = "-a android.intent.action.MAIN -c android.intent.category.HOME"

Shell = Callable[[str], str]


class ProvisionError(RuntimeError):
    """The guest did not end up provisioned."""


def _setting(shell: Shell, namespace: str, name: str) -> str:
    return shell(f"settings get {namespace} {name}").strip()


def home_package(shell: Shell) -> str:
    """The package HOME resolves to, or an empty string when none does."""
    brief = shell(f"cmd package resolve-activity --brief {HOME_INTENT}")
    # The last line of the brief form is the component, package/class.
    lines = [line.strip() for line in brief.splitlines() if "/" in line]
    return lines[-1].split("/", 1)[0] if lines else ""


def is_provisioned(shell: Shell) -> bool:
    """Whether both settings are set and HOME is not the setup wizard."""
    return (
        _setting(shell, "global", "device_provisioned") == "1"
        and _setting(shell, "secure", "user_setup_complete") == "1"
        and home_package(shell) != SETUP_WIZARD
    )


def provision(shell: Shell) -> bool:
    """Provision the guest; whether it changed anything.

    Run it after `sys.boot_completed`. A guest that is already provisioned is
    left alone. Raises ProvisionError when the guest is still not provisioned
    after the commands, so a harness stops instead of measuring the wizard.
    """
    if is_provisioned(shell):
        return False
    shell("settings put global device_provisioned 1")
    shell("settings put secure user_setup_complete 1")
    shell(f"pm disable-user --user 0 {SETUP_WIZARD}")
    if not is_provisioned(shell):
        raise ProvisionError(
            "the guest is not provisioned after the commands: "
            f"device_provisioned={_setting(shell, 'global', 'device_provisioned')!r}, "
            f"user_setup_complete={_setting(shell, 'secure', 'user_setup_complete')!r}, "
            f"HOME={home_package(shell)!r}"
        )
    return True


def adb_shell(adb: str, serial: str, *, timeout: int = 60) -> Shell:
    """A shell callable over `adb -s serial shell`."""

    def run(command: str) -> str:
        result = subprocess.run(
            [adb, "-s", serial, "shell", command],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False,
        )
        return result.stdout.replace("\r", "")

    return run


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--serial", required=True, help="the guest's adb serial (host:port)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--timeout", type=int, default=60, help="seconds for each adb command")
    options = parser.parse_args(argv)
    adb = shutil.which(options.adb) or options.adb
    shell = adb_shell(adb, options.serial, timeout=options.timeout)
    try:
        changed = provision(shell)
    except (ProvisionError, subprocess.SubprocessError, OSError) as error:
        print(f"android_provision.py: {error}", file=sys.stderr)
        return 1
    print("provisioned the guest" if changed else "the guest was already provisioned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
