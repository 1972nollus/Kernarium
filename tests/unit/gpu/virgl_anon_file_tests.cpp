// SPDX-License-Identifier: GPL-2.0-or-later

/* The segments that virglrenderer shares with the host GPU stack (a texture
 * export, a Venus shared-memory blob) are POSIX shared memory on macOS, which
 * counts in the process's memory footprint, and unlinked container files on
 * iOS, which do not. VIRGL_ANON_FILE_UNLINKED makes a macOS run take the
 * file, so a measurement run reads what an iOS run would. Unset, empty or "0"
 * keeps shared memory. The segment must be a shareable mapping either way. */

#include "require.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int os_create_anonymous_file(off_t size, const char* debug_name);

namespace {

using uml::testing::require;

constexpr off_t segmentBytes = 4 << 20;
constexpr const char* variable = "VIRGL_ANON_FILE_UNLINKED";

/* A segment created under `value` of the variable, or the variable unset
 * where `value` is null. */
int createSegment(const char* value)
{
    if (value == nullptr)
        require(unsetenv(variable) == 0, "the variable could not be unset");
    else
        require(setenv(variable, value, 1) == 0, "the variable was not set");
    const int fd = os_create_anonymous_file(segmentBytes, "unit-test");
    require(fd >= 0, "no segment was created");
    return fd;
}

/* Two writable shared mappings of the segment see each other's stores, so the
 * segment can carry a texture or a blob between the two sides that map it. */
void requireShareable(int fd)
{
    void* first = mmap(
        nullptr, segmentBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* second = mmap(
        nullptr, segmentBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    require(first != MAP_FAILED && second != MAP_FAILED,
        "the segment could not be mapped shared");
    std::memset(first, 0x5a, static_cast<std::size_t>(segmentBytes));
    require(static_cast<unsigned char*>(second)[segmentBytes - 1] == 0x5a,
        "a store through one mapping was not seen through the other");
    require(munmap(first, segmentBytes) == 0 &&
            munmap(second, segmentBytes) == 0,
        "the mappings could not be released");
}

/* An unlinked container file: a regular file with no name left. */
void requireContainerFile(const char* value)
{
    const int fd = createSegment(value);
    struct stat info {};
    require(fstat(fd, &info) == 0, "the segment could not be examined");
    require(S_ISREG(info.st_mode), "the segment is not a file");
    require(info.st_nlink == 0, "the segment still has a name");
    require(info.st_size == segmentBytes, "the segment has the wrong size");
    requireShareable(fd);
    require(close(fd) == 0, "the segment could not be closed");
}

/* POSIX shared memory: not a regular file, which is what makes its pages
 * anonymous and part of the footprint. */
void requireSharedMemory(const char* value)
{
    const int fd = createSegment(value);
    struct stat info {};
    require(fstat(fd, &info) == 0, "the segment could not be examined");
    require(!S_ISREG(info.st_mode),
        "the segment is a file where shared memory was expected");
    requireShareable(fd);
    require(close(fd) == 0, "the segment could not be closed");
}

} // namespace

int main()
{
    /* The default is unchanged. */
    requireSharedMemory(nullptr);
    requireSharedMemory("");
    requireSharedMemory("0");

    requireContainerFile("1");
    requireContainerFile("yes");

    /* Back to the default after a run that asked for files. */
    requireSharedMemory(nullptr);
    return EXIT_SUCCESS;
}
