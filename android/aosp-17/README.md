# AOSP 17 UML guest producer

This directory owns the standalone AOSP 17 producer lane for the native ARM64
Darwin UML guest. It starts from the exact source identities in
[`metadata/aosp.env`](metadata/aosp.env) and
[`../../kernel/android-6.18/patches/metadata.env`](../../kernel/android-6.18/patches/metadata.env).

The first producer milestone prepares and validates existing builder-local Repo
workspaces:

```sh
android/aosp-17/build/prepare-sources.sh \
  --aosp-source /absolute/path/to/aosp \
  --ack-workspace /absolute/path/to/ack-workspace

android/aosp-17/build/preflight.sh \
  --aosp-source /absolute/path/to/aosp \
  --ack-workspace /absolute/path/to/ack-workspace \
  --darwin-sdk /absolute/path/to/MacOSX.sdk \
  --work-root /absolute/path/to/work \
  --release-root /absolute/path/to/releases
```

Materialize and build the tracked ACK series in a separate worktree:

```sh
android/aosp-17/build/apply-kernel-patches.sh \
  --ack-workspace /absolute/path/to/ack-workspace \
  --kernel-source /absolute/path/to/new-android-kernel-source

android/aosp-17/build/build-kernel.sh \
  --ack-workspace /absolute/path/to/ack-workspace \
  --kernel-source /absolute/path/to/new-android-kernel-source \
  --darwin-sdk /absolute/path/to/MacOSX.sdk \
  --build /absolute/path/to/kernel-build
```

Apply the generated per-project AOSP series and build the initial 16 KiB,
ARM64-only product composition:

```sh
android/aosp-17/build/apply-aosp-patches.sh \
  --aosp-source /absolute/path/to/aosp

android/aosp-17/build/build-aosp.sh \
  --aosp-source /absolute/path/to/aosp \
  --out /absolute/path/to/aosp/out-uml-android17
```

[`metadata/base-manifest.xml`](metadata/base-manifest.xml) locks every resolved
Repo project. [`patches/projects.tsv`](patches/projects.tsv) routes each changed
project from its exact base commit and tree to its expected final tree. Files
destined for the AOSP checkout, including the UML product definition, are
introduced only by generated `git format-patch` series under `patches/projects`;
`support/` is never copied over an AOSP project.

`prepare-sources.sh` is the only producer command in this lane that may access
the public source network. It moves clean builder-local workspaces to the pinned
AOSP manifest and ACK commit. All later commands operate offline.

No tracked command connects to or transfers files to another machine. Copy a
reviewed outer-project snapshot to the builder before invoking these scripts,
then copy completed releases back through an operator-managed workflow outside
the repository.

The Android kernel has one profile: 16 KiB, ARM64-only, with
`CONFIG_MODULES=n`. The build rejects any `=m` configuration or `.ko` output
and verifies the built-in Android feature set, ELF geometry, and exact Darwin
bridge imports. AOSP product patches, image assembly, release publication, and
Apple consumers remain separate from the host build. The current AOSP product
is an initial page-size and ABI composition based on Cuttlefish; standalone UML
storage, boot, VINTF, SELinux, and HAL adaptation remain later milestones.
