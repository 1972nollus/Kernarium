// SPDX-License-Identifier: GPL-2.0-or-later

#include "vhost_user_server.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::atomic_bool stopRequested;

void stop(int)
{
    stopRequested.store(true, std::memory_order_release);
}

template<typename Integer>
Integer parseInteger(std::string_view value, std::string_view option)
{
    Integer result {};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc {} || end != value.data() + value.size())
        throw std::invalid_argument(
            "invalid value for " + std::string(option));
    return result;
}

void usage(const char* executable)
{
    std::cout
        << "Usage: " << executable << " [options]\n"
        << "  --socket PATH         vhost-user Unix socket (required)\n"
        << "  --vnc-bind ADDRESS    VNC IPv4 bind address (default: 127.0.0.1)\n"
        << "  --vnc-port PORT       VNC port (default: 5900)\n"
        << "  --vnc-password PASS   require a VNC password (1-8 bytes)\n"
        << "  --width PIXELS        scanout width (default: 1280)\n"
        << "  --height PIXELS       scanout height (default: 720)\n"
        << "  --self-test-pattern   show deterministic VNC test pixels\n";
}

} // namespace

int main(int argumentCount, char** arguments)
{
    try {
        std::filesystem::path socketPath;
        std::string vncBindAddress = "127.0.0.1";
        std::uint16_t vncPort = 5900;
        std::string vncPassword;
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool selfTestPattern = false;

        for (int index = 1; index < argumentCount; ++index) {
            const std::string_view argument(arguments[index]);
            const auto value = [&](std::string_view option) {
                if (++index >= argumentCount)
                    throw std::invalid_argument(
                        std::string(option) + " requires a value");
                return std::string_view(arguments[index]);
            };
            if (argument == "--socket")
                socketPath = value(argument);
            else if (argument == "--vnc-bind")
                vncBindAddress = value(argument);
            else if (argument == "--vnc-port")
                vncPort = parseInteger<std::uint16_t>(value(argument), argument);
            else if (argument == "--vnc-password") {
                vncPassword = value(argument);
                if (vncPassword.empty() || vncPassword.size() > 8)
                    throw std::invalid_argument(
                        "VNC password must contain between 1 and 8 bytes");
            }
            else if (argument == "--width")
                width = parseInteger<std::uint32_t>(value(argument), argument);
            else if (argument == "--height")
                height = parseInteger<std::uint32_t>(value(argument), argument);
            else if (argument == "--self-test-pattern")
                selfTestPattern = true;
            else if (argument == "--help" || argument == "-h") {
                usage(arguments[0]);
                return 0;
            } else {
                throw std::invalid_argument(
                    "unknown option: " + std::string(argument));
            }
        }
        if (socketPath.empty())
            throw std::invalid_argument("--socket is required");

        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGHUP, stop);

        uml::gpu::VhostUserServer server(
            std::move(socketPath),
            std::move(vncBindAddress),
            vncPort,
            std::move(vncPassword),
            width,
            height,
            selfTestPattern);
        server.run(stopRequested);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "uml-virtio-gpu: " << error.what() << '\n';
        return 1;
    }
}
