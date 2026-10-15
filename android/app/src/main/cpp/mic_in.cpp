// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - the phone's microphone as the DS's.
#include "mic_in.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

#define LOG_TAG "NinDS-mic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ninds {

namespace {
constexpr int32_t DS_RATE = 32768;
constexpr size_t PER_FRAME = 547;     // DS_RATE * CYCLES_PER_FRAME / ARM9_CLOCK_HZ
// As the desktop's audio.mic_gain / audio.mic_gate defaults: the DS amplifier
// expects a quieter signal than a phone delivers, and room noise under five
// times the running floor reads as silence (some games treat any hiss as blowing).
constexpr double GAIN = 0.25;
constexpr double GATE = 5.0;
}

MicIn::MicIn() : ring_(RING, 0) {}

MicIn::~MicIn() { close(); }

bool MicIn::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stream_) return true;
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setSampleRate(b, DS_RATE);   // a hint: the device may give its own rate
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_GENERIC);
  AAudioStreamBuilder_setDataCallback(b, &MicIn::on_data, this);
  AAudioStreamBuilder_setErrorCallback(b, &MicIn::on_error, this);
  const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK || !stream_) {
    LOGW("cannot open the microphone: %s", AAudio_convertResultToText(r));
    stream_ = nullptr;
    return false;
  }
  device_rate_ = AAudioStream_getSampleRate(stream_);
  if (device_rate_ <= 0) device_rate_ = 48000;
  rd_.store(0);
  wr_.store(0);
  phase_ = 0.0;
  if (!paused_ && AAudioStream_requestStart(stream_) != AAUDIO_OK) {
    LOGW("cannot start the microphone");
    AAudioStream_close(stream_);
    stream_ = nullptr;
    return false;
  }
  LOGI("microphone open at %d Hz", device_rate_);
  return true;
}

void MicIn::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stream_) return;
  AAudioStream_requestStop(stream_);
  AAudioStream_close(stream_);
  stream_ = nullptr;
}

void MicIn::set_paused(bool paused) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (paused == paused_) return;
  paused_ = paused;
  if (!stream_) return;
  if (paused) AAudioStream_requestStop(stream_);
  else { rd_.store(wr_.load()); AAudioStream_requestStart(stream_); }
}

aaudio_data_callback_result_t MicIn::on_data(AAudioStream*, void* user, void* audio, int32_t frames) {
  auto* self = static_cast<MicIn*>(user);
  const auto* in = static_cast<const int16_t*>(audio);
  size_t w = self->wr_.load(std::memory_order_relaxed);
  const size_t r = self->rd_.load(std::memory_order_acquire);
  for (int32_t i = 0; i < frames && w - r < RING; ++i, ++w) self->ring_[w & (RING - 1)] = in[i];
  self->wr_.store(w, std::memory_order_release);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void MicIn::on_error(AAudioStream*, void* user, aaudio_result_t error) {
  // The input route went away (a headset unplugged). Not reopened from here:
  // the next frame finds the ring dry and feeds silence.
  LOGW("microphone stream error: %s", AAudio_convertResultToText(error));
  (void)user;
}

void MicIn::take_frame(std::vector<int16_t>& out) {
  out.clear();
  if (!stream_) return;
  size_t r = rd_.load(std::memory_order_relaxed);
  const size_t w = wr_.load(std::memory_order_acquire);
  // Capture arrives in bursts; keep at most a few frames of backlog so a stall
  // never turns into lag.
  const double step = static_cast<double>(device_rate_) / DS_RATE;
  const size_t need = static_cast<size_t>(PER_FRAME * step) + 2;
  if (w - r > need * 4) r = w - need * 2;
  raw_.clear();
  while (raw_.size() < PER_FRAME && w - r >= 2) {
    // Linear resampling from the device rate to the DS's.
    const int16_t a = ring_[r & (RING - 1)], b = ring_[(r + 1) & (RING - 1)];
    raw_.push_back(static_cast<int16_t>(a + (b - a) * phase_));
    phase_ += step;
    const auto whole = static_cast<size_t>(phase_);
    r += whole;
    phase_ -= static_cast<double>(whole);
  }
  rd_.store(r, std::memory_order_release);
  if (raw_.empty()) return;

  double sq = 0;
  out.reserve(raw_.size());
  for (int16_t v : raw_) {
    dc_ += (v - dc_) * 0.005;
    const double y = std::clamp((v - dc_) * GAIN, -32768.0, 32767.0);
    out.push_back(static_cast<int16_t>(y));
    sq += y * y;
  }
  const double rms = std::sqrt(sq / static_cast<double>(out.size()));
  floor_ = rms < floor_ ? rms : floor_ + (rms - floor_) * 0.002;   // drops at once, creeps up
  if (rms < floor_ * GATE + 16) std::fill(out.begin(), out.end(), 0);
}

} // namespace ninds
