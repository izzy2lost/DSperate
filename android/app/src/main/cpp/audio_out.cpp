// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - AAudio output for the DS mixer.
#include "audio_out.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <thread>

#define LOG_TAG "NinDS-audio"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ninds {

namespace {
// Fill the ring is steered towards: about three DS frames of sound. Enough to
// ride out a slow frame, little enough that sound effects stay on the action.
constexpr size_t TARGET_FRAMES = 1600;
constexpr double MAX_ADJUST = 0.005;
}

AudioOut::AudioOut() : ring_(RING_FRAMES * 2, 0) {}

AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(double source_rate_hz) {
  src_rate_.store(source_rate_hz, std::memory_order_relaxed);
  running_.store(true);
  std::lock_guard<std::mutex> lock(stream_mutex_);
  return open_stream();
}

void AudioOut::stop() {
  running_.store(false);
  std::lock_guard<std::mutex> lock(stream_mutex_);
  close_stream();
}

bool AudioOut::open_stream() {
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
  AAudioStreamBuilder_setDataCallback(b, &AudioOut::on_data, this);
  AAudioStreamBuilder_setErrorCallback(b, &AudioOut::on_error, this);
  const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK || !stream_) {
    LOGW("cannot open the output stream: %s", AAudio_convertResultToText(r));
    stream_ = nullptr;
    return false;
  }
  device_rate_ = AAudioStream_getSampleRate(stream_);
  if (device_rate_ <= 0) device_rate_ = 48000;
  // Two bursts of buffering: low latency without underruns on most devices.
  const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
  if (burst > 0) AAudioStream_setBufferSizeInFrames(stream_, burst * 2);
  if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
    LOGW("cannot start the output stream");
    close_stream();
    return false;
  }
  return true;
}

void AudioOut::close_stream() {
  if (!stream_) return;
  AAudioStream_requestStop(stream_);
  AAudioStream_close(stream_);
  stream_ = nullptr;
}

void AudioOut::on_error(AAudioStream*, void* user, aaudio_result_t error) {
  // The stream died (headphones unplugged, a route change): it cannot be
  // reopened from the callback thread, so reopen from a thread of our own.
  AudioOut* self = static_cast<AudioOut*>(user);
  if (error != AAUDIO_ERROR_DISCONNECTED) return;
  std::thread([self] {
    std::lock_guard<std::mutex> lock(self->stream_mutex_);
    self->close_stream();
    if (self->running_.load()) self->open_stream();
  }).detach();
}

void AudioOut::set_paused(bool paused) {
  paused_.store(paused, std::memory_order_relaxed);
  if (paused) clear();
}

void AudioOut::clear() { rd_.store(wr_.load(std::memory_order_acquire), std::memory_order_release); }

size_t AudioOut::fill_level() const {
  return (wr_.load(std::memory_order_acquire) - rd_.load(std::memory_order_acquire)) & (RING_FRAMES - 1);
}

void AudioOut::push(const int16_t* stereo, size_t frames) {
  size_t w = wr_.load(std::memory_order_relaxed);
  const size_t r = rd_.load(std::memory_order_acquire);
  const size_t space = (RING_FRAMES - 1) - ((w - r) & (RING_FRAMES - 1));
  frames = std::min(frames, space);
  for (size_t i = 0; i < frames; ++i) {
    ring_[w * 2] = stereo[i * 2];
    ring_[w * 2 + 1] = stereo[i * 2 + 1];
    w = (w + 1) & (RING_FRAMES - 1);
  }
  wr_.store(w, std::memory_order_release);
}

aaudio_data_callback_result_t AudioOut::on_data(AAudioStream*, void* user, void* audio, int32_t frames) {
  static_cast<AudioOut*>(user)->fill(static_cast<int16_t*>(audio), frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioOut::fill(int16_t* out, int32_t frames) {
  if (paused_.load(std::memory_order_relaxed)) {
    std::memset(out, 0, static_cast<size_t>(frames) * 4);
    return;
  }
  const size_t level = fill_level();
  const double err = std::clamp((static_cast<double>(level) - TARGET_FRAMES) / TARGET_FRAMES, -1.0, 1.0);
  const double step = src_rate_.load(std::memory_order_relaxed) / device_rate_ * (1.0 + err * MAX_ADJUST);
  const int vol = volume_.load(std::memory_order_relaxed);
  size_t r = rd_.load(std::memory_order_relaxed);
  const size_t w = wr_.load(std::memory_order_acquire);
  for (int32_t i = 0; i < frames; ++i) {
    for (int c = 0; c < 2; ++c) {
      const double s = last_[c] + (cur_[c] - last_[c]) * phase_;
      out[i * 2 + c] = static_cast<int16_t>(static_cast<int>(s) * vol / 100);
    }
    phase_ += step;
    while (phase_ >= 1.0) {
      phase_ -= 1.0;
      last_[0] = cur_[0]; last_[1] = cur_[1];
      if (r != w) {   // underrun: hold the last sample rather than click to zero
        cur_[0] = ring_[r * 2]; cur_[1] = ring_[r * 2 + 1];
        r = (r + 1) & (RING_FRAMES - 1);
      }
    }
  }
  rd_.store(r, std::memory_order_release);
}

} // namespace ninds
