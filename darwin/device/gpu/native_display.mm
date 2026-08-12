// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "native_display_application.hpp"
#include "scanout_framebuffer.hpp"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace uml::gpu {
namespace {

namespace native_input = uml::input::vnc;

NSString* const metalShaderSource = @R"metal(
#include <metal_stdlib>
using namespace metal;

struct RasterData {
    float4 position [[position]];
    float2 textureCoordinate;
};

vertex RasterData displayVertex(unsigned int vertexId [[vertex_id]])
{
    const float2 positions[] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    const float2 coordinates[] = {
        float2(0.0,  1.0),
        float2(2.0,  1.0),
        float2(0.0, -1.0),
    };
    RasterData output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.textureCoordinate = coordinates[vertexId];
    return output;
}

fragment float4 displayFragment(
    RasterData input [[stage_in]],
    texture2d<float> scanout [[texture(0)]])
{
    constexpr sampler textureSampler(
        mag_filter::linear,
        min_filter::linear,
        address::clamp_to_edge);
    const float4 color = scanout.sample(
        textureSampler, input.textureCoordinate);
    return float4(color.rgb, 1.0);
}
)metal";

using NativeKey = std::pair<unsigned short, std::uint16_t>;

/* NSEvent keyCode is the physical ANSI keyboard position. Keep the mapping
 * independent of the active macOS layout, just as the VNC path maps keysyms
 * back to Linux physical input codes. */
constexpr std::array nativeKeys {
    NativeKey {0, 30}, NativeKey {1, 31}, NativeKey {2, 32},
    NativeKey {3, 33}, NativeKey {4, 35}, NativeKey {5, 34},
    NativeKey {6, 44}, NativeKey {7, 45}, NativeKey {8, 46},
    NativeKey {9, 47}, NativeKey {11, 48}, NativeKey {12, 16},
    NativeKey {13, 17}, NativeKey {14, 18}, NativeKey {15, 19},
    NativeKey {16, 21}, NativeKey {17, 20}, NativeKey {18, 2},
    NativeKey {19, 3}, NativeKey {20, 4}, NativeKey {21, 5},
    NativeKey {22, 7}, NativeKey {23, 6}, NativeKey {24, 13},
    NativeKey {25, 10}, NativeKey {26, 8}, NativeKey {27, 12},
    NativeKey {28, 9}, NativeKey {29, 11}, NativeKey {30, 27},
    NativeKey {31, 24}, NativeKey {32, 22}, NativeKey {33, 26},
    NativeKey {34, 23}, NativeKey {35, 25}, NativeKey {36, 28},
    NativeKey {37, 38}, NativeKey {38, 36}, NativeKey {39, 40},
    NativeKey {40, 37}, NativeKey {41, 39}, NativeKey {42, 43},
    NativeKey {43, 51}, NativeKey {44, 53}, NativeKey {45, 49},
    NativeKey {46, 50}, NativeKey {47, 52}, NativeKey {48, 15},
    NativeKey {49, 57}, NativeKey {50, 41}, NativeKey {51, 14},
    NativeKey {53, 1}, NativeKey {54, 126}, NativeKey {55, 125},
    NativeKey {56, 42}, NativeKey {57, 58}, NativeKey {58, 56},
    NativeKey {59, 29}, NativeKey {60, 54}, NativeKey {61, 100},
    NativeKey {62, 97}, NativeKey {65, 83}, NativeKey {67, 55},
    NativeKey {69, 78}, NativeKey {71, 69}, NativeKey {75, 98},
    NativeKey {76, 96}, NativeKey {78, 74}, NativeKey {81, 117},
    NativeKey {82, 82}, NativeKey {83, 79}, NativeKey {84, 80},
    NativeKey {85, 81}, NativeKey {86, 75}, NativeKey {87, 76},
    NativeKey {88, 77}, NativeKey {89, 71}, NativeKey {91, 72},
    NativeKey {92, 73}, NativeKey {96, 63}, NativeKey {97, 64},
    NativeKey {98, 65}, NativeKey {99, 61}, NativeKey {100, 66},
    NativeKey {101, 67}, NativeKey {103, 87}, NativeKey {109, 68},
    NativeKey {111, 88}, NativeKey {114, 110}, NativeKey {115, 102},
    NativeKey {116, 104}, NativeKey {117, 111}, NativeKey {118, 62},
    NativeKey {119, 107}, NativeKey {120, 60}, NativeKey {121, 109},
    NativeKey {122, 59}, NativeKey {123, 105}, NativeKey {124, 106},
    NativeKey {125, 108}, NativeKey {126, 103},
};

