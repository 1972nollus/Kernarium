# SPDX-License-Identifier: Apache-2.0
"""Sign a target-files archive with a private key directory.

`publish-release.py --signing-keys DIR` uses this module. The key directory
is copied into a private (0700) directory, the AOSP releasetools sign the
archive with those keys, the signing log is checked so no APK or APEX stays
signed with a key outside the copy, the OTA certificates are replaced and
checked so no public AOSP or LineageOS certificate stays trusted, and the copy is
deleted again, with the deletion verified, whether or not signing succeeded.

The key directory holds the Android platform set (`releasekey`, `platform`,
`media`, `shared`, `networkstack`, ... as `<name>.pk8` and `<name>.x509.pem`)
and one APEX key per container name (`<name>.pk8`, `<name>.x509.pem` and the
payload key `<name>.pem`). Nothing here prints key material or stores a key
path in a release.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import hashlib
import io
import os
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path

from release_common import fail

SECURITY_DIR = "build/make/target/product/security/"
# Source directories of public certificates (AOSP's test keys and LineageOS's
# release certificate) that no signed release may trust.
PUBLIC_CERTIFICATE_DIRS = (SECURITY_DIR, "vendor/lineage/build/target/product/security/")
PLATFORM_KEYS = ("releasekey", "platform", "media", "shared")
# `signing: NAME [container] (KEY)` and the continuation `: NAME payload (KEY)`;
# the tool may print the next record on the same line, so search the text.
SIGNING_RECORD = re.compile(
    r"(?:(?<!NOT )\bsigning:|(?<=\s):)\s+(\S+)\s+(?:container\s+|payload\s+)?\(([^)]+)\)"
)
FIELD = re.compile(r'(\w+)="([^"]*)"')


def check_private_directory(keys: Path) -> None:
    """Refuse a key directory other users could read."""
    if not keys.is_absolute() or any(part in (".", "..") for part in keys.parts):
        fail("signing key directory must be an absolute normalized path")
    if keys.is_symlink() or not keys.is_dir():
        fail("signing key directory must be a regular directory")
    info = keys.lstat()
    if info.st_uid != os.getuid():
        fail("signing key directory is not owned by the invoking user")
    if info.st_mode & 0o077:
        fail("signing key directory must not be accessible to group or others")
    for entry in sorted(keys.iterdir()):
        entry_info = entry.lstat()
        if stat.S_ISLNK(entry_info.st_mode):
            target = os.readlink(entry)
            if os.path.isabs(target) or "/" in target:
                fail(f"signing key link must name a sibling file: {entry.name}")
            continue
        if not stat.S_ISREG(entry_info.st_mode):
            fail(f"signing key directory holds a non-regular entry: {entry.name}")
        if entry_info.st_mode & 0o077:
            fail(f"signing key file is accessible to group or others: {entry.name}")


def read_member(archive: zipfile.ZipFile, member: str) -> str:
    try:
        return archive.read(member).decode("utf-8")
    except (KeyError, UnicodeError) as error:
        fail(f"target files omit {member}: {error}")


def key_arguments(target_files: Path, keys: Path) -> list[str]:
    """The sign_target_files_apks flags that route every non-platform key."""
    with zipfile.ZipFile(target_files) as archive:
        apexkeys = read_member(archive, "META/apexkeys.txt")
        apkcerts = read_member(archive, "META/apkcerts.txt")
    flags: list[str] = []
    required = {f"{base}{suffix}" for base in PLATFORM_KEYS for suffix in (".pk8", ".x509.pem")}
    for line in apexkeys.splitlines():
        fields = dict(FIELD.findall(line))
        if "name" not in fields:
            continue
        name, container = fields["name"], fields.get("container_private_key", "")
        if container == "PRESIGNED":
            continue
        # A container the tree signs with an AOSP public key gets the key that
        # carries the APEX's own name; the others keep the tree's key name.
        if container.startswith(SECURITY_DIR):
            base = re.sub(r"\.c?apex$", "", name)
        else:
            base = container.rsplit("/", 1)[-1].removesuffix(".pk8")
        flags += ["--extra_apks", f"{name}={keys}/{base}"]
        flags += ["--extra_apex_payload_key", f"{name}={keys}/{base}.pem"]
        required.update(f"{base}{suffix}" for suffix in (".pk8", ".x509.pem", ".pem"))
    for line in apkcerts.splitlines():
        fields = dict(FIELD.findall(line))
        if "name" not in fields or fields.get("partition") == "data":
            continue
        certificate = fields.get("certificate", "")
        if certificate == "PRESIGNED" or certificate.startswith(SECURITY_DIR):
            continue
        # An APK inside an APEX, or a module resource package, that the tree
        # signs with its own key: the release key signs it, as the LineageOS
        # signing guide does.
        flags += ["--extra_apks", f"{fields['name']}={keys}/releasekey"]
    missing = sorted(name for name in required if not (keys / name).is_file())
    if missing:
        fail("signing key directory lacks: " + ", ".join(missing[:12]))
    return flags


def check_signing_log(log: str, keys: Path) -> tuple[int, list[str]]:
    """Every signed file must use a key from the private copy."""
    signed = 0
    for name, key in SIGNING_RECORD.findall(log):
        signed += 1
        if not key.startswith(f"{keys}/"):
            fail(f"{name} stayed signed with a key outside the release key set")
    unsigned = [
        line.strip().removeprefix("NOT signing:").split()[0]
        for line in log.splitlines()
        if line.strip().startswith("NOT signing:") and line.strip() != "NOT signing:"
    ]
    if signed == 0:
        fail("the signing log records no signed APK or APEX")
    return signed, unsigned


def pem_digest(pem: bytes) -> str:
    """SHA-256 of the DER form of one PEM certificate (public data)."""
    body = "".join(
        line
        for line in pem.decode("ascii").splitlines()
        if line and not line.startswith("-----")
    )
    return hashlib.sha256(base64.b64decode(body)).hexdigest()


def certificate_digests(keys: Path) -> dict[str, str]:
    """SHA-256 of the DER form of each platform certificate (public data)."""
    return {
        name: pem_digest((keys / f"{name}.x509.pem").read_bytes())
        for name in PLATFORM_KEYS
    }


def check_ota_certificates(signed_output: Path, keys: Path, aosp_source: Path) -> None:
    """Every otacerts.zip, in the system and the recovery, must trust the
    release certificate and no public AOSP or LineageOS certificate.

    Without the OTA key replacement the recovery and the system keep the AOSP
    public test certificate, which would let anyone sideload an update."""
    release = pem_digest((keys / "releasekey.x509.pem").read_bytes())
    public = {
        pem_digest(path.read_bytes())
        for directory in PUBLIC_CERTIFICATE_DIRS
        for path in sorted((aosp_source / directory).glob("*.x509.pem"))
    }
    if not public:
        fail("the source tree holds no public certificates to check against")
    with zipfile.ZipFile(signed_output) as archive:
        names = [name for name in archive.namelist() if name.endswith("/otacerts.zip")]
        for required in (
            "SYSTEM/etc/security/otacerts.zip",
            "RECOVERY/RAMDISK/system/etc/security/otacerts.zip",
        ):
            if required not in names:
                fail(f"the signed target files omit {required}")
        for name in names:
            with zipfile.ZipFile(io.BytesIO(archive.read(name))) as certificates:
                digests = {
                    pem_digest(certificates.read(member))
                    for member in certificates.namelist()
                    if member.endswith(".pem")
                }
            if release not in digests:
                fail(f"{name} omits the release certificate")
            if digests & public:
                fail(f"{name} still trusts a public AOSP or LineageOS certificate")


def run_logged(arguments: list[str], *, cwd: Path, env: dict[str, str]) -> str:
    result = subprocess.run(
        arguments, cwd=cwd, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False,
    )
    if result.returncode != 0:
        fail("sign_target_files_apks failed:\n" + "\n".join(result.stdout.splitlines()[-25:]))
    return result.stdout


def copy_private(keys: Path, parent: Path | None) -> Path:
    check_private_directory(keys)
    old_umask = os.umask(0o077)
    try:
        copy = Path(tempfile.mkdtemp(prefix="uml-signing-", dir=parent))
        os.chmod(copy, 0o700)
        for entry in sorted(keys.iterdir()):
            destination = copy / entry.name
            if entry.is_symlink():
                os.symlink(os.readlink(entry), destination)
            else:
                shutil.copyfile(entry, destination)
                os.chmod(destination, 0o600)
    finally:
        os.umask(old_umask)
    return copy


def delete_private(copy: Path) -> None:
    shutil.rmtree(copy, ignore_errors=True)
    if copy.exists() or copy.is_symlink():
        fail("the private signing key copy could not be deleted")


def sign_target_files(
    *,
    target_files: Path,
    signed_output: Path,
    keys: Path,
    host_bin: Path,
    aosp_source: Path,
    jdk_bin: Path,
    temporary_parent: Path | None = None,
) -> tuple[int, list[str], dict[str, str]]:
    """Write a signed copy of the target files.

    Returns the signed file count, the presigned files left as published and
    the SHA-256 of each platform certificate."""
    if signed_output.exists() or signed_output.is_symlink():
        fail("signed target-files output already exists")
    tool = host_bin / "sign_target_files_apks"
    if not tool.exists():
        fail("sign_target_files_apks is missing from the host tools (build the otatools)")
    copy = copy_private(keys, temporary_parent)
    try:
        arguments = [
            str(tool),
            "-o",
            "-d", str(copy),
            *key_arguments(target_files, copy),
            str(target_files),
            str(signed_output),
        ]
        env = os.environ.copy()
        env["PATH"] = f"{host_bin}:{jdk_bin}:{env.get('PATH', '/usr/bin:/bin')}"
        log = run_logged(arguments, cwd=aosp_source, env=env)
        signed, unsigned = check_signing_log(log, copy)
        check_ota_certificates(signed_output, copy, aosp_source)
        # Signing rebuilds the partition images but not the data seed.
        with zipfile.ZipFile(target_files) as original, zipfile.ZipFile(
            signed_output, "a", zipfile.ZIP_DEFLATED
        ) as signed_archive:
            if "IMAGES/userdata.img" not in signed_archive.namelist():
                signed_archive.writestr(
                    "IMAGES/userdata.img", original.read("IMAGES/userdata.img")
                )
        return signed, unsigned, certificate_digests(copy)
    finally:
        delete_private(copy)
