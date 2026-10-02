// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumHarness.hpp"

#include "console/in_process_service.hpp"
#include "console/protocol.hpp"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace kernarium::harness {

namespace {

/* The kernel copies the whole initrd into boot memory before unpacking it,
 * and an archive larger than that memory panics the guest. */
constexpr std::uintmax_t maximumPayloadBytes = 64U << 20;

std::filesystem::path documentsDirectory()
{
    NSArray<NSString*>* directories = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* directory = directories.firstObject;
    if (directory == nil)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD: no Documents directory");
    return std::filesystem::path {directory.fileSystemRepresentation};
}

std::vector<std::byte> readPayload(std::string_view name)
{
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string_view::npos) {
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD must name a file in Documents");
    }
    const auto path = documentsDirectory() / std::string {name};
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD is not a regular file: " + path.string());
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumPayloadBytes)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD exceeds 64 MiB: " + path.string());
    std::vector<std::byte> contents(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(contents.data()),
            static_cast<std::streamsize>(contents.size())))
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD cannot be read: " + path.string());
    /* A newc segment the kernel can read after the runtime's own, which
     * uml-initramfs pads to four bytes. */
    constexpr std::string_view magic {"070701"};
    if (contents.size() < magic.size() || contents.size() % 4U != 0U ||
        std::memcmp(contents.data(), magic.data(), magic.size()) != 0)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD is not a padded newc segment: " +
            path.string());
    return contents;
}

bool validSystemdTarget(std::string_view unit) noexcept
{
    constexpr std::string_view suffix {".target"};
    if (unit.size() <= suffix.size() || unit.size() > 128U ||
        !unit.ends_with(suffix))
        return false;
    return std::ranges::all_of(unit, [](char character) {
        return (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' ||
            character == '@' || character == ':';
    });
}

} // namespace

/* Relays the artifact frames a guest campaign writes to its virtio-serial
 * port to standard output, where the host reads the console, and
 * acknowledges each one. */
class ArtifactRelay final {
public:
    explicit ArtifactRelay(std::mutex& stdoutMutex)
        : stdoutMutex_(stdoutMutex)
    {
        int descriptors[2] {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "artifact relay socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        try {
            service_ = uml::console::makeInProcessService(
                descriptors[0],
                std::string {
                    uml::console::protocol::hostSerialPortName});
            ::close(descriptors[0]);
            descriptors[0] = -1;
            host_ = descriptors[1];
            descriptors[1] = -1;
            relay_ = std::thread([this] { relay(); });
        } catch (...) {
            if (descriptors[0] >= 0)
                ::close(descriptors[0]);
            if (descriptors[1] >= 0)
                ::close(descriptors[1]);
            if (host_ >= 0) {
                ::close(host_);
                host_ = -1;
            }
            if (service_)
                service_->stop();
            throw;
        }
    }

    ~ArtifactRelay()
    {
        if (service_)
            service_->stop();
        if (host_ >= 0)
            (void)::shutdown(host_, SHUT_RDWR);
        if (relay_.joinable())
            relay_.join();
        if (host_ >= 0)
            ::close(host_);
    }

    ArtifactRelay(const ArtifactRelay&) = delete;
    ArtifactRelay& operator=(const ArtifactRelay&) = delete;

    [[nodiscard]] std::string takeConnectionName()
    {
        return service_->takeConnectionName();
    }

private:
    /* One frame is one base64 line plus its short header. The GL driver
     * encodes 1 KiB lines and the Vulkan driver 3 KiB ones, so a bound below
     * the larger of the two rejects every Vulkan artifact past the first
     * single-chunk file. */
    static constexpr std::size_t maximumFrameBytes = 4096U;

    [[nodiscard]] static bool decimal(std::string_view text) noexcept
    {
        return !text.empty() && std::ranges::all_of(
            text, [](char character) {
                return character >= '0' && character <= '9';
            });
    }

