#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check how the Mesa port generator handles more than one host platform.

The converter drops a generated source whose command differs between the host
platforms it merges. The generator converts each platform alone, keeps the
first platform's generator under its name, adds the others as `<name>_<arch>`,
and has each library that uses one select by arch. This test drives those
steps on synthetic Android.bp files, without the converter, and checks that
the tracked platform description covers the platforms the port names."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import sys
import tempfile
import tomllib
from pathlib import Path
from types import SimpleNamespace


def fail(message: str) -> None:
    raise SystemExit(f"Mesa port generator fixture: {message}")


def load(source_root: Path):
    tools = source_root / "android/lineage-24/tools"
    sys.path.insert(0, str(tools))
    path = tools / "generate-mesa-venus-port.py"
    spec = importlib.util.spec_from_file_location("generate_mesa_venus_port", path)
    if spec is None or spec.loader is None:
        fail("cannot load the generator")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def genrule(name: str, size: str, tools: str = "gen_tool") -> str:
    return f'''genrule {{
    name: "{name}",
    out: [
        "{name}.out",
    ],
    tools: [
        "{tools}",
    ],
    cmd: "$(location {tools}) in.xml {size} > $(location {name}.out)",
}}
'''


def consumer(kind: str, extra: str = "") -> str:
    return f'''{kind} {{
    name: "consumer",
    generated_headers: [
        "marshal.h",
        "other.h",
    ],
    srcs: [
        ":table.c",
        "a.c",
    ],
{extra}}}
'''


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def blocks(generator, root: Path, relative: str):
    bp = generator.Blueprint(root / relative)
    return {
        (relative, generator.block_name(block)): block
        for block in bp.blocks
        if generator.block_name(block) is not None
    }


def variants_for(generator, scratch: Path, sizes: dict[str, str]) -> dict:
    out = {}
    for platform, size in sizes.items():
        root = scratch / platform
        write(
            root, "gen/Android.bp",
            genrule("marshal.h", size) + "\n" + genrule("table.c", size)
            + "\n" + genrule("same.c", "common"),
        )
        out[platform] = blocks(generator, root, "gen/Android.bp")
    return out


PORT = SimpleNamespace(
    host_platforms=["p64", "p32"], platform_arch={"p64": "arm64", "p32": "arm"}
)


def check_split(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "variants", {"p64": "8", "p32": "4"})
    merged = scratch / "merged"
    # The converter keeps one platform's copy of a generator it merges wrongly
    # and drops another one, so the merged output has marshal.h with the last
    # platform's command and no table.c.
    write(merged, "gen/Android.bp", genrule("marshal.h", "4") + "\n" + genrule("same.c", "common"))
    write(merged, "lib/Android.bp", consumer("cc_library_static") + "\n" + '''cc_library_static {
    name: "bystander",
    srcs: [
        "b.c",
    ],
}
''')
    blueprints = generator.load(merged)
    count = generator.split_arch_genrules(merged, blueprints, variants, PORT)
    if count != 2:
        fail(f"two generators differ between the platforms, not {count}")
    for bp in blueprints:
        bp.save()
    gen = (merged / "gen/Android.bp").read_text()
    for wanted in (
        'name: "marshal.h",', 'name: "marshal.h_arm",', 'name: "table.c",',
        'name: "table.c_arm",', "in.xml 8 >", "in.xml 4 >",
    ):
        if wanted not in gen:
            fail(f"the generators lack {wanted}:\n{gen}")
    if gen.count('name: "same.c"') != 1 or "same.c_arm" in gen:
        fail("a generator that is the same on both platforms must stay as it is")
    names = [generator.block_name(b) for b in generator.Blueprint(merged / "gen/Android.bp").blocks]
    if sorted(names) != sorted(
        ["marshal.h", "marshal.h_arm", "table.c", "table.c_arm", "same.c"]
    ):
        fail(f"each generator is a module of its own: {names}")
    lib = (merged / "lib/Android.bp").read_text()
    for wanted in (
        '''    generated_headers: [
        "other.h",
    ],''',
        '''    srcs: [
        "a.c",
    ],''',
        '''    arch: {
        arm: {
            generated_headers: [
                "marshal.h_arm",
            ],
            srcs: [
                ":table.c_arm",
            ],
        },
        arm64: {
            generated_headers: [
                "marshal.h",
            ],
            srcs: [
                ":table.c",
            ],
        },
    },''',
    ):
        if wanted not in lib:
            fail(f"the consumer lacks:\n{wanted}\nin\n{lib}")
    if lib.count("arch: {") != 1:
        fail("only the library that uses a split generator gains an arch block")


