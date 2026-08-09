#!/usr/bin/env python3
"""Unit tests for result parsing and LTP expectation classification."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import base64
import gzip
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

LTP_ROOT = pathlib.Path(__file__).resolve().parents[2] / "integration/ltp"
sys.path.insert(0, str(LTP_ROOT))
import run_guest_ltp as runner  # noqa: E402
import guest as guest_support  # noqa: E402


class GuestConsoleWriteTests(unittest.TestCase):
    def test_console_write_retries_short_writes_until_complete(self) -> None:
        guest = guest_support.Guest.__new__(guest_support.Guest)
        guest._master = 17
        guest._process = mock.Mock()
        guest._process.poll.return_value = None
        payload = b"overlay-bootstrap\n"
        received = bytearray()

        def short_write(descriptor: int, remaining: memoryview) -> int:
            self.assertEqual(descriptor, 17)
            count = min(3, len(remaining))
            received.extend(bytes(remaining[:count]))
            return count

        with mock.patch.object(
            guest_support.os, "write", side_effect=short_write
        ) as write_mock:
            guest._write(payload.decode())

        self.assertEqual(bytes(received), payload)
        self.assertGreater(write_mock.call_count, 1)

    def test_console_write_waits_after_nonblocking_backpressure(self) -> None:
        guest = guest_support.Guest.__new__(guest_support.Guest)
        guest._master = 23
        guest._process = mock.Mock()
        guest._process.poll.return_value = None

        with (
            mock.patch.object(
                guest_support.os,
                "write",
                side_effect=(BlockingIOError(), len(b"command\n")),
            ) as write_mock,
            mock.patch.object(
                guest_support.select,
                "select",
                return_value=([], [23], []),
            ) as select_mock,
        ):
            guest._write("command\n")

        self.assertEqual(write_mock.call_count, 2)
        select_mock.assert_called_once()


class ResultParsingTests(unittest.TestCase):
    def test_repository_design_omissions_are_disabled_and_not_verified(self) -> None:
        disabled = runner.load_disabled_manifest(LTP_ROOT / "disabled-tests.tsv")
        verified = {
            case.key for case in runner.load_manifest(LTP_ROOT / "verified-tests.tsv")
        }
        expected = {
            "mm/mmap10",
            "mm/mmap10_1",
            "mm/ksm01",
            "mm/ksm01_1",
            "mm/ksm02",
            "mm/ksm02_1",
            "mm/ksm03",
            "mm/ksm03_1",
            "mm/ksm04",
            "mm/ksm04_1",
            "mm/ksm05",
            "mm/ksm06",
            "mm/ksm06_1",
            "mm/ksm06_2",
            "mm/ksm07",
            "mm/oom01",
            "mm/thp03",
            "pty/pty03",
            "pty/pty04",
            "pty/pty05",
            "pty/pty07",
            "controllers/cgroup_fj_function_debug",
            "controllers/cgroup_fj_stress_debug_2_2_none",
            "controllers/cgroup_fj_stress_debug_3_3_none",
            "controllers/cgroup_fj_stress_debug_4_4_none",
            "controllers/cgroup_fj_stress_debug_2_9_none",
            "controllers/cgroup_fj_stress_debug_10_3_none",
            "controllers/cgroup_fj_stress_debug_1_200_none",
            "controllers/cgroup_fj_stress_debug_200_1_none",
            "controllers/cgroup_fj_stress_debug_2_2_one",
            "controllers/cgroup_fj_stress_debug_3_3_one",
            "controllers/cgroup_fj_stress_debug_4_4_one",
            "controllers/cgroup_fj_stress_debug_2_9_one",
            "controllers/cgroup_fj_stress_debug_10_3_one",
            "controllers/cgroup_fj_stress_debug_1_200_one",
            "controllers/cgroup_fj_stress_debug_200_1_one",
            "controllers/cgroup_fj_stress_debug_2_2_each",
            "controllers/cgroup_fj_stress_debug_3_3_each",
            "controllers/cgroup_fj_stress_debug_4_4_each",
            "controllers/cgroup_fj_stress_debug_2_9_each",
            "controllers/cgroup_fj_stress_debug_10_3_each",
            "controllers/cgroup_fj_stress_debug_1_200_each",
            "controllers/cgroup_fj_stress_debug_200_1_each",
            "cve/cve-2011-2183",
            "cve/cve-2017-2636",
            "cve/cve-2020-11494",
            "cve/cve-2020-14416",
        }
        expected.update({
            "mm/thp01",
            "mm/thp02",
            "mm/thp04",
            "syscalls/cachestat03",
            "syscalls/futex_wake04",
            "syscalls/memfd_create03",
            "syscalls/memfd_create04",
            "controllers/cgroup_fj_function_hugetlb",
            "hugetlb/hugefallocate01",
            "hugetlb/hugefallocate02",
            "hugetlb/hugefork01",
            "hugetlb/hugefork02",
            "hugetlb/hugemmap01",
            "hugetlb/hugemmap02",
            "hugetlb/hugemmap04",
            "hugetlb/hugemmap05",
            "hugetlb/hugemmap06",
            "hugetlb/hugemmap07",
            "hugetlb/hugemmap08",
            "hugetlb/hugemmap09",
            "hugetlb/hugemmap10",
            "hugetlb/hugemmap11",
            "hugetlb/hugemmap12",
            "hugetlb/hugemmap13",
            "hugetlb/hugemmap14",
            "hugetlb/hugemmap15",
            "hugetlb/hugemmap16",
            "hugetlb/hugemmap17",
            "hugetlb/hugemmap18",
            "hugetlb/hugemmap19",
            "hugetlb/hugemmap20",
            "hugetlb/hugemmap21",
            "hugetlb/hugemmap22",
            "hugetlb/hugemmap23",
            "hugetlb/hugemmap24",
            "hugetlb/hugemmap25",
            "hugetlb/hugemmap26",
            "hugetlb/hugemmap27",
            "hugetlb/hugemmap28",
            "hugetlb/hugemmap29",
            "hugetlb/hugemmap30",
            "hugetlb/hugemmap31",
            "hugetlb/hugemmap34",
            "hugetlb/hugemmap05_1",
            "hugetlb/hugemmap05_2",
            "hugetlb/hugemmap05_3",
            "hugetlb/hugeshmat01",
            "hugetlb/hugeshmat02",
            "hugetlb/hugeshmat03",
            "hugetlb/hugeshmat04",
            "hugetlb/hugeshmat05",
            "hugetlb/hugeshmctl01",
            "hugetlb/hugeshmctl02",
            "hugetlb/hugeshmctl03",
            "hugetlb/hugeshmdt01",
            "hugetlb/hugeshmget01",
            "hugetlb/hugeshmget02",
            "hugetlb/hugeshmget03",
            "hugetlb/hugeshmget05",
            "hugetlb/hugeshmget06",
            "can/can_filter",
            "can/can_rcv_own_msgs",
            "can/can_bcm01",
            "cve/cve-2011-0999",
            "cve/cve-2017-1000405",
            "cve/cve-2021-3609",
            "controllers/cgroup_fj_stress_hugetlb_2_2_none",
            "controllers/cgroup_fj_stress_hugetlb_3_3_none",
            "controllers/cgroup_fj_stress_hugetlb_4_4_none",
            "controllers/cgroup_fj_stress_hugetlb_2_9_none",
            "controllers/cgroup_fj_stress_hugetlb_10_3_none",
            "controllers/cgroup_fj_stress_hugetlb_1_200_none",
            "controllers/cgroup_fj_stress_hugetlb_200_1_none",
            "controllers/cgroup_fj_stress_hugetlb_2_2_one",
            "controllers/cgroup_fj_stress_hugetlb_3_3_one",
            "controllers/cgroup_fj_stress_hugetlb_4_4_one",
            "controllers/cgroup_fj_stress_hugetlb_2_9_one",
            "controllers/cgroup_fj_stress_hugetlb_10_3_one",
            "controllers/cgroup_fj_stress_hugetlb_1_200_one",
            "controllers/cgroup_fj_stress_hugetlb_200_1_one",
            "controllers/cgroup_fj_stress_hugetlb_2_2_each",
            "controllers/cgroup_fj_stress_hugetlb_3_3_each",
            "controllers/cgroup_fj_stress_hugetlb_4_4_each",
            "controllers/cgroup_fj_stress_hugetlb_2_9_each",
            "controllers/cgroup_fj_stress_hugetlb_10_3_each",
            "controllers/cgroup_fj_stress_hugetlb_1_200_each",
            "controllers/cgroup_fj_stress_hugetlb_200_1_each",
            "hugetlb/hugemmap32",
        })
        self.assertEqual(len(verified), 2_428)
        self.assertEqual(len(disabled), 135)
        self.assertEqual(len(verified) + len(disabled), 2_563)
        self.assertEqual(set(disabled), expected)
        self.assertTrue(expected.isdisjoint(verified))

    def test_updated_rootfs_requalification_is_reflected_in_manifest(self) -> None:
        cases = {
            case.key: case
            for case in runner.load_manifest(LTP_ROOT / "verified-tests.tsv")
        }
        strict_passes = {
            "syscalls/fanotify22",
            "syscalls/mmap16",
            "syscalls/quotactl01",
            "syscalls/quotactl04",
            "syscalls/quotactl06",
            "syscalls/quotactl08",
            "syscalls/statx05",
            "syscalls/statx06",
            "syscalls/statx09",
            "containers/netns_netlink",
            "commands/mkfs01_ext3_sh",
            "commands/mkfs01_ext4_sh",
            "fs/binfmt_misc01",
            "fs/binfmt_misc02",
            "fs/quota_remount_test01",
            "kernel_misc/zram03",
        }
        metadata_driver_passes = {
            "containers/netns_breakns_ip_ipv4_netlink",
            "containers/netns_breakns_ip_ipv6_netlink",
            "containers/netns_breakns_ip_ipv4_ioctl",
            "containers/netns_breakns_ip_ipv6_ioctl",
            "containers/netns_breakns_ns_exec_ipv4_netlink",
            "containers/netns_breakns_ns_exec_ipv6_netlink",
            "containers/netns_breakns_ns_exec_ipv4_ioctl",
            "containers/netns_breakns_ns_exec_ipv6_ioctl",
            "containers/netns_comm_ip_ipv4_netlink",
            "containers/netns_comm_ip_ipv6_netlink",
            "containers/netns_comm_ip_ipv4_ioctl",
            "containers/netns_comm_ip_ipv6_ioctl",
            "containers/netns_comm_ns_exec_ipv4_netlink",
            "containers/netns_comm_ns_exec_ipv6_netlink",
            "containers/netns_comm_ns_exec_ipv4_ioctl",
            "containers/netns_comm_ns_exec_ipv6_ioctl",
            "containers/netns_sysfs",
        }
        strict_passes |= metadata_driver_passes
        userspace_exemptions = {
            "syscalls/fallocate04",
            "syscalls/fallocate05",
            "syscalls/fallocate06",
            "syscalls/fanotify13",
            "syscalls/fanotify24",
            "syscalls/preadv203",
            "syscalls/preadv203_64",
            "syscalls/quotactl09",
            "syscalls/statx10",
            "kernel_misc/zram01",
            "kernel_misc/zram02",
        }
        requalified = strict_passes | userspace_exemptions
        self.assertEqual(len(strict_passes), 33)
        self.assertEqual(len(requalified), 44)
        self.assertEqual(
            {cases[key].expectation for key in strict_passes}, {"pass"}
        )
        self.assertEqual(
            {cases[key].expectation for key in userspace_exemptions},
            {"userspace-exempt"},
        )
        self.assertEqual(
            cases["syscalls/statx05"].reason,
            "All four encrypted and plain ext4 statx attribute assertions "
            "pass",
        )
        self.assertEqual(
            cases["fs/quota_remount_test01"].reason,
            "Quota usage changes after the ext3 read-only and read-write "
            "remount cycle",
        )
        self.assertEqual(
            cases["syscalls/quotactl01"].reason,
            "All 36 user and group quota-control assertions pass across the "
            "vfsv0 and vfsv1 formats",
        )
        self.assertEqual(
            cases["syscalls/quotactl06"].reason,
            "All 22 quota error-path assertions pass across the vfsv0 and "
            "vfsv1 formats",
        )
        self.assertEqual(
            cases["kernel_misc/zram01"].reason,
            "All 12 applicable zram lifecycle and compression checks pass; "
            "one retired max_comp_streams attribute probe is skipped",
        )
        self.assertTrue(cases["kernel_misc/zram01"].serial)
        self.assertEqual(
            cases["kernel_misc/zram01"].kernel_arguments, ("ncpus=1",)
        )
        self.assertTrue(all(
            "BusyBox ip" not in cases[key].reason for key in requalified
        ))

    def test_disabled_manifest_records_design_omissions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "disabled.tsv"
            path.write_text(
                "# scenario\\ttest_id\\treason\n"
                "mm\tksm01\tKSM intentionally disabled\n",
                encoding="utf-8",
            )
            self.assertEqual(
                runner.load_disabled_manifest(path),
                {"mm/ksm01": "KSM intentionally disabled"},
            )

    def test_disabled_manifest_rejects_duplicate_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "disabled.tsv"
            path.write_text(
                "mm\tksm01\tfirst\nmm\tksm01\tsecond\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate disabled case"):
                runner.load_disabled_manifest(path)

    def test_guest_disk_uses_raw_erofs_root(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"), "1G"
        )
        self.assertIn("--execution-engine=jit", arguments)
        self.assertIn("ubd0c=rootfs.erofs", arguments)
        self.assertIn("root=/dev/ubda", arguments)
        self.assertIn("rootfstype=erofs", arguments)
        self.assertNotIn("uml_ltp_workspace=ext2", arguments)

    def test_boot_timeout_allows_parallel_host_startup_contention(self) -> None:
        self.assertGreaterEqual(runner.BOOT_TIMEOUT_SECONDS, 60)

    def test_overlay_shell_bootstrap_mounts_only_disposable_writable_state(self) -> None:
        bootstrap = runner.OVERLAY_SHELL_BOOTSTRAP
        self.assertEqual(bootstrap.splitlines()[0], "{")
        self.assertEqual(bootstrap.splitlines()[-2], "}")
        self.assertIn(
            guest_support.OVERLAY_READY_MARKER, bootstrap.splitlines()[-1]
        )
        self.assertLessEqual(max(map(len, bootstrap.splitlines())), 512)
        self.assertIn("lowerdir=/mnt/lower,upperdir=/mnt/upper", bootstrap)
        self.assertIn("mount -t proc proc /proc", bootstrap)
        self.assertIn("mount -t sysfs sysfs /sys", bootstrap)
        self.assertIn("mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run", bootstrap)
        self.assertIn("mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp", bootstrap)
        self.assertIn("uml_ltp_workspace=ext2", bootstrap)
        self.assertIn("command -v mkfs.ext2", bootstrap)
        self.assertIn("command -v mke2fs", bootstrap)
        self.assertIn("LTP ext2 workspace requires mkfs.ext2 or mke2fs", bootstrap)
        self.assertIn('"$workspace_mkfs" -q -t ext2', bootstrap)
        self.assertIn("mount -t ext2 -o noatime", bootstrap)
        self.assertLess(
            bootstrap.index("set +e"),
            bootstrap.index(guest_support.OVERLAY_READY_MARKER),
        )
        self.assertLess(
            bootstrap.index("set +u"),
            bootstrap.index(guest_support.OVERLAY_READY_MARKER),
        )
        self.assertNotIn("exec /bin/sh", bootstrap)
        self.assertNotIn("exec /lib/systemd/systemd", bootstrap)

    def test_modules_builtin_validation_is_canonical_and_exhaustive(self) -> None:
        entries = tuple(
            f"kernel/drivers/audit/driver_{index:03}.ko"
            for index in range(250)
        )
        data = ("\n".join(entries) + "\n").encode()
        self.assertEqual(runner.validate_modules_builtin(data), entries)
        invalid = (
            b"",
            b"kernel/drivers/net/veth.ko",
            b"/kernel/drivers/net/veth.ko\n",
            b"kernel/../veth.ko\n",
            b"kernel/drivers/net/veth.ko\n\n",
            b"kernel/drivers/net/veth.ko\nkernel/drivers/net/veth.ko\n",
            b"kernel/drivers/net/veth.o\n",
            b"\xff\n",
        )
        for value in invalid:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    runner.validate_modules_builtin(value)

    def test_modules_builtin_loader_requires_sibling_kernel_release(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory) / "build"
            release = build / "include/config/kernel.release"
            release.parent.mkdir(parents=True)
            modules_builtin = build / "modules.builtin"
            data = b"kernel/fs/binfmt_misc.ko\nkernel/drivers/net/veth.ko\n"
            modules_builtin.write_bytes(data)
            release.write_text("6.18.41-test\n", encoding="utf-8")
            self.assertEqual(
                runner.load_modules_builtin(modules_builtin),
                (data, "6.18.41-test"),
            )
            release.write_text("bad release\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid sibling"):
                runner.load_modules_builtin(modules_builtin)

    def test_ltp_builtin_driver_metadata_feeds_complete_kbuild_output(self) -> None:
        entries = (
            "kernel/fs/binfmt_misc.ko",
            "kernel/fs/quota/quota_v2.ko",
            "kernel/drivers/block/zram/zram.ko",
            "kernel/drivers/net/dummy.ko",
            "kernel/drivers/net/wireguard/wireguard.ko",
            "kernel/drivers/net/veth.ko",
            "kernel/net/netfilter/nf_tables.ko",
        ) + tuple(
            f"kernel/drivers/audit/driver_{index:03}.ko"
            for index in range(243)
        )
        data = ("\n".join(entries) + "\n").encode()
        self.assertGreaterEqual(len(data), 7797)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            release = modules_root / "test-release"
            release.mkdir(parents=True)
            outside_builtin = root / "outside-modules.builtin"
            outside_dep = root / "outside-modules.dep"
            outside_builtin.write_text("kernel/drivers/net/geneve.ko\n")
            outside_dep.write_text("kernel/drivers/net/geneve.ko:\n")
            (release / "modules.builtin").symlink_to(outside_builtin)
            (release / "modules.dep").symlink_to(outside_dep)
            config = root / "config.gz"
            tools_root = root / "run/uml-ltp-tools"
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("# CONFIG_MODULES is not set\n")
            setup = runner.ltp_builtin_driver_metadata_setup(
                data,
                "test-release",
                str(modules_root),
                str(config),
                ("gzip", "-cd"),
                ("printf", "test-release"),
                tools_root=str(tools_root),
            )
            self.assertLessEqual(max(map(len, setup.splitlines())), 512)
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual((release / "modules.dep").read_text(), "")
            self.assertEqual((release / "modules.builtin").read_bytes(), data)
            self.assertFalse((release / "modules.dep").is_symlink())
            self.assertFalse((release / "modules.builtin").is_symlink())
            self.assertEqual(
                outside_builtin.read_text(), "kernel/drivers/net/geneve.ko\n"
            )
            self.assertEqual(
                outside_dep.read_text(), "kernel/drivers/net/geneve.ko:\n"
            )
            self.assertTrue((tools_root / "kmod").stat().st_mode & 0o111)
            self.assertEqual((tools_root / "modprobe").readlink(), pathlib.Path("kmod"))
            self.assertEqual((tools_root / "rmmod").readlink(), pathlib.Path("kmod"))
            self.assertFalse((root / "usr/bin/modprobe").exists())

            def run_tool(
                name: str, *arguments: str
            ) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(tools_root / name), *arguments],
                    check=False,
                    capture_output=True,
                    text=True,
                )

            accepted = (
                ("modprobe", "binfmt_misc"),
                ("modprobe", "-a", "binfmt_misc", "veth"),
                ("modprobe", "--all", "quota_v2", "zram"),
                ("kmod", "modprobe", "nf_tables"),
            )
            for invocation in accepted:
                with self.subTest(invocation=invocation):
                    result = run_tool(*invocation)
                    self.assertEqual(result.returncode, 0, result.stderr)

            rejected = (
                ("modprobe",),
                ("modprobe", "geneve"),
                ("modprobe", "kernel/fs/binfmt_misc.ko"),
                ("modprobe", "binfmt_misc", "veth"),
                ("modprobe", "zram", "num_devices=2"),
                ("modprobe", "-q", "binfmt_misc"),
                ("modprobe", "-r", "binfmt_misc"),
                ("modprobe", "--remove", "binfmt_misc"),
                ("rmmod", "zram"),
                ("kmod", "rmmod", "zram"),
                ("kmod", "depmod"),
            )
            for invocation in rejected:
                with self.subTest(invocation=invocation):
                    self.assertNotEqual(run_tool(*invocation).returncode, 0)

            with (release / "modules.builtin").open("a", encoding="utf-8") as output:
                output.write("kernel/drivers/net/geneve.ko\n")
            self.assertNotEqual(run_tool("modprobe", "geneve").returncode, 0)
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write(
                    "# CONFIG_MODULES is not set\nCONFIG_MODULES=y\n"
                )
            self.assertNotEqual(run_tool("modprobe", "binfmt_misc").returncode, 0)
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("CONFIG_MODULES=y\n")
            self.assertNotEqual(run_tool("modprobe", "binfmt_misc").returncode, 0)
            config.unlink()
            self.assertNotEqual(run_tool("modprobe", "binfmt_misc").returncode, 0)

    def test_ltp_builtin_driver_metadata_rejects_release_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            config = root / "config.gz"
            tools_root = root / "run/uml-ltp-tools"
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("# CONFIG_MODULES is not set\n")
            setup = runner.ltp_builtin_driver_metadata_setup(
                b"kernel/fs/binfmt_misc.ko\n",
                "expected-release",
                str(modules_root),
                str(config),
                ("gzip", "-cd"),
                ("printf", "wrong-release"),
                tools_root=str(tools_root),
            )
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("kernel release mismatch", completed.stderr)
            self.assertFalse(modules_root.exists())
            self.assertFalse(tools_root.exists())

    def test_ltp_builtin_driver_metadata_rejects_contradictory_config(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            config = root / "config.gz"
            tools_root = root / "run/uml-ltp-tools"
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write(
                    "# CONFIG_MODULES is not set\nCONFIG_MODULES=y\n"
                )
            setup = runner.ltp_builtin_driver_metadata_setup(
                b"kernel/drivers/net/veth.ko\n",
                "test-release",
                str(modules_root),
                str(config),
                ("gzip", "-cd"),
                ("printf", "test-release"),
                tools_root=str(tools_root),
            )
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("requires CONFIG_MODULES=n", completed.stderr)
            self.assertFalse(modules_root.exists())
            self.assertFalse(tools_root.exists())

    def test_ltp_builtin_driver_metadata_rejects_config_reader_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            tools_root = root / "run/uml-ltp-tools"
            setup = runner.ltp_builtin_driver_metadata_setup(
                b"kernel/drivers/net/veth.ko\n",
                "test-release",
                str(modules_root),
                str(root / "missing-config"),
                ("false",),
                ("printf", "test-release"),
                tools_root=str(tools_root),
            )
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertFalse(modules_root.exists())
            self.assertFalse(tools_root.exists())

    def test_ltp_builtin_driver_metadata_does_not_fake_loadable_modules(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            config = root / "config.gz"
            tools_root = root / "run/uml-ltp-tools"
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("CONFIG_MODULES=y\n")
            setup = runner.ltp_builtin_driver_metadata_setup(
                b"kernel/drivers/net/veth.ko\n",
                "test-release",
                str(modules_root),
                str(config),
                ("gzip", "-cd"),
                ("printf", "test-release"),
                tools_root=str(tools_root),
            )
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("requires CONFIG_MODULES=n", completed.stderr)
            self.assertFalse(modules_root.exists())
            self.assertFalse(tools_root.exists())

    def test_ltp_builtin_driver_metadata_replaces_stale_regular_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root = root / "modules"
            tools_root = root / "run/uml-ltp-tools"
            release = modules_root / "test-release"
            release.mkdir(parents=True)
            modules_dep = release / "modules.dep"
            modules_builtin = release / "modules.builtin"
            modules_dep.write_text("real-module.ko:\n")
            modules_builtin.write_text("kernel/real-builtin.ko\n")
            config = root / "config.gz"
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("# CONFIG_MODULES is not set\n")
            setup = runner.ltp_builtin_driver_metadata_setup(
                b"kernel/drivers/net/veth.ko\n",
                "test-release",
                str(modules_root),
                str(config),
                ("gzip", "-cd"),
                ("printf", "test-release"),
                tools_root=str(tools_root),
            )
            completed = subprocess.run(
                ["/bin/sh", "-c", setup],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(modules_dep.read_text(), "")
            self.assertEqual(
                modules_builtin.read_text(), "kernel/drivers/net/veth.ko\n"
            )

    def test_ltp_metadata_transport_is_bounded_and_round_trips(self) -> None:
        large_script = "\n".join(
            f": metadata_{index:04}_{index * 2654435761:016x}"
            for index in range(800)
        )
        large_transport = runner.ltp_metadata_transport(large_script)
        self.assertGreater(len(large_transport.append_chunks), 1)
        commands = (
            large_transport.initialize,
            *large_transport.append_chunks,
            large_transport.execute,
            large_transport.cleanup,
        )
        self.assertLessEqual(
            max(len(command.encode()) for command in commands),
            runner.LTP_METADATA_TRANSPORT_MAX_COMMAND_BYTES,
        )
        self.assertNotIn(large_script, "\n".join(commands))

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            transport_root = root / "transport"
            sentinel = root / "sentinel"
            script = (
                "printf '%s' transported > "
                + shlex.quote(str(sentinel))
            )
            transport = runner.ltp_metadata_transport(
                script, str(transport_root)
            )
            for command in (
                transport.initialize,
                *transport.append_chunks,
                transport.execute,
                transport.cleanup,
            ):
                completed = subprocess.run(
                    ["/bin/sh", "-c", command],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(sentinel.read_text(), "transported")
            self.assertFalse(transport_root.exists())

    def test_ltp_metadata_transport_cleans_decode_decompress_and_script_failures(
        self,
    ) -> None:
        failure_cases = (
            ("decode", "%%%", "LTP metadata transport decode failed", None),
            (
                "decompress",
                base64.b64encode(b"not a gzip stream").decode(),
                "LTP metadata transport decompress failed",
                None,
            ),
            ("script", None, "LTP metadata setup script failed", 23),
        )
        for name, replacement, diagnostic, expected_code in failure_cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                transport_root = root / "transport"
                setup_script = "exit 23" if name == "script" else ":"
                transport = runner.ltp_metadata_transport(
                    setup_script, str(transport_root)
                )
                initialized = subprocess.run(
                    ["/bin/sh", "-c", transport.initialize],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(initialized.returncode, 0, initialized.stderr)
                if replacement is None:
                    append_commands = transport.append_chunks
                else:
                    payload_path = shlex.quote(
                        str(transport_root / "setup.sh.gz.b64")
                    )
                    append_commands = (
                        f"printf '%s' {shlex.quote(replacement)} > {payload_path}",
                    )
                for command in append_commands:
                    appended = subprocess.run(
                        ["/bin/sh", "-c", command],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(appended.returncode, 0, appended.stderr)
                executed = subprocess.run(
                    ["/bin/sh", "-c", transport.execute],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if expected_code is None:
                    self.assertNotEqual(executed.returncode, 0)
                else:
                    self.assertEqual(executed.returncode, expected_code)
                self.assertIn(diagnostic, executed.stderr)
                self.assertFalse(transport_root.exists())
                cleaned = subprocess.run(
                    ["/bin/sh", "-c", transport.cleanup],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(cleaned.returncode, 0, cleaned.stderr)

    def test_ltp_overlay_boot_prepares_builtin_driver_metadata(self) -> None:
        guest = object.__new__(runner.Guest)
        guest._overlay_shell = True
        guest.builtin_driver_metadata_setup = "\n".join(
            f": metadata_{index:04}_{index * 2654435761:016x}"
            for index in range(800)
        )
        transport = runner.ltp_metadata_transport(
            guest.builtin_driver_metadata_setup
        )
        self.assertGreater(len(transport.append_chunks), 1)
        guest.run_command = mock.Mock(
            return_value=runner.CommandResult(0, "")
        )
        with mock.patch.object(
            runner.GuestHarness, "boot_and_login"
        ) as boot_and_login:
            guest.boot_and_login(17)
        boot_and_login.assert_called_once_with(17)
        self.assertEqual(
            guest.run_command.call_args_list,
            [
                mock.call("stty -echo", 17),
                mock.call(transport.initialize, 17),
                *(mock.call(command, 17) for command in transport.append_chunks),
                mock.call(transport.execute, 17),
                mock.call(transport.cleanup, 17),
                mock.call("stty echo", 17),
            ],
        )
        self.assertLessEqual(
            max(
                len(call.args[0].encode())
                for call in guest.run_command.call_args_list
            ),
            runner.LTP_METADATA_TRANSPORT_MAX_COMMAND_BYTES,
        )

    def test_ltp_overlay_boot_requires_configured_metadata(self) -> None:
        guest = object.__new__(runner.Guest)
        guest._overlay_shell = True
        guest.builtin_driver_metadata_setup = None
        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login"),
            self.assertRaisesRegex(RuntimeError, "was not configured"),
        ):
            guest.boot_and_login()

    def test_ltp_overlay_boot_cleans_and_restores_after_unacknowledged_chunk(
        self,
    ) -> None:
        guest = object.__new__(runner.Guest)
        guest._overlay_shell = True
        guest.builtin_driver_metadata_setup = "\n".join(
            f": metadata_{index:04}_{index * 2654435761:016x}"
            for index in range(800)
        )
        transport = runner.ltp_metadata_transport(
            guest.builtin_driver_metadata_setup
        )
        failed_chunk = transport.append_chunks[1]

        def run_command(command, _timeout_seconds):
            if command == failed_chunk:
                raise TimeoutError("missing END acknowledgement")
            return runner.CommandResult(0, "")

        guest.run_command = mock.Mock(side_effect=run_command)
        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login"),
            self.assertRaisesRegex(
                RuntimeError, "transport chunk 2/.+not acknowledged"
            ),
        ):
            guest.boot_and_login()
        commands = [call.args[0] for call in guest.run_command.call_args_list]
        self.assertIn(transport.cleanup, commands)
        self.assertEqual(commands[-1], "stty echo")
        self.assertNotIn(transport.execute, commands)

    def test_ltp_overlay_boot_finalizes_disable_and_initialize_failures(
        self,
    ) -> None:
        for failed_stage in ("stty -echo", "initialize"):
            with self.subTest(failed_stage=failed_stage):
                guest = object.__new__(runner.Guest)
                guest._overlay_shell = True
                guest.builtin_driver_metadata_setup = "metadata setup"
                transport = runner.ltp_metadata_transport(
                    guest.builtin_driver_metadata_setup
                )
                failed_command = (
                    "stty -echo"
                    if failed_stage == "stty -echo"
                    else transport.initialize
                )

                def run_command(command, _timeout_seconds):
                    if command == failed_command:
                        return runner.CommandResult(1, "injected failure")
                    return runner.CommandResult(0, "")

                guest.run_command = mock.Mock(side_effect=run_command)
                with (
                    mock.patch.object(
                        runner.GuestHarness, "boot_and_login"
                    ),
                    self.assertRaisesRegex(RuntimeError, "injected failure"),
                ):
                    guest.boot_and_login()
                commands = [
                    call.args[0]
                    for call in guest.run_command.call_args_list
                ]
                self.assertEqual(commands[-1], "stty echo")
                if failed_stage == "initialize":
                    self.assertEqual(
                        commands[-2:], [transport.cleanup, "stty echo"]
                    )
                else:
                    self.assertNotIn(transport.cleanup, commands)
                self.assertNotIn(transport.execute, commands)

    def test_ltp_overlay_boot_reports_execute_and_finalization_failures(
        self,
    ) -> None:
        guest = object.__new__(runner.Guest)
        guest._overlay_shell = True
        guest.builtin_driver_metadata_setup = "metadata setup"
        transport = runner.ltp_metadata_transport(
            guest.builtin_driver_metadata_setup
        )

        def run_command(command, _timeout_seconds):
            if command == transport.execute:
                return runner.CommandResult(
                    1, "LTP metadata transport decompress failed"
                )
            if command == transport.cleanup:
                return runner.CommandResult(1, "cleanup refused")
            return runner.CommandResult(0, "")

        guest.run_command = mock.Mock(side_effect=run_command)
        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login"),
            self.assertRaisesRegex(
                RuntimeError,
                "decode/decompress/setup execution failed.+finalization",
            ),
        ):
            guest.boot_and_login()
        commands = [call.args[0] for call in guest.run_command.call_args_list]
        self.assertEqual(commands[-2:], [transport.cleanup, "stty echo"])

    def test_ltp_overlay_boot_fails_if_echo_restore_fails(self) -> None:
        guest = object.__new__(runner.Guest)
        guest._overlay_shell = True
        guest.builtin_driver_metadata_setup = "metadata setup"
        transport = runner.ltp_metadata_transport(
            guest.builtin_driver_metadata_setup
        )

        def run_command(command, _timeout_seconds):
            if command == "stty echo":
                return runner.CommandResult(1, "tty restore failed")
            return runner.CommandResult(0, "")

        guest.run_command = mock.Mock(side_effect=run_command)
        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login"),
            self.assertRaisesRegex(RuntimeError, "console-echo restore"),
        ):
            guest.boot_and_login()
        commands = [call.args[0] for call in guest.run_command.call_args_list]
        self.assertEqual(commands[-2:], [transport.cleanup, "stty echo"])

    def test_systemd_overlay_bootstrap_mounts_only_disposable_writable_state(self) -> None:
        bootstrap = runner.SYSTEMD_OVERLAY_BOOTSTRAP
        self.assertEqual(bootstrap.splitlines()[0], "{")
        self.assertEqual(bootstrap.splitlines()[-1], "}")
        self.assertLessEqual(max(map(len, bootstrap.splitlines())), 512)
        docker_tmpfs = (
            "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker"
        )
        containerd_tmpfs = (
            "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd"
        )
        self.assertIn("lowerdir=/mnt/lower,upperdir=/mnt/upper", bootstrap)
        self.assertIn("pivot_root . .uml-lower", bootstrap)
        self.assertIn("mkdir -p /var/lib/docker", bootstrap)
        self.assertIn(docker_tmpfs, bootstrap)
        self.assertIn("mkdir -p /var/lib/containerd", bootstrap)
        self.assertIn(containerd_tmpfs, bootstrap)
        self.assertNotIn("getty@tty0.service", bootstrap)
        self.assertNotIn("agetty --autologin", bootstrap)
        self.assertNotIn("useradd", bootstrap)
        self.assertNotIn("groupadd", bootstrap)
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
        self.assertNotIn("mount -o remount,rw", bootstrap)

    def test_systemd_overlay_login_reuses_uml_and_requires_noninteractive_sudo(
        self,
    ) -> None:
        guest = object.__new__(runner.Guest)
        guest._systemd_overlay = True
        guest._init_shell = False
        guest._overlay_shell = False
        guest._swap_path = None
        guest._cgroup1 = None
        guest._cgroup2_nsdelegate = False
        guest._transcript = bytearray(b"x" * 101)
        patterns = []
        writes = []
        commands = []
        search_offsets = []

        class Match:
            @staticmethod
            def end():
                return 73

        def read_until(pattern, _timeout_seconds, _search_from=0):
            patterns.append(pattern.pattern)
            search_offsets.append(_search_from)
            return Match()

        def run_command(command, _timeout_seconds=30):
            commands.append(command)
            return runner.CommandResult(0, "")

        guest._read_until = read_until
        guest._write = writes.append
        guest.run_command = run_command

        guest.boot_and_login()

        self.assertEqual(writes[0], runner.SYSTEMD_OVERLAY_BOOTSTRAP + "\n")
        self.assertIn(rb" login:\s*uml \(automatic login\)", patterns)
        self.assertNotIn(rb" login:\s*root \(automatic login\)", patterns)
        self.assertIn(guest_support.LINE_START + rb"[^\r\n]*\$ $", patterns)
        self.assertIn(73, search_offsets)
        self.assertEqual(search_offsets[-1], len(guest._transcript))
        self.assertEqual(patterns[-1], guest_support.LINE_START + rb"[^\r\n]*# $")
        self.assertEqual(commands, ['test "$(id -un)" = uml && sudo -n true'])
        self.assertEqual(writes[-1], "exec sudo -n /bin/sh\n")

    def test_prompt_patterns_match_the_bytes_a_real_console_emits(self) -> None:
        """Pin the prompt matchers against transcripts, not against themselves.

        The sibling test above mocks `_read_until`, so it only compares pattern
        strings and cannot tell a working matcher from a broken one. A Geekbench
        run timed out with `# ` already on screen because bash emits its
        bracketed-paste reset and a bare carriage return when it execs a
        replacement shell, and `[^\\r\\n]*` may not cross that carriage return.
        """
        root_prompt = re.compile(guest_support.LINE_START + rb"[^\r\n]*# $")
        user_prompt = re.compile(guest_support.LINE_START + rb"[^\r\n]*\$ $")

        exec_replacement_shell = (
            b" sudo -n /bin/sh\r\n"
            b"\x1b[?2004huml@debian-rootfs:~$ exec sudo -n /bin/sh\r\n"
            b"\x1b[?2004l\r# "
        )
        self.assertIsNotNone(root_prompt.search(exec_replacement_shell))

        for transcript in (
            b"\r\n# ",
            b"\n# ",
            b"# ",
            b"\r\n\x1b[?2004l\rdebian-rootfs:~# ",
        ):
            with self.subTest(transcript=transcript):
                self.assertIsNotNone(root_prompt.search(transcript))

        self.assertIsNotNone(
            user_prompt.search(b"\r\n\x1b[?2004h\ruml@debian-rootfs:~$ ")
        )

        # The anchor must still not match a prompt buried mid-line, which is
        # what the `[^\r\n]*` bound is there to prevent.
        self.assertIsNone(root_prompt.search(b"\r\nnot a prompt # yet\r\n"))

    def test_systemd_overlay_login_rejects_missing_noninteractive_sudo(self) -> None:
        guest = object.__new__(runner.Guest)
        guest._systemd_overlay = True
        guest._init_shell = False
        guest._overlay_shell = False
        guest._swap_path = None
        guest._cgroup1 = None
        guest._cgroup2_nsdelegate = False

        class Match:
            @staticmethod
            def end():
                return 0

        guest._read_until = lambda *_args, **_kwargs: Match()
        guest._write = lambda _text: None
        guest.run_command = lambda *_args, **_kwargs: runner.CommandResult(
            1, "sudo: a password is required"
        )

        with self.assertRaisesRegex(RuntimeError, "noninteractive sudo"):
            guest.boot_and_login()

    def test_guest_shell_boot_modes_are_mutually_exclusive(self) -> None:
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            runner.Guest(
                pathlib.Path("uml-darwin"),
                pathlib.Path("rootfs.erofs"),
                "1G",
                "tmpfs",
                init_shell=True,
                overlay_shell=True,
            )
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            runner.Guest(
                pathlib.Path("uml-darwin"),
                pathlib.Path("rootfs.erofs"),
                "1G",
                "tmpfs",
                overlay_shell=True,
                systemd_overlay=True,
            )

    def test_guest_termination_falls_back_to_direct_child_signal(self) -> None:
        guest = object.__new__(runner.Guest)
        guest._process = mock.Mock(pid=1234)
        guest._process.poll.return_value = None
        with mock.patch.object(
            runner.os, "killpg", side_effect=PermissionError
        ):
            guest.terminate()
        guest._process.send_signal.assert_called_once_with(runner.signal.SIGKILL)

    def test_ext2_workspace_is_opt_in(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"),
            "1G", "ext2",
        )
        self.assertIn("uml_ltp_workspace=ext2", arguments)

    def test_kernel_arguments_are_appended(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"),
            "1G", kernel_arguments=("ncpus=2",),
        )
        self.assertEqual(arguments[-1], "ncpus=2")

    def test_disposable_swap_disk_is_separate_from_rootfs(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"),
            "1G", swap_path=pathlib.Path("/tmp/swap.img"),
        )
        self.assertIn("ubd0c=rootfs.erofs", arguments)
        self.assertIn("ubd1=/tmp/swap.img", arguments)

    def test_swap_size_uses_binary_suffixes(self) -> None:
        self.assertEqual(runner.size_bytes("1G"), 1 << 30)
        self.assertEqual(runner.size_bytes("4K"), 4096)
        with self.assertRaises(ValueError):
            runner.size_bytes("3GB")

    def test_legacy_cgroup_controller_name_is_restricted(self) -> None:
        self.assertIsNotNone(runner.CGROUP1_CONTROLLER.fullmatch("memory"))
        self.assertIsNone(runner.CGROUP1_CONTROLLER.fullmatch("memory;reboot"))

    def test_manifest_parses_cgroup2_nsdelegate_resource(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tcgroup_core02\tcgroup_core02\tpass\t120\t"
                "namespace delegation\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertTrue(case.cgroup2_nsdelegate)
            self.assertIsNone(case.cgroup1)

    def test_manifest_rejects_mixed_cgroup_versions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tmixed\ttrue\tpass\t120\tinvalid\t"
                "cgroup1=memory\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "mutually exclusive"):
                runner.load_manifest(path)

    def test_manifest_rejects_duplicate_cgroup2_resource(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "cgroup2=nsdelegate\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate"):
                runner.load_manifest(path)

    def test_manifest_parses_serial_option(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "syscalls\ttiming\ttrue\tpass\t120\ttiming fidelity\t"
                "serial\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertTrue(case.serial)
            self.assertFalse(case.host_pressure)

    def test_manifest_rejects_duplicate_or_mixed_serial_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "syscalls\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "serial\tserial\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate"):
                runner.load_manifest(path)
            path.write_text(
                "syscalls\tmixed\ttrue\tpass\t120\tinvalid\t"
                "serial\thost-pressure\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "mutually exclusive"):
                runner.load_manifest(path)

    def test_manifest_parses_per_case_kernel_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "hugetlb\tlarge\tlarge\tpass\t120\tlarge page\t"
                "kernel-arg=hugepagesz=1G\tkernel-arg=ncpus=4\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertEqual(
                case.kernel_arguments, ("hugepagesz=1G", "ncpus=4")
            )

    def test_manifest_rejects_invalid_kernel_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "hugetlb\tempty\ttrue\tpass\t120\tinvalid\tkernel-arg=\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "empty kernel argument"):
                runner.load_manifest(path)
            path.write_text(
                "hugetlb\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "kernel-arg=ncpus=4\tkernel-arg=ncpus=4\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate kernel argument"):
                runner.load_manifest(path)

    def test_run_case_forwards_kernel_arguments(self) -> None:
        case = runner.Case(
            "hugetlb", "large", "large", "pass", 120, "large page",
            kernel_arguments=("hugepagesz=1G",),
        )
        expected = runner.Result(case, 0, {"passed": 1}, "")
        with mock.patch.object(runner, "Guest") as guest_class:
            guest_class.return_value.run.return_value = expected
            actual = runner.run_case(
                pathlib.Path("emulator"), pathlib.Path("rootfs"), "4G", case
            )
        self.assertEqual(actual, expected)
        guest_class.assert_called_once_with(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "4G", "tmpfs",
            kernel_arguments=("hugepagesz=1G",), swap_size=None, cgroup1=None,
            cgroup2_nsdelegate=False, overlay_shell=True, engine="jit",
        )

    def test_execution_engine_is_explicitly_selected(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"), "1G",
            engine="gadgets",
        )
        self.assertEqual(arguments[1], "--execution-engine=gadgets")

    def test_summary_is_authoritative(self) -> None:
        output = """case.c:1: TPASS: passed

