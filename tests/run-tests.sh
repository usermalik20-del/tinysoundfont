#!/usr/bin/env bash
# Hardening test suite for tsf.h / tml.h. Needs gcc with AddressSanitizer + UBSan. Run from any directory.
set -e
cd "$(dirname "$0")"
SF2=../examples/florestan-subset.sf2
MID=../examples/venture.mid
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
export ASAN_OPTIONS=detect_leaks=1

echo "[1/6] regression: audio output of a valid SoundFont (identical to upstream v0.9 when the hash is d77c557477edc394)"
gcc -O2 -I.. -o /tmp/tsf_regress regress.c -lm && /tmp/tsf_regress $SF2

echo "[2/6] fuzz: 4000 corrupted SoundFonts under ASan+UBSan"
gcc $SAN -I.. -o /tmp/tsf_fuzz fuzz.c -lm && ASAN_OPTIONS=detect_leaks=0 /tmp/tsf_fuzz $SF2 4000

echo "[3/6] allocation-failure injection (SoundFont loader) with leak checking"
gcc $SAN -I.. -o /tmp/tsf_oom oom.c -lm && /tmp/tsf_oom $SF2

echo "[4/6] voice limit: a new note must still get a voice"
gcc -O1 -I.. -o /tmp/tsf_steal steal.c -lm && /tmp/tsf_steal $SF2

echo "[5/6] fuzz: 4000 corrupted MIDI files under ASan+UBSan (valid-file hash identical to upstream: f5b1513f9f7e8e13)"
gcc $SAN -I.. -o /tmp/tml_fuzz tmlfuzz.c -lm && ASAN_OPTIONS=detect_leaks=0 /tmp/tml_fuzz $MID 4000

echo "[6/6] allocation-failure injection (MIDI loader) with leak checking"
gcc $SAN -I.. -o /tmp/tml_oom tmloom.c -lm && /tmp/tml_oom $MID
echo "ALL TESTS PASSED"
