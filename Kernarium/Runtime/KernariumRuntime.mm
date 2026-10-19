// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumRuntime.h"
#include "KernariumConsoleInput.hpp"
#if KERNARIUM_AUTOMATION_HARNESS
#include "KernariumHarness.hpp"
#endif

#include "audio/in_process_service.hpp"
#include "video/in_process_service.hpp"
#include "video/virtio_video_device.hpp"
#include "audio/protocol.hpp"
#include "balloon/in_process_service.hpp"
#include "balloon/protocol.hpp"
#include "battery/in_process_service.hpp"
#include "battery/protocol.hpp"
#include "console/in_process_service.hpp"
#include "console/protocol.hpp"
#include "gpu/frame_tap.hpp"
#include "gpu/in_process_service.hpp"
#include "gpu/protocol.hpp"
#include "input/virtio/in_process_service.hpp"
#include "block/android_boot.hpp"
#include "block/boot_initramfs.hpp"
#include "block/ubd_path.hpp"
#include "uml/emulator.h"
#include "uml_slirp_net.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char uml_elf_start[]
    __asm("section$start$__TEXT$__uml_elf");
extern const unsigned char uml_elf_end[]
    __asm("section$end$__TEXT$__uml_elf");
#if !KERNARIUM_RUNTIME_ANDROID_GUEST
/* The accepted aarch64 boot base archive and the generic per-run /init
 * (tools/rootfs/boot-ramdisk), linked in beside the kernel. */
