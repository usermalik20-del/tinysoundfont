# SF3 test fixture

`tone.sf3` is a minimal, real SoundFont3 whose only sample is a 0.4 s decaying
220 Hz sine stored as an Ogg Vorbis stream (the `smpl` chunk holds the Ogg
bytes, the sample header carries the compression flag `0x10`, and `start`/`end`
are byte offsets into that chunk — exactly what `tsf_decode_sf3_samples` reads).

| property | value |
| --- | --- |
| generator | `python3 tools/make-sf3-fixture.py tests/data/tone.sf3` |
| bytes | 4592 |
| decoded samples | 8821 |
| sha256 | `853397a2193dee1c71103241a9df74ae764d38d3694c81bb979fd189a89ef1ed` |

Regenerating needs `oggenc` (Alpine: `apk add vorbis-tools`) or an `ffmpeg`
build with a Vorbis encoder. The sample source (rate, length, frequency) is at
the top of the generator script.

`tests/sf3fix.c` loads this file, plays a note, renders 30 blocks and asserts
that the output is finite and non silent. It only runs when the suite is built
with stb_vorbis (`STBVORBIS=/path/to/stb_vorbis.c bash tests/run-tests.sh`);
without it the SF3 code path does not exist and the test reports that it
skipped.

The odd chunk size (4167 bytes of Ogg data plus a RIFF pad byte) is deliberate:
it is what exposed a missing pad byte skip in the SF3 loader (fixed in this
fork — see `HARDENING.md`).
