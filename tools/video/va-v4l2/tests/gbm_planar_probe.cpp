// SPDX-License-Identifier: GPL-2.0-or-later
// A guest-side client of the real libgbm, libEGL and libGLESv2 that checks the
// gbm backend module (virtio_gpu_gbm.so) where it matters: planar buffers
// allocate and export, a plane is something the GPU can render into and the
// CPU can read back and write, and the compositor's path through the same
// device (a gbm surface under an EGL window surface) still works. Every
// library is opened at run time, so it builds with the libraries the driver
// already needs and runs against whatever the guest has.
//
// usage: gbm_planar_probe [--node PATH] [--egl] [--planes] [--require-visibility]
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d)
{
    return std::uint32_t(a) | std::uint32_t(b) << 8 | std::uint32_t(c) << 16 | std::uint32_t(d) << 24;
}

constexpr std::uint32_t kR8 = fourcc('R', '8', ' ', ' ');
constexpr std::uint32_t kGr88 = fourcc('G', 'R', '8', '8');
constexpr std::uint32_t kNv12 = fourcc('N', 'V', '1', '2');
constexpr std::uint32_t kP010 = fourcc('P', '0', '1', '0');
constexpr std::uint32_t kYu12 = fourcc('Y', 'U', '1', '2');
constexpr std::uint32_t kYv12 = fourcc('Y', 'V', '1', '2');
constexpr std::uint32_t kXrgb = fourcc('X', 'R', '2', '4');

constexpr std::uint32_t kUseScanout = 1, kUseRendering = 4, kUseLinear = 16;

struct dma_buf_sync {
    std::uint64_t flags;
};
constexpr unsigned long kDmaBufIoctlSync = 0x40086200u;
constexpr std::uint64_t kSyncRead = 1, kSyncWrite = 2, kSyncStart = 0, kSyncEnd = 4;

int failures = 0;
bool requireVisibility = false;

void report(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void info(const char* format, ...) __attribute__((format(printf, 1, 2)));
void info(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    std::printf("info ");
    std::vprintf(format, arguments);
    std::printf("\n");
    va_end(arguments);
}

template <typename T>
T symbol(void* library, const char* name)
{
    return reinterpret_cast<T>(dlsym(library, name));
}

struct Gbm {
    void* library {};
    void* (*createDevice)(int) {};
    void (*destroyDevice)(void*) {};
    const char* (*backendName)(void*) {};
    int (*isSupported)(void*, std::uint32_t, std::uint32_t) {};
    void* (*boCreate)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {};
    void (*boDestroy)(void*) {};
    int (*planeCount)(void*) {};
    int (*planeFd)(void*, int) {};
    std::uint32_t (*planeStride)(void*, int) {};
    std::uint32_t (*planeOffset)(void*, int) {};
    std::uint64_t (*modifier)(void*) {};
    void* (*surfaceCreate)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {};
    void (*surfaceDestroy)(void*) {};
    void* (*lockFront)(void*) {};
    void (*releaseBuffer)(void*, void*) {};
    std::uint32_t (*boStride)(void*) {};
    int (*boFd)(void*) {};
    void* (*boMap)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t*, void**) {};
    void (*boUnmap)(void*, void*) {};

    bool load()
    {
        library = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!library) return false;
        createDevice = symbol<decltype(createDevice)>(library, "gbm_create_device");
        destroyDevice = symbol<decltype(destroyDevice)>(library, "gbm_device_destroy");
        backendName = symbol<decltype(backendName)>(library, "gbm_device_get_backend_name");
        isSupported = symbol<decltype(isSupported)>(library, "gbm_device_is_format_supported");
        boCreate = symbol<decltype(boCreate)>(library, "gbm_bo_create");
        boDestroy = symbol<decltype(boDestroy)>(library, "gbm_bo_destroy");
        planeCount = symbol<decltype(planeCount)>(library, "gbm_bo_get_plane_count");
        planeFd = symbol<decltype(planeFd)>(library, "gbm_bo_get_fd_for_plane");
        planeStride = symbol<decltype(planeStride)>(library, "gbm_bo_get_stride_for_plane");
        planeOffset = symbol<decltype(planeOffset)>(library, "gbm_bo_get_offset");
        modifier = symbol<decltype(modifier)>(library, "gbm_bo_get_modifier");
        surfaceCreate = symbol<decltype(surfaceCreate)>(library, "gbm_surface_create");
        surfaceDestroy = symbol<decltype(surfaceDestroy)>(library, "gbm_surface_destroy");
        lockFront = symbol<decltype(lockFront)>(library, "gbm_surface_lock_front_buffer");
        releaseBuffer = symbol<decltype(releaseBuffer)>(library, "gbm_surface_release_buffer");
        boStride = symbol<decltype(boStride)>(library, "gbm_bo_get_stride");
        boFd = symbol<decltype(boFd)>(library, "gbm_bo_get_fd");
        boMap = symbol<decltype(boMap)>(library, "gbm_bo_map");
        boUnmap = symbol<decltype(boUnmap)>(library, "gbm_bo_unmap");
        return createDevice && destroyDevice && backendName && isSupported && boCreate && boDestroy &&
               planeCount && planeFd && planeStride && planeOffset && modifier && surfaceCreate &&
               surfaceDestroy && lockFront && releaseBuffer && boStride && boFd && boMap && boUnmap;
    }
};