extern const unsigned char uml_boot_base_start[]
    __asm("section$start$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_base_end[]
    __asm("section$end$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_init_start[]
    __asm("section$start$__TEXT$__uml_boot_init");
extern const unsigned char uml_boot_init_end[]
    __asm("section$end$__TEXT$__uml_boot_init");
#endif
}

namespace {

[[noreturn]] void guestTimerMaskFailure() noexcept
{
    static constexpr char message[] =
        "Kernarium: unable to isolate the UML timer signal\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    _exit(127);
}

void blockGuestTimer(sigset_t* previous) noexcept
{
    sigset_t blocked;
    if (sigemptyset(&blocked) != 0 ||
        sigaddset(&blocked, SIGALRM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, previous) != 0)
        guestTimerMaskFailure();
}

class ScopedGuestTimerBlock final {
public:
    ScopedGuestTimerBlock() noexcept
    {
        blockGuestTimer(&previous_);
    }

    ~ScopedGuestTimerBlock()
    {
        if (::pthread_sigmask(SIG_SETMASK, &previous_, nullptr) != 0)
            guestTimerMaskFailure();
    }

    ScopedGuestTimerBlock(const ScopedGuestTimerBlock&) = delete;
    ScopedGuestTimerBlock& operator=(const ScopedGuestTimerBlock&) = delete;

private:
    sigset_t previous_ {};
};

std::atomic_bool runtimeActive;
/*
 * Each registration has its own mutex, and a call through a registered pointer
 * holds the mutex that registration's destructor takes, so the pointer stays
 * valid for that call. Sync, quiesce and resume can take seconds, so they
 * serialize on a lifecycle mutex of their own rather than stall a stop request
 * or main-thread touch and key input; the emulator registration takes both.
 * Console input can wait on a guest that stops reading, so it goes through a
 * registry of its own that no caller holds while it writes; see
 * kernarium::console::InputRegistry.
 */
std::mutex emulatorLifecycleMutex;
std::mutex activeEmulatorMutex;
uml_emulator* activeEmulator;
std::mutex activeInputMutex;
uml::input::virtio::InProcessInputService* activeTouchscreen;
uml::input::virtio::InProcessInputService* activeKeyboard;
/* The GPU hold the application last asked for, and the GPU service it applies
 * to while a guest runs. A hold asked for before the guest's GPU exists takes
 * effect when it does. */
std::mutex activeGpuMutex;
uml::gpu::InProcessGpuService* activeGpu;
bool gpuHoldRequested;
/* Longest a hold waits for a GPU request already running. The application
 * holds inside didEnterBackground, which iOS gives about five seconds. */
constexpr std::chrono::milliseconds gpuHoldBudget {1000};
/* Process lifetime, like the input callers it serves: the application's
 * terminal can still write while the process exits. */
kernarium::console::InputRegistry& consoleInputs =
    *new kernarium::console::InputRegistry;
std::mutex consoleOutputCallbackMutex;
std::mutex consoleStdoutMutex;
kernarium_runtime_console_output_callback consoleOutputCallback;
void* consoleOutputContext;

void publishConsoleOutput(const void* buffer, std::size_t length)
{
    kernarium_runtime_console_output_callback callback;
    void* context;
    {
        std::lock_guard lock(consoleOutputCallbackMutex);
        callback = consoleOutputCallback;
        context = consoleOutputContext;
    }
    if (callback != nullptr)
        callback(context, buffer, length);
}

class AppNetwork final {
public:
    AppNetwork()
        : context_(uml_slirp_net_create())
    {
        if (context_ == nullptr)
            throw std::bad_alloc {};
        const char* forwarding = std::getenv("KERNARIUM_HOST_FORWARD");
        if (forwarding != nullptr && forwarding[0] != '\0') {
            const int error = uml_slirp_net_add_forward(context_, forwarding);
            if (error != 0) {
                uml_slirp_net_destroy(context_);
                context_ = nullptr;
                throw std::runtime_error(
                    "invalid Kernarium host-forward specification: " +
                    std::to_string(error));
            }
        }
    }

    ~AppNetwork()
    {
        if (context_ != nullptr) {
            uml_slirp_net_stop(context_);
            uml_slirp_net_destroy(context_);
        }
    }

    AppNetwork(const AppNetwork&) = delete;
    AppNetwork& operator=(const AppNetwork&) = delete;

    [[nodiscard]] uml_emulator_network_callbacks callbacks() noexcept
    {
        return {
            .struct_size = sizeof(uml_emulator_network_callbacks),
            .context = context_,
            .open = uml_slirp_net_open,
            .irq_fd = uml_slirp_net_irq_fd,
            .read = uml_slirp_net_read,
            .write = uml_slirp_net_write,
        };
    }

private:
    void* context_ {};
};


class ActiveRuntime final {
public:
    ActiveRuntime()
    {
        if (runtimeActive.exchange(true, std::memory_order_acq_rel))
            throw std::runtime_error("a Kernarium guest is already active");
    }

    ~ActiveRuntime()
    {
        runtimeActive.store(false, std::memory_order_release);
    }

    ActiveRuntime(const ActiveRuntime&) = delete;
    ActiveRuntime& operator=(const ActiveRuntime&) = delete;
};

/* KERNARIUM_FOOTPRINT_LOG_MS reports the process footprint, the figure iOS
 * holds the application to, while the guest runs: one line per interval and
 * the peak when the guest stops. Each line also gives the headroom the kernel
 * leaves below the application's memory limit, so the footprint plus the
 * headroom is the limit and the least headroom is the closest approach, and
 * the process's CPU time, estimated energy, and performance-core time so far:
 * the difference between two lines over their interval is the average core use
 * and power, and how much of it ran on performance cores. */
class FootprintReporter final {
public:
    static long intervalMilliseconds() noexcept
    {
        const char* value = std::getenv("KERNARIUM_FOOTPRINT_LOG_MS");
        return value == nullptr ? 0 : std::strtol(value, nullptr, 10);
    }

    static bool enabled() noexcept { return intervalMilliseconds() > 0; }

    FootprintReporter()
    {
        const long interval = intervalMilliseconds();
        if (interval <= 0)
            return;
        interval_ = std::chrono::milliseconds {interval};
        /* The guest's timer signal must never land on this thread. */
        const ScopedGuestTimerBlock guestTimerBlock;
        thread_ = std::thread([this] { run(); });
    }

    ~FootprintReporter()
    {
        if (!thread_.joinable())
            return;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        stopped_.notify_one();
        thread_.join();
        std::printf("Kernarium: footprint peak %.1f MiB least remaining %.1f MiB\n",
            peak_ / 1048576.0, leastRemaining_ / 1048576.0);
    }

    FootprintReporter(const FootprintReporter&) = delete;
    FootprintReporter& operator=(const FootprintReporter&) = delete;

private:
    struct Sample {
        std::uint64_t footprint;
        std::uint64_t remaining;
        /* The share of the footprint charged as graphics memory: Metal
         * resources and the driver's command storage alike. */
        std::uint64_t graphics;
        /* The process's user and system CPU time so far, every thread that
         * ever ran included: the difference between two lines over their
         * interval is the process's average core use. */
        double cpuSeconds;
        /* What the system estimates the process has consumed, in joules, and
         * how many of its CPU seconds ran on performance cores. */
        double energyJoules;
        double performanceCoreSeconds;
    };

    struct PowerSample {
        double energyJoules;
        double performanceCoreSeconds;
    };

    static PowerSample processPower() noexcept
    {
        task_power_info_v2_data_t info {};
        mach_msg_type_number_t count = TASK_POWER_INFO_V2_COUNT;
        if (task_info(mach_task_self(), TASK_POWER_INFO_V2,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return {0.0, 0.0};
        mach_timebase_info_data_t timebase {};
        (void)mach_timebase_info(&timebase);
        const double ticksToSeconds = timebase.denom == 0 ? 0.0 :
            static_cast<double>(timebase.numer) /
                static_cast<double>(timebase.denom) / 1e9;
        return {
            static_cast<double>(info.task_energy) / 1e9,
            static_cast<double>(info.task_ptime) * ticksToSeconds,
        };
    }

    static double processCpuSeconds() noexcept
    {
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) != 0)
            return 0.0;
        const auto seconds = [](const timeval& time) {
            return static_cast<double>(time.tv_sec) +
                static_cast<double>(time.tv_usec) / 1e6;
        };
        return seconds(usage.ru_utime) + seconds(usage.ru_stime);
    }

    static Sample sample() noexcept
    {
        task_vm_info_data_t info {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return {0, UINT64_MAX, 0, processCpuSeconds(), 0.0, 0.0};
        const std::int64_t graphics = info.ledger_tag_graphics_footprint +
            info.ledger_tag_graphics_footprint_compressed;
        const PowerSample power = processPower();
        return {info.phys_footprint, info.limit_bytes_remaining,
            static_cast<std::uint64_t>(std::max<std::int64_t>(graphics, 0)),
            processCpuSeconds(), power.energyJoules,
            power.performanceCoreSeconds};
    }

    void run()
    {
        (void)pthread_setname_np("kernarium-footprint");
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex_);
        while (!stopped_.wait_for(lock, interval_, [this] { return stopping_; })) {
            const Sample current = sample();
            peak_ = std::max(peak_, current.footprint);
            leastRemaining_ = std::min(leastRemaining_, current.remaining);
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("Kernarium: footprint %.1f s %.1f MiB graphics %.1f MiB "
                "remaining %.1f MiB cpu %.2f s energy %.2f J pcore %.2f s\n",
                elapsed, current.footprint / 1048576.0,
                current.graphics / 1048576.0, current.remaining / 1048576.0,
                current.cpuSeconds, current.energyJoules,
                current.performanceCoreSeconds);
        }
    }

    std::chrono::milliseconds interval_ {};
    std::mutex mutex_;
    std::condition_variable stopped_;
    bool stopping_ {};
    std::uint64_t peak_ {};
    std::uint64_t leastRemaining_ {UINT64_MAX};
    std::thread thread_;
};

class ActiveEmulatorRegistration final {
public:
    explicit ActiveEmulatorRegistration(uml_emulator* emulator)
    {
        std::scoped_lock lock(emulatorLifecycleMutex, activeEmulatorMutex);
        activeEmulator = emulator;
    }

    ~ActiveEmulatorRegistration()
    {
        std::scoped_lock lock(emulatorLifecycleMutex, activeEmulatorMutex);
        activeEmulator = nullptr;
    }

    ActiveEmulatorRegistration(const ActiveEmulatorRegistration&) = delete;
    ActiveEmulatorRegistration& operator=(
        const ActiveEmulatorRegistration&) = delete;
};

class ActiveInputRegistration final {
public:
    ActiveInputRegistration(
        uml::input::virtio::InProcessInputService& touchscreen,
        uml::input::virtio::InProcessInputService& keyboard)
    {
        std::lock_guard lock(activeInputMutex);
        activeTouchscreen = &touchscreen;
        activeKeyboard = &keyboard;
    }

    ~ActiveInputRegistration()
    {
        std::lock_guard lock(activeInputMutex);
        activeTouchscreen = nullptr;
        activeKeyboard = nullptr;
    }

    ActiveInputRegistration(const ActiveInputRegistration&) = delete;
    ActiveInputRegistration& operator=(const ActiveInputRegistration&) = delete;
};

class ActiveGpuRegistration final {
public:
    explicit ActiveGpuRegistration(uml::gpu::InProcessGpuService& gpu)
    {
        std::lock_guard lock(activeGpuMutex);
        activeGpu = &gpu;
        if (gpuHoldRequested)
            (void)gpu.holdRequests(gpuHoldBudget);
    }

    ~ActiveGpuRegistration()
    {
        std::lock_guard lock(activeGpuMutex);
        activeGpu = nullptr;
    }

    ActiveGpuRegistration(const ActiveGpuRegistration&) = delete;
    ActiveGpuRegistration& operator=(const ActiveGpuRegistration&) = delete;
};

class ConsoleBridge final {
public:
    ConsoleBridge()
    {
        int descriptors[2] {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "console socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        guest_ = descriptors[0];
        const int host = descriptors[1];
        const int enabled = 1;
        if (setsockopt(
                host,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)) != 0) {
            const int error = errno;
            close(guest_);
            close(host);
            guest_ = -1;
            throw std::runtime_error(
                "console socket setup failed: " +
                std::string(std::strerror(error)));
        }
        try {
            host_.emplace(host);
        } catch (...) {
            close(guest_);
            close(host);
            guest_ = -1;
            throw;
        }
        try {
            relay_ = std::thread([this] {
                char buffer[4096];
                for (;;) {
                    const ssize_t received = host_->receive(
                        buffer, sizeof(buffer));
                    if (received > 0) {
                        publishConsoleOutput(
                            buffer, static_cast<std::size_t>(received));
                        std::lock_guard outputLock(consoleStdoutMutex);
                        std::string_view output(
                            buffer, static_cast<std::size_t>(received));
                        while (!output.empty()) {
                            const ssize_t written = write(
                                STDOUT_FILENO,
                                output.data(),
                                output.size());
                            if (written > 0) {
                                output.remove_prefix(
                                    static_cast<std::size_t>(written));
                            } else if (written < 0 && errno == EINTR) {
                                continue;
                            } else {
                                break;
                            }
                        }
                        continue;
                    }
                    if (received < 0 && errno == EINTR)
                        continue;
                    if (received < 0 && errno != ECONNRESET) {
                        std::fprintf(
                            stderr,
                            "Kernarium: console relay failed: %s\n",
                            std::strerror(errno));
                    }
                    break;
                }
                host_->close();
            });
        } catch (...) {
            close(guest_);
            guest_ = -1;
            throw;
        }
    }

    ~ConsoleBridge()
    {
        /* A console write still in progress ends with the input, and the
         * relay's receive ends when the guest end closes. */
        host_->stop();
        if (guest_ >= 0) {
            (void)shutdown(guest_, SHUT_RDWR);
            close(guest_);
        }
        if (relay_.joinable())
            relay_.join();
    }

    ConsoleBridge(const ConsoleBridge&) = delete;
    ConsoleBridge& operator=(const ConsoleBridge&) = delete;

    [[nodiscard]] kernarium::console::HostEnd& host() noexcept
    {
        return *host_;
    }

    [[nodiscard]] std::string kernelArgument() const
    {
        return "ssl0=fd:" + std::to_string(guest_) + ",fd:" +
            std::to_string(guest_);
    }

private:
    int guest_ {-1};
    std::optional<kernarium::console::HostEnd> host_;
    std::thread relay_;
};

class ActiveConsoleRegistration final {
public:
    explicit ActiveConsoleRegistration(ConsoleBridge& console)
    {
        consoleInputs.publish(console.host());
    }

    /* The guest has stopped reading for good, so a write that still waits
     * for it ends here rather than holding the run's teardown. */
    ~ActiveConsoleRegistration()
    {
        consoleInputs.retire();
    }

    ActiveConsoleRegistration(const ActiveConsoleRegistration&) = delete;
    ActiveConsoleRegistration& operator=(
        const ActiveConsoleRegistration&) = delete;
};

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
bool usableFile(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ext4Rootfs(const char* path)
{
    std::string extension = std::filesystem::path {path}.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".ext4";
}

bool immutableExt4Rootfs(const char* path)
{
    if (!ext4Rootfs(path))
        return true;
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    if (error)
        return false;
    constexpr auto writable =
        std::filesystem::perms::owner_write |
        std::filesystem::perms::group_write |
        std::filesystem::perms::others_write;
    return (permissions & writable) == std::filesystem::perms::none;
}

#endif

/* The translating engine to run guest userspace under.
 *
 * "auto" defers to the build's platform default and its availability check
 * rather than deciding here, which keeps executable-memory policy with its
 * single owner in the CPU layer. Both Apple products resolve that to the
 * data-only gadget engine: a device cannot publish into a MAP_JIT mapping at
 * all, and while the Simulator can, measurement rejects the JIT for the
 * browser workloads that matter (G157). The override exists so that choice
 * stays re-testable without a rebuild.
 */
const char* selectedExecutionEngine()
{
    const char* requested = std::getenv("KERNARIUM_EXECUTION_ENGINE");
    if (requested == nullptr)
        return "auto";
    for (const char* allowed : {"auto", "jit", "gadgets"}) {
        if (std::strcmp(requested, allowed) == 0)
            return allowed;
    }
    std::fputs(
        "Kernarium: ignoring unknown KERNARIUM_EXECUTION_ENGINE\n", stderr);
    return "auto";
}

#if KERNARIUM_RUNTIME_ANDROID_GUEST
void appendKernelCommandLine(
    std::vector<std::string>& arguments,
    const std::filesystem::path& path,
    uint32_t dotsPerInch)
{
    constexpr std::string_view densityPrefix {
        "androidboot.lcd_density="};
    constexpr std::string_view adbAuthorizationPrefix {
        "androidboot.kernarium.adb_auth="};
    constexpr std::array launcherPrefixes {
        densityPrefix, adbAuthorizationPrefix};
    for (auto& argument :
         uml::android::readKernelCommandLine(path, launcherPrefixes))
        arguments.push_back(std::move(argument));

    /* Android init imports this as ro.boot.lcd_density. The product's vendor
     * init then publishes the same value as ro.sf.lcd_density, keeping the
     * framework's logical density aligned with the per-launch EDID. Swift has
     * already scaled dotsPerInch by nativeScale when Retina mode is active. */
    arguments.emplace_back(
        std::string {densityPrefix} + std::to_string(dotsPerInch));
    /* androidboot properties become immutable ro.boot properties. iOS always
     * keeps the stock authorization prompt for a previously unseen ADB host. */
    arguments.emplace_back(std::string {adbAuthorizationPrefix} + "1");
}

#endif

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
std::string fbconFontArgument(uint32_t dotsPerInch)
{
    static constexpr uint32_t largeConsoleFontDpi = 240U;
    return dotsPerInch >= largeConsoleFontDpi
        ? "fbcon=font:TER16x32"
        : "fbcon=font:VGA8x16";
}

/* A Linux guest boots the embedded base archive and a segment holding the
 * generic per-run /init and these parameters: the image read-only beneath an
 * overlay whose writable layer is a fresh ext4 filesystem on the overlay
 * disk, ubd1, and then the image's own init. Kernarium/README.md and
 * tools/rootfs/boot-ramdisk/README.md own the contract. */
std::string bootParameters(bool ext4Image)
{
    std::string parameters {"UML_BOOT_ROOT=/dev/ubda\nUML_BOOT_ROOT_TYPE="};
    parameters.append(ext4Image ? "ext4" : "erofs");
    parameters.append(
        "\nUML_BOOT_UPPER=ext4\n"
        "UML_BOOT_UPPER_DEVICE=/dev/ubdb\n"
        "UML_BOOT_ENTRY=/sbin/init\n");
    return parameters;
}

/* The run's initrd, a file in the application's temporary directory that the
 * kernel reads while it boots. The process runs one guest, so an initrd an
 * earlier, killed process left behind is removed first; this one is removed
 * when the run returns. */
class RunInitrd final {
public:
    /* `payload` is a padded newc segment appended after the runtime's own,
     * whose files replace the runtime's. */
    RunInitrd(bool ext4Image, std::span<const std::byte> payload)
    {
        const std::filesystem::path directory {
            NSTemporaryDirectory().fileSystemRepresentation};
        std::error_code error;
        for (const auto& entry :
             std::filesystem::directory_iterator(directory, error)) {
            if (entry.path().filename().string().starts_with(prefix)) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
        path_ = directory /
            (std::string {prefix} + std::to_string(getpid()) + ".cpio");
        /* A kernel argument ends at whitespace and treats a double quote as
         * quoting. */
        for (const unsigned char character : path_.string()) {
            if (character <= ' ' || character == '"' || character >= 0x7f) {
                throw std::runtime_error(
                    "the temporary directory cannot be named on the kernel "
                    "command line: " + path_.string());
            }
        }
        uml::boot::InitramfsArchive archive;
        archive.addFile(
            "/init",
            std::as_bytes(std::span {
                uml_boot_init_start,
                static_cast<std::size_t>(
                    uml_boot_init_end - uml_boot_init_start)}),
            0755,
            0);
        const std::string parameters = bootParameters(ext4Image);
        archive.addFile(
            "/boot.env",
            std::as_bytes(std::span {parameters.data(), parameters.size()}),
            0644,
            0);
        auto initrd = uml::boot::composeInitrd(
            std::as_bytes(std::span {
                uml_boot_base_start,
                static_cast<std::size_t>(
                    uml_boot_base_end - uml_boot_base_start)}),
            archive.serialize());
        initrd.insert(initrd.end(), payload.begin(), payload.end());
        uml::boot::writeNewFile(path_, initrd);
    }

    ~RunInitrd()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    RunInitrd(const RunInitrd&) = delete;
    RunInitrd& operator=(const RunInitrd&) = delete;

    [[nodiscard]] std::string kernelArgument() const
    {
        return "initrd=" + path_.string();
    }

private:
    static constexpr std::string_view prefix {"kernarium-initrd-"};
    std::filesystem::path path_;
};
#endif

} // namespace

namespace {

int runtimeRun(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot)
{
    try {
        /* A pre-existing Swift worker need not inherit the initial main-thread
         * mask. Keep the launcher itself ineligible for ITIMER_REAL throughout
         * the managed run; every runtime worker it creates inherits this mask.
         */
        ScopedGuestTimerBlock guestTimerBlock;
        ActiveRuntime active;
        if (view == nullptr || boot == nullptr ||
            boot->struct_size < sizeof(kernarium_runtime_boot)) {
            std::fputs("Kernarium: invalid boot request\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const uint32_t width = boot->width;
        const uint32_t height = boot->height;
        const uint32_t dotsPerInch = boot->dots_per_inch;
        const uint32_t guestMemoryMiB = boot->guest_memory_mib;
        const uint32_t guestProcessorCount = boot->guest_processor_count;
        if (width == 0 || height == 0 ||
            width > uml::gpu::protocol::maximumEdidDimension ||
            height > uml::gpu::protocol::maximumEdidDimension ||
            dotsPerInch == 0) {
            std::fputs("Kernarium: invalid display geometry\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        if (boot->auto_resize_guest > 1U) {
            std::fputs("Kernarium: invalid display resize policy\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const NSUInteger hostProcessorCount =
            [NSProcessInfo processInfo].processorCount;
        const uint32_t maximumGuestProcessorCount = hostProcessorCount == 0
            ? 1U : static_cast<uint32_t>(hostProcessorCount);
        if (guestProcessorCount == 0 ||
            guestProcessorCount > maximumGuestProcessorCount ||
            guestMemoryMiB < 512U || guestMemoryMiB > 4096U ||
            guestMemoryMiB % 64U != 0) {
            std::fputs(
                "Kernarium: invalid guest CPU or memory configuration\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        if (boot->android_disk_path == nullptr ||
            boot->android_command_line_path == nullptr) {
            std::fputs("Kernarium: incomplete Android bundle\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const std::filesystem::path androidDisk {boot->android_disk_path};
        uml::android::validateWritableDisk(androidDisk);
        const auto androidBoot = uml::android::selectBoot(
            androidDisk,
            static_cast<uml::android::BootMode>(boot->android_boot_mode));
#else
        const char* rootfsPath = boot->rootfs_path;
        const char* overlayPath = boot->overlay_path;
        if (!usableFile(rootfsPath) || !immutableExt4Rootfs(rootfsPath) ||
            !usableFile(overlayPath)) {
            std::fputs("Kernarium: invalid rootfs or overlay path\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        /* Both are named in UBD kernel arguments, which cannot carry a path
         * with whitespace, a quote, a comma or a colon as itself. */
        if (!uml::block::usable_ubd_path(rootfsPath) ||
            !uml::block::usable_ubd_path(overlayPath)) {
            std::fprintf(
                stderr,
                "Kernarium: %s cannot be named on the kernel command line; "
                "rename it without spaces, quotes, commas or colons\n",
                uml::block::usable_ubd_path(rootfsPath) ? overlayPath
                                                        : rootfsPath);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const bool derivativeRootfs = ext4Rootfs(rootfsPath);
#endif
        (void)setvbuf(stdout, nullptr, _IONBF, 0);
        const auto bootingAt = std::chrono::steady_clock::now();
        std::printf(
            "Kernarium: booting %ux%u guest at %u DPI with mem=%uM "
            "ncpus=%u\n",
            width,
            height,
            dotsPerInch,
            guestMemoryMiB,
            guestProcessorCount);

        /* The host-visible shared-memory region belongs to the GPU backend,
         * and the video decoder fills capture buffers that live in it, so
         * both backends share one index of the ranges published there. */
        auto sharedMemoryRegions =
            std::make_shared<uml::virtio::SharedMemoryRegions>();
        /* GPU memory is charged to the application like guest RAM, so the
         * GPU may use at most half of the guest's allocation and the balloon
         * takes what it uses out of the guest. */
        const std::uint64_t guestMemoryBytes =
            static_cast<std::uint64_t>(guestMemoryMiB) << 20;
        const std::uint64_t deviceMemoryLimit = guestMemoryBytes / 2;
        uml::gpu::InProcessGpuService gpuService(
            uml::gpu::InProcessGpuOptions {
                .displayMode = uml::gpu::DisplayMode::native,
                .width = width,
                .height = height,
                .dotsPerInch = dotsPerInch,
                .nativeView = (__bridge void*)view,
                .autoResizeGuest = boot->auto_resize_guest != 0,
                .sharedMemoryRegions = sharedMemoryRegions,
                .deviceMemoryLimit = deviceMemoryLimit,
            });
        const auto balloonService = uml::balloon::makeInProcessService(
            uml::balloon::BalloonOptions {
                .guestMemoryBytes = guestMemoryBytes,
                .target = uml::balloon::hostGpuMemoryTarget(deviceMemoryLimit),
                .hostPageSize = static_cast<std::size_t>(getpagesize()),
            });
        uml::input::virtio::InProcessInputService touchscreen(
            uml::input::virtio::DeviceKind::touchscreen, width, height);
        uml::input::virtio::InProcessInputService keyboard(
            uml::input::virtio::DeviceKind::keyboard, width, height);
        const auto audioService = uml::audio::makeInProcessService();
        std::unique_ptr<uml::gpu::InProcessVhostUserService> videoService;
        if (!uml::platform::videoDecodeCapabilities().empty())
            videoService = uml::video::makeInProcessService(sharedMemoryRegions);
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        const auto batteryService = uml::battery::makeInProcessService();
#endif
        AppNetwork network;
        auto networkCallbacks = network.callbacks();
        std::vector<std::string> arguments {
            "uml-kernarium",
            std::string {"--execution-engine="} + selectedExecutionEngine(),
            "mem=" + std::to_string(guestMemoryMiB) + "M",
            "ncpus=" + std::to_string(guestProcessorCount),
            /* iOS can reclaim the application without running any more of its
             * code, and only the guest can write back its own page cache. The
             * host asks for that through UML's management console, which needs
             * magic SysRq to act rather than silently ignore the request;
             * Android's init turns SysRq off, so state it here. */
            "sysrq_always_enabled",
        };
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        ConsoleBridge consoleBridge;
        /* Android tags heap pointers, so the guest must run with the top byte
         * ignored exactly as it does on the macOS Android product. */
        arguments.emplace_back("--guest-top-byte-ignore");
        arguments.emplace_back(consoleBridge.kernelArgument());
#else
#if KERNARIUM_AUTOMATION_HARNESS
        const auto automation =
            kernarium::harness::AutomationBoot::fromEnvironment(
                consoleStdoutMutex);
        if (automation)
            std::fprintf(
                stderr,
                "Kernarium: automation boot with a %zu-byte payload and "
                "target %s\n",
                automation->payload().size(),
                automation->systemdUnit().empty()
                    ? "graphical.target"
                    : automation->systemdUnit().c_str());
        const RunInitrd initrd {
            derivativeRootfs,
            automation ? automation->payload()
                       : std::span<const std::byte> {}};
        const std::string systemdUnit =
            automation && !automation->systemdUnit().empty()
            ? automation->systemdUnit()
            : std::string {"graphical.target"};
#else
        const RunInitrd initrd {derivativeRootfs, {}};
        const std::string systemdUnit {"graphical.target"};
#endif
        ConsoleBridge consoleBridge;
        arguments.emplace_back(initrd.kernelArgument());
        arguments.emplace_back(uml::boot::missingInitArgument);
        arguments.emplace_back("console=tty0");
        arguments.emplace_back("console=ttyS0");
        arguments.emplace_back(fbconFontArgument(dotsPerInch));
        arguments.emplace_back(consoleBridge.kernelArgument());
        arguments.emplace_back("ubd1=" + std::string(overlayPath));
#endif
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            gpuService.takeConnectionName(),
            uml::gpu::protocol::virtioGpuDeviceId));
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* Android's vendor policy labels the battery's sysfs path by its
         * position among the Virtio devices
         * (android/lineage-24/overlay/sepolicy/vendor/genfs_contexts). Straight
         * after the GPU it is always virtio-uml.1/virtio1, whatever optional
         * devices follow. */
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            batteryService->takeConnectionName(),
            uml::battery::protocol::deviceId));
#endif
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            balloonService->takeConnectionName(),
            uml::balloon::protocol::deviceId));
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            touchscreen.takeConnectionName(),
            uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            keyboard.takeConnectionName(),
            uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            audioService->takeConnectionName(),
            uml::audio::protocol::deviceId));
        if (videoService) {
            arguments.emplace_back(uml::gpu::virtioDeviceArgument(
                videoService->takeConnectionName(), uml::video::deviceId));
        }
#if KERNARIUM_AUTOMATION_HARNESS && !KERNARIUM_RUNTIME_ANDROID_GUEST
        if (automation) {
            if (auto relay = automation->relayDeviceArgument(); !relay.empty())
                arguments.emplace_back(std::move(relay));
        }
#endif
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* The bundle GPT is the directly writable persistent disk. Its selected
         * ramdisk is exposed from the same file without a standalone image. */
        arguments.emplace_back("ubd0=" + androidDisk.string());
        arguments.emplace_back("initrd=" + androidDisk.string());
        arguments.emplace_back(
            "initrd_offset=" + std::to_string(androidBoot.ramdisk.offset));
        arguments.emplace_back(
            "initrd_size=" + std::to_string(androidBoot.ramdisk.size));
        arguments.emplace_back("root=/dev/ram0");
        arguments.emplace_back("rootfstype=ramfs");
        appendKernelCommandLine(
            arguments, boot->android_command_line_path, dotsPerInch);
#else
        /* A sealed ext4 derivative is attached read-only; the per-run /init
         * mounts either kind of image beneath the overlay. */
        if (derivativeRootfs)
            arguments.emplace_back("ubd0cr=" + std::string(rootfsPath));
        arguments.emplace_back("systemd.unit=" + systemdUnit);
        arguments.emplace_back("plymouth.enable=0");
#endif
        const uml_emulator_config config {
            .abi_version = UML_EMULATOR_ABI_VERSION,
            .struct_size = sizeof(uml_emulator_config),
#if KERNARIUM_RUNTIME_ANDROID_GUEST
            .rootfs_path = nullptr,
#else
            .rootfs_path = rootfsPath,
#endif
            .kernel_image = uml_elf_start,
            .kernel_image_size = static_cast<std::size_t>(
                uml_elf_end - uml_elf_start),
            .network = &networkCallbacks,
        };
        uml_emulator* created = nullptr;
        const int createResult = uml_emulator_create(&config, &created);
        if (createResult != UML_EMULATOR_OK) {
            std::fprintf(
                stderr, "Kernarium: emulator creation failed: %d\n",
                createResult);
            return createResult;
        }
        const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>
            emulator {created, uml_emulator_destroy};
        /* The host devices, the GPU driver among them, start before the
         * guest does; the footprint log times that stretch. */
        if (FootprintReporter::enabled())
            std::printf("Kernarium: emulator created %.1f ms after booting\n",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - bootingAt).count());
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size());
        for (const auto& argument : arguments)
            pointers.push_back(argument.c_str());
        int result = 0;
        {
            ActiveConsoleRegistration consoleRegistration {consoleBridge};
            ActiveInputRegistration inputRegistration {
                touchscreen, keyboard};
            ActiveEmulatorRegistration registration {emulator.get()};
            ActiveGpuRegistration gpuRegistration {gpuService};
            const FootprintReporter footprintReporter;
            result = uml_emulator_run(
                emulator.get(),
                static_cast<int>(pointers.size()),
                pointers.data(),
                nullptr);
        }
        std::printf("Kernarium: guest exited with status %d\n", result);
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Kernarium: startup failed: %s\n", error.what());
        return -UML_EMULATOR_RUNTIME_ERROR;
    } catch (...) {
        std::fputs("Kernarium: startup failed\n", stderr);
        return -UML_EMULATOR_RUNTIME_ERROR;
    }
}

void runtimeRequestStop(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator != nullptr)
        uml_emulator_request_stop(activeEmulator);
}

/*
 * iOS can reclaim a suspended application without running anything else in it,
 * so the guest's writable images have to reach stable storage while the app is
 * still scheduled. This flushes what the guest has already handed to the host;
 * pages still dirty inside the guest are written back by the guest itself.
 */
int runtimeSync(void)
{
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator == nullptr)
        return 0;
    return uml_emulator_sync(activeEmulator);
}

/*
 * Flushing alone is not enough once the application stops being scheduled at a
 * predictable moment: the guest keeps running afterwards and dirties what was
 * just written back. Freezing it as well means the disk it leaves behind is
 * the disk that was flushed.
 */
int runtimeQuiesce(void)
{
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator == nullptr)
        return 0;
    return uml_emulator_quiesce(activeEmulator);
}

void runtimeResume(void)
{
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator != nullptr)
        uml_emulator_resume(activeEmulator);
}

void runtimeSendTouch(
    uint32_t x,
    uint32_t y,
    int pressed)
{
    namespace input = uml::input::virtio::protocol;
    const std::array events {
        input::InputEvent {
            .type = input::eventAbsolute,
            .code = input::absoluteX,
            .value = static_cast<std::int32_t>(x),
        },
        input::InputEvent {
            .type = input::eventAbsolute,
            .code = input::absoluteY,
            .value = static_cast<std::int32_t>(y),
        },
        input::InputEvent {
            .type = input::eventKey,
            .code = input::buttonTouch,
            .value = pressed != 0 ? 1 : 0,
        },
        input::InputEvent {
            .type = input::eventSynchronize,
            .code = input::synchronizeReport,
            .value = 0,
        },
    };
    std::lock_guard lock(activeInputMutex);
    if (activeTouchscreen != nullptr)
        activeTouchscreen->send(events);
}

void runtimeSendKey(uint16_t code, int pressed)
{
    namespace input = uml::input::virtio::protocol;
    const std::array events {
        input::InputEvent {
            .type = input::eventKey,
            .code = code,
            .value = pressed != 0 ? 1 : 0,
        },
        input::InputEvent {
            .type = input::eventSynchronize,
            .code = input::synchronizeReport,
            .value = 0,
        },
    };
    {
        std::lock_guard lock(activeInputMutex);
        if (activeKeyboard == nullptr)
            return;
        activeKeyboard->send(events);
    }
    /* A guest that suspended itself, as Android does once its display is off,
     * holds the key until something wakes it. Power wakes it, as on a phone.
     * The wake can wait on the guest's console, so it leaves this thread. */
    if (code != input::keyPower || pressed == 0)
        return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::lock_guard lifecycle(emulatorLifecycleMutex);
        if (activeEmulator != nullptr)
            uml_emulator_wake(activeEmulator);
    });
}

