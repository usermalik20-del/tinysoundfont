# Hardening changes (on top of TinySoundFont v0.9)

Only `tsf.h` and `tml.h` changed. The API is unchanged. For a valid SoundFont / MIDI file the output is
bit-identical to upstream (verified by a hash over all presets and the channel API).
Run `tests/run-tests.sh` to reproduce every check (needs gcc with ASan/UBSan).

## tsf.h
Loader (corrupt or hostile .sf2 files):
- A SoundFont with fewer than 2 preset headers made `presetNum` negative -> huge allocation / crash. Now rejected.
- Preset / instrument / bag / generator index chains are clamped to their tables and forced monotonic before use
  (previously heap buffer overflows, confirmed with AddressSanitizer on the original header). Valid fonts are untouched.
- The terminal instrument record can no longer be referenced as an instrument; sample IDs are bounds-checked.
- Regions whose sample start is not before their end are dropped; loop points are clamped to the sample data.
- Short reads of hydra tables / sample data are detected (previously uninitialized data was used silently).
- A repeated hydra chunk no longer leaks the first copy; allocation size math is overflow-checked.
- RIFF reader: 32-bit overflow in the size check fixed, LIST chunks smaller than 4 bytes rejected.
- Memory/stdio streams: skip() overflow fixed, negative buffer size rejected, huge fseek counts handled on 32-bit `long`.
- Sample buffer gets one zero padding sample (linear interpolation read one float past the end on the last sample).
- Presets with zero regions no longer fail the whole load on platforms where malloc(0) returns NULL.

Runtime:
- tsf_note_on: an out-of-memory during voice growth left `voiceNum` too large (later out-of-bounds access). Fixed.
- tsf_note_on: velocity is clamped/NaN-safe before the short conversion.
- With tsf_set_max_voices, a new note is no longer silently dropped when every voice is busy: the oldest voice is
  stolen. Define `TSF_NO_VOICE_STEALING` to get the old behaviour.
- tsf_copy: the copy no longer inherits `maxVoiceNum` without owning any voices (that silently muted every note).
- tsf_render_*: negative sample counts are ignored (previously a huge memset).
- Channels: negative / absurd channel numbers rejected, MIDI controller values clamped to 0..127, NULL-channels
  dereference in tsf_channel_note_off removed, signed-shift UB removed.

## tml.h
- Out-of-memory while growing the message array lost the array and crashed later (SEGV confirmed on the original). Fixed.
- Unchecked malloc of the track table / track buffer.
- Header check used `&&` where `||` was needed (a division of 0 produced an infinite tempo factor).
- A bad sysex/meta length could move the read pointer backwards or overflow the pointer; now bounds-checked.
- Millisecond conversion no longer overflows `int`; signed shift UB removed.

## Not changed / not verified
- The SF3 / Ogg Vorbis path (needs stb_vorbis) only received the padding-sample change; it is not covered by the tests.
- 32-bit builds could not be tested here (the overflow guards are written for them but unexercised).
- Thread safety is unchanged: note_on/note_off and render still need external locking (see the header notes).
- Sound quality items (interpolation, filter, modulators, chorus/reverb sends) are not part of this change.

## Follow-up changes in this fork (after the original hardening patch)

- The internal stream helpers were called through function pointers cast to a different type.
  Calling a function through an incompatible function-pointer type is undefined behaviour and
  clang's UBSan reports it on every load. The helpers now take `void* data` and are assigned
  without casts; `struct tsf_stream` / `struct tml_stream` and the public API are unchanged.
- The voice iterators computed `f->voices + f->voiceNum` while `f->voices` was still NULL.
  Adding zero to a null pointer is undefined behaviour, reported by UBSan whenever a font is
  rendered before the first note. The end pointer is now derived only when the voice array exists.
- `tests/run-tests.sh` honours `$CC`; CI runs the suite with gcc and with clang, both under
  AddressSanitizer + UndefinedBehaviorSanitizer with leak checking.
- The gcc CI job asserts the valid-font audio hash against the upstream v0.9 reference
  (`d77c557477edc394`) and the valid-MIDI message hash against `f5b1513f9f7e8e13`, so a silent
  change in audio output or MIDI parsing fails the build instead of only being printed.
- The padding sample that was added to the sample buffer is now only allocated after an
  overflow check that also holds on 64-bit builds: `resNum + 1` was evaluated in 32-bit
  arithmetic on the SF3 / Ogg Vorbis path and in the trim step of `tsf_decode_sf3_samples`,
  so a decoded sample count of `SIZE_MAX` samples would have wrapped to a zero-sized
  allocation followed by an out-of-bounds write. The same check replaced the 32-bit-only
  guard on the raw PCM path, and the sample buffer growth in `tsf_decode_sf3_samples` now
  multiplies in `size_t`.
- The message-array growth in `tml.h` is computed in `size_t` and checked against
  `INT_MAX` and `SIZE_MAX / sizeof(tml_message)`, so the counters cannot overflow.
- CI gained an SF3 / Ogg Vorbis lane that compiles the `STB_VORBIS_INCLUDE_STB_VORBIS_H`
  path (previously never compiled here) with a pinned stb_vorbis revision and runs the
  whole suite through it. This is what the earlier "SF3 path only got the padding-sample
  change and is untested" note referred to.
