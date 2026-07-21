#!/usr/bin/env python3
"""Send one command to a User Mode Linux management console."""

from __future__ import annotations

import argparse
import os
import pathlib
import socket
import struct
import uuid


MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA_SIZE = 512
PACKET = struct.Struct(f"=III{MCONSOLE_DATA_SIZE}s")


def request(server: pathlib.Path, command: str, timeout: float = 5.0) -> str:
    """Return all reply packets for one management-console command."""
    encoded = command.encode()
    if len(encoded) >= MCONSOLE_DATA_SIZE:
        raise ValueError("management-console command is too long")
    client = pathlib.Path(
        f"/tmp/uml-mconsole-client-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    )
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.bind(str(client))
        sock.settimeout(timeout)
        sock.sendto(
            PACKET.pack(
                MCONSOLE_MAGIC,
                MCONSOLE_VERSION,
                len(encoded),
                encoded.ljust(MCONSOLE_DATA_SIZE, b"\0"),
            ),
            str(server),
        )
        chunks: list[str] = []
        while True:
            packet = sock.recv(PACKET.size)
            error, more, length, data = PACKET.unpack(
                packet.ljust(PACKET.size, b"\0")
            )
            if length > MCONSOLE_DATA_SIZE:
                raise RuntimeError("invalid management-console reply length")
            text = data[:length].rstrip(b"\0").decode(errors="replace")
            chunks.append(text)
            if error:
                raise RuntimeError("".join(chunks))
            if not more:
                return "".join(chunks)
    finally:
        sock.close()
        client.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket", type=pathlib.Path)
    parser.add_argument("command", nargs="+")
    parser.add_argument("--timeout", type=float, default=5.0)
    arguments = parser.parse_args()
    print(request(arguments.socket, " ".join(arguments.command), arguments.timeout))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
