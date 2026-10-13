// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - AAudio output for the DS mixer.
//
// The emulator thread pushes stereo s16 frames at the console's real rate
// (Spu::output_rate_hz, ~32728.5 Hz); AAudio's callback pulls them at the
// device rate through a linear resampler. Pacing comes from the frame
// limiter, not from here, so the two clocks drift: the resampling ratio is
// nudged by up to 0.5% to hold the ring near its target fill.
#pragma once
#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ninds {

class AudioOut {
public:
  AudioOut();
  ~AudioOut();

  bool start(double source_rate_hz);
  void stop();
  // Emulator thread. Drops what does not fit (fast-forward overruns the ring).
  void push(const int16_t* stereo, size_t frames);
  void set_source_rate(double hz) { src_rate_.store(hz, std::memory_order_relaxed); }
  void set_volume(int percent) { volume_.store(percent < 0 ? 0 : percent > 100 ? 100 : percent, std::memory_order_relaxed); }
  void set_paused(bool paused);
  void clear();

private:
  static aaudio_data_callback_result_t on_data(AAudioStream*, void* user, void* audio, int32_t frames);
  static void on_error(AAudioStream*, void* user, aaudio_result_t error);
  bool open_stream();
  void close_stream();
  void fill(int16_t* out, int32_t frames);
  size_t fill_level() const;

  std::mutex stream_mutex_;
  AAudioStream* stream_ = nullptr;
  int32_t device_rate_ = 48000;

  // SPSC ring of stereo frames: the emulator thread writes, the callback reads.
  static constexpr size_t RING_FRAMES = 1u << 13;   // ~250 ms at the DS rate
  std::vector<int16_t> ring_;
  std::atomic<size_t> rd_{0}, wr_{0};

  std::atomic<double> src_rate_{32728.5};
  std::atomic<int> volume_{100};
  std::atomic<bool> paused_{false};
  std::atomic<bool> running_{false};
  double phase_ = 0.0;                       // callback-only resampler state
  int16_t last_[2] = {0, 0}, cur_[2] = {0, 0};
};

} // namespace ninds
