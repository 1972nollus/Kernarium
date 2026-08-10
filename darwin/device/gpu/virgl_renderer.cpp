// SPDX-License-Identifier: GPL-2.0-or-later

#include "virgl_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdexcept>
#include <string>
#include <sys/uio.h>

extern "C" {
#include <virgl_hw.h>
#include <virglrenderer.h>

typedef void (*epoxy_resolver_stub_t)(void);
typedef epoxy_resolver_stub_t
    (*epoxy_resolver_failure_handler_t)(const char* name);
epoxy_resolver_failure_handler_t epoxy_set_resolver_failure_handler(
    epoxy_resolver_failure_handler_t handler);
}

namespace uml::gpu {
namespace {

// A renderer fence normally retires in milliseconds. A finite wait prevents a
// malformed guest command or a lost emulator callback from hanging the whole
// in-process emulator indefinitely.
constexpr auto maximumFenceWait = std::chrono::seconds(60);

std::atomic_bool missingGlEntryPoint;
std::atomic_bool rendererInstanceActive;

void unavailableGlEntryPoint()
{
}

epoxy_resolver_stub_t handleMissingGlEntryPoint(const char* name) noexcept
{
    missingGlEntryPoint.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "Virgl rejected unavailable host GL entry point: %s\n",
        name == nullptr ? "(unknown)" : name);
    /*
     * Libepoxy normally aborts here. Its supported failure-handler contract
     * allows a fallback stub; Virgl's public operation wrapper then turns the
     * recorded resolution failure into EIO for the guest context.
     */
    return &unavailableGlEntryPoint;
}

int checkGlDispatchResult(int result)
{
    if (!missingGlEntryPoint.exchange(false, std::memory_order_acq_rel))
        return result;
    return result == 0 ? EIO : result;
}

} // namespace

VirglRenderer::VirglRenderer()
{
    if (rendererInstanceActive.exchange(true, std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "an in-process Virgl renderer is already active");
    }
    try {
        epoxy_set_resolver_failure_handler(&handleMissingGlEntryPoint);
        // virgl_renderer_init() retains this pointer until cleanup rather than
        // copying the callback table. Keep it alive for the process lifetime
        // so deferred fence callbacks cannot refer to constructor storage.
        static virgl_renderer_callbacks callbacks = [] {
            virgl_renderer_callbacks result {};
            result.version = VIRGL_RENDERER_CALLBACKS_VERSION;
            result.write_fence = &VirglRenderer::writeFence;
            result.create_gl_context = &VirglRenderer::createGlContext;
            result.destroy_gl_context = &VirglRenderer::destroyGlContext;
            result.make_current = &VirglRenderer::makeGlContextCurrent;
            result.write_context_fence = &VirglRenderer::writeContextFence;
            return result;
        }();

        // With neither USE_EGL nor USE_GLX set, virglrenderer uses the CGL
        // callbacks above. Fence retirement remains explicitly polled because
        // its threaded path relies on Linux eventfd.
        constexpr int rendererFlags = 0;
        const int result =
            virgl_renderer_init(this, rendererFlags, &callbacks);
        const int checkedResult = checkGlDispatchResult(result);
        if (checkedResult != 0) {
            if (result == 0)
                virgl_renderer_cleanup(this);
            throw std::runtime_error(
                "failed to initialize in-process Virgl renderer: " +
                std::to_string(checkedResult));
        }
    } catch (...) {
        rendererInstanceActive.store(false, std::memory_order_release);
        throw;
    }
}

VirglRenderer::~VirglRenderer()
{
    virgl_renderer_cleanup(this);
    rendererInstanceActive.store(false, std::memory_order_release);
}

VirglRenderer::Capset VirglRenderer::capset(std::uint32_t capsetId) const
{
    Capset result;
    std::uint32_t maximumSize = 0;
    virgl_renderer_get_cap_set(
        capsetId, &result.maximumVersion, &maximumSize);
    if (maximumSize == 0)
        throw std::runtime_error("Virgl renderer returned an empty capset");
    result.data.resize(maximumSize);
    virgl_renderer_fill_caps(
        capsetId, result.maximumVersion, result.data.data());
    if (checkGlDispatchResult(0) != 0) {
        throw std::runtime_error(
            "Virgl capset requested an unavailable host GL entry point");
    }
    /*
     * Capability discovery intentionally probes optional GL entry points and
     * formats. Apple's core driver reports GL_INVALID_ENUM for some of those
     * probes. GET_CAPSET may run after a guest context becomes current, so
     * drain probe errors here before CHECK_GL_ERRORS can misattribute them to
     * the guest's next command.
     */
    for (GLenum error = glGetError();
         error != GL_NO_ERROR;
         error = glGetError()) {
        std::fprintf(
            stderr,
            "Virgl CGL capset probe GL error (cleared): %u\n",
            static_cast<unsigned int>(error));
    }
    return result;
}