std::optional<std::uint16_t> linuxKeyCode(unsigned short keyCode) noexcept
{
    for (const auto& [native, linux] : nativeKeys)
        if (native == keyCode)
            return linux;
    return std::nullopt;
}

NSEventModifierFlags modifierFlag(unsigned short keyCode) noexcept
{
    switch (keyCode) {
    case 56:
    case 60:
        return NSEventModifierFlagShift;
    case 59:
    case 62:
        return NSEventModifierFlagControl;
    case 58:
    case 61:
        return NSEventModifierFlagOption;
    case 54:
    case 55:
        return NSEventModifierFlagCommand;
    case 57:
        return NSEventModifierFlagCapsLock;
    default:
        return 0;
    }
}

std::string description(NSError* error)
{
    if (error == nil || error.localizedDescription == nil)
        return "unknown Metal error";
    const char* text = error.localizedDescription.UTF8String;
    return text == nullptr ? "unknown Metal error" : std::string(text);
}

void stopApplication() noexcept
{
    NSApplication* application = NSApplication.sharedApplication;
    [application stop:nil];
    NSEvent* wake = [NSEvent
        otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint
        modifierFlags:0
        timestamp:0
        windowNumber:0
        context:nil
        subtype:0
        data1:0
        data2:0];
    [application postEvent:wake atStart:NO];
}

class ScopedSignalMask final {
public:
    ScopedSignalMask()
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        const int error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous_);
        if (error != 0) {
            throw std::system_error(
                error,
                std::generic_category(),
                "blocking the UML timer on the AppKit thread");
        }
        active_ = true;
    }

    ScopedSignalMask(const ScopedSignalMask&) = delete;
    ScopedSignalMask& operator=(const ScopedSignalMask&) = delete;

    ~ScopedSignalMask()
    {
        if (active_)
            (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

    [[nodiscard]] const sigset_t& previous() const noexcept
    {
        return previous_;
    }

private:
    sigset_t previous_ {};
    bool active_ {};
};

} // namespace

enum class SurfaceState {
    available,
    writing,
    ready,
    inFlight,
};

struct SurfaceSlot {
    IOSurfaceRef surface {};
    id<MTLTexture> metalTexture;
    GLuint openGlTexture {};
    SurfaceState state {SurfaceState::available};
    std::uint64_t generation {};

    ~SurfaceSlot()
    {
        if (surface != nullptr)
            CFRelease(surface);
    }
};

