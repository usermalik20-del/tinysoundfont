# Audio hash baselines

`tests/run-tests.sh` compares the FNV-1a hash of a fixed 30 block render of
`examples/florestan-subset.sf2` against a file named

```
<uname -m>-<compiler>-<optimization>.txt
```

so a regression on one architecture does not have to fail another. Record a new
baseline by running the suite once on the target and saving the printed hash as
`RECORD:` reports it — the hash is a strict equality check once the file exists.

Baselines are pinned for architectures that are actually executed in CI
(x86_64 gcc/clang, x86_64 32 bit) and for aarch64 built with `-O0`, which is the
only level the local ARM64 emulator can execute.
