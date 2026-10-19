// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - RetroAchievements' HTTP through Java.
#include "http_jni.h"

#include <android/log.h>

#include <cstring>
#include <string>

namespace ninds {
namespace {

JavaVM* g_vm = nullptr;
jclass g_class = nullptr;      // global ref to CheevosHttp
jmethodID g_perform = nullptr;

// A native thread is attached on its first request and detached when it exits.
struct Attached {
  JNIEnv* env = nullptr;
  bool ours = false;
  ~Attached() { if (ours && g_vm) g_vm->DetachCurrentThread(); }
};

JNIEnv* thread_env() {
  thread_local Attached a;
  if (a.env) return a.env;
  if (!g_vm) return nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&a.env), JNI_VERSION_1_6) == JNI_OK) return a.env;
  JavaVMAttachArgs args{JNI_VERSION_1_6, "NinDS cheevos", nullptr};
  if (g_vm->AttachCurrentThread(&a.env, &args) != JNI_OK) { a.env = nullptr; return nullptr; }
  a.ours = true;
  return a.env;
}

class JniBackend final : public ds::cheevos::Backend {
public:
  const char* name() const override { return "android"; }

  // CheevosHttp.perform returns the status as four big-endian bytes, then the
  // body; status 0 carries the transport's error message as the body.
  ds::cheevos::Response perform(const ds::cheevos::Request& req, const char* user_agent) override {
    ds::cheevos::Response res;
    JNIEnv* env = thread_env();
    if (!env || !g_perform) { res.error = "no Java transport"; __android_log_print(ANDROID_LOG_WARN, "NinDS", "cheevos: %s", res.error.c_str()); return res; }
    jstring url = env->NewStringUTF(req.url.c_str());
    jstring body = env->NewStringUTF(req.body.c_str());
    jstring type = env->NewStringUTF(req.content_type.c_str());
    jstring ua = env->NewStringUTF(user_agent ? user_agent : "");
    auto out = static_cast<jbyteArray>(env->CallStaticObjectMethod(g_class, g_perform, url, body, type, ua));
    if (env->ExceptionCheck()) { env->ExceptionClear(); out = nullptr; }
    env->DeleteLocalRef(url); env->DeleteLocalRef(body); env->DeleteLocalRef(type); env->DeleteLocalRef(ua);
    if (!out) { res.error = "the request failed"; return res; }
    const jsize n = env->GetArrayLength(out);
    std::string bytes(static_cast<size_t>(n), '\0');
    if (n > 0) env->GetByteArrayRegion(out, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    env->DeleteLocalRef(out);
    if (bytes.size() < 4) { res.error = "the request failed"; return res; }
    const auto b = reinterpret_cast<const unsigned char*>(bytes.data());
    res.status = static_cast<int>((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    if (res.status == 0) res.error = bytes.substr(4);
    else res.body = bytes.substr(4);
    if (res.status == 0) __android_log_print(ANDROID_LOG_WARN, "NinDS", "cheevos: no answer from %s: %s", req.url.c_str(), res.error.c_str());
    return res;
  }
};

} // namespace

void http_jni_init(JavaVM* vm, JNIEnv* env) {
  g_vm = vm;
  jclass c = env->FindClass("com/izzy2lost/ninds/CheevosHttp");
  if (!c) { env->ExceptionClear(); __android_log_print(ANDROID_LOG_WARN, "NinDS", "cheevos: no CheevosHttp class"); return; }
  g_class = static_cast<jclass>(env->NewGlobalRef(c));
  env->DeleteLocalRef(c);
  g_perform = env->GetStaticMethodID(g_class, "perform", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B");
  if (!g_perform) env->ExceptionClear();
}

std::unique_ptr<ds::cheevos::Backend> make_jni_backend() { return std::make_unique<JniBackend>(); }

} // namespace ninds
