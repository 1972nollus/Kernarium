// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_LOADER_H
#define KERNARIUM_RUNTIME_LOADER_H

#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>
#include <stddef.h>
#include <stdint.h>

#include "../Runtime/KernariumRuntime.h"

NS_ASSUME_NONNULL_BEGIN

typedef void (^KernariumConsoleOutputHandler)(NSData* data);

#ifdef __cplusplus
extern "C" {
#endif

/* Selects and loads one bundled runtime for the lifetime of the process. The
 * flavour names exactly one embedded framework; every runtime has 4 KiB
 * guest pages. */
int kernarium_runtime_loader_select(
    uint32_t guest_flavor,
    uint32_t guest_page_size);

int kernarium_runtime_loader_run(
    MTKView* view,
    const kernarium_runtime_boot* boot);

void kernarium_runtime_loader_request_stop(void);
/* Flushes the guest's writable host files. Returns the number that could not
 * be flushed, or a negative value when no runtime is loaded. */
int kernarium_runtime_loader_sync(void);
/* Leaves the guest safely abandonable, and lets it run again. */
int kernarium_runtime_loader_quiesce(void);
void kernarium_runtime_loader_resume(void);
/* Holds (nonzero) or releases (zero) the guest's GPU requests; see
 * kernarium_runtime_set_gpu_hold_function. Returns the runtime's answer, or -1
 * when no runtime is loaded. */
int kernarium_runtime_loader_set_gpu_hold(int held);
void kernarium_runtime_loader_send_touch(
    uint32_t x,
    uint32_t y,
    int pressed);
void kernarium_runtime_loader_send_key(uint16_t code, int pressed);
void kernarium_runtime_loader_set_console_output_handler(
    KernariumConsoleOutputHandler _Nullable handler);
int kernarium_runtime_loader_send_console_input(
    const void* buffer,
    size_t length);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

#endif
