// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RGA nearest-neighbour probe: can the Rockchip RGA (V4L2 mem2mem) do an
// exact integer nearest scale of a 256x192 frame with no CPU pixel work?
// The driver has no filter control and scales with interpolation, but a job
// whose crop and compose match is a plain copy. So every job here is 1:1,
// and the repetition comes from describing the destination differently:
//
//   Rows: a W-wide buffer described as k*W wide and H/k tall puts k physical
//   rows side by side in one "virtual" row. A 1:1 job composed at x = j*W
//   lands on physical rows k*row + j; k jobs repeat every row k times.
//   Columns: the same trick on a 90-degree rotated copy, rotated back.
//
//   pass A (rotate 90):  src 256x192 -> I1, physically 192 x 256k,
//                        described as 192k x 256; k jobs, one per band
//   pass B (rotate 270): I1 as 192 x 256k -> target, described as
//                        k*W x H/k; k jobs, one per band
//
// 2k jobs per view, each 1:1. Two V4L2 contexts (two open()s) keep their
// formats and rotation, so a job only sets its compose rectangle. A third
// context ("sharp") optionally bilinear-scales the integer image to a
// fractional size (e.g. 640x480 at 2.5x from 512x384).
//
// The "-flat" cases run IntScalerFlat: the same result with only the small
// image rotated and the panel-sized writes unrotated, timed per pass, to
// show whether rotated writes are what costs.
//
// The CPU hybrids: "-cpu-rows" has the CPU repeat columns (a 256k x 192
// strip) and the RGA repeat rows with unrotated jobs, no rotation at all;
// "-cpu2-rot" has the CPU do 2x both ways and the RGA the rest by rotation.
// "-cpu-rows" times its CPU side as the two dma-buf syncs and the writes
// (NEON interleaving stores, memcpy at 1x); "baseline" is the 192 KB copy
// the presenter's CPU already makes per screen.
//   DS_RGA_STRIP_HEAP=name   the "-cpu-rows" strip from that dma-heap (listed at start)
//
// Every integer case compares each panel pixel with centre-sampled nearest
// (and the letterbox with its untouched fill). Device-only, not a test;
// --sim runs the same jobs through a software model of the RGA, which
// checks the geometry anywhere.
//
//   rga_nearest_probe [/dev/videoN | --sim] [iters]
//   DS_RGA_DW=1024x768   panel size              DS_RGA_K=n      force the scale
//   DS_RGA_DUMP=prefix   prefix-<case>.ppm        DS_RGA_SHARP=1  run the sharp case even when integer fits
//
// Kernel caveat: on RGA revisions with a zero major or minor version the
// mainline driver (rga-hw.c, rga_cmd_set_trans_info) trims the source of a
// 90/270 job by 8-16 pixels, which makes these rotated copies scale. A
// failing pass A with "neither rotation" points there; dmesg prints the
// revision ("HW Version: 0x..").
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(V4L2_PIX_FMT_XBGR32) && defined(V4L2_CAP_VIDEO_M2M_MPLANE)

