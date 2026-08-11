# Testing and preparation

## Current accepted state (2026-08-10)

The complete refactor acceptance remains anchored at implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97`. That baseline passed the ordinary
16 KiB and 4 KiB unit suites at 572/572 and 573/573, respectively, with
tooling at 51/51 in each profile. Full ASan+UBSan passed 572/572, TSan passed
8/8, the malformed loader ASan gate passed 1/1, and both normal and sanitized
GPU configurations passed 5/5 units plus the 1/1 VNC tooling test.
`integration-fast` passed 5/5 in each page profile, and the standalone SMP
workload reached 8.19 and 8.03 effective host cores at 16 KiB and 4 KiB. The
three-slice Apple package and its raw, installed-tree, and real Xcode consumers
passed. Those results are not silently reattributed to the later rootfs
migration.

The subsequent unified-rootfs migration supersedes the old partitioned-image
policy. Its final host workflows passed 572/572 units plus 49/49 tooling tests
at 16 KiB and 573/573 plus 49/49 at 4 KiB. Both production page profiles were
rebuilt; the latest regenerated `integration-fast` suites passed 5/5 in 65.42
seconds at 16 KiB and 5/5 in 69.33 seconds at 4 KiB with raw EROFS at
`/dev/ubda`: overlay/systemd, interpreter, gadgets, JIT, and deterministic
SLiRP. The complete integration workflows also passed 52/52 tooling checks in
each profile. Their overlay/systemd tests passed in 34.06 and 38.19 seconds,
respectively, with separate Docker and containerd tmpfs mounts and an offline
source-built hello-world container. Focused rootfs, provisioning, launcher,
GPU-runner, LTP-runner,
network-runner, performance-runner, emulator-option, and API contracts also
pass. The 16 KiB guest reports
the upstream `EXPERIMENTAL EROFS subpage compressed block support in use`
warning because the image has 4 KiB filesystem blocks; the 4 KiB guest does
not. This is a recorded upstream filesystem warning, not a failed mount or an
emulator diagnostic.

`rootfs.erofs` now supplies LTP at `/opt/ltp`, Node at `/opt/node/bin/node`,
`e2fsprogs` formatters (`mkfs.ext2`, `mkfs.ext3`, and `mkfs.ext4`),
`/usr/sbin/debugfs`, `/usr/sbin/e4crypt`, the full iproute2 `ip`, `bc` 1.07.1,
the 4.09 `quota`/`quotacheck` suite (`quota` is `/usr/bin/quota`), and
benchmark payloads under `/opt`. The manifests contain
2,428 verified LTP cases and 135 disabled cases, with no overlap; 88 cases
moved to disabled
because they require deliberately absent `CONFIG_HUGETLBFS` (79),
`CONFIG_TRANSPARENT_HUGEPAGE` (5), or `CONFIG_CAN` (4). This migration did not
run or claim the full LTP campaign. The credited final-image boundary is a
44-case focused gadgets-engine qualification: every live expectation was
accepted, `statx05` passed four assertions in both page profiles, and the
single-vCPU serialized `zram01` run returned zero with 12 TPASS and one
packaged-test TCONF in 167.6 seconds. The user-directed final Geekbench scoring
run remains skipped; the local Geekbench 6 command uses `--no-upload`, and no
new score is claimed.

All results were produced locally; no hosted CI workflow exists. Instruction
input materialization was off for the complete refactor acceptance and remains
an explicit, storage-intensive opt-in. The current rootfs size, digest, EROFS
superblock fields, and UUID are single-sourced in
`tools/rootfs/manifest.json` and its checked release statement instead of being
copied into this runbook.

Use CMake/Ninja and CTest as the first-party source of truth. The `unit` suite
is host-only, contains no Python, and neither configures a kernel nor reads a
rootfs. Native emulator behavior tests are C++/assembly. The separately owned
`tooling` checks may use shell, CMake, or Python for build/source contracts;
they remain host-only and are selected alongside `unit` by the standard unit
workflow rather than becoming guest integration tests.

Every first-party registration has exactly one enforced primary suite label:
`unit`, `tooling`, or `integration`. The test manifest, registration helpers,
and owner paths enforce that suite-level taxonomy. Domain words in names and
source ownership remain useful, but secondary per-domain labels and a specific
per-domain subdirectory layout are optional organization, not acceptance gates.

Reproducibility here means pinned official release archive and signature
identities, patch bytes and order, verified prepared source trees,
configuration inputs, ABI identity, and a recorded build procedure. Git,
Kbuild, compiler, linker, and Xcode-generated timestamps, commit IDs, build
IDs, user/host fields, package layouts, and output hashes stay authentic and
may differ between runs; record them as observations rather than rewriting
metadata or requiring byte equality. The signed `rootfs.erofs` digest is
intentionally stricter because it identifies the unified guest input.

## Green checkpoints and bisectable commits

Every completed feature or bug fix ends in a local commit only after
proportionate validation shows that the affected emulator state works. Start
with the narrow target or exact test, then run the complete supported workflow
for the affected subsystem. Page-size-sensitive CPU, memory, ABI, kernel, and
guest changes require both 16 KiB and 4 KiB coverage; GPU, loader, package, and
guest workflows are required when those paths change.

A green checkpoint has no known failure in the affected behavior and includes
the tests, documentation, manifests, ABI identities, and generated copies that
the implementation requires. Each commit must contain one coherent change
and pass its applicable checks independently so `git bisect` can identify the
first regression. Do not commit an incomplete implementation, a temporary
diagnostic, or a knowingly broken emulator state. If an external input,
credential, host permission, or unavailable campaign prevents required
validation, leave the change uncommitted and report the missing prerequisite.

This policy requires verified local commits; it does not authorize pushing,
rebasing, amending, force-pushing, or otherwise rewriting shared history.

```sh
cmake --preset macos-unit-16k
cmake --build --preset unit-test-binaries
ctest --preset unit

