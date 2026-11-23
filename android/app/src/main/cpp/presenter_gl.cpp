// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - draws the two DS screens into the game's SurfaceView.
#include "presenter_gl.h"
#include "xbrz6x_shader.hpp"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>

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

// The 3D merge (core/gpu/split3d.h is the reference; the SDL frontend's
// present.frag DS_MERGE path does the same on Vulkan). The screen's frame is
// its picture without the 3D layer; per DS pixel the split's word says how a
// 3D texel T goes into it:
//   min((T * ka + C * kb + round) >> shift, 63)   per 6-bit channel,
// then the line's master brightness (o * m + n) >> 4 and the 6 -> 8 bit
// expansion, all exact in float. u_layer is the raster's record at u_scale
// texels a DS pixel (an RGBA8 buffer holding the words: r, g, b of RGB666 and
// the 5-bit alpha, a byte each). Pixels without 3D show the picture the other
// filters made (u_base, filtered like the plain path).
const char* kHiresFragment = R"(#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform sampler2D u_base, u_frame, u_layer;
uniform usampler2D u_words, u_xy, u_lines;
uniform int u_scale, u_flags, u_mode;   // u_flags: 1 u_xy holds captured pixels' places, 2 some line has BG0 scrolled
uniform vec2 u_out;
in vec2 v_uv;
out vec4 o_col;
const vec2 SRC = vec2(256.0, 192.0);
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
  vec2 f = clamp(v_uv * SRC, vec2(0.0), vec2(255.999, 191.999));
  ivec2 px = ivec2(f);
  uint w = texelFetch(u_words, px, 0).r;
  uint op = (w >> 28) & 3u;
  if (op == 0u) { o_col = base_colour(); return; }
  uint lt = texelFetch(u_lines, ivec2(px.y, 0), 0).r;
  vec2 away = vec2(0.0);   // where the 3D pixel lies from here, in whole DS pixels
  if ((u_flags & 1) != 0 && (w & 0x80000000u) != 0u) {
    uint xy = texelFetch(u_xy, px, 0).r;
    away = vec2(float(int(xy & 255u) - px.x), float(int((xy >> 8) & 255u) - px.y));
  } else if ((u_flags & 2) != 0) {   // BG0's X offset on this line, 9 bits signed: off the layer shows the frame
    away.x = float(int(lt << 3) >> 23);
    if (f.x + away.x < 0.0 || f.x + away.x >= 256.0) { o_col = base_colour(); return; }
  }
  ivec2 lim = ivec2(256, 192) * u_scale - 1;
  uvec4 p = uvec4(round(texelFetch(u_layer, clamp(ivec2((f + away) * float(u_scale)), ivec2(0), lim), 0) * 255.0));
  uint a3 = p.a & 31u;
  if (a3 == 0u) { o_col = base_colour(); return; }
  float q = (w & 0x40000000u) != 0u ? 2.0 : 1.0;   // a capture's 5 bits a channel
  vec3 t = floor(vec3(p.rgb) / q) * q;
  vec3 o;
  if (op == 3u) o = t;   // the 3D pixel as it is
  else {
    bool by_alpha = op == 2u;
    float ka = float(by_alpha ? a3 + 1u : (w >> 18) & 31u), kb = float(by_alpha ? 31u - a3 : (w >> 23) & 31u);
    vec3 c = vec3(float(w & 63u), float((w >> 6) & 63u), float((w >> 12) & 63u));
    o = min(floor((t * ka + c * kb + (by_alpha ? 16.0 : 8.0)) * (by_alpha ? 0.03125 : 0.0625)), 63.0);
    o = floor(o / q) * q;
    o = floor((o * float(lt & 255u) + float((lt >> 8) & 4095u)) * 0.0625);
  }
  o_col = vec4((o * 4.0 + floor(o * 0.0625)) / 255.0, 1.0);
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
  hi_front_.clear();
  hi_back_.clear();
}

void Presenter::set_window(int display, ANativeWindow* window) {
  std::unique_lock<std::mutex> lock(mu_);
  ANativeWindow*& pending = pending_window_[display];
  if (pending && pending != window) ANativeWindow_release(pending);
  pending = window;
  window_change_[display] = true;
  const uint64_t gen = ++window_gen_;
  cv_.notify_all();
  if (!thread_.joinable()) return;
  ack_cv_.wait(lock, [&] { return window_done_ >= gen || quit_; });
}