int runtimeSetGpuHold(int held)
{
    std::lock_guard lock(activeGpuMutex);
    gpuHoldRequested = held != 0;
    if (activeGpu == nullptr)
        return -1;
    if (held == 0) {
        activeGpu->releaseRequests();
        return 1;
    }
    return activeGpu->holdRequests(gpuHoldBudget) ? 1 : 0;
}

int runtimeSetBackgroundThrottle(int enabled)
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return -1;
    int changed = 0;
    for (mach_msg_type_number_t index = 0; index < count; ++index) {
        thread_extended_info_data_t info {};
        mach_msg_type_number_t infoCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(
                threads[index],
                THREAD_EXTENDED_INFO,
                reinterpret_cast<thread_info_t>(&info),
                &infoCount) == KERN_SUCCESS &&
            std::strncmp(info.pth_name, "uml-vcpu-", 9) == 0) {
            thread_background_policy_data_t policy {
                .priority = enabled != 0 ?
                    THREAD_BACKGROUND_POLICY_DARWIN_BG :
                    0,
            };
            if (thread_policy_set(
                    threads[index],
                    THREAD_BACKGROUND_POLICY,
                    reinterpret_cast<thread_policy_t>(&policy),
                    THREAD_BACKGROUND_POLICY_COUNT) == KERN_SUCCESS) {
                ++changed;
            }
        }
        (void)mach_port_deallocate(mach_task_self(), threads[index]);
    }
    (void)vm_deallocate(
        mach_task_self(),
        reinterpret_cast<vm_address_t>(threads),
        count * sizeof(thread_act_t));
    return changed;
}

