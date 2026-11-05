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

// The edge cut (core/gpu/edge_cut.h is the reference; this is the SDL
// frontend's present.frag on GLES): each source pixel's panel block is cut
// along its 3D edge line instead of showing the 1x blend. u_edge words are
// 0xEERRGGBB: the edge byte (bit 7 edge, bit 6 the outside is the negative
// neighbour, bit 5 a vertical run, bits 0-4 coverage) over the pixel's own
// unblended colour. Nearest outside the cut.
const char* kEdgeFragment = R"(#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform sampler2D u_tex;    // the screen, RGBA8 (red and blue swizzled back)
uniform usampler2D u_edge;  // its edge plane, R32UI
uniform float u_bw;         // the blend band in DS pixels (0: the hard cut)
in vec2 v_uv;
out vec4 o_col;
uint frame_at(int x, int y) {
  uvec3 c = uvec3(texelFetch(u_tex, ivec2(x, y), 0).rgb * 255.0 + 0.5);
  return 0xFF000000u | (c.r << 16) | (c.g << 8) | c.b;
}
uint edge_at(int x, int y) { return texelFetch(u_edge, ivec2(x, y), 0).r; }
bool same_colour(uint a, uint b) {
  ivec3 d = abs(ivec3(int((a >> 16) & 255u), int((a >> 8) & 255u), int(a & 255u)) - ivec3(int((b >> 16) & 255u), int((b >> 8) & 255u), int(b & 255u)));
  return max(d.x, max(d.y, d.z)) <= 4;
}
float cut_pos(uint e) {
  float f = float((e & 31u) + 1u) / 32.0;
  return (e & 64u) != 0u ? 1.0 - f : f;
}
vec3 unpack(uint s) { return vec3(float((s >> 16) & 255u), float((s >> 8) & 255u), float(s & 255u)); }
float edge_line(uint e, int x, int y, float along) {
  bool vert = (e & 32u) != 0u, neg = (e & 64u) != 0u;
  int nx = x + (vert ? (along < 0.0 ? -1 : 1) : 0), ny = y + (vert ? 0 : (along < 0.0 ? -1 : 1));
  float pos = cut_pos(e), pn = pos;
  if (nx >= 0 && nx < 256 && ny >= 0 && ny < 192) {
    uint n = edge_at(nx, ny) >> 24;
    if ((n & 224u) == (e & 224u)) pn = cut_pos(n);
    else if ((n & 224u) == ((e ^ 64u) & 224u)) pn = cut_pos(n);
    else {
      int ox = nx + (vert ? 0 : (neg ? -1 : 1)), oy = ny + (vert ? (neg ? -1 : 1) : 0);
      if (ox >= 0 && ox < 256 && oy >= 0 && oy < 192) {
        uint o = edge_at(ox, oy) >> 24;
        if ((o & 224u) == ((e ^ 64u) & 224u)) pn = cut_pos(o) + (neg ? -1.0 : 1.0);
      }
    }
  }
  return pos + (pn - pos) * abs(along);
}
vec3 edge_cut(int sx, int sy, float u, float v, uint s, float bw) {
  uint w = edge_at(sx, sy);
  if ((w & 0x80000000u) == 0u) {
    vec3 c = unpack(s);
    if (bw <= 0.0 || w == 0u) return c;
    float best = 0.0;
    const ivec2 nb[4] = ivec2[4](ivec2(0, -1), ivec2(0, 1), ivec2(-1, 0), ivec2(1, 0));
    for (int k = 0; k < 4; ++k) {
      int qx = sx + nb[k].x, qy = sy + nb[k].y;
      if (qx < 0 || qx > 255 || qy < 0 || qy > 191) continue;
      uint wn = edge_at(qx, qy);
      if ((wn & 0x80000000u) == 0u) continue;
      uint en = wn >> 24;
      bool vn = (en & 32u) != 0u, gn = (en & 64u) != 0u;
      bool toward = vn ? (nb[k].x == 0 && (nb[k].y == -1 ? !gn : gn)) : (nb[k].y == 0 && (nb[k].x == -1 ? !gn : gn));
      if (!toward) continue;
      uint own = 0xFF000000u | (wn & 0xFFFFFFu);
      if (same_colour(own, s)) continue;
      float tn = vn ? v - float(nb[k].y) : u - float(nb[k].x);
      float ln = edge_line(en, qx, qy, (vn ? u : v) - 0.5);
      float wgt = clamp(0.5 + (gn ? tn - ln : ln - tn) / bw, 0.0, 1.0);
      if (wgt > best) { best = wgt; c = mix(unpack(s), unpack(own), wgt); }
    }
    return c;
  }
  uint e = w >> 24;
  bool vert = (e & 32u) != 0u, neg = (e & 64u) != 0u;
  float t = vert ? v : u;
  float line = edge_line(e, sx, sy, (vert ? u : v) - 0.5);
  bool inside = neg ? (t >= line) : (t < line);
  int px = vert ? sx : (neg ? sx - 1 : sx + 1), py = vert ? (neg ? sy - 1 : sy + 1) : sy;
  uint own = 0xFF000000u | (w & 0xFFFFFFu);
  if (px < 0 || px > 255 || py < 0 || py > 191) return unpack(own);
  uint o = frame_at(px, py);
  if (same_colour(own, o)) return unpack(s);
  { uint n = edge_at(px, py) >> 24; if ((n & 128u) != 0u && ((n ^ e) & 96u) == 64u) return unpack(s); }
  if (bw <= 0.0) return unpack(inside ? own : o);
  return mix(unpack(o), unpack(own), clamp(0.5 + (neg ? t - line : line - t) / bw, 0.0, 1.0));
}
void main() {
  float fx = v_uv.x * 256.0, fy = v_uv.y * 192.0;
  int sx = clamp(int(fx), 0, 255), sy = clamp(int(fy), 0, 191);
  uint s = frame_at(sx, sy);
  vec3 col = edge_cut(sx, sy, clamp(fx - float(sx), 0.0, 1.0), clamp(fy - float(sy), 0.0, 1.0), s, u_bw);
  o_col = vec4(col / 255.0, 1.0);
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
uniform sampler2D u_base, u_layer8, u_cur, u_gnat;
uniform int u_ghost;   // 1: drawing a screen from an earlier frame's data, where today's picture still matches it
uniform usampler2D u_layer, u_desc, u_oper, u_line;
uniform int u_scale, u_screen, u_mode, u_layer_kind;   // u_layer_kind 1: the layer is an RGBA8 buffer holding its words
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
  if (u_ghost == 1) {
    // A display capture keeps 5 bits a channel: compare at that precision.
    uvec3 now = uvec3(texelFetch(u_cur, px, 0).rgb * 255.0 + 0.5) >> 3;
    uvec3 then = uvec3(texelFetch(u_gnat, px, 0).rgb * 255.0 + 0.5) >> 3;
    if (any(notEqual(now, then))) { o_col = base_colour(); return; }
  }
  uint meta = texelFetch(u_line, ivec2(px.y, 0), 0).r;
  if ((meta & 1u) == 0u || int((meta >> 1) & 1u) != u_screen) { o_col = base_colour(); return; }
  uint d = texelFetch(u_desc, px, 0).r;
  uint mode = d & 7u;
  if (mode == 0u) { o_col = base_colour(); return; }
  ivec2 hp = clamp(ivec2(v_uv * SRC * float(u_scale)), ivec2(0), ivec2(256 * u_scale - 1, 192 * u_scale - 1));
  // The 3D layer's horizontal scroll (BG0HOFS): this column shows the
  // raster's column + scroll, mod 512, and nothing where that is past 255.
  int sx = hp.x + int((meta >> 2) & 0x1FFu) * u_scale;
  int span = 512 * u_scale;
  sx -= (sx / span) * span;
  uint h = 0u;
  if (sx < 256 * u_scale) {
    ivec2 sp = ivec2(sx, hp.y);
    if (u_layer_kind == 1) { uvec4 b = uvec4(round(texelFetch(u_layer8, sp, 0) * 255.0)); h = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24); }
    else h = texelFetch(u_layer, sp, 0).r;
  }
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
  hi_front_.release_buffer();
  hi_back_.release_buffer();
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

