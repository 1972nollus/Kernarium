// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumRuntime.h"
#include "KernariumAquarium.hpp"
#include "KernariumCtsCampaigns.hpp"

#include "audio/in_process_service.hpp"
#include "audio/protocol.hpp"
#include "gpu/in_process_service.hpp"
#include "gpu/protocol.hpp"
#include "input/virtio/in_process_service.hpp"
#include "uml/emulator.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#if DARWIN_UML_SIGNED_KERNEL_CARRIER
extern const unsigned char uml_elf_start[]
    __asm("section$start$__TEXT$__uml_elf");
extern const unsigned char uml_elf_end[]
    __asm("section$end$__TEXT$__uml_elf");
#else
extern const unsigned char uml_elf_start[]
    __asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
    __asm("section$end$__DATA$__uml_elf");
#endif
#if KERNARIUM_RUNTIME_ANDROID_GUEST
/* The accepted release identity is fixed at link time, so a bundle whose
 * payload was produced by a different release is refused before any of it is
 * mapped. */
extern const unsigned char uml_android_manifest_start[]
    __asm("section$start$__DATA$__uml_android");
extern const unsigned char uml_android_manifest_end[]
    __asm("section$end$__DATA$__uml_android");
#endif
int uml_slirp_net_open(void*, unsigned char[6], unsigned int*);
int uml_slirp_net_irq_fd(void*);
int uml_slirp_net_read(void*, void*, std::size_t);
int uml_slirp_net_write(void*, const void*, std::size_t);
void uml_slirp_net_stop(void*);
void* uml_slirp_net_create(void) noexcept;
void uml_slirp_net_destroy(void*) noexcept;
}

namespace {

[[noreturn]] void guestTimerMaskFailure() noexcept
{
    static constexpr char message[] =
        "Kernarium: unable to isolate the UML timer signal\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    _exit(127);
}

void blockGuestTimer(sigset_t* previous) noexcept
{
    sigset_t blocked;
    if (sigemptyset(&blocked) != 0 ||
        sigaddset(&blocked, SIGALRM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, previous) != 0)
        guestTimerMaskFailure();
}

class ScopedGuestTimerBlock final {
public:
    ScopedGuestTimerBlock() noexcept
    {
        blockGuestTimer(&previous_);
    }

    ~ScopedGuestTimerBlock()
    {
        if (::pthread_sigmask(SIG_SETMASK, &previous_, nullptr) != 0)
            guestTimerMaskFailure();
    }

    ScopedGuestTimerBlock(const ScopedGuestTimerBlock&) = delete;
    ScopedGuestTimerBlock& operator=(const ScopedGuestTimerBlock&) = delete;

private:
    sigset_t previous_ {};
};

std::atomic_bool runtimeActive;
std::mutex activeEmulatorMutex;
uml_emulator* activeEmulator;
uml::input::virtio::InProcessInputService* activeTouchscreen;
uml::input::virtio::InProcessInputService* activeKeyboard;
class ConsoleBridge;
ConsoleBridge* activeConsoleBridge;
std::mutex consoleOutputCallbackMutex;
kernarium_runtime_console_output_callback consoleOutputCallback;
void* consoleOutputContext;

void publishConsoleOutput(const void* buffer, std::size_t length)
{
    kernarium_runtime_console_output_callback callback;
    void* context;
    {
        std::lock_guard lock(consoleOutputCallbackMutex);
        callback = consoleOutputCallback;
        context = consoleOutputContext;
    }
    if (callback != nullptr)
        callback(context, buffer, length);
}

class AppNetwork final {
public:
    AppNetwork()
        : context_(uml_slirp_net_create())
    {
        if (context_ == nullptr)
            throw std::bad_alloc {};
    }

    ~AppNetwork()
    {
        if (context_ != nullptr) {
            uml_slirp_net_stop(context_);
            uml_slirp_net_destroy(context_);
        }
    }

    AppNetwork(const AppNetwork&) = delete;
    AppNetwork& operator=(const AppNetwork&) = delete;

