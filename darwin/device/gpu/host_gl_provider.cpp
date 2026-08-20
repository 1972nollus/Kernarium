// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"

#include <epoxy/egl.h>
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

#include <cerrno>
#include <cstdio>

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
