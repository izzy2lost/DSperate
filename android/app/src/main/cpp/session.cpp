// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - one running game: the machine, its thread, and what it talks to.
#include "session.h"

#include "core/cheat/database.h"
#include "core/cpu/timing_mode.h"
#include "core/host_cores.h"
#include "core/state/state.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "net/lan_mp.h"
#include "net/slirp_driver.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

#define LOG_TAG "NinDS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ninds {

namespace {
using Clock = std::chrono::steady_clock;
constexpr double DS_FPS = 59.8261;
constexpr int SRAM_QUIET_FRAMES = 60;   // write the battery save once the game has stopped writing for a second

bool file_exists(const std::string& p) {
  struct stat st{};
  return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<uint8_t> slurp(const std::string& path) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t buf[1 << 16];
    for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
    std::fclose(f);
  }
  return data;
}

bool write_atomic(const std::string& path, const uint8_t* data, size_t n) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, n, f) == n;
  std::fclose(f);
  return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string sanitize(const std::string& name) {
  std::string out;
  for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ' ') ? c : '_';
  return out.empty() ? std::string("game") : out;
}
} // namespace

Session::Session() = default;

Session::~Session() { stop(); hires_.release_buffer(); }

std::string Session::boot(const SessionConfig& cfg) {
  cfg_ = cfg;
  // The core's fastmem views are laid in 4 KB host pages; a 16 KB-page kernel
  // would refuse them, so the recompiler goes through the page table instead.
  if (sysconf(_SC_PAGESIZE) != 4096) setenv("DS_FASTMEM", "0", 1);
  ds::g_fast_timing = cfg.fast_timing && ds::kFastTimingAvailable;
  // Phones are big.LITTLE: the handheld pinning (emulation on the last core,
  // the device's interrupts on core 0) does not fit them. Let the scheduler place threads.
  ds::set_thread_layout(false);

  nds_ = std::make_unique<ds::NDS>();
  ds::NDS& nds = *nds_;

  std::string err;
  const std::string b9 = cfg.bios_dir + "/bios9.bin", b7 = cfg.bios_dir + "/bios7.bin", fw = cfg.bios_dir + "/firmware.bin";
  const bool have_bios = file_exists(b9) && file_exists(b7);
  if (!nds.load_bios(have_bios ? b9 : "", have_bios ? b7 : "", file_exists(fw) ? fw : "", cfg.user, &err)) {
    // A bad dump should not keep the game from starting: fall back to the built-in BIOS.
    LOGW("bios: %s; using the built-in BIOS", err.c_str());
    boot_note_ = "Your BIOS or firmware files could not be used (" + err.substr(err.rfind('/') + 1) + "); using the built-in BIOS.";
    if (!nds.load_bios("", "", "", cfg.user, &err)) return "BIOS: " + err;
  }
  if (!nds.firmware_synthetic) {
    fw_override_ = cfg.bios_dir + "/firmware.ovr";
    if (file_exists(fw_override_) && !nds.load_firmware_override(fw_override_, err)) LOGW("firmware override: %s", err.c_str());
  }
  nds.reset();
  nds.io.start_rtc_clock();

  nds.rom_cache_dir = cfg.cache_dir + "/unpacked";
  mkdir(nds.rom_cache_dir.c_str(), 0700);
  nds.rom_cache_max_bytes = 2048ull << 20;
  // A picked ROM is only readable through the descriptor Android handed over
  // (reopening it by path is refused), so the core reads the descriptor. The
  // path it is given names the game: a zipped ROM's unpacked copy is cached
  // under it, and kept for as long as that placeholder file exists.
  const std::string names = cfg.cache_dir + "/rom";
  mkdir(names.c_str(), 0700);
  const std::string name = names + "/" + sanitize(cfg.rom_name.empty() ? std::string("game.nds") : cfg.rom_name);
  if (FILE* f = std::fopen(name.c_str(), "ab")) std::fclose(f);
  if (!nds.load_rom_fd(cfg.rom_fd, name)) return "Could not read the ROM. Is it a Nintendo DS game?";
  // The DS menu needs the console's own BIOS and a DS or DS Lite firmware: a
  // DSi or 3DS dump (128 KB, console type 0x57) carries no DS boot code and
  // would leave the screens black. Anything less starts the game directly.
  bool firmware_boot = cfg.firmware_boot;
  if (firmware_boot && !nds.can_boot_firmware()) {
    firmware_boot = false;
    if (boot_note_.empty()) boot_note_ = "The DS menu needs your own BIOS and firmware dumps; the game was started directly.";
  } else if (firmware_boot && (nds.firmware.size() < 0x40000 || nds.firmware[0x1D] == 0x57)) {
    firmware_boot = false;
    boot_note_ = "This firmware is from a DSi or 3DS and cannot show the DS menu; the game was started directly.";
  }
  if (!boot_note_.empty()) LOGW("%s", boot_note_.c_str());
  if (!firmware_boot) nds.setup_direct_boot();

  if (nds.cart && !cfg.save_path.empty()) {
    std::vector<uint8_t> data = slurp(cfg.save_path);
    if (!data.empty()) {
      const ds::cart::Cart::SaveLoad r = nds.cart->load_save(data.data(), data.size());
      if (!r.fitted) {
        // The first write resizes the file to the chip: keep the player's original.
        const std::string bak = cfg.save_path + ".bak";
        if (!file_exists(bak)) write_atomic(bak, data.data(), data.size());
      }
    }
    last_sram_writes_ = nds.cart->sram_writes();
  }

#if DSPERATE_JIT
  jit_on_ = cfg.jit && ds::jit::attach(nds, true, true);
  if (cfg.jit && !jit_on_) LOGW("recompiler unavailable; interpreting");
  // The DS menu boots under per-instruction budget checks, as on the desktop:
  // block-granularity overshoot can stop it booting (a black screen). Dropped
  // once the menu has launched the game, which wants the speed.
  if (jit_on_ && firmware_boot) {
    ds::jit::set_strict(true);
    strict_for_menu_ = true;
    LOGI("jit: strict timing for the DS menu");
  }
#endif
  nds.gpu3d.renderer().set_aa(cfg.aa);
  // Upscaled 3D and smoothed textures are the GPU raster's: either turns it on.
  const int scale = std::clamp(cfg.gpu_scale, 1, 8);
  if (cfg.gpu3d || scale > 1 || cfg.tex_filter) {
    std::string why;
    nds.gpu3d.renderer().set_gpu_scale(static_cast<ds::u32>(scale));
    nds.gpu3d.renderer().set_gpu_tex_filter(cfg.tex_filter);
    if (!nds.gpu3d.renderer().set_gpu(true, &why)) LOGW("gpu3d: unavailable (%s), drawing on the CPU", why.c_str());
    else if (scale > 1) nds.gpu.set_hires(true);
  }
  const std::string& mode = cfg.net_mode;
  if (mode == "internet") {
    slirp_ = std::make_unique<ds::net::SlirpDriver>();
    // Wiimmfi's resolver, as the desktop frontends default to.
    if (slirp_->start(ds::net::SlirpDriver::Dns::Custom, 0xB23E2BD4u)) nds.io.set_net_driver(slirp_.get());
    else { LOGW("internet: %s", slirp_->error().c_str()); slirp_.reset(); }
  } else if (mode == "auto" || mode == "host" || mode == "guest" || mode == "join") {
    // Local wireless, as the desktop's net.mode. The scan blocks for up to
    // 2.5 s; boot runs off the UI thread, behind the loading screen.
    lan_name_ = cfg.user.nickname.empty() ? std::string("NinDS") : cfg.user.nickname;
    lan_ = std::make_unique<ds::net::LanMp>();
    bool up = lan_->ok();
    std::string status;
    if (up && (mode == "auto" || mode == "guest")) {
      const auto role = lan_->start_auto(lan_name_, 2500, 16, mode == "auto");
      up = role != ds::net::LanMp::Role::None;
      if (up) status = role == ds::net::LanMp::Role::Host ? "Hosting a session" : "Joined " + lan_->peer_name();
    } else if (up && mode == "join") {
      up = !cfg.lan_address.empty() && lan_->start_client(lan_name_, cfg.lan_address);
      if (up) status = "Joined " + cfg.lan_address;
    } else if (up) {
      up = lan_->start_host(lan_name_, 16);
      if (up) status = "Hosting a session";
    }
    if (!up) {
      LOGW("lan: %s", cfg.lan_address.empty() && mode == "join" ? "no host address set" : lan_->error().c_str());
      lan_.reset();
      // A guest that heard nobody tries again when the game puts its radio on the air.
      guest_retry_armed_ = mode == "guest";
    } else {
      LOGI("lan: %s, player %d", status.c_str(), lan_->my_id());
      nds.io.wifi.set_transport(lan_.get());
      lan_status_ = status;
    }
  }
  audio_on_.store(cfg.audio);
  audio_.set_volume(cfg.volume);
  LOGI("booted %.4s (%s, %s BIOS, %s)", nds.cart ? nds.cart->header().game_code : "????", jit_on_ ? "recompiler" : "interpreter",
       nds.bios_native ? "native" : "built-in", ds::g_fast_timing ? "fast timing" : "exact timing");
  return {};
}

