#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check release signing: key routing, key-directory hygiene and key deletion."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import io
import os
import stat
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"release signing fixture: {message}")


APEXKEYS = "\n".join([
    'name="com.android.adbd.apex" public_key="packages/modules/adb/apex/com.android.adbd.avbpubkey" '
    'private_key="packages/modules/adb/apex/com.android.adbd.pem" '
    'container_certificate="packages/modules/adb/apex/com.android.adbd.x509.pem" '
    'container_private_key="packages/modules/adb/apex/com.android.adbd.pk8" partition="system"',
    'name="com.android.tethering.apex" public_key="x" private_key="packages/modules/Connectivity/apex/com.android.tethering.pem" '
    'container_certificate="build/make/target/product/security/testkey.x509.pem" '
    'container_private_key="build/make/target/product/security/testkey.pk8" partition="system"',
    'name="com.android.apex.cts.shim.apex" public_key="PRESIGNED" private_key="PRESIGNED" '
    'container_certificate="PRESIGNED" container_private_key="PRESIGNED" partition="system"',
    "",
])
APKCERTS = "\n".join([
    'name="Settings.apk" certificate="build/make/target/product/security/platform.x509.pem" '
    'private_key="build/make/target/product/security/platform.pk8" partition="system_ext"',
    'name="WifiDialog.apk" certificate="packages/modules/Wifi/WifiDialog/certs/com.android.wifi.dialog.x509.pem" '
    'private_key="packages/modules/Wifi/WifiDialog/certs/com.android.wifi.dialog.pk8" partition="system"',
    'name="F-Droid.apk" certificate="PRESIGNED" private_key="" partition="product"',
    'name="CtsHelper.apk" certificate="cts/certs/cts-testkey1.x509.pem" private_key="cts/certs/cts-testkey1.pk8" partition="data"',
    "",
])
PLATFORM_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "AAECAwQFBgcICQ==\n"
    "-----END CERTIFICATE-----\n"
)
# The AOSP public test certificate and a vendor's extra recovery certificate.
AOSP_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "CgsMDQ4PEBESEw==\n"
    "-----END CERTIFICATE-----\n"
)
EXTRA_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "FBUWFxgZGhscHQ==\n"
    "-----END CERTIFICATE-----\n"
)


def otacerts(*certificates: str) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for index, certificate in enumerate(certificates):
            archive.writestr(f"cert{index}.x509.pem", certificate)
    return buffer.getvalue()

FAKE_SIGNER = r'''#!/usr/bin/env python3
import shutil, sys
args = sys.argv[1:]
record = open(RECORD, "w")
record.write("\n".join(args) + "\n")
record.close()
keys = args[args.index("-d") + 1]
shutil.copyfile(args[-2], args[-1])
mode = MODE
if mode == "fail":
    print("boom")
    sys.exit(3)
print("    signing: Settings.apk                                                            (%s/platform)" % keys)
print("    signing: com.android.adbd.apex                                                   container (%s/com.android.adbd)" % keys)
print("           : com.android.adbd.apex                                                   payload   (%s/com.android.adbd.pem)" % keys)
if mode == "leak":
    print("    signing: com.android.tethering.apex                                              container (build/make/target/product/security/testkey)")
print("NOT signing: F-Droid.apk (skipped due to special cert string)")
'''


def build_target_files(path: Path, ota: str) -> None:
    """The fake signer copies this archive, so it holds the signed OTA certificates."""
    recovery = {
        "ok": (PLATFORM_CERT, EXTRA_CERT),
        "ota-aosp": (PLATFORM_CERT, AOSP_CERT),
        "ota-missing": (EXTRA_CERT,),
    }[ota]
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("META/apexkeys.txt", APEXKEYS)
        archive.writestr("META/apkcerts.txt", APKCERTS)
        archive.writestr("IMAGES/userdata.img", b"seed")
        archive.writestr("SYSTEM/etc/security/otacerts.zip", otacerts(PLATFORM_CERT))
        archive.writestr(
            "RECOVERY/RAMDISK/system/etc/security/otacerts.zip", otacerts(*recovery)
        )


def build_keys(path: Path, *, skip: str | None = None, loose: bool = False) -> None:
    path.mkdir(mode=0o700)
    names = ["releasekey", "platform", "media", "shared"]
    files = []
    for base in names:
        files += [f"{base}.pk8", f"{base}.x509.pem"]
    for base in ("com.android.adbd", "com.android.tethering"):
        files += [f"{base}.pk8", f"{base}.x509.pem", f"{base}.pem"]
    for name in files:
        if name == skip:
            continue
        target = path / name
        target.write_text(PLATFORM_CERT if name.endswith(".x509.pem") else "key-material\n")
        os.chmod(target, 0o640 if loose and name == "media.pk8" else 0o600)
    os.symlink("releasekey.pk8", path / "testkey.pk8")


def entries(path: Path) -> list[str]:
    return sorted(entry.name for entry in path.iterdir())


