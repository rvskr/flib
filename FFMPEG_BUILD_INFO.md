# FFmpeg MPEG-4 Part 2 build

This library uses FFmpeg n7.1.1, configured for Android API 24 and these ABIs:
`arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

The build enables only the `mpeg4` decoder, `mpeg4video` parser, and `swscale`.
It disables GPL, nonfree components, static FFmpeg libraries, network protocols,
programs, filters, and demuxers. Media3 performs container demuxing; the decoder
receives MPEG-4 Part 2 samples from Media3 and renders YUV frames to its Surface.

The GitHub Actions run publishes an artifact containing the AAR, the corresponding
FFmpeg source archive, this build description, and the build script. The source
archive is the exact FFmpeg tag used for the AAR. The AAR dynamically links the
FFmpeg shared libraries so they can be replaced/relinked separately.

FFmpeg is distributed under LGPL 2.1 or later for this configuration. Review
FFmpeg's license and distribution requirements before shipping the APK.
