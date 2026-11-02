#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-virtio-sound-suspend: playback across a system suspend.

Usage: virtio_sound_suspend_probe.py [CASE...]   (default: every case, in order)

The guest suspends itself the way a host's quiesce does: an RTC alarm two
seconds ahead, then "mem" into /sys/power/state. The ALSA core restarts a
stream that was suspended at offset 0, so the driver must start it afresh. A
stream's driver state may not carry a position, a queued period or a pending
message across a suspend. Each case prints one "VSND <case> ok" or
"VSND <case> FAIL: <reason>" line, and the exit status is the number of
failed cases.

position      Play one period and suspend, with nothing queued. Recover, play
              one more period, and the stream position must be exactly that
              period, not one plus the position the stream had when it was
              suspended. One message is in flight, so completions cannot batch
              and hide a stale position.
fill          Write a period and a half, drop, prepare, write sixteen whole
              periods and drain. The half period the drop left behind must
              not keep its length, which would overfill it and end in -EIO
              (no suspend: it is the fill level a prepare resets).
paused        Play, pause, suspend. snd_pcm_recover must return 0 and the
              writes after it must succeed. A paused stream has no trigger at
              the suspend, but its device still loses the periods it held.
pausedreopen  Play, pause, suspend, close, open the device again and play.
              The messages the paused stream counted must not make the next
              open refuse. It runs last, because a driver that fails it fails
              every later open too.
"""

import ctypes
import re
import sys
import time

RATE = 48000
CHANNELS = 2
FRAME = 4
FORMAT_S16_LE = 2
ACCESS_RW_INTERLEAVED = 3
LATENCY_US = 300000
STATUS = "/proc/asound/card0/pcm0p/sub0/status"
HW_PARAMS = "/proc/asound/card0/pcm0p/sub0/hw_params"
WAKE_ALARM = "/sys/class/rtc/rtc0/wakealarm"
SUSPEND_STATS = "/sys/power/suspend_stats/success"
ALARM_SECONDS = 2
# A stream plays at the null sink's pace, a period in some forty
# milliseconds; every wait below ends at a deadline.
POLL_SECONDS = 0.005
SETTLE_SECONDS = 0.2
PLAY_SECONDS = 1.0
START_SECONDS = 5.0
CHUNK = 1024
ESTRPIPE = 86

alsa = ctypes.CDLL("libasound.so.2")
alsa.snd_pcm_writei.restype = ctypes.c_long
alsa.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
alsa.snd_pcm_recover.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
alsa.snd_strerror.restype = ctypes.c_char_p
alsa.snd_strerror.argtypes = [ctypes.c_int]
alsa.snd_pcm_sw_params_sizeof.restype = ctypes.c_size_t
for name in ("snd_pcm_sw_params_current", "snd_pcm_sw_params"):
    getattr(alsa, name).argtypes = [ctypes.c_void_p, ctypes.c_void_p]
for name in (
    "snd_pcm_sw_params_set_start_threshold",
    "snd_pcm_sw_params_set_stop_threshold",
):
    getattr(alsa, name).argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
    ]
alsa.snd_pcm_sw_params_get_boundary.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)
]
alsa.snd_pcm_pause.argtypes = [ctypes.c_void_p, ctypes.c_int]
for name in ("snd_pcm_drop", "snd_pcm_prepare", "snd_pcm_drain", "snd_pcm_close"):
    getattr(alsa, name).argtypes = [ctypes.c_void_p]

POINTERS = re.compile(rb"hw_ptr\s*:\s*(\d+)\s*\nappl_ptr\s*:\s*(\d+)")
PARAMETER = re.compile(r"(period_size|buffer_size): (\d+)")


class Failure(Exception):
    """A case's assertion failed; the message says which defect it is."""


def say(text):
    print(text, flush=True)


def describe(code):
    return f"{code} ({alsa.snd_strerror(int(code)).decode()})"


