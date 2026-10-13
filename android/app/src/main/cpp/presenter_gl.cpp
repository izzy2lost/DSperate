// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - draws the two DS screens into the game's SurfaceView.
#include "presenter_gl.h"
#include "xbrz6x_shader.hpp"

#include <android/log.h>
#include <pthread.h>

#include <chrono>
#include <cstring>
#include <string>

#define LOG_TAG "NinDS-video"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ninds {

namespace {
constexpr int W = 256, H = 192;

// One quad from gl_VertexID; each screen is drawn into its own viewport.
const char* kVertex = R"(#version 300 es
out vec2 v_uv;
void main() {
  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The core's pixels are 0xAARRGGBB words: in memory B, G, R, A. The source
// textures swap red and blue back with a texture swizzle, so every shader
// (including vendored ones) reads plain .rgb.
const char* kFragment = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex;
uniform int u_mode;      // 0 nearest, 1 linear (sampler state), 2 sharp bilinear, 3 LCD grid
uniform vec2 u_out;      // viewport size in pixels
in vec2 v_uv;
out vec4 o_col;
const vec2 SRC = vec2(256.0, 192.0);
void main() {
  vec2 uv = v_uv;
  float shade = 1.0;
  if (u_mode == 2) {
    vec2 scale = max(u_out / SRC, vec2(1.0));
    vec2 texel = uv * SRC;
    vec2 base = floor(texel);
    vec2 d = fract(texel) - 0.5;
    vec2 region = 0.5 - 0.5 / scale;
    vec2 f = (d - clamp(d, -region, region)) * scale + 0.5;
    uv = (base + f) / SRC;
  } else if (u_mode == 3) {
    vec2 scale = u_out / SRC;
    if (min(scale.x, scale.y) >= 2.5) {
      vec2 p = fract(uv * SRC);
      vec2 edge = min(p, 1.0 - p) * scale;   // distance to the cell edge in output pixels
      shade = mix(0.72, 1.0, smoothstep(0.0, 1.0, min(edge.x, edge.y)));
    }
  }
  o_col = vec4(texture(u_tex, uv).rgb * shade, 1.0);
}
)";

constexpr int XBRZ_SCALE = 6;

// Engine A's screen with its 3D at the internal resolution: each output pixel
// redoes the last compositing step the core did at 1x (engine2d.h HiresMode),
// over the scaled 3D sample, then master brightness and the 6->8 bit
// expansion, all in the core's integer arithmetic. Pixels without 3D show the
// picture the other filters made (u_base, filtered like the plain path).
const char* kHiresFragment = R"(#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform sampler2D u_base;
uniform usampler2D u_layer, u_desc, u_oper, u_line;
uniform int u_scale, u_screen, u_mode;
uniform vec2 u_out;
in vec2 v_uv;
out vec4 o_col;
const vec2 SRC = vec2(256.0, 192.0);
uvec3 ch(uint p) { return uvec3(p & 63u, (p >> 8) & 63u, (p >> 16) & 63u); }
vec4 base_colour() {
  vec2 uv = v_uv;
  if (u_mode == 2) {
    vec2 scale = max(u_out / SRC, vec2(1.0));
    vec2 texel = uv * SRC;
    vec2 d = fract(texel) - 0.5;
    vec2 region = 0.5 - 0.5 / scale;
    uv = (floor(texel) + (d - clamp(d, -region, region)) * scale + 0.5) / SRC;
  }
  return vec4(texture(u_base, uv).rgb, 1.0);
}
void main() {
  ivec2 px = clamp(ivec2(v_uv * SRC), ivec2(0), ivec2(255, 191));
  uint meta = texelFetch(u_line, ivec2(px.y, 0), 0).r;
  if ((meta & 1u) == 0u || int((meta >> 1) & 1u) != u_screen) { o_col = base_colour(); return; }
  uint d = texelFetch(u_desc, px, 0).r;
  uint mode = d & 7u;
  if (mode == 0u) { o_col = base_colour(); return; }
  ivec2 hp = clamp(ivec2(v_uv * SRC * float(u_scale)), ivec2(0), ivec2(256 * u_scale - 1, 192 * u_scale - 1));
  uint h = texelFetch(u_layer, hp, 0).r;
  uint a5 = (h >> 24) & 31u;
  uvec3 c3 = ch(h), op = ch(texelFetch(u_oper, px, 0).r);
  uint eva = (d >> 3) & 31u, evb = (d >> 8) & 31u, evy = (d >> 13) & 31u;
  uvec3 c;
  if (a5 == 0u) c = op;   // this sample of the pixel is not covered by 3D: what lies beneath
  else if (mode == 1u) c = c3;
  else if (mode == 2u) { uint ea = a5 + 1u; c = ea == 32u ? c3 : min(uvec3(63u), (c3 * ea + op * (32u - ea) + 16u) >> 5); }
  else if (mode == 3u) c = min(uvec3(63u), (c3 * eva + op * evb + 8u) >> 4);
  else if (mode == 4u) c = c3 + (((uvec3(63u) - c3) * evy + 8u) >> 4);
  else if (mode == 5u) c = c3 - ((c3 * evy + 7u) >> 4);
  else c = min(uvec3(63u), (op * eva + c3 * evb + 8u) >> 4);
  uint mb = meta >> 16, mm = mb >> 14, f = min(mb & 31u, 16u);
  if (mm == 1u) c = c + (((uvec3(63u) - c) * f) >> 4);
  else if (mm == 2u) c = c - ((c * f + 15u) >> 4);
  o_col = vec4(vec3((c << 2) | (c >> 4)) / 255.0, 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    LOGW("shader: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}
} // namespace

Presenter::Presenter() : back_(W * H * 2, 0xFF000000u), front_(W * H * 2, 0xFF000000u) {}

Presenter::~Presenter() { stop(); }

void Presenter::start() {
  if (thread_.joinable()) return;
  quit_ = false;
  thread_ = std::thread([this] { run(); });
}

void Presenter::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void Presenter::set_window(ANativeWindow* window) {
  std::unique_lock<std::mutex> lock(mu_);
  if (pending_window_ && pending_window_ != window) ANativeWindow_release(pending_window_);
  pending_window_ = window;
  window_change_ = true;
  const uint64_t gen = ++window_gen_;
  cv_.notify_all();
  if (!thread_.joinable()) return;
  ack_cv_.wait(lock, [&] { return window_done_ >= gen || quit_; });
}

void Presenter::set_layout(const ScreenRect& top, const ScreenRect& bottom) {
  std::lock_guard<std::mutex> lock(mu_);
  rect_[0] = top;
  rect_[1] = bottom;
  dirty_ = true;
  cv_.notify_all();
}

void Presenter::set_filter(int filter) {
  std::lock_guard<std::mutex> lock(mu_);
  filter_ = filter;
  dirty_ = true;
  cv_.notify_all();
}

void Presenter::submit(const uint32_t* top, const uint32_t* bottom, HiresFrame* hires) {
  std::lock_guard<std::mutex> lock(mu_);
  std::memcpy(back_.data(), top, W * H * 4);
  std::memcpy(back_.data() + W * H, bottom, W * H * 4);
  if (hires) std::swap(hi_back_, *hires);
  else hi_back_.valid = false;
  fresh_ = true;
  dirty_ = true;
  cv_.notify_all();
}

void Presenter::snapshot(std::vector<uint32_t>& out) {
  std::lock_guard<std::mutex> lock(mu_);
  out = back_;
}

bool Presenter::init_gl() {
  if (display_ == EGL_NO_DISPLAY) {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) { LOGW("eglInitialize failed"); return false; }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(display_, attrs, &config_, 1, &n) || n < 1) { LOGW("no ES3 config"); return false; }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctx_attrs);
    if (context_ == EGL_NO_CONTEXT) { LOGW("eglCreateContext failed"); return false; }
  }
  return true;
}

