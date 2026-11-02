// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNARIUM_RUNTIME_API_VERSION 17U

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

/* A boot request. Linux guests supply rootfs_path and overlay_path. Android
 * guests supply the release directory (guest/system.img, the read-only system
 * disk; guest/partition-layout.json; boot/kernel-command-line.txt), the path of
 * the instance's writable data disk, and the desired boot mode. The runtime
 * creates a data disk that is absent, sized android_data_gib GiB or the
 * release's default when that is 0, and grows an existing one to
 * android_data_gib GiB when that is larger; it never shrinks one. The guest
 * has exactly these two disks. The kernel is carried by the runtime.
 *
 * The host GPU's budget is B = clamp(L - A - G - E - S, 384 MiB, G / 2),
 * evaluated once at launch (see the architecture document): L is the
 * application's memory limit, G the guest's memory, and E the gadget cache
 * plus the other memory the guest side of the process uses. The request states
 * the three launch parameters: A, the application's own expected ceiling;
 * E_rest, the guest side's memory beyond the gadget cache; and S, the headroom
 * left unused. Zero, which is what the application states until their values
 * are chosen, adds nothing to the sum. */
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
    const char* android_data_disk_path;
    const char* android_release_path;
    uint32_t android_data_gib;
    uint32_t gpu_budget_application_mib;
    uint32_t gpu_budget_engine_mib;
    uint32_t gpu_budget_headroom_mib;
} kernarium_runtime_boot;

/* What a launch's host GPU budget comes to, as the runtime evaluates it for
 * the boot request it receives. */
typedef struct kernarium_runtime_gpu_budget {
    uint32_t struct_size;
    /* Nonzero when L - A - G - E - S is below the budget's 384 MiB floor: the
     * process is likely to be ended, the guest boots anyway with the budget
     * clamped to the floor, and the application is to say so. */
    uint32_t short_of_floor;
    /* B. */
    uint64_t budget_bytes;
    /* L, zero when the host sets no limit. */
    uint64_t process_limit_bytes;
    /* L - A - G - E - S, zero when the sum passes L. */
    uint64_t available_bytes;
    uint64_t guest_memory_bytes;
    /* E: the gadget cache the run will set plus E_rest. */
    uint64_t engine_bytes;
    uint64_t application_bytes;
    uint64_t headroom_bytes;
} kernarium_runtime_gpu_budget;

/* Boots the guest and returns when it is over. A guest that restarts itself is
 * booted again inside this call, on the same view, disk and network, so the
 * call returns only when the guest powers off, the application stops it, a run
 * fails, or the guest restarts itself again and again within seconds of
 * booting. It returns the guest's own status (0 for a power-off or a stop), a
 * negated uml_emulator error constant for a run or start-up that failed, or
 * KERNARIUM_RUNTIME_RESULT_RESTART_LOOP. */
typedef int (*kernarium_runtime_run_function)(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot);

enum {
    /* The guest restarted itself five times in a row, each within 15 seconds
     * of its boot, so the runtime stopped booting it. It is outside the range
     * of the negated uml_emulator errors (-1 through -4). */
    KERNARIUM_RUNTIME_RESULT_RESTART_LOOP = -100,
};

/* Evaluates the host GPU's budget for a guest of `guest_memory_mib` with the
 * launch parameters of kernarium_runtime_boot, as a run with that request will
 * (the application's memory limit and the gadget cache included), and fills
 * `plan`, whose struct_size the caller sets. The application calls it before
 * run to warn the user when `short_of_floor` is set. Returns 0, or a negative
 * value for a null or short `plan` or a guest memory of zero. */
typedef int (*kernarium_runtime_plan_gpu_budget_function)(
    uint32_t guest_memory_mib,
    uint32_t gpu_budget_application_mib,
    uint32_t gpu_budget_engine_mib,
    uint32_t gpu_budget_headroom_mib,
    kernarium_runtime_gpu_budget* plan);

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
/* Whether the guest's display is on: 1 while it has its primary scanout, 0
 * while it has none, as an Android screen that is off has not, and -1 when no
 * guest is running or its GPU has not started. A request the guest sent behind
 * a held GPU has not run, so the answer then is the one the guest's last
 * request that ran left; the application asks before it holds. */
