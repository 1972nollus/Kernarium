# Android motion input probes

`AndariumMotionProbe` is a foreground application using Android's public
SensorManager API. Build this directory as an explicit Soong module in the
Android workspace (`m AndariumMotionProbe`); it is not a product package and
is never installed in a release image. Place the temporary source copy at
`test/andarium-motion` in that workspace. After exporting the APK, remove that
copy and `system/app/AndariumMotionProbe` below the product output before a
release build.

Copy its APK into Andarium's `Library/Caches/andarium-motion-probe.apk` with
`devicectl device copy to`. From the repository root, run:

```sh
TEST_RUNNER_ANDARIUM_MOTION_TESTS=1 xcodebuild test \
  -project Andarium/Andarium.xcodeproj -scheme Andarium \
  -destination 'platform=iOS,id=<device-id>' \
  -only-testing:AndariumTests/MotionOverVsockTests
```

This automated run uses the application's actual scene model and runtime, installs
the probe over private adb, and checks the Motion Sensors switch off, on and off
again. The probe requests both sensors at 50 Hz and reports finite, monotonic
samples, gravity in m/s², angular speed in rad/s, and physical changes. The
75-second capture additionally checks that both streams remain current beyond
an ordinary idle minute. A run identifier binds each result to its request.

The test temporarily extends Android's screen timeout and wakes its display;
Android's display sleep is separate from iOS's idle timer. It restores the
timeout and previous motion choice, uninstalls its probe, and waits for a clean
guest shutdown, including after a probe failure.

For the separate hands-on check, copy the cached APK again and run the same
suite with `TEST_RUNNER_ANDARIUM_MOTION_INTERACTIVE=1` and
`TEST_RUNNER_ANDARIUM_ROTATION_VISUAL=1`. This does not repeat the 75-second
capture. Gently tilt and rotate the iPad while the probe is visible; the test
waits up to ten minutes for real movement and fails immediately on stale or
missing sensor input. A stationary device does not pass.

With Control Center's rotation lock off, then rotate the iPad in full screen,
switch to a window and rotate again, and return to full screen. The scene and
guest settings are checked automatically; the person checks the displayed
orientation. The foreground probe shows live sensor values throughout, with
another 20 seconds to check rotation after returning to full screen.

The foreground application is necessary for sustained validation: Android can
disable a plain shell UID's sensor subscription after that UID becomes idle,
independently of host delivery. Each run consumes the cached APK, so copy it
again before a later run.

The separate `ANDARIUM_COMPLETE_UPDATE_ONLY=1` test (passed as
`TEST_RUNNER_ANDARIUM_COMPLETE_UPDATE_ONLY=1` to `xcodebuild`) boots and cleanly
powers off the current image without testing sensors; it can finish an update
interrupted by a terminated test host.
