# ADR 0001: retain CMake/CTest with Ninja

- Status: accepted for the refactor
- Date: 2026-08-01

## Decision

CMake 3.27 or newer with Ninja remains the canonical first-party host
build/test system and CTest remains the canonical test registry. Linux Kbuild
and GNU Make remain authoritative inside the Linux source boundary. The
source-built GLib, libslirp, libepoxy, and virglrenderer dependencies retain
their upstream Meson/Ninja build boundary; other third-party release packages
retain their upstream CMake or Autoconf/GNU Make boundary. The outer CMake
graph consumes only the verified private static prefix and does not maintain a
second source manifest for those projects.

The refactor will improve the existing graph with narrow targets, explicit
source manifests, CMake presets, centralized CTest helpers, labels, fixtures,
resource policy, and a CMake File API/source-layer audit. It will not introduce
parallel Bazel or Meson source manifests.

## Consequences

- Configure, build, test, and workflow presets are part of the supported
  interface.
- Unit-test implementations and registered unit commands contain no Python and
  must build without the kernel runner, CLI, or a rootfs image. Python may be
  used as an explicit configure/build tool for repository-wide declaration and
  source-policy verification; those invocations are not unit-test commands.
- Guest/rootfs, GUI, networking, and performance workflows are explicit
  integration or benchmark targets and are not hidden in ordinary unit test
  registration.
- Any future Bazel evaluation is a time-boxed proof of concept and cannot
  become a second source of truth without a separate decision.

## References

The trade-off analysis and primary references are retained in
`REFACTORING_PLAN.md` under “Build and test system decision”.
