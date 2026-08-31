// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "metal_presentation.hpp"
#include "native_display_application.hpp"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace uml::gpu {
namespace {

namespace native_input = uml::input::vnc;
using NativeKey = std::pair<unsigned short, std::uint16_t>;
constexpr std::int64_t resizeSettleNanoseconds =
    150LL * NSEC_PER_MSEC;

// NSEvent keyCode is the physical ANSI keyboard position. Keep the mapping
// independent of the active macOS layout, as the VNC path does for keysyms.
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

std::optional<std::uint32_t> displayDimension(
    CGFloat dimension,
    std::uint32_t maximum) noexcept
{
    if (!std::isfinite(dimension) || dimension < 1.0)
        return std::nullopt;
    return static_cast<std::uint32_t>(std::min<CGFloat>(
        std::floor(dimension), maximum));
}

// Device-dependent modifier bits from IOKit's IOLLEvent.h. NSEvent carries
// them alongside the device-independent flags and they are the only way to
// tell which side of a modifier pair changed: NSEventModifierFlagShift stays
// set while either Shift is held, so releasing one of two held Shifts would
// otherwise be reported as a press and leave that key down in the guest.
constexpr NSEventModifierFlags deviceLeftControl = 0x00000001;
constexpr NSEventModifierFlags deviceLeftShift = 0x00000002;
constexpr NSEventModifierFlags deviceRightShift = 0x00000004;
constexpr NSEventModifierFlags deviceLeftCommand = 0x00000008;
constexpr NSEventModifierFlags deviceRightCommand = 0x00000010;
constexpr NSEventModifierFlags deviceLeftOption = 0x00000020;
constexpr NSEventModifierFlags deviceRightOption = 0x00000040;
constexpr NSEventModifierFlags deviceRightControl = 0x00002000;

NSEventModifierFlags modifierFlag(unsigned short keyCode) noexcept
{
    switch (keyCode) {
    case 56:
        return deviceLeftShift;
    case 60:
        return deviceRightShift;
    case 59:
        return deviceLeftControl;
    case 62:
        return deviceRightControl;
    case 58:
        return deviceLeftOption;
    case 61:
        return deviceRightOption;
    case 55:
        return deviceLeftCommand;
    case 54:
        return deviceRightCommand;
    case 57:
        return NSEventModifierFlagCapsLock;
    default:
        return 0;
    }
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

struct NativeDisplayState final :
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        native_input::VncInputSink sink,
        native_input::PointerMode mode,
        std::uint32_t maximumDimension,
        bool autoResizeGuest)
        : presentation(
              DisplayResizeNotifier::clampDimension(
                  width, maximumDimension),
              DisplayResizeNotifier::clampDimension(
                  height, maximumDimension))
        , resizeNotifier(width, height, maximumDimension)
        , geometryPolicy(
              DisplayResizeNotifier::clampDimension(
                  width, maximumDimension),
              DisplayResizeNotifier::clampDimension(
                  height, maximumDimension),
              autoResizeGuest)
        , inputSink(std::move(sink))
        , pointerMode(mode)
    {
    }

    void scheduleWindow();
    void scheduleResize(NSSize size) noexcept;
    void beginResize() noexcept;
    void commitResize(NSSize size) noexcept;
    void cancelResize() noexcept;
    [[nodiscard]] bool drainResizeEvent();
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    void resetGuestSession() noexcept;
    void scheduleGuestWindowResize(DisplaySize size) noexcept;
    void applyPendingGuestWindowResize() noexcept;
    void requestRedraw() noexcept;
    [[nodiscard]] AspectFitRectangle guestContentRectangle(
        NSSize bounds) const noexcept;
    [[nodiscard]] bool pointerLandedOnGuest(
        NSPoint location,
        NSSize bounds) const noexcept;
    void sendKey(unsigned short nativeCode, std::int32_t value) noexcept;
    void sendLinuxKey(std::uint16_t code, std::int32_t value) noexcept;
    void sendModifier(NSEvent* event) noexcept;
    void releaseHeldInput() noexcept;
    void releasePointerInput() noexcept;
    void releasePointerInputLocked(
        bool suppressActiveGesture = false) noexcept;
    void sendPointer(
        NSPoint location,
        NSSize bounds,
        NSUInteger buttons,
        double horizontalWheel,
        double verticalWheel) noexcept;
    void reportInputFailure() noexcept;
    void reportRenderFailure(const char* message) noexcept;
    void shutdown() noexcept;