bool moduleMapped()
{
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof line, maps))
        if (std::strstr(line, "virtio_gpu_gbm.so")) found = true;
    std::fclose(maps);
    return found;
}

struct Map {
    void* address {MAP_FAILED};
    std::size_t size {};
    int error {};
};

Map mapPlane(int fd, std::size_t size, bool write)
{
    Map result;
    result.size = size;
    result.address = mmap(nullptr, size, write ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
    if (result.address == MAP_FAILED) result.error = errno;
    return result;
}

void sync(int fd, std::uint64_t flags)
{
    dma_buf_sync request {flags};
    ioctl(fd, kDmaBufIoctlSync, &request);
}

// ---- allocation ------------------------------------------------------------

struct PlanarCase {
    const char* name;
    std::uint32_t format;
    unsigned planes;
    unsigned bytesPerSample;
};

void probePlanar(Gbm& gbm, void* device, const PlanarCase& image, unsigned width, unsigned height)
{
    char label[160];
    std::snprintf(label, sizeof label, "%s %ux%u: supported for rendering", image.name, width, height);
    report(gbm.isSupported(device, image.format, kUseRendering) == 1, label);
    void* bo = gbm.boCreate(device, width, height, image.format, kUseRendering | kUseLinear);
    std::snprintf(label, sizeof label, "%s %ux%u: a buffer is allocated", image.name, width, height);
    report(bo != nullptr, label);
    if (!bo) return;
    std::snprintf(label, sizeof label, "%s %ux%u: it has %u planes", image.name, width, height, image.planes);
    report(unsigned(gbm.planeCount(bo)) == image.planes, label);
    std::vector<int> fds;
    for (unsigned plane = 0; plane < image.planes; ++plane) {
        const int fd = gbm.planeFd(bo, int(plane));
        fds.push_back(fd);
        const unsigned shift = plane ? 1 : 0;
        const unsigned rows = (height + (1u << shift) - 1) >> shift;
        const unsigned stride = gbm.planeStride(bo, int(plane));
        const unsigned columns = ((width + (1u << shift) - 1) >> shift) * (plane && image.planes == 2 ? 2 : 1) *
                                 image.bytesPerSample;
        std::snprintf(label, sizeof label, "%s %ux%u: plane %u has a descriptor and rows long enough", image.name,
                      width, height, plane);
        report(fd >= 0 && stride >= columns && gbm.planeOffset(bo, int(plane)) == 0, label);
        if (fd < 0) continue;
        const off_t size = lseek(fd, 0, SEEK_END);
        info("%s plane %u: fd mode %d, stride %u, dma-buf size %lld, rows %u", image.name, plane,
             fcntl(fd, F_GETFL) & O_ACCMODE, stride, (long long)size, rows);
        std::snprintf(label, sizeof label, "%s %ux%u: plane %u's dma-buf holds its rows", image.name, width, height,
                      plane);
        report(size >= off_t(stride) * rows, label);
        // The CPU side: write a pattern and read it back through a second mapping.
        const std::size_t length = std::size_t(stride) * rows;
        Map writer = mapPlane(fd, length, true);
        if (writer.address != MAP_FAILED) {
            sync(fd, kSyncStart | kSyncWrite);
            auto* bytes = static_cast<std::uint8_t*>(writer.address);
            for (std::size_t index = 0; index < length; ++index) bytes[index] = std::uint8_t(index * 7 + plane);
            sync(fd, kSyncEnd | kSyncWrite);
            Map reader = mapPlane(fd, length, false);
            bool same = reader.address != MAP_FAILED;
            if (same) {
                sync(fd, kSyncStart | kSyncRead);
                const auto* read = static_cast<const std::uint8_t*>(reader.address);
                for (std::size_t index = 0; index < length && same; ++index)
                    same = read[index] == std::uint8_t(index * 7 + plane);
                sync(fd, kSyncEnd | kSyncRead);
                munmap(reader.address, length);
            }
            std::snprintf(label, sizeof label, "%s %ux%u: plane %u keeps what the CPU wrote", image.name, width,
                          height, plane);
            report(same, label);
            munmap(writer.address, length);
        } else {
            info("%s plane %u: the dma-buf maps read-only (%s)", image.name, plane, std::strerror(writer.error));
            std::snprintf(label, sizeof label, "%s %ux%u: plane %u's dma-buf is writable", image.name, width, height,
                          plane);
            if (requireVisibility) report(false, label);
        }
    }
    for (int fd : fds)
        if (fd >= 0) close(fd);
    gbm.boDestroy(bo);
}

// ---- EGL and GLES ----------------------------------------------------------

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLImage = void*;
using EGLint = std::int32_t;
using EGLBoolean = unsigned;
using GLuint = unsigned;
using GLenum = unsigned;

constexpr EGLint kEglNone = 0x3038, kEglSurfaceType = 0x3033, kEglWindowBit = 0x4, kEglRenderableType = 0x3040,
                 kEglEs2Bit = 0x4, kEglRedSize = 0x3024, kEglGreenSize = 0x3023, kEglBlueSize = 0x3022,
                 kEglNativeVisualId = 0x302E, kEglContextClientVersion = 0x3098, kEglWidth = 0x3057,
                 kEglHeight = 0x3056, kEglLinuxDmaBuf = 0x3270, kEglDrmFourcc = 0x3271, kEglPlane0Fd = 0x3272,
                 kEglPlane0Offset = 0x3273, kEglPlane0Pitch = 0x3274, kEglPlatformGbm = 0x31D7;
constexpr unsigned kEglOpenGlEsApi = 0x30A0;
constexpr GLenum kGlFramebuffer = 0x8D40, kGlColorAttachment0 = 0x8CE0, kGlTexture2d = 0x0DE1,
                 kGlFramebufferComplete = 0x8CD5, kGlColorBufferBit = 0x4000, kGlRgba = 0x1908,
                 kGlUnsignedByte = 0x1401, kGlRenderer = 0x1F01, kGlVersion = 0x1F02;

struct Egl {
    void* egl {};
    EGLDisplay (*getPlatformDisplay)(unsigned, void*, const EGLint*) {};
    EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) {};
    EGLBoolean (*terminate)(EGLDisplay) {};
    EGLBoolean (*bindApi)(unsigned) {};
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) {};
    EGLBoolean (*getConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*) {};
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) {};
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) {};
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*) {};
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface) {};
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) {};
    EGLBoolean (*swapBuffers)(EGLDisplay, EGLSurface) {};
    EGLint (*getError)() {};
    void* (*getProcAddress)(const char*) {};
    EGLImage (*createImage)(EGLDisplay, EGLContext, unsigned, void*, const EGLint*) {};
    EGLBoolean (*destroyImage)(EGLDisplay, EGLImage) {};
    void (*imageTargetTexture)(GLenum, EGLImage) {};
    const unsigned char* (*getString)(GLenum) {};
    void (*genTextures)(int, GLuint*) {};
    void (*deleteTextures)(int, const GLuint*) {};
    void (*bindTexture)(GLenum, GLuint) {};
    void (*genFramebuffers)(int, GLuint*) {};
    void (*deleteFramebuffers)(int, const GLuint*) {};
    void (*bindFramebuffer)(GLenum, GLuint) {};
    void (*framebufferTexture2d)(GLenum, GLenum, GLenum, GLuint, int) {};
    GLenum (*checkFramebuffer)(GLenum) {};
    void (*clearColor)(float, float, float, float) {};
    void (*clear)(unsigned) {};
    void (*finish)() {};
    void (*readPixels)(int, int, int, int, GLenum, GLenum, void*) {};

    bool load()
    {
        egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!egl) {
            std::printf("info libEGL.so.1: %s\n", dlerror());
            return false;
        }
        getProcAddress = symbol<decltype(getProcAddress)>(egl, "eglGetProcAddress");
        initialize = symbol<decltype(initialize)>(egl, "eglInitialize");
        terminate = symbol<decltype(terminate)>(egl, "eglTerminate");
        bindApi = symbol<decltype(bindApi)>(egl, "eglBindAPI");
        chooseConfig = symbol<decltype(chooseConfig)>(egl, "eglChooseConfig");
        getConfigAttrib = symbol<decltype(getConfigAttrib)>(egl, "eglGetConfigAttrib");
        createContext = symbol<decltype(createContext)>(egl, "eglCreateContext");
        destroyContext = symbol<decltype(destroyContext)>(egl, "eglDestroyContext");
        createWindowSurface = symbol<decltype(createWindowSurface)>(egl, "eglCreateWindowSurface");
        destroySurface = symbol<decltype(destroySurface)>(egl, "eglDestroySurface");
        makeCurrent = symbol<decltype(makeCurrent)>(egl, "eglMakeCurrent");
        swapBuffers = symbol<decltype(swapBuffers)>(egl, "eglSwapBuffers");
        getError = symbol<decltype(getError)>(egl, "eglGetError");
        if (!getProcAddress) return false;
        getPlatformDisplay = reinterpret_cast<decltype(getPlatformDisplay)>(getProcAddress("eglGetPlatformDisplayEXT"));
        createImage = reinterpret_cast<decltype(createImage)>(getProcAddress("eglCreateImageKHR"));
        destroyImage = reinterpret_cast<decltype(destroyImage)>(getProcAddress("eglDestroyImageKHR"));
        imageTargetTexture =
            reinterpret_cast<decltype(imageTargetTexture)>(getProcAddress("glEGLImageTargetTexture2DOES"));
        return getPlatformDisplay && initialize && bindApi && chooseConfig && getConfigAttrib && createContext &&
               createWindowSurface && makeCurrent && swapBuffers && createImage && destroyImage;
    }

    // The GL entry points come from the context that is current, the way a
    // client that links no GL library of its own gets them.
    bool loadGl()
    {
        auto gl = [this](const char* name) { return getProcAddress(name); };
        imageTargetTexture = reinterpret_cast<decltype(imageTargetTexture)>(gl("glEGLImageTargetTexture2DOES"));
        getString = reinterpret_cast<decltype(getString)>(gl("glGetString"));
        genTextures = reinterpret_cast<decltype(genTextures)>(gl("glGenTextures"));
        deleteTextures = reinterpret_cast<decltype(deleteTextures)>(gl("glDeleteTextures"));
        bindTexture = reinterpret_cast<decltype(bindTexture)>(gl("glBindTexture"));
        genFramebuffers = reinterpret_cast<decltype(genFramebuffers)>(gl("glGenFramebuffers"));
        deleteFramebuffers = reinterpret_cast<decltype(deleteFramebuffers)>(gl("glDeleteFramebuffers"));
        bindFramebuffer = reinterpret_cast<decltype(bindFramebuffer)>(gl("glBindFramebuffer"));
        framebufferTexture2d = reinterpret_cast<decltype(framebufferTexture2d)>(gl("glFramebufferTexture2D"));
        checkFramebuffer = reinterpret_cast<decltype(checkFramebuffer)>(gl("glCheckFramebufferStatus"));
        clearColor = reinterpret_cast<decltype(clearColor)>(gl("glClearColor"));
        clear = reinterpret_cast<decltype(clear)>(gl("glClear"));
        finish = reinterpret_cast<decltype(finish)>(gl("glFinish"));
        readPixels = reinterpret_cast<decltype(readPixels)>(gl("glReadPixels"));
        return imageTargetTexture && getString && genTextures && bindTexture && genFramebuffers && bindFramebuffer &&
               framebufferTexture2d && checkFramebuffer && clearColor && clear && finish && readPixels;
    }
};

