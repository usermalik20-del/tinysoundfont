#!/usr/bin/env bash
# Hardening test suite for tsf.h / tml.h. Needs a C compiler with AddressSanitizer + UBSan.
# The compiler defaults to gcc; set CC=clang to run it under clang. Run from any directory.
set -e
cd "$(dirname "$0")"
SF2=../examples/florestan-subset.sf2
MID=../examples/venture.mid
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
export ASAN_OPTIONS=detect_leaks=1

echo "[1/6] regression: audio output of a valid SoundFont (identical to upstream v0.9 when the hash is d77c557477edc394)"
"${CC:-gcc}" -O2 -I.. -o /tmp/tsf_regress regress.c -lm && /tmp/tsf_regress $SF2
HASH=$(/tmp/tsf_regress $SF2 | sed -n 's/.*hash: \([0-9a-f]*\).*/\1/p')
EXPECT=d77c557477edc394
if [ "${CC:-gcc}" = "gcc" ]; then
	[ "$HASH" = "$EXPECT" ] || { echo "audio hash $HASH differs from the upstream v0.9 reference $EXPECT"; exit 1; }
	echo "audio hash matches the upstream v0.9 reference ($HASH)"
else
	echo "audio hash $HASH (reference for gcc: $EXPECT)"
fi

echo "[2/6] fuzz: 4000 corrupted SoundFonts under ASan+UBSan"
"${CC:-gcc}" $SAN -I.. -o /tmp/tsf_fuzz fuzz.c -lm && ASAN_OPTIONS=detect_leaks=0 /tmp/tsf_fuzz $SF2 4000

echo "[3/6] allocation-failure injection (SoundFont loader) with leak checking"
"${CC:-gcc}" $SAN -I.. -o /tmp/tsf_oom oom.c -lm && /tmp/tsf_oom $SF2

echo "[4/6] voice limit: a new note must still get a voice"
"${CC:-gcc}" -O1 -I.. -o /tmp/tsf_steal steal.c -lm && /tmp/tsf_steal $SF2

echo "[5/6] fuzz: 4000 corrupted MIDI files under ASan+UBSan (valid-file hash identical to upstream: f5b1513f9f7e8e13)"
"${CC:-gcc}" $SAN -I.. -o /tmp/tml_fuzz tmlfuzz.c -lm && ASAN_OPTIONS=detect_leaks=0 /tmp/tml_fuzz $MID 4000

echo "[6/6] allocation-failure injection (MIDI loader) with leak checking"
"${CC:-gcc}" $SAN -I.. -o /tmp/tml_oom tmloom.c -lm && /tmp/tml_oom $MID
echo "ALL TESTS PASSED"
