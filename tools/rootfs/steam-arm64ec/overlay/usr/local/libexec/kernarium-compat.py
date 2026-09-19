#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply the image-owned Proton policy before the Steam client owns its config."""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import os
from pathlib import Path
import re
import shutil
import tempfile

TOOL = "proton-uml-arm64ec"
TOKENS = re.compile(r'\s+|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]')


def quoted(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


class Vdf:
    """Edit only selected value spans; retain unrelated account config verbatim."""
    def __init__(self, text: str):
        self.text = text
        self.tokens = []
        offset = 0
        for match in TOKENS.finditer(text):
            if match.start() != offset:
                raise ValueError("Unrecognized Steam config syntax; file preserved")
            offset = match.end()
            token = match[0]
            if not token.isspace() and not token.startswith("//"):
                self.tokens.append((token, match.start(), match.end()))
        if offset != len(text):
            raise ValueError("Incomplete Steam config syntax; file preserved")
        self.index = 0
        self.root, _ = self.parse(False)

    @staticmethod
    def value(token: str) -> str:
        if not token.startswith('"'):
            raise ValueError("Invalid Steam config value; file preserved")
        return re.sub(r'\\([\\"])', r'\1', token[1:-1])

    def parse(self, nested: bool):
        entries = {}
        while self.index < len(self.tokens):
            key, _, _ = self.tokens[self.index]
            self.index += 1
            if key == "}":
                if not nested:
                    raise ValueError("Unbalanced Steam config; file preserved")
                return entries, self.tokens[self.index - 1][1]
            key = self.value(key)
            if self.index == len(self.tokens):
                raise ValueError("Missing Steam config value; file preserved")
            token, start, end = self.tokens[self.index]
            self.index += 1
            if token == "{":
                children, close = self.parse(True)
                entry = (children, close, close)
            else:
                entry = (self.value(token), start, end)
            if key.lower() in entries:
                raise ValueError("Duplicate Steam config key; file preserved")
            entries[key.lower()] = entry
        if nested:
            raise ValueError("Unclosed Steam config; file preserved")
        return entries, len(self.text)

    def get(self, path: tuple[str, ...]):
        node = self.root
        for key in path:
            if not isinstance(node, dict) or key.lower() not in node:
                return None
            node = node[key.lower()][0]
        return node

    def set(self, path: tuple[str, ...], value: str) -> str:
        node, close = self.root, len(self.text)
        for index, key in enumerate(path):
            entry = node.get(key.lower())
            if entry is None:
                suffix = quoted(path[-1]) + " " + quoted(value) + "\n"
                for parent in reversed(path[index:-1]):
                    suffix = quoted(parent) + "\n{\n" + suffix + "}\n"
                return self.text[:close] + "\n" + suffix + self.text[close:]
            child, start, end = entry
            if index == len(path) - 1:
                if not isinstance(child, str):
                    raise ValueError("Expected Steam config scalar; file preserved")
                return self.text[:start] + quoted(value) + self.text[end:]
            if not isinstance(child, dict):
                raise ValueError("Expected Steam config section; file preserved")
            node, close = child, start
        raise ValueError("Empty Steam config path")


def update(text: str, path: tuple[str, ...], value: str) -> str:
    return Vdf(text).set(path, value)


def apply(steam: Path) -> None:
    pidfile = steam / "steam.pid"
    if pidfile.exists():
        try:
            pid = int(pidfile.read_text().strip())
            executable = Path(f"/proc/{pid}/exe").resolve(strict=True)
            if executable.is_relative_to(steam.resolve()):
                return  # A second invocation must not race the running client.
        except (OSError, ValueError):
            pass
    target = steam / "compatibilitytools.d" / TOOL
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() and not target.is_symlink():
        target.symlink_to("/opt/proton-uml-arm64ec")
    if target.resolve() != Path("/opt/proton-uml-arm64ec"):
        raise ValueError("Built-in Proton link differs from image policy; preserved")

    config = steam / "config/config.vdf"
    original = config.read_text() if config.exists() else ""
    text = original
    base = ("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping")
    mappings = Vdf(text).get(base) or {}
    appids = {"0", *mappings.keys()}
    libraries = {steam}
    library_file = steam / "steamapps/libraryfolders.vdf"
    if library_file.exists():
        for entry in (Vdf(library_file.read_text()).get(("libraryfolders",)) or {}).values():
            if isinstance(entry[0], dict) and "path" in entry[0]:
                libraries.add(Path(entry[0]["path"][0]))
    suspended = []
    for library in libraries:
        for manifest in (library / "steamapps").glob("appmanifest_*.acf"):
            app = Vdf(manifest.read_text())
            name = app.get(("AppState", "name")) or ""
            appid = app.get(("AppState", "appid")) or ""
            if name.lower().startswith(("proton", "fex")):
                suspended.append(manifest)
                appids.discard(appid)
            elif appid.isdigit() and not name.startswith("Steam Linux Runtime"):
                appids.add(appid)
    for appid in sorted(appids):
        if not appid.isdigit():
            raise ValueError("Invalid Steam compatibility app ID; config preserved")
        for key, value in (("name", TOOL), ("config", ""), ("priority", "250")):
            text = update(text, (*base, appid, key), value)
    config.parent.mkdir(parents=True, exist_ok=True)
    if text != original:
        backup = config.with_name("config.vdf.before-kernarium-proton")
        if config.exists() and not backup.exists():
            shutil.copy2(config, backup)
        descriptor, temporary = tempfile.mkstemp(prefix=".kernarium-config-", dir=config.parent)
        try:
            with os.fdopen(descriptor, "w") as output:
                output.write(text)
                output.flush()
                os.fsync(output.fileno())
            os.chmod(temporary, config.stat().st_mode & 0o777 if config.exists() else 0o600)
            os.replace(temporary, config)
        finally:
            Path(temporary).unlink(missing_ok=True)
    # Withdraw only incompatible tool download registrations. Preserve their
    # exact manifests and all downloaded files; never fabricate install status.
    for manifest in suspended:
        backup = manifest.parent / "kernarium-disabled-tools"
        backup.mkdir(exist_ok=True)
        digest = hashlib.sha256(manifest.read_bytes()).hexdigest()[:16]
        destination = backup / f"{manifest.stem}-{digest}.acf"
        if destination.exists():
            if destination.read_bytes() != manifest.read_bytes():
                raise ValueError("Tool manifest backup collision; original preserved")
            # Give every repeated registration its own recoverable destination.
            descriptor, name = tempfile.mkstemp(prefix=f"{manifest.stem}-", suffix=".acf", dir=backup)
            os.close(descriptor)
            destination = Path(name)
        manifest.replace(destination)
    print(f"Kernarium Proton policy: built-in tool selected; suspended {len(suspended)} incompatible tool downloads", flush=True)


if __name__ == "__main__":
    apply(Path.home() / ".local/share/Steam")
