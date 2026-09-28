#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include <android/native_window.h>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

struct GlOutput {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  ANativeWindow* window = nullptr;
  GLuint program = 0;
  GLuint texture = 0;
  GLint positionAttribute = -1;
  GLint texcoordAttribute = -1;
  GLint samplerUniform = -1;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

void ReleaseGlOutput(GlOutput* output);
bool RenderGlFrame(GlOutput* output, JNIEnv* env, jobject surface, const AVFrame* frame,
                   SwsContext** scaler, int64_t* convertUs, int64_t* postUs);
