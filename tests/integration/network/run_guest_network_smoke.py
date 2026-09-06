#!/usr/bin/env python3
"""Exercise the deterministic SLiRP guest network without external traffic.

The guest kernel deliberately has no ``CONFIG_IP_PNP``, so nothing is
configured before userspace starts and guest userspace owns every address.
This init-shell boot has no init system to run a DHCP client for it, so the
runner starts the one the product rootfs ships, ``systemd-networkd``, and then
proves the emulator's user-mode network answered with the documented lease:
address 10.0.2.15/24 and a default route through the gateway 10.0.2.2.

The default campaign reaches only the SLiRP host gateway.  An explicit
``--external`` switch is deliberately required before the guest resolves or
contacts an Internet host; CMake registers that variant only when the caller
has opted into external-network permission.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import pathlib
import re
import shlex
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


NODE = "/opt/node/bin/node"
NETWORKD = "/lib/systemd/systemd-networkd"
NETWORKD_LOG = "/run/uml-networkd.log"
NETWORKD_RUNTIME = "/run/systemd/netif"
NETWORK_DIRECTORY = "/run/systemd/network"
NETWORK_UNIT = f"{NETWORK_DIRECTORY}/10-eth0.network"
LEASE_TIMEOUT_SECONDS = 30
LEASE_POLL_SECONDS = 0.25
# 0202000A is the little-endian /proc/net/route spelling of 10.0.2.2.
SLIRP_DEFAULT_ROUTE = re.compile(r"^eth0\s+00000000\s+0202000A\s+", re.MULTILINE)
SLIRP_LEASE_ADDRESS = re.compile(r"\binet 10\.0\.2\.15/24\b")
SLIRP_DNS_PROBE = (
    'const dgram=require("dgram");'
    'const socket=dgram.createSocket("udp4");'
    'const query=Buffer.from('
    '"c0de01000001000000000000096c6f63616c686f73740000010001","hex");'
    "let done=false;"
    "const finish=(message)=>{"
    "if(done)return;done=true;clearTimeout(timer);socket.close();"
    "if(message){console.error(message);process.exitCode=1;}"
    'else console.log("slirp-dns-ok");};'
    'const timer=setTimeout(()=>finish("SLiRP DNS request timed out"),5000);'
    'socket.on("error",error=>finish(error.message));'
    'socket.on("message",(response,remote)=>{'
    'if(remote.address!=="10.0.2.3"||response.length<12||'
    'response.readUInt16BE(0)!==0xc0de){'
    'finish("invalid SLiRP DNS response");return;}finish();});'
    'socket.send(query,53,"10.0.2.3");'
)


def run(guest: Guest, command: str, timeout: int = 30) -> str:
    result = guest.run_command(command, timeout)
    if result.return_code != 0:
        raise RuntimeError(
            f"guest network command failed ({result.return_code}): {command}\n"
            f"{result.output}"
        )
    return result.output


def run_node(guest: Guest, script: str, timeout: int = 30) -> str:
    return run(guest, f"{NODE} -e {shlex.quote(script)}", timeout)


def start_guest_dhcp_client(guest: Guest) -> None:
    """Start the DHCP client the product rootfs ships on a bare init shell.

    ``systemd-networkd`` is the rootfs's only DHCP client, and here it runs
    without the rest of systemd, so the runner supplies the three things its
    unit would have provided.
    """
    run(guest, "mount -t proc proc /proc")
    # systemd reads a writable /sys as "udev runs here" and then parks every
    # link in "pending udev initialization" indefinitely.  A read-only /sys is
    # its documented convention for a userspace that has no udev, which is
    # exactly this boot.
    run(guest, "mount -t sysfs -o ro sysfs /sys")
    # The product root is read-only EROFS, so the drop-in and the daemon's
    # runtime state both live on a disposable tmpfs.
    run(guest, "mount -t tmpfs tmpfs /run")
    run(guest, f"mkdir -p {NETWORK_DIRECTORY}")
    # networkd drops to the systemd-network user and refuses a runtime
    # directory owned by anyone else, which is what its unit's
    # RuntimeDirectory= would have created for it.
    run(
        guest,
        "install -d -o systemd-network -g systemd-network -m 0755 "
        f"{NETWORKD_RUNTIME}",
    )
    run(
        guest,
        "printf '%s\\n' '[Match]' 'Name=eth0' '[Network]' 'DHCP=ipv4' "
        f"> {NETWORK_UNIT}",
    )
    # The console harness appends "; rc=$?" to every command, so a trailing
    # ampersand would be a shell syntax error.  The subshell detaches the
    # daemon and keeps that wrapper valid.
    run(guest, f"({NETWORKD} >{NETWORKD_LOG} 2>&1 </dev/null &)")


def wait_for_slirp_lease(
    guest: Guest, timeout_seconds: int = LEASE_TIMEOUT_SECONDS
) -> tuple[str, str, str]:
    """Return the guest routing, interface and address tables once DHCP lands.

    The lease normally arrives well inside a second; the wait exists so a
    loaded host reports the daemon's own diagnosis instead of an empty table.
    """
    deadline = time.monotonic() + timeout_seconds
    while True:
        route_table = run(guest, "cat /proc/net/route")
        if SLIRP_DEFAULT_ROUTE.search(route_table) is not None:
            return (
                route_table,
                run(guest, "cat /proc/net/dev"),
                run(guest, "ip -4 -oneline addr show eth0"),
            )
        if time.monotonic() >= deadline:
            raise RuntimeError(
                "guest userspace did not acquire the SLiRP DHCP lease within "
                f"{timeout_seconds}s:\n{route_table}\n"
                f"{run(guest, f'cat {NETWORKD_LOG}')}"
            )
        time.sleep(LEASE_POLL_SECONDS)


def verify_slirp_lease(
    route_table: str, interfaces: str, addresses: str
) -> None:
    if SLIRP_DEFAULT_ROUTE.search(route_table) is None:
        raise RuntimeError(
            "missing default route through the SLiRP gateway 10.0.2.2:\n"
            f"{route_table}"
        )
    if re.search(r"^\s*eth0:", interfaces, re.MULTILINE) is None:
        raise RuntimeError(f"SLiRP eth0 interface is missing:\n{interfaces}")
    # The single SLiRP lease is also the default target of a host forward, so
    # the address the guest is handed is part of the network's contract.
    if SLIRP_LEASE_ADDRESS.search(addresses) is None:
        raise RuntimeError(
            f"eth0 did not receive the SLiRP lease 10.0.2.15/24:\n{addresses}"
        )


def external_probe(host: str) -> str:
    hostname = json.dumps(host)
    return (
        'const dns=require("dns");const https=require("https");'
        'dns.setServers(["10.0.2.3"]);'
        f"const hostname={hostname};"
        "dns.resolve4(hostname,(error,addresses)=>{"
        "if(error)throw error;"
        'if(!addresses.length)throw new Error("DNS lookup returned no address");'
        "const request=https.get({host:addresses[0],servername:hostname,"
        'headers:{Host:hostname},path:"/"},response=>{'
        "if(response.statusCode<200||response.statusCode>=400)"
        "process.exitCode=2;"
        "response.resume();response.on('end',()=>console.log('external-ok'));"
        "});request.on('error',error=>{throw error});});"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--external", action="store_true")
    parser.add_argument("--external-host", default="example.com")
    arguments = parser.parse_args()

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (),
        # The production rootfs is raw EROFS and is intentionally entered
        # through a temporary init shell, which keeps this smoke to the
        # emulator's network device and the guest's own DHCP client.
        # guest-overlay-systemd-smoke separately proves the overlay pivot and
        # a real systemd boot.
        init_shell=True,
    )
    try:
        guest.boot_to_shell(180)
        start_guest_dhcp_client(guest)
        verify_slirp_lease(*wait_for_slirp_lease(guest))
        if arguments.external:
            dns_output = run_node(guest, SLIRP_DNS_PROBE, 15)
            if "slirp-dns-ok" not in dns_output:
                raise RuntimeError(
                    "SLiRP DNS probe did not report success; the external "
                    f"network campaign requires host-network permission:\n{dns_output}"
                )
            output = run_node(guest, external_probe(arguments.external_host), 60)
            if "external-ok" not in output:
                raise RuntimeError(
                    f"external connectivity probe did not report success:\n{output}"
                )
            print(f"external connectivity verified for {arguments.external_host}")
        else:
            print("deterministic SLiRP gateway connectivity verified")
        return 0
    except BaseException:
        print(guest.output(), file=sys.stderr)
        raise
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