int VirglRenderer::createContext(
    std::uint32_t contextId,
    std::uint32_t flags,
    std::string_view debugName) noexcept
{
    if (debugName.size() > std::numeric_limits<std::uint32_t>::max())
        return EINVAL;
    const int createResult = virgl_renderer_context_create_with_flags(
        contextId,
        flags,
        static_cast<std::uint32_t>(debugName.size()),
        debugName.data());
    const int checkedResult = checkGlDispatchResult(createResult);
    if (checkedResult != 0) {
        if (createResult == 0)
            virgl_renderer_context_destroy(contextId);
        return checkedResult;
    }

    /*
     * virglrenderer creates a fresh shared CGL context here and assumes that
     * it is returned with a clean GL error state. Apple's 4.1 core driver can
     * leave GL_INVALID_ENUM behind while virglrenderer probes that context;
     * CHECK_GL_ERRORS would otherwise attribute the probe error to the first
     * real guest command and permanently poison the context.
     */
    bool unexpectedError = false;
    for (GLenum error = glGetError();
         error != GL_NO_ERROR;
         error = glGetError()) {
        std::fprintf(
            stderr,
            "Virgl CGL context initialization GL error: %u\n",
            static_cast<unsigned int>(error));
        unexpectedError |= error != GL_INVALID_ENUM;
    }
    if (unexpectedError) {
        virgl_renderer_context_destroy(contextId);
        return EIO;
    }
    return 0;
}

void VirglRenderer::destroyContext(std::uint32_t contextId) noexcept
{
    virgl_renderer_context_destroy(contextId);
    try {
        std::lock_guard lock(mutex_);
        std::erase_if(
            retiredContextFences_,
            [contextId](const auto& entry) {
                return static_cast<std::uint32_t>(entry.first >> 8U) ==
                    contextId;
            });
    } catch (...) {
        // Never carry a C++ exception through teardown or the C renderer API.
    }
}

int VirglRenderer::submit(
    std::uint32_t contextId,
    std::span<const std::uint8_t> command) noexcept
{
    constexpr std::size_t commandWordSize = sizeof(std::uint32_t);
    if (command.size() % commandWordSize != 0 ||
        command.size() / commandWordSize >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }

    // virglrenderer accepts an immutable command but retains a legacy void*
    // API. The implementation does not modify this storage.
    return checkGlDispatchResult(
        virgl_renderer_submit_cmd(
            const_cast<std::uint8_t*>(command.data()),
            static_cast<int>(contextId),
            static_cast<int>(command.size() / commandWordSize)));
}

int VirglRenderer::createResource3d(
    const Resource3d& resource) noexcept
{
    virgl_renderer_resource_create_args arguments {
        .handle = resource.resourceId,
        .target = resource.target,
        .format = resource.format,
        .bind = resource.bind,
        .width = resource.width,
        .height = resource.height,
        .depth = resource.depth,
        .array_size = resource.arraySize,
        .last_level = resource.lastLevel,
        .nr_samples = resource.sampleCount,
        .flags = resource.flags,
    };
    const int createResult =
        virgl_renderer_resource_create(&arguments, nullptr, 0);
    const int checkedResult = checkGlDispatchResult(createResult);
    if (createResult == 0 && checkedResult != 0)
        virgl_renderer_resource_unref(resource.resourceId);
    return checkedResult;
}

int VirglRenderer::transferResource(
    bool toHost,
    std::uint32_t resourceId,
    std::uint32_t contextId,
    std::uint32_t level,
    std::uint32_t stride,
    std::uint32_t layerStride,
    const Box& box,
    std::uint64_t offset) noexcept
{
    virgl_box transferBox {
        .x = box.x,
        .y = box.y,
        .z = box.z,
        .w = box.width,
        .h = box.height,
        .d = box.depth,
    };
    if (toHost) {
        return checkGlDispatchResult(
            virgl_renderer_transfer_write_iov(
                resourceId,
                contextId,
                static_cast<int>(level),
                stride,
                layerStride,
                &transferBox,
                offset,
                nullptr,
                0));
    }
    return checkGlDispatchResult(
        virgl_renderer_transfer_read_iov(
            resourceId,
            contextId,
            level,
            stride,
            layerStride,
            &transferBox,
            offset,
            nullptr,
            0));
}