def pointers():
    """The core's (hw_ptr, appl_ptr) in frames; reading the status updates
    the hardware pointer through the driver."""
    try:
        with open(STATUS, "rb") as status:
            match = POINTERS.search(status.read())
    except OSError:
        return (-1, -1)
    return (int(match.group(1)), int(match.group(2))) if match else (-1, -1)


def open_stream(what="the open"):
    pcm = ctypes.c_void_p()
    code = alsa.snd_pcm_open(ctypes.byref(pcm), b"hw:0,0", 0, 0)
    if code < 0:
        raise Failure(f"{what} returned {describe(code)}")
    code = alsa.snd_pcm_set_params(
        pcm, FORMAT_S16_LE, ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 0, LATENCY_US
    )
    if code < 0:
        alsa.snd_pcm_close(pcm)
        raise Failure(f"snd_pcm_set_params returned {describe(code)}")
    with open(HW_PARAMS) as parameters:
        sizes = dict(
            (key, int(value)) for key, value in PARAMETER.findall(parameters.read())
        )
    return pcm, sizes["period_size"], sizes["buffer_size"]


def set_thresholds(pcm, start, stop):
    """Start threshold `start` frames; `stop` frames, or None for the
    boundary, which never ends a stream by underrun."""
    sw = ctypes.create_string_buffer(alsa.snd_pcm_sw_params_sizeof())
    alsa.snd_pcm_sw_params_current(pcm, sw)
    if stop is None:
        boundary = ctypes.c_ulong()
        alsa.snd_pcm_sw_params_get_boundary(sw, ctypes.byref(boundary))
        stop = boundary.value
    alsa.snd_pcm_sw_params_set_start_threshold(pcm, sw, start)
    alsa.snd_pcm_sw_params_set_stop_threshold(pcm, sw, stop)
    code = alsa.snd_pcm_sw_params(pcm, sw)
    if code < 0:
        raise Failure(f"snd_pcm_sw_params returned {describe(code)}")


def suspends():
    try:
        with open(SUSPEND_STATS) as stats:
            return int(stats.read())
    except (OSError, ValueError):
        return None


def suspend():
    """Suspend the guest until its RTC alarm; fail unless it did suspend."""
    with open(WAKE_ALARM, "w") as alarm:
        alarm.write("0")
    with open(WAKE_ALARM, "w") as alarm:
        alarm.write(f"+{ALARM_SECONDS}")
    before = suspends()
    try:
        with open("/sys/power/state", "w") as power:
            power.write("mem")
    except OSError as error:
        raise Failure(f"the guest's suspend failed: {error}") from error
    after = suspends()
    if before is not None and after != before + 1:
        raise Failure(f"the guest did not suspend (suspend_stats {before} -> {after})")


def silence(frames):
    return ctypes.create_string_buffer(frames * FRAME)


def write(pcm, data, frames):
    """One writei; on an error, recover once. Returns (frames written or the
    error, None or the recovery's result)."""
    written = alsa.snd_pcm_writei(pcm, data, frames)
    if written >= 0:
        return written, None
    return written, alsa.snd_pcm_recover(pcm, int(written), 1)


def play(pcm, label, seconds=PLAY_SECONDS):
    """Write half periods for `seconds`; every write must succeed."""
    data = silence(CHUNK)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        written, recovered = write(pcm, data, CHUNK)
        if recovered is not None:
            raise Failure(
                f"{label}: a write returned {describe(written)} and the "
                f"recovery returned {describe(recovered)}"
            )


def recover_from_suspend(pcm, frames):
    """The first write after the suspend must find the stream suspended, and
    snd_pcm_recover must prepare it again."""
    written, recovered = write(pcm, silence(frames), frames)
    if written != -ESTRPIPE:
        raise Failure(
            "the first write after the suspend returned "
            f"{describe(written)}, expected -ESTRPIPE: the stream was not suspended"
        )
    if recovered != 0:
        raise Failure(
            f"snd_pcm_recover returned {describe(recovered)}, expected 0: "
            "the driver refused to prepare the suspended stream"
        )


