// SPDX-License-Identifier: GPL-2.0-or-later

#import "KernariumRuntimeLoader.h"

#import <Foundation/Foundation.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr uint32_t fourKiB = 4096U;
constexpr uint32_t sixteenKiB = 16384U;

std::mutex selectionMutex;
std::atomic<const kernarium_runtime_api*> selectedApi;
uint32_t selectedPageSize;
uint32_t selectedFlavor;
bool selectionAttempted;
std::mutex consoleOutputHandlerMutex;
KernariumConsoleOutputHandler consoleOutputHandler;

void forwardConsoleOutput(
    void*,
    const void* buffer,
    std::size_t length)
{
    if (buffer == nullptr || length == 0U)
        return;
    NSData* data = [NSData dataWithBytes:buffer length:length];
    KernariumConsoleOutputHandler handler;
    {
        std::lock_guard lock(consoleOutputHandlerMutex);
        handler = consoleOutputHandler;
    }
    if (handler == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(data);
    });
}

void updateConsoleOutputForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard lock(consoleOutputHandlerMutex);
    runtime->set_console_output_callback(
        consoleOutputHandler == nil ? nullptr : forwardConsoleOutput,
        nullptr);
}

[[noreturn]] void guestTimerMaskFailure() noexcept
{
    static constexpr char message[] =
        "Kernarium: unable to isolate the UML timer signal\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    _exit(127);
}

void blockGuestTimer() noexcept
{
    sigset_t blocked;
    if (sigemptyset(&blocked) != 0 ||
        sigaddset(&blocked, SIGALRM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
        guestTimerMaskFailure();
}

/*
 * The process-directed early ITIMER_REAL calibration must not land on a
 * UIKit or Swift worker. This hook stays in the always-linked application so
 * it runs before main even though the selected emulator framework is loaded
 * later. Only the kernel's vCPU0 carrier temporarily unmasks SIGALRM.
 */
__attribute__((constructor)) void isolateGuestTimerFromApplicationThreads()
{
    blockGuestTimer();
}

[[nodiscard]] NSString* pageVariant(uint32_t guestPageSize)
{
    switch (guestPageSize) {
    case fourKiB:
        return @"4K";
    case sixteenKiB:
        return @"16K";
    default:
        return nil;
    }
}

/* The flavour leads the framework name. Linux additionally requires a numeric
 * release after its prefix because the specialized names share its stem. */
[[nodiscard]] NSString* flavorPrefix(uint32_t guestFlavor)
{
    switch (guestFlavor) {
    case KERNARIUM_RUNTIME_GUEST_LINUX:
        return @"KRuntime_";
    case KERNARIUM_RUNTIME_GUEST_ANDROID:
        return @"KRuntime_ACK_";
    default:
        return nil;
    }
}

[[nodiscard]] bool matchesFlavor(
    NSString* name,
    NSString* prefix,
    uint32_t guestFlavor)
{
    if (![name hasPrefix:prefix])
        return false;
    if (guestFlavor != KERNARIUM_RUNTIME_GUEST_LINUX)
        return true;

    NSUInteger releaseIndex = prefix.length;
    if (name.length <= releaseIndex)
        return false;
    unichar releaseFirst = [name characterAtIndex:releaseIndex];
    return releaseFirst >= '0' && releaseFirst <= '9';
}

[[nodiscard]] NSURL* findRuntimeFramework(
    NSString* prefix,
    NSString* variant,
    uint32_t guestFlavor)
{
    NSURL* frameworks = NSBundle.mainBundle.privateFrameworksURL;
    if (frameworks == nil)
        return nil;

    NSError* error = nil;
    NSArray<NSURL*>* contents = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:frameworks
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
        options:NSDirectoryEnumerationSkipsHiddenFiles
        error:&error];
    if (contents == nil) {
        std::fprintf(
            stderr,
            "Kernarium: cannot inspect embedded frameworks: %s\n",
            error.localizedDescription.UTF8String ?: "unknown error");
        return nil;
    }

    NSString* suffix = [NSString stringWithFormat:@"_%@.framework", variant];
    NSMutableArray<NSURL*>* matches = [NSMutableArray array];
    for (NSURL* candidate in contents) {
        NSString* name = candidate.lastPathComponent;
        if (matchesFlavor(name, prefix, guestFlavor) &&
            [name hasSuffix:suffix])
            [matches addObject:candidate];
    }
    [matches sortUsingComparator:^NSComparisonResult(NSURL* first, NSURL* second) {
        return [first.lastPathComponent compare:second.lastPathComponent];
    }];
    if (matches.count != 1U) {
        std::fprintf(
            stderr,
            "Kernarium: expected one embedded %s%s runtime framework, "
            "found %lu\n",
            prefix.UTF8String,
            variant.UTF8String,
            static_cast<unsigned long>(matches.count));
        return nil;
    }
    return matches.firstObject;
}

[[nodiscard]] bool validApi(
    const kernarium_runtime_api* api,
    uint32_t guestFlavor,
    uint32_t guestPageSize)
{
    if (api == nullptr ||
        api->abi_version != KERNARIUM_RUNTIME_API_VERSION ||
        api->struct_size < sizeof(kernarium_runtime_api) ||
        api->guest_page_size != guestPageSize ||
        api->guest_flavor != guestFlavor ||
        api->run == nullptr ||
        api->request_stop == nullptr ||
        api->sync == nullptr ||
        api->quiesce == nullptr ||
        api->resume == nullptr ||
        api->send_touch == nullptr ||
        api->send_key == nullptr ||
        api->set_console_output_callback == nullptr ||
        api->send_console_input == nullptr ||
        api->set_gpu_hold == nullptr) {
        std::fputs("Kernarium: runtime API validation failed\n", stderr);
        return false;
    }
    return true;
}

/* `prefix` and `variant` name the framework for `guestFlavor` and
 * `guestPageSize`; the caller has resolved both. The handle stays loaded for
 * the life of the process. */
[[nodiscard]] int loadRuntime(
    NSString* prefix,
    NSString* variant,
    uint32_t guestFlavor,
    uint32_t guestPageSize)
{
    NSURL* framework = findRuntimeFramework(prefix, variant, guestFlavor);
    NSBundle* bundle = framework == nil ? nil : [NSBundle bundleWithURL:framework];
    NSString* executable = bundle.executablePath;
    if (executable == nil) {
        std::fputs("Kernarium: selected runtime has no framework executable\n", stderr);
        return -1;
    }

    dlerror();
    void* handle = dlopen(executable.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        std::fprintf(
            stderr,
            "Kernarium: cannot load selected runtime: %s\n",
            error == nullptr ? "unknown dlopen error" : error);
        return -1;
    }

    dlerror();
    void* symbol = dlsym(handle, "kernarium_runtime_get_api_v11");
    const char* symbolError = dlerror();
    if (symbol == nullptr || symbolError != nullptr) {
        std::fprintf(
            stderr,
            "Kernarium: selected runtime lacks API v11: %s\n",
            symbolError == nullptr ? "symbol unavailable" : symbolError);
        (void)dlclose(handle);
        return -1;
    }
    const auto getter = reinterpret_cast<
        const kernarium_runtime_api* (*)(void)>(symbol);
    const kernarium_runtime_api* api = getter();
    if (!validApi(api, guestFlavor, guestPageSize)) {
        (void)dlclose(handle);
        return -1;
    }

    updateConsoleOutputForwarder(api);
    selectedPageSize = guestPageSize;
    selectedFlavor = guestFlavor;
    selectedApi.store(api, std::memory_order_release);
    std::printf(
        "Kernarium: loaded runtime with %u-byte guest pages\n",
        api->guest_page_size);
    return 0;
}

[[nodiscard]] const kernarium_runtime_api* api() noexcept
{
    return selectedApi.load(std::memory_order_acquire);
}

} // namespace

