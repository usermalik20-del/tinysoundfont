# Audio hash baselines

`tests/run-tests.sh` compares the FNV-1a hash of a fixed 30 block render of
`examples/florestan-subset.sf2` against a file named

```
<uname -m>-<compiler>-<optimization>.txt
```

so a regression on one architecture does not have to fail another. The hash is a
strict equality check once a file exists; without a file the suite prints
`RECORD: <file> does not exist yet - audio hash is <hash>` and passes, so a new
platform can be recorded from a CI log in one run.

The level of optimization is part of the file name because it changes the
floating point codegen: `aarch64-gcc-O0` (the only level the local ARM64
emulator can execute) and `aarch64-gcc-O2` (CI) legitimately differ.

| file | recorded by |
| --- | --- |
| `x86_64-gcc-O2.txt` | CI, "ASan + UBSan test suite (gcc)" |
| `x86_64-clang-O2.txt` | CI, clang lane |
| `x86_64-gcc-O2-m32.txt` | CI, 32-bit lane (`TSF_BASELINE_SUFFIX=-m32`) |
| `x86_64-gcc-O2-stbvorbis.txt` | CI, SF3 lane (`TSF_BASELINE_SUFFIX=-stbvorbis`); identical to the plain gcc hash, recorded separately so the lane can pin it |
| `aarch64-gcc-O2.txt` | CI, native ARM64 runner |
| `armv7-gcc-O2.txt` | CI, ARMv7 cross build run under qemu |
| `aarch64-gcc-O0.txt` | locally on the iSH ARM64 emulator |