struct NativeDisplayState final :
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        native_input::VncInputSink sink)
        : framebuffer(width, height)
        , inputSink(std::move(sink))
    {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            throw std::runtime_error("Metal has no default GPU device");
        commandQueue = [device newCommandQueue];
        if (commandQueue == nil)
            throw std::runtime_error("Metal command queue creation failed");

        NSError* error = nil;
        id<MTLLibrary> library = [device
            newLibraryWithSource:metalShaderSource
            options:nil
            error:&error];
        if (library == nil) {
            throw std::runtime_error(
                "Metal display shader compilation failed: " +
                description(error));
        }
        id<MTLFunction> vertex = [library newFunctionWithName:@"displayVertex"];
        id<MTLFunction> fragment =
            [library newFunctionWithName:@"displayFragment"];
        if (vertex == nil || fragment == nil)
            throw std::runtime_error("Metal display shader entry is missing");
        MTLRenderPipelineDescriptor* descriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = [device
            newRenderPipelineStateWithDescriptor:descriptor
            error:&error];
        if (pipeline == nil) {
            throw std::runtime_error(
                "Metal display pipeline creation failed: " +
                description(error));
        }

        MTLTextureDescriptor* textureDescriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                width:width
                height:height
                mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        fallbackTexture = [device newTextureWithDescriptor:textureDescriptor];
        if (fallbackTexture == nil)
            throw std::runtime_error("Metal scanout texture creation failed");

        for (auto& slot : surfaces) {
            const auto rowBytes = IOSurfaceAlignProperty(
                kIOSurfaceBytesPerRow,
                static_cast<std::size_t>(width) * 4U);
            const auto allocationSize = IOSurfaceAlignProperty(
                kIOSurfaceAllocSize,
                rowBytes * static_cast<std::size_t>(height));
            NSDictionary* properties = @{
                (id)kIOSurfaceWidth: @(width),
                (id)kIOSurfaceHeight: @(height),
                (id)kIOSurfaceBytesPerElement: @4,
                (id)kIOSurfaceBytesPerRow: @(rowBytes),
                (id)kIOSurfaceAllocSize: @(allocationSize),
                (id)kIOSurfacePixelFormat: @(0x42475241U),
            };
            slot.surface = IOSurfaceCreate(
                (__bridge CFDictionaryRef)properties);
            if (slot.surface == nullptr)
                throw std::runtime_error("IOSurface scanout creation failed");
            slot.metalTexture = [device
                newTextureWithDescriptor:textureDescriptor
                iosurface:slot.surface
                plane:0];
            if (slot.metalTexture == nil)
                throw std::runtime_error(
                    "Metal IOSurface texture creation failed");
        }
    }

    void scheduleWindow();
    void requestRedraw() noexcept;
    void publishCpuFrame() noexcept;
    [[nodiscard]] bool presentOpenGl(const OpenGlScanout& scanout) noexcept;
    [[nodiscard]] bool copyOpenGlToSurface(
        SurfaceSlot& slot, const OpenGlScanout& scanout) noexcept;
    void draw(MTKView* view) noexcept;
    void finishPresentation(std::optional<std::size_t> slot) noexcept;
    void sendKey(unsigned short nativeCode, std::int32_t value) noexcept;
    void sendModifier(NSEvent* event) noexcept;
    void sendPointer(
        NSPoint location,
        NSSize bounds,
        NSUInteger buttons,
        double horizontalWheel,
        double verticalWheel) noexcept;
    void reportInputFailure() noexcept;
    void reportRenderFailure(const char* message) noexcept;
    void shutdown() noexcept;

    ScanoutFramebuffer framebuffer;
    native_input::VncInputSink inputSink;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> fallbackTexture;
    std::array<SurfaceSlot, 2> surfaces;
    std::mutex presentationMutex;
    std::uint64_t nextGeneration {1};
    std::uint64_t cpuGeneration {1};
    bool cpuFrameReady {true};
    bool presentationInFlight {};
    __weak MTKView* view {};
    NSWindow* window {};
    id windowDelegate;
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    std::atomic_bool inputFailureReported {};
    std::atomic_bool renderFailureReported {};
    std::atomic_bool directPathReported {};
};

} // namespace uml::gpu

@interface UmlNativeMetalView : MTKView <MTKViewDelegate>
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                        state:(std::weak_ptr<uml::gpu::NativeDisplayState>)state;
- (void)sendPointerEvent:(NSEvent*)event;
@end

@interface UmlNativeWindowDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
- (instancetype)initWithState:(std::weak_ptr<uml::gpu::NativeDisplayState>)state;
@end

@implementation UmlNativeMetalView {
    std::weak_ptr<uml::gpu::NativeDisplayState> _state;
}

- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                        state:(std::weak_ptr<uml::gpu::NativeDisplayState>)state
{
    self = [super initWithFrame:frame device:device];
    if (self != nil) {
        _state = std::move(state);
        self.delegate = self;
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        self.framebufferOnly = YES;
        self.paused = YES;
        self.enableSetNeedsDisplay = YES;
        self.autoResizeDrawable = YES;
        CAMetalLayer* metalLayer = (CAMetalLayer*)self.layer;
        metalLayer.maximumDrawableCount = 2;
        metalLayer.displaySyncEnabled = YES;
    }
    return self;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)drawInMTKView:(MTKView*)view
{
    if (auto state = _state.lock())
        state->draw(view);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    (void)size;
}