int runtimeSetFrameSink(
    kernarium_runtime_frame_sink sink,
    void* context,
    std::uint32_t maximumWidth,
    std::uint32_t framesPerSecond)
{
    if (sink == nullptr) {
        uml::gpu::setFrameTap(nullptr, nullptr);
        return 0;
    }
    if (maximumWidth == 0 || framesPerSecond == 0)
        return -EINVAL;
    uml::gpu::setFrameTap(
        sink,
        context,
        {.maximumWidth = maximumWidth, .framesPerSecond = framesPerSecond});
    return 0;
}

void runtimeSetConsoleOutputCallback(
    kernarium_runtime_console_output_callback callback,
    void* context)
{
    std::lock_guard lock(consoleOutputCallbackMutex);
    consoleOutputCallback = callback;
    consoleOutputContext = context;
}

int runtimeSendConsoleInput(const void* buffer, std::size_t length)
{
    const int error = consoleInputs.send(buffer, length);
    return error == 0 ? 0 : -error;
}

const kernarium_runtime_api runtimeApi {
    .abi_version = KERNARIUM_RUNTIME_API_VERSION,
    .struct_size = sizeof(kernarium_runtime_api),
    .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
    .guest_flavor = KERNARIUM_RUNTIME_GUEST_FLAVOR,
    .run = runtimeRun,
    .request_stop = runtimeRequestStop,
    .sync = runtimeSync,
    .quiesce = runtimeQuiesce,
    .resume = runtimeResume,
    .send_touch = runtimeSendTouch,
    .send_key = runtimeSendKey,
    .set_console_output_callback = runtimeSetConsoleOutputCallback,
    .send_console_input = runtimeSendConsoleInput,
    .set_gpu_hold = runtimeSetGpuHold,
    .set_frame_sink = runtimeSetFrameSink,
    .set_background_throttle = runtimeSetBackgroundThrottle,
};

} // namespace

extern "C" const kernarium_runtime_api* kernarium_runtime_get_api_v11(void)
{
    return &runtimeApi;
}
