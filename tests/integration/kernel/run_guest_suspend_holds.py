#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A suspended guest stays suspended until one of its wake sources fires.

The management console's ``suspend`` puts the guest into full system
suspend, and its ``wake`` request, arriving on the console's socket, is
what ends it. Every other descriptor the guest reads stays quiet while it
is suspended, as an O_ASYNC descriptor whose flag the suspend cleared does
on a Linux host: data that arrives on it waits for the resume.

Each arm suspends a two-vCPU guest and keeps one kind of descriptor busy
for HOLD_SECONDS: a host client writing a byte every 250 ms through a
network forward into a guest echo server, a line of console input every
250 ms, or a vhost-user backend (the balloon churn server) raising a config
change on its backend channel every 50 ms. The guest must not leave suspend
until the runner sends ``wake``, then must resume promptly and handle what
arrived while it slept: every forwarded byte is echoed, the console input
runs, and the balloon keeps taking config changes.

A ``wake`` that arrives while the suspend is still being entered, before
it has stopped interrupts, is answered at once instead of waiting on the
console. The early-wake arm sends one as soon as the guest prints its
suspend entry line, and the guest must not stay suspended.

A guest can also suspend itself, as Android does once its display is off.
The self-wake arm checks that a ``wake``, which the host sends when its
user presses a power key, only answers a running guest, ends the guest's
own suspend once it holds, and ends it while it is still being entered:
eight threads in long reads keep the freezer waiting for most of a second,
as a busy Android does, and the wake must make the PM core give up.
The power arm forwards a power key through the VNC display to a guest that
suspended itself: the key must wake it promptly and reach it exactly once.
The contract is the suspend paragraph of the mainline kernel README.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
HOLD_SECONDS = 6.0
POKE_SECONDS = 0.25
REQUEST_SECONDS = 60.0
RESUME_SECONDS = 30.0
GUEST_PORT = 7000
BALLOON_DEVICE = 5
BALLOON_PERIOD_MS = 50
SERVER_EXIT_SECONDS = 20
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
SUSPEND_ENTRY = "PM: suspend entry"
SUSPEND_EXIT = "PM: suspend exit"
# Printed once the PM core has begun preparing the suspend, so a wake sent
# after it reaches a suspend in progress.
SUSPEND_FREEZING = "Freezing user space processes"
SELF_SUSPEND = "(echo mem > /sys/power/state) &\n"
HOLDER = "/opt/uml-test/hold_freeze.py"
HOLDER_READERS = 8
# Suspends the guest while HOLDER_READERS threads are each in one read of
# 2 GiB of holes from tmpfs, which no signal interrupts, so the freezer
# waits for them; on two vCPUs that keeps the suspend being entered for
# most of a second. Prints whether the suspend ran or was refused.
HOLDER_SOURCE = """\
import os, sys, threading
size = 2 << 20
descriptor = os.open("/run/hold-freeze", os.O_RDWR | os.O_CREAT | os.O_TRUNC)
os.ftruncate(descriptor, 1023 * size)
views = [memoryview(bytearray(size))] * 1023
readers = int(sys.argv[1])
start = threading.Barrier(readers + 1)
done = threading.Event()

def read():
    start.wait()
    while not done.is_set():
        os.preadv(descriptor, views, 0)

for _ in range(readers):
    threading.Thread(target=read, daemon=True).start()
start.wait()
state = os.open("/sys/power/state", os.O_WRONLY)
try:
    os.write(state, b"mem")
    result = "suspended"
except OSError as error:
    result = f"refused: {os.strerror(error.errno)}"
os.close(state)
done.set()
print(f"hold-freeze: {result}", flush=True)
"""
HOLDER_REFUSED = "hold-freeze: refused"
VNC_HANDSHAKE_SECONDS = 30
XF86_POWER_OFF = 0x1008FF2A
KEY_READER = "/opt/uml-test/power_keys.py"
KEY_LOG = "/run/power-keys"
# Logs the value of every KEY_POWER event the VNC keyboard delivers.
KEY_READER_SOURCE = f"""\
import glob, os, struct
names = glob.glob("/sys/class/input/event*/device/name")
device = next(
    "/dev/input/" + path.split("/")[4]
    for path in names
    if open(path).read().strip() == "Darwin UML VNC Keyboard"
)
descriptor = os.open(device, os.O_RDONLY)
layout = struct.Struct("llHHi")
with open("{KEY_LOG}", "w", buffering=1) as log:
    log.write("ready\\n")
    while True:
        data = os.read(descriptor, layout.size * 64)
        for offset in range(0, len(data) - layout.size + 1, layout.size):
            _, _, kind, code, value = layout.unpack_from(data, offset)
            if kind == 1 and code == 116:
                log.write(f"{{value}}\\n")
"""
ECHO = "/opt/uml-test/suspend_echo.py"
ECHO_SERVER = f"""\
import socket
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("0.0.0.0", {GUEST_PORT}))
listener.listen(1)
while True:
    connection, _ = listener.accept()
    while True:
        data = connection.recv(4096)
        if not data:
            break
        connection.sendall(data)
    connection.close()
"""
# The console input's echo shows the format; only its output joins the words.
CONSOLE_LINE = "printf 'suspend-%s-%s\\n' console delivered\n"
CONSOLE_OUTPUT = "suspend-console-delivered"
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|rcu: INFO"
)
BALLOON_SUMMARY = re.compile(
    rb"balloon churn: (\d+) config changes raised, (\d+) config reads answered"
)


