// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// Host lid switch (evdev SW_LID; SDL has no switch events). Hosts without a
// switch get a synthetic pulse: a BOOTTIME/MONOTONIC clock jump after resume
// closes the lid briefly, mimicking the real hinge sequence.
//
// The guest always sees a close last at least kMinClosedFrames emulated
// frames. Hosts suspend as the lid shuts, so the open can follow the close by
// one frame; a game still entering sleep then misses the wake and sits on a
// blank screen. Emulated frames don't advance while the host sleeps.
class Lid {
public:
  void open();
  void close();
  // Call once a frame. Returns true if state changed, stores it in `closed`.
  bool poll(bool& closed);
  bool has_switch() const { return fd_ >= 0; }

  static constexpr int kMinClosedFrames = 60;

private:
  int  fd_ = -1;
  bool closed_ = false;        // the switch itself
  bool shown_ = false;         // what the guest has been told
  int  hold_ = 0;              // frames the guest must still see closed
  int  pulse_ = 0;             // frames left of a suspend-detected close
  s64  skew_ns_ = 0;           // last boottime - monotonic
};

} // namespace ds::sdl