    [[nodiscard]] static bool lowercaseHex(
        std::string_view text,
        std::size_t expectedSize) noexcept
    {
        return text.size() == expectedSize && std::ranges::all_of(
            text, [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
    }

    [[nodiscard]] static bool base64(std::string_view text) noexcept
    {
        /* One chunk is one encoded line: the GL driver writes 1 KiB lines and
         * the Vulkan driver 3 KiB ones. */
        return !text.empty() && text.size() <= 3072U &&
            std::ranges::all_of(text, [](char character) {
                return (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') ||
                    character == '+' || character == '/' || character == '=';
            });
    }

    [[nodiscard]] static bool allowedArtifact(
        std::string_view name) noexcept
    {
        for (const std::string_view allowed : {
                 "renderer.txt",
                 "selected.txt",
                 "expected.txt",
                 /* A campaign whose device cannot hold every enumerated case
                  * reports which ones it left out, and under which reason. */
                 "skipped.txt",
                 "results.qpa",
                 "summary.txt"}) {
            if (name == allowed)
                return true;
        }
        /* A campaign that checkpoints its log ships it as fixed-width
         * numbered segments, which the host reassembles in order. */
        constexpr std::string_view segmentPrefix = "results.qpa.";
        constexpr std::size_t segmentDigits = 4U;
        if (name.size() != segmentPrefix.size() + segmentDigits ||
            name.substr(0, segmentPrefix.size()) != segmentPrefix) {
            return false;
        }
        return decimal(name.substr(segmentPrefix.size()));
    }

    [[nodiscard]] static std::vector<std::string_view> split(
        std::string_view frame)
    {
        std::vector<std::string_view> fields;
        while (true) {
            const auto separator = frame.find('|');
            fields.push_back(frame.substr(0, separator));
            if (separator == std::string_view::npos)
                break;
            frame.remove_prefix(separator + 1U);
        }
        return fields;
    }

    [[nodiscard]] static std::string acknowledgement(
        std::string_view frame)
    {
        const auto fields = split(frame);
        if (fields.size() == 5U &&
            fields[0] == "UML_CTS_ARTIFACT_BEGIN" &&
            allowedArtifact(fields[1]) &&
            fields[2] == "gzip+base64" && decimal(fields[3]) &&
            lowercaseHex(fields[4], 64U)) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|begin\n";
        }
        if (fields.size() == 4U &&
            fields[0] == "UML_CTS_ARTIFACT_CHUNK" &&
            allowedArtifact(fields[1]) && decimal(fields[2]) &&
            base64(fields[3])) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|" + std::string {fields[2]} + "\n";
        }
        if (fields.size() == 3U &&
            fields[0] == "UML_CTS_ARTIFACT_END" &&
            allowedArtifact(fields[1]) && decimal(fields[2])) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|end\n";
        }
        return {};
    }

    [[nodiscard]] static bool writeAll(
        int descriptor,
        std::string_view bytes) noexcept
    {
        while (!bytes.empty()) {
            const ssize_t written = ::write(
                descriptor, bytes.data(), bytes.size());
            if (written > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(written));
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    void report(std::string_view record) noexcept
    {
        std::lock_guard outputLock(stdoutMutex_);
        (void)writeAll(STDOUT_FILENO, record);
    }

    void relay() noexcept
    {
        (void)::pthread_setname_np("uml-cts-artifacts");
        std::string pending;
        std::size_t frames = 0;
        std::size_t acknowledgements = 0;
        bool valid = true;
        char buffer[4096];
        for (;;) {
            const ssize_t received = ::recv(host_, buffer, sizeof(buffer), 0);
            if (received > 0) {
                pending.append(buffer, static_cast<std::size_t>(received));
                while (true) {
                    const auto newline = pending.find('\n');
                    if (newline == std::string::npos)
                        break;
                    std::string frame = pending.substr(0, newline);
                    pending.erase(0, newline + 1U);
                    if (!frame.empty() && frame.back() == '\r')
                        frame.pop_back();
                    const std::string ack = acknowledgement(frame);
                    if (frame.size() > maximumFrameBytes || ack.empty()) {
                        valid = false;
                        break;
                    }
                    report(frame + "\n");
                    ++frames;
                    if (!writeAll(host_, ack)) {
                        valid = false;
                        break;
                    }
                    ++acknowledgements;
                }
                if (!valid || pending.size() > maximumFrameBytes)
                    break;
                continue;
            }
            if (received < 0 && errno == EINTR)
                continue;
            if (received < 0 && errno != ECONNRESET)
                valid = false;
            break;
        }
        if (!pending.empty())
            valid = false;
        if (!valid)
            report("UML_VK_CTS_ERROR: invalid artifact relay frame\n");
        report(
            "UML_VK_CTS_RELAY|frames=" + std::to_string(frames) +
            "|acks=" + std::to_string(acknowledgements) +
            "|valid=" + std::to_string(valid ? 1U : 0U) + "\n");
    }

    std::mutex& stdoutMutex_;
    int host_ {-1};
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
    std::thread relay_;
};

AutomationBoot::AutomationBoot() = default;

AutomationBoot::~AutomationBoot() = default;

std::unique_ptr<AutomationBoot> AutomationBoot::fromEnvironment(
    std::mutex& consoleStdoutMutex)
{
    const char* payload = std::getenv("KERNARIUM_BOOT_PAYLOAD");
    const char* unit = std::getenv("KERNARIUM_SYSTEMD_UNIT");
    const bool hasPayload = payload != nullptr && payload[0] != '\0';
    const bool hasUnit = unit != nullptr && unit[0] != '\0';
    if (!hasPayload && !hasUnit)
        return nullptr;
    std::unique_ptr<AutomationBoot> boot {new AutomationBoot};
    if (hasUnit) {
        if (!validSystemdTarget(unit))
            throw std::invalid_argument(
                "KERNARIUM_SYSTEMD_UNIT must name a systemd target");
        boot->systemdUnit_ = unit;
    }
    if (hasPayload) {
        boot->payload_ = readPayload(payload);
        boot->relay_ = std::make_unique<ArtifactRelay>(consoleStdoutMutex);
    }
    return boot;
}

std::string AutomationBoot::relayDeviceArgument()
{
    if (!relay_)
        return {};
    return "virtio_uml.device=" + relay_->takeConnectionName() + ":" +
        std::to_string(uml::console::protocol::deviceId);
}

} // namespace kernarium::harness