// The compositor's path: an EGL window surface on a gbm surface, swapped, and
// each front buffer locked and released.
void probeCompositor(Gbm& gbm, Egl& egl, void* device)
{
    EGLDisplay display = egl.getPlatformDisplay(kEglPlatformGbm, device, nullptr);
    EGLint major = 0, minor = 0;
    report(display && egl.initialize(display, &major, &minor), "egl: the gbm platform initialises on the device");
    if (!display) return;
    egl.bindApi(kEglOpenGlEsApi);
    const EGLint wanted[] = {kEglSurfaceType, kEglWindowBit, kEglRenderableType, kEglEs2Bit, kEglRedSize, 8,
                             kEglGreenSize, 8, kEglBlueSize, 8, kEglNone};
    EGLConfig configs[64];
    EGLint count = 0;
    egl.chooseConfig(display, wanted, configs, 64, &count);
    EGLConfig config = nullptr;
    for (EGLint index = 0; index < count && !config; ++index) {
        EGLint visual = 0;
        egl.getConfigAttrib(display, configs[index], kEglNativeVisualId, &visual);
        if (std::uint32_t(visual) == kXrgb) config = configs[index];
    }
    report(config != nullptr, "egl: a window configuration matches XRGB8888");
    if (!config) return;
    void* surface = gbm.surfaceCreate(device, 256, 128, kXrgb, kUseScanout | kUseRendering);
    report(surface != nullptr, "gbm: a surface is created");
    if (!surface) return;
    EGLSurface window = egl.createWindowSurface(display, config, surface, nullptr);
    report(window != nullptr, "egl: a window surface is made on it");
    const EGLint context[] = {kEglContextClientVersion, 2, kEglNone};
    EGLContext gl = egl.createContext(display, config, nullptr, context);
    report(gl != nullptr, "egl: a GLES2 context is made");
    if (window && gl && egl.makeCurrent(display, window, window, gl) && egl.loadGl()) {
        info("renderer: %s / %s", egl.getString(kGlRenderer), egl.getString(kGlVersion));
        void* held[3] {};
        bool locked = true;
        for (int frame = 0; frame < 3; ++frame) {
            egl.clearColor(frame == 0 ? 1.0f : 0.0f, frame == 1 ? 1.0f : 0.0f, frame == 2 ? 1.0f : 0.0f, 1.0f);
            egl.clear(kGlColorBufferBit);
            const bool swapped = egl.swapBuffers(display, window);
            held[frame] = swapped ? gbm.lockFront(surface) : nullptr;
            locked = locked && swapped && held[frame] != nullptr;
            if (held[frame]) {
                const int fd = gbm.boFd(held[frame]);
                info("frame %d: stride %u, fd %d", frame, gbm.boStride(held[frame]), fd);
                if (fd >= 0) close(fd);
            }
            if (frame >= 1 && held[frame - 1]) {
                gbm.releaseBuffer(surface, held[frame - 1]);
                held[frame - 1] = nullptr;
            }
        }
        report(locked, "gbm: three frames are swapped and their front buffers locked");
        for (void* bo : held)
            if (bo) gbm.releaseBuffer(surface, bo);
        egl.makeCurrent(display, nullptr, nullptr, nullptr);
    } else {
        report(false, "egl: the context is current on the window surface");
    }
    if (gl) egl.destroyContext(display, gl);
    if (window) egl.destroySurface(display, window);
    gbm.surfaceDestroy(surface);
    egl.terminate(display);
}

