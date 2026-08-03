# Darwin UML ABI source

`identity.env`, the `darwin_uml_*.h` files, and `uml_tlb_sync_ranges.h` are the
single plain-C sources for the host/kernel boundary. The bridge umbrella
preserves source compatibility while CPU, host-service, memory, and network
consumers include their narrow contract. Do not edit copied destinations below
`darwin/include` or a replayed Linux tree.

Run `kernel/abi/sync.sh --kernel-source /path/to/linux` to update both sides,
or add `--verify` in CI and before a kernel build. Sync also runs
`verify-declarations.py`, which removes comments, token-normalizes the canonical
headers plus the installed `darwin/api/include/uml/emulator.h`, and binds their
SHA-256 to `declaration-history.tsv`. Whitespace and comments do not change the
identity; declarations, layouts, enums, callbacks, and public functions do.

For an intentional ABI declaration change, run
`kernel/abi/verify-declarations.py --compute`, increment
`DARWIN_UML_ABI_VERSION`, set `DARWIN_UML_ABI_FINGERPRINT` to the reported
derived fingerprint, and append the version, fingerprint, declaration digest,
and rationale to `declaration-history.tsv`. Then run `kernel/abi/sync.sh` and
regenerate the Linux patch series. Verification rejects changed declarations
without a new history row as well as drift in copied headers, generated
identity checks, kernel-series metadata, or the public package ABI.