void Presenter::submit(const uint32_t* top, const uint32_t* bottom, HiresFrame* hires,
                       const uint32_t* const* edges, const uint64_t* const* edge_rows) {
  std::lock_guard<std::mutex> lock(mu_);
  std::memcpy(back_.data(), top, W * H * 4);
  std::memcpy(back_.data() + W * H, bottom, W * H * 4);
  for (int sc = 0; sc < 2; ++sc) {
    const bool on = edges && edges[sc] && edge_rows && edge_rows[sc] && (edge_rows[sc][0] | edge_rows[sc][1] | edge_rows[sc][2]);
    edge_back_on_[sc] = on;
    if (!on) continue;
    if (edge_back_.size() != static_cast<size_t>(2 * W * H)) edge_back_.assign(2 * W * H, 0);
    // Only the marked rows hold this frame's edges; the others may be stale in the core's plane.
    uint32_t* dst = edge_back_.data() + sc * W * H;
    for (int y = 0; y < H; ++y) {
      if ((edge_rows[sc][y >> 6] >> (y & 63)) & 1) std::memcpy(dst + y * W, edges[sc] + y * W, W * 4);
      else std::memset(dst + y * W, 0, W * 4);
    }
  }
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
    if (edge_prog_) glDeleteProgram(edge_prog_);
    if (edge_tex_[0]) glDeleteTextures(2, edge_tex_);
    if (hi_tex_[0]) glDeleteTextures(4, hi_tex_);
  }
  edge_prog_ = 0;
  edge_tex_[0] = edge_tex_[1] = 0;
  edge_uploaded_ = false;
  hi_prog_ = 0;
  hi_tex_[0] = hi_tex_[1] = hi_tex_[2] = hi_tex_[3] = 0;
  hi_layer_scale_ = 0;
  hi_uploaded_ = false;
  drop_imports(true);
  hi_layer8_tex_ = 0;
  for (Ghost& g : ghost_) {
    if (any != EGL_NO_SURFACE) { GLuint t[5] = {g.desc, g.oper, g.line, g.native, g.layer}; glDeleteTextures(5, t); }
    g = Ghost{};
  }
  if (ghost_fbo_[0] && any != EGL_NO_SURFACE) glDeleteFramebuffers(2, ghost_fbo_);
  ghost_fbo_[0] = ghost_fbo_[1] = 0;
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

