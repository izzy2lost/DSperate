package com.izzy2lost.ninds

import android.view.Surface

/**
 * The emulator core (libninds.so). The ":emu" process runs games with it, one per process, so
 * these act on a single global session. Settings loads it only to sign in to RetroAchievements.
 */
object NativeLib {
    const val MAIN_DISPLAY = 0
    const val SECOND_DISPLAY = 1

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
        /** 0 off, 1 on, 2 enhanced (the edge cut). */
        antiAliasing: Int,
        rumble: Boolean,
        guitarGrip: Boolean,
        analog: Boolean,
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
        cheevosUser: String,
        cheevosToken: String,
    ): String

    /** How the game was started, when the player asked for something else (the DS menu); else empty. */
    external fun nativeBootNote(): String

    /** RetroAchievements messages to show: kind, title, detail, points, badge URL, separated by \u001f. */
    external fun nativeCheevosMessages(): Array<String>
    /** Header row (state, user, game, game badge, unlocked, total, points, total points), then one row per achievement. */
    external fun nativeCheevosList(): Array<String>
    /** Blocking sign-in for Settings: {user, token, display name, score}, or {"", error}. */
    external fun nativeCheevosLogin(user: String, password: String): Array<String>

    /** Local wireless: empty when not in a session, else who we are in it. */
    external fun nativeLanStatus(): String

    external fun nativeStart()
    external fun nativeShutdown()
    /** [display]: [MAIN_DISPLAY], or [SECOND_DISPLAY] (dual-screen device or external monitor). */
    external fun nativeSetSurface(display: Int, surface: Surface?)
    external fun nativeSetLayout(
        display: Int,
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
    /** {name, description, code} triples, the built-in analog controls codes first; a single element is an error message. */
    external fun nativeDatabaseCheats(dbPath: String): Array<String>

    /** A GBA game into slot 2 (empty paths: none). Empty on success, else why not. */
    external fun nativeSetSlot2Gba(rom: String, save: String): String
    /** What is in slot 2: [SLOT2_EMPTY], [SLOT2_GBA], [SLOT2_RUMBLE] or [SLOT2_GRIP]. */
    external fun nativeSlot2Kind(): Int
    /** Whether the running game reads a GBA game in slot 2. */
    external fun nativeReadsGba(): Boolean
    /** The GBA ROMs among [paths] that the running game reads. */
    external fun nativeGbaOffers(paths: Array<String>): Array<String>
    /** Rumble Pak motor changes so far; a change is a pulse. */
    external fun nativeRumbleEvents(): Int
    /** Guitar Grip frets held: green 1, red 2, yellow 4, blue 8. */
    external fun nativeSetFrets(mask: Int)
    external fun nativeStrum()
    /** Bits 0-1: 0 no analog controls cheat running, 1 one is, 2 several are on at once; bit 2 ([ANALOG_FULL]): it takes the right stick and ZL / ZR too. */
    external fun nativeAnalogState(): Int
    /** Sticks in -32768..32767 (y down), and ZL / ZR. */
    external fun nativeSetAnalog(lx: Int, ly: Int, rx: Int, ry: Int, zl: Boolean, zr: Boolean)

    const val ANALOG_FULL = 4
    const val SLOT2_EMPTY = 0
    const val SLOT2_GBA = 1
    const val SLOT2_RUMBLE = 2
    const val SLOT2_GRIP = 3
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