void Presenter::set_layout(int display, const ScreenRect& top, const ScreenRect& bottom) {
  std::lock_guard<std::mutex> lock(mu_);
  rect_[display][0] = top;
  rect_[display][1] = bottom;
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
  else hi_back_.clear();
  fresh_ = true;
  dirty_ = true;
  cv_.notify_all();
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

bool Presenter::make_surface(int d) {
  if (!init_gl()) return false;
  surface_[d] = eglCreateWindowSurface(display_, config_, window_[d], nullptr);
  if (surface_[d] == EGL_NO_SURFACE) { LOGW("eglCreateWindowSurface failed: %x", eglGetError()); return false; }
  if (!eglMakeCurrent(display_, surface_[d], surface_[d], context_)) { LOGW("eglMakeCurrent failed"); return false; }
  // The main display paces the presenter; the second one never waits for its
  // own vsync, which would halve the rate when both are shown.
  eglSwapInterval(display_, d == MAIN ? 1 : 0);
  return init_programs();
}

// The shared GL objects, made once with any surface current.
bool Presenter::init_programs() {
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

void Presenter::release_surface(int d) {
  if (display_ != EGL_NO_DISPLAY) eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface_[d] != EGL_NO_SURFACE) eglDestroySurface(display_, surface_[d]);
  surface_[d] = EGL_NO_SURFACE;
  if (window_[d]) ANativeWindow_release(window_[d]);
  window_[d] = nullptr;
}

void Presenter::release_all() {
  // Any live surface will do to make the context current for the deletes.
  EGLSurface any = surface_[MAIN] != EGL_NO_SURFACE ? surface_[MAIN] : surface_[SECOND];
  if (display_ != EGL_NO_DISPLAY && any != EGL_NO_SURFACE) {
    eglMakeCurrent(display_, any, any, context_);
    if (program_) glDeleteProgram(program_);
    if (tex_[0]) glDeleteTextures(2, tex_);
    if (xbrz_prog_) glDeleteProgram(xbrz_prog_);
    if (xbrz_vbo_) glDeleteBuffers(1, &xbrz_vbo_);
    if (xbrz_fbo_[0]) glDeleteFramebuffers(2, xbrz_fbo_);
    if (xbrz_tex_[0]) glDeleteTextures(2, xbrz_tex_);
    if (hi_prog_) glDeleteProgram(hi_prog_);
    if (hi_words_[0]) { glDeleteTextures(2, hi_words_); glDeleteTextures(2, hi_xy_); glDeleteTextures(2, hi_lines_); }
  }
  hi_prog_ = 0;
  for (int i = 0; i < 2; ++i) hi_words_[i] = hi_xy_[i] = hi_lines_[i] = hi_layer_[i] = 0;
  hi_uploaded_ = false;
  drop_imports(true);
  program_ = 0;
  tex_[0] = tex_[1] = 0;
  xbrz_prog_ = xbrz_vbo_ = 0;
  xbrz_fbo_[0] = xbrz_fbo_[1] = xbrz_tex_[0] = xbrz_tex_[1] = 0;
  for (int d = 0; d < DISPLAYS; ++d) release_surface(d);
  if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
  context_ = EGL_NO_CONTEXT;
  // Not eglTerminate: EGL_DEFAULT_DISPLAY is the process's, and the OpenGL ES
  // 3D raster (core/gpu/gl) has its own context on it.
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
  hi_u_frame_ = glGetUniformLocation(hi_prog_, "u_frame");
  hi_u_layer_ = glGetUniformLocation(hi_prog_, "u_layer");
  hi_u_words_ = glGetUniformLocation(hi_prog_, "u_words");
  hi_u_xy_ = glGetUniformLocation(hi_prog_, "u_xy");
  hi_u_lines_ = glGetUniformLocation(hi_prog_, "u_lines");
  hi_u_scale_ = glGetUniformLocation(hi_prog_, "u_scale");
  hi_u_flags_ = glGetUniformLocation(hi_prog_, "u_flags");
  hi_u_mode_ = glGetUniformLocation(hi_prog_, "u_mode");
  hi_u_out_ = glGetUniformLocation(hi_prog_, "u_out");
  auto make = [](GLuint* t, GLsizei w, GLsizei h) {
    glGenTextures(2, t);
    for (int i = 0; i < 2; ++i) {
      glBindTexture(GL_TEXTURE_2D, t[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
  };
  make(hi_words_, W, H);
  make(hi_xy_, W, H);
  make(hi_lines_, H, 1);
  return true;
}

GLuint Presenter::import_buffer(AHardwareBuffer* b) {
  for (const Imported& i : imported_) if (i.buffer == b) return i.tex;
  if (import_failed_) return 0;
  static auto get_client_buffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
  static auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  static auto target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!get_client_buffer || !create_image || !target_texture) { LOGW("no EGL image import; high-res 3D shown at 1x"); import_failed_ = true; return 0; }
  const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR image = create_image(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, get_client_buffer(b), attrs);
  if (image == EGL_NO_IMAGE_KHR) { LOGW("eglCreateImageKHR failed: %x", eglGetError()); import_failed_ = true; return 0; }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  target_texture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  AHardwareBuffer_acquire(b);   // the cache's own reference: the address cannot be reused while cached
  imported_.push_back({b, image, tex});
  return tex;
}

// Imports no frame refers to any more (all: every one, the context is going).
void Presenter::drop_imports(bool all) {
  static auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  for (size_t i = 0; i < imported_.size();) {
    Imported& im = imported_[i];
    if (!all && (im.buffer == hi_front_.s[0].buffer || im.buffer == hi_front_.s[1].buffer || im.buffer == hi_back_.s[0].buffer || im.buffer == hi_back_.s[1].buffer)) { ++i; continue; }
    if (context_ != EGL_NO_CONTEXT && eglGetCurrentContext() == context_) glDeleteTextures(1, &im.tex);
    if (destroy_image) destroy_image(display_, static_cast<EGLImageKHR>(im.image));
    AHardwareBuffer_release(im.buffer);
    imported_.erase(imported_.begin() + static_cast<long>(i));
  }
}

void Presenter::upload_hires() {
  // A raster rebuild brings new buffers: let the old ones go.
  if (imported_.size() > 16) drop_imports(false);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  for (int sc = 0; sc < 2; ++sc) {
    HiresScreen& h = hi_front_.s[sc];
    hi_layer_[sc] = 0;
    if (!h.on) continue;
    hi_layer_[sc] = h.buffer ? import_buffer(h.buffer) : 0;
    if (!hi_layer_[sc]) { h.on = false; continue; }   // the import failed: the picture without its 3D would show
    glBindTexture(GL_TEXTURE_2D, hi_words_[sc]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.words.data());
    glBindTexture(GL_TEXTURE_2D, hi_lines_[sc]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, H, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, h.lines.data());
    if (h.coords) {
      glBindTexture(GL_TEXTURE_2D, hi_xy_[sc]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.xy.data());
    }
  }
  hi_uploaded_ = true;
}

void Presenter::draw() {
  ScreenRect rect[DISPLAYS][2];
  int filter;
  {
    std::lock_guard<std::mutex> lock(mu_);
    std::memcpy(rect, rect_, sizeof rect);
    filter = filter_;
    if (fresh_) {
      front_.swap(back_); std::memcpy(back_.data(), front_.data(), W * H * 2 * 4); fresh_ = false; have_frame_ = false;
      std::swap(hi_front_, hi_back_); hi_back_.clear(); hi_uploaded_ = false;
    }
    dirty_ = false;
  }
  // The uploads and passes below run once, with whichever surface is live.
  const int first = surface_[MAIN] != EGL_NO_SURFACE ? MAIN : SECOND;
  if (surface_[first] == EGL_NO_SURFACE || !eglMakeCurrent(display_, surface_[first], surface_[first], context_)) return;
  bool shown[2];
  for (int s = 0; s < 2; ++s) shown[s] = (surface_[MAIN] != EGL_NO_SURFACE && rect[MAIN][s].shown) || (surface_[SECOND] != EGL_NO_SURFACE && rect[SECOND][s].shown);
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
      if (!shown[s] || !xbrz_stale_[s]) continue;
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
  // The screens whose 3D is merged in at the raster's scale.
  bool hi_on[2] = {false, false};
  if (hi_front_.any() && init_hires()) {
    if (!hi_uploaded_) upload_hires();
    for (int sc = 0; sc < 2; ++sc) hi_on[sc] = hi_front_.s[sc].on && hi_layer_[sc];
  }
  // The second display first: the main one's swap waits for vsync.
  for (int d : {SECOND, MAIN}) {
    if (surface_[d] == EGL_NO_SURFACE) continue;
    if (!eglMakeCurrent(display_, surface_[d], surface_[d], context_)) continue;
    draw_display(d, rect[d], filter, xbrz, hi_on);
  }
}

void Presenter::draw_display(int d, const ScreenRect* rect, int filter, bool xbrz, const bool* hi_on) {
  const EGLint sw = ANativeWindow_getWidth(window_[d]), sh = ANativeWindow_getHeight(window_[d]);
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
    if (hi_on[s]) {
      const HiresScreen& h = hi_front_.s[s];
      glUseProgram(hi_prog_);
      glUniform1i(hi_u_base_, 0);
      glUniform1i(hi_u_frame_, 1);
      glUniform1i(hi_u_layer_, 2);
      glUniform1i(hi_u_words_, 3);
      glUniform1i(hi_u_xy_, 4);
      glUniform1i(hi_u_lines_, 5);
      glUniform1i(hi_u_scale_, static_cast<GLint>(h.scale));
      glUniform1i(hi_u_flags_, (h.coords ? 1 : 0) | (h.scrolled ? 2 : 0));
      glUniform1i(hi_u_mode_, xbrz ? LINEAR : filter);
      glUniform2f(hi_u_out_, static_cast<float>(r.w), static_cast<float>(r.h));
      const GLuint units[6] = {0, tex_[s], hi_layer_[s], hi_words_[s], hi_xy_[s], hi_lines_[s]};
      for (int t = 1; t < 6; ++t) { glActiveTexture(GL_TEXTURE0 + t); glBindTexture(GL_TEXTURE_2D, units[t]); }
      glActiveTexture(GL_TEXTURE0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glUseProgram(program_);
      continue;
    }
    glUniform2f(u_out_, static_cast<float>(r.w), static_cast<float>(r.h));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (!eglSwapBuffers(display_, surface_[d])) {
    const EGLint e = eglGetError();
    if (e == EGL_BAD_SURFACE || e == EGL_BAD_NATIVE_WINDOW) {
      // The surface went away under us; wait for the next window.
      release_surface(d);
    }
  }
}

void Presenter::run() {
  pthread_setname_np(pthread_self(), "NinDS present");
  auto any_change = [&] { return window_change_[MAIN] || window_change_[SECOND]; };
  auto any_window = [&] { return window_[MAIN] || window_[SECOND]; };
  for (;;) {
    ANativeWindow* adopt[DISPLAYS] = {};
    bool change[DISPLAYS] = {};
    uint64_t gen = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return quit_ || any_change() || (dirty_ && any_window()); });
      if (quit_) break;
      for (int d = 0; d < DISPLAYS; ++d) {
        if (!window_change_[d]) continue;
        change[d] = true;
        adopt[d] = pending_window_[d];
        pending_window_[d] = nullptr;
        window_change_[d] = false;
      }
      gen = window_gen_;
    }
    if (change[MAIN] || change[SECOND]) {
      for (int d = 0; d < DISPLAYS; ++d) {
        if (!change[d]) continue;
        release_surface(d);
        if (adopt[d]) {
          window_[d] = adopt[d];
          if (!make_surface(d)) release_surface(d);
        }
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        window_done_ = gen;
        dirty_ = true;
      }
      ack_cv_.notify_all();
    }
    if (surface_[MAIN] != EGL_NO_SURFACE || surface_[SECOND] != EGL_NO_SURFACE) {
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
  for (ANativeWindow*& w : pending_window_) if (w) { ANativeWindow_release(w); w = nullptr; }
  window_done_ = window_gen_;
  ack_cv_.notify_all();
}

} // namespace ninds
