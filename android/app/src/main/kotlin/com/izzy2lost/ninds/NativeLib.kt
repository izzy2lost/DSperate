package com.izzy2lost.ninds

import android.view.Surface

/**
 * The emulator core (libninds.so). Only the ":emu" process loads it; one game runs per
 * process, so these act on a single global session.
 */
object NativeLib {
    init {
        System.loadLibrary("ninds")
    }

    /** Returns an empty string on success, else why the game could not start. */
    external fun nativeBoot(
        romFd: Int,
        romName: String,
        savePath: String,
        cacheDir: String,
        biosDir: String,
        firmwareBoot: Boolean,
        jit: Boolean,
        fastTiming: Boolean,
        gpu3d: Boolean,
        gpuScale: Int,
        texFilter: Boolean,
        antiAliasing: Boolean,
        netMode: String,
        lanAddress: String,
        audio: Boolean,
        volume: Int,
        nickname: String,
        message: String,
        birthdayMonth: Int,
        birthdayDay: Int,
        colour: Int,
        language: Int,
    ): String

    /** How the game was started, when the player asked for something else (the DS menu); else empty. */
    external fun nativeBootNote(): String

    /** Local wireless: empty when not in a session, else who we are in it. */
    external fun nativeLanStatus(): String

    external fun nativeStart()
    external fun nativeShutdown()
    external fun nativeSetSurface(surface: Surface?)
    external fun nativeSetLayout(
        tx: Int, ty: Int, tw: Int, th: Int, topShown: Boolean,
        bx: Int, by: Int, bw: Int, bh: Int, bottomShown: Boolean,
    )
    external fun nativeSetFilter(filter: Int)
    external fun nativeSetPaused(paused: Boolean)
    external fun nativeSetButtons(mask: Int)
    external fun nativeSetTouch(x: Int, y: Int, down: Boolean)
    external fun nativeSetLid(closed: Boolean)
    external fun nativeSetMic(blowing: Boolean)
    /** 0 = off, 1 = the blow button only, 2 = the phone's microphone. */
    external fun nativeSetMicMode(mode: Int)
    external fun nativeSetFastForward(on: Boolean)
    external fun nativeSetFastForwardSpeed(multiplier: Int)
    external fun nativeSetFrameskip(frames: Int)
    external fun nativeSetVolume(percent: Int)
    external fun nativeSetAudioEnabled(on: Boolean)
    /** Both return an empty string on success, else a message for the player. */
    external fun nativeSaveState(path: String): String
    external fun nativeLoadState(path: String): String
    external fun nativeReset()
    external fun nativeFlushSave(): Boolean
    external fun nativeGetFps(): Float
    external fun nativeGameCode(): String
    /** Top screen above bottom, 256x384 opaque ARGB. */
    external fun nativeGetFrame(): IntArray?
    external fun nativeSetCheats(names: Array<String>, words: Array<IntArray>)
    /** {name, description, code} triples; a single element is an error message. */
    external fun nativeDatabaseCheats(dbPath: String): Array<String>
}

/** Bit numbers of the DS keypad, as the core's Io::Button enum numbers them. */
object DsButton {
    const val A = 0
    const val B = 1
    const val SELECT = 2
    const val START = 3
    const val RIGHT = 4
    const val LEFT = 5
    const val UP = 6
    const val DOWN = 7
    const val R = 8
    const val L = 9
    const val X = 10
    const val Y = 11
}
