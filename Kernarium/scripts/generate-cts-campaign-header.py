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


def load_runner(
    path: Path, module_name: str = "kernarium_vk_gl_cts_driver"
) -> ModuleType:
    specification = importlib.util.spec_from_file_location(
        module_name, path
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
    /etc/systemd/system/multi-user.target.wants
cat > /usr/local/libexec/uml-vk-gl-cts <<'KERNARIUM_CTS_LAUNCHER'
{launcher}
KERNARIUM_CTS_LAUNCHER
chmod 0755 /usr/local/libexec/uml-vk-gl-cts
cat > /etc/systemd/system/uml-vk-gl-cts.service <<'KERNARIUM_CTS_SERVICE'
{service}
KERNARIUM_CTS_SERVICE
ln -sf ../uml-vk-gl-cts.service \\
    /etc/systemd/system/multi-user.target.wants/uml-vk-gl-cts.service
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


# The official desktop GL matrix is bound to the sealed ext4 derivative, whose
# receipt names the must-pass XML and its leaves. The EROFS base image carries
# no such evidence, so that cell refuses to run there rather than reporting a
# result the receipt gates never covered.
UNSUPPORTED_SETUP = """echo 'UML_CTS_ERROR: the official GL matrix requires the sealed ext4 derivative'
systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
"""


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
        renderer_backend=shard.renderer_backend,
    ).rstrip()
    return wrap_setup(driver, launcher, shard.identifier)


def vulkan_development_setup(
    driver: ModuleType,
    identity: dict[str, object],
    page_size: int,
    experimental_venus: bool,
) -> tuple[str, str]:
    identifier = "vulkan-info-development-v1"
    selection = driver.resolve_selection(
        identity,
        "smoke",
        (),
        None,
        driver.Fraction(0, 1),
    )
    launcher = driver.build_guest_launcher(
        identity,
        selection,
        page_size=page_size,
        timeout=900,
        experimental_venus=experimental_venus,
    ).rstrip()
    service = """[Unit]
Description=Darwin UML Vulkan CTS development identity campaign
After=systemd-user-sessions.service
Wants=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-vulkan-cts-run
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
""".rstrip()
    setup = f"""install -d -m 0755 /usr/local/libexec \\
    /etc/systemd/system/multi-user.target.wants
cat > /usr/local/libexec/uml-vulkan-cts <<'KERNARIUM_VULKAN_CTS_LAUNCHER'
{launcher}
KERNARIUM_VULKAN_CTS_LAUNCHER
chmod 0755 /usr/local/libexec/uml-vulkan-cts
cat > /usr/local/libexec/uml-vulkan-cts-run <<'KERNARIUM_VULKAN_CTS_WRAPPER'
#!/bin/sh
set -eu
set +e
/usr/local/libexec/uml-vulkan-cts
status=$?
set -e
if [ "$status" -ne 137 ]; then
    echo "UML_VK_CTS_ERROR: expected no-device status 137, got $status"
    exit 1
fi
sync
systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
KERNARIUM_VULKAN_CTS_WRAPPER
chmod 0755 /usr/local/libexec/uml-vulkan-cts-run
cat > /etc/systemd/system/uml-vulkan-cts.service <<'KERNARIUM_VULKAN_CTS_SERVICE'
{service}
KERNARIUM_VULKAN_CTS_SERVICE
ln -sf ../uml-vulkan-cts.service \\
    /etc/systemd/system/multi-user.target.wants/uml-vulkan-cts.service
echo 'Kernarium Vulkan CTS development campaign enabled: {identifier}'
"""
    unsupported = """echo 'UML_VK_CTS_ERROR: the development Vulkan identity campaign requires the frozen EROFS image'
systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
"""
    return setup, unsupported


def vulkan_receipt_setup(
    driver: ModuleType,
    receipt: dict[str, object],
    receipt_path: Path,
    page_size: int,
) -> tuple[str, str]:
    identifier = "vulkan-info-receipt-smoke-v1"
    selection = driver.resolve_receipt_selection(
        "smoke",
        (),
        None,
        driver.Fraction(0, 1),
    )
    launcher = driver.build_receipt_guest_launcher(
        receipt,
        selection,
        page_size=page_size,
        timeout=900,
        receipt_path=receipt_path,
    ).rstrip()
    service = """[Unit]
Description=Darwin UML receipt-backed Vulkan CTS smoke
After=systemd-user-sessions.service
Wants=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-vulkan-cts-run
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
""".rstrip()
    setup = f"""install -d -m 0755 /usr/local/libexec \\
    /etc/systemd/system/multi-user.target.wants
cat > /usr/local/libexec/uml-vulkan-cts <<'KERNARIUM_VULKAN_CTS_LAUNCHER'
{launcher}
KERNARIUM_VULKAN_CTS_LAUNCHER
chmod 0755 /usr/local/libexec/uml-vulkan-cts
cat > /usr/local/libexec/uml-vulkan-cts-run <<'KERNARIUM_VULKAN_CTS_WRAPPER'
#!/bin/sh
set -eu
set +e
/usr/local/libexec/uml-vulkan-cts
status=$?
set -e
printf 'Kernarium receipt-backed Vulkan CTS exited with status %s\n' "$status"
sync
systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
KERNARIUM_VULKAN_CTS_WRAPPER
chmod 0755 /usr/local/libexec/uml-vulkan-cts-run
cat > /etc/systemd/system/uml-vulkan-cts.service <<'KERNARIUM_VULKAN_CTS_SERVICE'
{service}
KERNARIUM_VULKAN_CTS_SERVICE
ln -sf ../uml-vulkan-cts.service \\
    /etc/systemd/system/multi-user.target.wants/uml-vulkan-cts.service
echo 'Kernarium receipt-backed Vulkan CTS campaign enabled: {identifier}'
"""
    unsupported = """echo 'UML_VK_CTS_ERROR: the receipt-backed Vulkan campaign requires the sealed ext4 derivative'
systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
"""
    return unsupported, setup


