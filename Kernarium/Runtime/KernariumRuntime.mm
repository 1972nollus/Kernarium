// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumRuntime.h"

#include "gpu/in_process_service.hpp"
#include "gpu/protocol.hpp"
#include "input/virtio/in_process_service.hpp"
#include "uml/emulator.h"

#import <Foundation/Foundation.h>
#include <array>
#include <atomic>
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

constexpr std::string_view desktopBootstrap = R"krn(set -eu
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
mount -o bind / /mnt/lower || fail "EROFS lower bind"
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
if [ "${KERNARIUM_AQUARIUM:-0}" = 1 ]; then
    for unit in docker.service docker.socket containerd.service \
        packagekit.service apt-daily.service apt-daily.timer \
        apt-daily-upgrade.service apt-daily-upgrade.timer; do
        ln -sf /dev/null "/etc/systemd/system/$unit"
    done
    install -d -m 0755 /var/tmp/kernarium-aquarium-extension
    cat > /var/tmp/kernarium-aquarium-extension/manifest.json <<'AQUARIUM_MANIFEST'
{
  "manifest_version": 3,
  "name": "Kernarium Aquarium verifier",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://webglsamples.org/aquarium/*"],
    "js": ["verify.js"],
    "run_at": "document_start"
  }]
}
AQUARIUM_MANIFEST
    cat > /var/tmp/kernarium-aquarium-extension/verify.js <<'AQUARIUM_SCRIPT'
(() => {
  const delay = (milliseconds) =>
    new Promise((resolve) => setTimeout(resolve, milliseconds));
  const fail = (error) => console.error("UML_AQUARIUM_ERROR|" +
    String(error && error.stack || error).replace(/\s+/g, " ").trim());
  const run = async () => {
    if (document.readyState !== "complete") {
      await new Promise((resolve) =>
        addEventListener("load", resolve, {once: true}));
    }
    const marker = document.createElement("div");
    marker.textContent = "KERNARIUM WEBGL WARMUP";
    marker.style.cssText = [
      "position:fixed", "right:8px", "top:8px", "z-index:2147483647",
      "padding:12px", "background:#ff00ff", "color:#00ff00",
      "font:20px monospace"
    ].join(";");
    document.documentElement.appendChild(marker);

    let positiveReadings = 0;
    const deadline = Date.now() + 180000;
    while (Date.now() < deadline && positiveReadings < 5) {
      const message = document.getElementById("msgContainer")?.textContent || "";
      if (message.includes("does not appear your computer supports WebGL")) {
        throw new Error(message);
      }
      const fps = Number.parseFloat(
        document.getElementById("fps")?.textContent || "");
      positiveReadings = Number.isFinite(fps) && fps > 0
        ? positiveReadings + 1 : 0;
      await delay(1000);
    }
    if (positiveReadings < 5)
      throw new Error("Aquarium did not produce positive FPS readings");

    const canvas = document.getElementById("canvas");
    const gl = canvas?.getContext("webgl2");
    if (!gl || gl.isContextLost())
      throw new Error("WebGL 2 context unavailable or lost");

    const samples = [];
    marker.textContent = "KERNARIUM WEBGL MEASURING";
    for (let index = 0; index < 10; ++index) {
      await delay(1000);
      const fps = Number.parseFloat(
        document.getElementById("fps")?.textContent || "");
      if (!Number.isFinite(fps) || fps <= 0 || gl.isContextLost())
        throw new Error("invalid live WebGL FPS sample");
      samples.push(fps);
    }
    const average = samples.reduce((sum, value) => sum + value, 0) /
      samples.length;
    const extension = gl.getExtension("WEBGL_debug_renderer_info");
    const renderer = extension
      ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL)
      : gl.getParameter(gl.RENDERER);
    marker.textContent = average.toFixed(2) + " AVG FPS";
    console.log("UML_AQUARIUM_OK|AVG_FPS=" + average.toFixed(3) +
      "|MIN_FPS=" + Math.min(...samples).toFixed(3) +
      "|MAX_FPS=" + Math.max(...samples).toFixed(3) +
      "|SAMPLES=" + samples.length + "|FISH=500" +
      "|CANVAS=" + canvas.width + "x" + canvas.height +
      "|VERSION=" + gl.getParameter(gl.VERSION) +
      "|RENDERER=" + renderer);
  };
  run().catch(fail);
})();
AQUARIUM_SCRIPT
    cat > /usr/local/libexec/kernarium-chromium-aquarium <<'AQUARIUM_LAUNCHER'
#!/bin/sh
set -eu
runtime="/run/user/$(id -u uml)"
remaining=600
while [ ! -S /tmp/.X11-unix/X0 ] || [ ! -S "$runtime/bus" ]; do
    if [ "$remaining" -le 0 ]; then
        echo "UML_CHROMIUM_ERROR: KDE display did not become ready"
        exit 1
    fi
    remaining=$((remaining - 1))
    sleep 1