namespace {
constexpr uint32_t SW = 256, SH = 192;
constexpr uint32_t kFill = 0x00123456u;   // letterbox sentinel: the RGA must not touch it

struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

bool g_sim = false;

int xioctl(int fd, unsigned long req, void* arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; }
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Rect { int x, y, w, h; };
struct Fmt { uint32_t w, h; };   // XBGR32, stride w*4 (the driver aligns strides to 4 bytes)

// A dma-buf (or, under --sim, plain memory) of XRGB pixels.
struct Buf {
  int fd = -1; uint32_t* px = nullptr; size_t len = 0;
  // `heap`: a /dev/dma_heap name or path to use instead of the defaults.
  bool alloc(size_t bytes, const char* heap_override = nullptr) {
    len = bytes;
    if (g_sim) { px = static_cast<uint32_t*>(std::calloc(bytes, 1)); return px != nullptr; }
    const std::string forced = !heap_override ? "" : heap_override[0] == '/' ? heap_override : std::string("/dev/dma_heap/") + heap_override;
    const char* defaults[] = {"/dev/dma_heap/linux,cma", "/dev/dma_heap/system"};
    const char* only[] = {forced.c_str()};
    for (const char* heap : forced.empty() ? std::vector<const char*>(defaults, defaults + 2) : std::vector<const char*>(only, only + 1)) {
      const int h = open(heap, O_RDWR | O_CLOEXEC);
      if (h < 0) continue;
      dma_heap_allocation_data a{}; a.len = bytes; a.fd_flags = O_RDWR | O_CLOEXEC;
      const int r = ioctl(h, DMA_HEAP_IOCTL_ALLOC, &a);
      close(h);
      if (r == 0) { fd = static_cast<int>(a.fd); break; }
    }
    if (fd < 0) { std::printf("no dma-buf of %zu bytes%s%s\n", bytes, forced.empty() ? "" : " from ", forced.c_str()); return false; }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return false; }
    px = static_cast<uint32_t*>(p);
    return true;
  }
  // CPU access brackets: the CMA heap maps pages cached.
  void cpu(bool begin, bool write) const {
    if (fd < 0) return;
    dma_buf_sync s{}; s.flags = (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    xioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
  }
  ~Buf() {
    if (g_sim) { std::free(px); return; }
    if (px) munmap(px, len);
    if (fd >= 0) close(fd);
  }
};

// One mem2mem context: fixed formats and rotation, full-frame crop, a
// compose rectangle per job. Under --sim, a model of the same copy.
class Rga {
public:
  ~Rga() { if (fd_ >= 0) close(fd_); }
  bool open(const char* dev, Fmt in, Fmt out, int rot) {
    in_ = in; out_ = out; rot_ = rot;
    if (g_sim) return true;
    fd_ = ::open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { std::perror(dev); return false; }
    if (!set_fmt(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, in) || !set_fmt(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, out)) return false;
    // Before streaming, so the driver does not check the default rectangles against it.
    v4l2_control c{}; c.id = V4L2_CID_ROTATE; c.value = rot;
    if (xioctl(fd_, VIDIOC_S_CTRL, &c) < 0) { std::perror("S_CTRL rotate"); return false; }
    for (uint32_t t : {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE}) {
      v4l2_requestbuffers rb{}; rb.type = static_cast<v4l2_buf_type>(t); rb.memory = V4L2_MEMORY_DMABUF; rb.count = 1;
      if (xioctl(fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) { std::perror("REQBUFS"); return false; }
      int tt = static_cast<int>(t);
      if (xioctl(fd_, VIDIOC_STREAMON, &tt) < 0) { std::perror("STREAMON"); return false; }
    }
    return true;
  }
  // The whole source into `r` of the destination, rotated, 1:1.
  bool blit(const Buf& s, const Buf& d, const Rect& r) {
    const bool swap = rot_ == 90 || rot_ == 270;
    if (static_cast<uint32_t>(r.w) != (swap ? in_.h : in_.w) || static_cast<uint32_t>(r.h) != (swap ? in_.w : in_.h)) {
      std::printf("job %dx%d is not 1:1 with the %ux%u source\n", r.w, r.h, in_.w, in_.h); return false;
    }
    if (r.x < 0 || r.y < 0 || static_cast<uint32_t>(r.x + r.w) > out_.w || static_cast<uint32_t>(r.y + r.h) > out_.h) {
      std::printf("job %d,%d %dx%d outside the %ux%u destination\n", r.x, r.y, r.w, r.h, out_.w, out_.h); return false;
    }
    if (g_sim) { sim(s, d, r); return true; }
    v4l2_selection sel{}; sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; sel.target = V4L2_SEL_TGT_COMPOSE;
    sel.r.left = r.x; sel.r.top = r.y; sel.r.width = static_cast<uint32_t>(r.w); sel.r.height = static_cast<uint32_t>(r.h);
    if (xioctl(fd_, VIDIOC_S_SELECTION, &sel) < 0) { std::perror("S_SELECTION compose"); return false; }
    if (!qbuf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s, in_) || !qbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, d, out_)) return false;
    pollfd p{fd_, POLLIN | POLLOUT, 0};
    if (poll(&p, 1, 1000) <= 0) { std::printf("job timed out\n"); return false; }
    return dqbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) && dqbuf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  }
  // Bilinear (or whatever the driver picks) whole source into `r`: the sharp case's last pass.
  bool scale(const Buf& s, const Buf& d, const Rect& r) {
    if (g_sim) { sim_scale(s, d, r); return true; }
    v4l2_selection sel{}; sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; sel.target = V4L2_SEL_TGT_COMPOSE;
    sel.r.left = r.x; sel.r.top = r.y; sel.r.width = static_cast<uint32_t>(r.w); sel.r.height = static_cast<uint32_t>(r.h);
    if (xioctl(fd_, VIDIOC_S_SELECTION, &sel) < 0) { std::perror("S_SELECTION compose"); return false; }
    if (!qbuf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s, in_) || !qbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, d, out_)) return false;
    pollfd p{fd_, POLLIN | POLLOUT, 0};
    if (poll(&p, 1, 1000) <= 0) { std::printf("job timed out\n"); return false; }
    return dqbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) && dqbuf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  }

