#!/usr/bin/env python3
"""Create ABI-specific AARs without changing the universal decoder AAR."""

from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "decoder/build/outputs/aar/decoder-release.aar"
OUTPUT = ROOT / "dist"
ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")

OUTPUT.mkdir(exist_ok=True)
with ZipFile(SOURCE) as source:
    names = set(source.namelist())
    for abi in ABIS:
        if not any(name.startswith(f"jni/{abi}/") for name in names):
            raise RuntimeError(f"Missing native libraries for {abi}")
        target = OUTPUT / f"decoder-{abi}-release.aar"
        with ZipFile(target, "w") as output:
            for entry in source.infolist():
                if entry.filename.startswith("jni/") and not entry.filename.startswith(
                    f"jni/{abi}/"
                ):
                    continue
                output.writestr(
                    entry.filename,
                    source.read(entry.filename),
                    compress_type=ZIP_DEFLATED,
                    compresslevel=9,
                )
        print(f"{target.name}: {target.stat().st_size} bytes")
