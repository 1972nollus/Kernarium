# Patched Vulkan guest userspace

This directory owns the source inputs for the separately authorized Vulkan
rootfs derivative. It does not alter `rootfs.erofs` or the sealed GL/GLES
derivative. The image is accepted only when its checked receipt, read-only
attachment, CTS identity, and platform gates all pass.

The host backend is Venus over KosmicKrisp. The guest uses an ordinary Vulkan
ICD manifest rather than a portability-subset driver. Guest and renderer
codecs are generated offline into build directories from the same patched
Venus protocol and pinned SDK registry; existing command IDs and aliases
remain unchanged. Generated codecs, SDK headers, and registry copies are not
carried in the downstream patch series. The Android Vulkan HAL's port is the
one carrier of a generated codec, because Soong cannot generate it out of
tree. It pins the protocol series and Vulkan-Headers trees recorded here, so
a change to either needs that port regenerated, as the
[Android producer](../../../android/aosp-17/README.md#graphics) describes.

The guest Mesa series ([`patches/mesa/series.tsv`](patches/mesa/series.tsv))
is intentionally downstream and carries transport, reporting, and sharing
correctness fixes; each topic's commit message states its contract. The renderer pairing and receipt
gates below are what this image guarantees. How a guest shares a swapchain
image with the renderer, including the native-heap image-extent limit, is in
[`graphics.md`](../../../docs/graphics.md#vulkan).

Passing this project's receipt-gated CTS campaign demonstrates the tested
stack's result. It is not by itself a Khronos conformance claim; that also
requires the applicable submission, review, and trademark process.

`sources.tsv` pins every upstream commit, tree, annotated release object where
one exists, and generated Mesa, Venus-protocol, and CTS mail-patch series.
Vulkan-Headers and Vulkan-Loader use the matching 1.4.357 SDK tag. The CTS source
remains based on Vulkan CTS 1.4.6.2 and carries two authenticated correctness
patches: memory/image-copy address ranges stay within their buffer in both
directions, and compute-only cases are skipped when no exclusive compute queue
exists. Generated patch files must be changed through a topic checkout and
regenerated with:

```sh
tools/rootfs/vulkan/rebase-patches.sh \
  --component mesa --source /path/to/mesa-topic
```

The Mesa series is also the lower segment of the host's Mesa series in
[`../../../dependencies/patches/mesa`](../../../dependencies/README.md): one
topic branch carries these guest topics followed by the host-only topics.
Export this segment with `--head` naming its last commit, then the host layer
with `dependencies/rebase-patches.sh`. A guest Mesa change therefore also
invalidates the host prefixes.

The accepted CTS images and their receipts name one patched tree per component,
and each component's `series.tsv` records the tree its patches produce. A
series edit that keeps that tree, such as a reworded message or a folded
follow-up, leaves the receipts valid. A series that produces a different tree
invalidates the accepted receipts until new images are sealed and accepted. In
the meantime the CTS runners refuse the stale images, and the
`vulkan-guest-rootfs-safety` tooling test fails with
`accepted CTS image is stale`, naming the `<component>.patched_tree` that
changed. That is the intended interlock, not a defect in the gates; the way to
clear it is to seal new images, never to relax a verifier. The other
receipt-bound tooling tests check runner logic against copies of the accepted
receipts rebound to the current source locks and trees
(`tests/tooling/gpu/receipt_fixtures.py`), so a series change leaves them
passing.

The final build, installation, CTS build, image creation, filesystem checks,
and receipt generation run in a native arm64 Debian VM or, under `fakeroot`,
in an unprivileged arm64 Debian container that cannot create device nodes; the
sealer inspects the guest tree with a clean environment so no host preload
reaches the recorded runtime closure, and the receipt names the environment
kind it ran in. Prepared checkouts live
below `WORK/sources`, out-of-tree builds below `WORK/build`, staged installs
below `WORK/stage`, and immutable inputs below `WORK/input`. Mesa builds the
patched Virtio Vulkan driver together with matched GLX, EGL, GBM, OpenGL, and
GLES frontends. Its Gallium runtime contains Virgl for the ordinary desktop
path, Zink for OpenGL-over-Venus validation, and Softpipe as the non-LLVM
recovery path. Vulkan beta declarations remain enabled for the pinned CTS/API
surface but do not mark the ICD as a portability driver. Only Meson's
runtime-tagged install is accepted, and the sealer audits its exact files,
symlinks, and build options before copying the real runtime files.

`exclusions.txt` is the exact removal set applied to a fresh extraction of the
base image. The derivative retains the base libdrm, replaces the base Vulkan
loader with the pinned source-built 1.4.357 runtime, replaces the Virtio ICD in
place, and installs only the release-built Vulkan CTS runtime and its
manifest-referenced data. With `--include-opengl-cts`, the same source build
also installs `glcts` and its generated runtime-data closure. The GL46 scope is
derived from the four public must-pass XML collections used by the pinned
upstream runner: ES-for-GL, no-context, single-config, and per-config GL tests.
Unreferenced text files, including the retired GTF list, are not executable
requirements. This combined mode has a distinct
artifact identity and never replaces the accepted Vulkan-only image.

The immutable base and the derivative's identities, content digest, and
complete build identity are recorded in
`tests/integration/gpu/vulkan_cts_receipt.json` and checked by its verifier.

The combined Vulkan/Zink/OpenGL derivative is named
`rootfs-vulkan-zink-gl.ext4`, has its identity in
`tests/integration/gpu/vk_gl_cts_derivative_rootfs.json`, and is consumed only
with its emitted schema-4 receipt. It binds the upstream runner identity, all
four XML payloads and hashes, their derived configuration records, and the
eight referenced leaf identities. Full qualification additionally freezes an ordered runtime
`CTS-Configs.gl46` inventory: the first actual EGL configuration receives the
one-time suites and first-config records; later configurations receive their
other-config records. XML surface sizes, seeds, and named FBO configurations
are retained. The OpenGL runner proves both the Zink Gallium renderer and the
underlying Virtio-GPU Venus device before executing a shard. Vulkan and OpenGL
qualification require the same current
patched source trees for Mesa, Venus protocol, and CTS, plus the pinned
Vulkan-Headers and Vulkan-Loader sources. Their image, executable,
and runtime-closure identities remain separate.

Seal only new stage, image, and receipt paths. The Mesa build and stage roots
default below `WORK`, and the metadata and repository roots default to this
checkout; `--help` lists every option:

```sh
sudo tools/rootfs/vulkan/seal-in-vm.py --work-root WORK \
  --base-rootfs WORK/input/rootfs.erofs --stage-root NEW-STAGE \
  --output NEW.ext4 --receipt NEW.receipt.json
```

Add `--include-opengl-cts` for a combined qualification image.

The sealer refuses existing output paths and never mounts or boots an image
writable. It authenticates each source's base and the tree its series records;
regenerates every driver header from the selected protocol source and SDK into
a directory Mesa must select explicitly instead of its older vendored codecs,
and byte-checks the build's complete header inventory; byte-compares the
supplied runtime stage with a fresh runtime-only install; checks the CTS
build's directories, options and embedded releases against the authenticated
source without rebuilding it; and audits dynamic dependencies. The image it
publishes passes a read-only zero-error ext4 check and is mode 0444. `seal()`
holds the order of operations.

After direct transfer from the VM, verify both the receipt and image:

```sh
tools/rootfs/vulkan/verify-receipt.py \
  --image .build/rootfs/rootfs-vulkan-venus.ext4
```

## Relocatable guest runtime

The guest runtime layout differs between architectures only in its multiarch
library directory. `seal-in-vm.py` owns that single contract in
`architecture_profile()`: the driver inventory, install mapping, Meson and
Vulkan-Loader options, ICD identity, and audited runtime closure are shared,
and each supported Debian architecture names the same runtime in its own
library directory.

`stage-runtime.py` produces that runtime for one architecture as a relocatable
root plus its own receipt, with no CTS payload and no image identity. It runs
natively on the target architecture, reuses the sealer's source, protocol,
build, and stage verification, and records the runtime's dynamic-dependency
closure against a base root of the distribution the consuming image will use,
so an image's package set is known before the image exists.

```sh
sudo tools/rootfs/vulkan/stage-runtime.py --architecture amd64 \
  --work-root WORK --runtime-root WORK/stage/runtime-amd64 \
  --receipt WORK/output/guest-runtime-amd64.receipt.json \
  --closure-root WORK/closure/trixie-amd64
```

`install-runtime.py` imports that pair into an image root. It authenticates
every file against the receipt before the image is touched, recreates the
recorded symlinks, removes every other ICD manifest so a software driver
cannot mask a Venus failure, refuses an unresolved dynamic dependency, and
records what it installed in `/usr/share/uml-vulkan/guest-runtime.json`.

```sh
sudo tools/rootfs/vulkan/install-runtime.py --architecture amd64 \
  --source WORK/stage/runtime-amd64 \
  --receipt WORK/output/guest-runtime-amd64.receipt.json --root IMAGE_ROOT
```