private:
  bool set_fmt(uint32_t type, Fmt f) {
    v4l2_format v{}; v.type = static_cast<v4l2_buf_type>(type);
    v.fmt.pix_mp.width = f.w; v.fmt.pix_mp.height = f.h; v.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_XBGR32; v.fmt.pix_mp.field = V4L2_FIELD_NONE; v.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd_, VIDIOC_S_FMT, &v) < 0) { std::perror("S_FMT"); return false; }
    const auto& pm = v.fmt.pix_mp;
    if (pm.width != f.w || pm.height != f.h || pm.pixelformat != V4L2_PIX_FMT_XBGR32 || pm.plane_fmt[0].bytesperline != f.w * 4) {
      std::printf("%s: asked %ux%u XB24, got %ux%u %.4s stride %u: the stride trick needs the exact shape\n",
                  type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "output" : "capture", f.w, f.h, pm.width, pm.height,
                  reinterpret_cast<const char*>(&pm.pixelformat), pm.plane_fmt[0].bytesperline);
      return false;
    }
    return true;
  }
  bool qbuf(uint32_t type, const Buf& b, Fmt f) {
    const uint32_t bytes = f.w * f.h * 4;
    if (bytes > b.len) { std::printf("buffer of %zu bytes is smaller than the %ux%u format\n", b.len, f.w, f.h); return false; }
    v4l2_plane pl{}; pl.m.fd = b.fd; pl.length = static_cast<uint32_t>(b.len); if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) pl.bytesused = bytes;
    v4l2_buffer v{}; v.type = static_cast<v4l2_buf_type>(type); v.memory = V4L2_MEMORY_DMABUF; v.index = 0; v.m.planes = &pl; v.length = 1;
    if (xioctl(fd_, VIDIOC_QBUF, &v) < 0) { std::perror("QBUF"); return false; }
    return true;
  }
  bool dqbuf(uint32_t type) {
    v4l2_plane pl{};
    v4l2_buffer v{}; v.type = static_cast<v4l2_buf_type>(type); v.memory = V4L2_MEMORY_DMABUF; v.m.planes = &pl; v.length = 1;
    if (xioctl(fd_, VIDIOC_DQBUF, &v) < 0) { std::perror("DQBUF"); return false; }
    return true;
  }
  // The model: V4L2 ROTATE is clockwise. (Which way the driver turns does
  // not matter to the trick, only that 90 and 270 undo each other.)
  void sim(const Buf& s, const Buf& d, const Rect& r) const {
    for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x) {
      uint32_t sx = static_cast<uint32_t>(x), sy = static_cast<uint32_t>(y);
      if (rot_ == 90) { sx = static_cast<uint32_t>(y); sy = in_.h - 1 - static_cast<uint32_t>(x); }
      else if (rot_ == 270) { sx = in_.w - 1 - static_cast<uint32_t>(y); sy = static_cast<uint32_t>(x); }
      else if (rot_ == 180) { sx = in_.w - 1 - sx; sy = in_.h - 1 - sy; }
      d.px[static_cast<size_t>(r.y + y) * out_.w + static_cast<size_t>(r.x + x)] = s.px[static_cast<size_t>(sy) * in_.w + sx];
    }
  }
  void sim_scale(const Buf& s, const Buf& d, const Rect& r) const {   // nearest stands in for the hardware's filter
    for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x)
      d.px[static_cast<size_t>(r.y + y) * out_.w + static_cast<size_t>(r.x + x)] =
        s.px[static_cast<size_t>((2 * y + 1) * in_.h / (2 * r.h)) * in_.w + (2 * x + 1) * in_.w / (2 * r.w)];
  }
  int fd_ = -1, rot_ = 0;
  Fmt in_{}, out_{};
};

void diagnose_pass_a(const Buf& src, const Buf& i1, uint32_t k);