cmake --preset macos-unit-4k
cmake --build --preset unit-test-binaries-4k
ctest --preset unit-4k

# Equivalent authoritative configure/build/unit/tooling workflows.
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k

# Standalone exact instruction-evidence pipelines. Each selection orders the
# CMake reset, all host producers, and the Python validator.
ctest --preset instruction-evidence
ctest --preset instruction-evidence-4k
```

Prepare full integration explicitly. Linux must be replayed on a
case-sensitive filesystem. The canonical source and page-specific kernel build
trees stay on the dedicated `/Volumes/<case-sensitive-volume>` volume at `linux`,
`build-16k`, and `build-4k`. Do not create or attach a repository-local
sparsebundle for the canonical workflow, and do not substitute a transient
case-sensitive mount under `/tmp`.

Source preparation, dependency-prefix construction, and Linux replay are
explicit mutating setup steps. Kernel preparation authenticates the official
signed `linux-6.18.41.tar.xz` release archive and detached signature, then
creates a local Git replay with real generation timestamps. Their verification
paths are read-only and are what ordinary builds should use once those inputs
exist:

```sh
dependencies/prepare.sh
dependencies/build.sh
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux
# A fresh offline replay can use the authenticated files already in the cache.
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux-offline \
  --no-fetch

dependencies/prepare.sh --verify
dependencies/verify-prefix.sh \
  --prefix .build/dependencies/macos-arm64 \
  --manifest dependencies/manifest.tsv \
  --build-script dependencies/build.sh \
  --deployment-target 14.0
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux --verify
kernel/verify-replay.sh --work-root /Volumes/<case-sensitive-volume> --skip-build
python3 tools/rootfs/verify.py --image rootfs.erofs
cmake --preset macos-integration
cmake --build --preset macos-integration
ctest --preset integration-fast
```

The checked integration presets pin `/Volumes/<case-sensitive-volume>/linux`,
`/Volumes/<case-sensitive-volume>/build-16k`, and
`/Volumes/<case-sensitive-volume>/build-4k`. For another case-sensitive layout, override
both cache paths during each configure rather than relying on an environment
prefix:

```sh
cmake --preset macos-integration \
  -DUML_KERNEL_SOURCE_DIR=/other/case-sensitive/linux \
  -DUML_KERNEL_BUILD_DIR=/other/case-sensitive/build-16k
cmake --preset macos-integration-4k \
  -DUML_KERNEL_SOURCE_DIR=/other/case-sensitive/linux \
  -DUML_KERNEL_BUILD_DIR=/other/case-sensitive/build-4k
