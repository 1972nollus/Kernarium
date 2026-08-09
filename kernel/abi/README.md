# Darwin UML ABI identities

The installed emulator API and the host/Linux bridge are independent ABI
domains. A declaration change in one domain must not advance the other domain's
version, fingerprint, or history.

## Installed public API

The canonical public declarations are
`darwin/api/include/uml/emulator.h` and `uml/abi_identity.h`. Their selected
identity is in `darwin/api/abi/identity.env`; their independent history is in
`darwin/api/abi/declaration-history.tsv`. These headers are installed with
`libuml_emulator.a` and are not copied into the Linux replay.

For an intentional public declaration change:

1. run `kernel/abi/verify-declarations.py --domain public --compute`;
2. increment `UML_EMULATOR_ABI_VERSION`, install the reported derived
   fingerprint in `darwin/api/abi/identity.env` and
   `darwin/api/include/uml/abi_identity.h`, and append the new version,
   fingerprint, declaration SHA-256, and rationale to the public history; and
3. run `kernel/abi/verify-declarations.py --domain public`, then rebuild the
   C and C++ package consumers.

Public-only changes do not regenerate the Linux patch series or alter the
kernel bridge identity.

## Kernel bridge

`kernel/abi/identity.env`, the `darwin_uml_*.h` files, and
`uml_tlb_sync_ranges.h` are the canonical plain-C host/Linux bridge. The bridge
umbrella preserves source compatibility while CPU, host-service, memory, and
network consumers include their narrow contracts. `kernel/abi/sync.sh` is the
exact generated-copy authority. Its host outputs under `darwin/include/` are
`uml_darwin_abi.h`, `uml_darwin_abi_identity.h`, `uml_darwin_cpu.h`,
`uml_darwin_host.h`, `uml_darwin_memory.h`, `uml_darwin_network.h`,
`uml_darwin_bridge.h`, and `uml/tlb_sync_ranges.h`; corresponding replay
headers are generated too. Do not edit those copies directly.
The host-only `darwin/include/uml_darwin_guest.h`,
`darwin/include/uml_darwin_static_mmu.h`, and `darwin/include/uml_elf64.h` are
authored in place and are not sync outputs.

For an intentional bridge declaration change:

1. run `kernel/abi/verify-declarations.py --domain kernel --compute`;
2. increment `DARWIN_UML_ABI_VERSION`, install the derived fingerprint in
   `kernel/abi/identity.env` and `darwin_uml_abi_identity.h`, append the new row
   to `kernel/abi/declaration-history.tsv`, and update the matching kernel-series
   metadata identity;
3. run `kernel/abi/sync.sh` to update the host copies, or add
   `--kernel-source /path/to/linux` to update a prepared replay as well; and
4. verify the narrowed Linux imports and regenerate the affected topic patch.

Use `kernel/abi/sync.sh --verify` (and `--kernel-source` when applicable) before
a build. Comment and whitespace changes do not alter either identity;
declarations, layouts, enums, callbacks, and public functions do. Running
`kernel/abi/verify-declarations.py` without `--domain` verifies both independent
histories and rejects cross-domain drift.

Every kernel build also runs `verify-linux-imports.sh --page-size 16k|4k`.
`import-policy.tsv` identifies the seven native-shadow services that are active
only for a 4 KiB guest; all other synchronized declarations are active for both
page sizes. The verifier requires the image's undefined `darwin_uml_*` symbols
to exactly match that page size's active declaration set, rejects inactive
imports, and rejects active declarations that the image no longer consumes.
