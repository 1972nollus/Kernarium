#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the guest harness boots its session through the per-run initramfs,
drives the console, and cannot leave an emulator running after it dies, nor
its staged guest files for longer than the next guest's start."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import io
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SUPPORT = pathlib.Path(__file__).resolve().parents[2] / "integration/support"
sys.path.insert(0, str(SUPPORT))
import guest as guest_support  # noqa: E402


# A stand-in for uml-darwin. Guest only builds an argument vector and starts the
# process, so a payload that ignores its arguments and waits exercises the real
# construction path without a kernel, a rootfs, or a boot.
STAND_IN = "#!/bin/sh\nexec sleep 3600\n"

# A stand-in for a runner, so the test can kill a harness the way a shell job,
# a CTest timeout or a Ctrl-C does: by signalling its whole process group.
HARNESS = "\n".join(
    (
        "import pathlib, sys, time",
        f"sys.path.insert(0, {str(SUPPORT)!r})",
        "import guest as guest_support",
        "emulator = pathlib.Path(sys.argv[1])",
        "guest = guest_support.Guest(",
        "    emulator=emulator,",
        "    rootfs=pathlib.Path('/nonexistent/rootfs.erofs'),",
        "    memory='4G',",
        "    workspace='tmpfs',",
        "    init_shell=True,",
        ")",
        "print(guest.process_id, flush=True)",
        "time.sleep(3600)",
    )
)


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def wait_until_gone(pid: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not process_alive(pid):
            return True
        time.sleep(0.05)
    return not process_alive(pid)


class AttachedGuestSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="uml-guest-harness-")
        self.addCleanup(directory.cleanup)
        self.emulator = pathlib.Path(directory.name, "emulator")
        self.emulator.write_text(STAND_IN, encoding="utf-8")
        self.emulator.chmod(0o755)
        # The temporary directory of this test's guests, which no other
        # process shares.
        self.temporary = pathlib.Path(directory.name, "tmp")
        self.temporary.mkdir()

    def private_temporary_directory(self) -> contextlib.AbstractContextManager:
        return mock.patch.object(tempfile, "tempdir", str(self.temporary))

    def test_the_emulator_joins_the_harness_process_group(self) -> None:
        # This is the whole mechanism: sharing the group is what makes a group
        # kill reach the guest. Started in a session of its own it would be
        # unreachable, and a harness killed outright would leave it running.
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        self.assertEqual(os.getpgid(guest.process_id), os.getpgid(0))

    def test_return_code_reports_the_attached_process_status(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexit 23\n", encoding="utf-8")
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)

        deadline = time.monotonic() + 5
        while guest.return_code is None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(guest.return_code, 23)
        with self.assertRaises(AttributeError):
            guest.return_code = 0

    def test_a_failing_session_prints_its_transcript_and_closes(self) -> None:
        # A shell that honours the harness's "sync; poweroff -f" at once. A
        # host sync can take seconds on a busy machine, so it is a no-op here.
        self.emulator.write_text(
            "#!/bin/sh\n"
            "sync() { :; }\n"
            "poweroff() { exit 0; }\n"
            'while IFS= read -r line; do eval "$line"; done\n',
            encoding="utf-8",
        )
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(RuntimeError):
            with guest_support.Guest(
                emulator=self.emulator,
                rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
                memory="4G",
                workspace="tmpfs",
                init_shell=True,
            ) as guest:
                guest.check_command("printf '%s' session-transcript")
                raise RuntimeError("the runner failed")
        self.assertIn("session-transcript", stderr.getvalue())
        self.assertEqual(guest.return_code, 0)

    def test_commands_use_pipe_input_and_terminal_output(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexec /bin/sh\n", encoding="utf-8")
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        for message in ("first command", "second command"):
            result = guest.run_command(
                f"test ! -t 0 && test -t 1 && printf '%s' '{message}'"
            )
            self.assertEqual(result.return_code, 0)
            self.assertEqual(result.output, message)
        guest._write("exit\n")
        guest._process.wait(timeout=5)

    def test_a_console_sink_receives_every_byte_the_harness_reads(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexec /bin/sh\n", encoding="utf-8")
        sink = io.BytesIO()
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
            console_sink=sink,
        )
        self.addCleanup(guest.close)
        result = guest.run_command("printf '%s' sink-marker")
        self.assertEqual(result.output, "sink-marker")
        self.assertIn(b"sink-marker", sink.getvalue())
        self.assertEqual(sink.getvalue(), bytes(guest._transcript))

    def test_a_timed_out_command_keeps_what_it_printed(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexec /bin/sh\n", encoding="utf-8")
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        with self.assertRaises(guest_support.CommandTimeout) as raised:
            guest.run_command("printf '%s' partial-output; sleep 2", 1)
        self.assertEqual(raised.exception.output, "partial-output")
        self.assertIn("exceeded 1s", str(raised.exception))
        guest._write("exit\n")
        guest._process.wait(timeout=10)

    def test_a_command_reads_the_input_that_follows_it(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexec /bin/sh\n", encoding="utf-8")
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        result = guest.run_command("head -n 2", stdin="first line\nsecond line\n")
        self.assertEqual(result.return_code, 0)
        self.assertEqual(result.output, "first line\nsecond line")
        follow = guest.run_command("printf '%s' after")
        self.assertEqual(follow.output, "after")

    def recording_emulator(self, then: str = "exec sleep 3600") -> pathlib.Path:
        """A stand-in that saves its arguments and each guest file it names."""
        record = self.emulator.parent / "record"
        record.mkdir()
        self.emulator.write_text(
            "#!/bin/sh\n"
            f"record={record}\n"
            'printf \'%s\\n\' "$@" > "$record/arguments"\n'
            "index=0\n"
            'for argument; do\n'
            '    case "$argument" in\n'
            "    --guest-file=*)\n"
            '        pair=${argument#--guest-file=}\n'
            '        cp "${pair#*=}" "$record/file-$index"\n'
            '        ls -l "${pair#*=}" | cut -c1-10 > "$record/mode-$index"\n'
            '        index=$((index + 1)) ;;\n'
            "    esac\n"
            "done\n"
            ": > \"$record/done\"\n"
            f"{then}\n",
            encoding="utf-8",
        )
        return record

    def wait_for(self, path: pathlib.Path) -> None:
        deadline = time.monotonic() + 10
        while not path.exists():
            self.assertLess(time.monotonic(), deadline, f"{path} never appeared")
            time.sleep(0.01)

    def test_the_launcher_receives_the_session_and_its_payloads(self) -> None:
        record = self.recording_emulator()
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/images/rootfs.erofs"),
            memory="2G",
            workspace="ext2",
            kernel_arguments=("ncpus=2",),
            swap_size="64M",
            overlay_shell=True,
            payloads={
                "/usr/local/libexec/uml-test/probe": guest_support.GuestFile(
                    b"probe bytes", 0o755
                ),
                "/etc/uml-test.conf": guest_support.GuestFile(b"setting=1\n"),
            },
        )
        self.addCleanup(guest.close)
        self.wait_for(record / "done")
        arguments = (record / "arguments").read_text().splitlines()
        guest_files = [
            argument.removeprefix("--guest-file=").split("=", 1)[0]
            for argument in arguments
            if argument.startswith("--guest-file=")
        ]
        self.assertEqual(
            guest_files,
            [
                "/boot.env",
                "/payload/usr/local/libexec/uml-session",
                "/payload/etc/uml-session.env",
                "/payload/etc/uml-test.conf",
                "/payload/usr/local/libexec/uml-test/probe",
            ],
        )
        rest = [a for a in arguments if not a.startswith("--guest-file=")]
        self.assertEqual(
            rest[:2], ["--execution-engine=jit", "ubd0c=/images/rootfs.erofs"]
        )
        self.assertTrue(rest[2].startswith("ubd1="))
        self.assertEqual(
            rest[3:], ["mem=2G", "ncpus=2", "console=tty0", "console=ttyS0"]
        )
        self.assertFalse(
            [a for a in rest if a.split("=", 1)[0] in ("root", "rootfstype", "init", "ip")],
            "the launcher's per-run initramfs owns the root and the init",
        )
        contents = [
            (record / f"file-{index}").read_bytes() for index in range(5)
        ]
        self.assertEqual(
            contents[0], b"UML_BOOT_ENTRY=/usr/local/libexec/uml-session\n"
        )
        self.assertEqual(contents[1], guest_support.SESSION_SCRIPT.read_bytes())
        self.assertEqual(
            contents[2].decode().splitlines(),
            [
                "UML_SESSION_ENTRY=/bin/sh",
                "UML_SESSION_RESOLVER=user-network",
                "UML_SESSION_RUNTIME_MOUNTS=1",
                "UML_SESSION_NETWORK=user-network",
                "UML_SESSION_WORKSPACE=ext2",
                "UML_SESSION_SWAP=/dev/ubdb",
            ],
        )
        self.assertEqual(contents[3:], [b"setting=1\n", b"probe bytes"])
        self.assertEqual(
            (record / "mode-4").read_text().strip(), "-rwxr-xr-x"
        )
        self.assertEqual(
            (record / "mode-3").read_text().strip(), "-rw-r--r--"
        )
        staging = pathlib.Path(
            next(a for a in arguments if a.startswith("--guest-file=/boot.env="))
            .split("=", 2)[2]
        ).parent
        self.assertTrue(staging.is_dir())
        guest.close()
        self.assertFalse(staging.exists(), "the staged guest files outlived the run")

    def test_an_ext4_root_and_no_guest_network_reach_the_launcher(self) -> None:
        record = self.recording_emulator()
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/images/rootfs.ext4"),
            memory="2G",
            workspace="tmpfs",
            overlay_shell=True,
            root_type="ext4",
            network="image",
        )
        self.addCleanup(guest.close)
        self.wait_for(record / "done")
        self.assertEqual(
            (record / "file-0").read_text().splitlines(),
            [
                "UML_BOOT_ENTRY=/usr/local/libexec/uml-session",
                "UML_BOOT_ROOT_TYPE=ext4",
            ],
        )
        self.assertEqual(
            (record / "file-2").read_text().splitlines(),
            [
                "UML_SESSION_ENTRY=/bin/sh",
                "UML_SESSION_RESOLVER=image",
                "UML_SESSION_RUNTIME_MOUNTS=1",
                "UML_SESSION_NETWORK=image",
                "UML_SESSION_WORKSPACE=tmpfs",
            ],
        )
        arguments = (record / "arguments").read_text().splitlines()
        self.assertIn("ubd0c=/images/rootfs.ext4", arguments)

    def test_root_and_network_options_are_validated(self) -> None:
        for options in (
            {"root_type": "ext3"},
            {"network": "bridge"},
        ):
            with (
                self.subTest(options=options),
                self.assertRaises(ValueError),
                self.private_temporary_directory(),
            ):
                guest_support.Guest(
                    emulator=self.emulator,
                    rootfs=pathlib.Path("/images/rootfs.erofs"),
                    memory="2G",
                    workspace="tmpfs",
                    overlay_shell=True,
                    **options,
                )
        self.assertEqual(list(self.temporary.iterdir()), [])

    def test_a_bare_shell_session_names_only_its_entry(self) -> None:
        self.assertEqual(
            guest_support.session_parameters(
                overlay_shell=False,
                workspace="tmpfs",
                swap=False,
                cgroup1="cpu",
                cgroup2_nsdelegate=False,
            ),
            "UML_SESSION_ENTRY=/bin/sh\nUML_SESSION_CGROUP=v1:cpu\n",
        )
        with self.assertRaises(ValueError):
            guest_support.session_parameters(
                overlay_shell=False,
                workspace="ext2",
                swap=False,
                cgroup1=None,
                cgroup2_nsdelegate=False,
            )

    def test_payloads_the_session_would_hide_are_rejected(self) -> None:
        record = self.recording_emulator()
        for path in (
            "/tmp/probe",
            "/run/probe",
            "/dev/probe",
            "relative/probe",
            "/usr/../probe",
            "/usr//probe",
            "/usr/probe=1",
            guest_support.SESSION_GUEST_PATH,
        ):
            with (
                self.subTest(path=path),
                self.assertRaises(ValueError),
                self.private_temporary_directory(),
            ):
                guest_support.Guest(
                    emulator=self.emulator,
                    rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
                    memory="4G",
                    workspace="tmpfs",
                    init_shell=True,
                    payloads={path: guest_support.GuestFile(b"")},
                )
        self.assertFalse((record / "arguments").exists())
        self.assertEqual(list(self.temporary.iterdir()), [])

    def test_boot_waits_for_the_session_before_the_prompt(self) -> None:
        # A prompt-shaped line before the session reports ready, such as one
        # from the ramdisk, must not count as the harness shell.
        self.emulator.write_text(
            "#!/bin/sh\n"
            "printf '# \\n'\n"
            "sleep 0.5\n"
            "printf 'UML_SESSION_READY:/bin/sh\\n# '\n"
            "exec /bin/sh\n",
            encoding="utf-8",
        )
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        guest.boot_to_shell(10)
        self.assertIn("UML_SESSION_READY:/bin/sh", guest.output())
        self.assertEqual(guest.run_command("printf '%s' ready").output, "ready")

    def test_a_session_failure_ends_the_boot_with_its_reason(self) -> None:
        self.emulator.write_text(
            "#!/bin/sh\nprintf 'UML_SESSION_ERROR:cannot mount /run\\n'\nexit 1\n",
            encoding="utf-8",
        )
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        with self.assertRaisesRegex(RuntimeError, "guest exited with status 1"):
            guest.boot_to_shell(10)
        self.assertIn("UML_SESSION_ERROR:cannot mount /run", guest.output())

    def start_harness(self) -> tuple[subprocess.Popen[str], int]:
        """Start HARNESS in a process group of its own and return it with the
        process ID of its guest. Its guest files go to this test's temporary
        directory, since a harness killed outright cannot remove them."""
        harness = subprocess.Popen(
            [sys.executable, "-c", HARNESS, str(self.emulator)],
            stdout=subprocess.PIPE,
            text=True,
            start_new_session=True,
            env={**os.environ, "TMPDIR": str(self.temporary)},
        )
        self.addCleanup(self.reap, harness)
        assert harness.stdout is not None
        emulator_pid = int(harness.stdout.readline().strip())
        self.addCleanup(self.reap_emulator, emulator_pid)
        return harness, emulator_pid

    def test_a_killed_harness_group_takes_its_guest_with_it(self) -> None:
        harness, emulator_pid = self.start_harness()
        self.assertTrue(process_alive(emulator_pid))

        os.killpg(harness.pid, signal.SIGKILL)
        harness.wait()
        self.assertTrue(
            wait_until_gone(emulator_pid, 30),
            "the guest outlived its harness group; an orphan boots to idle and "
            "still burns host cores, quietly oversubscribing later benchmarks",
        )

    def test_the_next_guest_removes_a_killed_harness_guest_files(self) -> None:
        harness, _ = self.start_harness()
        abandoned = list(self.temporary.glob(f"uml-guest-files-{harness.pid}-*"))
        self.assertEqual(len(abandoned), 1)
        # Staged files of a process that is still running are kept.
        live = self.temporary / f"uml-guest-files-{os.getpid()}-live"
        live.mkdir()

        os.killpg(harness.pid, signal.SIGKILL)
        harness.wait()
        self.assertTrue(abandoned[0].is_dir())
        with self.private_temporary_directory():
            guest = guest_support.Guest(
                emulator=self.emulator,
                rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
                memory="4G",
                workspace="tmpfs",
                init_shell=True,
            )
        self.addCleanup(guest.close)
        self.assertFalse(
            abandoned[0].exists(),
            "a killed harness's guest files outlived the next guest's start",
        )
        self.assertTrue(live.is_dir())

    def test_a_killed_emulator_leaves_no_run_directories(self) -> None:
        # An emulator that ignores SIGTERM, as a wedged one does, so the
        # harness ends it with SIGKILL and no handler of its own runs.
        self.emulator.write_text(
            "#!/bin/sh\n"
            "trap '' TERM\n"
            'mkdir "$TMPDIR/uml-kern$$" "$TMPDIR/uml-guest-initrd-$$-x"\n'
            'touch "$TMPDIR/uml-kern$$/mconsole"\n'
            "echo made\n"
            "exec sleep 3600\n",
            encoding="utf-8",
        )
        # Another run's directories are that run's, and a later emulator's.
        exited = subprocess.Popen(["/usr/bin/true"])
        exited.wait()
        others = [
            self.temporary / f"uml-kern{os.getpid()}",
            self.temporary / f"uml-kern{exited.pid}",
        ]
        for directory in others:
            directory.mkdir()
        environment = {**os.environ, "TMPDIR": str(self.temporary)}
        with mock.patch.dict(os.environ, environment, clear=True):
            with self.private_temporary_directory():
                guest = guest_support.Guest(
                    emulator=self.emulator,
                    rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
                    memory="4G",
                    workspace="tmpfs",
                    init_shell=True,
                )
                self.addCleanup(guest.close)
                guest._read_until(re.compile(rb"made"), 30)
                pid = guest.process_id
                made = sorted(path.name for path in self.temporary.iterdir())
                self.assertIn(f"uml-kern{pid}", made)
                guest.close()
        self.assertEqual(guest.return_code, -signal.SIGKILL)
        left = sorted(
            path.name
            for path in self.temporary.iterdir()
            if not path.name.startswith("uml-guest-files-")
        )
        self.assertEqual(left, sorted(path.name for path in others))

    def test_swap_images_of_exited_processes_are_removed(self) -> None:
        finished = subprocess.Popen(["/usr/bin/true"])
        finished.wait()
        abandoned = self.temporary / f"uml-ltp-swap-{finished.pid}-a.img"
        live = self.temporary / f"uml-ltp-swap-{os.getpid()}-b.img"
        abandoned.touch()
        live.touch()
        with self.private_temporary_directory():
            guest_support.cleanup_stale_swap_images()
        self.assertFalse(abandoned.exists())
        self.assertTrue(live.exists())

    def reap(self, harness: subprocess.Popen[str]) -> None:
        if harness.poll() is None:
            harness.kill()
        harness.wait()
        if harness.stdout is not None:
            harness.stdout.close()

    def reap_emulator(self, emulator_pid: int) -> None:
        try:
            os.kill(emulator_pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


if __name__ == "__main__":
    unittest.main()