typedef int (*kernarium_runtime_guest_display_active_function)(void);
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
/* What set_microphone leaves, as its return value. */
enum {
    /* The Microphone setting is off. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_OFF = 0,
    /* On, but the system has not been asked for access: the guest records
     * silence until the application has asked, and the application, not the
     * runtime, is the one that asks. */
    KERNARIUM_RUNTIME_MICROPHONE_UNASKED = 1,
    /* On, but the system refuses access. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_DENIED = 2,
    /* On and permitted: a guest that records hears the microphone. */
    KERNARIUM_RUNTIME_MICROPHONE_ON = 3,
};
/* Turns the guest's microphone on (nonzero) or off (zero) and returns the
 * resulting KERNARIUM_RUNTIME_MICROPHONE_ value. It is off until the
 * application turns it on, and while it is off, or the system has not granted
 * access, a guest that records gets silence and nothing opens the microphone.
 * The runtime never asks the system for access and never shows a permission
 * prompt: the application asks, as the direct result of the user turning the
 * setting on, and then calls this. Calling it again with a nonzero value after
 * the answer, or when the application becomes active, lets a guest that is
 * recording pick the microphone up. It may be called whether or not a guest
 * runs; a guest that starts later follows it. */
typedef int (*kernarium_runtime_set_microphone_function)(int enabled);

/* What a restart event reports. */
enum {
    /* The guest asked to restart and the old guest is gone: its devices are
     * stopped and the runtime is preparing the next boot, which waits while
     * the application holds the guest's GPU requests or has quiesced the guest
     * (the new guest's GPU probe would otherwise time out and its boot would
     * run without 3D). */
    KERNARIUM_RUNTIME_RESTART_BEGAN = 0,
    /* The restarted guest's management console answers, so what belongs to a
     * kernel, the vCPU throttle and the page-cache trimming, can be applied to
     * the new one: it boots with neither. If the console has not answered
     * within two minutes the event is sent anyway. It is not sent when the
     * guest ends first. */
    KERNARIUM_RUNTIME_RESTART_BOOTED = 1,
};
/* `restart_count` is the number of times this launch's guest has restarted,
 * 1 for the first restart. An event follows BEGAN with the same count, or the
 * guest ends. It runs on a runtime thread and must return promptly. */
typedef void (*kernarium_runtime_restart_callback)(
    void* context,
    uint32_t event,
    uint32_t restart_count);
/* Sets, or with a null callback clears, the one receiver of restart events.
 * Once it returns, no call to the previous callback is running or will start,
 * so its context may be released; the callback must not call this. */
typedef void (*kernarium_runtime_set_restart_callback_function)(
    kernarium_runtime_restart_callback callback,
    void* context);

/* The application resolves only this versioned getter after loading exactly
 * one runtime framework. The table is immutable and remains valid until
 * process exit. The selected framework owns one host runtime and its
 * matching code-signed kernel carrier. */
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
    kernarium_runtime_set_microphone_function set_microphone;
    kernarium_runtime_set_restart_callback_function set_restart_callback;
    kernarium_runtime_guest_display_active_function guest_display_active;
    kernarium_runtime_plan_gpu_budget_function plan_gpu_budget;
} kernarium_runtime_api;

#if defined(__GNUC__)
#define KERNARIUM_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define KERNARIUM_RUNTIME_EXPORT
#endif

KERNARIUM_RUNTIME_EXPORT
const kernarium_runtime_api* kernarium_runtime_get_api_v17(void);

/* An automation-only extension, outside the versioned table: only a runtime
 * built for automation exports this symbol, and the application looks it up
 * without requiring it. It returns nonzero when the launch asked, through the
 * environment, for the background job to start as soon as the guest runs, and
 * zero otherwise. The setting's name is in the automation runtimes alone, so a
 * release runtime cannot be asked. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_job_at_start(void);

/* Another automation-only extension, found the same way: how many
 * milliseconds after the application enters the background the launch asked
 * for the guest's GPU to be held, instead of at once, so a measurement can
 * find how long the host GPU stays usable there. Zero means no delay, and a
 * runtime built for release has no such symbol. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_hold_delay_ms(void);

#ifdef __cplusplus
}
#endif

#endif
