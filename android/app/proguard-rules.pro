# Release shrinking rules.
#
# The default proguard-android-optimize.txt keeps `native <methods>` and their declaring
# classes, so the `external fun` declarations in NativeLib are safe without extra rules.

# Resolved from C++ by name via GetMethodID, so R8 cannot see the call sites.
# See ninds_jni.cpp.
-keep class com.izzy2lost.ninds.NativeLib { *; }
-keep interface com.izzy2lost.ninds.NativeLib$Callbacks { *; }
-keep class * implements com.izzy2lost.ninds.NativeLib$Callbacks { *; }