Summary:
passed   7
failed   0
broken   0
skipped  1
warnings 0
"""
        self.assertEqual(
            runner.parse_result_counts(output),
            {"passed": 7, "failed": 0, "broken": 0, "skipped": 1,
             "warnings": 0},
        )

    def test_uppercase_summary_is_authoritative(self) -> None:
        output = """SUMMARY:
PASSED   2
FAILED   0
BROKEN   0
SKIPPED  0
WARNINGS 0
"""
        self.assertEqual(
            runner.parse_result_counts(output),
            {"passed": 2, "failed": 0, "broken": 0, "skipped": 0,
             "warnings": 0},
        )

    def test_protocol_marker_tolerates_tty_case_conversion(self) -> None:
        pattern = runner.marker_pattern("__UML_LTP_END_deadbeef__")
        match = pattern.search(b"__UML_LTP_END_DEADBEEF__:0\r\n")
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), b"0")

    def test_case_runs_from_ltp_binary_directory(self) -> None:
        command = runner.ltp_guest_command("./helper.sh 'argument'")
        self.assertIn(
            "mkdir -p /tmp/uml-ltp-case "
            "/tmp/uml-ltp-output &&",
            command,
        )
        self.assertIn(
            "chmod 1777 /tmp/uml-ltp-case "
            "/tmp/uml-ltp-output &&",
            command,
        )
        self.assertIn(
            "cd /opt/ltp/testcases/bin &&", command
        )
        self.assertIn(
            "PATH=/run/uml-ltp-tools:/opt/ltp/testcases/bin:$PATH",
            command,
        )
        self.assertIn("LTPROOT=/opt/ltp", command)
        self.assertIn("TMPDIR=/tmp/uml-ltp-case", command)
        self.assertIn("LTP_TIMEOUT_MUL=4", command)

    def test_early_tconf_without_summary_is_counted(self) -> None:
        output = "tst_test.c:1417: TCONF: This arch 'aarch64' is not supported\n"
        self.assertEqual(runner.parse_result_counts(output), {"skipped": 1})

    def test_legacy_result_without_summary_is_counted(self) -> None:
        output = """clone02     0  TINFO  :  setting up
