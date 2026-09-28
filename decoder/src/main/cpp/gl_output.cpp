#include "gl_output.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <chrono>

namespace {
constexpr char kTag[] = "RezkaMp4v";
constexpr char kVertexShader[] =
    "attribute vec2 position; attribute vec2 texcoord; varying vec2 uv; "
    "void main() { gl_Position = vec4(position, 0.0, 1.0); uv = texcoord; }";
constexpr char kFragmentShader[] =
    "precision mediump float; varying vec2 uv; uniform sampler2D image; "
    "void main() { gl_FragColor = texture2D(image, uv); }";

int64_t MonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

GLuint CompileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  if (!shader) return 0;
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) return shader;
  char message[512] = {};
  glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
  __android_log_print(ANDROID_LOG_ERROR, kTag, "GL shader compile failed: %s", message);
  glDeleteShader(shader);
  return 0;
}

bool Initialize(GlOutput* output, JNIEnv* env, jobject surface, int width, int height) {
  output->window = ANativeWindow_fromSurface(env, surface);
  if (!output->window) return false;
  output->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (output->display == EGL_NO_DISPLAY || !eglInitialize(output->display, nullptr, nullptr))
    return false;
  const EGLint attributes[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (!eglChooseConfig(output->display, attributes, &config, 1, &count) || count == 0)
    return false;
  EGLint visualId = 0;
  if (!eglGetConfigAttrib(output->display, config, EGL_NATIVE_VISUAL_ID, &visualId))
    return false;
  if (ANativeWindow_setBuffersGeometry(output->window, width, height, visualId) != 0)
    return false;
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;
  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  output->context = eglCreateContext(output->display, config, EGL_NO_CONTEXT,
                                     contextAttributes);
  if (output->context == EGL_NO_CONTEXT) return false;
  output->surface = eglCreateWindowSurface(output->display, config, output->window, nullptr);
  if (output->surface == EGL_NO_SURFACE ||
      !eglMakeCurrent(output->display, output->surface, output->surface, output->context))
    return false;
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return false;
  }
  output->program = glCreateProgram();
  glAttachShader(output->program, vertex);
  glAttachShader(output->program, fragment);
  glLinkProgram(output->program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(output->program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) return false;
  output->positionAttribute = glGetAttribLocation(output->program, "position");
  output->texcoordAttribute = glGetAttribLocation(output->program, "texcoord");
  output->samplerUniform = glGetUniformLocation(output->program, "image");
  if (output->positionAttribute < 0 || output->texcoordAttribute < 0 ||
      output->samplerUniform < 0) return false;
  glGenTextures(1, &output->texture);
  if (!output->texture) return false;
  glBindTexture(GL_TEXTURE_2D, output->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  output->width = width;
  output->height = height;
  output->pixels.resize(static_cast<size_t>(width) * height * 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  __android_log_print(ANDROID_LOG_INFO, kTag, "EGL output initialized %dx%d", width, height);
  return glGetError() == GL_NO_ERROR;
}
}  // namespace

void ReleaseGlOutput(GlOutput* output) {
  if (output->display != EGL_NO_DISPLAY) {
    if (output->context != EGL_NO_CONTEXT && output->surface != EGL_NO_SURFACE)
      eglMakeCurrent(output->display, output->surface, output->surface, output->context);
    if (output->texture) glDeleteTextures(1, &output->texture);
    if (output->program) glDeleteProgram(output->program);
    eglMakeCurrent(output->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (output->surface != EGL_NO_SURFACE) eglDestroySurface(output->display, output->surface);
    if (output->context != EGL_NO_CONTEXT) eglDestroyContext(output->display, output->context);
    eglTerminate(output->display);
  }
  if (output->window) ANativeWindow_release(output->window);
  *output = GlOutput{};
}

bool RenderGlFrame(GlOutput* output, JNIEnv* env, jobject surface, const AVFrame* frame,
                   SwsContext** scaler, int64_t* convertUs, int64_t* postUs) {
  if (output->surface == EGL_NO_SURFACE || output->width != frame->width ||
      output->height != frame->height) {
    ReleaseGlOutput(output);
    if (!Initialize(output, env, surface, frame->width, frame->height)) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "EGL output unavailable: 0x%x", eglGetError());
      ReleaseGlOutput(output);
      return false;
    }
  }
  *scaler = sws_getCachedContext(*scaler, frame->width, frame->height,
      static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
      AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!*scaler) return false;
  uint8_t* dst[4] = {output->pixels.data(), nullptr, nullptr, nullptr};
  int stride[4] = {frame->width * 4, 0, 0, 0};
  const int64_t convertStartUs = MonotonicUs();
  const int rows = sws_scale(*scaler, frame->data, frame->linesize, 0, frame->height, dst, stride);
  *convertUs += MonotonicUs() - convertStartUs;
  if (rows != frame->height) return false;

  const int64_t postStartUs = MonotonicUs();
  glViewport(0, 0, frame->width, frame->height);
  glUseProgram(output->program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, output->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
                  GL_RGBA, GL_UNSIGNED_BYTE, output->pixels.data());
  glUniform1i(output->samplerUniform, 0);
  const GLfloat quad[] = {
      -1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
      -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f};
  glVertexAttribPointer(output->positionAttribute, 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(GLfloat), quad);
  glVertexAttribPointer(output->texcoordAttribute, 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(GLfloat), quad + 2);
  glEnableVertexAttribArray(output->positionAttribute);
  glEnableVertexAttribArray(output->texcoordAttribute);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  const GLenum error = glGetError();
  const bool swapped = error == GL_NO_ERROR && eglSwapBuffers(output->display, output->surface);
  *postUs += MonotonicUs() - postStartUs;
  if (!swapped) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "EGL frame failed: GL=0x%x EGL=0x%x",
                        error, eglGetError());
    ReleaseGlOutput(output);
  }
  return swapped;
}