- (void)keyDown:(NSEvent*)event
{
    if (auto state = _state.lock())
        state->sendKey(event.keyCode, event.isARepeat ? 2 : 1);
}

- (void)keyUp:(NSEvent*)event
{
    if (auto state = _state.lock())
        state->sendKey(event.keyCode, 0);
}

- (void)flagsChanged:(NSEvent*)event
{
    if (auto state = _state.lock())
        state->sendModifier(event);
}

- (void)sendPointerEvent:(NSEvent*)event
{
    if (auto state = _state.lock()) {
        const NSPoint location = [self convertPoint:event.locationInWindow
                                           fromView:nil];
        state->sendPointer(
            location,
            self.bounds.size,
            NSEvent.pressedMouseButtons,
            0.0,
            0.0);
    }
}

- (void)mouseDown:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)mouseUp:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)rightMouseDown:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)rightMouseUp:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)otherMouseDown:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)otherMouseUp:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)mouseMoved:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)mouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }

- (void)scrollWheel:(NSEvent*)event
{
    if (auto state = _state.lock()) {
        const NSPoint location = [self convertPoint:event.locationInWindow
                                           fromView:nil];
        state->sendPointer(
            location,
            self.bounds.size,
            NSEvent.pressedMouseButtons,
            event.scrollingDeltaX,
            event.scrollingDeltaY);
    }
}

@end

@implementation UmlNativeWindowDelegate {
    std::weak_ptr<uml::gpu::NativeDisplayState> _state;
}

- (instancetype)initWithState:(std::weak_ptr<uml::gpu::NativeDisplayState>)state
{
    self = [super init];
    if (self != nil)
        _state = std::move(state);
    return self;
}

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (!_state.expired())
        uml::gpu::stopApplication();
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    [sender stop:nil];
    uml::gpu::stopApplication();
    return NSTerminateCancel;
}

@end

namespace uml::gpu {

void NativeDisplayState::scheduleWindow()
{
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!state->running.load(std::memory_order_acquire))
            return;
        NSApplication* application = NSApplication.sharedApplication;
        const NSRect frame = NSMakeRect(
            0.0,
            0.0,
            static_cast<CGFloat>(state->framebuffer.width()),
            static_cast<CGFloat>(state->framebuffer.height()));
        UmlNativeMetalView* nativeView = [[UmlNativeMetalView alloc]
            initWithFrame:frame
            device:state->device
            state:state];
        if (nativeView == nil) {
            state->reportRenderFailure("AppKit Metal view creation failed");
            stopApplication();
            return;
        }
        const NSWindowStyleMask style =
            NSWindowStyleMaskTitled |
            NSWindowStyleMaskClosable |
            NSWindowStyleMaskMiniaturizable |
            NSWindowStyleMaskResizable;
        NSWindow* nativeWindow = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:style
            backing:NSBackingStoreBuffered
            defer:NO];
        if (nativeWindow == nil) {
            state->reportRenderFailure("AppKit native window creation failed");
            stopApplication();
            return;
        }
        UmlNativeWindowDelegate* delegate =
            [[UmlNativeWindowDelegate alloc] initWithState:state];
        nativeWindow.title = @"Darwin UML Virtio-GPU";
        nativeWindow.contentView = nativeView;
        nativeWindow.delegate = delegate;
        nativeWindow.acceptsMouseMovedEvents = YES;
        nativeWindow.releasedWhenClosed = NO;
        state->view = nativeView;
        state->window = nativeWindow;
        state->windowDelegate = delegate;
        application.delegate = delegate;
        [nativeWindow center];
        [nativeWindow makeKeyAndOrderFront:nil];
        [nativeWindow makeFirstResponder:nativeView];
        [application activateIgnoringOtherApps:YES];
        [nativeView draw];
    });
}

