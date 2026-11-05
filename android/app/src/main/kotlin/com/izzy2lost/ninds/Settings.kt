package com.izzy2lost.ninds

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
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
    /** off, on or enhanced (the edge cut). Before this list it was the ANTI_ALIASING switch. */
    val aaMode get() = getString(AA_MODE, if (getBoolean(ANTI_ALIASING, false)) "on" else "off")
    /** For nativeBoot: 0 off, 1 on, 2 enhanced. */
    val aaLevel get() = when (aaMode) { "on" -> 1; "enhanced" -> 2; else -> 0 }
    val texFilter get() = getBoolean(TEX_FILTER, false)
    val gpuScale get() = getString(GPU_SCALE, "1").toIntOrNull()?.coerceIn(1, 8) ?: 1
    val firmwareBoot get() = getBoolean(FIRMWARE_BOOT, false)
    /** What a second display (dual-screen device or external monitor) shows: SECOND_*. */
    val secondDisplay get() = getString(SECOND_DISPLAY, SECOND_BOTTOM)
    /** off, internet, auto, host, guest or join. Before this list it was the INTERNET switch. */
    val netMode get() = getString(NET_MODE, if (getBoolean(INTERNET, false)) "internet" else "off")
    val lanAddress get() = getString(LAN_ADDRESS, "").trim()
    val isLocalWireless get() = netMode in setOf("auto", "host", "guest", "join")
    val frameskip get() = getString(FRAMESKIP, "-1").toIntOrNull() ?: -1
    val fastForwardSpeed get() = getString(FF_SPEED, "3").toIntOrNull() ?: 3
    val fastForwardToggle get() = getBoolean(FF_TOGGLE, true)
    val autoSaveState get() = getBoolean(AUTO_SAVE_STATE, true)

    var layoutLandscape: String
        get() = getString(LAYOUT_LANDSCAPE, ScreenLayout.HYBRID)
        set(value) = put(LAYOUT_LANDSCAPE, value)
    var layoutPortrait: String
        get() = getString(LAYOUT_PORTRAIT, ScreenLayout.VERTICAL)
        set(value) = put(LAYOUT_PORTRAIT, value)
    var swapScreens: Boolean
        get() = getBoolean(SWAP_SCREENS, false)
        set(value) = put(SWAP_SCREENS, value)
    val filter get() = getString(FILTER, "2").toIntOrNull() ?: 2
    val integerScale get() = getBoolean(INTEGER_SCALE, false)
    val screenGap get() = getInt(SCREEN_GAP, 5)
    val showFps get() = getBoolean(SHOW_FPS, false)
    val orientation get() = getString(ORIENTATION, "sensor")

    val audio get() = getBoolean(AUDIO, true)
    val volume get() = getInt(VOLUME, 100)
    /** off, blow (the MIC button only) or phone (the real microphone; the button still blows). */
    val micMode get() = getString(MIC_MODE, "blow")

    val touchControls get() = getBoolean(TOUCH_CONTROLS, true)
    val hideWithGamepad get() = getBoolean(HIDE_WITH_GAMEPAD, true)
    val controlsOpacity get() = getInt(CONTROLS_OPACITY, 55)
    val controlsScale get() = getInt(CONTROLS_SCALE, 100)
    val haptics get() = getBoolean(HAPTICS, true)
    val stickAsDpad get() = getBoolean(STICK_AS_DPAD, true)
    val analogControls get() = getBoolean(ANALOG_CONTROLS, true)

    /** auto (when something can vibrate), on or off. */
    val rumble get() = getString(RUMBLE, "auto")
    val guitarGrip get() = getBoolean(GUITAR_GRIP, true)
    /** The GBA game a DS game was given for slot 2: a file name in AppPaths.gba, NO_GBA, or "" (never asked). */
    fun gbaFor(stem: String) = getString(GBA_PREFIX + stem, "")
    fun setGbaFor(stem: String, value: String) = put(GBA_PREFIX + stem, value)

    val nickname get() = getString(NICKNAME, "NinDS")
    val message get() = getString(MESSAGE, "")
    val birthdayMonth get() = getString(BIRTHDAY_MONTH, "1").toIntOrNull() ?: 1
    val birthdayDay get() = getInt(BIRTHDAY_DAY, 1)
    val favouriteColour get() = getString(COLOUR, "0").toIntOrNull() ?: 0
    val language get() = getString(LANGUAGE, "1").toIntOrNull() ?: 1
    val theme get() = getString(THEME, "system")
    val accent get() = getString(ACCENT, "red")
    val controlsSkin get() = getString(CONTROLS_SKIN, ControlSkins.CLASSIC)
    val faceColours get() = getBoolean(FACE_COLOURS, false)

    /**
     * Everything a game only reads when it starts (the emulator, the 3D renderer, the network,
     * achievements, the console's owner settings). A change shows up here as a different string.
     */
    fun bootFingerprint(): String = listOf(
        jit, fastTiming, gpu3d, gpuScale, texFilter, aaMode, rumble, guitarGrip, analogControls, firmwareBoot, netMode, lanAddress,
        cheevosEnabled, cheevosUser, cheevosToken, nickname, message, birthdayMonth, birthdayDay, favouriteColour, language,
    ).joinToString("|")

    // RetroAchievements: the token from sign-in stands for the password, which is never kept.
    val cheevosUser get() = getString(CHEEVOS_USER, "")
    val cheevosToken get() = getString(CHEEVOS_TOKEN, "")
    val cheevosDisplayName get() = getString(CHEEVOS_DISPLAY_NAME, "").ifEmpty { cheevosUser }
    val cheevosSignedIn get() = cheevosUser.isNotEmpty() && cheevosToken.isNotEmpty()
    val cheevosEnabled get() = getBoolean(CHEEVOS_ENABLED, true)
    val cheevosNotifications get() = getBoolean(CHEEVOS_NOTIFICATIONS, true)

    fun cheevosSignIn(user: String, token: String, displayName: String) {
        put(CHEEVOS_USER, user); put(CHEEVOS_TOKEN, token); put(CHEEVOS_DISPLAY_NAME, displayName)
    }

    fun cheevosSignOut() {
        put(CHEEVOS_USER, null); put(CHEEVOS_TOKEN, null); put(CHEEVOS_DISPLAY_NAME, null)
    }

    var libraryTipDismissed: Boolean
        get() = getBoolean(LIBRARY_TIP_DISMISSED, false)
        set(value) = put(LIBRARY_TIP_DISMISSED, value)

    var libraryCovers: Boolean
        get() = getBoolean(LIBRARY_COVERS, false)
        set(value) = put(LIBRARY_COVERS, value)

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
        const val AA_MODE = "aa_mode"
        const val RUMBLE = "rumble"
        const val GUITAR_GRIP = "guitar_grip"
        const val ANALOG_CONTROLS = "analog_controls"
        const val GBA_PREFIX = "gba_slot_"
        const val NO_GBA = "none"
        const val GPU_SCALE = "gpu_scale"
        const val TEX_FILTER = "tex_filter"
        const val FIRMWARE_BOOT = "firmware_boot"
        const val INTERNET = "internet"
        const val NET_MODE = "net_mode"
        const val LAN_ADDRESS = "lan_address"
        const val FRAMESKIP = "frameskip"
        const val FF_SPEED = "ff_speed"
        const val FF_TOGGLE = "ff_toggle"
        const val AUTO_SAVE_STATE = "auto_save_state"
        const val LAYOUT_LANDSCAPE = "layout_landscape"
        const val SECOND_DISPLAY = "second_display"
        const val SECOND_BOTTOM = "bottom"   // the bottom screen there, the top one on the device
        const val SECOND_TOP = "top"         // the top screen there, the bottom one (touch) on the device
        const val SECOND_BOTH = "both"       // both screens on both
        const val SECOND_OFF = "off"
        const val LAYOUT_PORTRAIT = "layout_portrait"
        const val SWAP_SCREENS = "swap_screens"
        const val FILTER = "filter"
        const val INTEGER_SCALE = "integer_scale"
        const val SCREEN_GAP = "screen_gap"
        const val SHOW_FPS = "show_fps"
        const val ORIENTATION = "orientation"
        const val AUDIO = "audio"
        const val VOLUME = "volume"
        const val MIC_MODE = "mic_mode"
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
        const val ACCENT = "accent"
        const val CONTROLS_SKIN = "controls_skin"
        const val FACE_COLOURS = "face_colours"
        const val LIBRARY_COVERS = "library_covers"
        const val LIBRARY_TIP_DISMISSED = "library_tip_dismissed"
        const val CHEEVOS_USER = "cheevos_user"
        const val CHEEVOS_TOKEN = "cheevos_token"
        const val CHEEVOS_DISPLAY_NAME = "cheevos_display_name"
        const val CHEEVOS_ENABLED = "cheevos_enabled"
        const val CHEEVOS_NOTIFICATIONS = "cheevos_notifications"

        @Volatile
        private var instance: Settings? = null

        fun get(context: Context): Settings =
            instance ?: synchronized(this) {
                instance ?: Settings(File(context.applicationContext.filesDir, "settings.properties")).also { instance = it }
            }

        /**
         * Whether the device has the Vulkan 1.1 the GPU 3D renderer needs. Without it, GPU 3D,
         * 3D resolution and smooth textures are unavailable: 3D is drawn on the CPU at 1x.
         */
        fun hasVulkan(context: Context): Boolean =
            context.packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, 0x401000)

        /** OpenGL ES 3.1, which the GPU 3D renderer falls back to without Vulkan (native resolution only). */
        fun hasGles31(context: Context): Boolean =
            (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).deviceConfigurationInfo.reqGlEsVersion >= 0x30001

        /** GPU 3D at all: on Vulkan, or on OpenGL ES 3.1 at native resolution. */
        fun hasGpu3d(context: Context) = hasVulkan(context) || hasGles31(context)
    }
}
