// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <utility>
#include <unistd.h>

namespace uml::gpu {

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

} // namespace uml::gpu
