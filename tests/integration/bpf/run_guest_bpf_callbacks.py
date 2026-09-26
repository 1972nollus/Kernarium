#!/usr/bin/env python3
"""Run BPF programs with callbacks on the interpreter-only guest kernel.

The guest kernel has no BPF JIT, so every callback a helper such as
bpf_loop() or bpf_for_each_map_elem() invokes enters the interpreter through
the kernel's fixed pool of native entry points. The probe
loads and runs such programs, checks the verifier limits still hold, and
exhausts and refills the entry-point pool.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import pathlib
import sys
import zlib


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("bpf_callbacks_probe.py")
CHUNK_CHARACTERS = 2048
PROBE_TIMEOUT_SECONDS = 420


def install_probe(guest: Guest) -> None:
    encoded = base64.b64encode(zlib.compress(PROBE.read_bytes(), 9)).decode()
    for offset in range(0, len(encoded), CHUNK_CHARACTERS):
        chunk = encoded[offset : offset + CHUNK_CHARACTERS]
        result = guest.run_command(f"printf '%s' '{chunk}' >> /tmp/probe.b64", 30)
        if result.return_code != 0:
            raise RuntimeError("could not copy the probe:\n" + result.output)
    result = guest.run_command(
        "python3 -B -c 'import base64, zlib; "
        'open("/tmp/probe.py", "wb").write(zlib.decompress('
        'base64.b64decode(open("/tmp/probe.b64").read())))\'',
        60,
    )
    if result.return_code != 0:
        raise RuntimeError("could not unpack the probe:\n" + result.output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        ("ncpus=4",),
        init_shell=True,
        engine=arguments.engine,
    )
    try:
        guest.boot_to_shell(180)
        mounted = guest.run_command(
            "mount -t proc proc /proc && mount -t tmpfs -o mode=1777 tmpfs /tmp",
            20,
        )
        if mounted.return_code != 0:
            raise RuntimeError("could not prepare the guest:\n" + mounted.output)
        install_probe(guest)
        result = guest.run_command("python3 -B /tmp/probe.py", PROBE_TIMEOUT_SECONDS)
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the BPF callback probe reported {result.return_code} failure(s)"
            )
        return 0
    except BaseException:
        print(guest.output()[-4000:], file=sys.stderr)
        raise
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
