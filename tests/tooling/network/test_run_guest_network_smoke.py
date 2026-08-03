#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = (Path(__file__).resolve().parents[2] /
          "integration/network/run_guest_network_smoke.py")
SPEC = importlib.util.spec_from_file_location("run_guest_network_smoke", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
run_guest_network_smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_guest_network_smoke)


class FakeGuest:
    def boot_to_shell(self, timeout: int) -> None:
        del timeout

    def output(self) -> str:
        return ""

    def close(self) -> None:
        pass


class NetworkSmokeTaxonomyTests(unittest.TestCase):
    def run_campaign(self, external: bool) -> list[str]:
        arguments = [
            "run_guest_network_smoke.py",
            "--emulator",
            "/tmp/uml-darwin",
            "--rootfs",
            "/tmp/rootfs.img",
        ]
        if external:
            arguments.extend(["--external", "--external-host", "example.test"])

        scripts: list[str] = []

        def run_node(_guest: FakeGuest, script: str, _timeout: int) -> str:
            scripts.append(script)
            if script == run_guest_network_smoke.SLIRP_DNS_PROBE:
                return "slirp-dns-ok"
            return "external-ok"

        with (
            mock.patch.object(sys, "argv", arguments),
            mock.patch.object(
                run_guest_network_smoke, "Guest", return_value=FakeGuest()
            ),
            mock.patch.object(run_guest_network_smoke, "run", return_value=""),
            mock.patch.object(
                run_guest_network_smoke, "verify_slirp_route"
            ) as verify_route,
            mock.patch.object(
                run_guest_network_smoke, "run_node", side_effect=run_node
            ),
        ):
            self.assertEqual(run_guest_network_smoke.main(), 0)
            verify_route.assert_called_once_with("", "")
        return scripts

    def test_default_campaign_does_not_issue_dns(self) -> None:
        self.assertEqual(self.run_campaign(external=False), [])

    def test_external_campaign_issues_dns_before_https(self) -> None:
        scripts = self.run_campaign(external=True)
        self.assertEqual(scripts[0], run_guest_network_smoke.SLIRP_DNS_PROBE)
        self.assertEqual(len(scripts), 2)
        self.assertIn("example.test", scripts[1])


if __name__ == "__main__":
    unittest.main()