clone02     1  TPASS  :  Test Passed
clone02     2  TPASS  :  Test Passed
"""
        self.assertEqual(runner.parse_result_counts(output), {"passed": 2})

    def test_legacy_result_without_space_before_colon_is_counted(self) -> None:
        output = "cpuset_base_ops 1 TCONF: The total of nodes is less than 2\n"
        self.assertEqual(runner.parse_result_counts(output), {"skipped": 1})

    def test_legacy_child_result_without_summary_is_counted(self) -> None:
        output = "setfsuid04  0  TINFO  :  Child process returned TPASS\n"
        self.assertEqual(runner.parse_result_counts(output), {"passed": 1})

    def test_kernel_bug_invalidates_nominal_userspace_pass(self) -> None:
        output = """mmap1.c:246: TPASS: System survived.
BUG: Bad page cache in process mmap1 pfn:06030
page dumped because: still mapped when deleted
"""
        self.assertEqual(
            runner.kernel_failure(output),
            "guest kernel reported a fatal diagnostic: "
            "BUG: Bad page cache in process mmap1 pfn:06030",
        )

    def test_normal_ltp_output_has_no_kernel_failure(self) -> None:
        self.assertIsNone(runner.kernel_failure("case.c:1: TPASS: passed\n"))

    def test_early_architecture_exemption_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "arch_prctl01", "arch_prctl01",
            "hardware-exempt", 60, "x86-only test",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"passed": 7, "skipped": 1}, "cannot fork")
        ))

    def test_hardware_failure_exemption_requires_assertion_failure(self) -> None:
        case = runner.Case(
            "irq", "irqbalance01", "irqbalance01",
            "hardware-failure-exempt", 60, "no physical IRQ router",
        )
        self.assertTrue(runner.accepted(
            runner.Result(case, 1, {"failed": 1, "broken": 0}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 32, {"skipped": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        ))

    def test_obsolete_userspace_case_is_accepted_as_not_applicable(self) -> None:
        case = runner.Case(
            "syscalls", "creat07", "creat07",
            "userspace-exempt", 60, "obsolete since Linux 6.11",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))

    def test_explicit_silent_userspace_case_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "fcntl01", "fcntl01",
            "userspace-exempt", 60, "upstream success path emits no result",
        )
        result = runner.Result(case, 0, {}, "")
        self.assertTrue(runner.accepted(result))

    def test_silent_result_is_not_accepted_as_a_pass(self) -> None:
        case = runner.Case(
            "syscalls", "silent", "silent", "pass", 60, "",
        )
        result = runner.Result(case, 0, {}, "")
        self.assertFalse(runner.accepted(result))

    def test_legacy_exit_zero_expectation_accepts_silent_success(self) -> None:
        case = runner.Case(
            "sched", "time-schedule01", "time-schedule",
            "exit-zero", 60, "legacy benchmark has no LTP result tags",
        )
        self.assertTrue(runner.accepted(runner.Result(case, 0, {}, "")))
        self.assertFalse(runner.accepted(runner.Result(case, 1, {}, "")))
        self.assertFalse(
            runner.accepted(runner.Result(case, 0, {"failed": 1}, ""))
        )

    def test_pass_with_ltp_warning_status_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "stress", "stress", "pass", 60, "",
        )
        result = runner.Result(
            case, 4, {"passed": 1, "failed": 0, "broken": 0,
                      "skipped": 0, "warnings": 1}, "",
        )
        self.assertTrue(runner.accepted(result))

    def test_warning_status_without_warning_or_pass_is_rejected(self) -> None:
        case = runner.Case(
            "syscalls", "stress", "stress", "pass", 60, "",
        )
        self.assertFalse(runner.accepted(
            runner.Result(case, 4, {"passed": 1, "warnings": 0}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 4, {"passed": 0, "warnings": 1}, "")
        ))

    def test_missing_packaged_binary_is_accepted_explicitly(self) -> None:
        case = runner.Case(
            "syscalls", "missing", "missing",
            "userspace-missing-exempt", 60, "not packaged for this ABI",
        )
        result = runner.Result(case, 127, {}, "missing: not found")
        self.assertTrue(runner.accepted(result))

    def test_missing_packaged_binary_contract_is_strict(self) -> None:
        case = runner.Case(
            "syscalls", "missing", "missing",
            "userspace-missing-exempt", 60, "not packaged for this ABI",
        )
        self.assertFalse(runner.accepted(runner.Result(case, 0, {}, "")))
        self.assertFalse(
            runner.accepted(runner.Result(case, 127, {"broken": 1}, ""))
        )

    def test_userspace_setup_break_is_accepted_explicitly(self) -> None:
        case = runner.Case(
            "containers", "netns_netlink", "netns_netlink",
            "userspace-broken-exempt", 60, "BusyBox ip lacks link add",
        )
        result = runner.Result(
            case, 2,
            {"passed": 0, "failed": 0, "broken": 1, "skipped": 0},
            "BusyBox ip usage followed by an LTP setup TBROK",
        )
        self.assertTrue(runner.accepted(result))

    def test_userspace_setup_break_contract_is_strict(self) -> None:
        case = runner.Case(
            "containers", "netns_netlink", "netns_netlink",
            "userspace-broken-exempt", 60, "BusyBox ip lacks link add",
        )
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"passed": 1, "broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        ))

    def test_variable_userspace_matrix_requires_a_passed_check(self) -> None:
        case = runner.Case(
            "controllers", "legacy", "legacy",
            "userspace-variable-matrix-exempt", 60,
            "obsolete mode produces timing-dependent result ordering",
        )
        for summary in (
            {"passed": 6, "failed": 3, "broken": 0, "skipped": 0},
            {"passed": 1, "failed": 0, "broken": 1, "skipped": 0},
            {"passed": 3, "failed": 1, "broken": 1, "skipped": 0},
            {"passed": 3, "failed": 4, "broken": 0, "skipped": 1},
        ):
            self.assertTrue(runner.accepted(runner.Result(case, 1, summary, "")))
        self.assertTrue(runner.accepted(runner.Result(
            case, 4,
            {
                "passed": 5,
                "failed": 0,
                "broken": 0,
                "skipped": 0,
                "warnings": 1,
            },
            "",
        )))
        self.assertTrue(runner.accepted(
            runner.Result(case, 0, {"passed": 1}, "")
        ))
        for return_code, summary in (
            (1, {"failed": 1}),
            (2, {"broken": 1}),
            (32, {"skipped": 1}),
        ):
            self.assertFalse(
                runner.accepted(runner.Result(case, return_code, summary, ""))
            )

    def test_raw_userspace_error_requires_nonzero_without_result_tags(self) -> None:
        case = runner.Case(
            "commands", "df01_sh", "df01.sh",
            "userspace-error-exempt", 60,
            "missing stat causes a shell arithmetic error",
        )
        self.assertTrue(runner.accepted(
            runner.Result(case, 2, {}, "arithmetic syntax error")
        ))
        self.assertFalse(runner.accepted(runner.Result(case, 0, {}, "")))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(runner.Result(case, 124, {}, "")))

    def test_static_kernel_module_case_is_accepted_as_not_applicable(self) -> None:
        case = runner.Case(
            "syscalls", "delete_module01", "delete_module01",
            "static-kernel-exempt", 60, "loadable modules intentionally disabled",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))

    def test_known_stale_userspace_failure_requires_a_real_failure(self) -> None:
        case = runner.Case(
            "syscalls", "ioctl_pidfd06", "ioctl_pidfd06",
            "userspace-failure-exempt", 60,
            "installed LTP expects the pre-backport errno",
        )
        stale = runner.Result(case, 1, {"failed": 1, "broken": 0}, "")
        fixed = runner.Result(case, 0, {"passed": 1, "failed": 0}, "")
        broken = runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        self.assertTrue(runner.accepted(stale))
        self.assertFalse(runner.accepted(fixed))
        self.assertFalse(runner.accepted(broken))

    def test_userspace_matrix_exemption_requires_failure_and_break(self) -> None:
        case = runner.Case(
            "controllers", "memcg_stat_rss", "memcg_stat_rss.sh",
            "userspace-matrix-exempt", 60,
            "fixed-size test reads batched counters before periodic flush",
        )
        stale = runner.Result(
            case, 7,
            {"passed": 2, "failed": 3, "broken": 1, "skipped": 0},
            "",
        )
        self.assertTrue(runner.accepted(stale))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 10}, "")
        ))
        self.assertTrue(runner.accepted(
            runner.Result(case, 1, {"failed": 3, "broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 1, {"passed": 2, "failed": 3}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"broken": 1}, "")
        ))

    def test_legacy_zero_exit_matrix_requires_mixed_results(self) -> None:
        case = runner.Case(
            "controllers", "legacy", "legacy",
            "userspace-legacy-matrix-exempt", 60,
            "legacy wrapper always exits zero",
        )
        self.assertTrue(runner.accepted(
            runner.Result(case, 0, {"passed": 14, "failed": 9}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 14}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"failed": 9}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(
                case, 0, {"passed": 14, "failed": 9, "broken": 1}, ""
            )
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 1, {"passed": 14, "failed": 9}, "")
        ))

    def test_partial_userspace_break_requires_pass_and_break(self) -> None:
        case = runner.Case(
            "commands", "mv01_sh", "mv_tests.sh",
            "userspace-partial-broken-exempt", 60,
            "supported BusyBox operation passes before an unsupported option",
        )
        self.assertTrue(runner.accepted(runner.Result(
            case, 2,
            {"passed": 1, "failed": 0, "broken": 1, "skipped": 0},
            "",
        )))
        self.assertTrue(runner.accepted(runner.Result(
            case, 34,
            {"passed": 5, "failed": 0, "broken": 1, "skipped": 1},
            "",
        )))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 1, "broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 3, {"passed": 1, "failed": 1, "broken": 1}, "")
        ))

    def test_userspace_warning_exemption_requires_warning_only_result(self) -> None:
        case = runner.Case(
            "controllers", "memcg_failcnt", "memcg_failcnt.sh",
            "userspace-warning-exempt", 60,
            "functional assertions pass before BusyBox cleanup warns",
        )
        warning = runner.Result(
            case, 4,
            {"passed": 3, "failed": 0, "broken": 0,
             "skipped": 0, "warnings": 1},
            "",
        )
        self.assertTrue(runner.accepted(warning))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 3}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 5, {"passed": 3, "failed": 1, "warnings": 1}, "")
        ))

    def test_flaky_userspace_exemption_accepts_only_known_outcomes(self) -> None:
        case = runner.Case(
            "controllers", "memcg_usage_in_bytes",
            "memcg_usage_in_bytes_test.sh",
            "userspace-flaky-exempt", 60,
            "kernel charge movement can trip the byte-exact helper delta",
        )
        warning = runner.Result(
            case, 4,
            {"passed": 4, "failed": 0, "broken": 0,
             "skipped": 0, "warnings": 1},
            "",
        )
        partial = runner.Result(
            case, 6,
            {"passed": 1, "failed": 0, "broken": 1, "skipped": 0},
            "",
        )
        self.assertTrue(runner.accepted(warning))
        self.assertTrue(runner.accepted(partial))
        self.assertFalse(runner.accepted(
            runner.Result(case, 7, {"passed": 1, "failed": 1, "broken": 1}, "")
        ))

    def test_parallel_runner_uses_requested_workers(self) -> None:
        barrier = threading.Barrier(2)
        worker_threads: set[int] = set()
        lock = threading.Lock()
        cases = [
            runner.Case("syscalls", f"case{index}", "true", "pass", 1, "")
            for index in range(2)
        ]

        def worker(emulator, rootfs, memory, case):
            del emulator, rootfs, memory
            self.assertEqual(case.kernel_arguments, ("ncpus=1",))
            with lock:
                worker_threads.add(threading.get_ident())
            barrier.wait(timeout=2)
            return runner.Result(case, 0, {"passed": 1}, "")

        results = list(runner.run_cases(
            pathlib.Path("emulator"),
            pathlib.Path("rootfs"),
            "1M",
            cases,
            2,
            worker,
        ))
        self.assertEqual({index for index, _ in results}, {1, 2})
        self.assertEqual(len(worker_threads), 2)

    def test_host_pressure_runner_preserves_manifest_cpu_topology(self) -> None:
        observed: list[tuple[str, ...]] = []
        case = runner.Case(
            "syscalls", "explicit-smp", "true", "pass", 1, "",
            host_pressure=True,
            kernel_arguments=("ncpus=4",),
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory
            observed.append(selected.kernel_arguments)
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            [case], 2, worker,
        ))
        self.assertEqual(observed, [("ncpus=4",)])

    def test_single_worker_limits_only_ordinary_guest_topology(self) -> None:
        observed: list[tuple[str, tuple[str, ...]]] = []
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
            kernel_arguments=("console=tty0", "ncpus=8"),
        )
        pressure = runner.Case(
            "sched", "smp", "true", "pass", 1, "",
            host_pressure=True,
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory
            observed.append((selected.test_id, selected.kernel_arguments))
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            [pressure, ordinary], 1, worker,
        ))
        self.assertEqual(observed, [
            ("ordinary", ("console=tty0", "ncpus=1")),
            ("smp", ()),
        ])

    def test_cases_from_returns_manifest_suffix(self) -> None:
        cases = [
            runner.Case("syscalls", name, "true", "pass", 1, "")
            for name in ("first", "second", "third")
        ]
        self.assertEqual(
            [case.key for case in runner.cases_from(cases, "syscalls/second")],
            ["syscalls/second", "syscalls/third"],
        )

    def test_cases_from_rejects_unknown_key(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown start case"):
            runner.cases_from([], "syscalls/missing")

    def test_host_pressure_cases_are_excluded_by_default(self) -> None:
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
        )
        pressure = runner.Case(
            "stress", "pressure", "true", "pass", 1, "",
            host_pressure=True,
        )
        selected, skipped = runner.select_host_pressure_cases(
            [ordinary, pressure], False
        )
        self.assertEqual(selected, [ordinary])
        self.assertEqual(skipped, 1)

    def test_host_pressure_cases_require_explicit_cli_option(self) -> None:
        required = [
            "--emulator", "uml-darwin",
            "--rootfs", "rootfs.erofs",
            "--manifest", "verified-tests.tsv",
            "--modules-builtin", "kernel-build/modules.builtin",
        ]
        defaults = runner.parse_arguments(required)
        explicit = runner.parse_arguments(required + ["--run-host-pressure"])
        self.assertFalse(defaults.run_host_pressure)
        self.assertTrue(explicit.run_host_pressure)

    def test_ltp_cmake_campaign_feeds_exact_kernel_metadata(self) -> None:
        cmake = (LTP_ROOT.parent / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            "--modules-builtin ${UML_KERNEL_BUILD_DIR}/modules.builtin",
            cmake,
        )
        self.assertNotIn('command -v modprobe', cmake)
        self.assertNotIn('command -v rmmod', cmake)

    def test_explicit_host_pressure_selection_preserves_all_cases(self) -> None:
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
        )
        pressure = runner.Case(
            "stress", "pressure", "true", "pass", 1, "",
            host_pressure=True,
        )
        cases = [ordinary, pressure]
        selected, skipped = runner.select_host_pressure_cases(cases, True)
        self.assertIs(selected, cases)
        self.assertEqual(skipped, 0)

    def test_case_memory_override_is_forwarded_to_worker(self) -> None:
        observed: list[str] = []
        case = runner.Case(
            "syscalls", "large", "true", "pass", 1, "large scratch", "4G"
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs
            observed.append(memory)
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1G",
            [case], 1, worker,
        ))
        self.assertEqual(observed, ["4G"])

    def test_exclusive_cases_run_in_order_after_parallel_cases(self) -> None:
        barrier = threading.Barrier(2)
        lock = threading.Lock()
        regular_completed = 0
        serial_completed = 0
        exclusive_active = 0
        maximum_exclusive_active = 0
        completion_order: list[str] = []
        cases = [
            runner.Case("syscalls", f"regular{index}", "true", "pass", 1, "")
            for index in range(2)
        ] + [runner.Case(
            "syscalls", "serial0", "true", "pass", 1, "", serial=True,
        )] + [
            runner.Case(
                "stress", f"pressure{index}", "true", "pass", 1, "",
                host_pressure=True,
            )
            for index in range(2)
        ]

        def worker(emulator, rootfs, memory, case):
            nonlocal regular_completed, serial_completed, exclusive_active
            nonlocal maximum_exclusive_active
            del emulator, rootfs, memory
            if not case.host_pressure and not case.serial:
                barrier.wait(timeout=2)
                with lock:
                    regular_completed += 1
                    completion_order.append(case.test_id)
            else:
                self.assertEqual(case.kernel_arguments, ())
                with lock:
                    self.assertEqual(regular_completed, 2)
                    if case.host_pressure:
                        self.assertEqual(serial_completed, 1)
                    exclusive_active += 1
                    maximum_exclusive_active = max(
                        maximum_exclusive_active, exclusive_active
                    )
                    completion_order.append(case.test_id)
                    if case.serial:
                        serial_completed += 1
                    exclusive_active -= 1
            return runner.Result(case, 0, {"passed": 1}, "")

        results = list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            cases, 2, worker,
        ))
        self.assertEqual({index for index, _ in results}, {1, 2, 3, 4, 5})
        self.assertEqual(
            completion_order[-3:], ["serial0", "pressure0", "pressure1"]
        )
        self.assertEqual(maximum_exclusive_active, 1)

    def test_parallel_interrupt_terminates_active_guests(self) -> None:
        case = runner.Case(
            "syscalls", "interrupt", "true", "pass", 1, ""
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory, selected
            raise KeyboardInterrupt

        with mock.patch.object(runner, "terminate_active_guests") as terminate:
            with self.assertRaises(KeyboardInterrupt):
                list(runner.run_cases(
                    pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
                    [case], 2, worker,
                ))
        terminate.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
