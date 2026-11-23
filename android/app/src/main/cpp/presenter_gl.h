// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - draws the two DS screens into the game's SurfaceView.
//
// Runs on a thread of its own with vsync, decoupled from the emulator: the
// emulator paces itself and hands over finished frames; the presenter shows
// the newest one at the display's rate (so fast-forward never blocks on
// eglSwapBuffers). Placement is decided by the Kotlin side, which also owns
// the touch mapping, and arrives here as two rectangles in surface pixels per
// display: the device's own (MAIN) and, on a dual-screen device or with an
// external monitor attached, a second one (SECOND), each with its own window.
#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace ninds {

struct ScreenRect { int x = 0, y = 0, w = 0, h = 0; bool shown = false; };

// One frame's high-resolution 3D merge (the core's 3D split, core/gpu/split3d.h),
// per screen: the split's words (ds::gpu::Gpu::Split3DScreen, copied) and the
// GPU raster's image of the 3D layer at its scale, an AHardwareBuffer holding
// the record's words (r, g, b of RGB666, then the 5-bit alpha, a byte each).
// The screen's own framebuffer is the picture without the 3D layer. Swapped
// in and out whole, so no buffer is copied twice.
struct HiresScreen {
  bool on = false;
  uint32_t scale = 1;                 // texels a DS pixel in `buffer` (the frame's own scale)
  bool coords = false;                // `xy` holds where a captured pixel's 3D texel is (split3d XY)
  bool scrolled = false;              // some line has BG0's X offset (bits 20-28 of its `lines` word)
  std::vector<uint32_t> words, xy;    // 256x192 each
  std::vector<uint32_t> lines;        // 192: master brightness m | n << 8, BG0's X offset in bits 20-28
  AHardwareBuffer* buffer = nullptr;  // a reference the frame's holder owns
  void release_buffer() { if (buffer) { AHardwareBuffer_release(buffer); buffer = nullptr; } }
};
struct HiresFrame {
  HiresScreen s[2];
  bool any() const { return s[0].on || s[1].on; }
  void release_buffer() { s[0].release_buffer(); s[1].release_buffer(); }
  void clear() { for (HiresScreen& h : s) { h.release_buffer(); h.on = false; } }
};

class Presenter {
public:
  enum Filter { NEAREST = 0, LINEAR = 1, SHARP = 2, LCD = 3, XBRZ = 4 };
  enum Display { MAIN = 0, SECOND = 1, DISPLAYS = 2 };

  Presenter();
  ~Presenter();

  void start();
  void stop();
  // Adopts `window` for `display` (the caller's reference passes to the
  // presenter), or drops that display's for null. Returns once the render
  // thread has done it, so a surfaceDestroyed callback can return safely.
  void set_window(int display, ANativeWindow* window);
  void set_layout(int display, const ScreenRect& top, const ScreenRect& bottom);
  void set_filter(int filter);
  // Emulator thread: both screens, 256x192 0xAARRGGBB each. With `hires`
  // (its contents are swapped with the presenter's previous ones), a split
  // screen gets its 3D merged in from the raster's image at its scale.
  void submit(const uint32_t* top, const uint32_t* bottom, HiresFrame* hires = nullptr);

private:
  void run();
  bool make_surface(int display);
  void release_surface(int display);
  bool init_programs();
  void draw_display(int display, const ScreenRect* rect, int filter, bool xbrz, const bool* hi_on);
  void release_all();
  bool init_gl();
  bool init_xbrz();
  bool init_hires();
  void upload_hires();
  GLuint import_buffer(AHardwareBuffer* b);   // the GL texture sampling `b` (cached)
  void drop_imports(bool all);
  void draw();

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_, ack_cv_;
  bool quit_ = false;

  // Guarded by mu_.
  ANativeWindow* pending_window_[DISPLAYS] = {};
  bool window_change_[DISPLAYS] = {};
  uint64_t window_gen_ = 0, window_done_ = 0;
  ScreenRect rect_[DISPLAYS][2];       // per display: top, bottom
  int filter_ = NEAREST;
  bool dirty_ = false;                 // something to draw (frame, layout, filter)
  std::vector<uint32_t> back_;         // newest submitted frame
  bool fresh_ = false;                 // back_ holds a frame not yet uploaded
  HiresFrame hi_back_;                 // the high-res data that goes with back_

  // Render thread only.
  // One context shared by both displays' surfaces, so every texture serves both.
  ANativeWindow* window_[DISPLAYS] = {};
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_[DISPLAYS] = {EGL_NO_SURFACE, EGL_NO_SURFACE};
  EGLConfig config_ = nullptr;
  GLuint program_ = 0, tex_[2] = {0, 0};
  GLint u_tex_ = -1, u_mode_ = -1, u_out_ = -1;
  std::vector<uint32_t> front_;
  bool have_frame_ = false;
  // xBRZ: each screen upscaled 6x into its own texture, redone only for a new frame.
  GLuint xbrz_prog_ = 0, xbrz_vbo_ = 0, xbrz_fbo_[2] = {0, 0}, xbrz_tex_[2] = {0, 0};
  GLint xbrz_u_tex_ = -1, xbrz_u_texsize_ = -1, xbrz_u_drawsize_ = -1;
  bool xbrz_failed_ = false, xbrz_stale_[2] = {true, true};
  // The 3D merge: the uploaded frame's data and its program.
  HiresFrame hi_front_;
  GLuint hi_prog_ = 0;
  GLuint hi_words_[2] = {0, 0}, hi_xy_[2] = {0, 0}, hi_lines_[2] = {0, 0};
  GLuint hi_layer_[2] = {0, 0};                      // each screen's buffer, imported (not owned)
  GLint hi_u_base_ = -1, hi_u_frame_ = -1, hi_u_layer_ = -1, hi_u_words_ = -1, hi_u_xy_ = -1, hi_u_lines_ = -1;
  GLint hi_u_scale_ = -1, hi_u_flags_ = -1, hi_u_mode_ = -1, hi_u_out_ = -1;
  bool hi_failed_ = false, hi_uploaded_ = false;
  struct Imported { AHardwareBuffer* buffer; void* image; GLuint tex; };   // image: EGLImageKHR
  std::vector<Imported> imported_;
  bool import_failed_ = false;
};

} // namespace ninds
