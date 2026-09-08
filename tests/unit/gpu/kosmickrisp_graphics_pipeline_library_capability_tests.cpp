// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_probe.hpp"

#include <cstdint>
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
    std::fprintf(stderr, "require failed at %s:%u\n", location.file_name(),
                 location.line());
    std::abort();
}

std::size_t countOccurrences(std::string_view text, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

uint64_t unsignedField(std::string_view report, std::string_view field)
{
    const auto position = report.find(field);
    require(position != std::string_view::npos);
    const auto first = position + field.size();
    auto last = first;
    while (last < report.size() && report[last] >= '0' &&
           report[last] <= '9') {
        ++last;
    }
    require(last != first);
    uint64_t value = 0;
    for (auto index = first; index < last; ++index) {
        value = value * 10U + static_cast<uint64_t>(report[index] - '0');
    }
    return value;
}

} // namespace

int main()
{
    std::ostringstream output;
    require(uml::gpu::run_kosmickrisp_capability_probe(output) == 0);

    const auto report = output.str();
    constexpr std::string_view pipelineLibrary =
        "{\"name\":\"VK_KHR_pipeline_library\",\"spec_version\":1}";
    constexpr std::string_view graphicsPipelineLibrary =
        "{\"name\":\"VK_EXT_graphics_pipeline_library\","
        "\"spec_version\":1}";
    constexpr std::string_view capability =
        "\"graphics_pipeline_library\":{"
        "\"pipeline_library_extension\":true,"
        "\"graphics_pipeline_library_extension\":true,"
        "\"feature\":true,\"fast_linking\":false,"
        "\"independent_interpolation_decoration\":true}";
    constexpr std::string_view successfulProbe =
        "\"graphics_pipeline_library_probe\":{"
        "\"subset_library_count\":4,"
        "\"compile_required_result\":1000297000,"
        "\"first_link_result\":0,\"cached_link_result\":0,"
        "\"mixed_link_result\":0,"
        "\"combined_link_result\":0,"
        "\"lto_link_result\":0,\"bind_result\":0";

    const auto khrCount = countOccurrences(report, pipelineLibrary);
    const auto extCount = countOccurrences(report, graphicsPipelineLibrary);
    require(khrCount != 0);
    require(extCount == khrCount);
    require(countOccurrences(report, capability) == extCount);
    require(countOccurrences(report, successfulProbe) == extCount);

    // Reject extension/feature mismatches and a fast-link claim: the measured
    // cold native Metal link is intentionally reported as non-fast.
    require(report.find(
                "\"graphics_pipeline_library_extension\":true,"
                "\"feature\":false") == std::string_view::npos);
    require(report.find("\"fast_linking\":true") ==
            std::string_view::npos);

    const auto firstLink = unsignedField(report, "\"first_link_ns\":");
    const auto cachedLink = unsignedField(report, "\"cached_link_ns\":");
    const auto mixedLink = unsignedField(report, "\"mixed_link_ns\":");
    const auto ltoLink = unsignedField(report, "\"lto_link_ns\":");
    const auto combinedLink =
        unsignedField(report, "\"combined_link_ns\":");
    const auto bindRecord = unsignedField(report, "\"bind_record_ns\":");
    require(firstLink != 0);
    require(cachedLink != 0);
    require(mixedLink != 0);
    require(ltoLink != 0);
    require(combinedLink != 0);
    require(bindRecord != 0);
    require(cachedLink < firstLink);
    require(bindRecord < firstLink);

    std::printf(
        "KosmicKrisp GPL linked four subsets, retained LTO state, and bound "
        "after library destruction (cold=%llu ns cached=%llu ns bind=%llu "
        "ns)\n",
        static_cast<unsigned long long>(firstLink),
        static_cast<unsigned long long>(cachedLink),
        static_cast<unsigned long long>(bindRecord));
    return EXIT_SUCCESS;
}
