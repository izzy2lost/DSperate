// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - draws the two DS screens into the game's SurfaceView.
//
// Runs on a thread of its own with vsync, decoupled from the emulator: the
// emulator paces itself and hands over finished frames; the presenter shows
// the newest one at the display's rate (so fast-forward never blocks on
// eglSwapBuffers). Placement is decided by the Kotlin side, which also owns
// the touch mapping, and arrives here as two rectangles in surface pixels.
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

// One frame's high-resolution 3D data (ds::gpu::Gpu::Hires, copied): engine
// A's per-pixel compositing words, its per-line record, and the 3D layer at
// `scale` x. Swapped in and out whole, so no buffer is copied twice.
struct HiresFrame {
  bool valid = false;
  uint32_t scale = 1;
  std::vector<uint32_t> desc, operand, layer;   // 256x192, 256x192, (256*scale)x(192*scale)
  std::vector<uint32_t> line;                   // 192
  // The 3D layer handed over without a copy (layer empty): an RGBA8 buffer
  // holding the same words. A reference the frame's holder owns.
  AHardwareBuffer* buffer = nullptr;
  void release_buffer() { if (buffer) { AHardwareBuffer_release(buffer); buffer = nullptr; } }
};

class Presenter {
public:
  enum Filter { NEAREST = 0, LINEAR = 1, SHARP = 2, LCD = 3, XBRZ = 4 };

  Presenter();
  ~Presenter();

  void start();
  void stop();
  // Adopts `window` (the caller's reference passes to the presenter), or
  // drops the current one for null. Returns once the render thread has done
  // it, so a surfaceDestroyed callback can return safely.
  void set_window(ANativeWindow* window);
  void set_layout(const ScreenRect& top, const ScreenRect& bottom);
  void set_filter(int filter);
  // Emulator thread: both screens, 256x192 0xAARRGGBB each. With `hires`
  // (its contents are swapped with the presenter's previous ones), engine
  // A's screen shows its 3D at the frame's internal resolution.
  void submit(const uint32_t* top, const uint32_t* bottom, HiresFrame* hires = nullptr);
  // The newest frame, top screen above bottom (256x384), for thumbnails.
  void snapshot(std::vector<uint32_t>& out);

private:
  void run();
  bool make_current(ANativeWindow* w);
  void release_surface();
  void release_all();
  bool init_gl();
  bool init_xbrz();
  bool init_hires();
  void upload_hires();
  GLuint import_buffer(AHardwareBuffer* b);   // the GL texture sampling `b` (cached)
  void store_ghost(int screen);
  void drop_imports(bool all);
  void draw();

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_, ack_cv_;
  bool quit_ = false;

  // Guarded by mu_.
  ANativeWindow* pending_window_ = nullptr;
  bool window_change_ = false;
  uint64_t window_gen_ = 0, window_done_ = 0;
  ScreenRect rect_[2];
  int filter_ = NEAREST;
  bool dirty_ = false;                 // something to draw (frame, layout, filter)
  std::vector<uint32_t> back_;         // newest submitted frame
  bool fresh_ = false;                 // back_ holds a frame not yet uploaded
  HiresFrame hi_back_;                 // the high-res data that goes with back_

  // Render thread only.
  ANativeWindow* window_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLConfig config_ = nullptr;
  GLuint program_ = 0, tex_[2] = {0, 0};
  GLint u_tex_ = -1, u_mode_ = -1, u_out_ = -1;
  std::vector<uint32_t> front_;
  bool have_frame_ = false;
  // xBRZ: each screen upscaled 6x into its own texture, redone only for a new frame.
  GLuint xbrz_prog_ = 0, xbrz_vbo_ = 0, xbrz_fbo_[2] = {0, 0}, xbrz_tex_[2] = {0, 0};
  GLint xbrz_u_tex_ = -1, xbrz_u_texsize_ = -1, xbrz_u_drawsize_ = -1;
  bool xbrz_failed_ = false, xbrz_stale_[2] = {true, true};
  // High-res 3D composite: the uploaded frame's data and its program.
  HiresFrame hi_front_;
  GLuint hi_prog_ = 0, hi_tex_[4] = {0, 0, 0, 0};   // desc, operand, line, layer
  uint32_t hi_layer_scale_ = 0;                      // the layer texture's allocated scale
  GLint hi_u_base_ = -1, hi_u_layer_ = -1, hi_u_desc_ = -1, hi_u_oper_ = -1, hi_u_line_ = -1;
  GLint hi_u_scale_ = -1, hi_u_screen_ = -1, hi_u_mode_ = -1, hi_u_out_ = -1;
  bool hi_failed_ = false, hi_uploaded_ = false;
  GLint hi_u_layer8_ = -1, hi_u_kind_ = -1;
  GLuint hi_layer8_tex_ = 0;                         // the frame's buffer, imported (not owned)
  struct Imported { AHardwareBuffer* buffer; void* image; GLuint tex; };   // image: EGLImageKHR
  std::vector<Imported> imported_;
  bool import_failed_ = false;
  // Per screen, the last picture it showed with high-res 3D, kept on the GPU.
  // A later frame that shows the same picture again (a game displaying a
  // display capture of it: dual-screen 3D, motion blur) draws its matching
  // pixels from this instead of at 1x. Expires two frames after it was taken.
  struct Ghost {
    bool valid = false;
    int age = 0;
    uint32_t scale = 1, layer_scale = 0;
    int layer_kind = 0;                       // 0 R32UI, 1 RGBA8 (from a buffer)
    GLuint desc = 0, oper = 0, line = 0, native = 0, layer = 0;
  };
  Ghost ghost_[2];
  GLuint ghost_fbo_[2] = {0, 0};              // read / draw framebuffers for the layer copy
  GLint hi_u_ghost_ = -1, hi_u_cur_ = -1, hi_u_gnat_ = -1;
};

} // namespace ninds
