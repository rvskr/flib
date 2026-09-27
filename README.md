# FLib: MPEG-4 Part 2 software decoder for Android

An Android Media3 renderer backed by a minimal FFmpeg build. It targets MPEG-4
Part 2 (`video/mp4v-es`) and builds shared FFmpeg libraries for `arm64-v8a`,
`armeabi-v7a`, `x86`, and `x86_64`.

## Build

Run the **Build MPEG-4 Part 2 decoder** workflow from the Actions tab, or build
locally with JDK 17+, Android SDK platform 36, NDK `27.2.12479018`, CMake 3.22.1,
`make`, `nasm`, and `patchelf`:

```bash
bash build_ffmpeg_android.sh
./gradlew :decoder:assembleRelease
```

The workflow publishes `decoder-release.aar`, the matching FFmpeg source archive,
and the build script as a downloadable Actions artifact. No AAR is committed to
this repository.

## Integration

Add the AAR as a dependency and create `Mpeg4RenderersFactory(context, force)`
for Media3's `ExoPlayer.Builder.setRenderersFactory(...)`. Set `force` to `true`
to prefer the software renderer for `video/mp4v-es`; otherwise it is available
after Media3's standard renderers.

## Licensing

FFmpeg is built with GPL and nonfree components disabled. Its source and build
configuration are included with each workflow artifact. Review the FFmpeg
license and redistribution requirements before shipping a product containing
the AAR. See [FFMPEG_BUILD_INFO.md](FFMPEG_BUILD_INFO.md).