```

`dependencies/prepare.sh --verify` authenticates the already prepared source
trees; it does not construct or verify the compiled dependency prefix.
`dependencies/build.sh` builds that private arm64 static prefix without network
access and publishes it transactionally. CMake configuration independently
checks current manifest/patch validation, the receipt's preparation/build-
policy identity, deployment target, complete payload observation, real archive
format, arm64 architecture, static linkage, and absence of ambient/staging
paths before integration targets can consume it. The payload hash protects one
built prefix against modification; it is not a cross-build byte-reproducibility
requirement.

Host build tools and the Apple SDK may come from the configured toolchain or
PATH. Production third-party library headers and binaries may come only from
the verified private prefix, never from system or package-manager locations.

`kernel/verify-replay.sh` creates and removes a fresh checkout, so its work root
must have several GiB free. Omit `--skip-build` to rebuild both page-size
kernels inside that replay. The materializer keeps its isolated GnuPG home on
a short secure temporary path to avoid Unix-domain socket limits under deeply
nested case-sensitive volumes; `UML_KERNEL_GPG_TEMP_ROOT` may override `/tmp`
for a restricted builder.

`integration-fast` runs host fixtures plus short real guest boots: the
production overlay/systemd path, interpreter, gadgets, JIT, and deterministic
SLiRP. The interpreter smoke intentionally stops after shell arithmetic and
CPU/HWCAP checks.

Keep the interpreter out of every heavy guest campaign, whether registered or
run by hand. It is far slower than the translating engines — enough that a
campaign finishing in minutes under gadgets or JIT may not finish in a
practical time under it — and it buys no semantic coverage, because the unit
suites already execute interpreter, gadgets, and JIT in order against one
oracle for each case. Full LTP, Docker, KDE native/VNC, WebGL, SMP and
shared-MM pressure, and every Geekbench measurement are therefore gadgets and
JIT only, and `uml_add_ltp_engine_campaign` is registered for those two
engines alone. Node/V8/Wasm coverage is likewise restricted to gadget and JIT
runs; do not add heavyweight Node.js, Go, or similar workloads to interpreter
acceptance.

When an interpreter-specific question does arise, answer it with a focused
probe instead of a campaign. A freestanding payload under
`tools/guest-shell.py --engine interpreter` boots in seconds and isolates the
instruction under test; that is how the missing `MRS <Xt>, DIT` encoding was
confirmed on all three engines without booting Docker under the interpreter.

`integration-full` (or `integration-full-4k` for the matching
page profile) additionally selects the 10-vCPU SMP and shared-MM pressure
tests. Shell-oriented workloads boot an explicit writable tmpfs
OverlayFS shell; that is intentional and avoids relying on a systemd getty for
PTY interaction, while `guest-overlay-systemd-smoke` validates the production
systemd path separately.  The shared-MM workload is a CMake-built freestanding
arm64/Linux executable, so the EROFS image does not need a guest compiler.

The bare CLI defaults to `init=/bin/sh`. Systemd-oriented launchers host-feed
their OverlayFS bootstrap to that temporary PID 1 shell, mount the EROFS lower
and tmpfs upper/work directories, then enter the overlay and start systemd.
They do not depend on a custom bootstrap executable inside the image. The
image owns the enabled tty0 getty and its login policy; launchers must preserve
that unit instead of creating a replacement symlink, drop-in, or autologin
override in the overlay. Before starting systemd, the bootstrap mounts tmpfs at
`/var/lib/docker` and `/var/lib/containerd` so Docker storage and containerd
state are not nested on the writable root OverlayFS. The
`guest-overlay-systemd-smoke` target builds a freestanding arm64/Linux
hello-world executable from repository source, packages it as a deterministic
rootfs archive, imports that archive into the disposable Docker state, and runs
the resulting local image with `--pull=never --network none`. The smoke has no
registry or external-network dependency. KDE/VNC and WebGL launchers reuse the
image's non-root `uml` account and must not create `plasma` or another desktop
account in the overlay.

Guest tests validate the EROFS image before and after each campaign, coordinate
with provisioning through the rootfs lock, map the shared image with `ubd0c=`,
and boot `root=/dev/ubda rootfstype=erofs`. EROFS supplies the read-only lower-
filesystem contract; do not add host-mode, `ubd0r`, or `ro` checks. The `c`
mapping disables UML's host-file lock but does not make raw `/dev/ubda` safe
scratch space, so no test may target the root block device for destructive
block-I/O probes. The wrapper's pre/post digest check remains the artifact-
integrity boundary. Guest tests obtain dynamic VNC ports through
LibVNCServer's 5900-and-up auto-port search.

Guest runners start the emulator **attached**, in the runner's own process
group, and signal that process directly. Whatever kills a runner's group -- a
shell job, a CTest timeout, Ctrl-C -- therefore takes its guest with it. Do not
reintroduce `start_new_session` or `killpg` here: a runner that is killed
outright runs no cleanup code, and a guest that survives one boots to idle and
still burns one to two host cores indefinitely, so a later benchmark silently
measures an oversubscribed host. `guest-harness-attached-session` pins both
halves of that property.
The VNC smoke is mandatory when registered: it does not convert a headless
graphics failure into a skip.

The KDE launcher opens a native Metal window by default. Select VNC explicitly
when a remote client or an automated capture runner is required:

```sh
python3 scripts/boot_kde_vnc.py
python3 scripts/boot_kde_vnc.py --display vnc --port 5900
```

The launcher always passes the host-only `--gpu-display=native|vnc` option.
`--bind`, `--port`, and `--password` configure only VNC and therefore require
`--display vnc`. The bare `uml-darwin --gpu` host option retains VNC
as its compatibility default, while the KDE launcher deliberately selects
native mode. Native keyboard, pointer, and wheel events use the same inherited
Virtio-input datagram boundary as VNC. The native Virgl path performs a GPU
blit from the renderer's CGL texture into a two-slot IOSurface bridge and Metal
presents that storage directly; there is no `glReadPixels`, CPU framebuffer
copy, or extra FIFO frame queue on this path. CPU readback remains the fallback
for unsupported scanout resources.

External network, KDE native/VNC, and rendered WebGL workflows are opt-in
because they need network/GUI permission:

```sh
cmake --preset macos-integration \
  -DUML_ENABLE_EXTERNAL_NETWORK_TESTS=ON
