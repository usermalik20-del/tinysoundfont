#!/usr/bin/env bash
# Hardening test suite for tsf.h / tml.h.
#
# The compiler defaults to gcc; set CC=clang for clang, or CC="gcc -m32" for a
# 32 bit build. Set STBVORBIS=/path/to/stb_vorbis.c to compile the SF3 / Ogg
# Vorbis path as well. Set TSF_SKIP_SAN=1 to skip the sanitizer lanes (used on
# hosts where libasan/libubsan cannot be linked). Set TSF_OPT to change the
# optimization level of the non-sanitizer lanes (default -O2).
set -e
cd "$(dirname "$0")"
SF2=../examples/florestan-subset.sf2
MID=../examples/venture.mid
CC=${CC:-gcc}
CCNAME=$(basename "${CC%% *}")
OPT=${TSF_OPT:--O2}
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
export ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1
SF3FLAGS=""
if [ -n "$STBVORBIS" ]; then
	# Including stb_vorbis.c defines STB_VORBIS_INCLUDE_STB_VORBIS_H, which makes tsf.h
	# compile its SF3 / Ogg Vorbis path instead of only the raw PCM path.
	SF3FLAGS="-include $STBVORBIS"
	echo "SF3/Ogg Vorbis path enabled ($STBVORBIS)"
fi
san_compile() { # san_compile <output> <source>
	[ -n "$TSF_SKIP_SAN" ] && { echo "skipped (TSF_SKIP_SAN=1): $2"; return 1; }
	$CC $SAN $SF3FLAGS -I.. -o "$1" "$2" -lm
}
ARCH=$(uname -m)
BASELINE="baselines/${ARCH}-${CCNAME}-${OPT#-}${TSF_BASELINE_SUFFIX}.txt"

echo "[ 1/10] regression: audio output of a valid SoundFont"
$CC $OPT $SF3FLAGS -I.. -o /tmp/tsf_regress regress.c -lm && /tmp/tsf_regress $SF2
HASH=$(/tmp/tsf_regress $SF2 | sed -n 's/.*hash: \([0-9a-f]*\).*/\1/p')
if [ -f "$BASELINE" ]; then
	EXPECT=$(cat "$BASELINE")
	[ "$HASH" = "$EXPECT" ] || { echo "audio hash $HASH differs from baseline $BASELINE ($EXPECT)"; exit 1; }
	echo "audio hash matches baseline $BASELINE ($HASH)"
else
	echo "RECORD: $BASELINE does not exist yet - audio hash is $HASH"
fi

echo "[ 2/10] fuzz: 4000 corrupted SoundFonts under ASan+UBSan"
if san_compile /tmp/tsf_fuzz fuzz.c; then ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 /tmp/tsf_fuzz $SF2 4000; fi

echo "[ 3/10] allocation-failure injection (SoundFont loader) with leak checking"
if san_compile /tmp/tsf_oom oom.c; then /tmp/tsf_oom $SF2; fi

echo "[ 4/10] voice limit: a new note must still get a voice"
$CC -O1 $SF3FLAGS -I.. -o /tmp/tsf_steal steal.c -lm && /tmp/tsf_steal $SF2

echo "[ 5/10] fuzz: 4000 corrupted MIDI files under ASan+UBSan (valid-file hash f5b1513f9f7e8e13)"
if san_compile /tmp/tml_fuzz tmlfuzz.c; then ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 /tmp/tml_fuzz $MID 4000; fi

echo "[ 6/10] allocation-failure injection (MIDI loader) with leak checking"
if san_compile /tmp/tml_oom tmloom.c; then /tmp/tml_oom $MID; fi

echo "[ 7/10] modulators: file records, SF2 defaults, mod wheel and channel pressure"
$CC $OPT $SF3FLAGS -I.. -o /tmp/tsf_mod mod.c -lm && /tmp/tsf_mod $SF2

echo "[ 8/10] effects: chorus / reverb stage, sends and CC91 / CC93"
$CC $OPT $SF3FLAGS -I.. -o /tmp/tsf_fx fx.c -lm && /tmp/tsf_fx $SF2

echo "[ 9/10] realtime: rendering performs no allocation after setup"
$CC $OPT $SF3FLAGS -I.. -o /tmp/tsf_realtime realtime.c -lm && /tmp/tsf_realtime $SF2

echo "[10/10] threads: tsf_playback serializes render and control threads"
$CC $OPT $SF3FLAGS -I.. -pthread -o /tmp/tsf_thread thread.c -lm && /tmp/tsf_thread $SF2

if [ -n "$STBVORBIS" ] && ls data/*.sf3 >/dev/null 2>&1; then
	FIXTURE=$(ls data/*.sf3 | head -1)
	echo "[extra] SF3 fixture: $FIXTURE"
	$CC $OPT $SF3FLAGS -I.. -o /tmp/tsf_sf3fix sf3fix.c -lm && /tmp/tsf_sf3fix "$FIXTURE"
fi
echo "ALL TESTS PASSED"