def run_case(
    sign_release, work: Path, name: str, mode: str, *, skip: str | None = None, loose: bool = False,
    key_mode: int = 0o700, ota: str = "ok",
):
    case = work / name
    case.mkdir()
    target_files = case / "target-files.zip"
    build_target_files(target_files, ota)
    security = case / "build/make/target/product/security"
    security.mkdir(parents=True)
    (security / "testkey.x509.pem").write_text(AOSP_CERT)
    keys = case / "keys"
    build_keys(keys, skip=skip, loose=loose)
    os.chmod(keys, key_mode)
    host_bin = case / "bin"
    host_bin.mkdir()
    record = case / "record.txt"
    signer = host_bin / "sign_target_files_apks"
    signer.write_text(
        FAKE_SIGNER.replace("RECORD", repr(str(record))).replace("MODE", repr(mode))
    )
    os.chmod(signer, 0o755)
    scratch = case / "scratch"
    scratch.mkdir()
    outcome: dict[str, object] = {"record": record, "scratch": scratch, "keys": keys}
    try:
        outcome["result"] = sign_release.sign_target_files(
            target_files=target_files,
            signed_output=case / "signed.zip",
            keys=keys,
            host_bin=host_bin,
            aosp_source=case,
            jdk_bin=case / "jdk",
            temporary_parent=scratch,
        )
        outcome["signed_zip"] = case / "signed.zip"
    except SystemExit as error:
        outcome["error"] = str(error)
    return outcome


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.source_root / "android/lineage-24/tools"))
    import sign_release  # noqa: E402

    with tempfile.TemporaryDirectory() as raw:
        work = Path(raw)

        ok = run_case(sign_release, work, "ok", "ok")
        if "error" in ok:
            fail(f"a valid signing run failed: {ok['error']}")
        signed, unsigned, digests = ok["result"]
        if signed != 3 or unsigned != ["F-Droid.apk"]:
            fail(f"unexpected signing summary: {signed} {unsigned}")
        if sorted(digests) != ["media", "platform", "releasekey", "shared"]:
            fail("certificate digests are incomplete")
        if len(set(digests.values())) != 1 or len(next(iter(digests.values()))) != 64:
            fail("certificate digest is not a SHA-256")
        if entries(ok["scratch"]):
            fail("the private key copy survived a successful signing")
        arguments = Path(ok["record"]).read_text().splitlines()
        if "-o" not in arguments:
            fail("signing did not replace the OTA keys")
        copy = arguments[arguments.index("-d") + 1]
        if Path(copy).parent != ok["scratch"] or str(ok["keys"]) in " ".join(arguments):
            fail("signing did not use a private copy of the key directory")
        expected = {
            f"com.android.adbd.apex={copy}/com.android.adbd",
            f"com.android.adbd.apex={copy}/com.android.adbd.pem",
            f"com.android.tethering.apex={copy}/com.android.tethering",
            f"com.android.tethering.apex={copy}/com.android.tethering.pem",
            f"WifiDialog.apk={copy}/releasekey",
        }
        if not expected <= set(arguments):
            fail(f"key routing is wrong: {sorted(expected - set(arguments))}")
        joined = " ".join(arguments)
        for forbidden in ("cts.certs", "CtsHelper", "F-Droid", "cts.shim", "Settings.apk"):
            if forbidden in joined:
                fail(f"{forbidden} must not be routed to a release key")
        with zipfile.ZipFile(ok["signed_zip"]) as archive:
            if archive.read("IMAGES/userdata.img") != b"seed":
                fail("the data seed image was not carried into the signed archive")

        for label, case_args, text in (
            ("leak", {"mode": "leak"}, "outside the release key set"),
            ("fail", {"mode": "fail"}, "sign_target_files_apks failed"),
            (
                "ota-aosp", {"mode": "ok", "ota": "ota-aosp"},
                "RECOVERY/RAMDISK/system/etc/security/otacerts.zip still trusts an AOSP",
            ),
            (
                "ota-missing", {"mode": "ok", "ota": "ota-missing"},
                "RECOVERY/RAMDISK/system/etc/security/otacerts.zip omits the release",
            ),
            ("missing", {"mode": "ok", "skip": "com.android.adbd.pem"}, "lacks: com.android.adbd.pem"),
            ("loose", {"mode": "ok", "loose": True}, "accessible to group or others"),
            ("open-dir", {"mode": "ok", "key_mode": 0o755}, "must not be accessible"),
        ):
            outcome = run_case(sign_release, work, label, **case_args)
            if text not in str(outcome.get("error", "")):
                fail(f"{label}: expected a refusal containing {text!r}, got {outcome.get('error')!r}")
            if entries(outcome["scratch"]):
                fail(f"{label}: a private key copy survived the refusal")
            if label in ("loose", "open-dir", "missing") and Path(outcome["record"]).exists():
                fail(f"{label}: the signer ran despite the refusal")
        mode = stat.S_IMODE(os.stat(work / "ok" / "keys").st_mode)
        if mode != 0o700:
            fail("the source key directory was modified")

    print("Android 17 release signing fixture passed")


if __name__ == "__main__":
    main()