ctest --preset integration-full
```

`guest-docker-hello-world` joins that opt-in set. `guest-overlay-systemd-smoke`
already proves Docker runs a locally imported image with no network; this one
covers the path a real user takes instead — registry DNS and TLS over SLiRP, an
image pull, and container networking over the `docker0` bridge. It is also the
broadest Go exercise in the suite, because `dockerd`, `containerd` and the
client all read HWCAP-gated system registers during startup. Run it directly
against either engine with:

```sh
tools/rootfs/with-verified-rootfs.sh -- \
  python3 tests/integration/gpu/run_gpu_guest.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.erofs \
    --command "docker run --rm hello-world" \
    -- --execution-engine=gadgets
```

### JIT translation cache budget

The JIT sizes its per-executor block cache, link tables and code arena from a
whole-process budget split across the guest's CPUs. By default that budget is
half of guest memory, read from the `mem=` argument the host passes the guest,
so an eight-gigabyte ten-CPU guest spends about 3.4 GiB of host memory on
translated-code caches rather than a fixed amount that ignores the guest.

`--jit-cache-budget=` overrides the total. It is a host-only option, consumed
before the kernel command line is built, and accepts a byte count with an
optional `K`, `M` or `G` suffix:

```sh
.build/presets/macos-integration/bin/uml-darwin \
  --jit-cache-budget=1G ubd0c=rootfs.erofs root=/dev/ubda \
  rootfstype=erofs mem=8G ncpus=10
```

Lowering it trades throughput for memory: the campaign in G134 of
`docs/HOT_PATH_EXPERIMENTS.md` measures about 8% on multi-core Clang and Object
Detection between a 4 GiB and an 8 GiB budget. Raising it past half of guest
memory is allowed but the host must have the memory to back it.

Full LTP is independently selected, never a unit test. The unified image
contains it at `/opt/ltp`; run it separately per engine and preserve the rootfs
wrapper. Cases whose required kernel options are deliberately disabled belong
in `disabled-tests.tsv` rather than prompting config re-enablement:

```sh
cmake --build .build/presets/macos-integration --target guest-ltp-gadgets
cmake --build .build/presets/macos-integration --target guest-ltp-jit
# Convenience aggregate after both independent campaigns are desired:
cmake --build .build/presets/macos-integration --target guest-ltp-verified
python3 tests/integration/ltp/run_guest_ltp.py --help
```

A direct focused run must name the matching Kbuild metadata explicitly; the
runner validates its complete canonical path inventory and sibling kernel
release before booting cases:

```sh
tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.erofs -- \
  python3 tests/integration/ltp/run_guest_ltp.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.erofs \
    --manifest tests/integration/ltp/verified-tests.tsv \
    --modules-builtin /Volumes/<case-sensitive-volume>/build-16k/modules.builtin \
    --case containers/netns_sysfs
