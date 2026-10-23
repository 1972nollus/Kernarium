#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the first-boot precompile tool: manifest rules, artifact naming per
instruction set, the ABI-set rules of a multilib image, the PRODUCT_COPY_FILES
fragment, reuse of earlier output, and the packaged-tree check."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import importlib.util
import io
import json
import shutil
import stat
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"first-boot precompile fixture: {message}")


def load(source_root: Path):
    path = source_root / "android/lineage-24/tools/precompile-first-boot-apps.py"
    spec = importlib.util.spec_from_file_location("precompile_first_boot_apps", path)
    if spec is None or spec.loader is None:
        fail("cannot load the precompile tool")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolves the defining module
    spec.loader.exec_module(module)
    return module


XMLTREE = """\
N: android=http://schemas.android.com/apk/res/android (line=2)
  E: manifest (line=2)
    A: package="org.example.app" (Raw: "org.example.app")
    A: platformBuildVersionCode=37
      E: uses-sdk (line=7)
        A: http://schemas.android.com/apk/res/android:minSdkVersion(0x0101020c)=24
        A: http://schemas.android.com/apk/res/android:targetSdkVersion(0x01010270)=36
      E: uses-feature (line=11)
        A: http://schemas.android.com/apk/res/android:name(0x01010003)="android.hardware.usb.host" (Raw: "android.hardware.usb.host")
        A: http://schemas.android.com/apk/res/android:required(0x0101028e)=false
      E: application (line=14)
        E: uses-library (line=16)
          A: http://schemas.android.com/apk/res/android:name(0x01010003)="libopt" (Raw: "libopt")
          A: http://schemas.android.com/apk/res/android:required(0x0101028e)=false
        E: uses-library (line=19)
          A: http://schemas.android.com/apk/res/android:name(0x01010003)="libreq" (Raw: "libreq")
        E: uses-library (line=21)
          A: http://schemas.android.com/apk/res/android:name(0x01010003)="libold" (Raw: "libold")
          A: http://schemas.android.com/apk/res/android:required(0x0101028e)=(type 0x12)0x0
"""

MULTI_ARCH_ATTRIBUTE = (
    "        A: http://schemas.android.com/apk/res/android:multiArch(0x010104ee)=(type 0x12)0xffffffff\n")
MULTI_ARCH_XMLTREE = XMLTREE.replace(
    "      E: application (line=14)\n", "      E: application (line=14)\n" + MULTI_ARCH_ATTRIBUTE)
NOT_MULTI_ARCH_XMLTREE = XMLTREE.replace(
    "      E: application (line=14)\n",
    "      E: application (line=14)\n"
    "        A: http://schemas.android.com/apk/res/android:multiArch(0x010104ee)=false\n")

FAKE_AAPT2 = r'''#!/usr/bin/env python3
import sys, zipfile
apk = sys.argv[-1]
with zipfile.ZipFile(apk) as archive:
    sys.stdout.write(archive.read("manifest.txt").decode())
'''

FAKE_DEX2OAT = r'''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
oat = next(a.split("=", 1)[1] for a in args if a.startswith("--oat-file="))
os.makedirs(os.path.dirname(oat), exist_ok=True)
location = next(a.split("=", 1)[1] for a in args if a.startswith("--dex-location="))
with open(oat, "w") as stream:
    stream.write("odex " + location)
with open(oat[:-4] + "vdex", "w") as stream:
    stream.write("vdex " + location)
with open(os.environ["FIXTURE_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
'''


