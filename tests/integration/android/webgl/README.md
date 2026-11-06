# Android WebGL memory workload

`AndariumWebGLProbe` runs the public WebGL Aquarium with 5,000 fish in Android
WebView at the guest display's pixel dimensions. It requires hardware rendering,
an intact WebGL context, advancing animation frames and 60 positive FPS samples
after warmup. Results include the renderer, canvas dimensions and frame rate;
this is a stress workload, not an assertion of interactive performance.

Build this directory as an explicit Soong module in the configured Android
workspace (`m AndariumWebGLProbe`), with a temporary source copy at
`test/andarium-webgl`. It is not a product package. Remove the source copy and
any installed product-output copy after exporting the APK. Copy the APK to
Andarium's `Library/Caches/andarium-webgl-probe.apk` before each run.

Run only `AndariumTests/RetinaMemoryTests` on a physical iPad using
`xcodebuild test`, passing these environment variables with the `TEST_RUNNER_` prefix:

```sh
ANDARIUM_RETINA_MEMORY_TESTS=1
ANDARIUM_GUEST_INSTANCE=retina-memory
ANDARIUM_RETINA_PROFILE=1
ANDARIUM_WEBGL_PROFILE=1
KERNARIUM_GUEST_MEMORY_MIB=1536
KERNARIUM_VCPU_COUNT=6
KERNARIUM_FOOTPRINT_LOG_MS=1000
```

The test runs two rounds of six installed system apps and WebGL, closing apps
between rounds, then powers the guest off cleanly. `ANDARIUM_RETINA_PROFILE=0`
compares point resolution. `ANDARIUM_GPU_CACHE_TRIM_PROFILE=1` additionally
measures unused GL driver caches after closing apps. To profile the existing
guest instead of the isolated guest, omit `ANDARIUM_GUEST_INSTANCE` and explicitly
set `ANDARIUM_PROFILE_EXISTING_GUEST=1`.

Collect `tmp/RetinaMemory/` from the app container afterward. Its JSONL snapshots
include the process footprint and limit, graphics ledger, Metal allocations,
malloc usage and VM regions, without allocation contents. WebGL reports and
guest memory summaries accompany them; each run replaces this temporary
directory. The initial 20-second pause permits Instruments attachment.
Compare settled allocations across rounds and after
shutdown; a process peak alone cannot establish a leak. Shutdown requires zero
wired pages and no dirty or swapped guest-domain or scratch-heap pages. The
test preserves display, motion and screen-timeout preferences, uninstalls the
probe and consumes the cached APK. It needs network access to load the Aquarium.
