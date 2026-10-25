# Virtio Sound

`UML::AudioDevice` serves Virtio device 25 over the common vhost-user
transport. It exposes two 48 kHz signed-16-bit streams in one function group,
which the guest driver presents as the two directions of PCM device 0: stereo
playback, and capture of one or two channels. Mixer controls and jacks are not
exposed. The device owns protocol validation and stream state; the host output
belongs to a `PlaybackSink` and the captured audio to a `CaptureSource`.

macOS and Android enable the service with `--audio`; Kernarium supplies it to
every guest it runs.

## Threads and the event descriptor

The transport worker runs the device. The Apple output signals it from
AVFAudio callbacks through a bounded completion ring, and the callback path
does not allocate or lock. iOS owns `AVAudioSession` interruption and route
recovery inside `ApplePlaybackSink`.

The worker polls one descriptor, which the device's `PeriodClock` owns: a
kqueue that watches a wake socket and one timer. The clock is the only time
base the device uses, so tests inject one and advance it by hand. The device
adds no thread of its own, and the timer is served by the worker's own poll.

## Playback pacing

The guest's playback pointer advances with each completion, so the device must
complete periods at the rate real playback would. When the host output cannot
play, the device discards the data and completes each period one period of
real time after the one before, on its `PeriodClock`:

- `ApplePlaybackSink::enqueue` reports `discarded` once the output has faulted:
  a default output that will not start, or a configuration change that the
  engine could not recover from. A start the output refuses does not fail the
  guest's `PCM_START`; the stream runs and the next route change or the end of
  an interruption tries the output again.
- A period the sink had accepted and then dropped, because of a route change
  or a release, completes as `discarded` and is paced the same way.
- A stopped stream holds its queued periods and times them again when it
  starts. A released stream returns every queued period at once, as the Virtio
  sound specification requires.

The guest therefore sees a steady device whatever the Mac's output does, and
never an I/O error from an output problem.

## Capture

While the guest records, the device completes one receive buffer per period of
real time on its `PeriodClock`, from `PCM_START` on, whatever the capture
source holds. The guest's capture clock therefore never depends on a source:
it is neither stalled by a slow one nor raced by a fast one. The device credits
the periods that have passed, up to the stream's buffer, so a guest that falls
behind loses the oldest audio and the next period stays on the original grid.
Buffers the driver queues before `PCM_START` wait for it, and a stopped or
released stream returns its queued buffers without data.

The audio comes from a `CaptureSource`, which fills the front of each period
and leaves the rest zero. A source cannot fail a stream: one that cannot
deliver audio returns nothing and the guest records silence. The default
source is silent, so an unmodified product delivers zeros in real time, which
is what the Android audio policy's built-in microphone needs to open input
streams. The microphone source is described below.

## Microphone

The product records from the host microphone only when the user turns the
Microphone setting on. Off is the default: the guest records silence, nothing
reads or opens the microphone, and no permission prompt can appear.

- `MicrophoneSwitch` holds the setting and the system's permission behind it
  (`UML::MicrophoneAuthorization`, over `AVAudioApplication`). The user's act of
  turning the setting on, `enable()`, is the only call that can show the system
  prompt; `restore()` carries an earlier choice into a new run and never asks.
  The setting is on, permitted and `active()`, or it is not, and a denied or
  not-yet-asked microphone is the same silence as an off one.
- `MicrophoneCaptureSource` starts the `MicrophoneInput` only while the guest
  records and the switch is active, and stops it when the guest stops, the
  switch turns off or the device resets, so the system's recording indicator
  is lit only while a guest listens. The input's audio thread writes a bounded
  `CaptureRing`; the source keeps at most 250 ms of backlog, so a guest that
  was not recording hears the present, not the past. A turn of the switch wakes
  the transport worker, which decides on its own thread.
- `AppleMicrophoneInput` taps the engine's input node and converts to 48 kHz
  signed 16-bit in the guest's channel count. A route change restarts the
  engine; on iOS an interruption stops the microphone and its end starts it
  again.
- The audio session is shared with the output (`audio_session.hpp`): playback
  alone uses the playback category, and while the microphone is captured the
  category is play-and-record, still mixing with other audio. macOS has no
  audio session.

macOS enables it with `--audio-input=host` (`silent` is the default; either
value implies `--audio`; `tools/boot_android.py --audio-input host` passes it):
the launcher turns the switch on at start, which is when macOS asks, and prints
the answer. A launcher run from a terminal is asked on behalf of the app that
started it. Kernarium's Microphone setting is
described in [`../../../Kernarium/README.md`](../../../Kernarium/README.md).

## Null output

`--audio-output=null` selects a sink that plays nothing: every period is
discarded and paced as above. A conformance campaign uses it so that tone-playing
tests make no sound. `host` is the default; either value implies `--audio`.
`tools/boot_android.py --audio-output null` passes it to the Android launcher,
and Kernarium keeps the host output.

## Tests

`virtio-sound-protocol` covers the protocol, both streams' lifecycles, the
pacing and capture delivery under an injected clock, and the production clock.
`virtio-sound-microphone` covers the setting, the ring and the capture source
over fakes; it never opens the microphone or shows a permission prompt.
`UML_TEST_APPLE_AUDIO=ON` adds `apple-audio-playback`, which plays a brief tone
through the real output (see
[`../../../docs/testing.md`](../../../docs/testing.md)).

The real microphone is checked by hand, because granting access is the user's
act: start a guest with `--audio-input=host`, answer the prompt, and record in
the guest (`tinycap` on Android, `arecord` on Linux). The recording indicator
lights while the guest records and goes out when it stops, and the capture
holds the room's sound. With the setting off, or access denied, the capture is
silence at the same rate.
