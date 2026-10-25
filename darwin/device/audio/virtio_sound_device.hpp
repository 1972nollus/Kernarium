// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "period_clock.hpp"
#include "playback_sink.hpp"
#include "vhost_user_device.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace uml::audio {

class VirtioSoundDevice final : public uml::virtio::VhostUserDevice {
public:
    /* `clock` is the one the sink signals its completions on. */
    VirtioSoundDevice(
        std::unique_ptr<PlaybackSink> sink,
        std::shared_ptr<PeriodClock> clock);
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

    // A period the sink could not play, completed on the period clock.
    struct PacedTransfer final {
        std::uint64_t requestId {};
        // Meaningful while the stream runs; a stopped stream holds its
        // periods and times them again when it starts.
        ClockTime due {};
        bool timed {};
    };

    struct ReadyCompletion final {
        std::uint64_t requestId {};
        std::uint32_t latencyBytes {};
    };

    [[nodiscard]] std::chrono::nanoseconds playbackPeriod() const noexcept;
    void pace(std::uint64_t requestId);
    void timePacedTransfers();
    void releasePacedTransfers(ClockTime now);
    void rearmClock();

    std::unique_ptr<PlaybackSink> sink_;
    std::shared_ptr<PeriodClock> clock_;
    StreamState state_ {StreamState::unconfigured};
    PlaybackParameters parameters_ {};
    // Transfers handed to the sink, or held for pacing, whose completions
    // have not been taken. Their buffers belong to the stream's current
    // configuration, so new parameters are refused until they come back, as
    // Linux itself does.
    std::size_t deferredTransfers_ {};
    // While the sink cannot play, the device completes playback on the
    // period clock and drops the data. The guest's playback pointer follows
    // its completions, so completing at once would drain it in no time and
    // underrun every write.
    std::deque<PacedTransfer> paced_;
    std::deque<ReadyCompletion> ready_;
    // When the paced data queued so far would finish playing.
    ClockTime playHead_ {};
    // The deadline the clock is armed for, cleared when it signals.
    std::optional<ClockTime> armed_;
};

} // namespace uml::audio
