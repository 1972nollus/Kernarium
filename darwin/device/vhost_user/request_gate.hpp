// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../../platform/descriptor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace uml::gpu {

/* Lets another thread stop a vhost-user server from handing requests to its
 * device, and later let them through again.
 *
 * Closing the gate does not touch the guest's rings: a request that arrives
 * while it is closed stays in its ring, unconsumed, whatever the ring's
 * enabled state does meanwhile, and runs in arrival order once the gate
 * opens. The server therefore never completes a held request on its own
 * account. A guest that suspends and resumes while the gate is closed finds
 * every request it queued still waiting. Only the device calls the server
 * makes for a request are gated: a deferred request completing, a device's
 * event source and the control channel carry on.
 *
 * The server runs one thread; hold() and release() are for any other. */
class RequestGate final {
public:
    RequestGate() : wake_("request gate wake") {}

    RequestGate(const RequestGate&) = delete;
    RequestGate& operator=(const RequestGate&) = delete;

    /* Closes the gate. When this returns true no device call is running and
     * none starts until release(). When it returns false a call was still
     * running after `budget`; the gate is closed regardless, and the call
     * ends on its own. Closing a closed gate waits again for the same
     * thing. */
    [[nodiscard]] bool hold(std::chrono::milliseconds budget) noexcept
    {
        std::unique_lock lock(mutex_);
        held_.store(true, std::memory_order_release);
        return idle_.wait_for(lock, budget, [this] { return active_ == 0; });
    }

    /* Opens the gate and wakes the server, which then takes up the requests
     * that waited. Opening an open gate does nothing. */
    void release() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (!held_.exchange(false, std::memory_order_acq_rel))
                return;
        }
        (void)wake_.notify();
    }

    [[nodiscard]] bool held() const noexcept
    {
        return held_.load(std::memory_order_acquire);
    }

    /* A device call in progress. An entry that does not own the gate means
     * it is closed, and the call must not be made. */
    class Entry final {
    public:
        Entry() = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr))
        {
        }
        Entry& operator=(Entry&& other) noexcept
        {
            if (this != &other) {
                leave();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Entry() { leave(); }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

    private:
        friend class RequestGate;
        explicit Entry(RequestGate& gate) noexcept : gate_(&gate) {}

        void leave() noexcept
        {
            if (gate_ != nullptr)
                gate_->leave();
            gate_ = nullptr;
        }

        RequestGate* gate_ {};
    };

    /* For the server. */
    [[nodiscard]] Entry enter() noexcept
    {
        std::lock_guard lock(mutex_);
        if (held_.load(std::memory_order_acquire))
            return {};
        ++active_;
        return Entry {*this};
    }

    /* A descriptor that polls readable after release(), until
     * acknowledgeRelease(). */
    [[nodiscard]] int wakeDescriptor() const noexcept
    {
        return wake_.descriptor();
    }

    void acknowledgeRelease() const noexcept { (void)wake_.drain(); }

private:
    void leave() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        idle_.notify_all();
    }

    uml::platform::WakeSocket wake_;
    std::mutex mutex_;
    std::condition_variable idle_;
    std::atomic_bool held_ {};
    unsigned active_ {};
};

} // namespace uml::gpu
