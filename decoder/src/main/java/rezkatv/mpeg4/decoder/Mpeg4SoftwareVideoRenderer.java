package rezkatv.mpeg4.decoder;

import android.os.Handler;
import android.view.Surface;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.decoder.DecoderInputBuffer;
import androidx.media3.exoplayer.BaseRenderer;
import androidx.media3.exoplayer.ExoPlaybackException;
import androidx.media3.exoplayer.FormatHolder;
import androidx.media3.exoplayer.RendererCapabilities;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.video.VideoRendererEventListener;
import java.nio.ByteBuffer;
import java.io.ByteArrayOutputStream;
import java.util.List;

/** FFmpeg software renderer limited to MPEG-4 Part 2 elementary video samples. */
@UnstableApi
public final class Mpeg4SoftwareVideoRenderer extends BaseRenderer {
  private static final String TAG = "Mpeg4VideoRenderer";
  private static final int MAX_SAMPLES_PER_RENDER = 4;
  private static final int MAX_SAMPLE_BYTES = 8 * 1024 * 1024;

  static { System.loadLibrary("rezka_mp4v"); }

  private final DecoderInputBuffer sampleBuffer =
      new DecoderInputBuffer(DecoderInputBuffer.BUFFER_REPLACEMENT_MODE_DIRECT);
  private final VideoRendererEventListener.EventDispatcher events;
  private final androidx.media3.exoplayer.DecoderCounters counters =
      new androidx.media3.exoplayer.DecoderCounters();
  private long decoder;
  @Nullable private Format format;
  @Nullable private Surface surface;
  private boolean inputEnded;
  private boolean outputEnded;

  public Mpeg4SoftwareVideoRenderer(
      long allowedJoiningTimeMs,
      @Nullable Handler eventHandler,
      @Nullable VideoRendererEventListener eventListener) {
    super(C.TRACK_TYPE_VIDEO);
    events = new VideoRendererEventListener.EventDispatcher(eventHandler, eventListener);
  }

  @Override public String getName() { return TAG; }

  @Override
  public int supportsFormat(Format candidate) {
    if (!MimeTypes.VIDEO_MP4V.equals(candidate.sampleMimeType)) {
      return RendererCapabilities.create(C.FORMAT_UNSUPPORTED_TYPE);
    }
    if (candidate.cryptoType != C.CRYPTO_TYPE_NONE) {
      return RendererCapabilities.create(C.FORMAT_UNSUPPORTED_DRM);
    }
    return RendererCapabilities.create(
        C.FORMAT_HANDLED, ADAPTIVE_NOT_SEAMLESS, TUNNELING_NOT_SUPPORTED);
  }

  @Override
  public void render(long positionUs, long elapsedRealtimeUs) throws ExoPlaybackException {
    if (outputEnded) return;
    try {
      if (surface != null) nativeRenderFrame(decoder, surface, positionUs, 35_000);
      for (int i = 0; i < MAX_SAMPLES_PER_RENDER && !inputEnded; i++) {
        FormatHolder holder = getFormatHolder();
        sampleBuffer.clear();
        int result = readSource(holder, sampleBuffer, 0);
        if (result == C.RESULT_NOTHING_READ) break;
        if (result == C.RESULT_FORMAT_READ) {
          onSampleFormatChanged(holder.format);
          continue;
        }
        if (result != C.RESULT_BUFFER_READ) break;
        if (sampleBuffer.isEndOfStream()) {
          inputEnded = true;
          nativeSignalEndOfStream(decoder);
          continue;
        }
        sampleBuffer.flip();
        ByteBuffer bytes = sampleBuffer.data;
        if (bytes == null || !bytes.isDirect() || bytes.remaining() > MAX_SAMPLE_BYTES) {
          throw new IllegalStateException("Invalid or oversized MPEG-4 sample");
        }
        int decoded = nativeDecode(decoder, bytes, bytes.position(), bytes.remaining(),
            sampleBuffer.timeUs + getStreamOffsetUs());
        if (decoded < 0) throw new IllegalStateException("FFmpeg MPEG-4 decode failed: " + decoded);
        if (surface != null) nativeRenderFrame(decoder, surface, positionUs, 35_000);
      }
      outputEnded = inputEnded && !nativeHasFrames(decoder);
    } catch (Exception e) {
      throw createRendererException(e, format, PlaybackException.ERROR_CODE_DECODING_FAILED);
    }
  }

  private void onSampleFormatChanged(@Nullable Format newFormat) throws ExoPlaybackException {
    format = newFormat;
    if (format == null || !MimeTypes.VIDEO_MP4V.equals(format.sampleMimeType)) {
      throw createRendererException(new IllegalArgumentException("Unexpected video format"),
          format, PlaybackException.ERROR_CODE_DECODING_FORMAT_UNSUPPORTED);
    }
    if (decoder != 0) nativeRelease(decoder);
    byte[] extraData = joinInitializationData(format.initializationData);
    decoder = nativeCreate(extraData, 2);
    if (decoder == 0) {
      throw createRendererException(new IllegalStateException("FFmpeg mpeg4 decoder unavailable"),
          format, PlaybackException.ERROR_CODE_DECODER_INIT_FAILED);
    }
  }

  @Override public boolean isReady() {
    return isSourceReady() || (decoder != 0 && nativeHasFrames(decoder));
  }

  @Override public boolean isEnded() { return outputEnded; }

  @Override public void handleMessage(int messageType, @Nullable Object message) {
    if (messageType == MSG_SET_VIDEO_OUTPUT) {
      surface = message instanceof Surface ? (Surface) message : null;
    }
  }

  @Override protected void onEnabled(boolean joining, boolean mayRenderStartOfStream) {
    events.enabled(counters);
  }

  @Override protected void onStreamChanged(
      Format[] formats, long startPositionUs, long offsetUs, MediaSource.MediaPeriodId mediaPeriodId)
      throws ExoPlaybackException {
    if (formats.length > 0) onSampleFormatChanged(formats[0]);
  }

  @Override protected void onPositionReset(long positionUs, boolean joining,
      boolean sampleStreamIsResetToKeyFrame) {
    inputEnded = false;
    outputEnded = false;
    if (decoder != 0) nativeFlush(decoder);
  }

  @Override protected void onDisabled() {
    surface = null;
    format = null;
    inputEnded = false;
    outputEnded = false;
    events.disabled(counters);
  }

  @Override protected void onReset() {
    if (decoder != 0) nativeRelease(decoder);
    decoder = 0;
  }

  @Override protected void onRelease() { onReset(); }

  private static byte[] joinInitializationData(List<byte[]> parts) {
    if (parts.isEmpty()) return null;
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    for (byte[] part : parts) out.write(part, 0, part.length);
    return out.toByteArray();
  }

  private static native long nativeCreate(@Nullable byte[] extraData, int threads);
  private static native int nativeDecode(long decoder, ByteBuffer packet, int offset, int size, long ptsUs);
  private static native void nativeSignalEndOfStream(long decoder);
  private static native void nativeFlush(long decoder);
  private static native boolean nativeHasFrames(long decoder);
  private static native boolean nativeRenderFrame(long decoder, Surface surface, long positionUs, long lateUs);
  private static native void nativeRelease(long decoder);
}
