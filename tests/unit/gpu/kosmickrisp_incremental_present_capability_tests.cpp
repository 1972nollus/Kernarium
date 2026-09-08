// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <sstream>
#include <string_view>

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

} // namespace

int main()
{
    std::ostringstream output;
    require(uml::gpu::run_kosmickrisp_capability_probe(output) == 0);

    const auto report = output.str();
    constexpr std::string_view extensionProperty =
        "{\"name\":\"VK_KHR_incremental_present\",\"spec_version\":2}";
    constexpr std::string_view enabledCapability =
        "\"incremental_present\":{\"extension\":true,"
        "\"swapchain_extension\":true}";
    constexpr std::string_view enabledDevice = "\"device_create_result\":0";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    const auto capabilityCount = countOccurrences(report, enabledCapability);
    const auto deviceCount = countOccurrences(report, enabledDevice);
    require(extensionCount != 0);
    require(capabilityCount == extensionCount);
    require(deviceCount == extensionCount);

    // The device extension depends on VK_KHR_swapchain. Reject a provider
    // report that advertises incremental present without its prerequisite.
    require(report.find(
                "\"incremental_present\":{\"extension\":true,"
                "\"swapchain_extension\":false}") ==
            std::string_view::npos);

    std::puts("KosmicKrisp incremental-present capabilities passed");
    return EXIT_SUCCESS;
}