int VirglRenderer::readResourceRectangle(
    std::uint32_t resourceId,
    std::span<std::uint8_t> destination,
    std::uint32_t offset,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    iovec vector {
        .iov_base = destination.data(),
        .iov_len = destination.size(),
    };
    virgl_renderer_get_rect(
        static_cast<int>(resourceId),
        &vector,
        1,
        offset,
        static_cast<int>(x),
        static_cast<int>(y),
        static_cast<int>(width),
        static_cast<int>(height));
    return checkGlDispatchResult(0);
}

int VirglRenderer::resourceTexture(
    std::uint32_t resourceId,
    std::uint32_t& textureId) noexcept
{
    virgl_renderer_resource_info information {};
    const int result = virgl_renderer_resource_get_info(
        static_cast<int>(resourceId), &information);
    const int checkedResult = checkGlDispatchResult(result);
    if (checkedResult != 0)
        return checkedResult;
    if (information.tex_id == 0)
        return EINVAL;
    textureId = information.tex_id;
    return 0;
}

int VirglRenderer::createFenceAndWait(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceId,
    const std::atomic_bool* stopRequested) noexcept
{
    try {
        if (fenceCallbackFailed_.load(std::memory_order_acquire))
            return EIO;
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            return ECANCELED;
        }
        const bool contextZero = contextId == 0;
        std::uint64_t key = 0;
        int result = 0;
        if (contextZero) {
            if (fenceId > std::numeric_limits<std::uint32_t>::max())
                return EOVERFLOW;
            result = virgl_renderer_create_fence(
                static_cast<int>(fenceId), contextId);
        } else {
            // Virgl fences retire in order within each context and ring.
            constexpr std::uint32_t mergeableFence =
                VIRGL_RENDERER_FENCE_FLAG_MERGEABLE;
            key = contextRingKey(contextId, ringIndex);
            {
                // Publish the callback slot before the renderer can retire the
                // fence. The C callback then performs only allocation-free
                // lookup and update and cannot throw across its ABI boundary.
                std::lock_guard lock(mutex_);
                retiredContextFences_.try_emplace(key, 0);
            }
            result = virgl_renderer_context_create_fence(
                contextId, mergeableFence, ringIndex, fenceId);
        }
        result = checkGlDispatchResult(result);
        if (result != 0)
            return result;

        const auto deadline =
            std::chrono::steady_clock::now() + maximumFenceWait;
        // Polling at one millisecond keeps fence latency low without spinning
        // a host CPU when a Metal queue has substantial work outstanding.
        constexpr auto pollingInterval = std::chrono::milliseconds(1);
        std::unique_lock lock(mutex_);
        while (true) {
            if (fenceCallbackFailed_.load(std::memory_order_acquire))
                return EIO;
            const bool retired = contextZero
                ? retiredFence_ >= fenceId
                : [&] {
                    const auto iterator = retiredContextFences_.find(key);
                    return iterator != retiredContextFences_.end() &&
                        iterator->second >= fenceId;
                }();
            if (retired)
                return 0;
            if (stopRequested != nullptr &&
                stopRequested->load(std::memory_order_acquire)) {
                return ECANCELED;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return ETIMEDOUT;

            lock.unlock();
            if (contextZero)
                virgl_renderer_poll();
            else
                virgl_renderer_context_poll(contextId);
            if (checkGlDispatchResult(0) != 0)
                return EIO;
            lock.lock();
            fenceCondition_.wait_until(
                lock, std::min(deadline, now + pollingInterval));
        }
    } catch (...) {
        return EIO;
    }
}

void VirglRenderer::unrefResource(std::uint32_t resourceId) noexcept
{
    virgl_renderer_resource_unref(resourceId);
}

int VirglRenderer::attachBacking(
    std::uint32_t resourceId,
    std::span<iovec> backing) noexcept
{
    if (backing.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }
    const int result = virgl_renderer_resource_attach_iov(
        static_cast<int>(resourceId),
        backing.data(),
        static_cast<int>(backing.size()));
    if (result != 0) {
        virgl_renderer_resource_detach_iov(
            static_cast<int>(resourceId), nullptr, nullptr);
    }
    return result;
}

void VirglRenderer::detachBacking(std::uint32_t resourceId) noexcept
{
    virgl_renderer_resource_detach_iov(
        static_cast<int>(resourceId), nullptr, nullptr);
}

int VirglRenderer::attachResource(
    std::uint32_t contextId,
    std::uint32_t resourceId) noexcept
{
    virgl_renderer_ctx_attach_resource(
        static_cast<int>(contextId), static_cast<int>(resourceId));
    const int result = checkGlDispatchResult(0);
    if (result != 0) {
        virgl_renderer_ctx_detach_resource(
            static_cast<int>(contextId), static_cast<int>(resourceId));
    }
    return result;
}