class Request:
    """One management-console request, answered at a socket of its own."""

    def __init__(self, directory: pathlib.Path, command: str, name: str) -> None:
        console = directory / "mconsole"
        if not console.is_socket():
            raise RuntimeError(f"the guest's management console is not {console}")
        self.command = command
        self._reply = directory / f"mc-suspend-{name}"
        self._reply.unlink(missing_ok=True)
        self._endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self._endpoint.bind(str(self._reply))
            data = command.encode()
            self._endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(console),
            )
            self._endpoint.setblocking(False)
        except BaseException:
            self.close()
            raise

    def answer(self) -> tuple[int, str] | None:
        """The reply's error flag and text, or None while there is none."""
        try:
            data = self._endpoint.recv(12 + MCONSOLE_DATA)
        except BlockingIOError:
            return None
        error, _, length = struct.unpack_from("<III", data)
        return error, data[12:12 + length].decode(errors="replace").rstrip("\0")

    def close(self) -> None:
        self._endpoint.close()
        self._reply.unlink(missing_ok=True)


class ManagementConsole:
    """Requests to the guest's management console that keep its serial
    console drained while they wait, so console output never stalls it."""

    def __init__(self, guest: Guest, private: pathlib.Path) -> None:
        self._guest = guest
        self._directory = private / f"uml-kern{guest.process_id}"
        self._sent = 0

    def send(self, command: str) -> Request:
        self._sent += 1
        return Request(self._directory, command, str(self._sent))

    def collect(self, request: Request) -> tuple[int, str]:
        deadline = time.monotonic() + REQUEST_SECONDS
        while (answer := request.answer()) is None:
            if time.monotonic() > deadline:
                raise RuntimeError(
                    f"mconsole {request.command}: no reply in {REQUEST_SECONDS:g} s"
                )
            drain(self._guest, 0.05)
        return answer

    def request(self, command: str) -> str:
        request = self.send(command)
        try:
            error, text = self.collect(request)
        finally:
            request.close()
        if error:
            raise RuntimeError(f"mconsole {command} failed: {text!r}")
        return text


def drain(guest: Guest, seconds: float) -> None:
    if guest.wait_for_exit(seconds):
        raise RuntimeError(
            f"the guest exited with status {guest.return_code}:\n"
            + guest.output()[-4000:]
        )


def hold(guest: Guest, console: ManagementConsole, arm: str, poke) -> None:
    """Suspend, keep a non-wake descriptor busy, then wake.

    The guest prints SUSPEND_EXIT as it resumes, so its appearance before
    the wake request is a guest that left suspend by itself."""
    exits = guest.output().count(SUSPEND_EXIT)
    request = len(guest.output())
    answer = console.request("suspend")
    if "suspended" not in answer:
        raise RuntimeError(f"{arm}: suspend replied {answer!r}")
    entered = time.monotonic()
    while time.monotonic() - entered < HOLD_SECONDS:
        poke()
        drain(guest, POKE_SECONDS)
        if guest.output().count(SUSPEND_EXIT) != exits:
            raise RuntimeError(
                f"{arm}: the guest left suspend "
                f"{time.monotonic() - entered:.2f} s after entering it, "
                "with no wake request:\n" + guest.output()[request:][-4000:]
            )
    woken = time.monotonic()
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"{arm}: wake replied {answer!r}")
    while guest.output().count(SUSPEND_EXIT) == exits:
        if time.monotonic() - woken > RESUME_SECONDS:
            raise RuntimeError(
                f"{arm}: the guest did not resume within {RESUME_SECONDS:g} s "
                "of its wake request:\n" + guest.output()[request:][-4000:]
            )
        drain(guest, POKE_SECONDS)
    print(
        f"{arm}: suspended {woken - entered:.1f} s under traffic, resumed "
        f"{time.monotonic() - woken:.2f} s after the wake request"
    )


