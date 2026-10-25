#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject overlay sepolicy that the platform's property neverallow would refuse.

The platform keeps its core domains from setting a vendor-owned property
(system/sepolicy/private/property.te). The policy build reports a violation
only after Soong has analysed the tree, so this check reads the overlay's own
sources first: a property type that a vendor_*_prop macro declares may be set
only by vendor_init or by a domain that the overlay itself declares without the
coredomain attribute. A core domain such as adbd reports through a type of the
system_ext policy (overlay/sepolicy/system_ext/private) instead. The exact
check runs on the built policy in verify-target-files.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from pathlib import Path

VENDOR_PROPERTY_MACRO = re.compile(r"\bvendor_(?:internal|restricted|public)_prop\(\s*(\w+)\s*\)")
DOMAIN_DECLARATION = re.compile(r"^\s*type\s+(\w+)\s*,([^;]*);", re.MULTILINE)
SET_PROP = re.compile(r"\bset_prop\(\s*(\w+)\s*,\s*(\w+)\s*\)")
ALLOW_SET = re.compile(
    r"\ballow\s+(\w+)\s+(\w+)\s*:\s*property_service\s+(?:set|\{[^}]*\bset\b[^}]*\})\s*;"
)


def fail(message: str) -> None:
    print(f"overlay sepolicy: {message}", file=sys.stderr)
    raise SystemExit(1)


def strip_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--overlay", type=Path, required=True)
    args = parser.parse_args()
    sources = {
        path: strip_comments(path.read_text(encoding="utf-8"))
        for path in sorted((args.overlay / "sepolicy").rglob("*.te"))
    }
    vendor_properties: set[str] = set()
    vendor_domains = {"vendor_init"}
    for text in sources.values():
        vendor_properties.update(VENDOR_PROPERTY_MACRO.findall(text))
        for name, attributes in DOMAIN_DECLARATION.findall(text):
            words = {word.strip() for word in attributes.split(",")}
            if "domain" in words and "coredomain" not in words:
                vendor_domains.add(name)
    for path, text in sources.items():
        for pattern in (SET_PROP, ALLOW_SET):
            for match in pattern.finditer(text):
                domain, target = match.groups()
                if target in vendor_properties and domain not in vendor_domains:
                    fail(
                        f"{path.relative_to(args.overlay)}: {domain} may be a core domain and "
                        f"may not set the vendor-owned property type {target}; declare the "
                        "property with a system_*_prop macro under sepolicy/system_ext/private"
                    )
    print(f"overlay sepolicy: {len(sources)} sources checked")


if __name__ == "__main__":
    main()