// The DS menu has launched the game once the ARM9 runs inside the game's own
// binary. Its code being in RAM says nothing: the menu loads the whole game
// while the health and safety screen is up, before anything is picked.
bool Session::game_launched(ds::NDS& nds) {
  if (!nds.cart) return true;
  const ds::cart::Header& h = nds.cart->header();
  const uint32_t pc = nds.arm9->hot.regs[15];
  return pc >= h.arm9_ram_address && pc - h.arm9_ram_address < h.arm9_size;
}

std::string Session::boot_note() {
  std::lock_guard<std::mutex> lock(mu_);
  return boot_note_;
}

std::string Session::game_code() const {
  if (!nds_ || !nds_->cart) return {};
  return std::string(nds_->cart->header().game_code, 4);
}

void Session::start() {
  if (thread_.joinable() || !nds_) return;
  presenter_.start();
  audio_.start(nds_->spu.output_rate_hz());   // always open: sound can be switched on mid-game
  quit_ = false;
  thread_ = std::thread([this] { run(); });
}

void Session::stop() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  audio_.stop();
  mic_in_.close();
  presenter_.stop();
  if (!nds_) return;
  if (nds_->cart && nds_->cart->sram_dirty()) write_save();
  if (!fw_override_.empty() && nds_->firmware_override_dirty()) {
    std::string err;
    if (!nds_->save_firmware_override(fw_override_, err)) LOGW("firmware override: %s", err.c_str());
  }
  if (slirp_) { nds_->io.set_net_driver(nullptr); slirp_->stop(); slirp_.reset(); }
  // A guest says goodbye rather than going silent.
  if (lan_) { lan_->end_session(); nds_->io.wifi.set_transport(nullptr); lan_.reset(); }
}

