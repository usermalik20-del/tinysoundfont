#!/usr/bin/env python3
"""Build the committed SoundFont3 (SF3) test fixture.

Creates a minimal but valid SF3: one preset, one instrument, one sample, whose
sample data is a single Ogg Vorbis stream stored in the 'smpo' chunk. The
sample is a short decaying sine, so a loaded and rendered font is guaranteed to
produce non silent, finite audio.

Usage: python3 tools/make-sf3-fixture.py [output.sf3]

Requires ffmpeg with a Vorbis encoder (or `oggenc` from vorbis-tools).
"""
import os
import struct
import subprocess
import sys
import tempfile
import wave
import math

SAMPLE_RATE = 22050
SECONDS = 0.4
FREQ = 220.0


def make_wav(path):
    n = int(SAMPLE_RATE * SECONDS)
    frames = bytearray()
    for i in range(n):
        t = i / float(SAMPLE_RATE)
        env = math.exp(-3.5 * t / SECONDS)
        v = int(0.8 * env * 32767.0 * math.sin(2.0 * math.pi * FREQ * t))
        frames += struct.pack('<h', max(-32768, min(32767, v)))
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(frames))
    return n


def encode_ogg(wav_path, ogg_path):
    if subprocess.call(['which', 'oggenc'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
        subprocess.check_call(['oggenc', '-Q', '-q', '4', '-o', ogg_path, wav_path])
    else:
        subprocess.check_call(['ffmpeg', '-y', '-loglevel', 'error', '-i', wav_path,
                               '-c:a', 'vorbis', '-strict', '-2', '-q:a', '4', ogg_path])
    with open(ogg_path, 'rb') as f:
        return f.read()


def fourcc(s):
    return s.encode('ascii')


def name20(s):
    b = s.encode('ascii')[:19]
    return b + b'\x00' * (20 - len(b))


def chunk(cid, data):
    out = fourcc(cid) + struct.pack('<I', len(data)) + data
    if len(data) & 1:
        out += b'\x00'
    return out


def sub_list(list_type, data):
    return chunk('LIST', fourcc(list_type) + data)


def build_sf3(ogg, path):
    # pdta tables: one preset, one instrument, one sample, one zone each, and
    # the mandatory terminal records.
    phdr = name20('SF3Tone') + struct.pack('<HHHIII', 0, 0, 0, 0, 0, 0)
    phdr += name20('EOP') + struct.pack('<HHHIII', 0, 0, 1, 0, 0, 0)

    pbag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 1, 0)      # genNdx, modNdx
    pmod = struct.pack('<HHhHH', 0, 0, 0, 0, 0)                      # terminal record
    pgen = struct.pack('<HH', 41, 0) + struct.pack('<HH', 0, 0)      # instrument, terminal

    inst = name20('SF3Inst') + struct.pack('<H', 0)
    inst += name20('EOI') + struct.pack('<H', 1)

    ibag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 1, 0)       # instGenNdx, instModNdx
    imod = struct.pack('<HHhHH', 0, 0, 0, 0, 0)                      # terminal record
    # generator 53 = sampleID, 58 = overridingRootKey, 54 = sampleModes (0 = no loop)
    igen = struct.pack('<HH', 53, 0) + struct.pack('<HH', 58, 60) + struct.pack('<HH', 0, 0)

    # compressed sample: start/end are byte offsets into the smpo chunk, the
    # loop points are relative to the decoded sample start. sampleType bit 0x10
    # marks an Ogg Vorbis sample.
    shdr = name20('SF3Sine') + struct.pack('<IIIII', 0, len(ogg), 0, 0, SAMPLE_RATE)
    shdr += struct.pack('<BbHH', 60, 0, 0, 0x10)
    shdr += name20('EOS') + struct.pack('<IIIII', 0, 0, 0, 0, 0) + struct.pack('<BbHH', 0, 0, 0, 0)

    pdta = chunk('phdr', phdr) + chunk('pbag', pbag) + chunk('pmod', pmod) + \
        chunk('pgen', pgen) + chunk('inst', inst) + chunk('ibag', ibag) + \
        chunk('imod', imod) + chunk('igen', igen) + chunk('shdr', shdr)
    sdta = chunk('smpl', ogg)
    info = sub_list('INFO', chunk('ifil', struct.pack('<HH', 2, 4)) + chunk('isng', b'EMU8000\x00'))

    body = fourcc('sfbk') + info + sub_list('sdta', sdta) + sub_list('pdta', pdta)
    data = chunk('RIFF', body)
    with open(path, 'wb') as f:
        f.write(data)
    return len(data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'tests', 'data', 'tone.sf3')
    out = os.path.abspath(out)
    d = tempfile.mkdtemp()
    wav = os.path.join(d, 'tone.wav')
    ogg = os.path.join(d, 'tone.ogg')
    n = make_wav(wav)
    ogg_data = encode_ogg(wav, ogg)
    size = build_sf3(ogg_data, out)
    print('wave samples: %d, ogg bytes: %d, sf3 bytes: %d -> %s' % (n, len(ogg_data), size, out))


if __name__ == '__main__':
    main()
