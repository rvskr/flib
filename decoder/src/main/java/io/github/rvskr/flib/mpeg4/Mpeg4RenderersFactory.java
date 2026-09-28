package io.github.rvskr.flib.mpeg4;

import android.content.Context;
import android.os.Handler;
import androidx.annotation.Nullable;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.Renderer;
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector;
import androidx.media3.exoplayer.video.VideoRendererEventListener;
import java.util.ArrayList;

/** Adds the MPEG-4 Part 2 software renderer as a fallback, or first when explicitly forced. */
@UnstableApi
public final class Mpeg4RenderersFactory extends DefaultRenderersFactory {
  private final boolean forceSoftwareMpeg4;

  public Mpeg4RenderersFactory(Context context, boolean forceSoftwareMpeg4) {
    super(context);
    this.forceSoftwareMpeg4 = forceSoftwareMpeg4;
  }

  @Override
  protected void buildVideoRenderers(
      Context context,
      @ExtensionRendererMode int extensionRendererMode,
      MediaCodecSelector mediaCodecSelector,
      boolean enableDecoderFallback,
      Handler eventHandler,
      VideoRendererEventListener eventListener,
      long allowedVideoJoiningTimeMs,
      ArrayList<Renderer> out) {
    if (forceSoftwareMpeg4) {
      out.add(new Mpeg4SoftwareVideoRenderer(allowedVideoJoiningTimeMs, eventHandler, eventListener));
    }
    super.buildVideoRenderers(context, extensionRendererMode, mediaCodecSelector,
        enableDecoderFallback, eventHandler, eventListener, allowedVideoJoiningTimeMs, out);
    if (!forceSoftwareMpeg4) {
      out.add(new Mpeg4SoftwareVideoRenderer(allowedVideoJoiningTimeMs, eventHandler, eventListener));
    }
  }

  @Override
  @Nullable
  public Renderer createSecondaryRenderer(
      Renderer renderer,
      Handler eventHandler,
      VideoRendererEventListener videoRendererEventListener,
      androidx.media3.exoplayer.audio.AudioRendererEventListener audioRendererEventListener,
      androidx.media3.exoplayer.text.TextOutput textRendererOutput,
      androidx.media3.exoplayer.metadata.MetadataOutput metadataRendererOutput) {
    if (renderer instanceof Mpeg4SoftwareVideoRenderer) return null;
    return super.createSecondaryRenderer(renderer, eventHandler, videoRendererEventListener,
        audioRendererEventListener, textRendererOutput, metadataRendererOutput);
  }
}
