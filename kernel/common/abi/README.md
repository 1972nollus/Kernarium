# Darwin UML ABI identities

The installed emulator API and host/Linux bridge are independent ABI domains.
A change in one domain must not advance the other's version or fingerprint.

Each domain's `identity.env` is the only hand-edited copy of its version and
fingerprint. The fingerprint is the first 64 bits of the SHA-256 of the
domain's normalized declarations, which
`verify-declarations.py --compute --domain <kernel|public>` prints. The
verifier rejects an `identity.env` whose fingerprint no longer matches the
declarations. Comment and whitespace changes do not change a fingerprint;
declarations, layouts, enums, callbacks, and exported functions do.

`sync.sh` writes each identity header from its `identity.env` and copies the
canonical bridge headers. `sync.sh --verify` requires every destination to be
byte-identical to what a write would produce. Do not edit a generated header.

## Installed public API

Canonical declarations live in `darwin/api/include/uml/` and the identity in
`darwin/api/abi/identity.env`, from which `uml/abi_identity.h` is generated.
These headers are installed with `libuml_emulator.a` and are not copied into
Linux.

For an intentional declaration change, set the new version and the computed
public fingerprint in `darwin/api/abi/identity.env`, run
`kernel/common/abi/sync.sh`, and rebuild the external C, C++, and Objective-C
consumers. Public-only changes do not alter the kernel bridge or patch series.

## Host/Linux bridge

Canonical bridge declarations, `identity.env`, and the import policy live in
this directory. `sync.sh` generates `uml_darwin_abi_identity.h` and copies the
other headers to `darwin/include/` and, with `--kernel-source`, to a prepared
kernel replay. The host-only contracts `darwin/include/uml_darwin_guest.h`,
`uml_darwin_static_mmu.h`, and `uml_elf64.h` are authored in place and are not
sync outputs.

Every arm64 task's register tail is its FP registers, then its SME registers,
552 bytes whether or not the run implements SME; an x86_64-userspace task's
tail is its FP registers alone. The host requires exactly the slice's size on
every entry.

An AArch32 task of an arm64 kernel uses the same register image, which
`struct darwin_uml_guest_registers` documents field by field, with TPIDRURO in
its last field, `tpidrro`. The arm64-only imports
`darwin_uml_guest_compat_hwcap()` and `darwin_uml_guest_compat_hwcap2()` give
the task's AT_HWCAP words. The first is zero exactly when the run's engine
does not execute AArch32 or the guest-AArch32 policy is off, so no separate
query exists, and the kernel then refuses 32-bit ELF files. The x86_64 slice
neither imports them nor builds AArch32.

For an intentional bridge declaration change:

1. set the new version and the computed kernel fingerprint in `identity.env`;
2. run `kernel/common/abi/sync.sh --kernel-source <replay>` and regenerate the
   kernel topic that carries the change; and
3. rebuild and re-accept the four kernel artifacts (mainline arm64 16 KiB and
   4 KiB, mainline x86_64 4 KiB, and Android arm64 4 KiB).

A change to what a bridge declaration promises, with no change to its form,
takes the same three steps with a new version and the unchanged fingerprint.

Each artifact descriptor records the bridge identity it was built with, and
configure rejects an artifact whose identity differs from `identity.env`.

`bridge_imports.py` owns the import policy. A kernel image imports exactly
the declared `darwin_uml_*` functions, less those `import-policy.tsv` limits
to 4 KiB pages (the native-shadow services) or to the arm64 slice, and never
Darwin's process-wide `sigprocmask` or variadic `fcntl`. Every kernel build
runs `bridge_imports.py --kernel-binary` on its image, the artifact verifier
applies the same check to each bundle, and `--print-expected` lists the set
for a page size and slice.
