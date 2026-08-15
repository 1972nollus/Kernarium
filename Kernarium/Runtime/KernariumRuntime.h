// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNARIUM_RUNTIME_API_VERSION 1U

#if defined(__OBJC__)
@class MTKView;
typedef MTKView* KernariumMetalView;
#else
typedef void* KernariumMetalView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*kernarium_runtime_run_function)(
    KernariumMetalView view,
    const char* rootfs_path,
    const char* overlay_path,
    uint32_t width,
    uint32_t height,
    uint32_t dots_per_inch,
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count);

typedef void (*kernarium_runtime_request_stop_function)(void);
typedef void (*kernarium_runtime_send_touch_function)(
    uint32_t x,
    uint32_t y,
    int pressed);
typedef void (*kernarium_runtime_send_key_function)(
    uint16_t code,
    int pressed);
typedef void (*kernarium_runtime_console_output_callback)(
    void* context,
    const void* buffer,
    size_t length);
typedef void (*kernarium_runtime_set_console_output_callback_function)(
    kernarium_runtime_console_output_callback callback,
    void* context);
typedef int (*kernarium_runtime_send_console_input_function)(
    const void* buffer,
    size_t length);

/* The application resolves only this versioned getter after loading exactly
 * one runtime framework. The table is immutable and remains valid until
 * process exit. The selected framework owns one page-profile-specific host
 * runtime and its matching code-signed kernel carrier. */
typedef struct kernarium_runtime_api {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t guest_page_size;
    uint32_t reserved;
    const char* kernel_release;
    const char* framework_name;
    kernarium_runtime_run_function run;
    kernarium_runtime_request_stop_function request_stop;
    kernarium_runtime_send_touch_function send_touch;
    kernarium_runtime_send_key_function send_key;
    kernarium_runtime_set_console_output_callback_function
        set_console_output_callback;
    kernarium_runtime_send_console_input_function send_console_input;
} kernarium_runtime_api;

#if defined(__GNUC__)
#define KERNARIUM_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define KERNARIUM_RUNTIME_EXPORT
#endif

KERNARIUM_RUNTIME_EXPORT
const kernarium_runtime_api* kernarium_runtime_get_api_v1(void);

#ifdef __cplusplus
}
#endif

#endif
