# x86-64 smoke rootfs

This producer creates the smallest immutable x86-64 userspace used to qualify
the 4 KiB Darwin UML x86 execution boundary. It is intentionally separate from
the accepted general-purpose rootfs.

Run `build.sh OUTPUT.erofs` on an x86-64 Linux builder with GNU binutils,
`busybox-static`, and `erofs-utils`. The image contains only x86-64 ELF
executables. Its PID 1 checks write syscalls, FS-base TLS addressing, signal
delivery, the native x86 XSAVE signal-frame layout, and x87/XMM/YMM restoration
through `rt_sigreturn`, then prints one success line and powers the guest off.
BusyBox is present for follow-on instruction-coverage work but is not part of
the initial boot path.

Validate the resulting image with
`tests/integration/x86_64/run_guest_x86_64_smoke.py`. The runner requires a
4 KiB mainline kernel artifact, selects the gadget engine, boots `/init`, and
accepts the run only after the guest reports its syscall, TLS, and signal
marker and powers itself off cleanly.
