// SPDX-License-Identifier: GPL-2.0-or-later

#include "connected_socket_channel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace uml::console {
namespace {

/* The device asks for as much as the guest's input buffers hold, which the
 * guest chooses: one descriptor may cover all of its RAM, and a chain may
 * repeat it. A read never allocates more than this, and recv() hands back no
 * more than is queued anyway. */
constexpr std::size_t maximumReadBytes = 64U * 1024U;

} // namespace

ConnectedSocketChannel::ConnectedSocketChannel(int descriptor)
    : descriptor_(descriptor)
{
    if (descriptor_ < 0)
        throw std::invalid_argument("connected console descriptor is invalid");
    const int descriptorFlags = ::fcntl(descriptor_, F_GETFD);
    const int statusFlags = ::fcntl(descriptor_, F_GETFL);
    if (descriptorFlags < 0 ||
        statusFlags < 0 ||
        ::fcntl(descriptor_, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0 ||
        ::fcntl(descriptor_, F_SETFL, statusFlags | O_NONBLOCK) != 0 ||
        ::fcntl(descriptor_, F_SETNOSIGPIPE, 1) != 0) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error(
            "configuring connected console descriptor failed: " +
            std::string(std::strerror(error)));
    }
}

ConnectedSocketChannel::~ConnectedSocketChannel()
{
    if (descriptor_ >= 0)
        ::close(descriptor_);
}

std::vector<std::uint8_t> ConnectedSocketChannel::read(
    std::size_t maximumBytes)
{
    std::vector<std::uint8_t> result(std::min(maximumBytes, maximumReadBytes));
    for (;;) {
        const ssize_t received = ::recv(
            descriptor_, result.data(), result.size(), MSG_DONTWAIT);
        if (received > 0) {
            result.resize(static_cast<std::size_t>(received));
            return result;
        }
        if (received < 0 && errno == EINTR)
            continue;
        result.clear();
        return result;
    }
}

void ConnectedSocketChannel::write(
    std::span<const std::uint8_t> bytes) noexcept
{
    while (!bytes.empty()) {
        const ssize_t sent = ::send(
            descriptor_, bytes.data(), bytes.size(), 0);
        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return;
    }
}

int ConnectedSocketChannel::eventDescriptor() const noexcept
{
    return descriptor_;
}

void ConnectedSocketChannel::drainEventSource()
{
}

void ConnectedSocketChannel::reset() noexcept
{
}

} // namespace uml::console