// The integer scaler: two contexts and the sh x sw*k intermediate, for a
// target buffer `tw` x `th` (stride tw*4). Views at any x0, y0 that fit.
// The source is the 256x192 frame, or the CPU-2x variant's 512x384.
struct IntScaler {
  uint32_t k = 1, tw = 0, th = 0, sw = SW, sh = SH;
  Rga a, b;
  Buf i1;
  bool open(const char* dev, uint32_t k_, uint32_t tw_, uint32_t th_, uint32_t sw_ = SW, uint32_t sh_ = SH) {
    k = k_; tw = tw_; th = th_; sw = sw_; sh = sh_;
    // Driver limits: widths/heights 34..8192, scale factor checks against the default rectangles.
    if (k * tw > 8192 || sw * k > 8192 || th / k < 34) { std::printf("k=%u on %ux%u exceeds the driver's 8192 / 34 limits\n", k, tw, th); return false; }
    if (!i1.alloc(static_cast<size_t>(sh) * sw * k * 4)) return false;
    return a.open(dev, Fmt{sw, sh}, Fmt{sh * k, sw}, 90) && b.open(dev, Fmt{sh, sw * k}, Fmt{k * tw, th / k}, 270);
  }
  static constexpr int kPasses = 2;
  static constexpr const char* kPassNames[kPasses] = {"A rot90, k jobs (192 x 256k)", "B rot270, k jobs (panel)"};
  double pass_mib(int j) const { return static_cast<double>(sw) * k * sh * 4 * (j == 0 ? 1 : k) / 1048576.0; }
  int jobs() const { return static_cast<int>(2 * k); }
  void diagnose(const Buf& src) const { if (sw == SW) diagnose_pass_a(src, i1, k); }
  // pass_ms, if given: the time of each pass.
  bool run(const Buf& src, const Buf& target, int x0, int y0, double* pass_ms = nullptr) {
    double t0 = now_ms();
    // Pass A: band j of the virtual sh*k x sw image is physical rows k*r + j.
    for (uint32_t j = 0; j < k; ++j)
      if (!a.blit(src, i1, Rect{static_cast<int>(sh * j), 0, static_cast<int>(sh), static_cast<int>(sw)})) return false;
    if (pass_ms) { const double t1 = now_ms(); pass_ms[0] = t1 - t0; t0 = t1; }
    // Pass B: physical row y0 + k*s + j is virtual row (y0 + j)/k + s, band (y0 + j) % k.
    for (uint32_t j = 0; j < k; ++j) {
      const uint32_t t = static_cast<uint32_t>(y0) + j;
      const Rect r{static_cast<int>((t % k) * tw) + x0, static_cast<int>(t / k), static_cast<int>(sw * k), static_cast<int>(sh)};
      if (!b.blit(i1, target, r)) return false;
    }
    if (pass_ms) pass_ms[1] = now_ms() - t0;
    return true;
  }
};

// The same result with the panel-sized writes unrotated: rotation only
// touches the column-repeated 256k x 192 image (twice), and the row repeat
// into the panel is a plain 1:1 copy. Worth it if rotated writes are the
// slow part. Its last pass reads a 256k-wide source (1024 px at 4x).
//
//   pass A (rotate 90):  src -> I1 (192 x 256k, columns repeated), k jobs
//   pass B (rotate 270): I1 -> I2 (256k x 192), one job
//   pass C (no rotation): I2 -> target described as k*W x H/k, k jobs
struct IntScalerFlat {
  uint32_t k = 1, tw = 0, th = 0;
  Rga a, b, c;
  Buf i1, i2;
  bool open(const char* dev, uint32_t k_, uint32_t tw_, uint32_t th_) {
    k = k_; tw = tw_; th = th_;
    if (k * tw > 8192 || SW * k > 8192 || th / k < 34) { std::printf("k=%u on %ux%u exceeds the driver's 8192 / 34 limits\n", k, tw, th); return false; }
    const size_t bytes = static_cast<size_t>(SH) * SW * k * 4;
    if (!i1.alloc(bytes) || !i2.alloc(bytes)) return false;
    return a.open(dev, Fmt{SW, SH}, Fmt{SH * k, SW}, 90) && b.open(dev, Fmt{SH, SW * k}, Fmt{SW * k, SH}, 270) &&
           c.open(dev, Fmt{SW * k, SH}, Fmt{k * tw, th / k}, 0);
  }
  static constexpr int kPasses = 3;
  static constexpr const char* kPassNames[kPasses] = {"A rot90, k jobs (192 x 256k)", "B rot270, 1 job (256k x 192)", "C unrotated, k jobs (panel)"};
  double pass_mib(int j) const { return static_cast<double>(SW) * k * SH * 4 * (j == 2 ? k : 1) / 1048576.0; }
  int jobs() const { return static_cast<int>(2 * k + 1); }
  void diagnose(const Buf& src) const { diagnose_pass_a(src, i1, k); }
  bool run(const Buf& src, const Buf& target, int x0, int y0, double* pass_ms = nullptr) {
    double t0 = now_ms();
    for (uint32_t j = 0; j < k; ++j)
      if (!a.blit(src, i1, Rect{static_cast<int>(SH * j), 0, static_cast<int>(SH), static_cast<int>(SW)})) return false;
    if (pass_ms) { const double t1 = now_ms(); pass_ms[0] = t1 - t0; t0 = t1; }
    if (!b.blit(i1, i2, Rect{0, 0, static_cast<int>(SW * k), static_cast<int>(SH)})) return false;
    if (pass_ms) { const double t1 = now_ms(); pass_ms[1] = t1 - t0; t0 = t1; }
    for (uint32_t j = 0; j < k; ++j) {
      const uint32_t t = static_cast<uint32_t>(y0) + j;
      const Rect r{static_cast<int>((t % k) * tw) + x0, static_cast<int>(t / k), static_cast<int>(SW * k), static_cast<int>(SH)};
      if (!c.blit(i2, target, r)) return false;
    }
    if (pass_ms) pass_ms[2] = now_ms() - t0;
    return true;
  }
};

