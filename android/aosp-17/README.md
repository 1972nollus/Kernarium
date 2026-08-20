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
  --out /absolute/path/to/aosp/out-uml-android17 \
  --kernel-elf /absolute/path/to/kernel-build/vmlinux
```

[`metadata/base-manifest.xml`](metadata/base-manifest.xml) locks every resolved
Repo project. [`patches/projects.tsv`](patches/projects.tsv) routes each changed
project from its exact base commit and tree to its expected final tree. Files
destined for the AOSP checkout, including the UML product definition, are
introduced only by generated `git format-patch` series under `patches/projects`;
`support/` is never copied over an AOSP project.

After amending a clean, linear per-project topic branch, regenerate its patch
bytes and all routed final-tree/count/manifest identities together:

```sh
android/aosp-17/tools/regenerate-aosp-patches.py \
  --android-root /absolute/path/to/copied/repository/android/aosp-17 \
  --route-id p0001 \
  --topic-checkout /absolute/path/to/project-topic
```

The topic must descend directly from the route's exact base commit, every
subject must start with `UML-ANDROID-LOCAL:`, and the worktree must be clean.
Never edit the generated patch bytes or either identity table by hand.

After packaging, the builder verifies that `BOOT/kernel` exactly matches the
selected ACK ELF, rejects DLKM and loadable-module payloads, rejects 32-bit
target ABIs, and checks 16 KiB load alignment for target executables.

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
bridge imports. The AOSP product disables system, vendor, and ODM DLKM images,
clears every inherited Cuttlefish module list, and rejects a module-bearing
configuration before compilation. It also disables Cuttlefish's Trusty
security VM before inheritance and retains only the explicitly nonsecure
software KeyMint and gatekeeper APEXes; the build rejects remote or
guest-Trusty alternatives. AOSP image assembly, release publication,
and Apple consumers remain separate from the host build. The current product is
still an initial page-size and ABI composition based on Cuttlefish; standalone
UML storage, boot, VINTF, SELinux, and HAL adaptation remain later milestones.

## Static UBD disk contract

`support/image/partition-layout.json` owns the first fixed GPT layout. It has
separate `system`, `system_ext`, `product`, `vendor`, `odm`, `metadata`, and
`userdata` partitions; it deliberately has no `super` or DLKM partition. The
kernel ELF and Android ramdisks remain separate release artifacts and are not
placed in this disk.

Once the static AOSP product emits the declared image names, assemble and
immediately verify a raw UBD disk locally on the builder with:

```sh
android/aosp-17/tools/ubd-disk.py assemble \
  --layout android/aosp-17/support/image/partition-layout.json \
  --images-dir /absolute/path/to/product-output \
  --output /absolute/path/to/release/android17-ubd.img
```

The assembler accepts ordinary raw images and Android sparse images, expands
payloads into a sparse host file, rejects overflow and unsafe image paths, and
writes matching primary and backup GPT metadata. Recheck an existing image
with the `verify` subcommand. The fixed partition capacities are intentionally
larger than the initial inherited Cuttlefish dynamic groups; the first static
product build must still prove every concrete image fits before publication.
