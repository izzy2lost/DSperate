# NinDS (Android)

The Android frontend for the DSperate core: package `com.izzy2lost.ninds`, arm64-v8a only.

Toolchain: AGP 9.2.1 (built-in Kotlin), Gradle 9.4.1, NDK 30.0.15729638, CMake 3.30.3,
compile/target SDK 36, min SDK 28, JVM 17.

    cd android
    ./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
    ./gradlew assembleRelease      # unsigned; sign as usual, or Generate Signed Bundle in Studio

The native library (`app/src/main/cpp`) builds the core straight from this repository's
`src/` tree, so there is nothing to fetch. Debug builds compile the native code with
optimisation too, since the emulator is unusably slow at -O0.

Layout:

- `cpp/session.*`: the machine, its emulator thread, pacing, frameskip, saves, states, cheats
- `cpp/presenter_gl.*`: GLES 3 presenter on its own vsync'd thread (nearest, bilinear, sharp bilinear, LCD grid)
- `cpp/audio_out.*`: AAudio output, resampling the DS's 32728.5 Hz with drift correction
- `kotlin/`: library (SAF folder scan, banner icons), emulator activity (runs in the `:emu`
  process, ended after each game), on-screen controls, controller mapping, settings

Settings live in `files/settings.properties`, not SharedPreferences, so the library process
and the `:emu` process always agree.
