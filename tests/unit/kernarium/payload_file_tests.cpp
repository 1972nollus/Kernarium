// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumPayloadFile.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using kernarium::harness::readPayload;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::string_view segment {"070701000000000000000000"};

void write(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "writing a test file");
}

/* A name that could leave Documents is refused before the file system is
 * consulted, and only a padded newc segment is a payload. */
void onlyAPaddedSegmentNamedByAPlainNameIsRead(
    const std::filesystem::path& documents)
{
    for (const char* name : {"", ".", "..", "../p.cpio", "a/p.cpio", "/p"})
        requireThrows<std::invalid_argument>(
            [&] { (void)readPayload(documents, name); },
            "a name that is not a plain file name was read");
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "absent.cpio"); },
        "an absent payload was read");

    write(documents / "short.cpio", "07070");
    write(documents / "unpadded.cpio", "07070100000");
    write(documents / "other.cpio", "070702000000000000000000");
    for (const char* name : {"short.cpio", "unpadded.cpio", "other.cpio"})
        requireThrows<std::invalid_argument>(
            [&] { (void)readPayload(documents, name); },
            "a file that is not a padded newc segment was read");

    write(documents / "good.cpio", segment);
    const auto bytes = readPayload(documents, "good.cpio");
    require(bytes.size() == segment.size(), "the payload size");
    require(std::memcmp(bytes.data(), segment.data(), segment.size()) == 0,
        "the payload bytes");

    std::filesystem::create_directory(documents / "directory.cpio");
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "directory.cpio"); },
        "a directory was read as a payload");
}

void aPayloadLargerThanTheKernelCanBootIsRefused(
    const std::filesystem::path& documents)
{
    const auto path = documents / "huge.cpio";
    write(path, segment);
    std::filesystem::resize_file(
        path, kernarium::harness::maximumPayloadBytes + 4U);
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "huge.cpio"); },
        "a payload over 64 MiB was read");
    std::filesystem::resize_file(
        path, kernarium::harness::maximumPayloadBytes);
    require(readPayload(documents, "huge.cpio").size() ==
            kernarium::harness::maximumPayloadBytes,
        "a payload of exactly 64 MiB");
}

} // namespace

int main()
{
    const uml::testing::ScratchSubdirectory scratch {"kernarium-payload-XXXXXX"};
    onlyAPaddedSegmentNamedByAPlainNameIsRead(scratch.path());
    aPayloadLargerThanTheKernelCanBootIsRefused(scratch.path());
    std::puts("PASS: the runtime reads a boot payload named by a plain name");
    return EXIT_SUCCESS;
}
