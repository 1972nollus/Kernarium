#!/usr/bin/env python3
"""Unit tests for result parsing and LTP expectation classification."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import pathlib
import sys
import tempfile
import threading
import unittest
from unittest import mock

LTP_ROOT = pathlib.Path(__file__).resolve().parents[2] / "integration/ltp"
sys.path.insert(0, str(LTP_ROOT))
import run_guest_ltp as runner  # noqa: E402


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
        self.assertEqual(set(disabled), expected)
        self.assertTrue(expected.isdisjoint(verified))

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

    def test_guest_disk_is_declared_read_only(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.img"), "1G"
        )
        self.assertIn("--execution-engine=jit", arguments)
        self.assertIn("ubd0r=rootfs.img", arguments)
        self.assertNotIn("ubd0=rootfs.img", arguments)
        self.assertIn("ro", arguments)
        self.assertNotIn("uml_ltp_workspace=ext2", arguments)

    def test_boot_timeout_allows_parallel_host_startup_contention(self) -> None:
        self.assertGreaterEqual(runner.BOOT_TIMEOUT_SECONDS, 60)

    def test_overlay_shell_bootstrap_mounts_only_disposable_writable_state(self) -> None:
        bootstrap = runner.OVERLAY_SHELL_BOOTSTRAP
        self.assertIn("lowerdir=/mnt/lower,upperdir=/mnt/upper", bootstrap)
        self.assertIn("mount -t proc proc /proc", bootstrap)
        self.assertIn("mount -t sysfs sysfs /sys", bootstrap)
        self.assertIn("mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp", bootstrap)
        self.assertIn("exec /bin/sh", bootstrap)
        self.assertNotIn("exec /lib/systemd/systemd", bootstrap)

    def test_systemd_overlay_bootstrap_mounts_only_disposable_writable_state(self) -> None:
        bootstrap = runner.SYSTEMD_OVERLAY_BOOTSTRAP
        self.assertIn("lowerdir=/mnt/lower,upperdir=/mnt/upper", bootstrap)
        self.assertIn("pivot_root . .uml-lower", bootstrap)
        self.assertIn("exec /lib/systemd/systemd", bootstrap)
        self.assertNotIn("mount -o remount,rw", bootstrap)

    def test_guest_shell_boot_modes_are_mutually_exclusive(self) -> None:
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            runner.Guest(
                pathlib.Path("uml-darwin"),
                pathlib.Path("rootfs.img"),
                "1G",
                "tmpfs",
                init_shell=True,
                overlay_shell=True,
            )
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            runner.Guest(
                pathlib.Path("uml-darwin"),
                pathlib.Path("rootfs.img"),
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
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.img"),
            "1G", "ext2",
        )
        self.assertIn("uml_ltp_workspace=ext2", arguments)

    def test_kernel_arguments_are_appended(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.img"),
            "1G", kernel_arguments=("ncpus=2",),
        )
        self.assertEqual(arguments[-1], "ncpus=2")

    def test_disposable_swap_disk_is_separate_from_rootfs(self) -> None:
        arguments = runner.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.img"),
            "1G", swap_path=pathlib.Path("/tmp/swap.img"),
        )
        self.assertIn("ubd0r=rootfs.img", arguments)
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
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.img"), "1G",
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
            "/usr/lib/ltp-testsuite/output &&",
            command,
        )
        self.assertIn(
            "chmod 1777 /tmp/uml-ltp-case "
            "/usr/lib/ltp-testsuite/output &&",
            command,
        )
        self.assertIn(
            "cd /usr/lib/ltp-testsuite/testcases/bin &&", command
        )
        self.assertIn("LTPROOT=/usr/lib/ltp-testsuite", command)
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
            "--rootfs", "rootfs.img",
            "--manifest", "verified-tests.tsv",
        ]
        defaults = runner.parse_arguments(required)
        explicit = runner.parse_arguments(required + ["--run-host-pressure"])
        self.assertFalse(defaults.run_host_pressure)
        self.assertTrue(explicit.run_host_pressure)

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
