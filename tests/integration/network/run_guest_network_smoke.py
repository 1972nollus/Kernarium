#!/usr/bin/env python3
"""Exercise the deterministic SLiRP guest network without external traffic.

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


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "ltp"))

from run_guest_ltp import Guest  # noqa: E402


NODE = "/root/.nvm/versions/node/v24.18.0/bin/node"
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


def verify_slirp_route(route_table: str, interfaces: str) -> None:
    if re.search(
        r"^eth0\s+00000000\s+0202000A\s+", route_table, re.MULTILINE
    ) is None:
        raise RuntimeError(
            "missing default route through the SLiRP gateway 10.0.2.2:\n"
            f"{route_table}"
        )
    if re.search(r"^\s*eth0:", interfaces, re.MULTILINE) is None:
        raise RuntimeError(f"SLiRP eth0 interface is missing:\n{interfaces}")


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
        ("root=/dev/ubda1",),
        # The production rootfs is partitioned and is intentionally entered
        # through a temporary init shell.  This smoke exercises the kernel
        # DHCP/SLiRP path there; guest-overlay-systemd-smoke separately proves
        # the overlay pivot and real systemd boot.
        init_shell=True,
    )
    try:
        guest.boot_to_shell(180)
        run(guest, "mount -t proc proc /proc")
        verify_slirp_route(
            run(guest, "cat /proc/net/route"),
            run(guest, "cat /proc/net/dev"),
        )
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