def require_clean(guest: Guest, arm: str, request: int) -> None:
    tail = guest.output()[request:]
    failure = FAILURE.search(tail)
    if failure is not None:
        raise RuntimeError(
            f"{arm}: the guest printed {failure[0]!r}:\n"
            + tail[max(0, failure.start() - 2000):][:6000]
        )


def wait_for_line(guest: Guest, line: str, count: int, what: str) -> None:
    """Waits until the transcript holds more than ``count`` of ``line``."""
    deadline = time.monotonic() + REQUEST_SECONDS
    while guest.output().count(line) <= count:
        if time.monotonic() > deadline:
            raise RuntimeError(f"{what}: the guest never printed {line!r}")
        drain(guest, 0.01)


def wait_for_exit_line(guest: Guest, exits: int, what: str) -> float:
    """Waits for the guest to leave suspend; returns how long that took."""
    started = time.monotonic()
    while guest.output().count(SUSPEND_EXIT) == exits:
        if time.monotonic() - started > RESUME_SECONDS:
            raise RuntimeError(
                f"{what}:\n" + guest.output()[-4000:]
            )
        drain(guest, 0.05)
    return time.monotonic() - started


def hold_quietly(guest: Guest, exits: int, seconds: float, what: str) -> None:
    """Requires a suspended guest to stay suspended for ``seconds``."""
    started = time.monotonic()
    while time.monotonic() - started < seconds:
        drain(guest, POKE_SECONDS)
        if guest.output().count(SUSPEND_EXIT) != exits:
            raise RuntimeError(
                f"{what}: the guest left its own suspend "
                f"{time.monotonic() - started:.2f} s after entering it, with "
                "no wake:\n" + guest.output()[-4000:]
            )


class VncKeyboard:
    """The smallest RFB client that can type: version 3.8, no security."""

    def __init__(self, guest: Guest, port: int) -> None:
        deadline = time.monotonic() + VNC_HANDSHAKE_SECONDS
        while True:
            try:
                self._socket = socket.create_connection(
                    ("127.0.0.1", port), timeout=10
                )
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError("power: the VNC display never listened")
                drain(guest, 0.2)
        try:
            if not self._receive(12).startswith(b"RFB "):
                raise RuntimeError("power: the VNC display is not RFB")
            self._socket.sendall(b"RFB 003.008\n")
            offered = self._receive(self._receive(1)[0])
            if 1 not in offered:
                raise RuntimeError(f"power: VNC security types {list(offered)}")
            self._socket.sendall(b"\x01")
            if struct.unpack(">I", self._receive(4))[0] != 0:
                raise RuntimeError("power: the VNC display refused the client")
            self._socket.sendall(b"\x01")
            server_init = self._receive(24)
            self._receive(struct.unpack(">I", server_init[20:24])[0])
        except BaseException:
            self._socket.close()
            raise

    def _receive(self, size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = self._socket.recv(size - len(data))
            if not chunk:
                raise RuntimeError("power: the VNC display closed the client")
            data += chunk
        return data

    def type_key(self, symbol: int) -> None:
        for down in (1, 0):
            self._socket.sendall(struct.pack(">BBxxI", 4, down, symbol))

    def close(self) -> None:
        self._socket.close()


def unused_loopback_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def connect_echo(guest: Guest, port: int) -> socket.socket:
    """A connection through the forward whose echo already works."""
    deadline = time.monotonic() + 30
    while True:
        connection = socket.create_connection(("127.0.0.1", port), timeout=10)
        try:
            connection.settimeout(2)
            connection.sendall(b"p")
            if connection.recv(1) == b"p":
                connection.settimeout(None)
                return connection
        except OSError:
            pass
        connection.close()
        if time.monotonic() > deadline:
            raise RuntimeError("the guest echo server never answered")
        drain(guest, 0.5)


def read_echo(guest: Guest, connection: socket.socket, count: int) -> None:
    connection.setblocking(False)
    received = b""
    deadline = time.monotonic() + RESUME_SECONDS
    while len(received) < count:
        try:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError("the echo connection closed after the resume")
            received += chunk
            continue
        except BlockingIOError:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"network: {len(received)} of the {count} bytes sent during the "
                "suspend were echoed after it"
            )
        drain(guest, 0.1)
    if received != b"x" * count:
        raise RuntimeError(f"network: the echo returned {received!r}")


