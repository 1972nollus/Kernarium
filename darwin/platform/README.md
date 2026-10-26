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
  order. B-frames are coded in one pattern that no property shortens
  (`MaxFrameDelayCount` and `ReferenceBufferCount` are refused by the hardware
  encoders): discovery reports its run (`maximumBFrames`, 3), and `bFrames` at
  or above it codes the pattern. A request below it codes no B-frames, so the
  output is never more out of order than the caller allowed.
- **Levels.** A stream states at least the level it was asked for, and at
  least the lowest level its picture size, frame rate and bit rate fit (H.264
  Table A-1; HEVC Tables A.8 and A.9, Main tier). VideoToolbox names few
  levels and weighs no bit rate (a CIF stream at 832 kbit/s comes out as level
  1.3, whose limit is 768), so the encoder rewrites the level in the
  parameter sets (the SPS, and the VPS of HEVC) when VideoToolbox's differs.
  It raises a lower one. It lowers a higher one, from VideoToolbox's choice to
  the lowest level at or above the floor that the stream provably fits, and
  otherwise keeps it. What is proven, from the parameter sets VideoToolbox
  wrote and the settings: the coded picture size and the sides of it, the
  frame rate (the settings' and the SPS timing information's), the references
  and the DPB (the stated one, or the references plus a reorder lag of three
  where B-frames may reorder), the vertical motion vector range, interlace,
  and any HRD parameters; and the bit rate and the CPB, only where the rate
  control bounds the rate (a constant bit rate, or a variable one with a
  peak, whose data-rate limit is a second long): the bound must be within the
  level's rate and CPB, and within the caller's buffer size. A stream the
  rate control leaves unbounded (constant quality, constant QP, a variable
  rate without a peak) is never lowered. HEVC is lowered only as a single
  layer in the Main tier, without tiles or HRD parameters. Level 1b is stated
  only where it is asked for.
  The rate control is a controller, not a guarantee: a QCIF stream at
  64 kbit/s spends two to three times its rate in the first second, on the
  key frame and while the rate control converges, so the rate and the CPB are
  the settings' envelope and the stream's first seconds can exceed it.
  The hardware codes no H.264 picture narrower than 192 samples or shorter
  than 96 (HEVC: 160 wide), and a picture is coded with the macroblocks it
  has: QCIF is 12x9 macroblocks, 108 against level 1.0's 99, and 1620 a second
  at 15 frames against 1485. The level of QCIF is 1.1 or higher, whatever was
  asked for.
  A rate or frame rate update that the stated level cannot hold starts a new
  session at a key frame, whose parameter sets state a level that does. A
  level VideoToolbox does name, and the stream fits, is requested from it as
  well; one it refuses for the picture's shape (Baseline 1.3 for a picture
  704 samples high) is left to its own choice.
- **Sizes** are even for video; JPEG keeps odd sizes exactly.

### Input layouts and colour

A layout is listed for a profile only when an encode from it came out right.
HEVC Main 10 takes P010, the 10-bit input that keeps its precision, as well as
the 8-bit layouts, which the encoder widens. `bgra` and `rgba` are offered
together, as one layout in two byte orders: `bgra` is CoreVideo 32BGRA, and
`rgba` the memory order R, G, B, A of Android's AB24 (DRM ABGR8888) surfaces.

CoreVideo has no 32RGBA pixel format: a buffer cannot be created in it, from a
pool or around an IOSurface, so VideoToolbox cannot take it natively. An `rgba`
frame is written into the BGRA pool and its red and blue bytes are exchanged in
place when it is submitted (`vImagePermuteChannels`, untiled, on the submitting
thread). The exchange is memory-bound and costs well under a millisecond per
frame up to 4K, against tens of milliseconds to encode one; a caller that holds
RGBA surfaces saves the guest CPU the same conversion, which on an interpreted
guest is far dearer. A stream from `rgba` matches one from `bgra` of the same
pictures in size and quality.

VideoToolbox converts RGB to video-range YCbCr only, whatever range the stream
asks for, and then states video range. A stream that asks for full range from
`bgra` or `rgba` is converted here instead (vImage's ARGB8888 to 4:2:0 YCbCr
over 0..255 with the stream's matrix, BT.709 from 720 lines up and BT.601 below
when none is stated; the permutation of `rgba` is part of the call). The encoder
then takes full-range NV12 and states full range. The conversion runs on the
submitting thread and costs about 0.8 ms at 720p, 1.2 ms at 1080p and 3.3 ms at
4K per frame, against 7, 14 and 27 ms to encode one.

The colour description (`VideoEncodeColor`: primaries, transfer function,
matrix and range) is attached to the pixel buffers and the session, and shows in
the stream: the SPS VUI of H.264 and HEVC states the ITU-T H.273 code points
(BT.2020 PQ is 9/16/9, BT.2020 HLG 9/18/9, with the range flag). Each aspect is
independent: a stream that asks for a transfer function or full range alone
states it, with 2 (unspecified) for the others, and one that asks for nothing
states no description. The tests check every combination of range, standard
and transfer for NV12, I420, BGRA and RGBA in both codecs. HDR10 static
metadata (`VideoHdrStaticMetadata`) belongs to HEVC Main 10 and is written as
described under Output. HLG is a transfer function the platform encodes and
tests, and V4L2 has no name for it (its transfer functions end at SMPTE 2084),
so a guest through the Virtio device cannot ask for it; it stays unexposed
there.

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
B-frames, layers, parameter sets, HDR, colour (every combination of aspects in
both codecs, and HEVC Main 10 from P010 with PQ and HLG, read back from the
VUI), RGBA channel order in both ranges, levels, sizes, flow and limits, and
decodes the streams with the platform decoder to compare pictures.
`platform-video-encoder-recovery` links a variant of the backend that fails
sessions on demand; `platform-video-encoder-cache` runs child processes against
valid, missing and unusable cache files. `Kernarium/KernariumTests/HostVideoEncoderTests.mm`
records an iPad's capabilities as a test attachment and checks its streams.
