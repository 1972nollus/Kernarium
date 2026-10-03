// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>

namespace uml::platform {

/* Owns one open file descriptor, closing it on reset and destruction. */
class UniqueDescriptor final {
public:
    UniqueDescriptor() = default;
    explicit UniqueDescriptor(int descriptor) noexcept
        : descriptor_(descriptor)
    {
    }
    ~UniqueDescriptor() { reset(); }

    UniqueDescriptor(const UniqueDescriptor&) = delete;
    UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;

    UniqueDescriptor(UniqueDescriptor&& other) noexcept
        : descriptor_(other.release())
    {
    }

    UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept
    {
        if (this != &other) {
            reset();
            descriptor_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    [[nodiscard]] int release() noexcept
    {
        return std::exchange(descriptor_, -1);
    }

    void reset(int descriptor = -1) noexcept
    {
        if (descriptor_ >= 0)
            (void)::close(descriptor_);
        descriptor_ = descriptor;
    }

private:
    int descriptor_ {-1};
};

/* The flags configureDescriptor() adds to a descriptor. */
struct DescriptorFlags final {
    bool closeOnExec {};
    bool nonblocking {};
    bool noSigpipe {};
};

/* Adds `flags` to `descriptor`. Both flag words it changes are read before
 * either is written, and F_SETNOSIGPIPE comes last. Returns false with errno
 * set if a call fails, leaving any flag already written in place. */
[[nodiscard]] inline bool configureDescriptor(
    int descriptor,
    DescriptorFlags flags)
{
    const int descriptorFlags =
        flags.closeOnExec ? ::fcntl(descriptor, F_GETFD) : 0;
    const int statusFlags =
        flags.nonblocking ? ::fcntl(descriptor, F_GETFL) : 0;
    return descriptorFlags >= 0 && statusFlags >= 0 &&
        (!flags.closeOnExec ||
         ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0) &&
        (!flags.nonblocking ||
         ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) == 0) &&
        (!flags.noSigpipe || ::fcntl(descriptor, F_SETNOSIGPIPE, 1) == 0);
}

/* configureDescriptor(), throwing "configuring <what> failed: <error>". */
inline void configureDescriptorOrThrow(
    int descriptor,
    DescriptorFlags flags,
    const char* what)
{
    if (configureDescriptor(descriptor, flags))
        return;
    const int error = errno;
    throw std::runtime_error(
        std::string("configuring ") + what + " failed: " +
        std::strerror(error));
}

} // namespace uml::platform
