# Darwin UML ABI identities

The installed emulator API and host/Linux bridge are independent ABI domains.
A change in one domain must not advance the other's version, fingerprint, or
history.

## Installed public API

Canonical declarations live in `darwin/api/include/uml/`. Identity and history
live in `darwin/api/abi/`. These headers are installed with
`libuml_emulator.a` and are not copied into Linux.

For an intentional declaration change, compute the public-domain fingerprint
with `verify-declarations.py`, advance the version and identity, append the
history row, verify the domain, and rebuild the external C, C++, and
Objective-C consumers. Public-only changes do not alter the kernel bridge or
patch series.

## Host/Linux bridge

Canonical bridge declarations, identity, history, and import policy live in
this directory. `sync.sh` is the exact generated-copy authority for the host
headers under `darwin/include/` and the corresponding headers in prepared
kernel replays. Do not edit generated copies directly.

The host-only contracts `darwin/include/uml_darwin_guest.h`,
`uml_darwin_static_mmu.h`, and `uml_elf64.h` are authored in place and are not
sync outputs.

For an intentional bridge declaration change:

1. compute and review the kernel-domain fingerprint;
2. advance the bridge version, identity, and history;
3. update matching versioned-kernel metadata;
4. run `kernel/common/abi/sync.sh`; and
5. regenerate and verify every affected kernel topic and import set.

Use `kernel/common/abi/sync.sh --verify` for ordinary host verification. A
versioned kernel line supplies its metadata and replay source explicitly; the
common tool has no implicit family owner.

Comment and whitespace changes do not change an ABI identity. Declarations,
layouts, enums, callbacks, and exported functions do. The declaration verifier
checks both histories and rejects cross-domain drift.

Every kernel build also verifies its exact active `darwin_uml_*` imports for
the selected page profile. The 4 KiB native-shadow services are inactive in
the 16 KiB profile; all other shared declarations apply to both.
