// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stdint.h>

#if defined(__OBJC__)
@class MTKView;
typedef MTKView* KernariumMetalView;
#else
typedef void* KernariumMetalView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Runs one embedded UML guest synchronously on the calling worker thread.
 * The MTKView remains owned by SwiftUI and must outlive this call. */
int kernarium_runtime_run(
    KernariumMetalView view,
    const char* rootfs_path,
    uint32_t width,
    uint32_t height);

void kernarium_runtime_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif
