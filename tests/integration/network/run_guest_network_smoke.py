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
import shlex
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402
from slirp_network import (  # noqa: E402
    NODE,
    start_guest_dhcp_client,
    verify_slirp_lease,
    wait_for_slirp_lease,
)


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


def run_node(guest: Guest, script: str, timeout: int = 30) -> str:
    return guest.check_command(f"{NODE} -e {shlex.quote(script)}", timeout)


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