bool Presenter::init_edges() {
  if (edge_prog_) return true;
  if (edge_failed_) return false;
  GLuint v = compile(GL_VERTEX_SHADER, kVertex), f = compile(GL_FRAGMENT_SHADER, kEdgeFragment);
  if (!v || !f) { edge_failed_ = true; LOGW("edge cut: shader rejected; drawing without it"); return false; }
  edge_prog_ = glCreateProgram();
  glAttachShader(edge_prog_, v);
  glAttachShader(edge_prog_, f);
  glLinkProgram(edge_prog_);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(edge_prog_, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(edge_prog_); edge_prog_ = 0; edge_failed_ = true; LOGW("edge cut: link failed"); return false; }
  edge_u_tex_ = glGetUniformLocation(edge_prog_, "u_tex");
  edge_u_edge_ = glGetUniformLocation(edge_prog_, "u_edge");
  edge_u_bw_ = glGetUniformLocation(edge_prog_, "u_bw");
  glGenTextures(2, edge_tex_);
  for (GLuint t : edge_tex_) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, W, H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
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
  hi_u_layer8_ = glGetUniformLocation(hi_prog_, "u_layer8");
  hi_u_kind_ = glGetUniformLocation(hi_prog_, "u_layer_kind");
  hi_u_ghost_ = glGetUniformLocation(hi_prog_, "u_ghost");
  hi_u_cur_ = glGetUniformLocation(hi_prog_, "u_cur");
  hi_u_gnat_ = glGetUniformLocation(hi_prog_, "u_gnat");
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
    if (!all && (im.buffer == hi_front_.buffer || im.buffer == hi_back_.buffer)) { ++i; continue; }
    if (context_ != EGL_NO_CONTEXT && eglGetCurrentContext() == context_) glDeleteTextures(1, &im.tex);
    if (destroy_image) destroy_image(display_, static_cast<EGLImageKHR>(im.image));
    AHardwareBuffer_release(im.buffer);
    imported_.erase(imported_.begin() + static_cast<long>(i));
  }
}

