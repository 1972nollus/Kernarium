# ADR 0001: CMake, CTest, and Ninja

- Status: accepted
- Date: 2026-08-01

CMake 3.27+ with Ninja is the first-party host build system, and CTest is the
test registry. Linux keeps Kbuild/GNU Make, while third-party projects keep
their verified upstream build systems.

Presets, narrow targets, centralized CTest helpers, and the CMake File API audit
form one supported graph. Do not add a second Bazel or Meson source manifest.
Guest, GUI, network, and performance work remains explicit rather than hidden
inside unit registration.