```

The CMake engine targets supply the corresponding
`${UML_KERNEL_BUILD_DIR}/modules.builtin` automatically. The harness copies
that complete file byte-exact into disposable OverlayFS state and replaces
`modules.dep` with an empty file only when the running config proves
`CONFIG_MODULES=n`. The generated setup is gzip-compressed on the host and
transported through independently acknowledged base64 append commands below
2 KiB; a final bounded command decodes, decompresses, executes, and removes the
staging files under `/run`, and the harness restores console echo on every
path. It also places fail-closed overlay-only `kmod`, `modprobe`, and `rmmod`
names ahead of the LTP binary directory in `PATH`. Fake loads are no-ops
limited to exact built-in names from the validated metadata; removal,
parameters, unknown names, and unsupported operations fail. The immutable
rootfs does not need kmod for this static-kernel accommodation.

The 10-core topology, utilization, and hotplug workload is also deliberately
outside CTest. Run its standalone target explicitly:

```sh
cmake --build .build/presets/macos-integration --target guest-smp-parallel
```

Provisioning an image is explicit and always verifies the manifested image
SHA-256; it refuses to replace an existing image without `--replace`. Local
artifacts must be authenticated out of band before being supplied with
`--source`. Network acquisition additionally verifies a detached signature
over the release digest statement: set the four `UML_ROOTFS_*` release
variables named in the manifest and pass `--allow-network`. The repository
intentionally cannot invent the release URL or trusted public key. EROFS
updates are complete image rebuilds followed by verified
`tools/rootfs/provision.py --replace`; in-place writable maintenance is
unsupported.

Use stock macOS commands when manually inspecting the image:

```sh
stat -f 'size=%z bytes' rootfs.erofs
shasum -a 256 rootfs.erofs
```

Guest benchmarks use payloads already installed under `/opt`; they do not
download inputs at run time. Geekbench 6 runs locally without an Internet
preflight or upload permission gate:

```sh
tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.erofs -- \
  python3 tests/perf/run_guest_geekbench.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.erofs \
    --console-log .build/results/geekbench-gadgets-16k.log \
    --engine gadgets --cpus 10 --memory 8G
