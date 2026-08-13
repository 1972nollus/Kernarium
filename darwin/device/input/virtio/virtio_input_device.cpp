// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_input_device.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace uml::input::virtio {
namespace {

constexpr std::size_t maximumQueuedEvents = 4096;

void configureDescriptor(int descriptor, bool output)
{
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    const int statusFlags = ::fcntl(descriptor, F_GETFL);
    if (descriptorFlags < 0 || statusFlags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0 ||
        ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) != 0 ||
        (output && ::fcntl(descriptor, F_SETNOSIGPIPE, 1) != 0)) {
        throw std::runtime_error(
            "configuring virtio-input event descriptor failed: " +
            std::string(std::strerror(errno)));
    }
}

void setBit(
    std::array<std::uint8_t, 128>& bitmap,
    std::uint16_t bit) noexcept
{
    bitmap[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

std::uint8_t bitmapSize(
    const std::array<std::uint8_t, 128>& bitmap) noexcept
{
    for (std::size_t index = bitmap.size(); index != 0; --index) {
        if (bitmap[index - 1U] != 0)
            return static_cast<std::uint8_t>(index);
    }
    return 0;
}

template<typename Value>
void copyValue(protocol::InputConfig& config, const Value& value)
{
    static_assert(sizeof(value) <= sizeof(config.value));
    std::memcpy(&config.value, &value, sizeof(value));
    config.size = static_cast<std::uint8_t>(sizeof(value));
}

void copyString(protocol::InputConfig& config, const char* string)
{
    const auto length = std::min<std::size_t>(
        std::strlen(string), config.value.string.size());
    std::memcpy(config.value.string.data(), string, length);
    config.size = static_cast<std::uint8_t>(length);
}

} // namespace

EventQueue::EventQueue()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "creating virtio-input event socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    readDescriptor_ = descriptors[0];
    writeDescriptor_ = descriptors[1];
    try {
        configureDescriptor(readDescriptor_, false);
        configureDescriptor(writeDescriptor_, true);
    } catch (...) {
        ::close(readDescriptor_);
        ::close(writeDescriptor_);
        readDescriptor_ = -1;
        writeDescriptor_ = -1;
        throw;
    }
}

EventQueue::~EventQueue()
{
    if (readDescriptor_ >= 0)
        ::close(readDescriptor_);
    if (writeDescriptor_ >= 0)
        ::close(writeDescriptor_);
}

void EventQueue::enqueue(
    std::span<const protocol::InputEvent> events) noexcept
{
    if (events.empty())
        return;
    bool wake = false;
    {
        std::lock_guard lock(mutex_);
        if (events.size() > maximumQueuedEvents - events_.size()) {
            if (!reportedOverflow_) {
                std::fputs(
                    "Kernarium: dropping input because the virtio queue is full\n",
                    stderr);
                reportedOverflow_ = true;
            }
            return;
        }
        wake = events_.empty();
        events_.insert(events_.end(), events.begin(), events.end());
    }
    if (wake) {
        constexpr std::uint8_t ready = 1;
        const auto result = ::send(
            writeDescriptor_, &ready, sizeof(ready), MSG_DONTWAIT);
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::fprintf(
                stderr,
                "Kernarium: virtio-input wake failed: %s\n",
                std::strerror(errno));
        }
    }
}

bool EventQueue::take(protocol::InputEvent& event) noexcept
{
    std::lock_guard lock(mutex_);
    if (events_.empty())
        return false;
    event = events_.front();
    events_.pop_front();
    return true;
}

int EventQueue::descriptor() const noexcept
{
    return readDescriptor_;
}

void EventQueue::drain()
{
    std::array<std::uint8_t, 64> bytes {};
    for (;;) {
        const auto result =
            ::recv(readDescriptor_, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (result > 0)
            continue;
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (result == 0)
            throw std::runtime_error("closed virtio-input event source");
        throw std::runtime_error(
            "reading virtio-input event source failed: " +
            std::string(std::strerror(errno)));
    }
}

void EventQueue::clear() noexcept
{
    std::lock_guard lock(mutex_);
    events_.clear();
}

VirtioInputDevice::VirtioInputDevice(
    DeviceKind kind,
    std::uint32_t width,
    std::uint32_t height,
    std::shared_ptr<EventQueue> events)
    : kind_(kind)
    , width_(width)
    , height_(height)
    , events_(std::move(events))
{
    if (!events_ || width == 0 || height == 0)
        throw std::invalid_argument("invalid virtio-input device options");
}

const char* VirtioInputDevice::name() const noexcept
{
    return kind_ == DeviceKind::touchscreen ?
        "Virtio touchscreen" : "Virtio keyboard";
}

std::uint64_t VirtioInputDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioInputDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioInputDevice::configuration() const
{
    const auto config = selectedConfiguration();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&config);
    return {bytes, bytes + sizeof(config)};
}

