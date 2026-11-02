#!/usr/bin/env python3
"""Build the drop-in vendor package that the KANTA Android app consumes.

The app vendors this library at apps/android/app/src/main/cpp/vendor/ as three
files: tsf.h, LICENSE and PROVENANCE.json (see apps/android/app/src/main/cpp/vendor/).
This script produces exactly that layout, plus loose copies of the library files and a
SHA256SUMS file, so a release of this fork can be pulled straight into the app.

Usage:
    tools/package-vendor.py --revision <commit-sha> [--out dist]
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPSTREAM_REPO = "https://github.com/schellingb/TinySoundFont"
UPSTREAM_REVISION = "853a0a171759f1ddba0de1442133a75912bbeffa"
FORK_REPO = "https://github.com/usermalik20-del/tinysoundfont"

VENDOR_FILES = ["tsf.h", "LICENSE"]          # what the app vendors
LOOSE_FILES = ["tsf.h", "tml.h", "LICENSE", "HARDENING.md"]  # full library drop-in


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def write_zip(zip_path, members, base):
    """Deterministic zip: fixed timestamps, no extra metadata."""
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in members:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(base, name), "rb") as f:
                z.writestr(info, f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--revision", default=os.environ.get("GITHUB_SHA", "unknown"))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--repository", default=FORK_REPO)
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    vendor = os.path.join(out, "kanta-vendor")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(vendor)

    # 1. the exact drop-in layout the app expects
    for name in VENDOR_FILES:
        shutil.copyfile(os.path.join(ROOT, name), os.path.join(vendor, name))

    provenance = {
        "repository": args.repository,
        "revision": args.revision,
        "license": "MIT",
        "files": {name: sha256(os.path.join(vendor, name)) for name in VENDOR_FILES},
        "upstreamRepository": UPSTREAM_REPO,
        "upstreamRevision": UPSTREAM_REVISION,
        "description": ("TinySoundFont v0.9 with the hardening patch; drop-in replacement for "
                        "apps/android/app/src/main/cpp/vendor/. See HARDENING.md in the same release."),
    }
    with open(os.path.join(vendor, "PROVENANCE.json"), "w") as f:
        json.dump(provenance, f, indent=2)
        f.write("\n")

    short = args.revision[:8] if args.revision != "unknown" else "unknown"
    zip_name = "tinysoundfont-kanta-vendor-%s.zip" % short
    write_zip(os.path.join(out, zip_name), VENDOR_FILES + ["PROVENANCE.json"], vendor)

    # 2. loose library files for other consumers
    for name in LOOSE_FILES:
        shutil.copyfile(os.path.join(ROOT, name), os.path.join(out, name))

    # 3. checksums over everything published
    names = sorted(n for n in os.listdir(out) if os.path.isfile(os.path.join(out, n)))
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        for name in names:
            f.write("%s  %s\n" % (sha256(os.path.join(out, name)), name))

    print("vendor package for revision %s:" % args.revision)
    for name in VENDOR_FILES + ["PROVENANCE.json"]:
        print("  kanta-vendor/%-16s %s" % (name, provenance["files"].get(name, "")))
    print("  %s" % zip_name)
    for name in LOOSE_FILES:
        print("  %s" % name)
    print("  SHA256SUMS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