def generate_header(
    driver: ModuleType,
    vulkan_driver: ModuleType,
    identity_path: Path,
    vulkan_identity_path: Path,
    campaigns_path: Path,
    matrix_path: Path,
    derivative_receipt_path: Path,
    vulkan_receipt_path: Path | None,
    page_size: int,
    experimental_venus: bool,
) -> str:
    identity = driver.load_identity(identity_path)
    vulkan_identity = vulkan_driver.load_identity(vulkan_identity_path)
    vulkan_receipt: dict[str, object] | None = None
    if experimental_venus:
        if vulkan_receipt_path is None:
            raise RuntimeError(
                "the experimental Venus product requires a Vulkan receipt"
            )
        vulkan_receipt = vulkan_driver.receipt_contract.validate_receipt(
            vulkan_receipt_path
        )
    campaigns = driver.load_apple_campaigns(identity, campaigns_path)
    _, erofs_contract = driver.select_rootfs_contract(
        identity, "erofs", None
    )
    derivative_receipt, ext4_contract = driver.select_rootfs_contract(
        identity, "ext4", derivative_receipt_path
    )
    # The desktop GL matrix cell lives in the sealed derivative receipt, so the
    # shard set is expanded against it rather than against the base identity.
    matrix_shards = driver.load_apple_matrix_shards(
        identity, matrix_path, derivative_receipt
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
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\", false"
            "},"
        )
    for shard in matrix_shards.values():
        index = len(rows)
        setups = (
            UNSUPPORTED_SETUP
            if shard.api == "gl46"
            else matrix_setup(driver, shard, page_size, erofs_contract),
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
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\", false"
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
            f"R\"{delimiters[1]}({setups[1]}){delimiters[1]}\", false"
            "},"
        )
    index = len(rows)
    vulkan_setups = vulkan_development_setup(
        vulkan_driver, vulkan_identity, page_size, experimental_venus
    )
    delimiters = (f"CTS{index}E", f"CTS{index}X")
    for setup, delimiter in zip(vulkan_setups, delimiters):
        if f"){delimiter}\"" in setup:
            raise RuntimeError(
                "raw-string delimiter collision for Vulkan development campaign"
            )
    rows.append(
        "    Campaign {"
        '"vulkan-info-development-v1", '
        f"R\"{delimiters[0]}({vulkan_setups[0]}){delimiters[0]}\", "
        f"R\"{delimiters[1]}({vulkan_setups[1]}){delimiters[1]}\", true"
        "},"
    )
    if experimental_venus:
        assert vulkan_receipt is not None
        assert vulkan_receipt_path is not None
        index = len(rows)
        receipt_setups = vulkan_receipt_setup(
            vulkan_driver,
            vulkan_receipt,
            vulkan_receipt_path,
            page_size,
        )
        delimiters = (f"CTS{index}E", f"CTS{index}X")
        for setup, delimiter in zip(receipt_setups, delimiters):
            if f"){delimiter}\"" in setup:
                raise RuntimeError(
                    "raw-string delimiter collision for receipt-backed "
                    "Vulkan campaign"
                )
        rows.append(
            "    Campaign {"
            '"vulkan-info-receipt-smoke-v1", '
            f"R\"{delimiters[0]}({receipt_setups[0]}){delimiters[0]}\", "
            f"R\"{delimiters[1]}({receipt_setups[1]}){delimiters[1]}\", true"
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
    bool namedSerialArtifacts;
}};

inline constexpr std::array<Campaign, {len(rows)}> campaigns {{{{
{joined}
}}}};

}} // namespace kernarium::cts
"""


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--vulkan-runner", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--vulkan-identity", type=Path, required=True)
    parser.add_argument("--campaigns", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--derivative-receipt", type=Path, required=True)
    parser.add_argument("--vulkan-receipt", type=Path)
    parser.add_argument("--page-size", type=int, choices=(4096, 16384), required=True)
    parser.add_argument("--experimental-venus", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    options = parse_arguments()
    driver = load_runner(options.runner.resolve())
    vulkan_driver = load_runner(
        options.vulkan_runner.resolve(), "kernarium_vulkan_cts_driver"
    )
    content = generate_header(
        driver,
        vulkan_driver,
        options.identity.resolve(),
        options.vulkan_identity.resolve(),
        options.campaigns.resolve(),
        options.matrix.resolve(),
        options.derivative_receipt.resolve(),
        (
            options.vulkan_receipt.resolve()
            if options.vulkan_receipt is not None
            else None
        ),
        options.page_size,
        options.experimental_venus,
    )
    destination = options.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.read_text(encoding="utf-8") == content:
        return 0
    destination.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