void VirtioInputDevice::writeConfiguration(
    std::uint32_t offset,
    std::uint32_t flags,
    std::span<const std::uint8_t> bytes)
{
    constexpr std::uint32_t writableFlag = 0;
    constexpr std::uint32_t migrationFlag = 1;
    if ((flags != writableFlag && flags != migrationFlag) || bytes.empty() ||
        offset > 2U || bytes.size() > 2U - offset) {
        throw std::runtime_error("invalid virtio-input config write");
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (offset + index == 0)
            select_ = bytes[index];
        else
            subselect_ = bytes[index];
    }
}

uml::virtio::VhostUserQueueResult VirtioInputDevice::processQueue(
    std::uint32_t index,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (index == protocol::eventQueue) {
        if (!request.empty())
            throw std::runtime_error("virtio-input event buffer is readable");
        protocol::InputEvent event {};
        if (!events_->take(event))
            return {};
        if (responseCapacity < sizeof(event))
            throw std::runtime_error("short virtio-input event buffer");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&event);
        if (!reportedDelivery_) {
            std::fprintf(stdout, "%s event queue active\n", name());
            reportedDelivery_ = true;
        }
        return {
            .consumed = true,
            .response = {bytes, bytes + sizeof(event)},
        };
    }
    if (index != protocol::statusQueue)
        throw std::runtime_error("invalid virtio-input queue");
    if (request.size() != sizeof(protocol::InputEvent) ||
        responseCapacity != 0) {
        throw std::runtime_error("invalid virtio-input status buffer");
    }
    return {.consumed = true, .response = {}};
}

void VirtioInputDevice::reset() noexcept
{
    select_ = protocol::configUnset;
    subselect_ = 0;
    reportedDelivery_ = false;
    events_->clear();
}

int VirtioInputDevice::eventDescriptor() const noexcept
{
    return events_->descriptor();
}

void VirtioInputDevice::drainEventSource()
{
    events_->drain();
}

protocol::InputConfig VirtioInputDevice::selectedConfiguration() const
{
    protocol::InputConfig config {};
    config.select = select_;
    config.subselect = subselect_;

    switch (select_) {
    case protocol::configIdName:
        copyString(
            config,
            kind_ == DeviceKind::touchscreen ?
                "Kernarium Touchscreen" : "Kernarium Keyboard");
        break;
    case protocol::configIdSerial:
        copyString(
            config,
            kind_ == DeviceKind::touchscreen ?
                "kernarium-touch-0" : "kernarium-keyboard-0");
        break;
    case protocol::configIdDeviceIds: {
        const protocol::InputDeviceIds ids {
            .bus = protocol::busVirtual,
            .vendor = 0x1af4,
            .product = static_cast<std::uint16_t>(
                kind_ == DeviceKind::touchscreen ? 1 : 2),
            .version = 1,
        };
        copyValue(config, ids);
        break;
    }
    case protocol::configPropertyBits:
        if (kind_ == DeviceKind::touchscreen && subselect_ == 0) {
            setBit(config.value.bitmap, protocol::propertyDirect);
            config.size = bitmapSize(config.value.bitmap);
        }
        break;
    case protocol::configEventBits:
        if (kind_ == DeviceKind::touchscreen) {
            if (subselect_ == protocol::eventKey) {
                setBit(config.value.bitmap, protocol::buttonTouch);
            } else if (subselect_ == protocol::eventAbsolute) {
                setBit(config.value.bitmap, protocol::absoluteX);
                setBit(config.value.bitmap, protocol::absoluteY);
            }
        } else if (subselect_ == protocol::eventKey) {
            for (std::uint16_t code = 1;
                 code <= protocol::keyboardKeyMaximum;
                 ++code) {
                setBit(config.value.bitmap, code);
            }
        } else if (subselect_ == protocol::eventLed) {
            setBit(config.value.bitmap, 0);
            setBit(config.value.bitmap, 1);
            setBit(config.value.bitmap, 2);
        }
        config.size = bitmapSize(config.value.bitmap);
        break;
    case protocol::configAbsoluteInfo:
        if (kind_ == DeviceKind::touchscreen &&
            (subselect_ == protocol::absoluteX ||
             subselect_ == protocol::absoluteY)) {
            const protocol::InputAbsoluteInfo info {
                .minimum = 0,
                .maximum = subselect_ == protocol::absoluteX ?
                    width_ - 1U : height_ - 1U,
                .fuzz = 0,
                .flat = 0,
                .resolution = 0,
            };
            copyValue(config, info);
        }
        break;
    default:
        break;
    }
    return config;
}

} // namespace uml::input::virtio
