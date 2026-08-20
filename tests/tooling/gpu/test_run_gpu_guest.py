#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import io
from pathlib import Path
import socket
import sys
import tarfile
import tempfile
import threading
import time
import unittest

GPU_INTEGRATION_ROOT = Path(__file__).resolve().parents[2] / "integration/gpu"
INTEGRATION_CMAKE = Path(__file__).resolve().parents[2] / "integration/CMakeLists.txt"
sys.path.insert(0, str(GPU_INTEGRATION_ROOT))
import run_gpu_guest  # noqa: E402


class GpuGuestSafetyTests(unittest.TestCase):
    def test_command_uses_the_raw_erofs_root(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.erofs"),
            "/dev/ubda",
            "8G",
            "multi-user.target",
            ["quiet"],
        )
        resolved_image = Path("/tmp/rootfs.erofs").resolve()
        self.assertIn(f"ubd0c={resolved_image}", command)
        self.assertIn("root=/dev/ubda", command)
        self.assertIn("rootfstype=erofs", command)
        self.assertIn("console=tty0", command)
        self.assertIn("console=ttyS0", command)
        self.assertNotIn(f"ubd0r={resolved_image}", command)

    def test_conflicting_arguments_are_rejected(self) -> None:
        forbidden = (
            "ubd0=/tmp/other.erofs",
            "ubd0c=/tmp/other.erofs",
            "ubd0r=/tmp/other.erofs",
            "root=/dev/ubdb",
            "rootfstype=ext4",
            "init=/sbin/init",
            "mem=2G",
            "console=ttyS0",
            "systemd.unit=graphical.target",
        )
        for argument in forbidden:
            with self.subTest(argument=argument):
                with self.assertRaises(run_gpu_guest.SafetyError):
                    run_gpu_guest.validate_extra_arguments([argument])

    def test_bootstrap_installs_overlay_guard_before_systemd(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap(
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True)
        ).decode()
        docker_tmpfs = (
            "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker"
        )
        containerd_tmpfs = (
            "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd"
        )
        self.assertIn("mount -t overlay overlay", bootstrap)
        self.assertIn("pivot_root . .uml-lower", bootstrap)
        self.assertIn("mkdir -p /var/lib/docker", bootstrap)
        self.assertIn(docker_tmpfs, bootstrap)
        self.assertIn("mkdir -p /var/lib/containerd", bootstrap)
        self.assertIn(containerd_tmpfs, bootstrap)
        self.assertIn(
            'findmnt -n -o FSTYPE -T /var/lib/docker)" tmpfs',
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True),
        )
        self.assertIn(
            'findmnt -n -o FSTYPE -T /var/lib/containerd)" tmpfs',
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True),
        )
        self.assertNotIn("getty@tty0.service", bootstrap)
        self.assertNotIn("agetty --autologin", bootstrap)
        self.assertNotIn("useradd", bootstrap)
        self.assertNotIn("groupadd", bootstrap)
        self.assertIn("uml-gpu-overlay-test.service", bootstrap)
        self.assertIn("exec /lib/systemd/systemd", bootstrap)
        self.assertLess(
            bootstrap.index("pivot_root . .uml-lower"),
            bootstrap.index(docker_tmpfs),
        )
        self.assertLess(
            bootstrap.index(docker_tmpfs),
            bootstrap.index(containerd_tmpfs),
        )
        self.assertLess(
            bootstrap.index(containerd_tmpfs),
            bootstrap.index("exec /lib/systemd/systemd"),
        )

    def test_offline_docker_smoke_is_local_and_fail_closed(self) -> None:
        payload = b"\x7fELFtest-payload"
        script = run_gpu_guest.make_test_script(
            ["true"],
            auto_poweroff=True,
            docker_hello_world=payload,
        )
        self.assertIn("docker import", script)
        self.assertIn("docker image inspect", script)
        self.assertIn("--pull=never --network none", script)
        self.assertIn("grep -Fxq 'Hello from Docker!'", script)
        self.assertIn('cat "$docker_hello_world_output"', script)
        self.assertIn("trap docker_hello_world_cleanup EXIT HUP INT TERM", script)
        self.assertIn(
            'timeout 10 docker rm -f \\\n'
            '        "$docker_hello_world_container"',
            script,
        )
        self.assertIn(
            'timeout 10 docker image rm -f \\\n'
            '        "$docker_hello_world_image"',
            script,
        )
        self.assertNotIn("docker pull", script)
        self.assertNotIn("docker run --rm hello-world", script)
        run_start = script.index("if ! timeout 30 docker run")
        grep_start = script.index("grep -Fxq 'Hello from Docker!'")
        run_block = script[run_start:grep_start]
        self.assertNotIn("|", run_block)
        self.assertIn('> "$docker_hello_world_output" 2>&1', run_block)
        self.assertLessEqual(max(map(len, script.splitlines())), 600)

        archive_bytes = run_gpu_guest.make_docker_rootfs_archive(payload)
        with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:") as archive:
            member = archive.getmember("hello")
            self.assertEqual(member.mode, 0o755)
            self.assertEqual(member.uid, 0)
            self.assertEqual(member.gid, 0)
            extracted = archive.extractfile(member)
            self.assertIsNotNone(extracted)
            assert extracted is not None
            self.assertEqual(extracted.read(), payload)

    def test_docker_payload_loader_rejects_non_elf_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "hello"
            payload.write_bytes(b"not an ELF")
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "is not an ELF file"
            ):
                run_gpu_guest.load_docker_hello_world(payload)

    def test_integration_graph_builds_and_passes_the_local_payload(self) -> None:
        source = INTEGRATION_CMAKE.read_text(encoding="utf-8")
        self.assertIn(
            "add_dependencies(integration-test-binaries uml-docker-hello-world)",
            source,
        )
        self.assertIn(
            "--docker-hello-world-binary ${UML_DOCKER_HELLO_WORLD}",
            source,
        )
        # Ordinary acceptance must never reach a registry; the offline smoke
        # imports a locally built payload instead. A registry pull is allowed
        # only behind the external-network opt-in, so scope the prohibition to
        # the unconditional part of the graph. Checking the whole file would
        # also forbid the sanctioned opt-in campaign, and checking nothing
        # would let a pull drift back into the default suite.
        guard = "if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)"
        self.assertIn(guard, source)
        unconditional, opt_in = source.split(guard, 1)
        self.assertNotIn("docker run", unconditional)
        self.assertNotIn("docker pull", unconditional)
        self.assertIn("docker run --rm hello-world", opt_in)

    def test_bootstrap_chunks_large_test_scripts_for_the_init_tty(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap("x" * 16_384).decode()
        self.assertLessEqual(max(map(len, bootstrap.splitlines())), 600)

    def test_test_script_stops_at_the_first_failed_command(self) -> None:
        script = run_gpu_guest.make_test_script(
            ["false", "true"], auto_poweroff=True
        )
        self.assertIn("    (\n        set -e\nfalse\ntrue\n    )", script)

    def test_console_short_writes_are_retried(self) -> None:
        class ShortWriter(io.BytesIO):
            def write(self, payload: bytes) -> int:
                return super().write(payload[:3])

        stream = ShortWriter()
        pending = bytearray(b"0123456789")
        run_gpu_guest.write_pending(stream, pending)
        self.assertEqual(pending, b"")
        self.assertEqual(stream.getvalue(), b"0123456789")

    def test_serial_socket_relays_console_output_and_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            serial_path = root / "serial.sock"
            received = bytearray()
            client_errors: list[BaseException] = []

            def attach() -> None:
                try:
                    deadline = time.monotonic() + 5
                    while not serial_path.exists():
                        if time.monotonic() >= deadline:
                            raise TimeoutError("serial socket was not created")
                        time.sleep(0.01)

                    def receive_until(
                        client: socket.socket,
                        output: bytearray,
                        marker: bytes,
                    ) -> None:
                        while marker not in output:
                            chunk = client.recv(4096)
                            if not chunk:
                                raise RuntimeError(
                                    "serial console closed before expected output"
                                )
                            output.extend(chunk)

                    first = bytearray()
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(5)
                        client.connect(str(serial_path))
                        receive_until(client, first, b"READY\n")

                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(5)
                        client.connect(str(serial_path))
                        receive_until(client, received, b"READY\n")
                        client.sendall(b"status\n")
                        receive_until(client, received, b"ECHO:status\n")
                except BaseException as error:
                    client_errors.append(error)

            fake_emulator = (
                "import sys,time;"
                "sys.stdout.write('\\n# ');sys.stdout.flush();"
                "sys.stdin.buffer.readline();"
                f"sys.stdout.write({run_gpu_guest.PASS_MARKER!r}+"
                "'\\nTTY:'+str(int(sys.stdin.isatty()))+'\\nREADY\\n');"
                "sys.stdout.flush();"
                "line=sys.stdin.buffer.readline();"
                "sys.stdout.buffer.write(b'ECHO:'+line);"
                "sys.stdout.buffer.flush();"
                "time.sleep(0.2)"
            )
            client_thread = threading.Thread(target=attach)
            client_thread.start()
            return_code, saw_pass, saw_fail = run_gpu_guest.run_guest(
                [sys.executable, "-c", fake_emulator],
                b"BOOT",
                root / "console.log",
                timeout=10,
                expect_poweroff=False,
                serial_socket=serial_path,
            )
            client_thread.join(timeout=5)
            self.assertFalse(client_thread.is_alive())
            self.assertEqual(client_errors, [])
            self.assertEqual(return_code, 0)
            self.assertTrue(saw_pass)
            self.assertFalse(saw_fail)
            self.assertIn(b"TTY:1\n", received)
            self.assertIn(b"ECHO:status\n", received)
            self.assertFalse(serial_path.exists())

    def test_serial_socket_refuses_to_replace_an_existing_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial.sock"
            path.write_text("keep", encoding="utf-8")
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "already exists"
            ):
                run_gpu_guest.SerialConsoleSocket(path)
            self.assertEqual(path.read_text(encoding="utf-8"), "keep")

    def test_serial_socket_does_not_remove_a_replacement_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial.sock"
            serial = run_gpu_guest.SerialConsoleSocket(path)
            path.unlink()
            path.write_text("replacement", encoding="utf-8")
            serial.close()
            self.assertEqual(path.read_text(encoding="utf-8"), "replacement")


if __name__ == "__main__":
    unittest.main()
