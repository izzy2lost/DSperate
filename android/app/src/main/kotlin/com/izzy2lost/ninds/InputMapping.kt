package com.izzy2lost.ninds

import android.view.InputDevice
import android.view.KeyEvent

/** Everything a physical button can be bound to: the DS keypad, then emulator hotkeys. */
enum class InputAction(val label: String, val dsBit: Int = -1) {
    A("A", DsButton.A),
    B("B", DsButton.B),
    X("X", DsButton.X),
    Y("Y", DsButton.Y),
    L("L", DsButton.L),
    R("R", DsButton.R),
    START("Start", DsButton.START),
    SELECT("Select", DsButton.SELECT),
    UP("D-Pad Up", DsButton.UP),
    DOWN("D-Pad Down", DsButton.DOWN),
    LEFT("D-Pad Left", DsButton.LEFT),
    RIGHT("D-Pad Right", DsButton.RIGHT),
    MENU("Open menu"),
    FAST_FORWARD("Fast forward"),
    SWAP_SCREENS("Swap screens"),
    CYCLE_LAYOUT("Next screen layout"),
    QUICK_SAVE("Quick save"),
    QUICK_LOAD("Quick load"),
    MICROPHONE("Blow into microphone"),
    LID("Close / open lid"),
    SCREENSHOT("Screenshot");

    val isDsButton get() = dsBit >= 0
    val settingsKey get() = "map_" + name.lowercase()
}

/**
 * Key bindings for gamepads and keyboards. The face buttons map by position, as Nintendo's own
 * layout has them: the bottom button is B and the right one A, whatever the pad prints on them.
 */
class InputMapping(private val settings: Settings) {
    private val bindings = HashMap<InputAction, MutableSet<Int>>()
    private val byKey = HashMap<Int, MutableList<InputAction>>()

    init {
        reload()
    }

    fun reload() {
        bindings.clear()
        for (action in InputAction.entries) {
            val stored = settings.getString(action.settingsKey, "\u0000")
            val keys = if (stored == "\u0000") DEFAULTS[action].orEmpty().toMutableSet()
            else stored.split(',').mapNotNull { it.trim().toIntOrNull() }.toMutableSet()
            bindings[action] = keys
        }
        rebuild()
    }

    private fun rebuild() {
        byKey.clear()
        for ((action, keys) in bindings) for (k in keys) byKey.getOrPut(k) { mutableListOf() } += action
    }

    fun actionsFor(keyCode: Int): List<InputAction> = byKey[keyCode].orEmpty()

    fun keysFor(action: InputAction): Set<Int> = bindings[action].orEmpty()

    /** Binds `keyCode` to `action` alone: a key does one thing. */
    fun bind(action: InputAction, keyCode: Int) {
        for ((other, keys) in bindings) if (other != action && keys.remove(keyCode)) save(other)
        bindings.getOrPut(action) { mutableSetOf() } += keyCode
        save(action)
        rebuild()
    }

    fun clear(action: InputAction) {
        bindings[action] = mutableSetOf()
        save(action)
        rebuild()
    }

    fun resetDefaults() {
        for (action in InputAction.entries) settings.put(action.settingsKey, null)
        reload()
    }

    private fun save(action: InputAction) = settings.put(action.settingsKey, bindings[action].orEmpty().joinToString(","))

    companion object {
        val DEFAULTS: Map<InputAction, Set<Int>> = mapOf(
            InputAction.A to setOf(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_X),
            InputAction.B to setOf(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_Z),
            InputAction.X to setOf(KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_S),
            InputAction.Y to setOf(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_A),
            InputAction.L to setOf(KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_Q),
            InputAction.R to setOf(KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_W),
            InputAction.START to setOf(KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER),
            InputAction.SELECT to setOf(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_SHIFT_RIGHT),
            InputAction.UP to setOf(KeyEvent.KEYCODE_DPAD_UP),
            InputAction.DOWN to setOf(KeyEvent.KEYCODE_DPAD_DOWN),
            InputAction.LEFT to setOf(KeyEvent.KEYCODE_DPAD_LEFT),
            InputAction.RIGHT to setOf(KeyEvent.KEYCODE_DPAD_RIGHT),
            InputAction.MENU to setOf(KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_ESCAPE),
            InputAction.FAST_FORWARD to setOf(KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_TAB),
            InputAction.SWAP_SCREENS to setOf(KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_F1),
            InputAction.CYCLE_LAYOUT to setOf(KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_F2),
            InputAction.QUICK_SAVE to setOf(KeyEvent.KEYCODE_F5),
            InputAction.QUICK_LOAD to setOf(KeyEvent.KEYCODE_F7),
            InputAction.MICROPHONE to setOf(KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_M),
            InputAction.LID to setOf(KeyEvent.KEYCODE_F3),
            InputAction.SCREENSHOT to setOf(KeyEvent.KEYCODE_F12),
        )

        fun keyName(keyCode: Int): String =
            KeyEvent.keyCodeToString(keyCode).removePrefix("KEYCODE_").replace('_', ' ')
                .lowercase().replaceFirstChar { it.uppercase() }

        fun isGamepad(device: InputDevice?): Boolean {
            if (device == null || device.isVirtual) return false
            val s = device.sources
            return (s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        }
    }
}