def wait_for_position(minimum):
    end = time.monotonic() + START_SECONDS
    while pointers()[0] < minimum and time.monotonic() < end:
        time.sleep(POLL_SECONDS)


def case_position():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND position period={period} buffer={buffer}")
        set_thresholds(pcm, period, None)
        written, recovered = write(pcm, silence(period), period)
        if written != period or recovered is not None:
            raise Failure(f"the first period was not written: {describe(written)}")
        wait_for_position(period)
        time.sleep(SETTLE_SECONDS)
        before = pointers()
        if before[0] != period:
            raise Failure(f"the first period never played (hw_ptr {before[0]})")
        suspend()
        recover_from_suspend(pcm, period)
        written, recovered = write(pcm, silence(period), period)
        if written != period or recovered is not None:
            raise Failure(
                f"the period after the recovery was not written: {describe(written)}"
            )
        # The first completion moves the position; one more is never queued.
        wait_for_position(1)
        time.sleep(SETTLE_SECONDS)
        position = pointers()[0]
        say(f"VSND position hw_ptr {before[0]} before the suspend, {position} "
            f"after one period played since the recovery")
        if position != period:
            raise Failure(
                f"hw_ptr is {position} after one period, expected {period}: "
                "the position the stream had when it was suspended was counted "
                "as played"
            )
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_fill():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND fill period={period} buffer={buffer}")
        # Started from the first period, so the drop stops a running stream
        # and releases its queued period.
        set_thresholds(pcm, period, None)
        half = period + period // 2
        written, recovered = write(pcm, silence(half), half)
        if written != half or recovered is not None:
            raise Failure(f"the first writes failed: {describe(written)}")
        alsa.snd_pcm_drop(pcm)
        code = alsa.snd_pcm_prepare(pcm)
        if code != 0:
            raise Failure(f"prepare after the drop returned {describe(code)}")
        data = silence(period)
        for index in range(16):
            written = alsa.snd_pcm_writei(pcm, data, period)
            if written < 0:
                raise Failure(
                    f"whole period {index} after the prepare returned "
                    f"{describe(written)}: the half period the drop left kept "
                    "its length"
                )
        code = alsa.snd_pcm_drain(pcm)
        if code != 0:
            raise Failure(f"drain returned {describe(code)}")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_paused():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND paused period={period} buffer={buffer}")
        play(pcm, "before the pause")
        code = alsa.snd_pcm_pause(pcm, 1)
        if code != 0:
            raise Failure(f"pause returned {describe(code)}")
        suspend()
        recover_from_suspend(pcm, CHUNK)
        play(pcm, "after the recovery")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_pausedreopen():
    pcm, period, buffer = open_stream("the first open")
    say(f"VSND pausedreopen period={period} buffer={buffer}")
    try:
        play(pcm, "before the pause")
        code = alsa.snd_pcm_pause(pcm, 1)
        if code != 0:
            raise Failure(f"pause returned {describe(code)}")
        suspend()
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)
    pcm, period, buffer = open_stream("the open after the suspend")
    try:
        play(pcm, "after the reopen")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


CASES = {
    "position": case_position,
    "fill": case_fill,
    "paused": case_paused,
    "pausedreopen": case_pausedreopen,
}


def main():
    names = sys.argv[1:] or list(CASES)
    unknown = [name for name in names if name not in CASES]
    if unknown:
        say(f"VSND unknown case {unknown[0]}; the cases are {' '.join(CASES)}")
        return 2
    failed = 0
    for name in names:
        try:
            CASES[name]()
        except Failure as failure:
            failed += 1
            say(f"VSND {name} FAIL: {failure}")
        except Exception as error:  # noqa: BLE001 - a case reports and goes on
            failed += 1
            say(f"VSND {name} FAIL: {error!r}")
        else:
            say(f"VSND {name} ok")
    say(f"VSND finished, {failed} failed")
    return failed


if __name__ == "__main__":
    sys.exit(main())