void Presenter::upload_hires() {
  const HiresFrame& h = hi_front_;
  hi_layer8_tex_ = 0;
  if (h.buffer) {
    // A raster rebuild (new scale) brings new buffers: let the old ones go.
    if (imported_.size() > 8) drop_imports(false);
    hi_layer8_tex_ = import_buffer(h.buffer);
  }
  if (!h.buffer && h.scale != hi_layer_scale_) {
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
  if (!h.buffer) {
    glBindTexture(GL_TEXTURE_2D, hi_tex_[3]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W * h.scale, H * h.scale, GL_RED_INTEGER, GL_UNSIGNED_INT, h.layer.data());
  }
  hi_uploaded_ = true;
}

void Presenter::store_ghost(int screen) {
  Ghost& g = ghost_[screen];
  while (glGetError() != GL_NO_ERROR) {}   // only this copy's errors decide its validity
  const HiresFrame& h = hi_front_;
  auto make = [](GLuint& t, GLenum fmt, GLsizei w, GLsizei hh, bool swizzle) {
    if (t) return;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, hh);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (swizzle) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
  };
  make(g.desc, GL_R32UI, W, H, false);
  make(g.oper, GL_R32UI, W, H, false);
  make(g.line, GL_R32UI, H, 1, false);
  make(g.native, GL_RGBA8, W, H, true);
  const int kind = h.buffer ? 1 : 0;
  if (g.layer && (g.layer_scale != h.scale || g.layer_kind != kind)) { glDeleteTextures(1, &g.layer); g.layer = 0; }
  make(g.layer, kind ? GL_RGBA8 : GL_R32UI, W * h.scale, H * h.scale, false);
  g.layer_scale = h.scale; g.layer_kind = kind; g.scale = h.scale;
  glBindTexture(GL_TEXTURE_2D, g.desc);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.desc.data());
  glBindTexture(GL_TEXTURE_2D, g.oper);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, h.operand.data());
  glBindTexture(GL_TEXTURE_2D, g.line);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, H, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, h.line.data());
  glBindTexture(GL_TEXTURE_2D, g.native);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, front_.data() + screen * W * H);
  // The 3D layer: a GPU-side copy, its source being reused by later frames.
  if (!ghost_fbo_[0]) glGenFramebuffers(2, ghost_fbo_);
  const GLuint src = kind ? hi_layer8_tex_ : hi_tex_[3];
  glBindFramebuffer(GL_READ_FRAMEBUFFER, ghost_fbo_[0]);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ghost_fbo_[1]);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.layer, 0);
  const GLint sw = static_cast<GLint>(W * h.scale), sh = static_cast<GLint>(H * h.scale);
  glBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  g.valid = glGetError() == GL_NO_ERROR;
  g.age = 0;
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
      std::swap(hi_front_, hi_back_); hi_back_.valid = false; hi_uploaded_ = false;
      if (edge_back_on_[0] || edge_back_on_[1]) edge_front_.swap(edge_back_);
      for (int sc = 0; sc < 2; ++sc) { edge_on_[sc] = edge_back_on_[sc]; edge_back_on_[sc] = false; }
      edge_uploaded_ = false;
      for (Ghost& g : ghost_) ++g.age;
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
  if (!edge_uploaded_ && (edge_on_[0] || edge_on_[1]) && init_edges()) {
    for (int sc = 0; sc < 2; ++sc) {
      if (!edge_on_[sc]) continue;
      glBindTexture(GL_TEXTURE_2D, edge_tex_[sc]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_INT, edge_front_.data() + sc * W * H);
    }
    edge_uploaded_ = true;
  }
  // Which screen (if any) shows engine A's 3D at the internal resolution.
  int hi_screen = -1;
  const bool have_layer = hi_front_.buffer || hi_front_.layer.size() >= static_cast<size_t>(W * H) * hi_front_.scale * hi_front_.scale;
  if (hi_front_.valid && hi_front_.scale > 1 && have_layer && init_hires()) {
    for (uint32_t l : hi_front_.line) if (l & 1u) { hi_screen = static_cast<int>((l >> 1) & 1u); break; }
    if (hi_screen >= 0 && !hi_uploaded_) {
      upload_hires();
      if (!(hi_front_.buffer && !hi_layer8_tex_)) store_ghost(hi_screen);
    }
    if (hi_front_.buffer && !hi_layer8_tex_) hi_screen = -1;   // the import failed: the 1x picture
  }
  // The second display first: the main one's swap waits for vsync.
  for (int d : {SECOND, MAIN}) {
    if (surface_[d] == EGL_NO_SURFACE) continue;
    if (!eglMakeCurrent(display_, surface_[d], surface_[d], context_)) continue;
    draw_display(d, rect[d], filter, xbrz, hi_screen);
  }
}