void NativeDisplayState::requestRedraw() noexcept
{
    if (!running.load(std::memory_order_acquire) ||
        redrawScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->redrawScheduled.store(false, std::memory_order_release);
        if (!state->running.load(std::memory_order_acquire))
            return;
        MTKView* nativeView = state->view;
        if (nativeView != nil)
            [nativeView draw];
    });
}

void NativeDisplayState::publishCpuFrame() noexcept
{
    {
        std::lock_guard lock(presentationMutex);
        cpuGeneration = ++nextGeneration;
        cpuFrameReady = true;
    }
    requestRedraw();
}

bool NativeDisplayState::copyOpenGlToSurface(
    SurfaceSlot& slot,
    const OpenGlScanout& scanout) noexcept
{
    constexpr std::uint32_t pipeTexture2d = 2;
    if (scanout.resourceTarget != pipeTexture2d ||
        scanout.textureId == 0 || scanout.width == 0 ||
        scanout.height == 0 || scanout.width > framebuffer.width() ||
        scanout.height > framebuffer.height() ||
        scanout.sourceX > scanout.sourceWidth ||
        scanout.sourceY > scanout.sourceHeight ||
        scanout.width > scanout.sourceWidth - scanout.sourceX ||
        scanout.height > scanout.sourceHeight - scanout.sourceY) {
        return false;
    }
    CGLContextObj context = CGLGetCurrentContext();
    if (context == nullptr)
        return false;

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture2d = 0;
    GLint previousRectangleTexture = 0;
    GLint previousReadBuffer = 0;
    GLint previousDrawBuffer = 0;
    GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean previousFramebufferSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean previousDither = glIsEnabled(GL_DITHER);
    GLboolean previousColorMask[4] {};
    GLfloat previousClearColor[4] {};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2d);
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &previousRectangleTexture);
    glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
    glGetIntegerv(GL_DRAW_BUFFER, &previousDrawBuffer);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

    GLuint framebuffers[2] {};
    glGenFramebuffers(2, framebuffers);
    bool succeeded = framebuffers[0] != 0 && framebuffers[1] != 0;
    if (succeeded && slot.openGlTexture == 0) {
        glGenTextures(1, &slot.openGlTexture);
        succeeded = slot.openGlTexture != 0;
        if (succeeded) {
            glBindTexture(GL_TEXTURE_RECTANGLE, slot.openGlTexture);
            glTexParameteri(
                GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(
                GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(
                GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(
                GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            succeeded = CGLTexImageIOSurface2D(
                context,
                GL_TEXTURE_RECTANGLE,
                GL_RGBA8,
                static_cast<GLsizei>(framebuffer.width()),
                static_cast<GLsizei>(framebuffer.height()),
                GL_BGRA,
                GL_UNSIGNED_INT_8_8_8_8_REV,
                slot.surface,
                0) == kCGLNoError;
        }
        if (!succeeded && slot.openGlTexture != 0) {
            glDeleteTextures(1, &slot.openGlTexture);
            slot.openGlTexture = 0;
        }
    }
    if (succeeded) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
        glFramebufferTexture2D(
            GL_READ_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            scanout.textureId,
            0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
        glFramebufferTexture2D(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_RECTANGLE,
            slot.openGlTexture,
            0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        succeeded =
            glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE &&
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE;
    }
    if (succeeded) {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        const GLint sourceBottom = static_cast<GLint>(
            scanout.sourceHeight - scanout.sourceY - scanout.height);
        const GLint sourceTop = static_cast<GLint>(
            scanout.sourceHeight - scanout.sourceY);
        const GLint destinationBottom = static_cast<GLint>(
            framebuffer.height() - scanout.height);
        glBlitFramebuffer(
            static_cast<GLint>(scanout.sourceX),
            sourceBottom,
            static_cast<GLint>(scanout.sourceX + scanout.width),
            sourceTop,
            0,
            destinationBottom,
            static_cast<GLint>(scanout.width),
            static_cast<GLint>(framebuffer.height()),
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST);
        glFlush();
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    glReadBuffer(previousReadBuffer);
    glDrawBuffer(previousDrawBuffer);
    glBindTexture(GL_TEXTURE_2D, previousTexture2d);
    glBindTexture(GL_TEXTURE_RECTANGLE, previousRectangleTexture);
    glActiveTexture(previousActiveTexture);
    if (previousScissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    if (previousFramebufferSrgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    else
        glDisable(GL_FRAMEBUFFER_SRGB);
    if (previousDither)
        glEnable(GL_DITHER);
    else
        glDisable(GL_DITHER);
    glColorMask(
        previousColorMask[0], previousColorMask[1],
        previousColorMask[2], previousColorMask[3]);
    glClearColor(
        previousClearColor[0], previousClearColor[1],
        previousClearColor[2], previousClearColor[3]);
    glDeleteFramebuffers(2, framebuffers);
    return succeeded;
}

bool NativeDisplayState::presentOpenGl(
    const OpenGlScanout& scanout) noexcept
{
    try {
        std::size_t selected = surfaces.size();
        {
            std::lock_guard lock(presentationMutex);
            for (std::size_t index = 0; index < surfaces.size(); ++index) {
                if (surfaces[index].state == SurfaceState::available) {
                    selected = index;
                    break;
                }
                if (surfaces[index].state == SurfaceState::ready)
                    selected = index;
            }
            if (selected == surfaces.size())
                return false;
            surfaces[selected].state = SurfaceState::writing;
        }
        if (!copyOpenGlToSurface(surfaces[selected], scanout)) {
            std::lock_guard lock(presentationMutex);
            surfaces[selected].state = SurfaceState::available;
            return false;
        }
        if (!directPathReported.exchange(true, std::memory_order_relaxed)) {
            std::fputs(
                "Native display: direct CGL/IOSurface/Metal presentation\n",
                stderr);
        }
        {
            std::lock_guard lock(presentationMutex);
            surfaces[selected].generation = ++nextGeneration;
            surfaces[selected].state = SurfaceState::ready;
        }
        requestRedraw();
        return true;
    } catch (...) {
        reportRenderFailure("OpenGL-to-IOSurface presentation failed");
        return false;
    }
}

void NativeDisplayState::finishPresentation(
    std::optional<std::size_t> slot) noexcept
{
    bool pending = false;
    {
        std::lock_guard lock(presentationMutex);
        if (slot)
            surfaces[*slot].state = SurfaceState::available;
        presentationInFlight = false;
        pending = cpuFrameReady;
        for (const auto& surface : surfaces)
            pending = pending || surface.state == SurfaceState::ready;
    }
    if (pending)
        requestRedraw();
}

void NativeDisplayState::draw(MTKView* nativeView) noexcept
{
    std::optional<std::size_t> selectedSlot;
    bool presentationClaimed = false;
    @autoreleasepool {
        try {
            MTLRenderPassDescriptor* pass =
                nativeView.currentRenderPassDescriptor;
            id<CAMetalDrawable> drawable = nativeView.currentDrawable;
            if (pass == nil || drawable == nil)
                return;
            id<MTLTexture> sourceTexture = nil;
            {
                std::lock_guard lock(presentationMutex);
                if (presentationInFlight)
                    return;
                std::uint64_t newest = cpuFrameReady ? cpuGeneration : 0;
                for (std::size_t index = 0; index < surfaces.size(); ++index) {
                    if (surfaces[index].state == SurfaceState::ready &&
                        surfaces[index].generation > newest) {
                        newest = surfaces[index].generation;
                        selectedSlot = index;
                    }
                }
                if (newest == 0)
                    return;
                if (selectedSlot) {
                    auto& selected = surfaces[*selectedSlot];
                    selected.state = SurfaceState::inFlight;
                    sourceTexture = selected.metalTexture;
                } else {
                    sourceTexture = fallbackTexture;
                    cpuFrameReady = false;
                }
                for (std::size_t index = 0; index < surfaces.size(); ++index) {
                    if ((!selectedSlot || index != *selectedSlot) &&
                        surfaces[index].state == SurfaceState::ready &&
                        surfaces[index].generation <= newest) {
                        surfaces[index].state = SurfaceState::available;
                    }
                }
                if (cpuFrameReady && cpuGeneration <= newest)
                    cpuFrameReady = false;
                presentationInFlight = true;
                presentationClaimed = true;
            }
            if (!selectedSlot) {
                std::lock_guard lock(framebuffer);
                const MTLRegion region = MTLRegionMake2D(
                    0, 0, framebuffer.width(), framebuffer.height());
                [fallbackTexture
                    replaceRegion:region mipmapLevel:0
                    withBytes:framebuffer.pixels().data()
                    bytesPerRow:framebuffer.rowBytes()];
            }
            id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
            if (buffer == nil) {
                reportRenderFailure("Metal command buffer creation failed");
                finishPresentation(selectedSlot);
                presentationClaimed = false;
                return;
            }
            id<MTLRenderCommandEncoder> encoder =
                [buffer renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil) {
                reportRenderFailure("Metal render encoder creation failed");
                finishPresentation(selectedSlot);
                presentationClaimed = false;
                return;
            }
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:sourceTexture atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:3];
            [encoder endEncoding];
            [buffer presentDrawable:drawable];
            auto state = shared_from_this();
            [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                state->finishPresentation(selectedSlot);
            }];
            [buffer commit];
            presentationClaimed = false;
        } catch (...) {
            if (presentationClaimed)
                finishPresentation(selectedSlot);
            reportRenderFailure("unexpected native display rendering failure");
        }
    }
}

void NativeDisplayState::sendKey(
    unsigned short nativeCode,
    std::int32_t value) noexcept
{
    if (!inputSink)
        return;
    const auto code = linuxKeyCode(nativeCode);
    if (!code)
        return;
    const std::array events {
        native_input::VncInputEvent {
            .type = native_input::eventKey,
            .code = *code,
            .value = value,
        },
        native_input::VncInputEvent {
            .type = native_input::eventSync,
            .code = native_input::syncReport,
            .value = 0,
        },
    };
    if (!native_input::tryDeliver(inputSink, events))
        reportInputFailure();
}

void NativeDisplayState::sendModifier(NSEvent* event) noexcept
{
    const auto flag = modifierFlag(event.keyCode);
    if (flag == 0)
        return;
    if (event.keyCode == 57) {
        sendKey(event.keyCode, 1);
        sendKey(event.keyCode, 0);
        return;
    }
    sendKey(
        event.keyCode,
        (event.modifierFlags & flag) != 0 ? 1 : 0);
}

void NativeDisplayState::sendPointer(
    NSPoint location,
    NSSize bounds,
    NSUInteger buttons,
    double horizontalWheel,
    double verticalWheel) noexcept
{
    if (!inputSink || bounds.width <= 0.0 || bounds.height <= 0.0)
        return;
    const auto scale = [](double coordinate, double extent) {
        const double normalized = std::clamp(coordinate / extent, 0.0, 1.0);
        return static_cast<std::int32_t>(std::lround(
            normalized * native_input::absoluteMaximum));
    };
    std::array<native_input::VncInputEvent, 10> events {};
    std::size_t count = 0;
    const auto append = [&](std::uint16_t type,
                            std::uint16_t code,
                            std::int32_t value) {
        events[count++] = {.type = type, .code = code, .value = value};
    };
    append(
        native_input::eventAbsolute,
        native_input::absoluteX,
        scale(location.x, bounds.width));
    append(
        native_input::eventAbsolute,
        native_input::absoluteY,
        scale(bounds.height - location.y, bounds.height));
    append(
        native_input::eventKey,
        native_input::buttonLeft,
        (buttons & 1U) != 0 ? 1 : 0);
    append(
        native_input::eventKey,
        native_input::buttonMiddle,
        (buttons & 4U) != 0 ? 1 : 0);
    append(
        native_input::eventKey,
        native_input::buttonRight,
        (buttons & 2U) != 0 ? 1 : 0);
    if (verticalWheel != 0.0) {
        append(
            native_input::eventRelative,
            native_input::relativeWheel,
            verticalWheel > 0.0 ? 1 : -1);
    }
    if (horizontalWheel != 0.0) {
        append(
            native_input::eventRelative,
            native_input::relativeHorizontalWheel,
            horizontalWheel > 0.0 ? 1 : -1);
    }
    append(native_input::eventSync, native_input::syncReport, 0);
    if (!native_input::tryDeliver(inputSink, std::span(events).first(count)))
        reportInputFailure();
}

void NativeDisplayState::reportInputFailure() noexcept
{
    if (!inputFailureReported.exchange(true, std::memory_order_relaxed)) {
        std::fputs(
            "Unable to deliver native display input to the emulator\n",
            stderr);
    }
}

void NativeDisplayState::reportRenderFailure(const char* message) noexcept
{
    if (!renderFailureReported.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "Native GPU display failed: %s\n", message);
}

void NativeDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        NSApplication* application = NSApplication.sharedApplication;
        if (application.delegate == state->windowDelegate)
            application.delegate = nil;
        state->window.delegate = nil;
        [state->window orderOut:nil];
        [state->window close];
        state->view = nil;
        state->window = nil;
        state->windowDelegate = nil;
        stopApplication();
    });
}

NativeDisplay::NativeDisplay(
    std::uint32_t width,
    std::uint32_t height,
    native_input::VncInputSink inputSink,
    void*)
    : state_(std::make_shared<NativeDisplayState>(
          width, height, std::move(inputSink)))
{
    state_->scheduleWindow();
}

NativeDisplay::~NativeDisplay()
{
    state_->shutdown();
}

std::uint32_t NativeDisplay::width() const noexcept
{
    return state_->framebuffer.width();
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->framebuffer.height();
}

void NativeDisplay::clear() noexcept
{
    state_->framebuffer.clear();
    state_->publishCpuFrame();
}

void NativeDisplay::update(
    const std::vector<std::uint8_t>& source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    protocol::GpuFormat sourceFormat,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height)
{
    update(
        std::span<const std::uint8_t>(source),
        sourceWidth,
        sourceHeight,
        static_cast<std::size_t>(sourceWidth) *
            protocol::gpuBytesPerPixel,
        sourceFormat,
        x,
        y,
        x,
        y,
        width,
        height);
}

void NativeDisplay::update(
    std::span<const std::uint8_t> source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::size_t sourceRowBytes,
    protocol::GpuFormat sourceFormat,
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint32_t destinationX,
    std::uint32_t destinationY,
    std::uint32_t width,
    std::uint32_t height)
{
    const auto rectangle = state_->framebuffer.update(
        source,
        sourceWidth,
        sourceHeight,
        sourceRowBytes,
        sourceFormat,
        sourceX,
        sourceY,
        destinationX,
        destinationY,
        width,
        height);
    if (rectangle.width != 0 && rectangle.height != 0)
        state_->publishCpuFrame();
}

void NativeDisplay::fillSelfTestPattern()
{
    state_->framebuffer.fillSelfTestPattern();
    state_->publishCpuFrame();
}

bool NativeDisplay::presentOpenGl(const OpenGlScanout& scanout) noexcept
{
    return state_->presentOpenGl(scanout);
}

bool NativeDisplay::presentMetal(const MetalScanout&) noexcept
{
    return false;
}

int runNativeDisplayApplication(
    const std::function<int()>& run,
    const std::function<void()>& requestStop)
{
    if (!run || !requestStop)
        throw std::invalid_argument("native display callbacks are required");
    if (::pthread_main_np() == 0)
        throw std::logic_error("AppKit must run on the process main thread");

    ScopedSignalMask mainThreadSignalMask;
    NSApplication* application = NSApplication.sharedApplication;
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];

    std::atomic_bool finished {};
    int result = 1;
    std::exception_ptr failure;
    const sigset_t workerMask = mainThreadSignalMask.previous();
    std::thread worker([&] {
        (void)::pthread_sigmask(SIG_SETMASK, &workerMask, nullptr);
        try {
            result = run();
        } catch (...) {
            failure = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
        dispatch_async(dispatch_get_main_queue(), ^{
            stopApplication();
        });
    });

    [application run];
    if (!finished.load(std::memory_order_acquire))
        requestStop();
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    return result;
}

} // namespace uml::gpu
