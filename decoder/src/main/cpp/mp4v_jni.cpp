#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr char kTag[] = "RezkaMp4v";
constexpr size_t kMaxQueuedFrames = 48;
constexpr int64_t kMaxFrameLatenessUs = 100000;

struct Decoder {
  AVCodecContext* context = nullptr;
  SwsContext* scaler = nullptr;
  std::deque<AVFrame*> frames;
  int renderDiagnostics = 0;
  int surfaceDiagnostics = 0;
  bool surfaceConfigured = false;
  int surfaceWidth = 0;
  int surfaceHeight = 0;
  int64_t metricsStartUs = 0;
  uint64_t metricsRenderCalls = 0;
  uint64_t metricsDecodedFrames = 0;
  uint64_t metricsPresentedFrames = 0;
  uint64_t metricsLateDrops = 0;
  uint64_t metricsDecodeCalls = 0;
  int64_t metricsDecodeUs = 0;
  int64_t metricsWindowLockUs = 0;
  int64_t metricsConvertUs = 0;
  int64_t metricsPostUs = 0;
  int64_t lastPostUs = 0;
  uint64_t metricsPostGaps = 0;
  int64_t metricsPostGapUs = 0;
  int64_t metricsMaxPostGapUs = 0;
  double metricsPostGapSquaredUs = 0;
};

int64_t MonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

void MaybeLogMetrics(Decoder* decoder) {
  const int64_t now = MonotonicUs();
  if (decoder->metricsStartUs == 0) {
    decoder->metricsStartUs = now;
    return;
  }
  const int64_t elapsed = now - decoder->metricsStartUs;
  if (elapsed < 2000000) return;
  const double seconds = elapsed / 1000000.0;
  const double averageGapMs = decoder->metricsPostGaps == 0 ? 0.0
      : decoder->metricsPostGapUs / 1000.0 / decoder->metricsPostGaps;
  const double gapVarianceUs = decoder->metricsPostGaps == 0 ? 0.0
      : decoder->metricsPostGapSquaredUs / decoder->metricsPostGaps
          - std::pow(decoder->metricsPostGapUs / static_cast<double>(decoder->metricsPostGaps), 2);
  __android_log_print(ANDROID_LOG_INFO, kTag,
      "perf intervalMs=%lld renderCallsPerSec=%.1f decodedFps=%.1f presentedFps=%.1f "
      "lateDrops=%llu queue=%zu avgDecodeMs=%.2f avgLockMs=%.2f avgConvertMs=%.2f avgPostMs=%.2f "
      "avgFrameGapMs=%.2f maxFrameGapMs=%.2f frameGapStdMs=%.2f",
      static_cast<long long>(elapsed / 1000), decoder->metricsRenderCalls / seconds,
      decoder->metricsDecodedFrames / seconds, decoder->metricsPresentedFrames / seconds,
      static_cast<unsigned long long>(decoder->metricsLateDrops), decoder->frames.size(),
      decoder->metricsDecodeCalls == 0 ? 0.0
          : decoder->metricsDecodeUs / 1000.0 / decoder->metricsDecodeCalls,
      decoder->metricsPresentedFrames == 0 ? 0.0
          : decoder->metricsWindowLockUs / 1000.0 / decoder->metricsPresentedFrames,
      decoder->metricsPresentedFrames == 0 ? 0.0
          : decoder->metricsConvertUs / 1000.0 / decoder->metricsPresentedFrames,
      decoder->metricsPresentedFrames == 0 ? 0.0
          : decoder->metricsPostUs / 1000.0 / decoder->metricsPresentedFrames,
      averageGapMs, decoder->metricsMaxPostGapUs / 1000.0,
      std::sqrt(std::max(0.0, gapVarianceUs)) / 1000.0);
  decoder->metricsStartUs = now;
  decoder->metricsRenderCalls = 0;
  decoder->metricsDecodedFrames = 0;
  decoder->metricsPresentedFrames = 0;
  decoder->metricsLateDrops = 0;
  decoder->metricsDecodeCalls = 0;
  decoder->metricsDecodeUs = 0;
  decoder->metricsWindowLockUs = 0;
  decoder->metricsConvertUs = 0;
  decoder->metricsPostUs = 0;
  decoder->metricsPostGaps = 0;
  decoder->metricsPostGapUs = 0;
  decoder->metricsMaxPostGapUs = 0;
  decoder->metricsPostGapSquaredUs = 0;
}

void ClearFrames(Decoder* decoder) {
  while (!decoder->frames.empty()) {
    AVFrame* frame = decoder->frames.front();
    decoder->frames.pop_front();
    av_frame_free(&frame);
  }
}

