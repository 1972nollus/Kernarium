// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <EGL/eglext_angle.h>

// EGL display creation must precede libepoxy's version discovery. The common
// framework exports this platform bootstrap entry point directly; all later
// EGL and GLES dispatch continues through libepoxy.
#undef eglGetPlatformDisplayEXT
#undef eglGetError
extern "C" EGLDisplay eglGetPlatformDisplayEXT(
    EGLenum platform,
    void* nativeDisplay,
    const EGLint* attributes);
extern "C" EGLint eglGetError(void);

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>

namespace uml::gpu {
namespace {

void reportEglFailure(const char* operation) noexcept
{
    std::fprintf(
        stderr,
        "ANGLE/Metal EGL %s failed: 0x%04x\n",
        operation,
        static_cast<unsigned int>(eglGetError()));
}

std::string glString(GLenum name)
{
    const auto* value = glGetString(name);
    return value == nullptr ? std::string {} :
        std::string(reinterpret_cast<const char*>(value));
}

} // namespace

HostGlProvider::~HostGlProvider()
{
    std::lock_guard lock(mutex_);
    void* publishedDisplay = display_.load(std::memory_order_acquire);
    if (publishedDisplay == nullptr)
        return;
    const auto eglDisplay = static_cast<EGLDisplay>(publishedDisplay);
    (void)eglMakeCurrent(
        eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    (void)eglTerminate(eglDisplay);
    display_.store(nullptr, std::memory_order_release);
    config_ = nullptr;
    sharedContext_ = nullptr;
}

void* HostGlProvider::display() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (void* publishedDisplay =
                display_.load(std::memory_order_acquire)) {
            return publishedDisplay;
        }
        const EGLint displayAttributes[] {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE,
            EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
            EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
            EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
            EGL_NONE,
        };
        // The packaged EGL facade loads the implementation from its
        // authenticated GLES companion framework.
        const auto eglDisplay = eglGetPlatformDisplayEXT(
            EGL_PLATFORM_ANGLE_ANGLE, nullptr, displayAttributes);
        if (eglDisplay == EGL_NO_DISPLAY) {
            reportEglFailure("display creation");
            return nullptr;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(eglDisplay, &major, &minor) != EGL_TRUE) {
            reportEglFailure("initialization");
            (void)eglTerminate(eglDisplay);
            return nullptr;
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            reportEglFailure("API binding");
            (void)eglTerminate(eglDisplay);
            return nullptr;
        }
        const EGLint configAttributes[] {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig eglConfig = nullptr;
        EGLint count = 0;
        if (eglChooseConfig(
                eglDisplay,
                configAttributes,
                &eglConfig,
                1,
                &count) != EGL_TRUE ||
            count != 1 || eglConfig == nullptr) {
            reportEglFailure("configuration selection");
            (void)eglTerminate(eglDisplay);
            return nullptr;
        }
        config_ = eglConfig;
        std::fputs("Virgl ANGLE/Metal provider: hardware\n", stderr);
        display_.store(eglDisplay, std::memory_order_release);
        return eglDisplay;
    } catch (...) {
        return nullptr;
    }
}

HostGlProvider::ContextProbe HostGlProvider::probeContext(
    std::uint32_t major,
    std::uint32_t minor) noexcept
{
    ContextProbe result {
        .requestedMajor = major,
        .requestedMinor = minor,
    };
    if (display() == nullptr) {
        result.eglError = static_cast<std::uint32_t>(eglGetError());
        return result;
    }

    std::lock_guard lock(mutex_);
    const auto eglDisplay = static_cast<EGLDisplay>(
        display_.load(std::memory_order_acquire));
    const auto eglConfig = static_cast<EGLConfig>(config_);
    const EGLDisplay previousDisplay = eglGetCurrentDisplay();
    const EGLContext previousContext = eglGetCurrentContext();
    const EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
    EGLContext context = EGL_NO_CONTEXT;
    auto restore = [&] {
        if (previousDisplay != EGL_NO_DISPLAY) {
            (void)eglMakeCurrent(
                previousDisplay,
                previousDraw,
                previousRead,
                previousContext);
        } else {
            (void)eglMakeCurrent(
                eglDisplay,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                EGL_NO_CONTEXT);
        }
        if (context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(eglDisplay, context);
    };

    try {
        const EGLint attributes[] {
            EGL_CONTEXT_MAJOR_VERSION_KHR, static_cast<EGLint>(major),
            EGL_CONTEXT_MINOR_VERSION_KHR, static_cast<EGLint>(minor),
            EGL_NONE,
        };
        context = eglCreateContext(
            eglDisplay, eglConfig, EGL_NO_CONTEXT, attributes);
        if (context == EGL_NO_CONTEXT) {
            result.eglError = static_cast<std::uint32_t>(eglGetError());
            return result;
        }
        if (eglMakeCurrent(
                eglDisplay,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                context) != EGL_TRUE) {
            result.eglError = static_cast<std::uint32_t>(eglGetError());
            restore();
            return result;
        }

        GLint actualMajor = 0;
        GLint actualMinor = 0;
        GLint extensionCount = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &actualMajor);
        glGetIntegerv(GL_MINOR_VERSION, &actualMinor);
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
        result.actualMajor = actualMajor < 0 ? 0U :
            static_cast<std::uint32_t>(actualMajor);
        result.actualMinor = actualMinor < 0 ? 0U :
            static_cast<std::uint32_t>(actualMinor);
        result.vendor = glString(GL_VENDOR);
        result.renderer = glString(GL_RENDERER);
        result.version = glString(GL_VERSION);
        result.shadingLanguageVersion =
            glString(GL_SHADING_LANGUAGE_VERSION);
        if (extensionCount > 0) {
            result.extensions.reserve(
                static_cast<std::size_t>(extensionCount));
            for (GLint index = 0; index < extensionCount; ++index) {
                const auto* extension = glGetStringi(
                    GL_EXTENSIONS, static_cast<GLuint>(index));
                if (extension != nullptr) {
                    result.extensions.emplace_back(
                        reinterpret_cast<const char*>(extension));
                }
            }
            std::ranges::sort(result.extensions);
            result.extensions.erase(
                std::unique(
                    result.extensions.begin(), result.extensions.end()),
                result.extensions.end());
        }
        result.created = true;
        result.eglError = EGL_SUCCESS;
        restore();
        return result;
    } catch (...) {
        result = ContextProbe {
            .requestedMajor = major,
            .requestedMinor = minor,
            .eglError = EGL_BAD_ALLOC,
        };
        restore();
        return result;
    }
}

void* HostGlProvider::createContext(
    const virgl_renderer_gl_ctx_param& parameters) noexcept
{
    try {
        if (parameters.version != 2 || parameters.compat_ctx ||
            parameters.major_ver < 2 ||
            parameters.major_ver > 3) {
            return nullptr;
        }
        if (display() == nullptr)
            return nullptr;
        std::lock_guard lock(mutex_);
        const auto eglDisplay = static_cast<EGLDisplay>(
            display_.load(std::memory_order_acquire));
        const auto eglConfig = static_cast<EGLConfig>(config_);
        EGLContext shared = EGL_NO_CONTEXT;
        if (parameters.shared) {
            shared = static_cast<EGLContext>(sharedContext_);
            if (shared == EGL_NO_CONTEXT)
                return nullptr;
        }
        const EGLint attributes[] {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE,
        };
        const auto context = eglCreateContext(
            eglDisplay, eglConfig, shared, attributes);
        if (context == EGL_NO_CONTEXT) {
            reportEglFailure("context creation");
            return nullptr;
        }
        if (!parameters.shared)
            sharedContext_ = context;
        return context;
    } catch (...) {
        return nullptr;
    }
}

void HostGlProvider::destroyContext(void* context) noexcept
{
    try {
        if (context == nullptr)
            return;
        std::lock_guard lock(mutex_);
        if (sharedContext_ == context)
            sharedContext_ = nullptr;
        void* publishedDisplay = display_.load(std::memory_order_acquire);
        if (publishedDisplay != nullptr) {
            (void)eglDestroyContext(
                static_cast<EGLDisplay>(publishedDisplay),
                static_cast<EGLContext>(context));
        }
    } catch (...) {
    }
}

int HostGlProvider::makeCurrent(void* context) noexcept
{
    void* publishedDisplay = display_.load(std::memory_order_acquire);
    if (publishedDisplay == nullptr)
        return -EIO;
    if (eglMakeCurrent(
            static_cast<EGLDisplay>(publishedDisplay),
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            static_cast<EGLContext>(context)) == EGL_TRUE) {
        return 0;
    }
    reportEglFailure("make-current");
    return -EIO;
}

} // namespace uml::gpu