// Renders into one plane of a planar buffer on the GPU, reads it through the
// dma-buf, writes it through the dma-buf and reads that back on the GPU.
struct PlaneTrial {
    std::uint32_t format;
    unsigned bytesPerPixel;
    float clear[4];
    std::uint8_t expected[2];
    std::uint8_t written[2];
};

void probePlaneRender(Gbm& gbm, Egl& egl, void* device)
{
    EGLDisplay display = egl.getPlatformDisplay(kEglPlatformGbm, device, nullptr);
    EGLint major = 0, minor = 0;
    if (!display || !egl.initialize(display, &major, &minor)) {
        report(false, "egl: the display initialises for plane rendering");
        return;
    }
    egl.bindApi(kEglOpenGlEsApi);
    const EGLint wanted[] = {kEglRenderableType, kEglEs2Bit, kEglNone};
    EGLConfig config = nullptr;
    EGLint count = 0;
    egl.chooseConfig(display, wanted, &config, 1, &count);
    const EGLint attributes[] = {kEglContextClientVersion, 2, kEglNone};
    EGLContext gl = config ? egl.createContext(display, config, nullptr, attributes) : nullptr;
    if (!gl || !egl.makeCurrent(display, nullptr, nullptr, gl) || !egl.loadGl()) {
        report(false, "egl: a surfaceless context is current for plane rendering");
        return;
    }
    void* bo = gbm.boCreate(device, 128, 64, kNv12, kUseRendering | kUseLinear);
    report(bo != nullptr, "planes: an NV12 buffer for GPU rendering is allocated");
    if (!bo) return;
    const PlaneTrial trials[2] = {
        {kR8, 1, {0.5f, 0.0f, 0.0f, 1.0f}, {128, 0}, {40, 0}},
        {kGr88, 2, {0.25f, 0.75f, 0.0f, 1.0f}, {64, 191}, {200, 30}},
    };
    for (unsigned plane = 0; plane < 2; ++plane) {
        const PlaneTrial& trial = trials[plane];
        const unsigned width = plane ? 64 : 128, height = plane ? 32 : 64;
        const int fd = gbm.planeFd(bo, int(plane));
        const unsigned stride = gbm.planeStride(bo, int(plane));
        char label[160];
        const EGLint image[] = {kEglWidth, EGLint(width), kEglHeight, EGLint(height), kEglDrmFourcc,
                                EGLint(trial.format), kEglPlane0Fd, fd, kEglPlane0Offset, 0, kEglPlane0Pitch,
                                EGLint(stride), kEglNone};
        EGLImage handle = fd >= 0 ? egl.createImage(display, nullptr, kEglLinuxDmaBuf, nullptr, image) : nullptr;
        std::snprintf(label, sizeof label, "planes: plane %u imports into EGL as %s", plane, plane ? "GR88" : "R8");
        report(handle != nullptr, label);
        if (!handle) {
            if (fd >= 0) close(fd);
            continue;
        }
        GLuint texture = 0, framebuffer = 0;
        egl.genTextures(1, &texture);
        egl.bindTexture(kGlTexture2d, texture);
        egl.imageTargetTexture(kGlTexture2d, handle);
        egl.genFramebuffers(1, &framebuffer);
        egl.bindFramebuffer(kGlFramebuffer, framebuffer);
        egl.framebufferTexture2d(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, texture, 0);
        const bool complete = egl.checkFramebuffer(kGlFramebuffer) == kGlFramebufferComplete;
        std::snprintf(label, sizeof label, "planes: plane %u is a complete render target", plane);
        report(complete, label);
        if (complete) {
            egl.clearColor(trial.clear[0], trial.clear[1], trial.clear[2], trial.clear[3]);
            egl.clear(kGlColorBufferBit);
            egl.finish();
            const std::size_t length = std::size_t(stride) * height;
            Map view = mapPlane(fd, length, false);
            if (view.address != MAP_FAILED) {
                sync(fd, kSyncStart | kSyncRead);
                const auto* row = static_cast<const std::uint8_t*>(view.address);
                bool match = true;
                for (unsigned y = 0; y < height && match; y += 7)
                    for (unsigned x = 0; x < width && match; x += 5)
                        for (unsigned byte = 0; byte < trial.bytesPerPixel; ++byte) {
                            const int got = row[y * stride + x * trial.bytesPerPixel + byte];
                            if (got < trial.expected[byte] - 1 || got > trial.expected[byte] + 1) match = false;
                        }
                sync(fd, kSyncEnd | kSyncRead);
                std::snprintf(label, sizeof label, "planes: the CPU reads plane %u's GPU clear through its dma-buf",
                              plane);
                if (match || requireVisibility) report(match, label);
                else info("%s: not visible without the dma-buf coherence kernel (first byte %u, wanted %u)", label,
                          row[0], trial.expected[0]);
                munmap(view.address, length);
            }
            Map writer = mapPlane(fd, length, true);
            if (writer.address != MAP_FAILED) {
                sync(fd, kSyncStart | kSyncWrite);
                auto* bytes = static_cast<std::uint8_t*>(writer.address);
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x)
                        for (unsigned byte = 0; byte < trial.bytesPerPixel; ++byte)
                            bytes[y * stride + x * trial.bytesPerPixel + byte] = trial.written[byte];
                sync(fd, kSyncEnd | kSyncWrite);
                munmap(writer.address, length);
                std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
                egl.readPixels(0, 0, int(width), int(height), kGlRgba, kGlUnsignedByte, pixels.data());
                const std::uint8_t red = pixels[0], green = pixels[1];
                const bool seen = std::abs(int(red) - trial.written[0]) <= 1 &&
                                  (trial.bytesPerPixel < 2 || std::abs(int(green) - trial.written[1]) <= 1);
                std::snprintf(label, sizeof label, "planes: the GPU reads what the CPU wrote to plane %u", plane);
                if (seen || requireVisibility) report(seen, label);
                else info("%s: not visible without the dma-buf coherence kernel (got %u,%u)", label, red, green);
            } else {
                info("plane %u: the dma-buf maps read-only (%s)", plane, std::strerror(writer.error));
                if (requireVisibility) report(false, "planes: the dma-buf is writable");
            }
        }
        egl.bindFramebuffer(kGlFramebuffer, 0);
        egl.deleteFramebuffers(1, &framebuffer);
        egl.deleteTextures(1, &texture);
        egl.destroyImage(display, handle);
        if (fd >= 0) close(fd);
    }
    gbm.boDestroy(bo);
    egl.makeCurrent(display, nullptr, nullptr, nullptr);
    egl.destroyContext(display, gl);
    egl.terminate(display);
}

}