    [[nodiscard]] uml_emulator_network_callbacks callbacks() noexcept
    {
        return {
            .struct_size = sizeof(uml_emulator_network_callbacks),
            .context = context_,
            .open = uml_slirp_net_open,
            .irq_fd = uml_slirp_net_irq_fd,
            .read = uml_slirp_net_read,
            .write = uml_slirp_net_write,
        };
    }

private:
    void* context_ {};
};

constexpr std::string_view desktopBootstrapPrefix = R"krn(set -eu
fail()
{
    echo "Unable to construct the Kernarium ext4 overlay root: $*" >&2
    exec /bin/sh
}
[ -b /dev/ubdb ] || fail "overlay block device"
mkfs_ext4=$(command -v mkfs.ext4 || true)
[ -n "$mkfs_ext4" ] || fail "ext4 formatter"
"$mkfs_ext4" -F -q -m 0 \
    -E nodiscard,lazy_itable_init=1,lazy_journal_init=1 /dev/ubdb || \
    fail "ext4 format"
mount -t ext4 -o noatime,nosuid,nodev /dev/ubdb /mnt || fail "ext4 mount"
mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root \
    /mnt/state/docker /mnt/state/containerd || fail "overlay workspace"
chmod 0711 /mnt/state/docker /mnt/state/containerd || \
    fail "container state directory modes"
mount -o bind / /mnt/lower || fail "read-only lower bind"
mount -t overlay overlay \
    -o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work \
    /mnt/root || fail "overlay mount"
mount -t devtmpfs devtmpfs /mnt/root/dev || fail "devtmpfs mount"
mkdir -p /mnt/root/.uml-lower || fail "old-root directory"
cd /mnt/root || fail "new-root chdir"
pivot_root . .uml-lower || fail "pivot_root"
rm -f /etc/resolv.conf
printf '%s\n' \
    '# Darwin UML user-mode network' \
    'nameserver 1.1.1.1' \
    'nameserver 8.8.8.8' \
    'nameserver 10.0.2.3' \
    'options timeout:1 attempts:3 rotate' > /etc/resolv.conf
mkdir -p /var/lib/docker
mount -o bind /.uml-lower/mnt/state/docker /var/lib/docker || \
    fail "Docker state bind"
mkdir -p /var/lib/containerd
mount -o bind /.uml-lower/mnt/state/containerd /var/lib/containerd || \
    fail "containerd state bind"
state_device=$(stat -c %d /.uml-lower/mnt)
[ -n "$state_device" ] || fail "state block-device discovery"
[ "$(stat -c %d /var/lib/docker)" = "$state_device" ] || \
    fail "Docker state is outside the shared ext4 filesystem"
[ "$(stat -c %d /var/lib/containerd)" = "$state_device" ] || \
    fail "containerd state is outside the shared ext4 filesystem"
echo "Kernarium shared ext4 state: device=$state_device"
if getent passwd uml >/dev/null 2>&1 && \
    [ -f /usr/share/plasma/plasmoids/org.kde.desktopcontainment/contents/config/main.xml ]; then
install -d -m 0755 /usr/local/libexec \
    /etc/systemd/system/multi-user.target.wants \
    /etc/systemd/system/getty.target.wants
# The image enables getty@tty0. /dev/tty0 is the *active* VT alias rather than
# a fixed console, so once SDDM logs "Jumping to VT 2" that getty follows the
# compositor onto VT 2 and both claim the same VT; the Wayland session then
# dies immediately with no output. Relocate the prompt to a fixed VT: tty1
# keeps the pre-desktop text console boot leaves on VT 1 and never collides
# with SDDM's VT. This runs before systemd starts, so plain symlinks suffice.
ln -sf /dev/null /etc/systemd/system/getty@tty0.service
ln -sf /usr/lib/systemd/system/getty@.service \
    /etc/systemd/system/getty.target.wants/getty@tty1.service
cat > /usr/local/libexec/kernarium-kde-setup <<'KDE_SETUP'
#!/bin/sh
set -eu
getent passwd uml >/dev/null
usermod -a -G video,render,input uml
install -d -m 0755 -o uml -g uml \
    /home/uml/Desktop /home/uml/Documents /home/uml/Downloads \
    /home/uml/Music /home/uml/Pictures /home/uml/Public \
    /home/uml/Templates /home/uml/Videos /home/uml/.config \
    /home/uml/.local/share/pki \
    /home/uml/.local/share/Trash/files \
    /home/uml/.local/share/Trash/info \
    /home/uml/.local/share/sddm
printf '%s\n' \
    'XDG_DESKTOP_DIR="$HOME/Desktop"' \
    'XDG_DOWNLOAD_DIR="$HOME/Downloads"' \
    'XDG_TEMPLATES_DIR="$HOME/Templates"' \
    'XDG_PUBLICSHARE_DIR="$HOME/Public"' \
    'XDG_DOCUMENTS_DIR="$HOME/Documents"' \
    'XDG_MUSIC_DIR="$HOME/Music"' \
    'XDG_PICTURES_DIR="$HOME/Pictures"' \
    'XDG_VIDEOS_DIR="$HOME/Videos"' \
    > /home/uml/.config/user-dirs.dirs
chown uml:uml /home/uml/.config/user-dirs.dirs
rm -f /usr/share/vulkan/icd.d/lvp_icd.json
sed -i \
    's#<default>desktop:/</default>#<default>file:///home/uml/Desktop</default>#' \
    /usr/share/plasma/plasmoids/org.kde.desktopcontainment/contents/config/main.xml
install -d -m 0755 /etc/sddm.conf.d
printf '%s\n' '[Autologin]' 'User=uml' 'Session=plasma' 'Relogin=false' \
    > /etc/sddm.conf.d/uml-autologin.conf
printf '%s\n' '[Daemon]' 'Autolock=false' 'LockOnResume=false' 'Timeout=0' \
    > /home/uml/.config/kscreenlockerrc
chown uml:uml /home/uml/.config/kscreenlockerrc
echo "Kernarium KDE overlay setup complete"
KDE_SETUP
chmod 0755 /usr/local/libexec/kernarium-kde-setup
cat > /etc/systemd/system/kernarium-kde-setup.service <<'KDE_SERVICE'
[Unit]
Description=Kernarium disposable KDE guest setup
After=local-fs.target
Before=display-manager.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/kernarium-kde-setup
RemainAfterExit=yes
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
KDE_SERVICE
ln -sf ../kernarium-kde-setup.service \
    /etc/systemd/system/multi-user.target.wants/kernarium-kde-setup.service
elif [ -x /usr/bin/phosh-session ] && \
    getent passwd _greetd >/dev/null 2>&1; then
if getent passwd 1000 >/dev/null 2>&1; then
    phosh_user=$(getent passwd 1000 | cut -d: -f1)
    usermod -a -G video,render,input "$phosh_user"
fi

# Discard the image's fixed 720x1440 Virtual-1 override for both the Phrog
# greeter and the logged-in Phosh session. Phoc then uses the preferred
# physical-pixel mode and effective density from Virtio-GPU EDID throughout.
install -d -m 0755 /etc/phrog /etc/phosh
install -m 0644 /dev/null /etc/phrog/phoc.ini
install -m 0644 /dev/null /etc/phosh/phoc.ini

# The image owns its VT and serial getties plus their login policy. Keep those
# intact. It also enables the mutually exclusive development phosh.service
# beside its display-manager alias; remove only that stale enablement so
# graphical.target follows the stock display-manager -> greetd -> Phrog path.
rm -f /etc/systemd/system/graphical.target.wants/phosh.service
echo "Kernarium Phrog overlay setup complete; image getties preserved, greetd selected"
elif [ -x /sbin/openrc ] && [ -x /usr/bin/sxmo_swayinit.sh ] && \
    [ -x /usr/bin/tinydm-run-session ]; then
    pmos_user=$(getent passwd 10000 | cut -d: -f1)
    [ -n "$pmos_user" ] || fail "postmarketOS user"
    [ -e /var/lib/tinydm/default-session.desktop ] || \
        fail "postmarketOS tinydm session"
    # OpenRC groups UML with containers and consequently suppresses udev's
    # coldplug services. This UML owns its devtmpfs and virtual hardware, so
    # let the image's ordinary udev service classify the pre-existing devices.
    for service in udev udev-trigger udev-settle; do
        sed -i '/^[[:space:]]*keyword -containers[[:space:]]*$/d' \
            "/etc/init.d/$service" || fail "postmarketOS $service policy"
    done
    cat >> /etc/deviceinfo <<'PMOS_DEVICEINFO'
deviceinfo_arch="aarch64"
deviceinfo_name="Kernarium"
deviceinfo_codename="uml"
deviceinfo_manufacturer="Darwin UML"
deviceinfo_tmp_as_tmpfs_size="0"
deviceinfo_swap_size_recommended="0"
deviceinfo_zram_swap_pct="0"
PMOS_DEVICEINFO
    install -d -m 0755 /etc/udev/rules.d
    cat > /etc/udev/rules.d/59-kernarium-input.rules <<'PMOS_INPUT_RULES'
ACTION=="add|change", SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Kernarium Touchscreen", ENV{ID_INPUT}="1", ENV{ID_INPUT_TOUCHSCREEN}="1"
ACTION=="add|change", SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Kernarium Keyboard", ENV{ID_INPUT}="1", ENV{ID_INPUT_KEYBOARD}="1"
ACTION=="add|change", SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Darwin UML VNC Keyboard", ENV{ID_INPUT}="1", ENV{ID_INPUT_KEYBOARD}="1"
ACTION=="add|change", SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Darwin UML VNC Tablet", ENV{ID_INPUT}="1", ENV{ID_INPUT_MOUSE}="1"
PMOS_INPUT_RULES
    cat > /etc/init.d/kernarium-device-access <<'PMOS_DEVICE_ACCESS'
#!/sbin/openrc-run
description="Grant the postmarketOS desktop access to Kernarium devices"

depend() {
    need udev-settle
    before display-manager
}

start() {
    ebegin "Configuring Kernarium display and input access"
    found_drm=0
    for node in /dev/dri/card* /dev/dri/renderD*; do
        [ -e "$node" ] || continue
        chgrp video "$node" || return 1
        chmod 0660 "$node" || return 1
        found_drm=1
    done
    [ "$found_drm" = 1 ] || return 1
    for node in /dev/input/event*; do
        [ -e "$node" ] || continue
        chgrp input "$node" || return 1
        chmod 0660 "$node" || return 1
    done
    eend 0
}
PMOS_DEVICE_ACCESS
    chmod 0755 /etc/init.d/kernarium-device-access
    ln -sf /etc/init.d/kernarium-device-access \
        /etc/runlevels/default/kernarium-device-access
    echo "Kernarium postmarketOS overlay setup complete; image Sxmo/Sway session preserved"
else
    echo "Kernarium desktop setup is owned by the rootfs image"
fi
)krn";

