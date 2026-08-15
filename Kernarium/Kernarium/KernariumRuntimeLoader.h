// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_LOADER_H
#define KERNARIUM_RUNTIME_LOADER_H

#import <MetalKit/MetalKit.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Selects and loads one bundled runtime for the lifetime of the process. */
int kernarium_runtime_loader_select(uint32_t guest_page_size);

int kernarium_runtime_loader_run(
    MTKView* view,
    const char* rootfs_path,
    const char* overlay_path,
    uint32_t width,
    uint32_t height,
    uint32_t dots_per_inch,
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count);

void kernarium_runtime_loader_request_stop(void);
void kernarium_runtime_loader_send_touch(
    uint32_t x,
    uint32_t y,
    int pressed);
void kernarium_runtime_loader_send_key(uint16_t code, int pressed);

#ifdef __cplusplus
}
#endif

#endif