int ReceiveFrames(Decoder* decoder) {
  for (;;) {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) return AVERROR(ENOMEM);
    const int result = avcodec_receive_frame(decoder->context, frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
      av_frame_free(&frame);
      return 0;
    }
    if (result < 0) {
      av_frame_free(&frame);
      return result;
    }
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) frame->pts = frame->best_effort_timestamp;
    decoder->metricsDecodedFrames++;
    if (decoder->frames.size() >= kMaxQueuedFrames) {
      av_frame_free(&decoder->frames.front());
      decoder->frames.pop_front();
    }
    decoder->frames.push_back(frame);
  }
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeCreate(
    JNIEnv* env, jclass, jbyteArray extra_data, jint threads) {
  const AVCodec* codec = avcodec_find_decoder_by_name("mpeg4");
  if (codec == nullptr) return 0;
  Decoder* decoder = new Decoder();
  decoder->context = avcodec_alloc_context3(codec);
  if (decoder->context == nullptr) { delete decoder; return 0; }
  decoder->context->thread_count = std::max(1, std::min(threads, 4));
  decoder->context->pkt_timebase = AVRational{1, 1000000};
  if (extra_data != nullptr) {
    const jsize size = env->GetArrayLength(extra_data);
    decoder->context->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (decoder->context->extradata == nullptr) {
      avcodec_free_context(&decoder->context); delete decoder; return 0;
    }
    env->GetByteArrayRegion(extra_data, 0, size,
        reinterpret_cast<jbyte*>(decoder->context->extradata));
    decoder->context->extradata_size = size;
  }
  if (avcodec_open2(decoder->context, codec, nullptr) < 0) {
    avcodec_free_context(&decoder->context); delete decoder; return 0;
  }
  return reinterpret_cast<jlong>(decoder);
}

extern "C" JNIEXPORT jint JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeDecode(
    JNIEnv* env, jclass, jlong handle, jobject input, jint offset, jint size, jlong pts_us) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  auto* bytes = static_cast<uint8_t*>(env->GetDirectBufferAddress(input));
  const jlong capacity = input == nullptr ? -1 : env->GetDirectBufferCapacity(input);
  if (decoder == nullptr || bytes == nullptr || capacity < 0 || offset < 0 || size <= 0 ||
      offset > capacity || size > capacity - offset) {
    return AVERROR(EINVAL);
  }
  const int64_t decodeStartUs = MonotonicUs();
  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) {
    decoder->metricsDecodeCalls++;
    decoder->metricsDecodeUs += MonotonicUs() - decodeStartUs;
    return AVERROR(ENOMEM);
  }
  int result = av_new_packet(packet, size);
  if (result >= 0) {
    memcpy(packet->data, bytes + offset, size);
    packet->pts = pts_us;
    packet->dts = AV_NOPTS_VALUE;
    result = avcodec_send_packet(decoder->context, packet);
  }
  av_packet_free(&packet);
  if (result < 0) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "send packet failed: %d", result);
    decoder->metricsDecodeCalls++;
    decoder->metricsDecodeUs += MonotonicUs() - decodeStartUs;
    return result;
  }
  result = ReceiveFrames(decoder);
  decoder->metricsDecodeCalls++;
  decoder->metricsDecodeUs += MonotonicUs() - decodeStartUs;
  return result;
}

extern "C" JNIEXPORT void JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeSignalEndOfStream(
    JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder != nullptr && avcodec_send_packet(decoder->context, nullptr) >= 0) ReceiveFrames(decoder);
}

