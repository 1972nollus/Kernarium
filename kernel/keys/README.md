# Linux release signing key

`38DBBDC86092693E.asc` is a minimal OpenPGP export of Greg Kroah-Hartman's
Linux stable release-signing key from the official kernel.org developer
keyring:

<https://git.kernel.org/pub/scm/docs/kernel/pgpkeys.git/tree/keys/38DBBDC86092693E.asc>

Its full primary fingerprint is
`647F28654894E3BD457199BE38DBBDC86092693E`, also published by kernel.org at
<https://www.kernel.org/signature.html>. The exact key-file SHA-256 and expected
signer fingerprint are pinned in `kernel/patches/metadata.env`.

Preparation uses an empty temporary GnuPG home, disables automatic key
retrieval, validates the detached signature cryptographically, and accepts it
only when GnuPG's `VALIDSIG` status names that full fingerprint. GnuPG's local
trust database is intentionally irrelevant to this pinned-key verification.

Do not refresh this file automatically. A deliberate signing-key rotation must
update the key, its hash, the expected fingerprint, the release signature
identity, and the offline authentication tests together.