    MetalPresentation presentation;
    DisplayResizeNotifier resizeNotifier;
    DisplayGeometryPolicy geometryPolicy;
    native_input::VncInputSink inputSink;
    __weak MTKView* view {};
    NSWindow* window {};
    id windowDelegate;
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    std::atomic_bool inputFailureReported {};
    std::atomic_bool renderFailureReported {};
    std::atomic_bool resizeInProgress {};
    std::atomic_bool resizeCommitPending {};
    std::atomic_uint64_t resizeGeneration {};
    std::mutex resizeLifecycleMutex;
    std::uint32_t resizeTransitionDepth {};
    std::optional<DisplaySize> pendingGuestWindowSize;
    native_input::PointerMode pointerMode;
    std::mutex pointerMutex;
    native_input::DirectTouchContact touchContact;
    NSUInteger deliveredPointerButtons {};
    bool pointerGestureSuppressed {};
    // AppKit stops delivering keyUp and flagsChanged once the window loses key
    // status, so remember what the guest still believes is held and release it
    // at that boundary. Keyboard codes are all below BTN_MISC.
    std::array<bool, 256> heldKeys {};
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
        state->presentation.draw((__bridge void*)view);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)size;
    if (auto state = _state.lock()) {
        state->presentation.drawableSizeChanged();
        if (!view.inLiveResize)
            state->scheduleResize(view.bounds.size);
    }
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

- (BOOL)resignFirstResponder
{
    if (auto state = _state.lock())
        state->releaseHeldInput();
    return [super resignFirstResponder];
}

- (BOOL)pointerLandedOnSurface:(NSEvent*)event
{
    const NSPoint location = [self convertPoint:event.locationInWindow
                                       fromView:nil];
    if (auto state = _state.lock())
        return state->pointerLandedOnGuest(location, self.bounds.size);
    return NO;
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

- (void)mouseMoved:(NSEvent*)event
{
    if ([self pointerLandedOnSurface:event])
        [self sendPointerEvent:event];
}

- (void)mouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendPointerEvent:event]; }

- (void)scrollWheel:(NSEvent*)event
{
    if (![self pointerLandedOnSurface:event])
        return;
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

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    // Command-Tab and any other focus change delivers the modifier release to
    // whichever application took over, so the guest would keep those keys down
    // for the rest of the session.
    if (auto state = _state.lock())
        state->releaseHeldInput();
}

- (void)windowWillStartLiveResize:(NSNotification*)notification
{
    (void)notification;
    if (auto state = _state.lock())
        state->beginResize();
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    NSWindow* window = notification.object;
    if (auto state = _state.lock()) {
        state->commitResize(
            window.contentView == nil ? NSZeroSize :
                window.contentView.bounds.size);
    }
}

- (void)windowWillEnterFullScreen:(NSNotification*)notification
{
    (void)notification;
    if (auto state = _state.lock())
        state->beginResize();
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification
{
    NSWindow* window = notification.object;
    if (auto state = _state.lock()) {
        state->commitResize(
            window.contentView == nil ? NSZeroSize :
                window.contentView.bounds.size);
    }
}

- (void)windowDidFailToEnterFullScreen:(NSWindow*)window
{
    if (auto state = _state.lock()) {
        state->commitResize(
            window.contentView == nil ? NSZeroSize :
                window.contentView.bounds.size);
    }
}

- (void)windowWillExitFullScreen:(NSNotification*)notification
{
    (void)notification;
    if (auto state = _state.lock())
        state->beginResize();
}

- (void)windowDidExitFullScreen:(NSNotification*)notification
{
    NSWindow* window = notification.object;
    if (auto state = _state.lock()) {
        state->commitResize(
            window.contentView == nil ? NSZeroSize :
                window.contentView.bounds.size);
    }
}

- (void)windowDidFailToExitFullScreen:(NSWindow*)window
{
    if (auto state = _state.lock()) {
        state->commitResize(
            window.contentView == nil ? NSZeroSize :
                window.contentView.bounds.size);
    }
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
    presentation.setRedrawCallback([weak = weak_from_this()] {
        if (auto state = weak.lock())
            state->requestRedraw();
    });
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!state->running.load(std::memory_order_acquire))
            return;
        NSApplication* application = NSApplication.sharedApplication;
        const auto hostSize = state->geometryPolicy.settledHostSize();
        const NSRect frame = NSMakeRect(
            0.0,
            0.0,
            static_cast<CGFloat>(hostSize.width),
            static_cast<CGFloat>(hostSize.height));
        UmlNativeMetalView* nativeView = [[UmlNativeMetalView alloc]
            initWithFrame:frame
            device:(__bridge id<MTLDevice>)state->presentation.device()
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
        nativeWindow.contentMinSize = NSMakeSize(
            DisplayResizeNotifier::minimumDimension,
            DisplayResizeNotifier::minimumDimension);
        nativeWindow.contentMaxSize = NSMakeSize(
            state->resizeNotifier.maximumDimension(),
            state->resizeNotifier.maximumDimension());
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
        state->applyPendingGuestWindowResize();
    });
}