```

The runner's default guest command is
`/opt/geekbench/release/geekbench6 --no-upload`.
See [`../tests/perf/README.md`](../tests/perf/README.md) for profiling and probe
commands. Full Geekbench scoring remains a separate manual campaign.

Sanitizer, iOS, XCFramework, benchmark, and guest-workload runs are separate
acceptance campaigns.  Typical host-product commands are:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan -j 10 \
  --resource-spec-file "$PWD/cmake/ctest-resources.json"

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan

# Canonical rootfs-free GPU/display descriptor-ownership gates.
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan

# Canonical malformed embedded-loader sanitizer gate.
cmake --preset macos-loader-asan-ubsan
cmake --build --preset loader-asan-ubsan
ctest --preset loader-asan-ubsan

cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

`unit-tsan` intentionally runs eight tests labelled `tsan`:
`emulator-native-16k-ownership`, `static-mmu-batch`,
`arm64-pc-alignment-contract`, `arm64-static-runtime`,
`arm64-jit-publication_gate`, `memory-domain-lifetime`,
`runtime-route-transaction`, and `runtime-context-lifecycle`. It does not
select the instruction-evidence reset. The full normal and ASan/UBSan suites
retain the complete registered ordinary instruction-test selection.

The two GPU workflows are host-only and rootfs-free. They configure the GPU
graph, build both GPU unit fixtures and all registered tooling fixtures plus
the architecture audit, run the GPU unit selection, and then run the
independently labelled `virtio-gpu-vnc-smoke` tooling check. This covers
service ownership, direct-presentation protocol bypass, VNC lifetime/port
reuse, and the real LibVNCServer wiring normally or under ASan+UBSan. The
interactive AppKit window remains an external GUI check; it is not opened by
ordinary host acceptance. Loopback socket policy must permit the VNC
port-reuse cases.

The strict `unit` selection is Python-free. In configurations that enable both
unit and tooling tests, it includes the CMake evidence reset so stale per-test
TSVs are removed before producers run. In a unit-only, tooling-disabled graph,
neither the reset nor the tooling validators are registered. The
`instruction-evidence` presets add the validator and are standalone after the
matching build preset. A raw `tooling` selection is non-destructive: in a
unit-enabled tree its evidence validator requires a preceding unit producer
run, as provided by `check-unit`; in the tooling-only tree that conditional
validator is not registered.

With `UML_INSTRUCTION_INPUT_MATERIALIZATION=OFF`, the ordinary combined
unit/tooling graph writes no input sidecars and does not register
`arm64-input-materialization-join`. Setting the option to `ON` makes each unit
producer record one compressed sidecar and registers exactly that join. The
join deliberately requires the complete producer set, so selecting it without
first completing the matching unit run fails instead of yielding a partial
index. Raw sidecars stay under the build tree. The resumable checkpoints and
contract index can be placed on a larger volume by setting the CMake cache path
`UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT`; page-size-specific subdirectories
are added automatically. The registered join preserves at least 5 GiB on the
checkpoint volume and rejects a producer before its bounded working estimate
would consume that reserve. This opt-in exhaustive tooling gate covers roughly
30 million contributions and 449 producer shards per page profile, uses
resumable checkpoints, and has a bounded four-hour CTest timeout.

## Exact official A64 campaign

The `arm64-aslref-campaign-*` CTests are host-only fixtures for publication,
authority reconstruction, sharding, cancellation/resume, and final joining.
They do **not** execute the complete official campaign. The complete campaign
starts with fresh 4 KiB and 16 KiB unit evidence, reconstructs all 449 producer
checkpoints for each profile, and then executes the commands below. It can run
for days and must remain outside ordinary CTest.

Use a new absolute work root on a local filesystem. Output, scratch, and run
roots are siblings, not nested within one another or any frozen input. The
example official-tool paths are the paths found by the 2026-08-05 audit;
replace any that no longer exist. `PDF` must point at the exact source PDF.
Every `test` below must succeed before proceeding.

```sh
REPO=/Users/dev/Downloads/linux-um-darwin
WORK=/Volumes/<case-sensitive-volume>/a64-official-2026-06-run-001
ARCH_ROOT=/private/tmp/a64-asl-2026-06.3oq9iS/ISA_A64_xml_A_profile-2026-06
ARCHIVE=/private/tmp/ISA_A64_xml_A_profile-2026-06.tar.gz
FEATURES=/private/tmp/a64-features-2026-06.S4QKh4/Features.json
ASLREF=/private/tmp/aslref-inspect.k1syjc/herdtools7-aslref-v1.0_draft.2/_build/default/asllib/aslref.exe
BUNDLER=/private/tmp/aslref-inspect.k1syjc/herdtools7-aslref-v1.0_draft.2/herd/libdir/asl-pseudocode/bundler.py
PDF=/Volumes/<case-sensitive-volume>/spec/ISA_A64_xml_A_profile-2026-06.pdf

test ! -e "$WORK"
test -d "$ARCH_ROOT"
test -f "$ARCHIVE"
test -f "$FEATURES"
test -x "$ASLREF"
test -f "$BUNDLER"
test -f "$PDF"

mkdir -p "$WORK"/join-4k "$WORK"/join-16k \
  "$WORK"/shards-4k "$WORK"/shards-16k \
  "$WORK"/local-shard-4k "$WORK"/local-shard-16k \
  "$WORK"/local-plan-4k "$WORK"/local-plan-16k \
  "$WORK"/plans "$WORK"/run-4k "$WORK"/run-16k \
  "$WORK"/local-run-4k "$WORK"/local-run-16k \
  "$WORK"/local-verify-4k "$WORK"/local-verify-16k \
  "$WORK"/local-promotion "$WORK"/promotion

cd "$REPO"
cmake --preset macos-unit-4k \
  -DUML_INSTRUCTION_INPUT_MATERIALIZATION=ON
