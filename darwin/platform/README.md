# Platform video encoder

`video_encoder.hpp` is the platform's hardware video encoder interface; nothing
in it names VideoToolbox, V4L2 or the Virtio wire. The Apple backend is
`apple/video_encoder.mm` (sessions), `apple/video_encoder_discovery.mm`
(capability discovery and its cache) and `apple/video_encoder_bitstream.hpp`
(Annex B, SPS, PPS and SEI readers). It builds as `UML::VideoEncoder` for macOS,
iOS device and iOS Simulator, and only through public VideoToolbox API.
The [Virtio encoder
device](../device/video/README.md#encoder-device-id-30) maps this interface to
the wire and depends on nothing below it.

## Discovery

`videoEncodeCapabilities()` returns what an encode on this host proved. A
profile, input layout, size, rate-control mode, QP bound, bitrate, B-frame
run, temporal layer count, entropy mode or HDR metadata is listed only after a
stream that used it came out right: the SPS states the profile, the PPS the
entropy mode, the SEI the HDR metadata, a frame the layer it belongs to. VideoToolbox
lists no ranges, so discovery drives the production encoder through an
unchecked configure and every probe exercises the pipeline a guest's stream
does. A rate-control mode is published only when its parameter moves the size of
the stream the right way round, because VideoToolbox accepts properties it
ignores. `prepareVideoEncodeCapabilities()` starts discovery on its own thread;
the first query waits for it, at most five minutes, and a host that answers
nothing then reports no encoder.

The result is cached as JSON keyed by `hw.machine`, `hw.model`,
`kern.osversion` and a revision of the file format. `UML_VIDEO_ENCODE_CACHE`
names the file; an empty value turns the cache off. The default is the
temporary directory on macOS and the Caches directory on iOS. A file that is
missing, from another host or revision, or not what a writer of this revision
stores is ignored and replaced. A session that discovery promised and
VideoToolbox then refuses removes the file, so the next process rediscovers.

Profiles a guest API cannot name (ProRes, HEVC range extensions, alpha, depth)
are not probed or listed.

## Sessions

Hardware is required. A frame is written once, by the caller, into a pixel buffer
from the session's own pool and handed to the encoder without a second copy.
At most four frames (six with B-frames) are in flight; `acquireFrame()` returns
null beyond that, and a frame dropped unsubmitted returns its slot.

- **Output** is Annex B. Parameter sets come from the format description and are
  placed as the settings ask: joined to a key frame or emitted as a frame of
  their own, repeated at every key frame or not. HDR10 metadata is written as a
  prefix SEI (payload types 137 and 144) before every key frame, because
  VideoToolbox keeps it only in the format description. A JPEG image gets the
  JFIF segment the hardware leaves out.
- **Frame type** is read from the sample: key, predicted, or bidirectional when
  its presentation time is below one already returned. Output is in decode
  order. The longest B-frame run cannot be limited; discovery reports the most
  it saw, and a stream is promised no more.
- **Levels.** An explicit H.264 level is honored when the stream fits it and
  VideoToolbox names it; otherwise the encoder chooses (a level below what the
  stream needs makes every frame fail). An HEVC level is only a ceiling.
- **Sizes** are even for video; JPEG keeps odd sizes exactly.

### Rate control

The standard encoder (`ave`) is real-time by default here: it holds its target
far closer than non-real-time mode and, unlike it, takes a bitrate change while
a stream runs. Only a guest that asks for low latency also trades quality for
speed.

| Mode | Property | Notes |
| --- | --- | --- |
| variable | `AverageBitRate`, `DataRateLimits` for the peak | |
| constant bitrate | `ConstantBitRate` | |
| constant quality | `Quality` = q/100 | `ConstantQualityFactor` is accepted and has no effect |
| constant QP | min = max QP on the standard encoder | the low-latency encoder takes `BaseFrameQP` per frame |

The low-latency encoder (`rtvc`) is chosen by the platform, never by the caller,
when a stream needs what only it does: two temporal layers, or a QP per frame
type (it then places the key frames itself). It writes fewer profiles than the
standard encoder, so a request it cannot serve is refused or falls back to one
QP. Layers cannot combine with B-frames or constant bitrate.

`update()` applies a bitrate, peak or frame rate change to the running session.
VideoToolbox ignores a changed quality or maximum QP under a running session, so
those replace the session between two frames: everything in flight is delivered,
and the stream continues from a new session's key frame.

### Errors and recovery

`configure()` answers `invalid` for arguments no encoder could honor, `unsupported`
for what this host's discovery did not prove or VideoToolbox refuses, `exhausted`
when no hardware session is left, and `failed` otherwise. A frame the encoder
could not code is answered by an `error` entry with its timestamp; every
submitted frame is answered exactly once.

A session the system revokes is rebuilt at the same settings, and the frames
that never got an answer are encoded again, the first as a key frame. A second
loss with no output in between ends the stream: every frame in flight and every
later one is answered with an error until `reset()`, which starts a new stream at
a key frame. `drain()` follows a recovery and flushes the session that replaced
the lost one.

## Tests

`platform-video-encoder-bitstream` checks the readers against the decoder
fixtures and needs no hardware. `platform-video-encoder` exercises every
advertised profile and layout, each rate-control mode and update, key frames,
B-frames, layers, parameter sets, HDR, colour, levels, sizes, flow and limits,
and decodes the streams with the platform decoder to compare pictures.
`platform-video-encoder-recovery` links a variant of the backend that fails
sessions on demand; `platform-video-encoder-cache` runs child processes against
valid, missing and unusable cache files. `Kernarium/KernariumTests/HostVideoEncoderTests.mm`
records an iPad's capabilities as a test attachment and checks its streams.
