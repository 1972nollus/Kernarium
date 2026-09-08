#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the standard Virtio-GPU shared-aperture kernel topic and ordering."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
from pathlib import Path


SUBJECT = "virtio: add UML vhost-user shared-memory aperture"
EXPECTED_PATHS = {
    "arch/um/drivers/vhost_user.h",
    "arch/um/drivers/virtio_uml.c",
    "arch/um/include/shared/mem.h",
    "arch/um/kernel/physmem.c",
    "arch/um/kernel/um_arch.c",
    "drivers/gpu/drm/virtio/virtgpu_vram.c",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def section(text: str, start: str, end: str) -> str:
    first = text.find(start)
    last = text.find(end, first + len(start))
    require(first >= 0 and last > first, f"missing ordered section: {start}")
    return text[first:last]


def validate(patch: str) -> None:
    paths: list[str] = []
    for line in patch.splitlines():
        if not line.startswith("diff --git a/"):
            continue
        left, right = line.removeprefix("diff --git a/").split(" b/", 1)
        require(left == right, "topic renames a shared-memory policy path")
        paths.append(left)
    require(
        len(paths) == len(EXPECTED_PATHS) and set(paths) == EXPECTED_PATHS,
        "shared-memory topic changes an unexpected path or omits an owner",
    )

    required_tokens = (
        "+#define VHOST_USER_PROTOCOL_F_SHMEM_MAP\t\t\t22",
        "+\tVHOST_USER_GET_SHMEM_CONFIG = 44,",
        "+\tVHOST_USER_SLAVE_SHMEM_MAP = 9,",
        "+\tVHOST_USER_SLAVE_SHMEM_UNMAP = 10,",
        "+#define VHOST_USER_MAX_SHMEM_REGIONS\t256",
        "+#define VHOST_USER_SHMEM_REGION_HOST_VISIBLE\t1",
        "+\t    msg.payload.shmem_config.num_regions != 1 ||",
        "+\t\theader_size = os_rcv_fd_msg(vu_dev->req_fd, fds, 2, msg,",
        "+static int vhost_user_recv_ack(struct virtio_uml_device *vu_dev, u64 *status)",
        "+\t\tconst int poll_fds[] = { vu_dev->req_fd, vu_dev->sock };",
        "+\t\tvu_req_read_message(vu_dev, NULL);",
        "+\t\trc = vhost_user_recv_ack(vu_dev, &status);",
        "+\traw_spinlock_t req_lock;",
        "+\traw_spin_lock_irqsave(&vu_dev->req_lock, flags);",
        "+\tresult = __vu_req_read_message(vu_dev, ev);",
        "+\traw_spin_unlock_irqrestore(&vu_dev->req_lock, flags);",
        "+\traw_spin_lock_init(&vu_dev->req_lock);",
        "+\t.get_shm_region = vu_get_shm_region,",
        "+#define UML_VHOST_USER_SHMEM_ALIGNMENT\t(16ULL * 1024ULL)",
        "+#define UML_VHOST_USER_SHMEM_SIZE\t(1024ULL * 1024ULL * 1024ULL)",
        "+#define UML_VHOST_USER_SHMEM_RESERVATION_SIZE \\",
        "+\tif (size > ULONG_MAX - UML_VHOST_USER_SHMEM_RESERVATION_SIZE -",
        "+\treturn PAGE_ALIGN(size + UML_VHOST_USER_SHMEM_RESERVATION_SIZE +",
        "+\t(UML_VHOST_USER_SHMEM_SIZE / UML_VHOST_USER_SHMEM_ALIGNMENT)",
        "+static unsigned int uml_vhost_user_shmem_pages[UML_VHOST_USER_SHMEM_MAX_MAPS];",
        "+\t\tuml_vhost_user_shmem_pages[i] = first + 1;",
        "+\tindex = uml_vhost_user_shmem_pages[offset / UML_VHOST_USER_SHMEM_ALIGNMENT];",
        "+\tint fds[64];",
        "+\tuml_vhost_user_shmem_revoking = true;",
        "+\tuml_vhost_user_shmem_revoking = false;",
        "+\tif (os_stat_fd(fd, &stat) || fd_offset + length > stat.ust_size)",
        "+\tret = drm_mm_insert_node_generic(&vgdev->host_visible_mm,",
        "+\tparams->size = ALIGN(params->size,",
        "+\t\t\t     VIRTIO_GPU_HOST_VISIBLE_ALIGNMENT);",
    )
    for token in required_tokens:
        require(token in patch, f"missing shared-memory contract: {token}")
    require(
        patch.count(
            "+\t    shm_offset + length > UML_VHOST_USER_SHMEM_SIZE)"
        ) == 2,
        "both map and unmap must reject ranges outside the aperture",
    )
    require(
        patch.count("+\tuml_vhost_user_shmem_release(vu_dev);") == 3,
        "disconnect, release, and failed initialization must revoke ownership",
    )

    handler = section(
        patch,
        "static void vhost_user_reply",
        "static int vhost_user_init_slave_req",
    )
    response = handler.find("+\t\tint response = 1;")
    receive = handler.find("vhost_user_recv_req")
    map_case = handler.find("VHOST_USER_SLAVE_SHMEM_MAP")
    map_call = handler.find("uml_vhost_user_shmem_map", map_case)
    transfer = handler.find("fds[0] = -1", map_call)
    unmap_case = handler.find("VHOST_USER_SLAVE_SHMEM_UNMAP")
    unmap_call = handler.find("uml_vhost_user_shmem_unmap", unmap_case)
    close_fds = handler.find("if (fds[0] >= 0)", unmap_call)
    event = handler.find("if (ev && !vu_dev->suspended)", close_fds)
    require(
        -1 not in (
            response,
            receive,
            map_case,
            map_call,
            transfer,
            unmap_case,
            unmap_call,
            close_fds,
            event,
        )
        and response < receive < map_case < map_call < transfer
        and map_case < unmap_case < unmap_call < close_fds < event,
        "backend descriptors, mapping ownership, and acknowledgements are misordered",
    )

    progress = section(
        patch,
        "static int vhost_user_recv_ack",
        "static int vhost_user_send",
    )
    backend_first = progress.find(
        "{ vu_dev->req_fd, vu_dev->sock }"
    )
    poll = progress.find("os_poll", backend_first)
    drain = progress.find("vu_req_read_message", poll)
    main_reply = progress.find("vhost_user_recv_u64", drain)
    require(
        -1 not in (backend_first, poll, drain, main_reply)
        and backend_first < poll < drain < main_reply,
        "reply-ack wait no longer prioritizes backend-request progress",
    )

    initialization = section(
        patch,
        "static int vhost_user_init(struct virtio_uml_device *vu_dev)",
        "static void vu_reset",
    )
    set_protocol = initialization.find("vhost_user_set_protocol_features")
    shmem_config = initialization.find("vhost_user_get_shmem_config")
    require(
        -1 not in (set_protocol, shmem_config) and set_protocol < shmem_config,
        "shared-memory config must follow protocol selection",
    )
    require(
        "VHOST_USER_PROTOCOL_F_SLAVE_REQ" in initialization
        and "VHOST_USER_PROTOCOL_F_REPLY_ACK" in initialization
        and "VHOST_USER_PROTOCOL_F_SHMEM_MAP" in initialization,
        "shared-memory negotiation lost a required protocol dependency",
    )

    require(
        "include/uapi/" not in "\n".join(paths)
        and "virtgpu_ioctl.c" not in paths,
        "shared aperture must not introduce a private userspace ABI",
    )


def replace_once(text: str, old: str, new: str) -> str:
    require(text.count(old) == 1, f"mutation anchor is not unique: {old!r}")
    return text.replace(old, new, 1)


def run_mutations(patch: str) -> None:
    mutations = (
        ("request-number", "VHOST_USER_GET_SHMEM_CONFIG = 44", "VHOST_USER_GET_SHMEM_CONFIG = 45"),
        ("protocol-bit", "VHOST_USER_PROTOCOL_F_SHMEM_MAP\t\t\t22", "VHOST_USER_PROTOCOL_F_SHMEM_MAP\t\t\t21"),
        ("descriptor-count", "fds, 2, msg", "fds, 1, msg"),
        (
            "backend-progress-order",
            "{ vu_dev->req_fd, vu_dev->sock }",
            "{ vu_dev->sock, vu_dev->req_fd }",
        ),
        (
            "backend-stream-lock",
            "raw_spin_lock_irqsave(&vu_dev->req_lock, flags)",
            "raw_spin_lock_irqsave(&vu_dev->sock_lock, flags)",
        ),
        ("region-count", "num_regions != 1", "num_regions != 2"),
        (
            "host-alignment",
            "UML_VHOST_USER_SHMEM_ALIGNMENT\t(16ULL * 1024ULL)",
            "UML_VHOST_USER_SHMEM_ALIGNMENT\t(4ULL * 1024ULL)",
        ),
        (
            "host-capacity",
            "UML_VHOST_USER_SHMEM_SIZE\t(1024ULL * 1024ULL * 1024ULL)",
            "UML_VHOST_USER_SHMEM_SIZE\t(256ULL * 1024ULL * 1024ULL)",
        ),
        (
            "auxiliary-preservation",
            "PAGE_ALIGN(size + UML_VHOST_USER_SHMEM_RESERVATION_SIZE +",
            "PAGE_ALIGN(UML_VHOST_USER_SHMEM_RESERVATION_SIZE +",
        ),
        ("fd-extent", "os_stat_fd(fd, &stat)", "os_stat_fd(-1, &stat)"),
        (
            "mapping-capacity",
            "UML_VHOST_USER_SHMEM_SIZE / UML_VHOST_USER_SHMEM_ALIGNMENT)",
            "UML_VHOST_USER_SHMEM_SIZE / UML_VHOST_USER_SHMEM_SIZE)",
        ),
        (
            "page-owner-sentinel",
            "uml_vhost_user_shmem_pages[i] = first + 1;",
            "uml_vhost_user_shmem_pages[i] = first;",
        ),
        ("bounded-revoke-stack", "int fds[64];", "int fds[UML_VHOST_USER_SHMEM_MAX_MAPS];"),
        ("transport-hook", ".get_shm_region = vu_get_shm_region,", ".get_shm_region = NULL,"),
        ("drm-alignment", "VIRTIO_GPU_HOST_VISIBLE_ALIGNMENT);", "PAGE_SIZE);"),
    )
    for name, old, new in mutations:
        candidate = replace_once(patch, old, new)
        try:
            validate(candidate)
        except AssertionError:
            continue
        raise AssertionError(f"{name}: policy validator accepted the mutation")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--series-manifest", type=Path, required=True)
    parser.add_argument("--series-directory", type=Path, required=True)
    arguments = parser.parse_args()

    with arguments.series_manifest.resolve().open(
        encoding="utf-8", newline=""
    ) as stream:
        rows = list(
            csv.DictReader(
                (line for line in stream if not line.startswith("#")),
                delimiter="\t",
            )
        )
    matches = [row for row in rows if row["subject"] == SUBJECT]
    require(len(matches) == 1, "shared-memory policy must have one exact topic")
    patch = (
        arguments.series_directory.resolve() / matches[0]["filename"]
    ).read_text(encoding="utf-8")
    validate(patch)
    run_mutations(patch)
    print("Virtio-GPU shared-memory aperture policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