done
xauthority="$(find "$runtime" -maxdepth 1 -type f -name 'xauth_*' -print |
    head -n 1)"
if [ -z "$xauthority" ]; then
    echo "UML_CHROMIUM_ERROR: Xauthority cookie was not found"
    exit 1
fi
chromium --version || true
exec runuser -u uml -- env \
    HOME=/home/uml DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 \
    XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
    chromium --no-first-run --disable-crash-reporter \
    --enable-logging=stderr --start-maximized --use-gl=angle \
    --use-angle=gles-egl --ignore-gpu-blocklist \
    --user-data-dir=/tmp/chromium-aquarium \
    --disable-extensions-except=/var/tmp/kernarium-aquarium-extension \
    --load-extension=/var/tmp/kernarium-aquarium-extension \
    'https://webglsamples.org/aquarium/aquarium.html?numFish=500'
AQUARIUM_LAUNCHER
    chmod 0755 /usr/local/libexec/kernarium-chromium-aquarium
    cat > /etc/systemd/system/kernarium-chromium-aquarium.service <<'AQUARIUM_SERVICE'
[Unit]
Description=Kernarium Chromium WebGL Aquarium verification
After=display-manager.service network-online.target
Wants=display-manager.service network-online.target

[Service]
Type=simple
ExecStart=/usr/local/libexec/kernarium-chromium-aquarium
TasksMax=infinity
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
AQUARIUM_SERVICE
    install -d -m 0755 /etc/systemd/system/graphical.target.wants
    ln -sf ../kernarium-chromium-aquarium.service \
        /etc/systemd/system/graphical.target.wants/kernarium-chromium-aquarium.service
    echo "Kernarium Aquarium verification enabled"