// CPU pixel repeats for the hybrids: each of `n` source pixels k times in
// a row. With no row padding a whole frame is one call. NEON: one
// interleaving store of k copies of the same vector writes 4 pixels k times
// each (vst2q/vst3q/vst4q); two of them per loop for 8 pixels.
void repeat_px(const uint32_t* s, uint32_t* d, uint32_t k, size_t n) {
  if (k == 1) { std::memcpy(d, s, n * 4); return; }
  size_t x = 0;
#if defined(__ARM_NEON)
  if (k == 2)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst2q_u32(d + 2 * x, (uint32x4x2_t{{a, a}})); vst2q_u32(d + 2 * x + 8, (uint32x4x2_t{{b, b}}));
    }
  else if (k == 3)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst3q_u32(d + 3 * x, (uint32x4x3_t{{a, a, a}})); vst3q_u32(d + 3 * x + 12, (uint32x4x3_t{{b, b, b}}));
    }
  else if (k == 4)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst4q_u32(d + 4 * x, (uint32x4x4_t{{a, a, a, a}})); vst4q_u32(d + 4 * x + 16, (uint32x4x4_t{{b, b, b, b}}));
    }
#endif
  for (; x < n; ++x) for (uint32_t i = 0; i < k; ++i) d[x * k + i] = s[x];
}

// CPU repeats columns (256k x 192, rows not repeated); the RGA repeats rows
// with k unrotated 1:1 jobs into the panel described as k*W x H/k. No rotation.
// The CPU side is timed in three: the begin-access sync, the writes, and
// the end-access sync (the cache clean of the strip). DS_RGA_STRIP_HEAP
// puts the strip in another dma-heap (an uncached one needs no clean).
struct IntScalerCpuRows {
  uint32_t k = 1, tw = 0, th = 0;
  Rga c;
  Buf h;
  bool open(const char* dev, uint32_t k_, uint32_t tw_, uint32_t th_) {
    k = k_; tw = tw_; th = th_;
    if (k * tw > 8192 || SW * k > 8192 || th / k < 34) { std::printf("k=%u on %ux%u exceeds the driver's 8192 / 34 limits\n", k, tw, th); return false; }
    return h.alloc(static_cast<size_t>(SW) * k * SH * 4, std::getenv("DS_RGA_STRIP_HEAP")) && c.open(dev, Fmt{SW * k, SH}, Fmt{k * tw, th / k}, 0);
  }
  static constexpr int kPasses = 4;
  static constexpr const char* kPassNames[kPasses] = {"CPU sync start", "CPU columns xk (256k x 192)", "CPU sync end (cache clean)", "RGA unrotated, k jobs (panel)"};
  double pass_mib(int j) const { return j == 0 || j == 2 ? 0.0 : static_cast<double>(SW) * k * SH * 4 * (j == 1 ? 1 : k) / 1048576.0; }
  int jobs() const { return static_cast<int>(k); }
  void diagnose(const Buf&) const {}
  bool run(const Buf& src, const Buf& target, int x0, int y0, double* pass_ms = nullptr) {
    double t0 = now_ms();
    auto lap = [&](int i) { if (pass_ms) { const double t1 = now_ms(); pass_ms[i] = t1 - t0; t0 = t1; } };
    h.cpu(true, true);
    lap(0);
    repeat_px(src.px, h.px, k, static_cast<size_t>(SW) * SH);
    lap(1);
    h.cpu(false, true);
    lap(2);
    for (uint32_t j = 0; j < k; ++j) {
      const uint32_t t = static_cast<uint32_t>(y0) + j;
      const Rect r{static_cast<int>((t % k) * tw) + x0, static_cast<int>(t / k), static_cast<int>(SW * k), static_cast<int>(SH)};
      if (!c.blit(h, target, r)) return false;
    }
    lap(3);
    return true;
  }
};