constexpr std::string_view desktopBootstrapSuffix = R"krn(echo "Darwin UML root overlay: lower=read-only, upper=ext4"
if [ -x /lib/systemd/systemd ]; then
    exec /lib/systemd/systemd
fi
if [ -x /usr/lib/systemd/systemd ]; then
    exec /usr/lib/systemd/systemd
fi
if [ -x /sbin/openrc ] && [ -x /sbin/init ]; then
    mount -t proc proc /proc || fail "OpenRC proc mount"
    mount -t sysfs sysfs /sys || fail "OpenRC sysfs mount"
    mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run || \
        fail "OpenRC run mount"
    exec /sbin/init
fi
fail "supported init executable not found"
)krn";

constexpr std::string_view geekbenchBootstrap = R"krn(set -eu
benchmark_fail()
{
    echo "KERNARIUM_GEEKBENCH_ERROR: $*" >&2
    sync
    poweroff -f
}
[ -b /dev/ubdb ] || benchmark_fail "overlay block device is unavailable"
/usr/sbin/mkfs.ext4 -F -q -m 0 \
    -E nodiscard,lazy_itable_init=1,lazy_journal_init=1 /dev/ubdb || \
    benchmark_fail "ext4 format failed"
mount -t ext4 -o noatime,nosuid,nodev /dev/ubdb /mnt || \
    benchmark_fail "ext4 mount failed"
mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root || \
    benchmark_fail "overlay workspace failed"
mount -o bind / /mnt/lower || benchmark_fail "EROFS lower bind failed"
mount -t overlay overlay \
    -o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work \
    /mnt/root || benchmark_fail "overlay mount failed"
mount -t devtmpfs devtmpfs /mnt/root/dev || \
    benchmark_fail "devtmpfs mount failed"
mkdir -p /mnt/root/.uml-lower || benchmark_fail "old-root directory failed"
cd /mnt/root || benchmark_fail "new-root chdir failed"
pivot_root . .uml-lower || benchmark_fail "pivot_root failed"
mount -t proc proc /proc || benchmark_fail "proc mount failed"
mount -t sysfs sysfs /sys || benchmark_fail "sysfs mount failed"
mkdir -p /run /tmp
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run || \
    benchmark_fail "run tmpfs mount failed"
mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp || \
    benchmark_fail "temporary tmpfs mount failed"
benchmark=/opt/geekbench/release/geekbench6
[ -x "$benchmark" ] || benchmark_fail "Geekbench payload is unavailable"
export HOME=/root
echo "KERNARIUM_GEEKBENCH_READY"
set +e
if [ -n "${KERNARIUM_GEEKBENCH_WORKLOADS:-}" ]; then
    echo "KERNARIUM_GEEKBENCH_TARGETS:$KERNARIUM_GEEKBENCH_WORKLOADS"
    "$benchmark" --no-upload --single-core --section 1 \
        --workload "$KERNARIUM_GEEKBENCH_WORKLOADS"