void NativeDisplayState::scheduleResize(NSSize size) noexcept
{
    if (resizeInProgress.load(std::memory_order_acquire)) {
        cancelResize();
        return;
    }
    const auto generation =
        resizeGeneration.fetch_add(1, std::memory_order_acq_rel) + 1U;
    const auto width = displayDimension(
        size.width, resizeNotifier.maximumDimension());
    const auto height = displayDimension(
        size.height, resizeNotifier.maximumDimension());
    if (!width || !height)
        return;
    auto weak = weak_from_this();
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, resizeSettleNanoseconds),
        dispatch_get_main_queue(),
        ^{
            if (auto state = weak.lock()) {
                std::lock_guard lock(state->resizeLifecycleMutex);
                MTKView* nativeView = state->view;
                if (state->running.load(std::memory_order_acquire) &&
                    !state->resizeInProgress.load(std::memory_order_acquire) &&
                    state->resizeGeneration.load(std::memory_order_acquire) ==
                        generation &&
                    nativeView != nil && !nativeView.inLiveResize) {
                    state->resizeNotifier.request(*width, *height);
                }
            }
        });
}

void NativeDisplayState::beginResize() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    ++resizeTransitionDepth;
    resizeInProgress.store(true, std::memory_order_release);
    cancelResize();
}

void NativeDisplayState::commitResize(NSSize size) noexcept
{
    {
        std::lock_guard lock(resizeLifecycleMutex);
        cancelResize();
        if (resizeTransitionDepth != 0)
            --resizeTransitionDepth;
        if (resizeTransitionDepth != 0)
            return;
        const auto width = displayDimension(
            size.width, resizeNotifier.maximumDimension());
        const auto height = displayDimension(
            size.height, resizeNotifier.maximumDimension());
        if (width && height && running.load(std::memory_order_acquire)) {
            resizeNotifier.requestCommitted(*width, *height);
            resizeCommitPending.store(
                geometryPolicy.autoResizeGuestEnabled(),
                std::memory_order_release);
        }
        resizeInProgress.store(false, std::memory_order_release);
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->applyPendingGuestWindowResize();
    });
}

void NativeDisplayState::cancelResize() noexcept
{
    resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
}

bool NativeDisplayState::drainResizeEvent()
{
    std::lock_guard lock(resizeLifecycleMutex);
    const auto size = resizeNotifier.drain();
    if (resizeInProgress.load(std::memory_order_acquire))
        return false;
    const bool changed = geometryPolicy.hostSizeSettled(
        size.width, size.height);
    resizeCommitPending.store(false, std::memory_order_release);
    return changed;
}

bool NativeDisplayState::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    bool followGuest = false;
    bool publishFrame = false;
    {
        std::lock_guard pointerLock(pointerMutex);
        const auto active = geometryPolicy.activeGuestSize();
        std::lock_guard lock(resizeLifecycleMutex);
        const auto& framebuffer = presentation.framebuffer();
        const bool presentationChanged =
            framebuffer.width() != width || framebuffer.height() != height;
        if (presentationChanged) {
            if (!presentation.resize(width, height, false))
                return false;
            publishFrame = true;
        }
        if (active.width != width || active.height != height)
            releasePointerInputLocked(true);
        const auto change = geometryPolicy.guestScanoutChanged(width, height);
        if (change.changed) {
            publishFrame = true;
            if (!geometryPolicy.autoResizeGuestEnabled())
                resizeCommitPending.store(false, std::memory_order_release);
            followGuest = change.independent &&
                geometryPolicy.configuredAutoResizeGuest();
        }
    }
    if (publishFrame)
        presentation.publishCpuFrame();
    if (followGuest)
        scheduleGuestWindowResize({width, height});
    return true;
}