std::string Session::lan_status() {
  std::lock_guard<std::mutex> lock(mu_);
  return lan_status_;
}

void Session::set_lan_status(std::string s) {
  std::lock_guard<std::mutex> lock(mu_);
  lan_status_ = std::move(s);
}

void Session::set_paused(bool paused) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = paused;
    clock_reset_.store(true);
  }
  audio_.set_paused(paused);
  mic_in_.set_paused(paused);
  cv_.notify_all();
}

void Session::post(std::function<void()> job, bool wait) {
  if (!thread_.joinable()) { job(); return; }
  if (!wait) {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.push_back(std::move(job));
    cv_.notify_all();
    return;
  }
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> f = done->get_future();
  {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.push_back([job = std::move(job), done] { job(); done->set_value(); });
    cv_.notify_all();
  }
  f.wait();
}

void Session::drain_jobs() {
  for (;;) {
    std::function<void()> job;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

void Session::write_save() {
  ds::cart::Cart* cart = nds_->cart.get();
  if (!cart || cart->sram().empty() || cfg_.save_path.empty()) return;
  if (write_atomic(cfg_.save_path, cart->sram().data(), cart->sram().size())) cart->clear_sram_dirty();
  else LOGW("save: cannot write %s", cfg_.save_path.c_str());
}

bool Session::flush_save() {
  bool wrote = false;
  post([&] {
    if (nds_ && nds_->cart && nds_->cart->sram_dirty()) { write_save(); wrote = true; }
  }, true);
  return wrote;
}

std::string Session::save_state(const std::string& path) {
  std::string result;
  post([&] {
    ds::state::Writer w;
    std::string err;
    if (!nds_->save_state(w, err)) { result = "Could not save: " + err; return; }
    if (!write_atomic(path, w.data().data(), w.data().size())) result = "Could not write the state file";
  }, true);
  return result;
}

std::string Session::load_state(const std::string& path) {
  std::string result;
  post([&] {
    std::vector<uint8_t> bytes = slurp(path);
    if (bytes.empty()) { result = "That slot is empty"; return; }
    ds::state::Reader r(bytes.data(), bytes.size());
    std::string err;
    if (!nds_->load_state(r, err)) {
      // A failed load leaves the machine unusable: start the game over.
      result = "Could not load: " + err;
#if DSPERATE_JIT
      if (jit_on_) ds::jit::flush_all();
#endif
      nds_->reset();
      nds_->setup_direct_boot();
    }
    clock_reset_.store(true);
    audio_.clear();
  }, true);
  return result;
}

void Session::reset() {
  post([this] {
    if (nds_->cart && nds_->cart->sram_dirty()) write_save();
#if DSPERATE_JIT
    if (jit_on_) ds::jit::flush_all();
#endif
    nds_->reset();
    if (!cfg_.firmware_boot || !nds_->can_boot_firmware()) nds_->setup_direct_boot();
    clock_reset_.store(true);
  }, true);
}

void Session::set_cheats(std::vector<CheatItem> enabled) {
  post([this, items = std::move(enabled)] {
    std::vector<ds::cheat::Code> codes;
    for (const CheatItem& it : items) {
      ds::cheat::Code c;
      c.name = it.name;
      c.enabled = true;
      c.words.assign(it.words.begin(), it.words.end());
      codes.push_back(std::move(c));
    }
    nds_->cheats.codes = std::move(codes);
  }, true);
}

std::vector<CheatItem> Session::database_cheats(const std::string& db_path, std::string& err) {
  std::vector<CheatItem> out;
  post([&] {
    if (!nds_->cart) { err = "No game is loaded"; return; }
    uint8_t header[512];
    nds_->cart->rom_read(0, header, sizeof header);
    ds::cheat::GameCheats found;
    if (!ds::cheat::load_for_header(db_path, header, found, err)) {
      if (err.empty()) err = "This game is not in the cheat database";
      return;
    }
    for (const ds::cheat::Code& c : found.codes) {
      if (c.is_note()) continue;
      CheatItem it;
      it.name = c.group >= 0 && c.group < static_cast<int>(found.groups.size()) ? found.groups[c.group].name + ": " + c.name : c.name;
      it.description = c.description;
      it.words.assign(c.words.begin(), c.words.end());
      out.push_back(std::move(it));
    }
  }, true);
  return out;
}

void Session::run_one_frame() {
  ds::NDS& nds = *nds_;
  const uint32_t mask = buttons_.load(std::memory_order_relaxed);
  nds.io.set_buttons(mask);
  const uint32_t t = touch_.load(std::memory_order_relaxed);
  nds.io.set_touch(static_cast<int>((t >> 8) & 0xFF), static_cast<int>(t & 0xFF), (t >> 31) != 0);
  const bool lid = lid_.load(std::memory_order_relaxed);
  if (lid != nds.io.lid_closed()) {
    nds.io.set_lid(lid);
    if (lid && nds.cart && nds.cart->sram_dirty()) write_save();
  }
  const int mic_mode = mic_mode_.load(std::memory_order_relaxed);
  if (mic_mode > 0 && mic_.load(std::memory_order_relaxed)) {
    // Blowing into the microphone: loud noise is what games listen for.
    mic_buf_.resize(800);
    for (int16_t& s : mic_buf_) {
      mic_seed_ = mic_seed_ * 1664525u + 1013904223u;
      s = static_cast<int16_t>(static_cast<int32_t>(mic_seed_ >> 16) - 32768) / 2;
    }
    nds.io.set_mic(mic_buf_.data(), mic_buf_.size());
  } else if (mic_mode == 2) {
    // Opened the first time the game reads the microphone, not before.
    if (!mic_tried_ && nds.io.mic_used()) { mic_tried_ = true; mic_in_.open(); }
    mic_in_.take_frame(mic_buf_);
    nds.io.set_mic(mic_buf_.empty() ? nullptr : mic_buf_.data(), mic_buf_.size());
  } else {
    if (mic_in_.active()) { mic_in_.close(); mic_tried_ = false; }
    nds.io.set_mic(nullptr, 0);
  }
  const int shown = shown_.load(std::memory_order_relaxed);
  nds.gpu.set_screen_visible(0, (shown & 1) != 0);
  nds.gpu.set_screen_visible(1, (shown & 2) != 0);

  // Frameskip for frame i+1 is decided before frame i runs (its 3D is drawn during frame i).
  // In a local wireless session the peers keep time: no fast forward.
  const bool ff = ff_.load(std::memory_order_relaxed) && !lan_;
  const int fs = frameskip_.load(std::memory_order_relaxed);
  bool skip_next = false;
  int pattern = 0;
  if (ff) { const int s = ff_speed_.load(std::memory_order_relaxed); pattern = s <= 0 ? 7 : std::min(s - 1, 7); }
  else if (fs > 0) pattern = fs;
  if (pattern > 0) {
    // Whole display periods, so a game alternating its screens keeps both.
    const int period = std::max<int>(1, nds.gpu.display_phase_period());
    const int skip = pattern * period, cycle = skip + period;
    skip_next = static_cast<int>((frame_index_ + 1) % static_cast<uint64_t>(cycle)) < skip;
  } else if (fs < 0) {
    skip_next = late_ && skip_run_ < 3;
  }
  skip_run_ = skip_next ? skip_run_ + 1 : 0;
  nds.gpu.set_frame_skip(skip_next);
  const bool drawn = !nds.gpu.will_skip_frame();

  if (slirp_) slirp_->process();
  if ((lan_ || guest_retry_armed_ || scanning_) && !ff) run_net_frame(nds);
  else nds.run_frame();
  ++frame_index_;

  int16_t buf[1024 * 2];
  const bool sound = audio_on_.load(std::memory_order_relaxed);
  for (size_t n; (n = nds.spu.take(buf, 1024)) != 0;) if (sound) audio_.push(buf, n);
  audio_.set_source_rate(nds.spu.output_rate_hz());

  static Clock::time_point last_submit;
  const Clock::time_point now = Clock::now();
  if (drawn && (!ff || now - last_submit >= std::chrono::milliseconds(14))) {
    const ds::gpu::Gpu::Hires& h = nds.gpu.hires();
    if ((h.layer3d || h.layer3d_buffer) && h.scale > 1) {
      // Copied now: the line records are reused by later frames. The 3D
      // picture is copied too, unless it came as a buffer (zero-copy), which
      // is referenced instead.
      const size_t n = static_cast<size_t>(ds::SCREEN_W) * ds::SCREEN_H;
      hires_.valid = true;
      hires_.scale = h.scale;
      hires_.desc.assign(h.desc.begin(), h.desc.end());
      hires_.operand.assign(h.operand.begin(), h.operand.end());
      hires_.line.assign(h.line.begin(), h.line.end());
      hires_.release_buffer();
      if (h.layer3d_buffer) {
        hires_.buffer = static_cast<AHardwareBuffer*>(const_cast<void*>(h.layer3d_buffer));
        AHardwareBuffer_acquire(hires_.buffer);
        hires_.layer.clear();
      } else {
        hires_.layer.assign(h.layer3d, h.layer3d + n * h.scale * h.scale);
      }
      presenter_.submit(nds.gpu.framebuffer(0), nds.gpu.framebuffer(1), &hires_);
    } else {
      presenter_.submit(nds.gpu.framebuffer(0), nds.gpu.framebuffer(1));
    }
    last_submit = now;
  }

  if (nds.cart) {
    const uint32_t writes = nds.cart->sram_writes();
    if (writes != last_sram_writes_) { last_sram_writes_ = writes; sram_quiet_frames_ = 0; }
    else if (nds.cart->sram_dirty() && ++sram_quiet_frames_ >= SRAM_QUIET_FRAMES) { write_save(); sram_quiet_frames_ = 0; }
  }
#if DSPERATE_JIT
  if (strict_for_menu_ && game_launched(nds)) {
    strict_for_menu_ = false;
    ds::jit::set_strict(std::getenv("DS_JIT_STRICT") != nullptr);
    LOGI("jit: the DS menu launched the game; full speed");
  }
#endif
  // The game asked the console to switch off; there is no power to cut, so carry on.
  if (nds.power_off) nds.power_off = false;
}

void Session::run_net_frame(ds::NDS& nds) {
  if (lan_) lan_->process();   // ENet and discovery, once per frame, on this thread
  // Edge-triggered: the game switching its radio on is when a guest looks again.
  const bool on_air = nds.io.wifi.tx_frames() > 0;
  if (on_air && !radio_was_on_ && guest_retry_armed_ && !scanning_) {
    guest_retry_armed_ = false;
    lan_ = std::make_unique<ds::net::LanMp>();
    if (lan_->ok() && lan_->start_discovery()) {
      scanning_ = true;
      scan_began_ = Clock::now();
      LOGI("lan: the game is on the air; looking for a session");
    } else {
      LOGW("lan: %s", lan_->error().c_str());
      lan_.reset();
    }
  }
  radio_was_on_ = on_air;
  if (scanning_) {
    lan_->scan_step();
    if (Clock::now() - scan_began_ >= std::chrono::milliseconds(2500)) {   // as start_auto: a host beacons once a second
      scanning_ = false;
      if (lan_->scan_join(lan_name_)) {
        nds.io.wifi.set_transport(lan_.get());
        LOGI("lan: joined %s as player %d", lan_->peer_name().c_str(), lan_->my_id());
        set_lan_status("Joined " + lan_->peer_name());
      } else {
        LOGW("lan: %s", lan_->error().c_str());
        lan_.reset();
      }
    }
  }
  if (!lan_ || scanning_) { nds.run_frame(); return; }

  // Spread the frame's emulation across its period in 1 ms slices, so a
  // peer's CMD is answered within a slice rather than a frame later.
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / DS_FPS));
  const Clock::time_point start = std::max(frame_start_, Clock::now() - period / 2);
  constexpr uint64_t slice = ds::ARM9_CLOCK_HZ / 1000;
  constexpr int slices = static_cast<int>(ds::CYCLES_PER_FRAME / slice) + 1;
  for (int k = 1; !nds.run_frame_slice(slice); ++k) {
    const Clock::time_point due = start + period * k / slices;
    if (due - Clock::now() >= std::chrono::microseconds(500)) std::this_thread::sleep_until(due);
  }
}