void Presenter::draw_display(int d, const ScreenRect* rect, int filter, bool xbrz, int hi_screen) {
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
      glUniform1i(hi_u_layer8_, 5);
      glUniform1i(hi_u_kind_, hi_front_.buffer ? 1 : 0);
      for (int t = 0; t < 4; ++t) { glActiveTexture(GL_TEXTURE1 + t); glBindTexture(GL_TEXTURE_2D, hi_tex_[t == 0 ? 3 : t - 1]); }
      glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, hi_layer8_tex_);
      glUniform1i(hi_u_ghost_, 0);
      glActiveTexture(GL_TEXTURE0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glUseProgram(program_);
      continue;
    }
    if (edge_on_[s] && edge_prog_) {
      glUseProgram(edge_prog_);
      glUniform1i(edge_u_tex_, 0);
      glUniform1i(edge_u_edge_, 1);
      glUniform1f(edge_u_bw_, 1.0f);
      glBindTexture(GL_TEXTURE_2D, tex_[s]);   // texelFetch: the sampler state is not used
      glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, edge_tex_[s]);
      glActiveTexture(GL_TEXTURE0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glUseProgram(program_);
      continue;
    }
    const Ghost& g = ghost_[s];
    if (hi_prog_ && g.valid && g.age <= 2) {
      glUseProgram(hi_prog_);
      glUniform1i(hi_u_base_, 0);
      glUniform1i(hi_u_layer_, 1);
      glUniform1i(hi_u_desc_, 2);
      glUniform1i(hi_u_oper_, 3);
      glUniform1i(hi_u_line_, 4);
      glUniform1i(hi_u_layer8_, 5);
      glUniform1i(hi_u_cur_, 6);
      glUniform1i(hi_u_gnat_, 7);
      glUniform1i(hi_u_scale_, static_cast<GLint>(g.scale));
      glUniform1i(hi_u_screen_, s);
      glUniform1i(hi_u_mode_, xbrz ? LINEAR : filter);
      glUniform1i(hi_u_kind_, g.layer_kind);
      glUniform1i(hi_u_ghost_, 1);
      glUniform2f(hi_u_out_, static_cast<float>(r.w), static_cast<float>(r.h));
      const GLuint units[8] = {0, g.layer_kind ? 0u : g.layer, g.desc, g.oper, g.line, g.layer_kind ? g.layer : 0u, tex_[s], g.native};
      for (int t = 1; t < 8; ++t) { glActiveTexture(GL_TEXTURE0 + t); glBindTexture(GL_TEXTURE_2D, units[t]); }
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
