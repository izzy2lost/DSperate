// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - JNI surface of com.izzy2lost.ninds.NativeLib.
//
// The emulator runs in its own process (":emu"), one game per process, so a
// single global session is all there ever is.
#include "session.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

std::unique_ptr<ninds::Session> g_session;

std::string str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

ninds::Session* session() { return g_session.get(); }

// NewStringUTF takes modified UTF-8 and aborts on anything else under
// CheckJNI; cheat databases are often Shift-JIS or Latin-1. Keeps valid
// one- to three-byte sequences and turns the rest into '?'.
jstring jstr(JNIEnv* env, const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 0;
    bool ok = n != 0 && c != 0 && i + n <= in.size();
    for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
    if (ok) { out.append(in, i, n); i += n; }
    else { out += '?'; ++i; }
  }
  return env->NewStringUTF(out.c_str());
}

// The core reports through stderr, which Android discards: forward it to logcat.
void forward_stderr() {
  int fds[2];
  if (pipe(fds) != 0) return;
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  std::thread([rd = fds[0]] {
    std::string line;
    char buf[512];
    for (ssize_t n; (n = read(rd, buf, sizeof buf)) > 0;) {
      for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') { __android_log_write(ANDROID_LOG_INFO, "DSperate", line.c_str()); line.clear(); }
        else line += buf[i];
      }
    }
  }).detach();
}

} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
  forward_stderr();
  return JNI_VERSION_1_6;
}

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_izzy2lost_ninds_NativeLib_##name

JNI_FN(jstring, nativeBoot)(JNIEnv* env, jobject, jint rom_fd, jstring rom_name, jstring save_path, jstring cache_dir,
                            jstring bios_dir, jboolean firmware_boot, jboolean jit, jboolean fast_timing, jboolean gpu3d,
                            jint gpu_scale, jboolean aa, jboolean internet, jboolean audio, jint volume, jstring nickname, jstring message,
                            jint birthday_month, jint birthday_day, jint colour, jint language) {
  if (g_session) { g_session->stop(); g_session.reset(); }
  ninds::SessionConfig cfg;
  cfg.rom_fd = rom_fd;
  cfg.rom_name = str(env, rom_name);
  cfg.save_path = str(env, save_path);
  cfg.cache_dir = str(env, cache_dir);
  cfg.bios_dir = str(env, bios_dir);
  cfg.firmware_boot = firmware_boot;
  cfg.jit = jit;
  cfg.fast_timing = fast_timing;
  cfg.gpu3d = gpu3d;
  cfg.gpu_scale = gpu_scale;
  cfg.aa = aa;
  cfg.internet = internet;
  cfg.audio = audio;
  cfg.volume = volume;
  const std::string nick = str(env, nickname);
  if (!nick.empty()) cfg.user.nickname = nick.substr(0, 10);
  cfg.user.message = str(env, message).substr(0, 26);
  cfg.user.birthday_month = static_cast<ds::u8>(birthday_month < 1 || birthday_month > 12 ? 1 : birthday_month);
  cfg.user.birthday_day = static_cast<ds::u8>(birthday_day < 1 || birthday_day > 31 ? 1 : birthday_day);
  cfg.user.favourite_colour = static_cast<ds::u8>(colour & 15);
  cfg.user.language = static_cast<ds::u8>(language < 0 || language > 5 ? 1 : language);
  g_session = std::make_unique<ninds::Session>();
  const std::string err = g_session->boot(cfg);
  if (!err.empty()) g_session.reset();
  return jstr(env, err);
}

JNI_FN(void, nativeStart)(JNIEnv*, jobject) { if (auto* s = session()) s->start(); }

JNI_FN(void, nativeShutdown)(JNIEnv*, jobject) {
  if (g_session) { g_session->stop(); g_session.reset(); }
}

