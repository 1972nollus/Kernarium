// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_LOADER_H
#define KERNARIUM_RUNTIME_LOADER_H

#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>
#include <stddef.h>
#include <stdint.h>

#include "../../Kernarium/Runtime/KernariumRuntime.h"

/* UML_APP_STORE_APPLICATION is defined by the App Store configuration of the
 * application that compiles this loader (Andarium/README.md#configurations).
 * That configuration leaves out the loader's debugging and automation
 * entries: the display mirror, console input and the automation lookups. The
 * App Store runtime answers the first two with -ENOTSUP in any case. */

NS_ASSUME_NONNULL_BEGIN

typedef void (^KernariumConsoleOutputHandler)(NSData* data);
/* A restart event of the guest, delivered on the main queue: `event` is a
 * KERNARIUM_RUNTIME_RESTART_ value and `restartCount` the number of times the
 * guest has restarted. */
typedef void (^KernariumRestartHandler)(
    uint32_t event,
    uint32_t restartCount);

#ifdef __cplusplus
extern "C" {
#endif

/* Selects and loads one bundled runtime for the lifetime of the process. The
 * flavour names exactly one embedded framework; every runtime has 4 KiB
 * guest pages. */
int kernarium_runtime_loader_select(uint32_t guest_flavor);

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
/* Whether the guest's display is on; see
 * kernarium_runtime_guest_display_active_function. Returns the runtime's
 * answer, or -1 when no runtime is loaded. */
int kernarium_runtime_loader_guest_display_active(void);
/* The running guest's host-side figures; see
 * kernarium_runtime_read_statistics_function. Returns the runtime's answer, or
 * -ENODEV when no runtime is loaded. */
int kernarium_runtime_loader_read_statistics(
    kernarium_runtime_statistics* statistics);
/* The guest kernel's own memory figures; see
 * kernarium_runtime_read_guest_memory_function. Returns the runtime's answer,
 * or -ENODEV when no runtime is loaded. Call it away from the main thread: it
 * may wait up to 500 ms. */
int kernarium_runtime_loader_read_guest_memory(
    kernarium_runtime_guest_memory* memory);
/* Moves the guest's vCPU threads to (nonzero) or from (zero) the system's
 * background policy; see kernarium_runtime_set_background_throttle_function.
 * Returns the number of threads changed, or -1 when no runtime is loaded. */
int kernarium_runtime_loader_set_background_throttle(int enabled);
#ifndef UML_APP_STORE_APPLICATION
/* Mirrors the guest's display to `sink`, or stops the mirror for a null sink;
 * see kernarium_runtime_set_frame_sink_function. Returns the runtime's answer,
 * or -1 when no runtime is loaded. */
int kernarium_runtime_loader_set_frame_sink(
    _Nullable kernarium_runtime_frame_sink sink,
    void* _Nullable context,
    uint32_t maximum_width,
    uint32_t frames_per_second);
#endif
/* Turns periodic guest page-cache trimming on (nonzero) only while the guest
 * is kept running in the background, and off again on return to the
 * foreground. Returns 0 once the guest has applied it, a negative value
 * otherwise, -1 when no runtime is loaded. Call it away from the main thread:
 * it may wait for the guest's console. */
int kernarium_runtime_loader_set_page_cache_trimming(int enabled);
/* Evaluates the host GPU's budget for a launch with these parameters, as the
 * selected runtime's run will, and fills `plan`; see
 * kernarium_runtime_plan_gpu_budget_function. Returns the runtime's answer, or
 * -1 when no runtime is loaded. */
int kernarium_runtime_loader_plan_gpu_budget(
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    uint32_t application_mib,
    uint32_t engine_mib,
    uint32_t headroom_mib,
    kernarium_runtime_gpu_budget* plan);
/* A, E_rest and S of the host GPU's budget in MiB: the launch parameters
 * (runtime/guest_memory_budget.hpp). */
typedef struct kernarium_launch_memory_set {
    uint32_t application_mib;
    uint32_t engine_mib;
    uint32_t headroom_mib;
} kernarium_launch_memory_set;

/* The parameters a launch states unless it states its own. The loader answers
 * the next two functions itself, from the same header the runtime's budget
 * comes from: the runtime is loaded once, for the flavour a launch chooses, so
 * the settings cannot ask it. */
kernarium_launch_memory_set kernarium_runtime_loader_launch_memory_set(void);
/* The most guest RAM, in MiB, the settings offer at `guest_processor_count`
 * vCPUs with `parameters` (zero vCPUs count as one): the largest multiple of 64
 * from 512 through 4096 that leaves the host GPU its 384 MiB floor under the
 * memory limit the system sets this process. 4096 where the system sets none,
 * the Mac included; 512 where even that leaves less than the floor. */
uint32_t kernarium_runtime_loader_maximum_guest_memory_mib(
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The vCPU count the applications default to and offer at most on a device with
 * `physical_processor_count` physical cores (zero counts as one): every core,
 * and never more than the count the memory budget is measured for
 * (application_vcpu_limit, runtime/guest_memory_budget.hpp). The loader answers
 * it too, for the same reason. */
uint32_t kernarium_runtime_loader_application_vcpu_limit(
    uint32_t physical_processor_count);
/* Turns the guest's microphone on (nonzero) or off (zero), as
 * kernarium_runtime_set_microphone_function does, and returns the resulting
 * KERNARIUM_RUNTIME_MICROPHONE_ value, or -1 when no runtime is loaded. It
 * never asks the system for access; the caller asks first. */
int kernarium_runtime_loader_set_microphone(int enabled);
void kernarium_runtime_loader_send_touch(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed);
/* Withdraws the contact in `slot` instead of releasing it, as
 * kernarium_runtime_cancel_touch_function does. */
void kernarium_runtime_loader_cancel_touch(uint32_t slot);
void kernarium_runtime_loader_send_key(uint16_t code, int pressed);
void kernarium_runtime_loader_set_console_output_handler(
    KernariumConsoleOutputHandler _Nullable handler);
/* Receives the guest's restart events (see
 * kernarium_runtime_set_restart_callback_function), or none for a nil handler.
 * It may be set before a runtime is selected. */
void kernarium_runtime_loader_set_restart_handler(
    KernariumRestartHandler _Nullable handler);
#ifndef UML_APP_STORE_APPLICATION
/* Whether the selected runtime is an automation build and the launch asked it,
 * through the environment, to start the background job with the guest. Always
 * zero for a release runtime, which cannot be asked, and when none is loaded. */
int kernarium_runtime_loader_automation_job_at_start(void);
/* The milliseconds after the application enters the background at which an
 * automation launch asked for the guest's GPU to be held, or zero for the
 * edge itself. Always zero for a release runtime, which cannot be asked, and
 * when none is loaded. */
int kernarium_runtime_loader_automation_hold_delay_ms(void);
int kernarium_runtime_loader_send_console_input(
    const void* buffer,
    size_t length);
#endif
/* Opens a stream to the guest's vsock port; see
 * kernarium_runtime_connect_vsock_function. Returns a connected descriptor
 * the caller owns, or a negated errno: -ENODEV when no runtime is loaded or no
 * guest runs. It blocks for up to `timeout_ms`, so call it away from the main
 * thread. */
int kernarium_runtime_loader_connect_vsock(
    uint32_t port,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

#endif
