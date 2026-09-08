// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_probe.hpp"

#include <charconv>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <source_location>
#include <sstream>
#include <string_view>
#include <system_error>

namespace {

void require(
    bool condition,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "require failed at %s:%u\n",
        location.file_name(),
        location.line());
    std::abort();
}

std::size_t countOccurrences(
    std::string_view text,
    std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

uint32_t parseUnsigned(std::string_view text, std::size_t position)
{
    uint32_t value = 0;
    const char *first = text.data() + position;
    const char *last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    require(result.ec == std::errc{});
    return value;
}

} // namespace

int main()
{
    std::ostringstream output;
    require(uml::gpu::run_kosmickrisp_capability_probe(output) == 0);

    const auto report = output.str();
    constexpr std::string_view extensionProperty =
        "{\"name\":\"VK_KHR_device_address_commands\","
        "\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"device_address_commands\":{\"extension\":true,"
        "\"feature\":true,\"buffer_device_address\":true}";
    constexpr std::string_view executionPrefix =
        "\"device_address_commands_execution\":{\"entrypoint_count\":";
    constexpr std::string_view executionResult =
        "\"state_commands_recorded\":true,"
        "\"transfer_verified\":true}";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    require(extensionCount != 0);
    require(countOccurrences(report, enabledCapability) == extensionCount);
    require(countOccurrences(report, executionPrefix) == extensionCount);
    require(countOccurrences(report, executionResult) == extensionCount);
    require(countOccurrences(report, "\"device_create_result\":0") ==
            extensionCount);

    std::size_t position = 0;
    for (std::size_t index = 0; index < extensionCount; ++index) {
        position = report.find(executionPrefix, position);
        require(position != std::string_view::npos);
        position += executionPrefix.size();
        require(parseUnsigned(report, position) >= 11);
    }

    require(report.find(
                "\"device_address_commands\":{\"extension\":true,"
                "\"feature\":false") == std::string_view::npos);
    require(report.find(
                "\"device_address_commands\":{\"extension\":false,"
                "\"feature\":true") == std::string_view::npos);
    require(report.find(
                "\"device_address_commands\":{\"extension\":true,"
                "\"feature\":true,\"buffer_device_address\":false}") ==
            std::string_view::npos);

    std::puts(
        "KosmicKrisp device-address command capabilities and execution passed");
    return EXIT_SUCCESS;
}
