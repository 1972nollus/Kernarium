#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the maintained Darwin IRQ backend without booting a guest."""
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True, timeout=30)


def main():
    if platform.system() != "Darwin":
        print("Darwin kqueue regression is only applicable on macOS")
        return
    fixtures = Path(__file__).with_name("irq-edge")
    with tempfile.TemporaryDirectory(prefix="uml-irq-edge-") as temporary:
        work = Path(temporary)
        # irq.c and its private header are whole-file additions in this topic.
        patch = ROOT / "kernel/mainline-6.18/patches/series/0007-uml-add-Darwin-host-service-implementation.patch"
        run("git", "apply", "--include=arch/um/os-Darwin/irq.c", "--include=arch/um/os-Darwin/irq.h", str(patch), cwd=work)
        source = work / "arch/um/os-Darwin/irq.c"
        run("cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}", "-c", str(source), "-o", str(work / "irq.o"))
        run("c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(fixtures / "driver.cpp"), str(work / "irq.o"), "-o", str(work / "check"))
        run(str(work / "check"))


if __name__ == "__main__":
    main()