else
    "$benchmark" --no-upload
fi
benchmark_rc=$?
printf '\nKERNARIUM_GEEKBENCH_END:%s\n' "$benchmark_rc"
sync
poweroff -f
exit "$benchmark_rc"
)krn";

enum class ConsoleBootMode {
    desktop,
    aquarium,
    cts,
    geekbench,
    /* Android brings up its own init from the bundle's ramdisk. The relay
     * carries its console in both directions and injects nothing. */
    android,
};

class ActiveRuntime final {
public:
    ActiveRuntime()
    {
        if (runtimeActive.exchange(true, std::memory_order_acq_rel))
            throw std::runtime_error("a Kernarium guest is already active");
    }

    ~ActiveRuntime()
    {
        runtimeActive.store(false, std::memory_order_release);
    }

    ActiveRuntime(const ActiveRuntime&) = delete;
    ActiveRuntime& operator=(const ActiveRuntime&) = delete;
};

class ActiveEmulatorRegistration final {
public:
    explicit ActiveEmulatorRegistration(uml_emulator* emulator)
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeEmulator = emulator;
    }

    ~ActiveEmulatorRegistration()
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeEmulator = nullptr;
    }

    ActiveEmulatorRegistration(const ActiveEmulatorRegistration&) = delete;
    ActiveEmulatorRegistration& operator=(
        const ActiveEmulatorRegistration&) = delete;
};

class ActiveInputRegistration final {
public:
    ActiveInputRegistration(
        uml::input::virtio::InProcessInputService& touchscreen,
        uml::input::virtio::InProcessInputService& keyboard)
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeTouchscreen = &touchscreen;
        activeKeyboard = &keyboard;
    }

    ~ActiveInputRegistration()
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeTouchscreen = nullptr;
        activeKeyboard = nullptr;
    }

    ActiveInputRegistration(const ActiveInputRegistration&) = delete;
    ActiveInputRegistration& operator=(const ActiveInputRegistration&) = delete;
};

class ConsoleBridge final {
public:
    explicit ConsoleBridge(
        ConsoleBootMode bootMode,
        std::string geekbenchWorkloads,
        const kernarium::cts::Campaign* ctsCampaign,
        bool derivativeRootfs)
    {
        if ((bootMode == ConsoleBootMode::cts) != (ctsCampaign != nullptr))
            throw std::invalid_argument("invalid CTS console campaign");
        int descriptors[2] {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "console socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        guest_ = descriptors[0];
        host_ = descriptors[1];
        const int enabled = 1;
        if (setsockopt(
                host_,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)) != 0) {
            const int error = errno;
            close(guest_);
            close(host_);
            guest_ = -1;
            host_ = -1;
            throw std::runtime_error(
                "console socket setup failed: " +
                std::string(std::strerror(error)));
        }
        try {
            relay_ = std::thread([
                this,
                bootMode,
                geekbenchWorkloads = std::move(geekbenchWorkloads),
                ctsCampaign,
                derivativeRootfs] {
                constexpr std::string_view shellReady =
                    "Run /bin/sh as init process";
                bool bootstrapDelivered =
                    bootMode == ConsoleBootMode::android;
                std::string observed;
                char buffer[4096];
                for (;;) {
                    const ssize_t received = recv(
                        host_, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        publishConsoleOutput(
                            buffer, static_cast<std::size_t>(received));
                        std::string_view output(
                            buffer, static_cast<std::size_t>(received));
                        while (!output.empty()) {
                            const ssize_t written = write(
                                STDOUT_FILENO,
                                output.data(),
                                output.size());
                            if (written > 0) {
                                output.remove_prefix(
                                    static_cast<std::size_t>(written));
                            } else if (written < 0 && errno == EINTR) {
                                continue;
                            } else {
                                break;
                            }
                        }
                        if (!bootstrapDelivered) {
                            observed.append(
                                buffer,
                                static_cast<std::size_t>(received));
                            if (observed.size() > 1024U)
                                observed.erase(0, observed.size() - 1024U);
                            if (observed.find(shellReady) != std::string::npos) {
                                std::string bootstrap;
                                if (bootMode == ConsoleBootMode::geekbench) {
                                    if (!geekbenchWorkloads.empty()) {
                                        bootstrap =
                                            "KERNARIUM_GEEKBENCH_WORKLOADS=";
                                        bootstrap.append(geekbenchWorkloads);
                                        bootstrap.push_back('\n');
                                    }
                                    bootstrap.append(geekbenchBootstrap);
                                } else {
                                    bootstrap.append(desktopBootstrapPrefix);
                                    if (bootMode == ConsoleBootMode::aquarium)
                                        bootstrap.append(
                                            kernarium::aquarium::setup);
                                    if (bootMode == ConsoleBootMode::cts)
                                        bootstrap.append(
                                            derivativeRootfs
                                                ? ctsCampaign->ext4Setup
                                                : ctsCampaign->erofsSetup);
                                    bootstrap.append(desktopBootstrapSuffix);
                                }
                                bootstrap.push_back('\n');
                                const int error = sendInput(
                                    bootstrap.data(), bootstrap.size());
                                bootstrapDelivered = error == 0;
                                if (!bootstrapDelivered)
                                    std::fprintf(
                                        stderr,
                                        "Kernarium: console bootstrap write "
                                        "failed: %s\n",
                                        std::strerror(error));
                                if (bootstrapDelivered)
                                    std::fprintf(
                                        stderr,
                                        "Kernarium: %s bootstrap delivered\n",
                                        bootMode == ConsoleBootMode::geekbench
                                            ? "Geekbench"
                                            : bootMode == ConsoleBootMode::cts
                                            ? "CTS"
                                            : bootMode == ConsoleBootMode::aquarium
                                            ? "Aquarium"
                                            : "desktop");
                                observed.clear();
                            }
                        }
                        continue;
                    }
                    if (received < 0 && errno == EINTR)
                        continue;
                    if (received < 0 && errno != ECONNRESET) {
                        std::fprintf(
                            stderr,
                            "Kernarium: console relay failed: %s\n",
                            std::strerror(errno));
                    }
                    break;
                }
                closeHost();
            });
        } catch (...) {
            close(guest_);
            closeHost();
            guest_ = -1;
            throw;
        }
    }

