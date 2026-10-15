// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - the phone's microphone as the DS's.
//
// AAudio captures mono s16 in a callback into a ring; the emulator thread
// takes one DS frame's worth (547 samples at 32768 Hz) per frame, resampled
// from whatever rate the device gave, cleaned up as the desktop frontend does
// (DC removed, x0.25 gain, a noise gate). Opened only once a game reads the
// microphone, so games that never use it never light the privacy indicator.
#pragma once
#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ninds {

class MicIn {
public:
  MicIn();
  ~MicIn();

  bool open();      // false: no permission, no microphone, or AAudio refused
  void close();
  bool active() const { return stream_ != nullptr; }
  void set_paused(bool paused);   // stops capturing while the game is paused or in the background

  // Emulator thread: one DS frame of processed samples (empty while closed).
  void take_frame(std::vector<int16_t>& out);

private:
  static aaudio_data_callback_result_t on_data(AAudioStream*, void* user, void* audio, int32_t frames);
  static void on_error(AAudioStream*, void* user, aaudio_result_t error);

  std::mutex mutex_;
  AAudioStream* stream_ = nullptr;
  int32_t device_rate_ = 48000;
  bool paused_ = false;

  // SPSC ring of device-rate samples: the callback writes, the emulator thread reads.
  static constexpr size_t RING = 1u << 14;
  std::vector<int16_t> ring_;
  std::atomic<size_t> rd_{0}, wr_{0};

  // Emulator thread only.
  double phase_ = 0.0;
  double dc_ = 0.0;
  double floor_ = 0.0;
  std::vector<int16_t> raw_;
};

} // namespace ninds
