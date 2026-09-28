// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "playback_sink.hpp"
#include "vhost_user_device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace uml::audio {

class VirtioSoundDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit VirtioSoundDevice(std::unique_ptr<PlaybackSink> sink);
    ~VirtioSoundDevice() override;

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override;

private:
    enum class StreamState : std::uint8_t {
        unconfigured,
        configured,
        prepared,
        running,
        stopped,
        released,
    };

    [[nodiscard]] uml::virtio::VhostUserQueueResult processControl(
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity);
    [[nodiscard]] uml::virtio::VhostUserQueueResult processTransmit(
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity);

    std::unique_ptr<PlaybackSink> sink_;
    StreamState state_ {StreamState::unconfigured};
    PlaybackParameters parameters_ {};
    // Transfers handed to the sink whose completions have not been taken.
    // Their buffers belong to the sink's current configuration, so new
    // parameters are refused until they come back, as Linux itself does.
    std::size_t deferredTransfers_ {};
};

} // namespace uml::audio
