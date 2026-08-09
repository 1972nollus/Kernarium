# Testing and preparation

## Latest accepted local baseline (2026-08-09)

Implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97` passed the ordinary 16 KiB and
4 KiB unit suites at 572/572 and 573/573, respectively, with tooling at 51/51
in each profile. Full ASan+UBSan passed 572/572, TSan passed 8/8, the malformed
loader ASan gate passed 1/1, and both normal and sanitized GPU configurations
passed 5/5 units plus the 1/1 VNC tooling test. `integration-fast` passed 5/5
in each page profile, and the standalone SMP workload reached 8.19 and 8.03
effective host cores at 16 KiB and 4 KiB. The three-slice Apple package and its
raw, installed-tree, and real Xcode consumers passed. The immutable rootfs
remained mode 0444, 8,589,934,592 bytes, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.

These results were produced locally; no hosted CI workflow exists. The LTP
payload was absent, so full LTP was not run. Instruction input materialization
was off and emitted zero sidecars; it remains an explicit, storage-intensive
opt-in. Rootfs-management redesign remains deferred. The final Geekbench rerun
was intentionally skipped at the user's direction after all other acceptance
tasks completed, so this baseline makes no new end-to-end Geekbench claim.

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
metadata or requiring byte equality. The signed `rootfs.img` digest is
intentionally stricter because it identifies an immutable input.

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
case-sensitive filesystem. A local case-sensitive APFS sparse bundle is one
portable option when no suitable volume already exists:

```sh
hdiutil create -size 80g -type SPARSEBUNDLE -fs APFSX \
  -volname Kernel .build/Kernel.sparsebundle
hdiutil attach .build/Kernel.sparsebundle
```

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
python3 tools/rootfs/verify.py --image rootfs.img
UML_KERNEL_SOURCE_DIR=/Volumes/<case-sensitive-volume>/linux \
  cmake --preset macos-integration
cmake --build --preset macos-integration
ctest --preset integration-fast
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
CPU/HWCAP checks; Node/V8/Wasm coverage is restricted to gadget and JIT runs.
Do not add heavyweight Node.js, Go, or similar workloads to interpreter
acceptance. `integration-full` (or `integration-full-4k` for the matching
page profile) additionally selects the 10-vCPU SMP and shared-MM pressure
tests. Shell-oriented workloads boot an explicit writable tmpfs
OverlayFS shell; that is intentional and avoids relying on a systemd getty for
PTY interaction, while `guest-overlay-systemd-smoke` validates the production
systemd path separately.  The shared-MM workload is a CMake-built freestanding
arm64/Linux executable, so the immutable rootfs does not need a guest compiler.

Guest tests validate the lower image before and after each campaign, serialize
it with the rootfs lock, obtain dynamic VNC ports through LibVNCServer's
5900-and-up auto-port search, and terminate guest process groups.  The VNC
smoke is mandatory when registered: it does not convert a headless graphics
failure into a skip.

External network, KDE/VNC, and rendered WebGL workflows are opt-in because
they need network/GUI permission:

```sh
UML_KERNEL_SOURCE_DIR=/Volumes/<case-sensitive-volume>/linux \
  cmake --preset macos-integration \
  -DUML_ENABLE_EXTERNAL_NETWORK_TESTS=ON
ctest --preset integration-full
```

Full LTP is independently selected, never a unit test.  It requires an
immutable image that contains `/usr/lib/ltp-testsuite`; run it separately per
engine and preserve the rootfs wrapper:

```sh
cmake --build .build/presets/macos-integration --target guest-ltp-gadgets
cmake --build .build/presets/macos-integration --target guest-ltp-jit
# Convenience aggregate after both independent campaigns are desired:
cmake --build .build/presets/macos-integration --target guest-ltp-verified
python3 tests/integration/ltp/run_guest_ltp.py --help
```

The 10-core topology, utilization, and hotplug workload is also deliberately
outside CTest. Run its standalone target explicitly:

```sh
cmake --build .build/presets/macos-integration --target guest-smp-parallel
```

Provisioning an image is explicit and verifies a detached signature over a
small digest statement plus the manifested image SHA-256; it refuses to
replace an existing image without `--replace`. Use publisher-supplied local
artifacts, or set the four `UML_ROOTFS_*` release variables named in the
manifest and pass `--allow-network`. The repository intentionally cannot invent
the release URL or trusted public key. Authorized writable-image maintenance is
`tools/rootfs/maintenance.py` and is intentionally outside all presets.

Use stock macOS commands when manually inspecting the image:

```sh
stat -f 'mode=%Sp size=%z bytes' rootfs.img
shasum -a 256 rootfs.img
```

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

# Canonical rootfs-free GPU/VNC descriptor-ownership gates.
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
service ownership, VNC lifetime/port reuse, and the real LibVNCServer wiring
normally or under ASan+UBSan. Loopback socket policy must permit the VNC
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
