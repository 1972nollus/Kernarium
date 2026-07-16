# Darwin UML runtime

This directory contains the macOS host half of the AArch64 UML port.  Code
that implements Linux architecture contracts stays in `linux-6.18/arch/um`
and `linux-6.18/arch/arm64/um`; code that deals only with the Darwin launcher,
Mach exception transport, 4 KiB shadow MMU, and network backend lives here.
The release consists of a kernel/runtime executable and a separate ext2 root
filesystem image.

## Build and run

From the project root:

```sh
gmake -j8 uml-darwin
cd .build/darwin
./uml-darwin
```

The launcher finds a writable `rootfs.ext2` beside the executable, then in the
current directory, or at `UML_ROOTFS`.  Unless explicitly overridden it adds
`ubd0=... root=/dev/ubda init=/sbin/init mem=256M rw`.  The executable embeds
the UML kernel and Darwin runtime, but never embeds the rootfs.

## User-mode transition

Linux AArch64 binaries issue system calls with `svc #0`, the syscall number in
`x8`, arguments in `x0` through `x5`, and the result in `x0`.  XNU consumes an
`svc` directly as a Darwin system-call entry.  It does not offer the Linux
register convention to a POSIX signal handler or task exception port, and an
unmodified Linux `svc #0` can therefore never be allowed to execute.

The backend gives executable guest mappings a private shadow and replaces
the aligned AArch64 instruction `svc #0` (`0xd4000001`) with the reserved
breakpoint `brk #0x4c4`.  A task-local Mach exception port catches that
breakpoint, saves the complete `ARM_THREAD_STATE64`, and redirects the same
Darwin thread to its UML kernel stack.  The Linux syscall dispatcher consumes
the saved state and later resumes the thread at the instruction after the
breakpoint.  Executable pages remain byte-for-byte intact in guest physical
memory; only their Darwin execution shadows are patched.

The programs in `probes/` are executable platform assertions for this
contract.  Run them with `gmake probes` from the project root.

## 4 KiB guest MMU

Apple Silicon uses 16 KiB host VM pages while arm64 Linux applications commonly
use 4 KiB pages.  Low guest-user addresses therefore use private 4 KiB shadow
quarters inside 16 KiB Darwin slots.  The runtime synchronizes dirty quarters
with UML physical memory, preserves independent Linux permissions, and switches
the active shadow generation when Linux changes `mm`.  Before a real fork,
first-write tracking flushes only modified slots and closes their host write
permission so Linux COW remains authoritative.  A broad Linux TLB update records
metadata for unloaded slots without materializing them; the exact faulting slot
is reconstructed on demand.  Mapping and discovered-code targets are
hash-indexed, and a loaded 16 KiB slot always contains every valid 4 KiB sibling.

## Networking and virtio assessment

The built-in `eth0` driver exchanges Ethernet frames directly with the host
runtime.  The runtime uses libslirp for unprivileged NAT, gateway `10.0.2.2`,
DNS `10.0.2.3`, and guest address `10.0.2.15/24`.  A kqueue helper wakes the UML
idle pthread for readable host descriptors, and the interrupt handler drains
the receive ring.  `gmake network-probe` verifies the host-side ARP path.

Linux 6.18 also contains `CONFIG_VIRTIO_UML`, a vhost-user socket transport;
with `CONFIG_VIRTIO_NET` it can instantiate a network device using
`virtio_uml.device=<socket>:1`.  It is not enabled for the Darwin release:
virtio would put vring negotiation, kick/call descriptors, and a vhost-user
protocol in front of the same fd wake path.  The measured latency defect was in
that shared wake/timer layer, not the direct frame transport.  After fixing it,
the direct in-process driver measures roughly 0.12--0.82 ms to the SLiRP gateway
and is the lower-overhead design.  Virtio remains useful as a future optional
compatibility/offload path, not as a latency fix.

## Performance

Host stack and event profiling identified unconditional shadow reconstruction,
linear discovered-code lookup, and repeated whole-`mm` replay as the important
costs.  Backing-page change detection, mapping/code-target hashes, direct syscall
coroutines, and metadata-only replay of unloaded PTEs reduce the measured
`/sbin/init` ready time to about 0.23--0.35 seconds.  On the same system,
`neofetch` completes in 3.54 seconds cold and 3.25 seconds warm; 100 executions
of `/bin/true` take 1.19 seconds and 50 pipelines take 1.33 seconds.  Set
`UML_PROFILE_TRAPS=1` for trap, syscall, boundary, and shadow-loader counters,
or `UML_TIMER_TRACE=1` only when diagnosing host timer setup.