    ~ConsoleBridge()
    {
        if (guest_ >= 0) {
            (void)shutdown(guest_, SHUT_RDWR);
            close(guest_);
        }
        if (relay_.joinable())
            relay_.join();
    }

    ConsoleBridge(const ConsoleBridge&) = delete;
    ConsoleBridge& operator=(const ConsoleBridge&) = delete;

    [[nodiscard]] int sendInput(
        const void* buffer,
        std::size_t length) noexcept
    {
        if (length == 0U)
            return 0;
        if (buffer == nullptr)
            return EINVAL;
        std::lock_guard lock(hostWriteMutex_);
        if (host_ < 0)
            return ENOTCONN;
        std::string_view remaining {
            static_cast<const char*>(buffer), length};
        while (!remaining.empty()) {
            const ssize_t sent = send(
                host_, remaining.data(), remaining.size(), 0);
            if (sent > 0) {
                remaining.remove_prefix(static_cast<std::size_t>(sent));
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else {
                return sent == 0 ? EIO : errno;
            }
        }
        return 0;
    }

    [[nodiscard]] std::string kernelArgument() const
    {
        return "ssl0=fd:" + std::to_string(guest_) + ",fd:" +
            std::to_string(guest_);
    }

private:
    void closeHost() noexcept
    {
        std::lock_guard lock(hostWriteMutex_);
        if (host_ >= 0) {
            close(host_);
            host_ = -1;
        }
    }

    int guest_ {-1};
    int host_ {-1};
    std::mutex hostWriteMutex_;
    std::thread relay_;
};

class ActiveConsoleRegistration final {
public:
    explicit ActiveConsoleRegistration(ConsoleBridge& console)
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeConsoleBridge = &console;
    }

    ~ActiveConsoleRegistration()
    {
        std::lock_guard lock(activeEmulatorMutex);
        activeConsoleBridge = nullptr;
    }

    ActiveConsoleRegistration(const ActiveConsoleRegistration&) = delete;
    ActiveConsoleRegistration& operator=(
        const ActiveConsoleRegistration&) = delete;
};

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
bool usableFile(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ext4Rootfs(const char* path)
{
    std::string extension = std::filesystem::path {path}.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".ext4";
}

bool immutableExt4Rootfs(const char* path)
{
    if (!ext4Rootfs(path))
        return true;
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    if (error)
        return false;
    constexpr auto writable =
        std::filesystem::perms::owner_write |
        std::filesystem::perms::group_write |
        std::filesystem::perms::others_write;
    return (permissions & writable) == std::filesystem::perms::none;
}

bool environmentEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

const kernarium::cts::Campaign* findCtsCampaign(
    std::string_view identifier) noexcept
{
    for (const auto& campaign : kernarium::cts::campaigns) {
        if (campaign.identifier == identifier)
            return &campaign;
    }
    return nullptr;
}

bool validGeekbenchWorkloads(std::string_view workloads)
{
    if (workloads.empty() || workloads.size() > 256U)
        return false;
    bool needsDigit = true;
    for (const char character : workloads) {
        if (character >= '0' && character <= '9') {
            needsDigit = false;
        } else if (character == ',' && !needsDigit) {
            needsDigit = true;
        } else {
            return false;
        }
    }
    return !needsDigit;
}
#endif

/* The translating engine to run guest userspace under.
 *
 * "auto" defers to the build's platform default and its availability check
 * rather than deciding here, which keeps executable-memory policy with its
 * single owner in the CPU layer. Both Apple products resolve that to the
 * data-only gadget engine: a device cannot publish into a MAP_JIT mapping at
 * all, and while the Simulator can, measurement rejects the JIT for the
 * browser workloads that matter (G157). The override exists so that choice
 * stays re-testable without a rebuild.
 */
const char* selectedExecutionEngine()
{
    const char* requested = std::getenv("KERNARIUM_EXECUTION_ENGINE");
    if (requested == nullptr)
        return "auto";
    for (const char* allowed : {"auto", "jit", "gadgets", "interpreter"}) {
        if (std::strcmp(requested, allowed) == 0)
            return allowed;
    }
    std::fputs(
        "Kernarium: ignoring unknown KERNARIUM_EXECUTION_ENGINE\n", stderr);
    return "auto";
}

#if KERNARIUM_RUNTIME_ANDROID_GUEST
std::size_t checkedRegularFileSize(
    const std::filesystem::path& path,
    std::uintmax_t expectedSize = 0)
{
    struct stat details {};
    if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
        details.st_size < 0 ||
        (expectedSize != 0U &&
         static_cast<std::uintmax_t>(details.st_size) != expectedSize)) {
        throw std::runtime_error(
            "bundle payload is missing or unexpected: " + std::string {path});
    }
    return static_cast<std::size_t>(details.st_size);
}

std::vector<unsigned char> readRegularFile(
    const std::filesystem::path& path,
    std::uintmax_t expectedSize = 0)
{
    const auto size = checkedRegularFileSize(path, expectedSize);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        throw std::runtime_error("cannot open " + std::string {path});
    std::vector<unsigned char> payload(size);
    const auto read = std::fread(payload.data(), 1, payload.size(), file);
    const int closeResult = std::fclose(file);
    if (read != payload.size() || closeResult != 0)
        throw std::runtime_error("cannot read " + std::string {path});
    return payload;
}

/* A UBD specification is comma and colon delimited, so a path carrying either
 * would silently become a different device configuration. */
void validateUbdPath(const std::filesystem::path& path, const char* label)
{
    const auto value = path.string();
    if (value.empty() || value.find_first_of(",:") != std::string::npos) {
        throw std::invalid_argument(
            std::string {label} + " path is unusable as a UBD device");
    }
}