extern "C" int kernarium_runtime_loader_select(
    uint32_t guestFlavor,
    uint32_t guestPageSize)
{
    NSString* variant = pageVariant(guestPageSize);
    NSString* prefix = flavorPrefix(guestFlavor);
    if (variant == nil || prefix == nil) {
        std::fprintf(
            stderr,
            "Kernarium: unsupported guest flavour %u or page size %u\n",
            guestFlavor,
            guestPageSize);
        return -1;
    }

    std::lock_guard lock(selectionMutex);
    if (api() != nullptr) {
        return selectedPageSize == guestPageSize &&
                selectedFlavor == guestFlavor
            ? 0 : -1;
    }
    if (selectionAttempted)
        return -1;
    selectionAttempted = true;
    return loadRuntime(prefix, variant, guestFlavor, guestPageSize);
}

extern "C" int kernarium_runtime_loader_run(
    MTKView* view,
    const kernarium_runtime_boot* boot)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr) {
        std::fputs("Kernarium: no runtime framework is selected\n", stderr);
        return -1;
    }
    return runtime->run(view, boot);
}

extern "C" void kernarium_runtime_loader_request_stop(void)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->request_stop();
}

extern "C" int kernarium_runtime_loader_sync(void)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->sync();
}

extern "C" int kernarium_runtime_loader_quiesce(void)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->quiesce();
}

extern "C" void kernarium_runtime_loader_resume(void)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->resume();
}

extern "C" int kernarium_runtime_loader_set_gpu_hold(int held)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->set_gpu_hold(held);
}

extern "C" void kernarium_runtime_loader_send_touch(
    uint32_t x,
    uint32_t y,
    int pressed)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->send_touch(x, y, pressed);
}

extern "C" void kernarium_runtime_loader_send_key(uint16_t code, int pressed)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->send_key(code, pressed);
}

extern "C" void kernarium_runtime_loader_set_console_output_handler(
    KernariumConsoleOutputHandler handler)
{
    {
        std::lock_guard lock(consoleOutputHandlerMutex);
        consoleOutputHandler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateConsoleOutputForwarder(runtime);
}

extern "C" int kernarium_runtime_loader_send_console_input(
    const void* buffer,
    std::size_t length)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr)
        return -1;
    return runtime->send_console_input(buffer, length);
}
