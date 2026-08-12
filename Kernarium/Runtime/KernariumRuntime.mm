// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumRuntime.h"

#include "in_process_service.hpp"
#include "protocol.hpp"
#include "uml/emulator.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
extern const unsigned char uml_elf_start[]
    __asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
    __asm("section$end$__DATA$__uml_elf");
}

namespace {

std::atomic_bool runtimeActive;
std::mutex activeEmulatorMutex;
uml_emulator* activeEmulator;

constexpr std::string_view kdeSystemdBootstrap = R"krn(set -eu
fail()
{
    echo "Unable to construct the Kernarium tmpfs overlay root: $*" >&2
    exec /bin/sh
}
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt || fail "tmpfs mount"
mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root || fail "overlay workspace"
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
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker || \
    fail "Docker state tmpfs"
mkdir -p /var/lib/containerd
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd || \
    fail "containerd state tmpfs"
install -d -m 0755 /usr/local/libexec \
    /etc/systemd/system/multi-user.target.wants
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
    /home/uml/.local/share/Trash/info
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
echo "Darwin UML root overlay: lower=erofs, upper=tmpfs"
if [ -x /lib/systemd/systemd ]; then
    exec /lib/systemd/systemd
fi
if [ -x /usr/lib/systemd/systemd ]; then
    exec /usr/lib/systemd/systemd
fi
fail "systemd executable not found"
)krn";

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

class ConsoleBridge final {
public:
    ConsoleBridge()
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
            relay_ = std::thread([host] {
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
                                std::string bootstrap {kdeSystemdBootstrap};
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
                                if (bootstrapDelivered) {
                                    std::fputs(
                                        "Kernarium: systemd bootstrap "
                                        "delivered\n",
                                        stderr);
                                }
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
        return "con0=fd:" + std::to_string(guest_);
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

} // namespace

extern "C" int kernarium_runtime_run(
    KernariumMetalView view,
    const char* rootfsPath,
    uint32_t width,
    uint32_t height)
{
    try {
        ActiveRuntime active;
        if (view == nullptr || !usableFile(rootfsPath) ||
            width == 0 || height == 0) {
            std::fputs("Kernarium: invalid display or rootfs path\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        (void)setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf(
            "Kernarium: booting %ux%u guest with mem=2G ncpus=4\n",
            width,
            height);

        uml::gpu::InProcessGpuService gpuService(
            uml::gpu::InProcessGpuOptions {
                .displayMode = uml::gpu::DisplayMode::native,
                .width = width,
                .height = height,
                .nativeView = (__bridge void*)view,
            });
        ConsoleBridge consoleBridge;
        std::vector<std::string> arguments {
            "uml-kernarium",
            "--execution-engine=gadgets",
            "mem=2G",
            "ncpus=4",
            "init=/bin/sh",
            "console=tty0",
            "systemd.unit=graphical.target",
            consoleBridge.kernelArgument(),
            "virtio_uml.device=" + gpuService.takeConnectionName() + ":" +
                std::to_string(uml::gpu::protocol::virtioGpuDeviceId),
            "uml_vnc_input." + gpuService.takeInputConnectionName(),
        };
        const uml_emulator_config config {
            .abi_version = UML_EMULATOR_ABI_VERSION,
            .struct_size = sizeof(uml_emulator_config),
            .rootfs_path = rootfsPath,
            .kernel_image = uml_elf_start,
            .kernel_image_size = static_cast<std::size_t>(
                uml_elf_end - uml_elf_start),
            .native_kernel_entry = nullptr,
            .network = nullptr,
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

extern "C" void kernarium_runtime_request_stop(void)
{
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator != nullptr)
        uml_emulator_request_stop(activeEmulator);
}
