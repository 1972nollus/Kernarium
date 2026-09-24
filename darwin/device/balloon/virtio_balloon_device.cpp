// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_balloon_device.hpp"

#include "host_gpu_memory.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include <sys/event.h>

namespace uml::balloon {
namespace {

constexpr std::uintptr_t timerIdentifier = 1;

std::vector<std::uint8_t> bytes(const protocol::Configuration& value)
{
    std::vector<std::uint8_t> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

uml::gpu::UniqueDescriptor makeTimer(std::chrono::milliseconds interval)
{
    uml::gpu::UniqueDescriptor queue {::kqueue()};
    if (queue.get() < 0) {
        throw std::runtime_error(
            "creating the Virtio balloon timer failed: " +
            std::string(std::strerror(errno)));
    }
    struct kevent timer {};
    EV_SET(&timer, timerIdentifier, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0,
        static_cast<std::intptr_t>(std::max<std::chrono::milliseconds::rep>(
            interval.count(), 1)),
        nullptr);
    if (::kevent(queue.get(), &timer, 1, nullptr, 0, nullptr) != 0) {
        throw std::runtime_error(
            "arming the Virtio balloon timer failed: " +
            std::string(std::strerror(errno)));
    }
    return queue;
}

class GuestRamBalloonMemory final : public BalloonMemory {
public:
    int release(std::uint64_t offset, std::uint64_t length) noexcept override
    {
        return uml::memory::release_guest_ram(offset, length);
    }

    int commit(std::uint64_t offset, std::uint64_t length) noexcept override
    {
        return uml::memory::commit_guest_ram(offset, length);
    }
};

} // namespace

std::unique_ptr<BalloonMemory> makeGuestRamBalloonMemory()
{
    return std::make_unique<GuestRamBalloonMemory>();
}

std::function<std::uint64_t()> hostGpuMemoryTarget(std::uint64_t limit)
{
    return [limit] {
        return std::min(uml::platform::hostGpuAllocatedBytes(), limit);
    };
}

VirtioBalloonDevice::VirtioBalloonDevice(BalloonOptions options,
    std::unique_ptr<BalloonMemory> memory)
    : options_(std::move(options))
    , memory_(std::move(memory))
{
    if (memory_ == nullptr)
        throw std::invalid_argument("Virtio balloon memory is required");
    if (options_.hostPageSize < protocol::pageSize ||
        options_.hostPageSize % protocol::pageSize != 0 ||
        options_.granularity == 0) {
        throw std::invalid_argument("invalid Virtio balloon geometry");
    }
    framesPerHostPage_ = options_.hostPageSize / protocol::pageSize;
    frameCount_ = options_.guestMemoryBytes / options_.hostPageSize *
        framesPerHostPage_;
    frames_.assign((frameCount_ + 63) / 64, 0);
    eventQueue_ = makeTimer(options_.interval);
    updateTarget();
    configChangePending_ = false;
}

const char* VirtioBalloonDevice::name() const noexcept
{
    return "Virtio balloon";
}

std::uint64_t VirtioBalloonDevice::features() const noexcept
{
    return protocol::featureMustTellHost;
}

std::size_t VirtioBalloonDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioBalloonDevice::configuration() const
{
    return bytes(protocol::Configuration {
        .numPages = targetPages_,
        .actual = actualPages_,
        .freePageHintCommandId = 0,
        .poisonValue = 0,
    });
}

void VirtioBalloonDevice::writeConfiguration(
    std::uint32_t offset,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    /* The driver reports how many pages it holds; nothing else is writable. */
    if (offset != protocol::actualOffset || bytes.size() != sizeof(std::uint32_t))
        return;
    std::memcpy(&actualPages_, bytes.data(), sizeof(actualPages_));
}

uml::virtio::VhostUserQueueResult VirtioBalloonDevice::processQueue(
    std::uint32_t index,
    std::uint64_t,
    std::span<const std::uint8_t> request,
    std::size_t)
{
    if (index == protocol::inflateQueue)
        inflate(request);
    else if (index == protocol::deflateQueue)
        deflate(request);
    return {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = {},
    };
}

void VirtioBalloonDevice::inflate(std::span<const std::uint8_t> frames)
{
    std::vector<std::uint64_t> released;
    for (std::size_t at = 0; at + sizeof(std::uint32_t) <= frames.size();
         at += sizeof(std::uint32_t)) {
        std::uint32_t frame;
        std::memcpy(&frame, frames.data() + at, sizeof(frame));
        if (frame >= frameCount_)
            continue;
        auto& word = frames_[frame / 64];
        const std::uint64_t bit = 1ULL << (frame % 64);
        if ((word & bit) != 0)
            continue;
        word |= bit;
        /* The host page leaves only once every frame of it is ballooned. */
        const std::uint64_t hostPage = frame / framesPerHostPage_;
        bool full = true;
        for (std::uint64_t sibling = hostPage * framesPerHostPage_;
             sibling < (hostPage + 1) * framesPerHostPage_; ++sibling) {
            if ((frames_[sibling / 64] & (1ULL << (sibling % 64))) == 0) {
                full = false;
                break;
            }
        }
        if (full)
            released.push_back(hostPage);
    }
    apply(true, released);
}

void VirtioBalloonDevice::deflate(std::span<const std::uint8_t> frames)
{
    std::vector<std::uint64_t> committed;
    for (std::size_t at = 0; at + sizeof(std::uint32_t) <= frames.size();
         at += sizeof(std::uint32_t)) {
        std::uint32_t frame;
        std::memcpy(&frame, frames.data() + at, sizeof(frame));
        if (frame >= frameCount_)
            continue;
        auto& word = frames_[frame / 64];
        const std::uint64_t bit = 1ULL << (frame % 64);
        if ((word & bit) == 0)
            continue;
        const std::uint64_t hostPage = frame / framesPerHostPage_;
        bool wasFull = true;
        for (std::uint64_t sibling = hostPage * framesPerHostPage_;
             sibling < (hostPage + 1) * framesPerHostPage_; ++sibling) {
            if ((frames_[sibling / 64] & (1ULL << (sibling % 64))) == 0) {
                wasFull = false;
                break;
            }
        }
        word &= ~bit;
        if (wasFull)
            committed.push_back(hostPage);
    }
    apply(false, committed);
}

void VirtioBalloonDevice::apply(bool release, std::vector<std::uint64_t>& hostPages)
{
    std::sort(hostPages.begin(), hostPages.end());
    hostPages.erase(std::unique(hostPages.begin(), hostPages.end()),
        hostPages.end());
    const std::uint64_t pageSize = options_.hostPageSize;
    for (std::size_t first = 0; first < hostPages.size();) {
        std::size_t last = first + 1;
        while (last < hostPages.size() &&
               hostPages[last] == hostPages[last - 1] + 1)
            ++last;
        const std::uint64_t offset = hostPages[first] * pageSize;
        const std::uint64_t length = (last - first) * pageSize;
        /* A host that cannot release or wire a page leaves the guest's view
         * correct either way; the page is only counted differently. */
        (void)(release ? memory_->release(offset, length) :
                         memory_->commit(offset, length));
        first = last;
    }
    if (release)
        releasedHostPages_ += hostPages.size();
    else
        releasedHostPages_ -= std::min<std::uint64_t>(
            releasedHostPages_, hostPages.size());
}

void VirtioBalloonDevice::reset() noexcept
{
    /* A driver that goes away takes every page back without telling the
     * host, so give them all back first. */
    std::vector<std::uint64_t> committed;
    const std::uint64_t hostPages = frameCount_ / framesPerHostPage_;
    for (std::uint64_t hostPage = 0; hostPage < hostPages; ++hostPage) {
        bool full = true;
        for (std::uint64_t frame = hostPage * framesPerHostPage_;
             frame < (hostPage + 1) * framesPerHostPage_; ++frame) {
            if ((frames_[frame / 64] & (1ULL << (frame % 64))) == 0) {
                full = false;
                break;
            }
        }
        if (full)
            committed.push_back(hostPage);
    }
    try {
        apply(false, committed);
    } catch (...) {
    }
    std::fill(frames_.begin(), frames_.end(), 0);
    releasedHostPages_ = 0;
    actualPages_ = 0;
    configChangePending_ = false;
}

int VirtioBalloonDevice::eventDescriptor() const noexcept
{
    return eventQueue_.get();
}

void VirtioBalloonDevice::drainEventSource()
{
    std::array<struct kevent, 4> events {};
    const timespec timeout {};
    int count;
    do {
        count = ::kevent(eventQueue_.get(), nullptr, 0, events.data(),
            static_cast<int>(events.size()), &timeout);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw std::runtime_error("draining the Virtio balloon timer failed");
    if (count > 0)
        updateTarget();
}

void VirtioBalloonDevice::updateTarget()
{
    const std::uint64_t granule = options_.granularity;
    std::uint64_t wanted = options_.target ? options_.target() : 0;
    wanted = std::min(wanted, options_.guestMemoryBytes);
    const std::uint64_t current =
        static_cast<std::uint64_t>(targetPages_) * protocol::pageSize;
    /* Follow the target only once it has moved a whole granule away. */
    if (wanted < current + granule && wanted + granule > current)
        return;
    const std::uint64_t pages = wanted / granule * granule / protocol::pageSize;
    const auto limited = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        pages, std::numeric_limits<std::uint32_t>::max()));
    if (limited == targetPages_)
        return;
    targetPages_ = limited;
    configChangePending_ = true;
}

bool VirtioBalloonDevice::configChangePending() const noexcept
{
    return configChangePending_;
}

void VirtioBalloonDevice::clearConfigChange() noexcept
{
    configChangePending_ = false;
}

std::uint64_t VirtioBalloonDevice::targetBytes() const noexcept
{
    return static_cast<std::uint64_t>(targetPages_) * protocol::pageSize;
}

std::uint64_t VirtioBalloonDevice::actualBytes() const noexcept
{
    return static_cast<std::uint64_t>(actualPages_) * protocol::pageSize;
}

std::uint64_t VirtioBalloonDevice::releasedBytes() const noexcept
{
    return releasedHostPages_ * options_.hostPageSize;
}

} // namespace uml::balloon
