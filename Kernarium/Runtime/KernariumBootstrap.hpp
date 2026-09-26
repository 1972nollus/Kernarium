// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace kernarium::bootstrap {

/* The desktop console bootstrap: it turns the read-only root the kernel
 * mounted into a writable overlay and starts the image's init. A mode may
 * insert its own setup between the two halves. */
inline constexpr std::string_view desktopPrefix = R"krn(set -eu
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

inline constexpr std::string_view desktopSuffix = R"krn(echo "Darwin UML root overlay: lower=read-only, upper=ext4"
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

/* The relay types the bootstrap into the shell the kernel starts as init, so
 * the shell reads it from the console one line at a time, and it runs each
 * command as soon as it has read it. A failed step hands the console to a
 * rescue shell, which would then read and run the rest of the bootstrap: it
 * would report an overlay that was never built and start init on the
 * read-only root. One brace group is one command, so the shell reads all of
 * the bootstrap before it runs any of it, and the rescue shell starts at the
 * end of what the relay sent. */
[[nodiscard]] inline std::string desktop(std::string_view modeSetup)
{
    std::string text;
    text.reserve(desktopPrefix.size() + modeSetup.size() +
        desktopSuffix.size() + 4U);
    text.append("{\n");
    text.append(desktopPrefix);
    text.append(modeSetup);
    text.append(desktopSuffix);
    text.append("}\n");
    return text;
}

} // namespace kernarium::bootstrap
