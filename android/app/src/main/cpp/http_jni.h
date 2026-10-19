// SPDX-License-Identifier: GPL-3.0-or-later
// NinDS - RetroAchievements' HTTP through Java: Android has no libcurl, so the
// core's Backend is served by com.izzy2lost.ninds.CheevosHttp (HttpURLConnection).
#pragma once
#include "cheevos/cheevos_http.h"

#include <jni.h>
#include <memory>

namespace ninds {

// Call from JNI_OnLoad: caches the VM and the Java class (a worker thread's
// FindClass would not see the app's classes).
void http_jni_init(JavaVM* vm, JNIEnv* env);
std::unique_ptr<ds::cheevos::Backend> make_jni_backend();

} // namespace ninds