int VirglRenderer::detachResource(
    std::uint32_t contextId,
    std::uint32_t resourceId) noexcept
{
    virgl_renderer_ctx_detach_resource(
        static_cast<int>(contextId), static_cast<int>(resourceId));
    const int result = checkGlDispatchResult(0);
    if (result != 0) {
        virgl_renderer_ctx_attach_resource(
            static_cast<int>(contextId), static_cast<int>(resourceId));
    }
    return result;
}

void VirglRenderer::resetFenceState() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        retiredFence_ = 0;
        retiredContextFences_.clear();
        fenceCallbackFailed_.store(false, std::memory_order_release);
    } catch (...) {
    }
}

void* VirglRenderer::createGlContext(
    void* cookie,
    int,
    virgl_renderer_gl_ctx_param* parameters) noexcept
{
    try {
        if (parameters == nullptr || parameters->compat_ctx ||
            parameters->major_ver > 4 ||
            (parameters->major_ver == 4 && parameters->minor_ver > 1) ||
            parameters->major_ver < 3) {
            return nullptr;
        }

        const auto profile =
            parameters->major_ver >= 4
            ? kCGLOGLPVersion_GL4_Core
            : kCGLOGLPVersion_3_2_Core;
        const CGLPixelFormatAttribute attributes[] {
            kCGLPFAOpenGLProfile,
            static_cast<CGLPixelFormatAttribute>(profile),
            kCGLPFAAccelerated,
            kCGLPFANoRecovery,
            static_cast<CGLPixelFormatAttribute>(0),
        };

        CGLPixelFormatObj pixelFormat = nullptr;
        GLint pixelFormatCount = 0;
        if (CGLChoosePixelFormat(
                attributes, &pixelFormat, &pixelFormatCount) != kCGLNoError ||
            pixelFormat == nullptr || pixelFormatCount == 0) {
            return nullptr;
        }

        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        CGLContextObj sharedContext = nullptr;
        {
            std::lock_guard lock(renderer.glContextMutex_);
            if (parameters->shared) {
                sharedContext =
                    static_cast<CGLContextObj>(renderer.sharedGlContext_);
            }
        }
        if (parameters->shared && sharedContext == nullptr) {
            CGLReleasePixelFormat(pixelFormat);
            return nullptr;
        }

        CGLContextObj context = nullptr;
        const auto result =
            CGLCreateContext(pixelFormat, sharedContext, &context);
        CGLReleasePixelFormat(pixelFormat);
        if (result != kCGLNoError || context == nullptr)
            return nullptr;

        if (!parameters->shared) {
            std::lock_guard lock(renderer.glContextMutex_);
            renderer.sharedGlContext_ = context;
        }
        return context;
    } catch (...) {
        return nullptr;
    }
}

void VirglRenderer::destroyGlContext(void* cookie, void* context) noexcept
{
    try {
        if (context == nullptr)
            return;
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        {
            std::lock_guard lock(renderer.glContextMutex_);
            if (renderer.sharedGlContext_ == context)
                renderer.sharedGlContext_ = nullptr;
        }
        CGLDestroyContext(static_cast<CGLContextObj>(context));
    } catch (...) {
    }
}

int VirglRenderer::makeGlContextCurrent(void*, int, void* context) noexcept
{
    return CGLSetCurrentContext(static_cast<CGLContextObj>(context)) ==
            kCGLNoError
        ? 0
        : -EIO;
}

void VirglRenderer::writeFence(
    void* cookie, std::uint32_t fenceId) noexcept
{
    try {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        {
            std::lock_guard lock(renderer.mutex_);
            renderer.retiredFence_ = std::max(renderer.retiredFence_, fenceId);
        }
        renderer.fenceCondition_.notify_all();
    } catch (...) {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        renderer.fenceCallbackFailed_.store(true, std::memory_order_release);
        renderer.fenceCondition_.notify_all();
    }
}

void VirglRenderer::writeContextFence(
    void* cookie,
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceId) noexcept
{
    try {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        {
            std::lock_guard lock(renderer.mutex_);
            const auto iterator = renderer.retiredContextFences_.find(
                contextRingKey(contextId, ringIndex));
            if (iterator != renderer.retiredContextFences_.end())
                iterator->second = std::max(iterator->second, fenceId);
        }
        renderer.fenceCondition_.notify_all();
    } catch (...) {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        renderer.fenceCallbackFailed_.store(true, std::memory_order_release);
        renderer.fenceCondition_.notify_all();
    }
}

std::uint64_t VirglRenderer::contextRingKey(
    std::uint32_t contextId,
    std::uint32_t ringIndex) noexcept
{
    // The Virtio-GPU header carries an 8-bit ring index.
    return (static_cast<std::uint64_t>(contextId) << 8U) |
        (ringIndex & 0xffU);
}

} // namespace uml::gpu