def check_unchanged(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "same-variants", {"p64": "8", "p32": "8"})
    merged = scratch / "same-merged"
    write(merged, "gen/Android.bp", genrule("marshal.h", "8") + "\n" + genrule("table.c", "8")
          + "\n" + genrule("same.c", "common"))
    write(merged, "lib/Android.bp", consumer("cc_library_static"))
    before = {p: p.read_text() for p in merged.rglob("Android.bp")}
    blueprints = generator.load(merged)
    if generator.split_arch_genrules(merged, blueprints, variants, PORT) != 0:
        fail("platforms that agree split nothing")
    for bp in blueprints:
        if bp.save():
            fail("platforms that agree must leave every file as it was")
    if {p: p.read_text() for p in merged.rglob("Android.bp")} != before:
        fail("platforms that agree must leave every file as it was")


def check_refusals(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "refuse-variants", {"p64": "8", "p32": "4"})

    def split(consumer_text: str, name: str):
        merged = scratch / name
        write(merged, "gen/Android.bp", genrule("marshal.h", "4"))
        write(merged, "lib/Android.bp", consumer_text)
        blueprints = generator.load(merged)
        try:
            generator.split_arch_genrules(merged, blueprints, variants, PORT)
        except SystemExit as error:
            return str(error)
        fail(f"{name} must be refused")
        return ""

    # A reference inside a field that already carries a select cannot be moved.
    message = split('''cc_library_static {
    name: "consumer",
    srcs: [
        "a.c",
    ] + select(arch(), {
        "arm": [
            ":table.c",
        ],
        default: [],
    }),
}
''', "conditional")
    if "conditional srcs" not in message:
        fail(f"a reference in a conditional list is refused: {message}")
    message = split(consumer("cc_defaults"), "defaults")
    if "arch block cannot carry" not in message:
        fail(f"a module that cannot take an arch block is refused: {message}")


def check_host_platforms(generator) -> None:
    ok = {
        "config": [{"platforms": {"host_platforms": ["p64", "p32"]}}],
        "kernarium": {"platform_arch": {"p64": "arm64", "p32": "arm"}},
    }
    platforms, arches = generator.host_platforms(ok)
    if platforms != ["p64", "p32"] or arches != {"p64": "arm64", "p32": "arm"}:
        fail(f"host platforms: {platforms} {arches}")
    single = {"config": [{"platforms": {"host_platforms": ["p64"]}}]}
    if generator.host_platforms(single)[0] != ["p64"]:
        fail("one platform needs no arch map")
    for label, table in (
        ("a missing arch map", {"config": ok["config"]}),
        ("a shared arch", {**ok, "kernarium": {"platform_arch": {"p64": "arm", "p32": "arm"}}}),
        ("an arch for another platform", {**ok, "kernarium": {"platform_arch": {"p64": "arm64"}}}),
        ("no host platform", {"config": [{"platforms": {"host_platforms": []}}]}),
        ("two configs", {"config": ok["config"] * 2}),
    ):
        try:
            generator.host_platforms(table)
        except SystemExit:
            continue
        fail(f"{label} must be refused")


def check_tracked_platforms(source_root: Path) -> None:
    mesa = source_root / "android/lineage-24/third_party/mesa-venus"
    port = tomllib.loads((mesa / "soong-port.toml").read_text())
    platforms = tomllib.loads((mesa / "soong-platforms.toml").read_text())
    defined = {platform["name"]: platform for platform in platforms["platform"]}
    config = port["config"][0]["platforms"]
    for name in config["host_platforms"] + config["build_platforms"]:
        if name not in defined:
            fail(f"soong-platforms.toml does not define {name}")
    if set(port["kernarium"]["platform_arch"]) != set(config["host_platforms"]):
        fail("every host platform needs its Soong arch in [kernarium.platform_arch]")
    for name in config["host_platforms"]:
        for language in ("c", "cpp"):
            if "toolchain" in defined[name][language]:
                fail(f"{name} must not name a toolchain: the converter would download it")
    arm = defined["android_uml_arm"]
    for language in ("c", "cpp"):
        if arm[language]["compiles"]["fails"].get("__uint128_t") is not True:
            fail("a 32-bit ARM target has no __uint128_t")
        if arm[language]["sizeof"]["sizes"]["void*"] != 4:
            fail("a 32-bit ARM target has four-byte pointers")
        if "sizeof" in defined["android_uml_arm64"][language]:
            fail("the 64-bit platform takes the converter's eight-byte defaults")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    generator = load(args.source_root)
    check_host_platforms(generator)
    check_tracked_platforms(args.source_root)
    with tempfile.TemporaryDirectory(prefix="mesa-port-generator.") as name:
        scratch = Path(name)
        check_split(generator, scratch)
        check_unchanged(generator, scratch)
        check_refusals(generator, scratch)
    print("Mesa port generator fixture passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