cmake --build --preset unit-test-binaries-4k
ctest --preset unit-4k
cmake --preset macos-unit-16k \
  -DUML_INSTRUCTION_INPUT_MATERIALIZATION=ON
cmake --build --preset unit-test-binaries
ctest --preset unit

EVIDENCE_4K="$REPO/.build/presets/macos-unit-4k/instruction-evidence"
EVIDENCE_16K="$REPO/.build/presets/macos-unit-16k/instruction-evidence"
test "$(find "$EVIDENCE_4K" -name '*.tsv.inputs.jsonl.gz' -type f | wc -l | tr -d ' ')" -eq 449
test "$(find "$EVIDENCE_16K" -name '*.tsv.inputs.jsonl.gz' -type f | wc -l | tr -d ' ')" -eq 449

python3 -B tests/tooling/arm64/join_a64_input_materializations.py \
  --evidence-directory "$EVIDENCE_4K" \
  --guest-page-size 4096 --require-execution-ready \
  --output "$WORK/join-4k/index.tsv" \
  --contract-index-output "$WORK/join-4k/contracts.tsv.gz" \
  --checkpoint-directory "$WORK/join-4k/checkpoints" \
  --free-space-reserve-gib 20
python3 -B tests/tooling/arm64/join_a64_input_materializations.py \
  --evidence-directory "$EVIDENCE_16K" \
  --guest-page-size 16384 --require-execution-ready \
  --output "$WORK/join-16k/index.tsv" \
  --contract-index-output "$WORK/join-16k/contracts.tsv.gz" \
  --checkpoint-directory "$WORK/join-16k/checkpoints" \
  --free-space-reserve-gib 20

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_4K" --guest-page-size 4096 \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-4k" \
  --local-temp-root "$WORK/local-shard-4k" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_16K" --guest-page-size 16384 \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-16k" \
  --local-temp-root "$WORK/local-shard-16k" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require

TOTAL_4K=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["total_contracts"])' "$WORK/shards-4k/shard-set.json")
TOTAL_16K=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["total_contracts"])' "$WORK/shards-16k/shard-set.json")
test "$TOTAL_4K" -gt 4096
test "$TOTAL_16K" -gt 4096

# Re-run in exact-resume mode so the derived totals become explicit gates.
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_4K" --guest-page-size 4096 \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-4k" \
  --local-temp-root "$WORK/local-shard-4k" \
  --expected-total-contracts "$TOTAL_4K" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_16K" --guest-page-size 16384 \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-16k" \
  --local-temp-root "$WORK/local-shard-16k" \
  --expected-total-contracts "$TOTAL_16K" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py plan \
  --campaign-id a64-2026-06-4k \
  --input-list "$WORK/shards-4k/shard-input-list.txt" \
  --output "$WORK/plans/campaign-4k.json" \
  --local-temp-root "$WORK/local-plan-4k" \
  --minimum-local-free-gib 20 --guest-page-size 4096 \
  --expected-total-contracts "$TOTAL_4K" \
  --architecture-root "$ARCH_ROOT" --aslref "$ASLREF" \
  --bundler "$BUNDLER" --features-json "$FEATURES" \
  --closure-capture-directory "$EVIDENCE_4K" \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --timeout-seconds 1800 --max-rss-kib 12582912 \
  --max-encoded-bytes 134217728 --max-artifact-bytes 536870912 \
  --max-artifact-files 4096 --max-log-bytes 8388608 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --freeze-path "$ARCHIVE" --freeze-path "$PDF"
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py plan \
  --campaign-id a64-2026-06-16k \
  --input-list "$WORK/shards-16k/shard-input-list.txt" \
  --output "$WORK/plans/campaign-16k.json" \
  --local-temp-root "$WORK/local-plan-16k" \
  --minimum-local-free-gib 20 --guest-page-size 16384 \
  --expected-total-contracts "$TOTAL_16K" \
  --architecture-root "$ARCH_ROOT" --aslref "$ASLREF" \
  --bundler "$BUNDLER" --features-json "$FEATURES" \
  --closure-capture-directory "$EVIDENCE_16K" \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --timeout-seconds 1800 --max-rss-kib 12582912 \
  --max-encoded-bytes 134217728 --max-artifact-bytes 536870912 \
  --max-artifact-files 4096 --max-log-bytes 8388608 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --freeze-path "$ARCHIVE" --freeze-path "$PDF"

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py pilot \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-run-4k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py pilot \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-run-16k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require

