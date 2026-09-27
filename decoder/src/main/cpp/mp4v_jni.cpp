#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr char kTag[] = "RezkaMp4v";
constexpr size_t kMaxQueuedFrames = 48;

struct Decoder {
  AVCodecContext* context = nullptr;
  SwsContext* scaler = nullptr;
  std::deque<AVFrame*> frames;
  int windowWidth = 0;
  int windowHeight = 0;
};

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
  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) return AVERROR(ENOMEM);
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
    return result;
  }
  return ReceiveFrames(decoder);
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
  if (decoder != nullptr) { ClearFrames(decoder); avcodec_flush_buffers(decoder->context); }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeHasFrames(JNIEnv*, jclass, jlong handle) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  return decoder != nullptr && !decoder->frames.empty();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_rezkatv_mpeg4_decoder_Mpeg4SoftwareVideoRenderer_nativeRenderFrame(
    JNIEnv* env, jclass, jlong handle, jobject surface, jlong position_us, jlong late_us) {
  auto* decoder = reinterpret_cast<Decoder*>(handle);
  if (decoder == nullptr || surface == nullptr || decoder->frames.empty()) return JNI_FALSE;
  AVFrame* selected = nullptr;
  const int64_t due = position_us + late_us;
  while (!decoder->frames.empty()) {
    AVFrame* frame = decoder->frames.front();
    if (frame->pts != AV_NOPTS_VALUE && frame->pts > due) break;
    decoder->frames.pop_front();
    if (selected != nullptr) av_frame_free(&selected);
    selected = frame;
  }
  if (selected == nullptr) return JNI_FALSE;

  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) { av_frame_free(&selected); return JNI_FALSE; }
  if (decoder->windowWidth != selected->width || decoder->windowHeight != selected->height) {
    ANativeWindow_setBuffersGeometry(window, selected->width, selected->height, WINDOW_FORMAT_RGBA_8888);
    decoder->windowWidth = selected->width;
    decoder->windowHeight = selected->height;
  }
  ANativeWindow_Buffer buffer{};
  int result = ANativeWindow_lock(window, &buffer, nullptr);
  if (result == 0) {
    decoder->scaler = sws_getCachedContext(decoder->scaler,
        selected->width, selected->height, static_cast<AVPixelFormat>(selected->format),
        buffer.width, buffer.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (decoder->scaler != nullptr) {
      uint8_t* dst[4] = {static_cast<uint8_t*>(buffer.bits), nullptr, nullptr, nullptr};
      int stride[4] = {buffer.stride * 4, 0, 0, 0};
      sws_scale(decoder->scaler, selected->data, selected->linesize, 0, selected->height, dst, stride);
    }
    ANativeWindow_unlockAndPost(window);
  }
  ANativeWindow_release(window);
  av_frame_free(&selected);
  return result == 0 ? JNI_TRUE : JNI_FALSE;
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
