#!/usr/bin/env bash
# Hardening test suite for tsf.h / tml.h. Needs a C compiler with AddressSanitizer + UBSan.
# The compiler defaults to gcc; set CC=clang to run it under clang.
# Set STBVORBIS=/path/to/stb_vorbis.c to also compile and test the SF3 / Ogg Vorbis path.
set -e
cd "$(dirname "$0")"
SF2=../examples/florestan-subset.sf2
MID=../examples/venture.mid
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
export ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1
CC=${CC:-gcc}
SF3FLAGS=""
if [ -n "$STBVORBIS" ]; then
	# Including stb_vorbis.c defines STB_VORBIS_INCLUDE_STB_VORBIS_H, which makes tsf.h
	# compile its SF3 / Ogg Vorbis path instead of only the raw PCM path.
	SF3FLAGS="-include $STBVORBIS"
	echo "SF3/Ogg Vorbis path enabled ($STBVORBIS)"
fi

echo "[1/6] regression: audio output of a valid SoundFont (identical to upstream v0.9 when the hash is d77c557477edc394)"
$CC -O2 $SF3FLAGS -I.. -o /tmp/tsf_regress regress.c -lm && /tmp/tsf_regress $SF2
HASH=$(/tmp/tsf_regress $SF2 | sed -n 's/.*hash: \([0-9a-f]*\).*/\1/p')
EXPECT=d77c557477edc394
if [ "$CC" = "gcc" ]; then
	[ "$HASH" = "$EXPECT" ] || { echo "audio hash $HASH differs from the upstream v0.9 reference $EXPECT"; exit 1; }
	echo "audio hash matches the upstream v0.9 reference ($HASH)"
else
	echo "audio hash $HASH (reference for gcc: $EXPECT)"
fi

echo "[2/6] fuzz: 4000 corrupted SoundFonts under ASan+UBSan"
$CC $SAN $SF3FLAGS -I.. -o /tmp/tsf_fuzz fuzz.c -lm && ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 /tmp/tsf_fuzz $SF2 4000

echo "[3/6] allocation-failure injection (SoundFont loader) with leak checking"
$CC $SAN $SF3FLAGS -I.. -o /tmp/tsf_oom oom.c -lm && /tmp/tsf_oom $SF2

echo "[4/6] voice limit: a new note must still get a voice"
$CC -O1 $SF3FLAGS -I.. -o /tmp/tsf_steal steal.c -lm && /tmp/tsf_steal $SF2

echo "[5/6] fuzz: 4000 corrupted MIDI files under ASan+UBSan (valid-file hash identical to upstream: f5b1513f9f7e8e13)"
$CC $SAN $SF3FLAGS -I.. -o /tmp/tml_fuzz tmlfuzz.c -lm && ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 /tmp/tml_fuzz $MID 4000

echo "[6/6] allocation-failure injection (MIDI loader) with leak checking"
$CC $SAN $SF3FLAGS -I.. -o /tmp/tml_oom tmloom.c -lm && /tmp/tml_oom $MID
echo "ALL TESTS PASSED"