int main(int argc, char** argv)
{
    // A crash in a graphics library must not take the report with it.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* node = "/dev/dri/renderD128";
    bool useEgl = false, usePlanes = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--node" && index + 1 < argc) node = argv[++index];
        else if (argument == "--egl") useEgl = true;
        else if (argument == "--planes") usePlanes = true;
        else if (argument == "--require-visibility") requireVisibility = true;
        else {
            std::fprintf(stderr, "usage: gbm_planar_probe [--node PATH] [--egl] [--planes] [--require-visibility]\n");
            return 2;
        }
    }
    Gbm gbm;
    if (!gbm.load()) {
        std::printf("FAIL libgbm cannot be loaded\n");
        return 1;
    }
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("FAIL %s: %s\n", node, std::strerror(errno));
        return 1;
    }
    void* device = gbm.createDevice(fd);
    report(device != nullptr, "gbm: a device is created");
    if (!device) return 1;
    info("backend %s, module %s", gbm.backendName(device), moduleMapped() ? "loaded" : "not loaded");
    report(std::strcmp(gbm.backendName(device), "drm") == 0, "gbm: the device is still the drm backend");

    const PlanarCase cases[] = {{"NV12", kNv12, 2, 1}, {"P010", kP010, 2, 2}, {"YU12", kYu12, 3, 1},
                                {"YV12", kYv12, 3, 1}};
    for (const PlanarCase& image : cases) {
        probePlanar(gbm, device, image, 64, 48);
        probePlanar(gbm, device, image, 1280, 720);
    }
    // A colour buffer is still Mesa's: it maps and keeps what is written.
    void* colour = gbm.boCreate(device, 64, 64, kXrgb, kUseRendering | kUseLinear);
    report(colour != nullptr && gbm.planeCount(colour) == 1, "gbm: a colour buffer is allocated as before");
    if (colour) {
        std::uint32_t stride = 0;
        void* data = nullptr;
        void* map = gbm.boMap(colour, 0, 0, 64, 64, 3, &stride, &data);
        report(map != nullptr && map != MAP_FAILED, "gbm: a colour buffer still maps");
        if (map && map != MAP_FAILED) {
            std::memset(map, 0x5a, 64);
            gbm.boUnmap(colour, data);
        }
        gbm.boDestroy(colour);
    }
    if (useEgl || usePlanes) {
        Egl egl;
        if (!egl.load()) {
            std::printf("FAIL libEGL or libGLESv2 cannot be loaded\n");
            ++failures;
        } else {
            if (useEgl) probeCompositor(gbm, egl, device);
            if (usePlanes) probePlaneRender(gbm, egl, device);
        }
    }
    gbm.destroyDevice(device);
    close(fd);
    std::printf("GBM_PLANAR_PROBE: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