def run_network(arguments, private: pathlib.Path) -> None:
    port = unused_loopback_port()
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", f"--net-forward=tcp:127.0.0.1:{port}-{GUEST_PORT}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={ECHO: GuestFile(ECHO_SERVER.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        guest.check_command(
            f"(python3 {ECHO} >/run/suspend-echo.log 2>&1 </dev/null &)", 30
        )
        connection = connect_echo(guest, port)
        sent = 0

        def send() -> None:
            nonlocal sent
            connection.sendall(b"x")
            sent += 1

        hold(guest, console, "network", send)
        read_echo(guest, connection, sent)
        print(f"network: all {sent} bytes sent during the suspend were echoed")
        connection.close()
        require_clean(guest, "network", request)
        if not guest.shutdown(60):
            raise RuntimeError("network: the guest did not power off")


def run_console(arguments, private: pathlib.Path) -> None:
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        lines = 0

        def type_line() -> None:
            nonlocal lines
            guest._write(CONSOLE_LINE if lines == 0 else ":\n")
            lines += 1

        hold(guest, console, "console", type_line)
        output = guest.check_command("echo console-after-resume", 60)
        if CONSOLE_OUTPUT not in guest.output()[request:]:
            raise RuntimeError(
                "console: the line typed during the suspend did not run after "
                f"it:\n{output}"
            )
        print(f"console: the {lines} lines typed during the suspend ran after it")
        require_clean(guest, "console", request)
        if not guest.shutdown(60):
            raise RuntimeError("console: the guest did not power off")


def run_vhost_user(arguments, private: pathlib.Path) -> None:
    backend_end, guest_end = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    server = subprocess.Popen(
        [
            str(arguments.server.resolve()),
            "--fd", str(backend_end.fileno()),
            "--period-ms", str(BALLOON_PERIOD_MS),
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
                arguments.emulator,
                arguments.rootfs,
                "512M",
                "tmpfs",
                (
                    f"ncpus={VCPUS}",
                    f"virtio_uml.device=fd={guest_end.fileno()}:{BALLOON_DEVICE}",
                ),
                init_shell=True,
                engine=arguments.engine,
                pass_fds=(guest_end.fileno(),),
            )
        finally:
            guest_end.close()
        with guest:
            guest.boot_to_shell(180)
            console = ManagementConsole(guest, private)
            request = len(guest.output())
            hold(guest, console, "vhost-user", lambda: None)
            guest.check_command("sleep 1; echo balloon-after-resume", 60)
            require_clean(guest, "vhost-user", request)
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off")
        output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
        summary = BALLOON_SUMMARY.search(output)
        if server.returncode != 0 or summary is None:
            raise RuntimeError("the balloon backend failed")
        changes, reads = (int(value) for value in summary.groups())
        print(
            f"vhost-user: {changes} balloon config changes raised and {reads} "
            "config reads answered across the suspend"
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


def run_early_wake(arguments, private: pathlib.Path) -> None:
    """A wake sent as soon as the suspend starts, while the console still
    answers it, must not leave the guest suspended."""
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        entries = guest.output().count(SUSPEND_ENTRY)
        exits = guest.output().count(SUSPEND_EXIT)
        suspend = console.send("suspend")
        try:
            deadline = time.monotonic() + REQUEST_SECONDS
            while guest.output().count(SUSPEND_ENTRY) == entries:
                if time.monotonic() > deadline:
                    raise RuntimeError("early-wake: the suspend never began")
                drain(guest, 0.01)
            answer = console.request("wake")
            if "running" not in answer:
                raise RuntimeError(f"early-wake: wake replied {answer!r}")
            woken = time.monotonic()
            # pm_suspend() prints its exit line however the suspend ends.
            # Nothing else reaches the guest meanwhile: console input would
            # end a suspend by itself on a kernel without the O_ASYNC fix.
            while guest.output().count(SUSPEND_EXIT) == exits:
                if time.monotonic() - woken > RESUME_SECONDS:
                    raise RuntimeError(
                        "early-wake: the guest stayed suspended after a wake "
                        "sent while it was entering the suspend:\n"
                        + guest.output()[request:][-4000:]
                    )
                drain(guest, POKE_SECONDS)
            running = time.monotonic() - woken
            _, suspended = console.collect(suspend)
        finally:
            suspend.close()
        guest.check_command("echo early-wake-running", 60)
        print(
            f"early-wake: the suspend ended {running:.2f} s after a wake sent "
            f"as it began; it replied {suspended!r}"
        )
        require_clean(guest, "early-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("early-wake: the guest did not power off")


def run_self_wake(arguments, private: pathlib.Path) -> None:
    """The power key's wake: an answer to a running guest, and the end of a
    suspend the guest entered or is still entering by itself."""
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={HOLDER: GuestFile(HOLDER_SOURCE.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: a running guest replied {answer!r}")

        # Nothing of that answer lingers: the guest's next suspend holds.
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        guest._write(SELF_SUSPEND)
        wait_for_line(guest, SUSPEND_ENTRY, entries, "self-wake")
        hold_quietly(guest, exits, HOLD_SECONDS, "self-wake")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: wake replied {answer!r}")
        held = wait_for_exit_line(
            guest, exits,
            "self-wake: the guest stayed in its own suspend after a wake",
        )

        # A wake sent while the freezer still waits for the readers, long
        # before interrupts stop, makes the PM core give the suspend up.
        exits = guest.output().count(SUSPEND_EXIT)
        freezing = guest.output().count(SUSPEND_FREEZING)
        guest._write(f"python3 {HOLDER} {HOLDER_READERS} &\n")
        wait_for_line(guest, SUSPEND_FREEZING, freezing, "self-wake")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: early wake replied {answer!r}")
        early = wait_for_exit_line(
            guest, exits,
            "self-wake: the guest stayed suspended after a wake sent while it "
            "was entering its own suspend",
        )
        wait_for_line(guest, "hold-freeze: ", 0, "self-wake")
        if HOLDER_REFUSED not in guest.output():
            raise RuntimeError(
                "self-wake: the wake reached the suspend only after it had "
                "been entered:\n" + guest.output()[-4000:]
            )
        guest.check_command("echo self-wake-running", 60)
        print(
            f"self-wake: a held suspend ended {held:.2f} s and one being "
            f"entered {early:.2f} s after a wake; a running guest only "
            "answered"
        )
        require_clean(guest, "self-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("self-wake: the guest did not power off")


def run_power(arguments, private: pathlib.Path) -> None:
    """A power key forwarded through the VNC display wakes a guest that
    suspended itself, and reaches it exactly once."""
    port = unused_loopback_port()
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (
            f"ncpus={VCPUS}",
            "--gpu",
            "--gpu-display=vnc",
            f"--gpu-vnc-port={port}",
        ),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={KEY_READER: GuestFile(KEY_READER_SOURCE.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        request = len(guest.output())
        guest.check_command(
            f"(python3 {KEY_READER} >/run/power-keys.err 2>&1 </dev/null &)",
            30,
        )
        deadline = time.monotonic() + REQUEST_SECONDS
        while "ready" not in guest.check_command(
            f"cat {KEY_LOG} 2>/dev/null; true", 30
        ):
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "power: the key reader did not start:\n"
                    + guest.check_command("cat /run/power-keys.err; true", 30)
                )
            drain(guest, 0.2)
        keyboard = VncKeyboard(guest, port)
        try:
            exits = guest.output().count(SUSPEND_EXIT)
            entries = guest.output().count(SUSPEND_ENTRY)
            guest._write(SELF_SUSPEND)
            wait_for_line(guest, SUSPEND_ENTRY, entries, "power")
            hold_quietly(guest, exits, 2.0, "power")
            keyboard.type_key(XF86_POWER_OFF)
            woke = wait_for_exit_line(
                guest, exits,
                "power: the guest stayed in its own suspend after a power key",
            )
        finally:
            keyboard.close()
        # The emulator's VNC server reports the keyboard client's departure
        # on the console it shares with the guest, and that report can land
        # in this command's output. One printf writes the events as one line,
        # which reaches the console in one piece, and only that line is read.
        listed = guest.check_command(
            f"printf 'keys:%s:end\\n' \"$(tr '\\n' ' ' < {KEY_LOG})\"", 30
        )
        found = re.search(r"keys:([^:\r\n]*):end", listed)
        if found is None:
            raise RuntimeError(f"power: no key events in {listed!r}")
        keys = found.group(1).split()
        if keys != ["ready", "1", "0"]:
            raise RuntimeError(
                f"power: the guest received power key events {keys[1:]}, not "
                "one press and one release"
            )
        print(
            f"power: a power key woke the guest's own suspend in {woke:.2f} s "
            "and arrived once"
        )
        require_clean(guest, "power", request)
        if not guest.shutdown(60):
            raise RuntimeError("power: the guest did not power off")


ARMS = {
    "network": run_network,
    "console": run_console,
    "vhost-user": run_vhost_user,
    "early-wake": run_early_wake,
    "self-wake": run_self_wake,
    "power": run_power,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--arm", action="append", choices=tuple(ARMS))
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-suspend-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        for arm in arguments.arm or tuple(ARMS):
            ARMS[arm](arguments, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