bool Presenter::make_current(ANativeWindow* w) {
  if (!init_gl()) return false;
  surface_ = eglCreateWindowSurface(display_, config_, w, nullptr);
  if (surface_ == EGL_NO_SURFACE) { LOGW("eglCreateWindowSurface failed: %x", eglGetError()); return false; }
  if (!eglMakeCurrent(display_, surface_, surface_, context_)) { LOGW("eglMakeCurrent failed"); return false; }
  eglSwapInterval(display_, 1);
  if (!program_) {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    u_tex_ = glGetUniformLocation(program_, "u_tex");
    u_mode_ = glGetUniformLocation(program_, "u_mode");
    u_out_ = glGetUniformLocation(program_, "u_out");
    glGenTextures(2, tex_);
    for (GLuint t : tex_) {
      glBindTexture(GL_TEXTURE_2D, t);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    have_frame_ = false;
  }
  return true;
}

void Presenter::release_surface() {
  if (display_ != EGL_NO_DISPLAY) eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
  surface_ = EGL_NO_SURFACE;
  if (window_) ANativeWindow_release(window_);
  window_ = nullptr;
}

void Presenter::release_all() {
  if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
    eglMakeCurrent(display_, surface_, surface_, context_);
    if (program_) glDeleteProgram(program_);
    if (tex_[0]) glDeleteTextures(2, tex_);
    if (xbrz_prog_) glDeleteProgram(xbrz_prog_);
    if (xbrz_vbo_) glDeleteBuffers(1, &xbrz_vbo_);
    if (xbrz_fbo_[0]) glDeleteFramebuffers(2, xbrz_fbo_);
    if (xbrz_tex_[0]) glDeleteTextures(2, xbrz_tex_);
    if (hi_prog_) glDeleteProgram(hi_prog_);
    if (hi_tex_[0]) glDeleteTextures(4, hi_tex_);
  }
  hi_prog_ = 0;
  hi_tex_[0] = hi_tex_[1] = hi_tex_[2] = hi_tex_[3] = 0;
  hi_layer_scale_ = 0;
  hi_uploaded_ = false;
  program_ = 0;
  tex_[0] = tex_[1] = 0;
  xbrz_prog_ = xbrz_vbo_ = 0;
  xbrz_fbo_[0] = xbrz_fbo_[1] = xbrz_tex_[0] = xbrz_tex_[1] = 0;
  release_surface();
  if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
  context_ = EGL_NO_CONTEXT;
  if (display_ != EGL_NO_DISPLAY) eglTerminate(display_);
  display_ = EGL_NO_DISPLAY;
}