// CPU 2x both ways (512x384); the RGA does the remaining k/2 with the
// rotation trick on that larger image. Even k only.
struct IntScalerCpu2Rot {
  uint32_t k = 2;
  IntScaler rga;
  Buf p;
  bool open(const char* dev, uint32_t k_, uint32_t tw, uint32_t th) {
    k = k_;
    if (k % 2) { std::printf("needs an even scale\n"); return false; }
    return p.alloc(static_cast<size_t>(SW) * 2 * SH * 2 * 4) && rga.open(dev, k / 2, tw, th, SW * 2, SH * 2);
  }
  static constexpr int kPasses = 3;
  static constexpr const char* kPassNames[kPasses] = {"CPU 2x2 (512x384)", "A rot90, k/2 jobs (384 x 512k/2)", "B rot270, k/2 jobs (panel)"};
  double pass_mib(int j) const { return j == 0 ? SW * 2.0 * SH * 2 * 4 / 1048576.0 : rga.pass_mib(j - 1); }
  int jobs() const { return rga.jobs(); }
  void diagnose(const Buf&) const {}
  bool run(const Buf& src, const Buf& target, int x0, int y0, double* pass_ms = nullptr) {
    const double t0 = now_ms();
    p.cpu(true, true);
    for (uint32_t y = 0; y < SH; ++y) {
      uint32_t* row = p.px + static_cast<size_t>(y) * 2 * SW * 2;
      repeat_px(src.px + static_cast<size_t>(y) * SW, row, 2, SW);
      std::memcpy(row + SW * 2, row, SW * 2 * 4);
    }
    p.cpu(false, true);
    if (pass_ms) pass_ms[0] = now_ms() - t0;
    return rga.run(p, target, x0, y0, pass_ms ? pass_ms + 1 : nullptr);
  }
};

void fill_source(const Buf& src) {
  uint32_t rng = 0xA1B2C3D4u;
  src.cpu(true, true);
  for (uint32_t i = 0; i < SW * SH; ++i) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; src.px[i] = 0xFF000000u | (rng & 0xFFFFFFu); }
  src.cpu(false, true);
}
void fill(const Buf& b, size_t n, uint32_t v) { b.cpu(true, true); std::fill(b.px, b.px + n, v); b.cpu(false, true); }

// Every pixel of the target: the view by centre-sampled nearest at k, the rest untouched.
size_t check(const Buf& src, const Buf& t, uint32_t tw, uint32_t th, uint32_t k, int x0, int y0) {
  size_t bad = 0;
  src.cpu(true, false); t.cpu(true, false);
  for (uint32_t y = 0; y < th; ++y) for (uint32_t x = 0; x < tw; ++x) {
    const int vx = static_cast<int>(x) - x0, vy = static_cast<int>(y) - y0;
    const bool in = vx >= 0 && vy >= 0 && vx < static_cast<int>(SW * k) && vy < static_cast<int>(SH * k);
    const uint32_t want = in ? src.px[static_cast<size_t>(vy) / k * SW + static_cast<size_t>(vx) / k] & 0xFFFFFFu : kFill & 0xFFFFFFu;
    const uint32_t got = t.px[static_cast<size_t>(y) * tw + x] & 0xFFFFFFu;
    if (got != want && bad++ < 8) std::printf("  mismatch at %u,%u (%s): got %06x want %06x\n", x, y, in ? "view" : "letterbox", got, want);
  }
  src.cpu(false, false); t.cpu(false, false);
  return bad;
}

// After a failure: is I1 an exact rotated, row-repeated copy, and which way?
void diagnose_pass_a(const Buf& src, const Buf& i1, uint32_t k) {
  size_t cw = 0, ccw = 0;
  src.cpu(true, false); i1.cpu(true, false);
  for (uint32_t r = 0; r < SW * k; ++r) for (uint32_t c = 0; c < SH; ++c) {
    const uint32_t got = i1.px[static_cast<size_t>(r) * SH + c] & 0xFFFFFFu, col = r / k;
    cw  += got != (src.px[static_cast<size_t>(SH - 1 - c) * SW + col] & 0xFFFFFFu);
    ccw += got != (src.px[static_cast<size_t>(c) * SW + (SW - 1 - col)] & 0xFFFFFFu);
  }
  src.cpu(false, false); i1.cpu(false, false);
  std::printf("  pass A intermediate: %zu mismatches as clockwise, %zu as counter-clockwise (%s)\n", cw, ccw,
              !cw || !ccw ? "pass A is exact; look at pass B" : "neither rotation: the rotated 1:1 copy is not exact (see the kernel caveat above)");
}

void dump(const char* prefix, const char* name, const Buf& b, uint32_t w, uint32_t h) {
  if (!prefix) return;
  const std::string path = std::string(prefix) + "-" + name + ".ppm";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::perror(path.c_str()); return; }
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  b.cpu(true, false);
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) { const uint32_t p = b.px[i]; const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p)}; std::fwrite(rgb, 1, 3, f); }
  b.cpu(false, false);
  std::fclose(f);
  std::printf("  wrote %s\n", path.c_str());
}