void appendKernelCommandLine(
    std::vector<std::string>& arguments,
    const std::filesystem::path& path)
{
    const auto payload = readRegularFile(path);
    const std::string text(payload.begin(), payload.end());
    std::size_t cursor = 0;
    std::size_t appended = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               (text[cursor] == ' ' || text[cursor] == '\n' ||
                text[cursor] == '\t'))
            ++cursor;
        const auto begin = cursor;
        while (cursor < text.size() && text[cursor] != ' ' &&
               text[cursor] != '\n' && text[cursor] != '\t')
            ++cursor;
        if (cursor != begin) {
            arguments.emplace_back(text.substr(begin, cursor - begin));
            ++appended;
        }
    }
    if (appended == 0U)
        throw std::invalid_argument("the bundle kernel command line is empty");
}

/* The instance disk is the guest's only persistent state. It lives beside the
 * payload it belongs to, so a bundle carries its own history and cannot be
 * paired with another release's disk. */
void prepareAndroidInstance(const std::filesystem::path& instance)
{
    struct stat details {};
    if (lstat(instance.c_str(), &details) == 0) {
        if (!S_ISREG(details.st_mode))
            throw std::invalid_argument("the instance disk is not a file");
        if (::access(instance.c_str(), R_OK | W_OK) != 0)
            throw std::invalid_argument("the instance disk is not writable");
        return;
    }
    if (errno != ENOENT)
        throw std::runtime_error("cannot inspect the instance disk");
    /* UBD creates the copy-on-write disk itself on first boot; the bundle only
     * has to be a writable directory when it does. */
    if (::access(instance.parent_path().c_str(), W_OK) != 0)
        throw std::invalid_argument("the bundle directory is not writable");
}
#endif

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
std::string fbconFontArgument(uint32_t dotsPerInch)
{
    static constexpr uint32_t largeConsoleFontDpi = 240U;
    return dotsPerInch >= largeConsoleFontDpi
        ? "fbcon=font:TER16x32"
        : "fbcon=font:VGA8x16";
}
#endif

} // namespace