void NativeDisplayState::resetGuestSession() noexcept
{
    std::lock_guard pointerLock(pointerMutex);
    releasePointerInputLocked(true);
    std::lock_guard lock(resizeLifecycleMutex);
    const auto latestHost = resizeNotifier.drain();
    (void)geometryPolicy.hostSizeSettled(
        latestHost.width, latestHost.height);
    const auto restored = geometryPolicy.settledHostSize();
    const auto& framebuffer = presentation.framebuffer();
    const DisplaySize presented {
        .width = framebuffer.width(),
        .height = framebuffer.height(),
    };
    const bool presentationReady = presented == restored ||
        presentation.resize(restored.width, restored.height, false);
    geometryPolicy.resetGuestSession(
        presentationReady ? restored : presented);
    pendingGuestWindowSize.reset();
    resizeCommitPending.store(false, std::memory_order_release);
}

void NativeDisplayState::scheduleGuestWindowResize(DisplaySize size) noexcept
{
    {
        std::lock_guard lock(resizeLifecycleMutex);
        if (!running.load(std::memory_order_acquire))
            return;
        pendingGuestWindowSize = size;
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->applyPendingGuestWindowResize();
    });
}

void NativeDisplayState::applyPendingGuestWindowResize() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    NSWindow* nativeWindow = window;
    if (!pendingGuestWindowSize || nativeWindow == nil ||
        !running.load(std::memory_order_acquire) ||
        resizeTransitionDepth != 0 || nativeWindow.inLiveResize ||
        (nativeWindow.styleMask & NSWindowStyleMaskFullScreen) != 0) {
        return;
    }
    const auto size = *pendingGuestWindowSize;
    pendingGuestWindowSize.reset();
    /* An independent guest mode already latched host-to-guest resizing off.
     * Keep the lifecycle lock across this synchronous AppKit adjustment so a
     * reset cannot restore that direction between the decision and the frame
     * change. The drawable callback only schedules its delayed work here. */
    [nativeWindow setContentSize:NSMakeSize(
        static_cast<CGFloat>(size.width),
        static_cast<CGFloat>(size.height))];
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

AspectFitRectangle NativeDisplayState::guestContentRectangle(
    NSSize bounds) const noexcept
{
    const auto guestSize = geometryPolicy.activeGuestSize();
    MTKView* nativeView = view;
    const NSSize drawable = nativeView == nil ? NSZeroSize :
        nativeView.drawableSize;
    const auto fitted = aspectFitRectangle(
        drawable.width,
        drawable.height,
        guestSize.width,
        guestSize.height);
    if (fitted.width <= 0.0 || fitted.height <= 0.0 ||
        bounds.width <= 0.0 || bounds.height <= 0.0 ||
        drawable.width <= 0.0 || drawable.height <= 0.0) {
        return {};
    }
    const double scaleX = bounds.width / drawable.width;
    const double scaleY = bounds.height / drawable.height;
    return {
        .x = fitted.x * scaleX,
        .y = fitted.y * scaleY,
        .width = fitted.width * scaleX,
        .height = fitted.height * scaleY,
    };
}

bool NativeDisplayState::pointerLandedOnGuest(
    NSPoint location,
    NSSize bounds) const noexcept
{
    const auto content = guestContentRectangle(bounds);
    return content.width > 0.0 && content.height > 0.0 &&
        location.x >= content.x &&
        location.x <= content.x + content.width &&
        location.y >= content.y &&
        location.y <= content.y + content.height;
}

void NativeDisplayState::sendKey(
    unsigned short nativeCode,
    std::int32_t value) noexcept
{
    const auto code = linuxKeyCode(nativeCode);
    if (!code)
        return;
    sendLinuxKey(*code, value);
}