void report(const char* what, std::vector<double>& ms, int jobs) {
  if (ms.empty()) return;
  std::sort(ms.begin(), ms.end());
  std::printf("  %s: median %.3f ms, p90 %.3f, max %.3f (%d jobs per view, %zu runs)\n", what, ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back(), jobs, ms.size());
}
// One pass's median, and the rate of its writes.
void report_pass(const char* what, std::vector<double>& ms, double mib) {
  if (ms.empty()) return;
  std::sort(ms.begin(), ms.end());
  const double med = ms[ms.size() / 2];
  if (mib <= 0) { std::printf("    %-32s median %.3f ms\n", what, med); return; }
  std::printf("    %-32s median %.3f ms, writes %.2f MiB (%.0f MiB/s)\n", what, med, mib, med > 0 ? mib * 1000.0 / med : 0.0);
}

// One integer case into a panel-sized target: correctness, then timing (in total and per pass).
template <class S>
bool int_case(const char* dev, const char* name, const Buf& src, uint32_t pw, uint32_t ph, uint32_t k, int x0, int y0, int iters) {
  std::printf("%s: %ux at %d,%d (%ux%u) on %ux%u\n", name, k, x0, y0, SW * k, SH * k, pw, ph);
  S s;
  Buf panel;
  if (!panel.alloc(static_cast<size_t>(pw) * ph * 4) || !s.open(dev, k, pw, ph)) { std::printf("  FAIL: setup\n"); return false; }
  fill(panel, static_cast<size_t>(pw) * ph, kFill);
  if (!s.run(src, panel, x0, y0)) { std::printf("  FAIL: job\n"); return false; }
  const size_t bad = check(src, panel, pw, ph, k, x0, y0);
  dump(std::getenv("DS_RGA_DUMP"), name, panel, pw, ph);
  if (bad) { std::printf("  FAIL: %zu of %u pixels differ\n", bad, pw * ph); s.diagnose(src); return false; }
  std::printf("  exact: every pixel matches nearest, letterbox untouched\n");
  std::vector<double> ms, pass[S::kPasses];
  for (int i = 0; i < iters; ++i) {
    double p[S::kPasses];
    const double t0 = now_ms();
    if (!s.run(src, panel, x0, y0, p)) return false;
    ms.push_back(now_ms() - t0);
    for (int j = 0; j < S::kPasses; ++j) pass[j].push_back(p[j]);
  }
  report("integer nearest", ms, s.jobs());
  for (int j = 0; j < S::kPasses; ++j) report_pass(S::kPassNames[j], pass[j], s.pass_mib(j));
  return true;
}

// Integer nearest into a 256k x 192k intermediate, then one scaling job to the fitted rectangle.
bool sharp_case(const char* dev, const Buf& src, uint32_t pw, uint32_t ph, uint32_t k, int iters) {
  const uint32_t fw = std::min(pw, ph * SW / SH), fh = fw * SH / SW;
  const Rect fit{static_cast<int>(pw - fw) / 2, static_cast<int>(ph - fh) / 2, static_cast<int>(fw), static_cast<int>(fh)};
  std::printf("sharp: %ux nearest (%ux%u) then one scaling job to %ux%u at %d,%d on %ux%u\n", k, SW * k, SH * k, fw, fh, fit.x, fit.y, pw, ph);
  IntScaler s;
  Rga c;
  Buf i2, panel;
  if (!i2.alloc(static_cast<size_t>(SW) * k * SH * k * 4) || !panel.alloc(static_cast<size_t>(pw) * ph * 4) ||
      !s.open(dev, k, SW * k, SH * k) || !c.open(dev, Fmt{SW * k, SH * k}, Fmt{pw, ph}, 0)) { std::printf("  FAIL: setup\n"); return false; }
  fill(panel, static_cast<size_t>(pw) * ph, kFill);
  const int jobs = s.jobs();
  if (!s.run(src, i2, 0, 0)) { std::printf("  FAIL: job\n"); return false; }
  const size_t bad = check(src, i2, SW * k, SH * k, k, 0, 0);
  if (bad) { std::printf("  FAIL: the integer stage has %zu bad pixels\n", bad); s.diagnose(src); return false; }
  if (!c.scale(i2, panel, fit)) { std::printf("  FAIL: scaling job\n"); return false; }
  std::printf("  integer stage exact; the final scale is the hardware's filter (inspect the dump)\n");
  dump(std::getenv("DS_RGA_DUMP"), "sharp", panel, pw, ph);
  std::vector<double> ms;
  for (int i = 0; i < iters; ++i) {
    const double t0 = now_ms();
    if (!s.run(src, i2, 0, 0) || !c.scale(i2, panel, fit)) return false;
    ms.push_back(now_ms() - t0);
  }
  report("sharp bilinear", ms, jobs + 1);
  return true;
}

// The first rockchip-rga mem2mem node, as the presenter finds it.
std::string find_rga() {
  for (int i = 0; i < 16; ++i) {
    char path[32]; std::snprintf(path, sizeof path, "/dev/video%d", i);
    const int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    v4l2_capability cap{};
    const bool rga = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && std::strcmp(reinterpret_cast<const char*>(cap.driver), "rockchip-rga") == 0;
    close(fd);
    if (rga) return path;
  }
  return {};
}
}