extern "C" JNIEXPORT void JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeFlush(JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder != nullptr) {
    ClearFrames(decoder);
    avcodec_flush_buffers(decoder->context);
    decoder->lastPostUs = 0;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeHasFrames(JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  return decoder != nullptr && !decoder->frames.empty();
}

extern "C" JNIEXPORT jint JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeGetQueuedFrames(
    JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  return decoder == nullptr ? 0 : static_cast<jint>(decoder->frames.size());
}

extern "C" JNIEXPORT void JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeResetRenderDiagnostics(
    JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder != nullptr) {
    decoder->renderDiagnostics = 0;
    decoder->surfaceDiagnostics = 0;
    decoder->surfaceConfigured = false;
    decoder->lastPostUs = 0;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeRenderFrame(
    JNIEnv* env, jclass, jlong handle, jobject surface, jlong position_us, jlong late_us) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder == nullptr) return JNI_FALSE;
  decoder->metricsRenderCalls++;
  MaybeLogMetrics(decoder);
  if (surface == nullptr || decoder->frames.empty()) return JNI_FALSE;
  AVFrame* selected = nullptr;
  const int64_t due = position_us + late_us;
  if (!decoder->frames.empty() && decoder->renderDiagnostics++ < 8) {
    const AVFrame* front = decoder->frames.front();
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "render clock positionUs=%lld dueUs=%lld frontPtsUs=%lld queued=%zu",
        static_cast<long long>(position_us), static_cast<long long>(due),
        static_cast<long long>(front->pts), decoder->frames.size());
  }
  while (!decoder->frames.empty()) {
    AVFrame* frame = decoder->frames.front();
    if (frame->pts != AV_NOPTS_VALUE && frame->pts < position_us - kMaxFrameLatenessUs) {
      decoder->frames.pop_front();
      av_frame_free(&frame);
      decoder->metricsLateDrops++;
      continue;
    }
    if (frame->pts != AV_NOPTS_VALUE && frame->pts > due) break;
    decoder->frames.pop_front();
    selected = frame;
    break;
  }
  if (selected == nullptr) return JNI_FALSE;

  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "ANativeWindow_fromSurface failed");
    av_frame_free(&selected);
    return JNI_FALSE;
  }
  if (!decoder->surfaceConfigured || decoder->surfaceWidth != selected->width
      || decoder->surfaceHeight != selected->height) {
    const int geometry_result = ANativeWindow_setBuffersGeometry(
        window, selected->width, selected->height, WINDOW_FORMAT_RGB_565);
    if (geometry_result != 0) {
      __android_log_print(ANDROID_LOG_ERROR, kTag,
          "setBuffersGeometry(%dx%d) failed: %d",
          selected->width, selected->height, geometry_result);
      ANativeWindow_release(window);
      av_frame_free(&selected);
      return JNI_FALSE;
    }
    decoder->surfaceWidth = selected->width;
    decoder->surfaceHeight = selected->height;
    decoder->surfaceConfigured = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "configured surface buffer to decoded size %dx%d",
        selected->width, selected->height);
  }
  ANativeWindow_Buffer buffer{};
  const int64_t lockStartUs = MonotonicUs();
  int result = ANativeWindow_lock(window, &buffer, nullptr);
  decoder->metricsWindowLockUs += MonotonicUs() - lockStartUs;
  int converted_rows = 0;
  if (result == 0) {
    AVPixelFormat output_format;
    int bytes_per_pixel;
    switch (buffer.format) {
      case WINDOW_FORMAT_RGBA_8888:
        output_format = AV_PIX_FMT_RGBA;
        bytes_per_pixel = 4;
        break;
      case WINDOW_FORMAT_RGBX_8888:
        output_format = AV_PIX_FMT_RGB0;
        bytes_per_pixel = 4;
        break;
      case WINDOW_FORMAT_RGB_565:
        output_format = AV_PIX_FMT_RGB565LE;
        bytes_per_pixel = 2;
        break;
      default:
        __android_log_print(ANDROID_LOG_ERROR, kTag,
            "unsupported ANativeWindow buffer format=%d", buffer.format);
        ANativeWindow_unlockAndPost(window);
        result = AVERROR(EINVAL);
        output_format = AV_PIX_FMT_NONE;
        bytes_per_pixel = 0;
        break;
    }
    if (result == 0) {
      if (decoder->surfaceDiagnostics++ < 3) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "locked surface buffer=%dx%d stride=%d format=%d",
            buffer.width, buffer.height, buffer.stride, buffer.format);
      }
      decoder->scaler = sws_getCachedContext(decoder->scaler,
          selected->width, selected->height, static_cast<AVPixelFormat>(selected->format),
          buffer.width, buffer.height, output_format, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (decoder->scaler != nullptr) {
        uint8_t* dst[4] = {static_cast<uint8_t*>(buffer.bits), nullptr, nullptr, nullptr};
        int stride[4] = {buffer.stride * bytes_per_pixel, 0, 0, 0};
      const int64_t convertStartUs = MonotonicUs();
        converted_rows = sws_scale(
            decoder->scaler, selected->data, selected->linesize, 0, selected->height, dst, stride);
      decoder->metricsConvertUs += MonotonicUs() - convertStartUs;
      } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "sws_getCachedContext failed");
      }
      const int64_t postStartUs = MonotonicUs();
      const int post_result = ANativeWindow_unlockAndPost(window);
      decoder->metricsPostUs += MonotonicUs() - postStartUs;
      if (post_result != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
            "ANativeWindow_unlockAndPost failed: %d", post_result);
      }
      result = post_result;
    }
  } else {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "ANativeWindow_lock failed: %d", result);
  }
  ANativeWindow_release(window);
  if (result == 0 && converted_rows > 0) {
    const int64_t postedAtUs = MonotonicUs();
    if (decoder->lastPostUs != 0) {
      const int64_t gapUs = postedAtUs - decoder->lastPostUs;
      if (gapUs > 0 && gapUs < 1000000) {
        decoder->metricsPostGaps++;
        decoder->metricsPostGapUs += gapUs;
        decoder->metricsMaxPostGapUs = std::max(decoder->metricsMaxPostGapUs, gapUs);
        decoder->metricsPostGapSquaredUs += static_cast<double>(gapUs) * gapUs;
      }
    }
    decoder->lastPostUs = postedAtUs;
  }
  av_frame_free(&selected);
  if (result == 0 && converted_rows > 0) decoder->metricsPresentedFrames++;
  return result == 0 && converted_rows > 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeRelease(JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder == nullptr) return;
  ClearFrames(decoder);
  sws_freeContext(decoder->scaler);
  avcodec_free_context(&decoder->context);
  delete decoder;
}
