# Shared Kernarium Steam appliance assets

The [x86-64 Steam appliance](../steam-x86_64/README.md) installs from this
directory. It holds the parts that do not depend on the guest architecture:
the startup, storage and launch contract that the boot benchmark and the
producer share.

- `overlay/` is copied into the image root before the producer's own overlay:
  the image-owned init and its storage preparation, the Gamescope session
  supervisor, the splash hand-off unit and service, and the V4L2 decoder
  discovery rule.
- `splash/` is the C++20 Kernarium boot and installation renderer. Each
  producer compiles it natively inside its own verified toolchain; the build
  refuses to run outside one.
- `launcher.py` owns the persistent GPT user-data disk and the kernel command
  line. Each producer's `boot.py` adds only the emulator options its guest
  needs, and the launcher rejects those and the storage, init, console and
  memory arguments in a caller's pass-through list.

## The startup movie, and why it is not hardware decoded

Big Picture plays a startup movie on every start. The client's
UI bundle selects `steamui/movies/bigpicture_startup.webm` — VP9 Profile 0,
1920x1200, 6 s, fullscreen — for any device that is neither SteamOS nor Deck,
Steam Machine or Frame hardware, which is this profile, and `steamwebhelper`
opens and streams that file within a second of the `Steam Big Picture Mode`
window becoming viewable.

**Access times cannot observe this.** The user-data partition is mounted
`noatime`, so every movie keeps its packaged timestamp however often it is
read. Watch the directory with `inotify`, or sample `/proc/<pid>/fd`.

It is decoded in software, and the decoder is not the reason.
Steam writes a `chrome://gpu` report to
`~/.local/share/Steam/logs/webhelper_gpu.txt`; in the guest it reports
`videoAcceleratorsInfo Decoding (null)`, every GpuMemoryBuffer format
`Software only` and `YUV_420_BIPLANAR: not supported`, after the GPU process
logs `gbm_wrapper.cc: Failed to export buffer to dma_buf` for every allocation
it attempts. Chromium's `VideoDecoderPipeline`, the backend of `VaapiVideoDecoder`, needs
dma-buf-backed NV12 frames,
so with no dma-buf export it advertises no decode configuration at all and CEF
falls back to its built-in software decoder. The V4L2 node, this directory's
udev rule, the device's VP9 Profile 0 advertisement and the client's own
`--enable-features=PlatformHEVCDecoderSupport,V4L2VideoDecode` are all present
and correct; the blocker is above them, in the guest's Zink-on-Venus GBM.

The x86-64 client's CEF carries only VA-API. It loads `libva.so.2` and
`libva-drm.so.2` into its GPU process, so it needs a libva backend over the
decoder node that this directory's rule publishes as `/dev/video-dec0`. The
image installs [one](../../video/va-v4l2/README.md), and this launcher then
starts the client with Valve's `-cef-enable-vaapi` option when the session
requests it through `KERNARIUM_CEF_VAAPI`.