fi
echo "Darwin UML root overlay: lower=erofs, upper=ext4"
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
    geekbench,
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
        std::string geekbenchWorkloads)
    {
        int descriptors[2] {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "console socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        guest_ = descriptors[0];
        const int host = descriptors[1];
        const int enabled = 1;
        if (setsockopt(
                host,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)) != 0) {
            const int error = errno;
            close(guest_);
            close(host);
            guest_ = -1;
            throw std::runtime_error(
                "console socket setup failed: " +
                std::string(std::strerror(error)));
        }
        try {
            relay_ = std::thread([
                host,
                bootMode,
                geekbenchWorkloads = std::move(geekbenchWorkloads)] {
                constexpr std::string_view shellReady =
                    "Run /bin/sh as init process";
                bool bootstrapDelivered = false;
                std::string observed;
                char buffer[4096];
                for (;;) {
                    const ssize_t received = recv(
                        host, buffer, sizeof(buffer), 0);
                    if (received > 0) {
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
                                if (bootMode == ConsoleBootMode::aquarium)
                                    bootstrap = "KERNARIUM_AQUARIUM=1\n";
                                if (bootMode == ConsoleBootMode::geekbench) {
                                    if (!geekbenchWorkloads.empty()) {
                                        bootstrap =
                                            "KERNARIUM_GEEKBENCH_WORKLOADS=";
                                        bootstrap.append(geekbenchWorkloads);
                                        bootstrap.push_back('\n');
                                    }
                                    bootstrap.append(geekbenchBootstrap);
                                } else {
                                    bootstrap.append(desktopBootstrap);
                                }
                                bootstrap.push_back('\n');
                                std::string_view remaining {bootstrap};
                                while (!remaining.empty()) {
                                    const ssize_t sent = send(
                                        host,
                                        remaining.data(),
                                        remaining.size(),
                                        0);
                                    if (sent > 0) {
                                        remaining.remove_prefix(
                                            static_cast<std::size_t>(sent));
                                    } else if (sent < 0 && errno == EINTR) {
                                        continue;
                                    } else {
                                        std::fprintf(
                                            stderr,
                                            "Kernarium: console bootstrap "
                                            "write failed: %s\n",
                                            std::strerror(errno));
                                        break;
                                    }
                                }
                                bootstrapDelivered = remaining.empty();
                                if (bootstrapDelivered)
                                    std::fprintf(
                                        stderr,
                                        "Kernarium: %s bootstrap delivered\n",
                                        bootMode == ConsoleBootMode::geekbench
                                            ? "Geekbench"
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
                close(host);
            });
        } catch (...) {
            close(guest_);
            close(host);
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

    [[nodiscard]] std::string kernelArgument() const
    {
        return "ssl0=fd:" + std::to_string(guest_) + ",fd:" +
            std::to_string(guest_);
    }

private:
    int guest_ {-1};
    std::thread relay_;
};

bool usableFile(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool environmentEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
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

std::string fbconFontArgument(uint32_t dotsPerInch)
{
    static constexpr uint32_t largeConsoleFontDpi = 240U;
    return dotsPerInch >= largeConsoleFontDpi
        ? "fbcon=font:TER16x32"
        : "fbcon=font:VGA8x16";
}

} // namespace

namespace {

int runtimeRun(
    KernariumMetalView view,
    const char* rootfsPath,
    const char* overlayPath,
    uint32_t width,
    uint32_t height,
    uint32_t dotsPerInch,
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount)
{
    try {
        /* A pre-existing Swift worker need not inherit the initial main-thread
         * mask. Keep the launcher itself ineligible for ITIMER_REAL throughout
         * the managed run; every runtime worker it creates inherits this mask.
         */
        ScopedGuestTimerBlock guestTimerBlock;
        ActiveRuntime active;
        if (view == nullptr || !usableFile(rootfsPath) ||
            !usableFile(overlayPath) ||
            width == 0 || height == 0 ||
            width > uml::gpu::protocol::maximumEdidDimension ||
            height > uml::gpu::protocol::maximumEdidDimension ||
            dotsPerInch == 0) {
            std::fputs(
                "Kernarium: invalid display, rootfs, or overlay path\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const NSUInteger hostProcessorCount =
            [NSProcessInfo processInfo].processorCount;
        const uint32_t maximumGuestProcessorCount = hostProcessorCount == 0
            ? 1U : static_cast<uint32_t>(hostProcessorCount);
        if (guestProcessorCount == 0 ||
            guestProcessorCount > maximumGuestProcessorCount ||
            guestMemoryMiB < 512U || guestMemoryMiB > 4096U ||
            guestMemoryMiB % 128U != 0) {
            std::fputs(
                "Kernarium: invalid guest CPU or memory configuration\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
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
        AppNetwork network;
        auto networkCallbacks = network.callbacks();
        const bool aquarium = environmentEnabled("KERNARIUM_WEBGL_AQUARIUM");
        const bool geekbench = environmentEnabled("KERNARIUM_GEEKBENCH");
        const char* geekbenchWorkloadsEnvironment =
            std::getenv("KERNARIUM_GEEKBENCH_WORKLOADS");
        const std::string geekbenchWorkloads =
            geekbenchWorkloadsEnvironment == nullptr
            ? std::string {}
            : std::string {geekbenchWorkloadsEnvironment};
        if (aquarium && geekbench) {
            std::fputs(
                "Kernarium: Aquarium and Geekbench modes are mutually "
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
            : aquarium ? ConsoleBootMode::aquarium : ConsoleBootMode::desktop;
        ConsoleBridge consoleBridge {bootMode, geekbenchWorkloads};
        if (aquarium)
            std::fputs("Kernarium: WebGL Aquarium verification requested\n", stderr);
        if (geekbench)
            std::fputs("Kernarium: no-systemd Geekbench requested\n", stderr);
        std::vector<std::string> arguments {
            "uml-kernarium",
            "--execution-engine=gadgets",
            "mem=" + std::to_string(guestMemoryMiB) + "M",
            "ncpus=" + std::to_string(guestProcessorCount),
            "init=/bin/sh",
            "console=tty0",
            "console=ttyS0",
            fbconFontArgument(dotsPerInch),
            consoleBridge.kernelArgument(),
            "ubd1=" + std::string(overlayPath),
            "virtio_uml.device=" + gpuService.takeConnectionName() + ":" +
                std::to_string(uml::gpu::protocol::virtioGpuDeviceId),
            "virtio_uml.device=" + touchscreen.takeConnectionName() + ":" +
                std::to_string(uml::input::virtio::protocol::deviceId),
            "virtio_uml.device=" + keyboard.takeConnectionName() + ":" +
                std::to_string(uml::input::virtio::protocol::deviceId),
        };
        if (!geekbench) {
            arguments.emplace_back("systemd.unit=graphical.target");
            arguments.emplace_back("plymouth.enable=0");
        }
        const uml_emulator_config config {
            .abi_version = UML_EMULATOR_ABI_VERSION,
            .struct_size = sizeof(uml_emulator_config),
            .rootfs_path = rootfsPath,
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

const kernarium_runtime_api runtimeApi {
    .abi_version = KERNARIUM_RUNTIME_API_VERSION,
    .struct_size = sizeof(kernarium_runtime_api),
    .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
    .reserved = 0,
    .kernel_release = KERNARIUM_KERNEL_RELEASE,
    .framework_name = KERNARIUM_RUNTIME_FRAMEWORK_NAME,
    .run = runtimeRun,
    .request_stop = runtimeRequestStop,
    .send_touch = runtimeSendTouch,
    .send_key = runtimeSendKey,
};

} // namespace

extern "C" const kernarium_runtime_api* kernarium_runtime_get_api_v1(void)
{
    return &runtimeApi;
}
