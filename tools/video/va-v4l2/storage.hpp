// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace uml::va {

// Which kind of buffer backs a surface. A surface always has a CPU mapping;
// the difference is whether the file descriptor it exports is something a
// graphics stack can import.
enum class StorageKind {
    memory,   // memfd: a CPU buffer with a descriptor, for clients that read pixels
    gbm,      // a linear GBM buffer object, importable by the guest graphics stack
    imported, // a descriptor handed to the driver by a client
};

// One buffer object holding every plane of one surface: a single allocation, so
// that it can be exported as one DRM PRIME object with per-plane offsets, which
// is what Chromium's VA-API wrapper requires.
class PixelBuffer {
public:
    static std::unique_ptr<PixelBuffer> allocate(std::uint32_t fourcc, unsigned width, unsigned height,
                                                 std::string& error);
    static std::unique_ptr<PixelBuffer> adopt(int fd, std::uint32_t fourcc, unsigned width, unsigned height,
                                              unsigned planes, const std::uint32_t* pitch,
                                              const std::uint32_t* offset, std::uint64_t modifier,
                                              std::size_t size, std::string& error);
    ~PixelBuffer();

    PixelBuffer(const PixelBuffer&) = delete;
    PixelBuffer& operator=(const PixelBuffer&) = delete;

    // A descriptor the caller owns and must close, or -1 when this buffer
    // cannot be shared.
    int exportDescriptor() const;
    // The CPU mapping, created on demand. Null when the buffer cannot be mapped.
    std::uint8_t* map();
    const std::uint8_t* map() const { return const_cast<PixelBuffer*>(this)->map(); }

    StorageKind kind() const noexcept { return kind_; }
    std::uint32_t fourcc() const noexcept { return fourcc_; }
    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    unsigned planes() const noexcept { return planes_; }
    std::size_t pitch(unsigned plane) const noexcept { return pitch_[plane]; }
    std::size_t offset(unsigned plane) const noexcept { return offset_[plane]; }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t modifier() const noexcept { return modifier_; }
    // The identity of the underlying object, so that a descriptor handed back
    // to the driver can be recognised as one of its own allocations.
    std::uint64_t identity() const noexcept { return identity_; }

private:
    PixelBuffer() = default;

    StorageKind kind_ {StorageKind::memory};
    std::uint32_t fourcc_ {};
    unsigned width_ {};
    unsigned height_ {};
    unsigned planes_ {};
    std::size_t pitch_[4] {};
    std::size_t offset_[4] {};
    std::size_t size_ {};
    std::uint64_t modifier_ {};
    std::uint64_t identity_ {};
    int fd_ {-1};
    std::uint8_t* map_ {};
    void* bo_ {};
};

// Chooses the backing store once per process: UML_VA_V4L2_SURFACES is
// memory|gbm|auto, and auto prefers a GBM buffer the graphics stack can import
// and falls back to plain memory.
void selectStorage(const char* request);
StorageKind selectedStorage() noexcept;
const char* storageName(StorageKind kind) noexcept;

// The plane layout this driver uses for a format, matching what the Virtio
// video device produces: tightly packed rows.
bool planeLayout(std::uint32_t fourcc, unsigned width, unsigned height, unsigned& planes,
                 std::size_t* pitch, std::size_t* offset, std::size_t& size) noexcept;

}