def write_executable(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def make_apk(path: Path, manifest: str, dex: bytes | None, libs: tuple[str, ...] = ()) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("manifest.txt", manifest)
        if dex is not None:
            archive.writestr("classes.dex", dex)
        for abi in libs:
            archive.writestr(f"lib/{abi}/libnative.so", b"\x7fELF")


def check_pure_functions(tool) -> None:
    target, required, optional = tool.parse_xmltree(XMLTREE)
    if target != 36 or required != ["libreq"] or optional != ["libopt", "libold"]:
        fail(f"manifest parse: {target} {required} {optional}")
    # A pre-P target of a system app gains the legacy HTTP and test libraries
    # and, below Q, the HIDL ones, each prefixed ahead of the last.
    required, optional = tool.apply_backward_compatibility(27, True, ["libreq"], [])
    expected = [tool.TEST_BASE, *reversed(tool.HIDL_LIBRARIES), tool.APACHE_HTTP_LEGACY, "libreq"]
    if required != expected:
        fail(f"implicit libraries for a P-era system app: {required}")
    required, optional = tool.apply_backward_compatibility(
        36, True, ["libreq", tool.HIDL_LIBRARIES[0]], [tool.HIDL_LIBRARIES[1]])
    if required != ["libreq"] or optional != []:
        fail(f"HIDL libraries must not stay for a modern target: {required} {optional}")
    required, _ = tool.apply_backward_compatibility(29, False, [], [])
    if required != [tool.TEST_BASE]:
        fail(f"a Q app gains only android.test.base: {required}")
    for text, wanted in ((XMLTREE, False), (MULTI_ARCH_XMLTREE, True), (NOT_MULTI_ARCH_XMLTREE, False)):
        if tool.parse_multi_arch(text) is not wanted:
            fail(f"multiArch of the application element: expected {wanted}")
    # Only the application element counts, not an element that merely shares the name.
    elsewhere = XMLTREE.replace("      E: uses-feature (line=11)\n",
                                "      E: uses-feature (line=11)\n" + MULTI_ARCH_ATTRIBUTE)
    if tool.parse_multi_arch(elsewhere):
        fail("multiArch outside the application element must not count")
    if tool.class_loader_context([]) != "PCL[]":
        fail("empty class loader context")
    if tool.class_loader_context(["/a.jar", "/b.jar"]) != "PCL[]{PCL[/a.jar]#PCL[/b.jar]}":
        fail("class loader context shape")
    name = tool.apex_odex_name("com.android.wifi", "/app/WifiDialog/WifiDialog.apk")
    if name != "apex@com.android.wifi@app@WifiDialog@WifiDialog.apk@classes.odex":
        fail(f"APEX odex name: {name}")


def check_makefile(tool, scratch: Path) -> None:
    out = scratch / "mk"
    out.mkdir()
    tool.write_makefile(out, [
        {"odex": "system/framework/oat/arm64/apex@a@app@X@X.apk@classes.odex",
         "vdex": "system/framework/oat/arm64/apex@a@app@X@X.apk@classes.vdex"},
        {"odex": "product/app/F/oat/arm64/F.odex", "vdex": "product/app/F/oat/arm64/F.vdex"},
        {"odex": "product/app/F/oat/arm/F.odex", "vdex": "product/app/F/oat/arm/F.vdex"},
    ])
    lines = (out / "first-boot-oat.mk").read_text().splitlines()
    wanted = [
        "PRODUCT_COPY_FILES += $(LINEAGE24_FIRST_BOOT_OAT_DIR)system/framework/oat/arm64/"
        "apex@a@app@X@X.apk@classes.odex:$(TARGET_COPY_OUT_SYSTEM)/framework/oat/arm64/"
        "apex@a@app@X@X.apk@classes.odex",
        "PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += "
        "system/framework/oat/arm64/apex@a@app@X@X.apk@classes.vdex",
        "PRODUCT_COPY_FILES += $(LINEAGE24_FIRST_BOOT_OAT_DIR)product/app/F/oat/arm64/F.vdex"
        ":$(TARGET_COPY_OUT_PRODUCT)/app/F/oat/arm64/F.vdex",
        "PRODUCT_COPY_FILES += $(LINEAGE24_FIRST_BOOT_OAT_DIR)product/app/F/oat/arm/F.odex"
        ":$(TARGET_COPY_OUT_PRODUCT)/app/F/oat/arm/F.odex",
    ]
    for line in wanted:
        if line not in lines:
            fail(f"fragment lacks: {line}")
    allowed = [line for line in lines if "ALLOWED_LIST" in line]
    if len(allowed) != 2 or any("product/" in line for line in allowed):
        fail(f"only system files join the artifact path allowed list: {allowed}")


def build_fixture(root: Path) -> Path:
    aosp = root / "aosp"
    bin_dir = aosp / "out/host/linux-x86/bin"
    write_executable(bin_dir / "aapt2", FAKE_AAPT2)
    write_executable(bin_dir / "debugfs", "#!/bin/sh\nexit 1\n")
    write_executable(bin_dir / "dex2oatd64", FAKE_DEX2OAT)
    for isa, variant in (("arm64", "cortex-a53"), ("arm", "cortex-a53")):
        invocation = aosp / f"out/soong/.intermediates/x/dexpreopt/App/oat/{isa}/package.invocation"
        invocation.parent.mkdir(parents=True)
        boot = "/system/framework/boot.art" if isa == "arm64" else "/system/framework/arm/boot.art"
        tokens = [
            "out/host/linux-x86/bin/dex2oatd64", f"--boot-image={boot}",
            "-Xbootclasspath:/system/framework/core-oj.jar", "--dex-location=/system/app/App/App.apk",
            "--dex-file=out/App.apk", "--oat-file=out/App.odex", "--compiler-filter=speed",
            "--compilation-reason=prebuilt", "--class-loader-context=PCL[]", "--strip",
            "--generate-debug-info", f"--instruction-set={isa}",
            f"--instruction-set-variant={variant}", "--no-inline-from=core-oj.jar",
        ]
        invocation.write_text("\n".join(tokens) + "\n", encoding="utf-8")
    product = aosp / "out/target/product/darwin"
    make_apk(product / "product/app/Foo/Foo.apk", XMLTREE, b"dex-foo")
    make_apk(product / "product/app/Own/Own.apk", XMLTREE, b"dex-own")
    own_oat = product / "product/app/Own/oat/arm64"
    own_oat.mkdir(parents=True)
    (own_oat / "Own.odex").write_text("soong odex")
    (own_oat / "Own.vdex").write_text("soong vdex")
    make_apk(product / "product/app/Resources/Resources.apk", XMLTREE, None)
    # The ABI-set rules: a multiArch package with both ABIs runs in both ISAs; a
    # package with both ABIs that is not multiArch takes the first; one with
    # armeabi-v7a libraries only runs in arm; preinstalled libraries for both ISAs
    # beside the APK make both.
    both_abis = ("arm64-v8a", "armeabi-v7a")
    make_apk(product / "product/app/webview/webview.apk", MULTI_ARCH_XMLTREE, b"dex-web", both_abis)
    make_apk(product / "product/app/Plain/Plain.apk", NOT_MULTI_ARCH_XMLTREE, b"dex-plain", both_abis)
    make_apk(product / "product/app/Only32/Only32.apk", XMLTREE, b"dex-only32", ("armeabi-v7a",))
    make_apk(product / "product/app/Bundled/Bundled.apk", XMLTREE, b"dex-bundled")
    for isa in ("arm64", "arm"):
        (product / "product/app/Bundled/lib" / isa).mkdir(parents=True)
        (product / "product/app/Bundled/lib" / isa / "libnative.so").write_text("elf")
    bundled_oat = product / "product/app/Bundled/oat/arm64"
    bundled_oat.mkdir(parents=True)
    (bundled_oat / "Bundled.odex").write_text("soong odex")
    (bundled_oat / "Bundled.vdex").write_text("soong vdex")
    permissions = product / "system/etc/permissions"
    permissions.mkdir(parents=True)
    (permissions / "libs.xml").write_text(
        '<permissions>\n'
        '  <library name="libreq" file="/system/framework/libreq.jar"/>\n'
        '  <library name="libopt" file="/system/framework/libopt.jar"/>\n'
        '  <library name="libold" file="/system/framework/libold.jar"/>\n'
        '</permissions>\n')
    for jar in ("libreq", "libopt"):
        (product / "system/framework").mkdir(parents=True, exist_ok=True)
        (product / f"system/framework/{jar}.jar").write_text("jar")
    return aosp


def target_files_tree(aosp: Path, artifacts: Path, destination: Path) -> None:
    """Stage what the second pass packages: the build output plus the artifacts, upper-cased."""
    product = aosp / "out/target/product/darwin"
    for partition in ("system", "product"):
        shutil.copytree(product / partition, destination / partition.upper(), dirs_exist_ok=True)
        if (artifacts / partition).is_dir():
            shutil.copytree(artifacts / partition, destination / partition.upper(), dirs_exist_ok=True)


def run_tool(tool, *argv: str, expect_fail: bool = False) -> str:
    sys.argv = ["precompile-first-boot-apps", *argv]
    buffer = io.StringIO()
    try:
        with contextlib.redirect_stdout(buffer):
            tool.main()
    except SystemExit as exit_:
        if not expect_fail:
            fail(f"{argv[0]} failed: {exit_}")
        return str(exit_)
    if expect_fail:
        fail(f"{argv[0]} should have failed")
    return buffer.getvalue()


def invocations(log: Path) -> dict[tuple[str, str], list[str]]:
    """The dex2oat arguments by (dex location, ISA directory of the odex)."""
    found = {}
    for line in log.read_text().splitlines():
        args = json.loads(line)
        location = next(a.split("=", 1)[1] for a in args if a.startswith("--dex-location="))
        oat = next(a.split("=", 1)[1] for a in args if a.startswith("--oat-file="))
        found[(location, Path(oat).parent.name)] = args
    return found


def check_build_and_verify(tool, scratch: Path) -> None:
    import os

    aosp = build_fixture(scratch)
    log = scratch / "dex2oat.log"
    os.environ["FIXTURE_LOG"] = str(log)
    common = ["--aosp-source", str(aosp), "--out-dir", "out"]

    # An ISA with no recorded Soong invocation cannot be compiled.
    arm_invocation = aosp / "out/soong/.intermediates/x/dexpreopt/App/oat/arm/package.invocation"
    saved = arm_invocation.read_bytes()
    arm_invocation.unlink()
    message = run_tool(tool, "build", *common, "--product", "darwin",
                       "--output", str(scratch / "no-arm"), expect_fail=True)
    if "no recorded arm dex2oat invocation" not in message:
        fail(f"a missing arm template must be named: {message}")
    arm_invocation.write_bytes(saved)

    first = scratch / "first"
    run_tool(tool, "build", *common, "--product", "darwin", "--output", str(first), "--jobs", "2")
    manifest = json.loads((first / "manifest.json").read_text())
    if "isa" in manifest:
        fail("the ISA belongs to each entry, not to the manifest")
    compiled = [(p["location"], p["isa"]) for p in manifest["packages"]]
    expected = [
        ("/product/app/Bundled/Bundled.apk", "arm"),
        ("/product/app/Foo/Foo.apk", "arm64"),
        ("/product/app/Only32/Only32.apk", "arm"),
        ("/product/app/Plain/Plain.apk", "arm64"),
        ("/product/app/webview/webview.apk", "arm64"),
        ("/product/app/webview/webview.apk", "arm"),
    ]
    if compiled != expected:
        fail("only the ISAs without artifacts are compiled, never Soong's or a dexless package's, "
             f"and each package gets its whole ISA set: {compiled}")
    entries = {(p["location"], p["isa"]): p for p in manifest["packages"]}
    entry = entries[("/product/app/Foo/Foo.apk", "arm64")]
    if entry["odex"] != "product/app/Foo/oat/arm64/Foo.odex" or entry["filter"] != "verify":
        fail(f"manifest entry: {entry}")
    if entries[("/product/app/webview/webview.apk", "arm")]["odex"] != \
            "product/app/webview/oat/arm/webview.odex":
        fail("the arm artifacts belong in oat/arm")
    if entry["target_sdk"] != 36 or entry["class_loader_context"] != \
            "PCL[]{PCL[/system/framework/libreq.jar]#PCL[/system/framework/libopt.jar]}":
        fail(f"class loader context or target SDK: {entry}")
    runs = invocations(log)
    if set(runs) != {(location, isa) for location, isa in expected}:
        fail(f"one dex2oat run per artifact set: {sorted(runs)}")
    for (location, isa), invocation in runs.items():
        flags = {a.split("=", 1)[0] for a in invocation if a.startswith("--")}
        for dropped in ("--strip", "--generate-debug-info"):
            if dropped in flags:
                fail(f"{dropped} must not be passed")
        for kept in ("--boot-image", "--instruction-set", "--instruction-set-variant"):
            if kept not in flags:
                fail(f"{kept} is Soong's flag and must be kept")
        # Each ISA compiles with the flags Soong recorded for that ISA.
        if f"--instruction-set={isa}" not in invocation or \
                f"--boot-image={'/system/framework/arm/boot.art' if isa == 'arm' else '/system/framework/boot.art'}" \
                not in invocation:
            fail(f"{location} ({isa}) used another ISA's template: {invocation}")
        for token, wanted in (("--compiler-filter", "verify"), ("--compilation-reason", "prebuilt"),
                              ("--dex-location", location)):
            values = [a for a in invocation if a.startswith(token + "=")]
            if values != [f"{token}={wanted}"]:
                fail(f"{token} must be set once to {wanted}: {values}")
        if invocation[invocation.index("--runtime-arg") + 1] != "-Xtarget-sdk-version:36":
            fail("target SDK runtime argument")

    staging = scratch / "target-files"
    target_files_tree(aosp, first, staging)
    verify = ["verify", "--host-bin", str(aosp / "out/host/linux-x86/bin"),
              "--target-files-dir", str(staging), "--artifacts", str(first)]
    run_tool(tool, *verify)

    # Output that already exists is never overwritten.
    run_tool(tool, "build", *common, "--product", "darwin", "--output", str(first), expect_fail=True)

    # Installing the artifacts into the build output makes a rebuild with --previous
    # recognise them as its own and reproduce the same output.
    product = aosp / "out/target/product/darwin"
    shutil.copytree(first / "product", product / "product", dirs_exist_ok=True)
    second = scratch / "second"
    run_tool(tool, "build", *common, "--product", "darwin", "--output", str(second),
             "--previous", str(first), "--jobs", "2")
    if (first / "manifest.json").read_bytes() != (second / "manifest.json").read_bytes() or \
            (first / "first-boot-oat.mk").read_bytes() != (second / "first-boot-oat.mk").read_bytes():
        fail("a rebuild over its own installed output must be identical")
    # Without --previous, installed artifacts look like Soong's and nothing is left to do.
    third = scratch / "third"
    message = run_tool(tool, "build", *common, "--product", "darwin", "--output", str(third),
                       expect_fail=True)
    if "nothing to precompile" not in message:
        fail(f"unexpected failure without --previous: {message}")

    # The packaged-tree check names each way first boot would still compile.
    (staging / "PRODUCT/app/Foo/oat/arm64/Foo.odex").unlink()
    message = run_tool(tool, *verify, expect_fail=True)
    if "first boot would compile /product/app/Foo/Foo.apk for arm64" not in message:
        fail(f"missing odex not reported: {message}")
    target_files_tree(aosp, first, staging)
    (staging / "PRODUCT/app/webview/oat/arm/webview.vdex").unlink()
    message = run_tool(tool, *verify, expect_fail=True)
    if "first boot would compile /product/app/webview/webview.apk for arm:" not in message or \
            "webview.apk for arm64" in message:
        fail(f"a missing artifact of one ISA must name only that ISA: {message}")
    target_files_tree(aosp, first, staging)
    (staging / "PRODUCT/app/Bundled/oat/arm/Bundled.odex").unlink()
    message = run_tool(tool, *verify, expect_fail=True)
    if "first boot would compile /product/app/Bundled/Bundled.apk for arm:" not in message:
        fail(f"a package with Soong's arm64 artifacts still needs arm: {message}")
    target_files_tree(aosp, first, staging)
    (staging / "PRODUCT/app/Foo/oat/arm64/Foo.vdex").write_text("tampered")
    message = run_tool(tool, *verify, expect_fail=True)
    if "differ from the manifest" not in message:
        fail(f"tampered vdex not reported: {message}")
    target_files_tree(aosp, first, staging)
    make_apk(staging / "PRODUCT/app/Foo/Foo.apk", XMLTREE, b"another dex")
    message = run_tool(tool, *verify, expect_fail=True)
    if "no longer matches its artifacts" not in message:
        fail(f"changed APK not reported: {message}")
    target_files_tree(aosp, first, staging)
    make_apk(staging / "PRODUCT/app/Extra/Extra.apk", XMLTREE, b"extra dex")
    message = run_tool(tool, *verify, expect_fail=True)
    if "first boot would compile /product/app/Extra/Extra.apk for arm64" not in message:
        fail(f"a new application without artifacts not reported: {message}")
    # A package that stops carrying 32-bit libraries no longer runs in arm, so its
    # arm artifacts are stale.
    target_files_tree(aosp, first, staging)
    make_apk(staging / "PRODUCT/app/webview/webview.apk", MULTI_ARCH_XMLTREE, b"dex-web", ("arm64-v8a",))
    message = run_tool(tool, *verify, expect_fail=True)
    if "not in the image: /product/app/webview/webview.apk (arm)" not in message:
        fail(f"artifacts of an ISA the package no longer runs in not reported: {message}")
    # A manifest from before per-ISA artifacts cannot be verified against.
    target_files_tree(aosp, first, staging)
    old = scratch / "old-manifest"
    old.mkdir()
    legacy = json.loads((first / "manifest.json").read_text())
    for package in legacy["packages"]:
        del package["isa"]
    (old / "manifest.json").write_text(json.dumps(legacy))
    message = run_tool(tool, "verify", "--host-bin", str(aosp / "out/host/linux-x86/bin"),
                       "--target-files-dir", str(staging), "--artifacts", str(old), expect_fail=True)
    if "predates per-ISA artifacts" not in message:
        fail(f"a manifest without ISAs must be refused: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    tool = load(args.source_root)
    check_pure_functions(tool)
    with tempfile.TemporaryDirectory(prefix="first-boot-precompile.") as name:
        scratch = Path(name)
        check_makefile(tool, scratch)
        check_build_and_verify(tool, scratch / "tree")
    print("first-boot precompile fixture passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
