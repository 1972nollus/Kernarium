// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNARIUM_RUNTIME_API_VERSION 12U

#if defined(__OBJC__)
@class MTKView;
typedef MTKView* KernariumMetalView;
#else
typedef void* KernariumMetalView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What the framework's embedded kernel boots. One framework carries exactly
 * one flavour, because the kernel ELF is fixed in its signed carrier. */
enum {
    KERNARIUM_RUNTIME_GUEST_LINUX = 0,
    KERNARIUM_RUNTIME_GUEST_ANDROID = 1,
};

/* The send_touch coordinates are absolute positions in the guest's
 * touchscreen range, 0 through this value on both axes, wherever the display
 * is and however it is sized. The host maps its view onto that range, so the
 * range never has to follow a mode change. */
enum {
    KERNARIUM_RUNTIME_TOUCH_MAXIMUM = 32767,
};

enum {
    KERNARIUM_RUNTIME_ANDROID_BOOT_AUTO = 0,
    KERNARIUM_RUNTIME_ANDROID_BOOT_NORMAL = 1,
    KERNARIUM_RUNTIME_ANDROID_BOOT_RECOVERY = 2,
};

/* A boot request. Linux guests supply rootfs_path and overlay_path; Android
 * guests supply one directly writable GPT disk, its command line, and the
 * desired boot mode. The kernel is carried by the runtime. */
typedef struct kernarium_runtime_boot {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t dots_per_inch;
    uint32_t guest_memory_mib;
    uint32_t guest_processor_count;
    uint32_t android_boot_mode;
    uint32_t auto_resize_guest;
    const char* rootfs_path;
    const char* overlay_path;
    const char* android_disk_path;
    const char* android_command_line_path;
} kernarium_runtime_boot;

typedef int (*kernarium_runtime_run_function)(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot);

typedef void (*kernarium_runtime_request_stop_function)(void);
/* Flushes the guest's writable host files. Safe to call from any thread while
 * a guest runs, and reports the number of files that could not be flushed. */
typedef int (*kernarium_runtime_sync_function)(void);
/* Leaves the guest safely abandonable: trims its cold page cache once, writes
 * its filesystems back, enters full system suspend, then flushes the host's
 * own files. Pair it with resume. */
typedef int (*kernarium_runtime_quiesce_function)(void);
typedef void (*kernarium_runtime_resume_function)(void);
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
/* Holds (nonzero) or releases (zero) every GPU request the guest makes, so
 * none reaches the host GPU while it is held. A request that arrives waits in
 * its virtqueue, through any suspend of the guest, and runs in order after the
 * release. Work already in the renderer carries on. The application holds
 * before it enters the background, where the host GPU is revoked within
 * seconds, and releases when it returns. Returns 1 when the setting is in
 * effect and no request was still running after one second, 0 when one was
 * (the hold is in effect regardless), and -1 when no guest is running: the
 * setting is kept and applies to the guest's GPU once it starts. */
typedef int (*kernarium_runtime_set_gpu_hold_function)(int held);
/* Receives one copy of the guest's display as a CVPixelBufferRef: an
 * IOSurface-backed 32BGRA buffer from a pool, turned the way the user turned
 * the display, no wider than the width asked for. The buffer is borrowed for
 * the call; a receiver that keeps it retains it. It runs on a runtime thread
 * and must return promptly. */
typedef void (*kernarium_runtime_frame_sink)(void* context, void* pixel_buffer);
/* Mirrors the guest's display to `sink`, for a view of it that the
 * application shows while its own window cannot, such as Picture in Picture.
 * At most `frames_per_second` frames arrive each second, and a static display
 * arrives again about once a second. Nothing is rendered while no sink is set.
 * A null sink stops the mirror, and returns only once no call to the previous
 * sink is running or will start, so its context may be released; the sink must
 * not call this. Returns 0, or a negative value for a width or rate of zero.
 * The setting applies to the guest's display whenever one exists. */
typedef int (*kernarium_runtime_set_frame_sink_function)(
    kernarium_runtime_frame_sink sink,
    void* context,
    uint32_t maximum_width,
    uint32_t frames_per_second);
/* Moves the guest's vCPU threads to (nonzero) or from (zero) the system's
 * background scheduling policy, which runs them at low priority, prefers the
 * efficiency cores, and throttles their I/O. The application turns it on while
 * the guest is kept running behind a window the user is not looking at, so the
 * guest keeps its clock and its work moving without a foreground's power draw,
 * and off when the application returns. It applies to the vCPU threads that
 * exist when it is called, and leaves the host's other threads alone. Returns
 * the number of threads changed, or a negative value if the threads could not
 * be listed. */
typedef int (*kernarium_runtime_set_background_throttle_function)(int enabled);
/* Turns the guest's periodic page-cache trimming on (nonzero) or off, as
 * uml_emulator_set_page_cache_trimming does, and returns its result. A guest
 * boots with it off, as a guest in front of its user should be; the
 * application turns it on only while the guest is kept running in the
 * background. Quiescing trims once by itself. It may wait for the guest's
 * console, so call it away from the main thread. */
typedef int (*kernarium_runtime_set_page_cache_trimming_function)(int enabled);

/* The application resolves only this versioned getter after loading exactly
 * one runtime framework. The table is immutable and remains valid until
 * process exit. The selected framework owns one page-profile-specific host
 * runtime and its matching code-signed kernel carrier. */
typedef struct kernarium_runtime_api {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t guest_page_size;
    uint32_t guest_flavor;
    kernarium_runtime_run_function run;
    kernarium_runtime_request_stop_function request_stop;
    kernarium_runtime_sync_function sync;
    kernarium_runtime_quiesce_function quiesce;
    kernarium_runtime_resume_function resume;
    kernarium_runtime_send_touch_function send_touch;
    kernarium_runtime_send_key_function send_key;
    kernarium_runtime_set_console_output_callback_function
        set_console_output_callback;
    kernarium_runtime_send_console_input_function send_console_input;
    kernarium_runtime_set_gpu_hold_function set_gpu_hold;
    kernarium_runtime_set_frame_sink_function set_frame_sink;
    kernarium_runtime_set_background_throttle_function set_background_throttle;
    kernarium_runtime_set_page_cache_trimming_function
        set_page_cache_trimming;
} kernarium_runtime_api;

#if defined(__GNUC__)
#define KERNARIUM_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define KERNARIUM_RUNTIME_EXPORT
#endif

KERNARIUM_RUNTIME_EXPORT
const kernarium_runtime_api* kernarium_runtime_get_api_v12(void);

#ifdef __cplusplus
}
#endif

#endif
