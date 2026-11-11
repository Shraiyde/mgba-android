#!/usr/bin/env python3
"""Patch SDL 2.30 AAudio so a callback buffer is queued in full.

SDL's aaudio_PlayDevice writes with a 1 ms timeout (the comment says 8 ms).
AAudio then accepts only the frames that fit in that window and SDL discards
the rest of the mix buffer. Playback is mostly silence, and the audio thread
spins instead of waiting out the samples it was given.
"""
import pathlib
import sys

MARKER = "mgba-android: queue the entire AAudio callback"
OLD = """static void aaudio_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *private = this->hidden;
    aaudio_result_t res;
    int64_t timeoutNanoseconds = 1 * 1000 * 1000; /* 8 ms */
    res = ctx.AAudioStream_write(private->stream, private->mixbuf, private->mixlen / private->frame_size, timeoutNanoseconds);
    if (res < 0) {
        LOGI("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        if (RecoverAAudioDevice(this) < 0) {
            return;  /* oh well, we went down hard. */
        }
    } else {
        LOGI("SDL AAudio play: %d frames, wanted:%d frames", (int)res, private->mixlen / private->frame_size);
    }

#if 0
    /* Log under-run count */
    {
        static int prev = 0;
        int32_t cnt = ctx.AAudioStream_getXRunCount(private->stream);
        if (cnt != prev) {
            SDL_Log("AAudio underrun: %d - total: %d", cnt - prev, cnt);
            prev = cnt;
        }
    }
#endif
}
"""

NEW = """static void aaudio_PlayDevice(_THIS)
{
    /* mgba-android: queue the entire AAudio callback.
       The previous 1 ms timeout returned after a few dozen frames and the
       rest of this mix buffer was discarded. Wait long enough to hand the
       whole callback to the stream. */
    struct SDL_PrivateAudioData *private = this->hidden;
    const int frame_size = private->frame_size > 0 ? private->frame_size : 4;
    int frames_left = private->mixlen / frame_size;
    const Uint8 *buf = private->mixbuf;
    const int rate = this->spec.freq > 0 ? this->spec.freq : 48000;
    aaudio_result_t res;
    int64_t timeoutNanoseconds;

    while (frames_left > 0) {
        timeoutNanoseconds = (int64_t) frames_left * 1000000000LL / rate;
        if (timeoutNanoseconds < 8000000) {
            timeoutNanoseconds = 8000000;
        }
        res = ctx.AAudioStream_write(private->stream, buf, frames_left, timeoutNanoseconds);
        if (res < 0) {
            LOGI("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
            if (RecoverAAudioDevice(this) < 0) {
                return;  /* oh well, we went down hard. */
            }
            break;
        }
        if (res == 0) {
            LOGI("SDL AAudio play: wrote 0 of %d frames", frames_left);
            break;
        }
        buf += (int) res * frame_size;
        frames_left -= (int) res;
    }
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-sdl-aaudio.py SDL2_SRC", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1]) / "src" / "audio" / "aaudio" / "SDL_aaudio.c"
    if not path.is_file():
        print(f"SDL AAudio source not found at {path}", file=sys.stderr)
        return 1
    text = path.read_text()
    if MARKER in text:
        print(f"SDL AAudio already patched: {path}")
        return 0
    if OLD not in text:
        print(f"SDL AAudio PlayDevice did not match the expected SDL 2.30 source: {path}", file=sys.stderr)
        return 1
    path.write_text(text.replace(OLD, NEW, 1))
    print(f"Patched SDL AAudio playback: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