# These commands resume the four pilot shards and run every remaining shard.
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py run \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-run-4k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py run \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-run-16k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20 \
  --directory-sync-policy require

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py verify \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-verify-4k" --minimum-local-free-gib 20 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py verify \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-verify-16k" --minimum-local-free-gib 20 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184

python3 -B tests/tooling/arm64/join_a64_aslref_campaign_results.py \
  --campaign-4k-plan "$WORK/plans/campaign-4k.json" \
  --campaign-4k-root "$WORK/run-4k" \
  --campaign-16k-plan "$WORK/plans/campaign-16k.json" \
  --campaign-16k-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-promotion" --minimum-local-free-gib 20 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --output "$WORK/promotion/a64_aslref_behavior_status.tsv" \
  --identity-output "$WORK/promotion/a64_aslref_behavior_status.identity.json"
python3 -B tests/tooling/arm64/join_a64_aslref_campaign_results.py \
  --campaign-4k-plan "$WORK/plans/campaign-4k.json" \
  --campaign-4k-root "$WORK/run-4k" \
  --campaign-16k-plan "$WORK/plans/campaign-16k.json" \
  --campaign-16k-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-promotion" --minimum-local-free-gib 20 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --output "$WORK/promotion/a64_aslref_behavior_status.tsv" \
  --identity-output "$WORK/promotion/a64_aslref_behavior_status.identity.json" \
  --verify-only
```

The first sharding pass derives the exact campaign counts from the freshly
joined authority; never substitute aggregate-row counts or an older run's raw
contract totals. The current frozen aggregate identity has 32,169 rows
(16,084 at 4 KiB and 16,085 at 16 KiB), ID digest
`00ebf4703abb2e9c59050d4e7f00418dcb6b5f61f0e61dddbe566c51f2057d66`,
and row digest
`f42d770d290b88135f5da3eb04793d516cd89723e614b4e2911499fcc5541c0`;
those are aggregate identities, not campaign totals. The derived shard count
is `ceil(TOTAL/1024)`, and the pilot itself requires the first four shards to
contain exactly 1,024 contracts each.

`require` durability makes unsupported directory `fsync` fatal. Sharding and
campaign publication fsync files before dependent metadata and sync staging,
shard, and output directories before success. Run/pilot recheck both reserves
before each submission and after every completion using the frozen artifact,
file-count, and combined-log caps for every active worker. On interruption,
only exact owned `.a64-campaign-*` local workspaces and shard partials are
recovered; published `SUCCESS.json` trees are verified and retained byte for
byte. Sharding holds the canonical exclusive producer/evidence locks across
source identity, authority reconstruction, generation, and final verification,
so one shard set is a quiescent snapshot rather than a mixture of producer
generations. A probe using `--generated-directory` rejects placement in any
source, input, or capture tree and checks its explicit
`--minimum-generated-free-gib` reserve plus the artifact hard cap before it
creates the directory. Promotion locks both profile roots in canonical order,
keeps per-input results in its size-capped, locally rooted SQLite database,
streams indexed bindings one aggregate at a time, and verifies the complete
frozen join before publishing or verifying its output pair.
The 128 GiB database caps are deliberate: the bounded promotion scale fixture
measured 1,043 bytes per input at its peak after including the authority,
campaign-ID, result, and normalized per-shard tables. The capacity model rounds
to 1,536 bytes, allows 40 million inputs, and applies a 3/2 safety factor
(92,160,000,000 required bytes). The promotion identity binds both that model
and the actual database/scratch caps.

`scripts/build-xcframework.sh` builds a complete aggregate archive, verifies
its exact production-object membership and absence of test/profiling hooks,
and links external C and C++ consumers for macOS, iOS device, and iOS
simulator. It also rejects non-JIT executable-memory publication references in
either iOS archive.
Interpreter, gadgets, native-shadow mappings, and software-MMU storage remain
data-only: no W+X mapping and no RW/RX transition. Executable publication is
retained only in the explicitly selected JIT arena for entitled
debugger/jailbreak use.

The current refactor evidence index and the local raw-result locations are in
[`refactor-evidence.md`](refactor-evidence.md).

They require the permissions and host hardware described by their commands; do
not convert an unavailable GUI, network, physical device, or performance host
into a passing result.
