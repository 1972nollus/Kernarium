#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate fixed Kernarium CTS campaign bootstraps from the common driver."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
from pathlib import Path
import sys
from types import ModuleType


def load_runner(path: Path) -> ModuleType:
    specification = importlib.util.spec_from_file_location(
        "kernarium_vk_gl_cts_driver", path
    )
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load CTS driver: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def wrap_setup(driver: ModuleType, launcher: str, identifier: str) -> str:
    service = driver.CTS_SERVICE.rstrip()
    return f"""install -d -m 0755 /usr/local/libexec \\
    /etc/systemd/system/graphical.target.wants
cat > /usr/local/libexec/uml-vk-gl-cts <<'KERNARIUM_CTS_LAUNCHER'
{launcher}
KERNARIUM_CTS_LAUNCHER
chmod 0755 /usr/local/libexec/uml-vk-gl-cts
cat > /etc/systemd/system/uml-vk-gl-cts.service <<'KERNARIUM_CTS_SERVICE'
{service}
KERNARIUM_CTS_SERVICE
ln -sf ../uml-vk-gl-cts.service \\
    /etc/systemd/system/graphical.target.wants/uml-vk-gl-cts.service
echo 'Kernarium CTS campaign enabled: {identifier}'
"""


def campaign_setup(
    driver: ModuleType,
    campaign: object,
    page_size: int,
    rootfs_contract: object,
) -> str:
    launcher = driver.build_guest_launcher(
        campaign.selection,
        campaign.seed,
        campaign.shard_timeout,
        page_size,
        rootfs_contract,
    ).rstrip()
    return wrap_setup(driver, launcher, campaign.identifier)


def config_inventory_setup(
    driver: ModuleType,
    api: str,
    page_size: int,
    rootfs_contract: object,
) -> str:
    identifier = f"{api}-config-inventory-v1"
    launcher = driver.build_guest_launcher(
        None,
        0,
        900,
        page_size,
        rootfs_contract,
        config_inventory_api=api,
    ).rstrip()
    return wrap_setup(driver, launcher, identifier)


def matrix_setup(
    driver: ModuleType,
    shard: object,
    page_size: int,
    rootfs_contract: object,
) -> str:
    launcher = driver.build_guest_launcher(
        shard.selection,
        shard.seed,
        shard.shard_timeout,
        page_size,
        rootfs_contract,
        matrix=shard.matrix,
    ).rstrip()
    return wrap_setup(driver, launcher, shard.identifier)


def generate_header(
    driver: ModuleType,
    identity_path: Path,
    campaigns_path: Path,
    matrix_path: Path,
    derivative_receipt_path: Path,
    page_size: int,
) -> str:
    identity = driver.load_identity(identity_path)
    campaigns = driver.load_apple_campaigns(identity, campaigns_path)
    matrix_shards = driver.load_apple_matrix_shards(identity, matrix_path)
    _, erofs_contract = driver.select_rootfs_contract(
        identity, "erofs", None
    )
    _, ext4_contract = driver.select_rootfs_contract(
        identity, "ext4", derivative_receipt_path
    )
    rows: list[str] = []
    for index, campaign in enumerate(campaigns.values()):
        setups = (
            campaign_setup(driver, campaign, page_size, erofs_contract),
            campaign_setup(driver, campaign, page_size, ext4_contract),
        )
        delimiters = (f"CTS{index}E", f"CTS{index}X")
        for setup, delimiter in zip(setups, delimiters):
            if f"){delimiter}\"" in setup:
                raise RuntimeError(
                    "raw-string delimiter collision for "
                    f"{campaign.identifier}"
                )
        rows.append(
            "    Campaign {"
            f"{json.dumps(campaign.identifier)}, "
            f"R\"{delimiters[0]}({setups[0]}){delimiters[0]}\", "
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\""
            "},"
        )
    for shard in matrix_shards.values():
        index = len(rows)
        setups = (
            matrix_setup(driver, shard, page_size, erofs_contract),
            matrix_setup(driver, shard, page_size, ext4_contract),
        )
        delimiters = (f"CTS{index}E", f"CTS{index}X")
        for setup, delimiter in zip(setups, delimiters):
            if f"){delimiter}\"" in setup:
                raise RuntimeError(
                    f"raw-string delimiter collision for {shard.identifier}"
                )
        rows.append(
            "    Campaign {"
            f"{json.dumps(shard.identifier)}, "
            f"R\"{delimiters[0]}({setups[0]}){delimiters[0]}\", "
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\""
            "},"
        )
    for api in driver.BASELINE_GLES_APIS:
        index = len(rows)
        identifier = f"{api}-config-inventory-v1"
        setups = (
            config_inventory_setup(driver, api, page_size, erofs_contract),
            config_inventory_setup(driver, api, page_size, ext4_contract),
        )
        delimiters = (f"CTS{index}E", f"CTS{index}X")
        for setup, delimiter in zip(setups, delimiters):
            if f"){delimiter}\"" in setup:
                raise RuntimeError(
                    f"raw-string delimiter collision for {identifier}"
                )
        rows.append(
            "    Campaign {"
            f"{json.dumps(identifier)}, "
            f"R\"{delimiters[0]}({setups[0]}){delimiters[0]}\", "
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\""
            "},"
        )
    joined = "\n".join(rows)
    return f"""// SPDX-License-Identifier: GPL-2.0-or-later
// Generated by Kernarium/scripts/generate-cts-campaign-header.py.
#pragma once

#include <array>
#include <string_view>

namespace kernarium::cts {{

struct Campaign {{
    std::string_view identifier;
    std::string_view erofsSetup;
    std::string_view ext4Setup;
}};

inline constexpr std::array<Campaign, {len(rows)}> campaigns {{{{
{joined}
}}}};

}} // namespace kernarium::cts
"""


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--campaigns", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--derivative-receipt", type=Path, required=True)
    parser.add_argument("--page-size", type=int, choices=(4096, 16384), required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    options = parse_arguments()
    driver = load_runner(options.runner.resolve())
    content = generate_header(
        driver,
        options.identity.resolve(),
        options.campaigns.resolve(),
        options.matrix.resolve(),
        options.derivative_receipt.resolve(),
        options.page_size,
    )
    destination = options.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.read_text(encoding="utf-8") == content:
        return 0
    destination.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