namespace {

int runtimeRun(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot)
{
    try {
        /* A pre-existing Swift worker need not inherit the initial main-thread
         * mask. Keep the launcher itself ineligible for ITIMER_REAL throughout
         * the managed run; every runtime worker it creates inherits this mask.
         */
        ScopedGuestTimerBlock guestTimerBlock;
        ActiveRuntime active;
        if (view == nullptr || boot == nullptr ||
            boot->struct_size < sizeof(kernarium_runtime_boot)) {
            std::fputs("Kernarium: invalid boot request\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const uint32_t width = boot->width;
        const uint32_t height = boot->height;
        const uint32_t dotsPerInch = boot->dots_per_inch;
        const uint32_t guestMemoryMiB = boot->guest_memory_mib;
        const uint32_t guestProcessorCount = boot->guest_processor_count;
        if (width == 0 || height == 0 ||
            width > uml::gpu::protocol::maximumEdidDimension ||
            height > uml::gpu::protocol::maximumEdidDimension ||
            dotsPerInch == 0) {
            std::fputs("Kernarium: invalid display geometry\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const NSUInteger hostProcessorCount =
            [NSProcessInfo processInfo].processorCount;
        const uint32_t maximumGuestProcessorCount = hostProcessorCount == 0
            ? 1U : static_cast<uint32_t>(hostProcessorCount);
        if (guestProcessorCount == 0 ||
            guestProcessorCount > maximumGuestProcessorCount ||
            guestMemoryMiB < 512U || guestMemoryMiB > 4096U ||
            guestMemoryMiB % 64U != 0) {
            std::fputs(
                "Kernarium: invalid guest CPU or memory configuration\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        if (boot->android_manifest_path == nullptr ||
            boot->android_base_disk_path == nullptr ||
            boot->android_instance_path == nullptr ||
            boot->android_ramdisk_path == nullptr ||
            boot->android_command_line_path == nullptr) {
            std::fputs("Kernarium: incomplete Android bundle\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const std::filesystem::path baseDisk {boot->android_base_disk_path};
        const std::filesystem::path instance {boot->android_instance_path};
        const std::filesystem::path ramdisk {boot->android_ramdisk_path};
        const auto manifest = readRegularFile(boot->android_manifest_path);
        const std::size_t embeddedManifestSize = static_cast<std::size_t>(
            uml_android_manifest_end - uml_android_manifest_start);
        if (manifest.size() != embeddedManifestSize ||
            !std::equal(
                manifest.begin(),
                manifest.end(),
                uml_android_manifest_start)) {
            std::fputs(
                "Kernarium: the bundle belongs to a different Android "
                "release than this runtime\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        (void)checkedRegularFileSize(
            ramdisk, KERNARIUM_ANDROID_RAMDISK_SIZE);
        (void)checkedRegularFileSize(
            baseDisk, KERNARIUM_ANDROID_BASE_DISK_SIZE);
        validateUbdPath(baseDisk, "Android base disk");
        validateUbdPath(instance, "Android instance");
        prepareAndroidInstance(instance);
        const bool derivativeRootfs = false;
#else
        const char* rootfsPath = boot->rootfs_path;
        const char* overlayPath = boot->overlay_path;
        if (!usableFile(rootfsPath) || !immutableExt4Rootfs(rootfsPath) ||
            !usableFile(overlayPath)) {
            std::fputs("Kernarium: invalid rootfs or overlay path\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const bool derivativeRootfs = ext4Rootfs(rootfsPath);
#endif
        (void)setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf(
            "Kernarium: booting %ux%u guest at %u DPI with mem=%uM "
            "ncpus=%u\n",
            width,
            height,
            dotsPerInch,
            guestMemoryMiB,
            guestProcessorCount);

        uml::gpu::InProcessGpuService gpuService(
            uml::gpu::InProcessGpuOptions {
                .displayMode = uml::gpu::DisplayMode::native,
                .width = width,
                .height = height,
                .dotsPerInch = dotsPerInch,
                .nativeView = (__bridge void*)view,
            });
        uml::input::virtio::InProcessInputService touchscreen(
            uml::input::virtio::DeviceKind::touchscreen, width, height);
        uml::input::virtio::InProcessInputService keyboard(
            uml::input::virtio::DeviceKind::keyboard, width, height);
        uml::audio::InProcessAudioService audioService;
        AppNetwork network;
        auto networkCallbacks = network.callbacks();
        std::vector<std::string> arguments {
            "uml-kernarium",
            std::string {"--execution-engine="} + selectedExecutionEngine(),
            "mem=" + std::to_string(guestMemoryMiB) + "M",
            "ncpus=" + std::to_string(guestProcessorCount),
            /* iOS can reclaim the application without running any more of its
             * code, and only the guest can write back its own page cache. The
             * host asks for that through UML's management console, which needs
             * magic SysRq to act rather than silently ignore the request;
             * Android's init turns SysRq off, so state it here. */
            "sysrq_always_enabled",
        };
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        const ConsoleBootMode bootMode = ConsoleBootMode::android;
        const kernarium::cts::Campaign* ctsCampaign = nullptr;
        const std::string geekbenchWorkloads;
        ConsoleBridge consoleBridge {
            bootMode, geekbenchWorkloads, ctsCampaign, derivativeRootfs};
        /* Android tags heap pointers, so the guest must run with the top byte
         * ignored exactly as it does on the macOS Android product. */
        arguments.emplace_back("--guest-top-byte-ignore");
        arguments.emplace_back(consoleBridge.kernelArgument());
#else
        const bool aquarium = environmentEnabled("KERNARIUM_WEBGL_AQUARIUM");
        if (aquarium && KERNARIUM_AQUARIUM_SUPPORTED == 0) {
            std::fputs(
                "Kernarium: WebGL Aquarium is Simulator-only\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const bool geekbench = environmentEnabled("KERNARIUM_GEEKBENCH");
        const char* ctsCampaignEnvironment =
            std::getenv("KERNARIUM_CTS_CAMPAIGN");
        const bool ctsRequested = ctsCampaignEnvironment != nullptr &&
            ctsCampaignEnvironment[0] != '\0';
        const kernarium::cts::Campaign* ctsCampaign = ctsRequested
            ? findCtsCampaign(ctsCampaignEnvironment)
            : nullptr;
        const char* geekbenchWorkloadsEnvironment =
            std::getenv("KERNARIUM_GEEKBENCH_WORKLOADS");
        const std::string geekbenchWorkloads =
            geekbenchWorkloadsEnvironment == nullptr
            ? std::string {}
            : std::string {geekbenchWorkloadsEnvironment};
        if (ctsRequested && ctsCampaign == nullptr) {
            std::fputs(
                "Kernarium: unknown CTS campaign identifier\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const unsigned int specialModeCount =
            static_cast<unsigned int>(aquarium) +
            static_cast<unsigned int>(geekbench) +
            static_cast<unsigned int>(ctsRequested);
        if (specialModeCount > 1U) {
            std::fputs(
                "Kernarium: Aquarium, CTS, and Geekbench modes are mutually "
                "exclusive\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        if (!geekbenchWorkloads.empty() &&
            (!geekbench || !validGeekbenchWorkloads(geekbenchWorkloads))) {
            std::fputs(
                "Kernarium: targeted workloads require Geekbench mode and a "
                "comma-separated numeric list\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const ConsoleBootMode bootMode = geekbench
            ? ConsoleBootMode::geekbench
            : ctsRequested ? ConsoleBootMode::cts
            : aquarium ? ConsoleBootMode::aquarium
            : ConsoleBootMode::desktop;
        ConsoleBridge consoleBridge {
            bootMode, geekbenchWorkloads, ctsCampaign, derivativeRootfs};
        if (aquarium)
            std::fputs(
                "Kernarium: WebGL Aquarium verification requested\n",
                stderr);
        if (ctsRequested) {
            std::fprintf(
                stderr,
                "Kernarium: VK-GL-CTS campaign requested: %s\n",
                ctsCampaignEnvironment);
        }
        if (geekbench)
            std::fputs("Kernarium: no-systemd Geekbench requested\n", stderr);
        arguments.emplace_back("init=/bin/sh");
        arguments.emplace_back("console=tty0");
        arguments.emplace_back("console=ttyS0");
        arguments.emplace_back(fbconFontArgument(dotsPerInch));
        arguments.emplace_back(consoleBridge.kernelArgument());
        arguments.emplace_back("ubd1=" + std::string(overlayPath));
#endif
        arguments.emplace_back(
            "virtio_uml.device=" + gpuService.takeConnectionName() + ":" +
            std::to_string(uml::gpu::protocol::virtioGpuDeviceId));
        arguments.emplace_back(
            "virtio_uml.device=" + touchscreen.takeConnectionName() + ":" +
            std::to_string(uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(
            "virtio_uml.device=" + keyboard.takeConnectionName() + ":" +
            std::to_string(uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(
            "virtio_uml.device=" + audioService.takeConnectionName() + ":" +
            std::to_string(uml::audio::protocol::deviceId));
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* The instance disk is the copy-on-write layer over the immutable
         * bundle image, so every guest write lands in the bundle and survives
         * the next launch. */
        arguments.emplace_back(
            "ubd0=" + instance.string() + "," + baseDisk.string());
        arguments.emplace_back("initrd=" + ramdisk.string());
        arguments.emplace_back("root=/dev/ram0");
        arguments.emplace_back("rootfstype=ramfs");
        appendKernelCommandLine(
            arguments, boot->android_command_line_path);
#else
        if (derivativeRootfs) {
            arguments.emplace_back("ubd0cr=" + std::string(rootfsPath));
            arguments.emplace_back("root=/dev/ubda");
            arguments.emplace_back("rootfstype=ext4");
            arguments.emplace_back("ro");
        }
        if (!geekbench) {
            arguments.emplace_back("systemd.unit=graphical.target");
            arguments.emplace_back("plymouth.enable=0");
        }
#endif
        const uml_emulator_config config {
            .abi_version = UML_EMULATOR_ABI_VERSION,
            .struct_size = sizeof(uml_emulator_config),
#if KERNARIUM_RUNTIME_ANDROID_GUEST
            .rootfs_path = nullptr,
#else
            .rootfs_path = rootfsPath,
#endif
            .kernel_image = uml_elf_start,
            .kernel_image_size = static_cast<std::size_t>(
                uml_elf_end - uml_elf_start),
            .native_kernel_entry = nullptr,
            .network = &networkCallbacks,
        };
        uml_emulator* created = nullptr;
        const int createResult = uml_emulator_create(&config, &created);
        if (createResult != UML_EMULATOR_OK) {
            std::fprintf(
                stderr, "Kernarium: emulator creation failed: %d\n",
                createResult);
            return createResult;
        }
        const std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>
            emulator {created, uml_emulator_destroy};
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size());
        for (const auto& argument : arguments)
            pointers.push_back(argument.c_str());
        int result = 0;
        {
            ActiveConsoleRegistration consoleRegistration {consoleBridge};
            ActiveInputRegistration inputRegistration {
                touchscreen, keyboard};
            ActiveEmulatorRegistration registration {emulator.get()};
            result = uml_emulator_run(
                emulator.get(),
                static_cast<int>(pointers.size()),
                pointers.data(),
                nullptr);
        }
        std::printf("Kernarium: guest exited with status %d\n", result);
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Kernarium: startup failed: %s\n", error.what());
        return -UML_EMULATOR_RUNTIME_ERROR;
    } catch (...) {
        std::fputs("Kernarium: startup failed\n", stderr);
        return -UML_EMULATOR_RUNTIME_ERROR;
    }
}

void runtimeRequestStop(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator != nullptr)
        uml_emulator_request_stop(activeEmulator);
}

/*
 * iOS can reclaim a suspended application without running anything else in it,
 * so the guest's writable images have to reach stable storage while the app is
 * still scheduled. This flushes what the guest has already handed to the host;
 * pages still dirty inside the guest are written back by the guest itself.
 */
int runtimeSync(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator == nullptr)
        return 0;
    return uml_emulator_sync(activeEmulator);
}

/*
 * Flushing alone is not enough once the application stops being scheduled at a
 * predictable moment: the guest keeps running afterwards and dirties what was
 * just written back. Freezing it as well means the disk it leaves behind is
 * the disk that was flushed.
 */
int runtimeQuiesce(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator == nullptr)
        return 0;
    return uml_emulator_quiesce(activeEmulator);
}

void runtimeResume(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator != nullptr)
        uml_emulator_resume(activeEmulator);
}

void runtimeSendTouch(
    uint32_t x,
    uint32_t y,
    int pressed)
{
    namespace input = uml::input::virtio::protocol;
    const std::array events {
        input::InputEvent {
            .type = input::eventAbsolute,
            .code = input::absoluteX,
            .value = static_cast<std::int32_t>(x),
        },
        input::InputEvent {
            .type = input::eventAbsolute,
            .code = input::absoluteY,
            .value = static_cast<std::int32_t>(y),
        },
        input::InputEvent {
            .type = input::eventKey,
            .code = input::buttonTouch,
            .value = pressed != 0 ? 1 : 0,
        },
        input::InputEvent {
            .type = input::eventSynchronize,
            .code = input::synchronizeReport,
            .value = 0,
        },
    };
    std::lock_guard lock(activeEmulatorMutex);
    if (activeTouchscreen != nullptr)
        activeTouchscreen->send(events);
}

void runtimeSendKey(uint16_t code, int pressed)
{
    namespace input = uml::input::virtio::protocol;
    const std::array events {
        input::InputEvent {
            .type = input::eventKey,
            .code = code,
            .value = pressed != 0 ? 1 : 0,
        },
        input::InputEvent {
            .type = input::eventSynchronize,
            .code = input::synchronizeReport,
            .value = 0,
        },
    };
    std::lock_guard lock(activeEmulatorMutex);
    if (activeKeyboard != nullptr)
        activeKeyboard->send(events);
}

void runtimeSetConsoleOutputCallback(
    kernarium_runtime_console_output_callback callback,
    void* context)
{
    std::lock_guard lock(consoleOutputCallbackMutex);
    consoleOutputCallback = callback;
    consoleOutputContext = context;
}

int runtimeSendConsoleInput(const void* buffer, std::size_t length)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeConsoleBridge == nullptr)
        return -ENOTCONN;
    const int error = activeConsoleBridge->sendInput(buffer, length);
    return error == 0 ? 0 : -error;
}

const kernarium_runtime_api runtimeApi {
    .abi_version = KERNARIUM_RUNTIME_API_VERSION,
    .struct_size = sizeof(kernarium_runtime_api),
    .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
    .guest_flavor = KERNARIUM_RUNTIME_ANDROID_GUEST
        ? KERNARIUM_RUNTIME_GUEST_ANDROID
        : KERNARIUM_RUNTIME_GUEST_LINUX,
    .kernel_release = KERNARIUM_KERNEL_RELEASE,
    .framework_name = KERNARIUM_RUNTIME_FRAMEWORK_NAME,
    .run = runtimeRun,
    .request_stop = runtimeRequestStop,
    .sync = runtimeSync,
    .quiesce = runtimeQuiesce,
    .resume = runtimeResume,
    .send_touch = runtimeSendTouch,
    .send_key = runtimeSendKey,
    .set_console_output_callback = runtimeSetConsoleOutputCallback,
    .send_console_input = runtimeSendConsoleInput,
};

} // namespace

extern "C" const kernarium_runtime_api* kernarium_runtime_get_api_v3(void)
{
    return &runtimeApi;
}
