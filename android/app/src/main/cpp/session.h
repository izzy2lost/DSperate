// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - one running game: the machine, its thread, and what it talks to.
//
// The emulator thread owns the NDS. Everything else reaches it through
// atomics (input, speed) or the command queue, whose jobs run between frames
// (where the core allows save states, resets and cheat changes).
#pragma once
#include "audio_out.h"
#include "presenter_gl.h"

#include "core/nds.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ds::net { class SlirpDriver; }

namespace ninds {

struct SessionConfig {
  int rom_fd = -1;              // the ROM, open for reading; the caller keeps it open for the session
  std::string rom_name;         // display name; names the zip cache entry
  std::string save_path;        // battery save
  std::string cache_dir;        // zip extraction and the ROM link
  std::string bios_dir;         // bios9.bin, bios7.bin, firmware.bin (each optional)
  bool firmware_boot = false;   // boot through the firmware menu when real dumps allow it
  bool jit = true;
  bool fast_timing = true;
  bool gpu3d = false;
  int gpu_scale = 1;            // 3D internal resolution (1-8); above 1 implies the GPU raster
  bool tex_filter = false;      // bilinear texture filtering; implies the GPU raster
  bool aa = false;
  bool internet = false;
  bool audio = true;
  int volume = 100;
  ds::bios::UserSettings user;
};

struct CheatItem {
  std::string name, description;
  std::vector<uint32_t> words;
};

class Session {
public:
  Session();
  ~Session();

  // Builds the machine and loads the game; empty on success, else why not.
  std::string boot(const SessionConfig& cfg);
  void start();
  void stop();   // flushes the battery save and the firmware settings

  Presenter& presenter() { return presenter_; }

  void set_paused(bool paused);
  void set_buttons(uint32_t mask) { buttons_.store(mask, std::memory_order_relaxed); }
  void set_touch(int x, int y, bool down) {
    touch_.store((down ? 1u << 31 : 0u) | (static_cast<uint32_t>(x & 0xFF) << 8) | static_cast<uint32_t>(y & 0xFF), std::memory_order_relaxed);
  }
  void set_lid(bool closed) { lid_.store(closed, std::memory_order_relaxed); }
  void set_mic(bool blowing) { mic_.store(blowing, std::memory_order_relaxed); }
  void set_fast_forward(bool on) { ff_.store(on, std::memory_order_relaxed); }
  void set_ff_speed(int multiplier) { ff_speed_.store(multiplier, std::memory_order_relaxed); }   // 0 = unlimited
  // 0 = none, -1 = auto (skip when late, up to 3 in a row), N = draw one in N+1.
  void set_frameskip(int n) { frameskip_.store(n, std::memory_order_relaxed); }
  void set_volume(int percent) { audio_.set_volume(percent); }
  void set_audio_enabled(bool on) { audio_on_.store(on, std::memory_order_relaxed); }
  void set_screens_shown(bool top, bool bottom) { shown_.store((top ? 1 : 0) | (bottom ? 2 : 0), std::memory_order_relaxed); }

  // Run on the emulator thread between frames; these block until done.
  std::string save_state(const std::string& path);
  std::string load_state(const std::string& path);
  void reset();
  void set_cheats(std::vector<CheatItem> enabled);
  bool flush_save();
  // The game's cheats from an R4 usrcheat.dat, for the current cart.
  std::vector<CheatItem> database_cheats(const std::string& db_path, std::string& err);

  double fps() const { return fps_.load(std::memory_order_relaxed); }
  std::string game_code() const;

private:
  void run();
  void run_one_frame();
  void post(std::function<void()> job, bool wait);
  void drain_jobs();
  void write_save();

  std::unique_ptr<ds::NDS> nds_;
  SessionConfig cfg_;
  bool jit_on_ = false;
  std::string fw_override_;
  Presenter presenter_;
  AudioOut audio_;
  std::unique_ptr<ds::net::SlirpDriver> slirp_;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool quit_ = false;
  bool paused_ = false;
  std::atomic<bool> clock_reset_{true};

  std::atomic<uint32_t> buttons_{0}, touch_{0};
  std::atomic<bool> lid_{false}, mic_{false}, ff_{false}, audio_on_{true};
  std::atomic<int> ff_speed_{3}, frameskip_{0}, shown_{3};
  std::atomic<double> fps_{0.0};

  // Emulator thread only.
  uint32_t last_sram_writes_ = 0;
  int sram_quiet_frames_ = 0;
  int skip_run_ = 0;              // frames skipped in a row (auto)
  bool late_ = false;             // the last frame missed its deadline
  uint64_t frame_index_ = 0;
  std::vector<int16_t> mic_buf_;
  HiresFrame hires_;            // this frame's high-res 3D data, on its way to the presenter
  uint32_t mic_seed_ = 0x1234567u;
};

} // namespace ninds
