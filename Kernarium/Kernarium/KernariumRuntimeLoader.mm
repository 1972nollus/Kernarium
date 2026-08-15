// SPDX-License-Identifier: GPL-2.0-or-later

#import "KernariumRuntimeLoader.h"

#include "../Runtime/KernariumRuntime.h"

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
void* selectedHandle;
uint32_t selectedPageSize;
bool selectionAttempted;

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

[[nodiscard]] NSURL* findRuntimeFramework(uint32_t guestPageSize)
{
    NSString* variant = pageVariant(guestPageSize);
    NSURL* frameworks = NSBundle.mainBundle.privateFrameworksURL;
    if (variant == nil || frameworks == nil)
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
        if ([name hasPrefix:@"KernariumRuntime_"] &&
            [name hasSuffix:suffix])
            [matches addObject:candidate];
    }
    [matches sortUsingComparator:^NSComparisonResult(NSURL* first, NSURL* second) {
        return [first.lastPathComponent compare:second.lastPathComponent];
    }];
    if (matches.count != 1U) {
        std::fprintf(
            stderr,
            "Kernarium: expected one embedded %s runtime framework, found %lu\n",
            variant.UTF8String,
            static_cast<unsigned long>(matches.count));
        return nil;
    }
    return matches.firstObject;
}

[[nodiscard]] bool validApi(
    const kernarium_runtime_api* api,
    NSURL* framework,
    uint32_t guestPageSize)
{
    if (api == nullptr ||
        api->abi_version != KERNARIUM_RUNTIME_API_VERSION ||
        api->struct_size < sizeof(kernarium_runtime_api) ||
        api->guest_page_size != guestPageSize ||
        api->reserved != 0U ||
        api->kernel_release == nullptr ||
        api->framework_name == nullptr ||
        api->run == nullptr ||
        api->request_stop == nullptr ||
        api->send_touch == nullptr ||
        api->send_key == nullptr) {
        std::fputs("Kernarium: runtime API validation failed\n", stderr);
        return false;
    }

    NSString* release = [NSString stringWithUTF8String:api->kernel_release];
    NSString* reportedName = [NSString stringWithUTF8String:api->framework_name];
    NSString* variant = pageVariant(guestPageSize);
    if (release == nil || reportedName == nil || variant == nil)
        return false;
    NSString* releaseComponent = [release stringByReplacingOccurrencesOfString:@"."
                                                                    withString:@"_"];
    NSString* expectedName = [NSString stringWithFormat:
        @"KernariumRuntime_%@_%@", releaseComponent, variant];
    NSString* bundledName = framework.URLByDeletingPathExtension.lastPathComponent;
    if (![reportedName isEqualToString:expectedName] ||
        ![bundledName isEqualToString:expectedName]) {
        std::fprintf(
            stderr,
            "Kernarium: runtime identity mismatch: bundled=%s reported=%s "
            "expected=%s\n",
            bundledName.UTF8String,
            reportedName.UTF8String,
            expectedName.UTF8String);
        return false;
    }
    return true;
}

[[nodiscard]] int loadRuntime(uint32_t guestPageSize)
{
    NSURL* framework = findRuntimeFramework(guestPageSize);
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
    void* symbol = dlsym(handle, "kernarium_runtime_get_api_v1");
    const char* symbolError = dlerror();
    if (symbol == nullptr || symbolError != nullptr) {
        std::fprintf(
            stderr,
            "Kernarium: selected runtime lacks API v1: %s\n",
            symbolError == nullptr ? "symbol unavailable" : symbolError);
        (void)dlclose(handle);
        return -1;
    }
    const auto getter = reinterpret_cast<
        const kernarium_runtime_api* (*)(void)>(symbol);
    const kernarium_runtime_api* api = getter();
    if (!validApi(api, framework, guestPageSize)) {
        (void)dlclose(handle);
        return -1;
    }

    selectedHandle = handle;
    selectedPageSize = guestPageSize;
    selectedApi.store(api, std::memory_order_release);
    std::printf(
        "Kernarium: loaded %s (Linux %s, %u-byte guest pages)\n",
        api->framework_name,
        api->kernel_release,
        api->guest_page_size);
    return 0;
}

[[nodiscard]] const kernarium_runtime_api* api() noexcept
{
    return selectedApi.load(std::memory_order_acquire);
}

} // namespace

extern "C" int kernarium_runtime_loader_select(uint32_t guestPageSize)
{
    if (pageVariant(guestPageSize) == nil) {
        std::fprintf(
            stderr,
            "Kernarium: unsupported guest page size: %u\n",
            guestPageSize);
        return -1;
    }

    std::lock_guard lock(selectionMutex);
    if (api() != nullptr)
        return selectedPageSize == guestPageSize ? 0 : -1;
    if (selectionAttempted)
        return -1;
    selectionAttempted = true;
    return loadRuntime(guestPageSize);
}

extern "C" int kernarium_runtime_loader_run(
    MTKView* view,
    const char* rootfsPath,
    const char* overlayPath,
    uint32_t width,
    uint32_t height,
    uint32_t dotsPerInch,
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr) {
        std::fputs("Kernarium: no runtime framework is selected\n", stderr);
        return -1;
    }
    return runtime->run(
        view,
        rootfsPath,
        overlayPath,
        width,
        height,
        dotsPerInch,
        guestMemoryMiB,
        guestProcessorCount);
}

extern "C" void kernarium_runtime_loader_request_stop(void)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->request_stop();
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
