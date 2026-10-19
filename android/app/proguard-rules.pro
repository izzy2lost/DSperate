# Release shrinking rules.
#
# The default proguard-android-optimize.txt keeps `native <methods>` and their declaring
# classes, so the `external fun` declarations in NativeLib are safe without extra rules.

# Resolved from C++ by name via GetMethodID, so R8 cannot see the call sites.
# See ninds_jni.cpp.
-keep class com.izzy2lost.ninds.NativeLib { *; }
-keep interface com.izzy2lost.ninds.NativeLib$Callbacks { *; }
-keep class * implements com.izzy2lost.ninds.NativeLib$Callbacks { *; }

# RetroAchievements' HTTP: FindClass + GetStaticMethodID from native code (cpp/http_jni.cpp),
# so R8 sees no caller and would remove or rename it ("no Java transport").
-keep class com.izzy2lost.ninds.CheevosHttp {
    public static byte[] perform(java.lang.String, java.lang.String, java.lang.String, java.lang.String);
}
