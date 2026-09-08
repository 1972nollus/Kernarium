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
        "{\"name\":\"VK_EXT_dynamic_rendering_unused_attachments\","
        "\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"dynamic_rendering_unused_attachments\":{"
        "\"extension\":true,\"feature\":true}";
    constexpr std::string_view enabledDevice = "\"device_create_result\":0";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    const auto capabilityCount = countOccurrences(report, enabledCapability);
    const auto deviceCount = countOccurrences(report, enabledDevice);
    require(extensionCount != 0);
    require(capabilityCount == extensionCount);
    require(deviceCount == extensionCount);

    // Reject contradictory provider reports rather than accepting an
    // extension string without the feature or a feature with no extension.
    require(report.find(
                "\"dynamic_rendering_unused_attachments\":{"
                "\"extension\":true,\"feature\":false}") ==
            std::string_view::npos);
    require(report.find(
                "\"dynamic_rendering_unused_attachments\":{"
                "\"extension\":false,\"feature\":true}") ==
            std::string_view::npos);

    std::puts(
        "KosmicKrisp unused dynamic-rendering attachment capabilities passed");
    return EXIT_SUCCESS;
}