int main(int argc, char** argv) {
  std::string dev = argc > 1 ? argv[1] : "";
  const int iters = argc > 2 ? std::atoi(argv[2]) : 300;
  g_sim = dev == "--sim";
  if (!g_sim && dev.empty()) dev = find_rga();
  if (!g_sim && dev.empty()) { std::printf("no rockchip-rga V4L2 device (pass /dev/videoN, or --sim)\n"); return 1; }
  std::printf("%s\n", g_sim ? "software model (--sim): checks the job geometry, not the hardware" : dev.c_str());
  if (!g_sim) {
    std::printf("dma-heaps:");
    if (DIR* d = opendir("/dev/dma_heap")) { while (const dirent* e = readdir(d)) if (e->d_name[0] != '.') std::printf(" %s", e->d_name); closedir(d); }
    std::printf("%s%s\n", std::getenv("DS_RGA_STRIP_HEAP") ? " (strip from " : "", std::getenv("DS_RGA_STRIP_HEAP") ? std::string(std::getenv("DS_RGA_STRIP_HEAP")).append(")").c_str() : "");
  }

  uint32_t pw = 1024, ph = 768;
  if (const char* s = std::getenv("DS_RGA_DW")) std::sscanf(s, "%ux%u", &pw, &ph);
  uint32_t k = std::max(1u, std::min(pw / SW, ph / SH));
  if (const char* s = std::getenv("DS_RGA_K")) k = std::max(1, std::atoi(s));
  if (SW * k > pw || SH * k > ph) { std::printf("%ux does not fit %ux%u\n", k, pw, ph); return 1; }

  Buf src;
  if (!src.alloc(static_cast<size_t>(SW) * SH * 4)) return 1;
  fill_source(src);

  // What the presenter's CPU does today per screen: the 1x frame into the RGA source.
  {
    Buf stage;
    if (!stage.alloc(static_cast<size_t>(SW) * SH * 4)) return 1;
    std::vector<double> ms;
    for (int i = 0; i < iters; ++i) {
      const double t0 = now_ms();
      stage.cpu(true, true); std::memcpy(stage.px, src.px, static_cast<size_t>(SW) * SH * 4); stage.cpu(false, true);
      ms.push_back(now_ms() - t0);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("baseline: today's 192 KB copy into the RGA source, median %.3f ms\n", ms.empty() ? 0.0 : ms[ms.size() / 2]);
  }

  const int cx = static_cast<int>(pw - SW * k) / 2, cy = static_cast<int>(ph - SH * k) / 2;
  bool ok = int_case<IntScaler>(dev.c_str(), "centred", src, pw, ph, k, cx, cy, iters);
  ok &= int_case<IntScalerFlat>(dev.c_str(), "centred-flat", src, pw, ph, k, cx, cy, iters);
  ok &= int_case<IntScalerCpuRows>(dev.c_str(), "centred-cpu-rows", src, pw, ph, k, cx, cy, iters);
  if (k % 2 == 0) ok &= int_case<IntScalerCpu2Rot>(dev.c_str(), "centred-cpu2-rot", src, pw, ph, k, cx, cy, iters);
  // Odd offsets exercise the band carry in pass B (y0 not a multiple of k).
  if (k >= 2) {
    const uint32_t k2 = k - 1;
    const int x0 = static_cast<int>((pw - SW * k2) / 2) | 1, y0 = static_cast<int>((ph - SH * k2) / 2) | 1;
    if (static_cast<uint32_t>(x0) + SW * k2 <= pw && static_cast<uint32_t>(y0) + SH * k2 <= ph)
    {
      ok &= int_case<IntScaler>(dev.c_str(), "offset", src, pw, ph, k2, x0, y0, iters);
      ok &= int_case<IntScalerFlat>(dev.c_str(), "offset-flat", src, pw, ph, k2, x0, y0, iters);
      ok &= int_case<IntScalerCpuRows>(dev.c_str(), "offset-cpu-rows", src, pw, ph, k2, x0, y0, iters);
    }
  }
  // Fractional fits (640x480 is 2.5x): the integer stage plus one scaling job.
  const uint32_t fw = std::min(pw, ph * SW / SH);
  if (fw != SW * k || std::getenv("DS_RGA_SHARP")) ok &= sharp_case(dev.c_str(), src, pw, ph, k, iters);

  std::printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

#else
#include <cstdio>
int main() { std::fprintf(stderr, "rga_nearest_probe: this V4L2 has no multi-planar dma-buf mem2mem support\n"); return 2; }
#endif