JNI_FN(void, nativeSetSurface)(JNIEnv* env, jobject, jobject surface) {
  auto* s = session();
  if (!s) return;
  s->presenter().set_window(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}

JNI_FN(void, nativeSetLayout)(JNIEnv*, jobject, jint tx, jint ty, jint tw, jint th, jboolean tshown,
                              jint bx, jint by, jint bw, jint bh, jboolean bshown) {
  auto* s = session();
  if (!s) return;
  s->presenter().set_layout({tx, ty, tw, th, tshown != 0}, {bx, by, bw, bh, bshown != 0});
  s->set_screens_shown(tshown, bshown);
}

JNI_FN(void, nativeSetFilter)(JNIEnv*, jobject, jint filter) { if (auto* s = session()) s->presenter().set_filter(filter); }
JNI_FN(void, nativeSetPaused)(JNIEnv*, jobject, jboolean p) { if (auto* s = session()) s->set_paused(p); }
JNI_FN(void, nativeSetButtons)(JNIEnv*, jobject, jint mask) { if (auto* s = session()) s->set_buttons(static_cast<uint32_t>(mask)); }
JNI_FN(void, nativeSetTouch)(JNIEnv*, jobject, jint x, jint y, jboolean down) { if (auto* s = session()) s->set_touch(x, y, down); }
JNI_FN(void, nativeSetLid)(JNIEnv*, jobject, jboolean closed) { if (auto* s = session()) s->set_lid(closed); }
JNI_FN(void, nativeSetMic)(JNIEnv*, jobject, jboolean on) { if (auto* s = session()) s->set_mic(on); }
JNI_FN(void, nativeSetFastForward)(JNIEnv*, jobject, jboolean on) { if (auto* s = session()) s->set_fast_forward(on); }
JNI_FN(void, nativeSetFastForwardSpeed)(JNIEnv*, jobject, jint m) { if (auto* s = session()) s->set_ff_speed(m); }
JNI_FN(void, nativeSetFrameskip)(JNIEnv*, jobject, jint n) { if (auto* s = session()) s->set_frameskip(n); }
JNI_FN(void, nativeSetVolume)(JNIEnv*, jobject, jint v) { if (auto* s = session()) s->set_volume(v); }
JNI_FN(void, nativeSetAudioEnabled)(JNIEnv*, jobject, jboolean on) { if (auto* s = session()) s->set_audio_enabled(on); }

JNI_FN(jstring, nativeSaveState)(JNIEnv* env, jobject, jstring path) {
  auto* s = session();
  return jstr(env, s ? s->save_state(str(env, path)) : "No game is running");
}

JNI_FN(jstring, nativeLoadState)(JNIEnv* env, jobject, jstring path) {
  auto* s = session();
  return jstr(env, s ? s->load_state(str(env, path)) : "No game is running");
}

JNI_FN(void, nativeReset)(JNIEnv*, jobject) { if (auto* s = session()) s->reset(); }
JNI_FN(jboolean, nativeFlushSave)(JNIEnv*, jobject) { auto* s = session(); return s && s->flush_save(); }
JNI_FN(jfloat, nativeGetFps)(JNIEnv*, jobject) { auto* s = session(); return s ? static_cast<jfloat>(s->fps()) : 0.f; }

JNI_FN(jstring, nativeGameCode)(JNIEnv* env, jobject) {
  auto* s = session();
  return env->NewStringUTF(s ? s->game_code().c_str() : "");
}

// Both screens, top above bottom, as opaque ARGB ints (Bitmap's layout).
JNI_FN(jintArray, nativeGetFrame)(JNIEnv* env, jobject) {
  auto* s = session();
  if (!s) return nullptr;
  std::vector<uint32_t> px;
  s->presenter().snapshot(px);
  for (uint32_t& p : px) p |= 0xFF000000u;
  jintArray out = env->NewIntArray(static_cast<jsize>(px.size()));
  if (out) env->SetIntArrayRegion(out, 0, static_cast<jsize>(px.size()), reinterpret_cast<const jint*>(px.data()));
  return out;
}

// Enabled codes: names[i] with its words[i] (pairs of 32-bit words).
JNI_FN(void, nativeSetCheats)(JNIEnv* env, jobject, jobjectArray names, jobjectArray words) {
  auto* s = session();
  if (!s) return;
  std::vector<ninds::CheatItem> items;
  const jsize n = names ? env->GetArrayLength(names) : 0;
  for (jsize i = 0; i < n; ++i) {
    ninds::CheatItem it;
    auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
    it.name = str(env, name);
    env->DeleteLocalRef(name);
    auto w = static_cast<jintArray>(env->GetObjectArrayElement(words, i));
    if (w) {
      const jsize len = env->GetArrayLength(w);
      it.words.resize(static_cast<size_t>(len));
      env->GetIntArrayRegion(w, 0, len, reinterpret_cast<jint*>(it.words.data()));
      env->DeleteLocalRef(w);
    }
    items.push_back(std::move(it));
  }
  s->set_cheats(std::move(items));
}

// The running game's entries in an R4 usrcheat.dat, flattened as
// {name, description, code text} triples. A single element is an error message.
JNI_FN(jobjectArray, nativeDatabaseCheats)(JNIEnv* env, jobject, jstring db_path) {
  jclass string_class = env->FindClass("java/lang/String");
  auto* s = session();
  std::string err;
  std::vector<ninds::CheatItem> items;
  if (s) items = s->database_cheats(str(env, db_path), err);
  else err = "No game is running";
  if (items.empty()) {
    if (err.empty()) err = "No cheats for this game";
    jobjectArray out = env->NewObjectArray(1, string_class, nullptr);
    env->SetObjectArrayElement(out, 0, jstr(env, err));
    return out;
  }
  jobjectArray out = env->NewObjectArray(static_cast<jsize>(items.size() * 3), string_class, nullptr);
  for (size_t i = 0; i < items.size(); ++i) {
    std::string code;
    char word[10];
    for (size_t k = 0; k < items[i].words.size(); ++k) {
      std::snprintf(word, sizeof word, "%08X", items[i].words[k]);
      code += word;
      code += (k & 1) ? "\n" : " ";
    }
    while (!code.empty() && (code.back() == '\n' || code.back() == ' ')) code.pop_back();
    const std::string fields[3] = {items[i].name, items[i].description, code};
    for (int f = 0; f < 3; ++f) {
      jstring js = jstr(env, fields[f]);
      env->SetObjectArrayElement(out, static_cast<jsize>(i * 3 + f), js);
      env->DeleteLocalRef(js);
    }
  }
  return out;
}
