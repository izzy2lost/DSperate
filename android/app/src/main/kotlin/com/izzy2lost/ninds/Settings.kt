package com.izzy2lost.ninds

import android.content.Context
import androidx.preference.PreferenceDataStore
import java.io.File
import java.util.Properties

/**
 * NinDS's settings, shared by the library process and the ":emu" process.
 *
 * SharedPreferences caches per process and writes its whole map back, so a change made in one
 * process would be lost to the other. This store is a small properties file read again whenever
 * it changes on disk and written key by key, so both processes always see the latest values.
 */
class Settings private constructor(private val file: File) {
    private val lock = Any()
    private var cache = Properties()
    private var stamp = -1L to -1L

    private fun fresh(): Properties {
        val now = file.lastModified() to file.length()
        if (now != stamp) {
            val p = Properties()
            if (file.isFile) runCatching { file.inputStream().use { p.load(it) } }
            cache = p
            stamp = now
        }
        return cache
    }

    fun getString(key: String, default: String): String = synchronized(lock) { fresh().getProperty(key) ?: default }

    fun getBoolean(key: String, default: Boolean): Boolean =
        synchronized(lock) { fresh().getProperty(key)?.toBooleanStrictOrNull() ?: default }

    fun getInt(key: String, default: Int): Int = synchronized(lock) { fresh().getProperty(key)?.toIntOrNull() ?: default }

    fun put(key: String, value: Any?) = synchronized(lock) {
        val p = Properties().apply { putAll(fresh()) }
        if (value == null) p.remove(key) else p.setProperty(key, value.toString())
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.outputStream().use { p.store(it, "NinDS settings") }
        tmp.renameTo(file)
        cache = p
        stamp = file.lastModified() to file.length()
    }

    // Typed accessors. The defaults here must match res/xml/preferences.xml.
    var romTreeUri: String
        get() = getString(ROM_TREE_URI, "")
        set(value) = put(ROM_TREE_URI, value)

    val jit get() = getBoolean(JIT, true)
    val fastTiming get() = getBoolean(FAST_TIMING, true)
    val gpu3d get() = getBoolean(GPU3D, false)
    val antiAliasing get() = getBoolean(ANTI_ALIASING, false)
    val gpuScale get() = getString(GPU_SCALE, "1").toIntOrNull()?.coerceIn(1, 8) ?: 1
    val firmwareBoot get() = getBoolean(FIRMWARE_BOOT, false)
    val internet get() = getBoolean(INTERNET, false)
    val frameskip get() = getString(FRAMESKIP, "-1").toIntOrNull() ?: -1
    val fastForwardSpeed get() = getString(FF_SPEED, "3").toIntOrNull() ?: 3
    val fastForwardToggle get() = getBoolean(FF_TOGGLE, true)
    val autoSaveState get() = getBoolean(AUTO_SAVE_STATE, true)

    var layoutLandscape: String
        get() = getString(LAYOUT_LANDSCAPE, ScreenLayout.VERTICAL)
        set(value) = put(LAYOUT_LANDSCAPE, value)
    var layoutPortrait: String
        get() = getString(LAYOUT_PORTRAIT, ScreenLayout.VERTICAL)
        set(value) = put(LAYOUT_PORTRAIT, value)
    var swapScreens: Boolean
        get() = getBoolean(SWAP_SCREENS, false)
        set(value) = put(SWAP_SCREENS, value)
    val filter get() = getString(FILTER, "2").toIntOrNull() ?: 2
    val integerScale get() = getBoolean(INTEGER_SCALE, false)
    val screenGap get() = getInt(SCREEN_GAP, 0)
    val showFps get() = getBoolean(SHOW_FPS, false)
    val orientation get() = getString(ORIENTATION, "sensor")

    val audio get() = getBoolean(AUDIO, true)
    val volume get() = getInt(VOLUME, 100)

    val touchControls get() = getBoolean(TOUCH_CONTROLS, true)
    val hideWithGamepad get() = getBoolean(HIDE_WITH_GAMEPAD, true)
    val controlsOpacity get() = getInt(CONTROLS_OPACITY, 55)
    val controlsScale get() = getInt(CONTROLS_SCALE, 100)
    val haptics get() = getBoolean(HAPTICS, true)
    val stickAsDpad get() = getBoolean(STICK_AS_DPAD, true)

    val nickname get() = getString(NICKNAME, "NinDS")
    val message get() = getString(MESSAGE, "")
    val birthdayMonth get() = getString(BIRTHDAY_MONTH, "1").toIntOrNull() ?: 1
    val birthdayDay get() = getInt(BIRTHDAY_DAY, 1)
    val favouriteColour get() = getString(COLOUR, "0").toIntOrNull() ?: 0
    val language get() = getString(LANGUAGE, "1").toIntOrNull() ?: 1
    val theme get() = getString(THEME, "system")

    /** For androidx.preference screens: every Preference reads and writes through this store. */
    val dataStore: PreferenceDataStore = object : PreferenceDataStore() {
        override fun getString(key: String, defValue: String?) = this@Settings.getString(key, defValue ?: "")
        override fun putString(key: String, value: String?) = put(key, value)
        override fun getBoolean(key: String, defValue: Boolean) = this@Settings.getBoolean(key, defValue)
        override fun putBoolean(key: String, value: Boolean) = put(key, value)
        override fun getInt(key: String, defValue: Int) = this@Settings.getInt(key, defValue)
        override fun putInt(key: String, value: Int) = put(key, value)
    }

    companion object {
        const val ROM_TREE_URI = "rom_tree_uri"
        const val JIT = "jit"
        const val FAST_TIMING = "fast_timing"
        const val GPU3D = "gpu3d"
        const val ANTI_ALIASING = "anti_aliasing"
        const val GPU_SCALE = "gpu_scale"
        const val FIRMWARE_BOOT = "firmware_boot"
        const val INTERNET = "internet"
        const val FRAMESKIP = "frameskip"
        const val FF_SPEED = "ff_speed"
        const val FF_TOGGLE = "ff_toggle"
        const val AUTO_SAVE_STATE = "auto_save_state"
        const val LAYOUT_LANDSCAPE = "layout_landscape"
        const val LAYOUT_PORTRAIT = "layout_portrait"
        const val SWAP_SCREENS = "swap_screens"
        const val FILTER = "filter"
        const val INTEGER_SCALE = "integer_scale"
        const val SCREEN_GAP = "screen_gap"
        const val SHOW_FPS = "show_fps"
        const val ORIENTATION = "orientation"
        const val AUDIO = "audio"
        const val VOLUME = "volume"
        const val TOUCH_CONTROLS = "touch_controls"
        const val HIDE_WITH_GAMEPAD = "hide_with_gamepad"
        const val CONTROLS_OPACITY = "controls_opacity"
        const val CONTROLS_SCALE = "controls_scale"
        const val HAPTICS = "haptics"
        const val STICK_AS_DPAD = "stick_as_dpad"
        const val NICKNAME = "nickname"
        const val MESSAGE = "message"
        const val BIRTHDAY_MONTH = "birthday_month"
        const val BIRTHDAY_DAY = "birthday_day"
        const val COLOUR = "colour"
        const val LANGUAGE = "language"
        const val THEME = "theme"

        @Volatile
        private var instance: Settings? = null

        fun get(context: Context): Settings =
            instance ?: synchronized(this) {
                instance ?: Settings(File(context.applicationContext.filesDir, "settings.properties")).also { instance = it }
            }
    }
}