void NativeDisplayState::sendLinuxKey(
    std::uint16_t code,
    std::int32_t value) noexcept
{
    if (!inputSink)
        return;
    if (code < heldKeys.size())
        heldKeys[code] = value != 0;
    const std::array events {
        native_input::VncInputEvent {
            .type = native_input::eventKey,
            .code = code,
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

void NativeDisplayState::releaseHeldInput() noexcept
{
    for (std::size_t code = 0; code < heldKeys.size(); ++code) {
        if (heldKeys[code])
            sendLinuxKey(static_cast<std::uint16_t>(code), 0);
    }
    releasePointerInput();
}

void NativeDisplayState::releasePointerInput() noexcept
{
    std::lock_guard lock(pointerMutex);
    releasePointerInputLocked();
}

void NativeDisplayState::releasePointerInputLocked(
    bool suppressActiveGesture) noexcept
{
    if (suppressActiveGesture && deliveredPointerButtons != 0)
        pointerGestureSuppressed = true;
    else if (!suppressActiveGesture)
        pointerGestureSuppressed = false;
    // Pointer contacts live above the tracked keyboard range, so clear them
    // explicitly when the window loses focus.
    if (pointerMode == native_input::PointerMode::touchscreen) {
        if (touchContact.release())
            sendLinuxKey(native_input::buttonTouch, 0);
    } else {
        sendLinuxKey(native_input::buttonLeft, 0);
        sendLinuxKey(native_input::buttonMiddle, 0);
        sendLinuxKey(native_input::buttonRight, 0);
    }
    deliveredPointerButtons = 0;
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
    sendKey(event.keyCode, (event.modifierFlags & flag) != 0 ? 1 : 0);
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
    std::lock_guard lock(pointerMutex);
    const auto content = guestContentRectangle(bounds);
    if (content.width <= 0.0 || content.height <= 0.0)
        return;
    const bool inside = location.x >= content.x &&
        location.x <= content.x + content.width &&
        location.y >= content.y &&
        location.y <= content.y + content.height;
    const NSUInteger pointerButtons = buttons & 7U;
    if (pointerGestureSuppressed) {
        if (pointerButtons == 0)
            pointerGestureSuppressed = false;
        return;
    }
    if (!inside && deliveredPointerButtons == 0) {
        if (pointerButtons != 0)
            pointerGestureSuppressed = true;
        return;
    }
    const bool pressed = (buttons & 1U) != 0;
    if (pointerMode == native_input::PointerMode::touchscreen &&
        !touchContact.update(pressed))
        return;
    deliveredPointerButtons =
        pointerMode == native_input::PointerMode::touchscreen ?
            (pressed ? 1U : 0U) : pointerButtons;
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
        native_input::normalizedAbsoluteCoordinate(
            location.x - content.x, content.width));
    append(
        native_input::eventAbsolute,
        native_input::absoluteY,
        native_input::normalizedAbsoluteCoordinate(
            content.y + content.height - location.y,
            content.height));
    if (pointerMode == native_input::PointerMode::touchscreen) {
        append(
            native_input::eventKey,
            native_input::buttonTouch,
            pressed ? 1 : 0);
    } else {
        append(
            native_input::eventKey,
            native_input::buttonLeft,
            pressed ? 1 : 0);
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
    {
        std::lock_guard lock(resizeLifecycleMutex);
        resizeInProgress.store(true, std::memory_order_release);
        cancelResize();
    }
    presentation.shutdown();
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
    void*,
    native_input::PointerMode pointerMode,
    std::uint32_t maximumDimension,
    bool autoResizeGuest)
    : state_(std::make_shared<NativeDisplayState>(
          width,
          height,
          std::move(inputSink),
          pointerMode,
          maximumDimension,
          autoResizeGuest))
{
    state_->scheduleWindow();
}

NativeDisplay::~NativeDisplay()
{
    state_->shutdown();
}

std::uint32_t NativeDisplay::width() const noexcept
{
    return state_->geometryPolicy.advertisedSize().width;
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->geometryPolicy.advertisedSize().height;
}

int NativeDisplay::resizeEventDescriptor() const noexcept
{
    return state_->resizeNotifier.descriptor();
}

bool NativeDisplay::drainResizeEvent()
{
    return state_->drainResizeEvent();
}

bool NativeDisplay::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return state_->guestScanoutChanged(width, height);
}

void NativeDisplay::resetGuestSession() noexcept
{
    state_->resetGuestSession();
}

bool NativeDisplay::claimResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.lock();
    if (state_->resizeInProgress.load(std::memory_order_acquire) ||
        state_->resizeCommitPending.load(std::memory_order_acquire) ||
        !state_->geometryPolicy.autoResizeGuestEnabled()) {
        state_->resizeLifecycleMutex.unlock();
        return false;
    }
    return true;
}

void NativeDisplay::releaseResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.unlock();
}

void NativeDisplay::clear() noexcept
{
    state_->presentation.framebuffer().clear();
    state_->presentation.publishCpuFrame();
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
        static_cast<std::size_t>(sourceWidth) * protocol::gpuBytesPerPixel,
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
    const auto rectangle = state_->presentation.framebuffer().update(
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
        state_->presentation.publishCpuFrame();
}

void NativeDisplay::fillSelfTestPattern()
{
    state_->presentation.framebuffer().fillSelfTestPattern();
    state_->presentation.publishCpuFrame();
}

PresentOutcome NativeDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    return state_->presentation.present(scanout);
}

DisplayStatistics NativeDisplay::statistics() const noexcept
{
    return state_->presentation.statistics();
}

void NativeDisplay::recordCpuFallbackFrame() noexcept
{
    state_->presentation.recordCpuFallbackFrame();
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
