// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_resize_notifier.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace uml::gpu {
namespace {

void configureDescriptor(int descriptor, bool output)
{
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    const int statusFlags = ::fcntl(descriptor, F_GETFL);
    if (descriptorFlags < 0 || statusFlags < 0 ||
        ::fcntl(
            descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0 ||
        ::fcntl(
            descriptor, F_SETFL, statusFlags | O_NONBLOCK) != 0 ||
        (output && ::fcntl(descriptor, F_SETNOSIGPIPE, 1) != 0)) {
        throw std::runtime_error(
            "configuring display resize descriptor failed: " +
            std::string(std::strerror(errno)));
    }
}

} // namespace

DisplayResizeNotifier::DisplayResizeNotifier(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension)
    : maximumDimension_(std::clamp(
          maximumDimension,
          minimumDimension,
          protocol::maximumDisplayDimension))
    , requested_(packDisplaySize({
          clampDimension(width, maximumDimension_),
          clampDimension(height, maximumDimension_),
      }))
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "creating display resize socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    read_ = UniqueDescriptor {descriptors[0]};
    write_ = UniqueDescriptor {descriptors[1]};
    configureDescriptor(read_.get(), false);
    configureDescriptor(write_.get(), true);
}

std::uint32_t DisplayResizeNotifier::clampDimension(
    std::uint32_t dimension,
    std::uint32_t maximumDimension) noexcept
{
    const auto boundedMaximum = std::clamp(
        maximumDimension,
        minimumDimension,
        protocol::maximumDisplayDimension);
    return std::clamp(dimension, minimumDimension, boundedMaximum);
}

int DisplayResizeNotifier::descriptor() const noexcept
{
    return read_.get();
}

std::uint32_t DisplayResizeNotifier::maximumDimension() const noexcept
{
    return maximumDimension_;
}

void DisplayResizeNotifier::request(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    publish(width, height, false);
}

void DisplayResizeNotifier::requestCommitted(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    publish(width, height, true);
}

void DisplayResizeNotifier::publish(
    std::uint32_t width,
    std::uint32_t height,
    bool ensureNotification) noexcept
{
    if (width == 0 || height == 0)
        return;
    width = clampDimension(width, maximumDimension_);
    height = clampDimension(height, maximumDimension_);
    const auto encoded = packDisplaySize({width, height});
    if (requested_.exchange(encoded, std::memory_order_acq_rel) == encoded &&
        !ensureNotification) {
        return;
    }
    constexpr std::uint8_t ready = 1;
    ssize_t result;
    do {
        result = ::send(
            write_.get(), &ready, sizeof(ready), MSG_DONTWAIT);
    } while (result < 0 && errno == EINTR);
}

DisplaySize DisplayResizeNotifier::drain() noexcept
{
    std::array<std::uint8_t, 64> bytes {};
    while (::recv(
               read_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT) > 0) {
    }
    return unpackDisplaySize(requested_.load(std::memory_order_acquire));
}

} // namespace uml::gpu