bool Presenter::init_xbrz() {
  if (xbrz_prog_) return true;
  if (xbrz_failed_) return false;
  // The vendored GLSL asks for ES 3.1 but uses nothing past 3.0; drop to 3.00
  // only where the driver refuses 3.10.
  for (const char* version : {"#version 310 es", "#version 300 es"}) {
    std::string vs = shaders::kXbrz6xVertex, fs = shaders::kXbrz6xFragment;
    vs.replace(vs.find("#version 310 es"), 15, version);
    fs.replace(fs.find("#version 310 es"), 15, version);
    GLuint v = compile(GL_VERTEX_SHADER, vs.c_str()), f = compile(GL_FRAGMENT_SHADER, fs.c_str());
    if (v && f) {
      GLuint p = glCreateProgram();
      glAttachShader(p, v);
      glAttachShader(p, f);
      glBindAttribLocation(p, 0, "VertexCoord");
      glBindAttribLocation(p, 1, "TexCoord");
      glLinkProgram(p);
      GLint ok = 0;
      glGetProgramiv(p, GL_LINK_STATUS, &ok);
      if (ok) xbrz_prog_ = p;
      else glDeleteProgram(p);
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    if (xbrz_prog_) break;
  }
  if (!xbrz_prog_) { LOGW("xBRZ shader unavailable; using sharp bilinear"); xbrz_failed_ = true; return false; }
  xbrz_u_tex_ = glGetUniformLocation(xbrz_prog_, "Texture");
  xbrz_u_texsize_ = glGetUniformLocation(xbrz_prog_, "TextureSize");
  xbrz_u_drawsize_ = glGetUniformLocation(xbrz_prog_, "DrawingSize");
  // Clip-space position, then texture coordinate: texture row 0 (the DS's top
  // line) lands on framebuffer row 0, the same orientation as the source.
  const float quad[] = {-1.f, -1.f, 0.f, 0.f,  1.f, -1.f, 1.f, 0.f,  -1.f, 1.f, 0.f, 1.f,  1.f, 1.f, 1.f, 1.f};
  glGenBuffers(1, &xbrz_vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, xbrz_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glGenTextures(2, xbrz_tex_);
  glGenFramebuffers(2, xbrz_fbo_);
  for (int s = 0; s < 2; ++s) {
    glBindTexture(GL_TEXTURE_2D, xbrz_tex_[s]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W * XBRZ_SCALE, H * XBRZ_SCALE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, xbrz_fbo_[s]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xbrz_tex_[s], 0);
    xbrz_stale_[s] = true;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

bool Presenter::init_hires() {
  if (hi_prog_) return true;
  if (hi_failed_) return false;
  GLuint v = compile(GL_VERTEX_SHADER, kVertex), f = compile(GL_FRAGMENT_SHADER, kHiresFragment);
  if (v && f) {
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok) hi_prog_ = p;
    else glDeleteProgram(p);
  }
  if (v) glDeleteShader(v);
  if (f) glDeleteShader(f);
  if (!hi_prog_) { LOGW("high-res 3D composite shader unavailable; showing the 1x picture"); hi_failed_ = true; return false; }
  hi_u_base_ = glGetUniformLocation(hi_prog_, "u_base");
  hi_u_layer_ = glGetUniformLocation(hi_prog_, "u_layer");
  hi_u_desc_ = glGetUniformLocation(hi_prog_, "u_desc");
  hi_u_oper_ = glGetUniformLocation(hi_prog_, "u_oper");
  hi_u_line_ = glGetUniformLocation(hi_prog_, "u_line");
  hi_u_scale_ = glGetUniformLocation(hi_prog_, "u_scale");
  hi_u_screen_ = glGetUniformLocation(hi_prog_, "u_screen");
  hi_u_mode_ = glGetUniformLocation(hi_prog_, "u_mode");
  hi_u_out_ = glGetUniformLocation(hi_prog_, "u_out");
  glGenTextures(3, hi_tex_);
  const GLsizei dims[3][2] = {{W, H}, {W, H}, {H, 1}};
  for (int i = 0; i < 3; ++i) {
    glBindTexture(GL_TEXTURE_2D, hi_tex_[i]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, dims[i][0], dims[i][1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  return true;
}

void Presenter::upload_hires() {
  const HiresFrame& h = hi_front_;
  if (h.scale != hi_layer_scale_) {
    // Immutable storage: a new scale is a new texture.
    if (hi_tex_[3]) glDeleteTextures(1, &hi_tex_[3]);
    glGenTextures(1, &hi_tex_[3]);
    glBindTexture(GL_TEXTURE_2D, hi_tex_[3]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, W * h.scale, H * h.scale);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    hi_layer_scale_ = h.scale;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, hi_tex_[0]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.desc.data());
  glBindTexture(GL_TEXTURE_2D, hi_tex_[1]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.operand.data());
  glBindTexture(GL_TEXTURE_2D, hi_tex_[2]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, H, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, h.line.data());
  glBindTexture(GL_TEXTURE_2D, hi_tex_[3]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W * h.scale, H * h.scale, GL_RED_INTEGER, GL_UNSIGNED_INT, h.layer.data());
  hi_uploaded_ = true;
}

void Presenter::draw() {
  ScreenRect rect[2];
  int filter;
  {
    std::lock_guard<std::mutex> lock(mu_);
    rect[0] = rect_[0];
    rect[1] = rect_[1];
    filter = filter_;
    if (fresh_) {
      front_.swap(back_); std::memcpy(back_.data(), front_.data(), W * H * 2 * 4); fresh_ = false; have_frame_ = false;
      std::swap(hi_front_, hi_back_); hi_back_.valid = false; hi_uploaded_ = false;
    }
    dirty_ = false;
  }
  if (!have_frame_) {
    for (int s = 0; s < 2; ++s) {
      glBindTexture(GL_TEXTURE_2D, tex_[s]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, front_.data() + s * W * H);
    }
    have_frame_ = true;
    xbrz_stale_[0] = xbrz_stale_[1] = true;
  }
  const bool xbrz = filter == XBRZ && init_xbrz();
  if (filter == XBRZ && !xbrz) filter = SHARP;
  if (xbrz) {
    // Pass 1: the 6x upscale, per shown screen, only when its picture changed.
    glUseProgram(xbrz_prog_);
    glUniform1i(xbrz_u_tex_, 0);
    glUniform2f(xbrz_u_texsize_, static_cast<float>(W), static_cast<float>(H));
    glUniform2f(xbrz_u_drawsize_, static_cast<float>(W), static_cast<float>(H));
    glBindBuffer(GL_ARRAY_BUFFER, xbrz_vbo_);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, W * XBRZ_SCALE, H * XBRZ_SCALE);
    for (int s = 0; s < 2; ++s) {
      if (!rect[s].shown || !xbrz_stale_[s]) continue;
      glBindFramebuffer(GL_FRAMEBUFFER, xbrz_fbo_[s]);
      glBindTexture(GL_TEXTURE_2D, tex_[s]);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      xbrz_stale_[s] = false;
    }
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  // Which screen (if any) shows engine A's 3D at the internal resolution.
  int hi_screen = -1;
  if (hi_front_.valid && hi_front_.scale > 1 && hi_front_.layer.size() >= static_cast<size_t>(W * H) * hi_front_.scale * hi_front_.scale && init_hires()) {
    for (uint32_t l : hi_front_.line) if (l & 1u) { hi_screen = static_cast<int>((l >> 1) & 1u); break; }
    if (hi_screen >= 0 && !hi_uploaded_) upload_hires();
  }
  const EGLint sw = ANativeWindow_getWidth(window_), sh = ANativeWindow_getHeight(window_);
  glViewport(0, 0, sw, sh);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(program_);
  glUniform1i(u_tex_, 0);
  // Pass 2 for xBRZ is a plain bilinear scale of its 6x picture to the window.
  glUniform1i(u_mode_, xbrz ? LINEAR : filter);
  glActiveTexture(GL_TEXTURE0);
  const GLint mag = (xbrz || filter == LINEAR || filter == SHARP) ? GL_LINEAR : GL_NEAREST;
  for (int s = 0; s < 2; ++s) {
    const ScreenRect& r = rect[s];
    if (!r.shown || r.w <= 0 || r.h <= 0) continue;
    if (xbrz) {
      glBindTexture(GL_TEXTURE_2D, xbrz_tex_[s]);
    } else {
      glBindTexture(GL_TEXTURE_2D, tex_[s]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mag);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    }
    glViewport(r.x, sh - r.y - r.h, r.w, r.h);
    if (s == hi_screen) {
      glUseProgram(hi_prog_);
      glUniform1i(hi_u_base_, 0);
      glUniform1i(hi_u_layer_, 1);
      glUniform1i(hi_u_desc_, 2);
      glUniform1i(hi_u_oper_, 3);
      glUniform1i(hi_u_line_, 4);
      glUniform1i(hi_u_scale_, static_cast<GLint>(hi_front_.scale));
      glUniform1i(hi_u_screen_, s);
      glUniform1i(hi_u_mode_, xbrz ? LINEAR : filter);
      glUniform2f(hi_u_out_, static_cast<float>(r.w), static_cast<float>(r.h));
      for (int t = 0; t < 4; ++t) { glActiveTexture(GL_TEXTURE1 + t); glBindTexture(GL_TEXTURE_2D, hi_tex_[t == 0 ? 3 : t - 1]); }
      glActiveTexture(GL_TEXTURE0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glUseProgram(program_);
      continue;
    }
    glUniform2f(u_out_, static_cast<float>(r.w), static_cast<float>(r.h));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (!eglSwapBuffers(display_, surface_)) {
    const EGLint e = eglGetError();
    if (e == EGL_BAD_SURFACE || e == EGL_BAD_NATIVE_WINDOW) {
      // The surface went away under us; wait for the next window.
      release_surface();
    }
  }
}

void Presenter::run() {
  pthread_setname_np(pthread_self(), "NinDS present");
  for (;;) {
    ANativeWindow* adopt = nullptr;
    bool change = false;
    uint64_t gen = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return quit_ || window_change_ || (dirty_ && window_); });
      if (quit_) break;
      if (window_change_) {
        change = true;
        adopt = pending_window_;
        pending_window_ = nullptr;
        window_change_ = false;
        gen = window_gen_;
      }
    }
    if (change) {
      release_surface();
      if (adopt) {
        window_ = adopt;
        if (!make_current(window_)) release_surface();
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        window_done_ = gen;
        dirty_ = true;
      }
      ack_cv_.notify_all();
    }
    if (window_ && surface_ != EGL_NO_SURFACE) {
      bool want;
      {
        std::lock_guard<std::mutex> lock(mu_);
        want = dirty_;
      }
      if (want) draw();
    }
  }
  release_all();
  std::lock_guard<std::mutex> lock(mu_);
  if (pending_window_) { ANativeWindow_release(pending_window_); pending_window_ = nullptr; }
  window_done_ = window_gen_;
  ack_cv_.notify_all();
}

} // namespace ninds