void Session::run() {
  pthread_setname_np(pthread_self(), "NinDS emu");
  // As urgent as the display: a late frame is a dropped frame. Fails quietly where refused.
  setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -10);

  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / DS_FPS));
  Clock::time_point deadline = Clock::now();
  Clock::time_point fps_mark = deadline;
  int fps_frames = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return quit_ || !paused_ || !jobs_.empty(); });
      if (quit_) break;
    }
    drain_jobs();
    bool paused, reset_clock;
    {
      std::lock_guard<std::mutex> lock(mu_);
      paused = paused_;
      reset_clock = clock_reset_.exchange(false);
      if (quit_) break;
    }
    if (paused) continue;
    if (reset_clock) { deadline = Clock::now(); late_ = false; }

    frame_start_ = deadline;
    run_one_frame();

    const bool ff = ff_.load(std::memory_order_relaxed) && !lan_;
    const int speed = ff ? ff_speed_.load(std::memory_order_relaxed) : 1;
    const Clock::time_point now = Clock::now();
    if (speed > 0) {
      deadline += period / speed;
      late_ = now > deadline + std::chrono::milliseconds(1);
      // Far behind (a hitch, a debugger, the app in the background): start over
      // rather than run flat out to catch up.
      if (now > deadline + period * 3) deadline = now;
      else if (deadline > now) std::this_thread::sleep_until(deadline);
    } else {
      deadline = now;
      late_ = false;
    }

    ++fps_frames;
    const auto since = Clock::now() - fps_mark;
    if (since >= std::chrono::milliseconds(500)) {
      fps_.store(fps_frames / std::chrono::duration<double>(since).count(), std::memory_order_relaxed);
      fps_frames = 0;
      fps_mark = Clock::now();
    }
  }
  drain_jobs();
}

} // namespace ninds
