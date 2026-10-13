package com.izzy2lost.ninds

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.graphics.Bitmap
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process
import android.provider.MediaStore
import android.text.format.DateUtils
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.izzy2lost.ninds.databinding.ActivityEmulatorBinding
import java.io.File
import kotlin.math.abs

/**
 * Runs one game. Lives in the ":emu" process, which is ended when the game is closed so the
 * next game starts from a clean slate (the recompiler and guest memory are process-wide).
 */
class EmulatorActivity : AppCompatActivity(), SurfaceHolder.Callback, InputManager.InputDeviceListener {
    private lateinit var binding: ActivityEmulatorBinding
    private lateinit var settings: Settings
    private lateinit var paths: AppPaths
    private lateinit var mapping: InputMapping
    private lateinit var controls: TouchControlsView
    private lateinit var cheatStore: CheatStore
    private val handler = Handler(Looper.getMainLooper())

    private var romFd: ParcelFileDescriptor? = null
    private lateinit var stem: String
    private lateinit var gameTitle: String
    private var booted = false
    private var exiting = false
    private var surfaceReady = false

    private var activityPaused = false
    private var dialogHolds = 0
    private var currentDialog: AlertDialog? = null

    private var touchMask = 0
    private val heldKeys = HashSet<Int>()
    private var padMask = 0
    private var stickMask = 0
    private var fastForward = false
    private var fastForwardHeld = false
    private var lidClosed = false

    private val fpsTicker = object : Runnable {
        override fun run() {
            if (booted && settings.showFps) {
                binding.fpsText.text = getString(R.string.fps_format, NativeLib.nativeGetFps())
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings.get(this)
        paths = AppPaths(this)
        mapping = InputMapping(settings)
        binding = ActivityEmulatorBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val uri = intent.getStringExtra(EXTRA_URI)
        val fileName = intent.getStringExtra(EXTRA_FILE_NAME) ?: "game.nds"
        if (uri == null) { finish(); return }
        stem = AppPaths.stemOf(fileName)
        gameTitle = intent.getStringExtra(EXTRA_TITLE) ?: stem
        cheatStore = CheatStore(paths.cheatFile(stem))

        applyOrientation()
        goImmersive()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        binding.surface.holder.addCallback(this)
        controls = TouchControlsView(this).apply {
            onButtons = { touchMask = it; pushButtons() }
            onTouchScreen = { x, y, down -> NativeLib.nativeSetTouch(x, y, down) }
            onAction = { action, down -> handleAction(action, down) }
        }
        binding.root.addView(controls, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        binding.fpsText.bringToFront()
        binding.bootOverlay.bringToFront()
        binding.root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updateLayout() }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = showMenu()
        })
        getSystemService(InputManager::class.java).registerInputDeviceListener(this, handler)

        applyRuntimeSettings()
        boot(Uri.parse(uri), fileName, intent.getIntExtra(EXTRA_LOAD_SLOT, -1))
    }

    private fun boot(uri: Uri, fileName: String, loadSlot: Int) {
        binding.bootText.text = getString(R.string.boot_loading, gameTitle)
        Thread({
            val error = runCatching {
                val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return@runCatching getString(R.string.error_open_rom)
                romFd = pfd
                NativeLib.nativeBoot(
                    pfd.fd, fileName, paths.savFile(stem).path, paths.cache.path, paths.bios.path,
                    settings.firmwareBoot, settings.jit, settings.fastTiming, settings.gpu3d, settings.gpuScale, settings.texFilter, settings.antiAliasing,
                    settings.internet, settings.audio, settings.volume, settings.nickname, settings.message,
                    settings.birthdayMonth, settings.birthdayDay, settings.favouriteColour, settings.language,
                )
            }.getOrElse { it.message ?: getString(R.string.error_open_rom) }
            var stateMessage: String? = null
            if (error.isEmpty()) {
                cheatStore.apply(cheatStore.load())
                if (loadSlot >= 0) {
                    val slot = SaveStateSlot(paths.stateDir(stem), loadSlot)
                    stateMessage = NativeLib.nativeLoadState(slot.state.path).ifEmpty { null }
                }
            }
            runOnUiThread { onBooted(error, stateMessage) }
        }, "NinDS boot").start()
    }

    private fun onBooted(error: String, stateMessage: String?) {
        if (isFinishing || isDestroyed) return
        if (error.isNotEmpty()) {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.error_boot_title)
                .setMessage(error)
                .setPositiveButton(android.R.string.ok) { _, _ -> exitNow(save = false) }
                .setCancelable(false)
                .show()
            return
        }
        booted = true
        binding.bootOverlay.visibility = View.GONE
        applyRuntimeSettings()
        NativeLib.nativeStart()
        if (surfaceReady) NativeLib.nativeSetSurface(binding.surface.holder.surface)
        updateLayout()
        updatePause()
        stateMessage?.let { Toast.makeText(this, it, Toast.LENGTH_LONG).show() }
        handler.post(fpsTicker)
    }

    // ---- Lifecycle ------------------------------------------------------------------------

    override fun onResume() {
        super.onResume()
        activityPaused = false
        mapping.reload()
        applyRuntimeSettings()
        goImmersive()
        updatePause()
    }

    override fun onPause() {
        activityPaused = true
        updatePause()
        if (booted) NativeLib.nativeFlushSave()
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        getSystemService(InputManager::class.java).unregisterInputDeviceListener(this)
        super.onDestroy()
        if (isFinishing) {
            if (!exiting) shutdownNative(save = true)
            // One game per process: the next one gets fresh JIT and guest-memory state.
            Process.killProcess(Process.myPid())
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) goImmersive()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        controls.relayout()
        updateLayout()
    }

    private fun applyOrientation() {
        requestedOrientation = when (settings.orientation) {
            "landscape" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            "portrait" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            else -> ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
        }
    }

    private fun goImmersive() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    /** Settings that can change while the game runs (from the menu or the settings screen). */
    private fun applyRuntimeSettings() {
        applyOrientation()
        controls.opacity = settings.controlsOpacity / 100f
        controls.scale = settings.controlsScale / 100f
        controls.haptics = settings.haptics
        updateControlsVisibility()
        binding.fpsText.visibility = if (settings.showFps) View.VISIBLE else View.GONE
        if (!booted) return
        NativeLib.nativeSetFilter(settings.filter)
        NativeLib.nativeSetFrameskip(settings.frameskip)
        NativeLib.nativeSetFastForwardSpeed(settings.fastForwardSpeed)
        NativeLib.nativeSetVolume(settings.volume)
        NativeLib.nativeSetAudioEnabled(settings.audio)
        updateLayout()
    }

    private fun updatePause() {
        val paused = !booted || activityPaused || dialogHolds > 0
        controls.suspended = paused
        if (booted) NativeLib.nativeSetPaused(paused)
        if (paused) { heldKeys.clear(); padMask = 0; stickMask = 0; touchMask = 0; pushButtons() }
    }

    // ---- Surface and layout -----------------------------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceReady = true
        if (booted) NativeLib.nativeSetSurface(holder.surface)
        updateLayout()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        if (booted) NativeLib.nativeSetSurface(null)
    }

    private fun isPortrait() = binding.root.height > binding.root.width

    private fun currentLayoutMode() = if (isPortrait()) settings.layoutPortrait else settings.layoutLandscape

    private fun updateLayout() {
        val w = binding.surface.width
        val h = binding.surface.height
        if (w <= 0 || h <= 0) return
        val portrait = h > w
        val reserve = if (portrait && controls.controlsVisible) (h * 0.40f).toInt() else 0
        val gap = (settings.screenGap * resources.displayMetrics.density).toInt()
        val p = ScreenLayout.compute(w, h, currentLayoutMode(), settings.swapScreens, settings.integerScale, gap, reserve)
        controls.portraitBandTop = if (portrait) h - reserve else 0
        controls.touchScreenRect = if (p.bottomShown) p.bottom else null
        controls.relayout()
        if (booted) {
            NativeLib.nativeSetLayout(
                p.top.left, p.top.top, p.top.width(), p.top.height(), p.topShown,
                p.bottom.left, p.bottom.top, p.bottom.width(), p.bottom.height(), p.bottomShown,
            )
        }
    }

    // ---- Input --------------------------------------------------------------------------------

    private fun pushButtons() {
        if (!booted) return
        NativeLib.nativeSetButtons(touchMask or padMask or stickMask)
    }

    private fun updateControlsVisibility() {
        val padConnected = InputDevice.getDeviceIds().any { InputMapping.isGamepad(InputDevice.getDevice(it)) }
        controls.controlsVisible = settings.touchControls && !(settings.hideWithGamepad && padConnected)
        updateLayout()
    }

    override fun onInputDeviceAdded(deviceId: Int) = updateControlsVisibility()
    override fun onInputDeviceRemoved(deviceId: Int) = updateControlsVisibility()
    override fun onInputDeviceChanged(deviceId: Int) = Unit

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val code = event.keyCode
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_MUTE) {
            return super.dispatchKeyEvent(event)
        }
        if (currentDialog != null || !booted) return super.dispatchKeyEvent(event)
        val actions = mapping.actionsFor(code)
        if (actions.isEmpty()) {
            // An unbound gamepad B still reads as Back to Android; treat Back as the menu key.
            if (code == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) showMenu()
            return code == KeyEvent.KEYCODE_BACK || super.dispatchKeyEvent(event)
        }
        val down = event.action == KeyEvent.ACTION_DOWN
        if (down && event.repeatCount > 0) return true
        if (down) heldKeys += code else heldKeys -= code
        for (a in actions) {
            if (a.isDsButton) continue
            handleAction(a, down)
        }
        recomputePadMask()
        return true
    }

    private fun recomputePadMask() {
        var m = 0
        for (k in heldKeys) for (a in mapping.actionsFor(k)) if (a.isDsButton) m = m or (1 shl a.dsBit)
        padMask = m
        pushButtons()
    }

    // Analog triggers arrive as axes on many pads; they act like the L2/R2 keys.
    private var triggerL = false
    private var triggerR = false

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val joystick = event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (!joystick || event.action != MotionEvent.ACTION_MOVE || !booted || currentDialog != null) {
            return super.dispatchGenericMotionEvent(event)
        }
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        var x = hatX
        var y = hatY
        if (settings.stickAsDpad && abs(hatX) < 0.5f && abs(hatY) < 0.5f) {
            x = event.getAxisValue(MotionEvent.AXIS_X)
            y = event.getAxisValue(MotionEvent.AXIS_Y)
        }
        var m = 0
        if (x <= -0.5f) m = m or (1 shl DsButton.LEFT)
        if (x >= 0.5f) m = m or (1 shl DsButton.RIGHT)
        if (y <= -0.5f) m = m or (1 shl DsButton.UP)
        if (y >= 0.5f) m = m or (1 shl DsButton.DOWN)
        if (m != stickMask) { stickMask = m; pushButtons() }

        val l = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)) > 0.5f
        val r = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)) > 0.5f
        if (l != triggerL) { triggerL = l; synthesizeKey(KeyEvent.KEYCODE_BUTTON_L2, l) }
        if (r != triggerR) { triggerR = r; synthesizeKey(KeyEvent.KEYCODE_BUTTON_R2, r) }
        return true
    }

    private fun synthesizeKey(code: Int, down: Boolean) {
        val actions = mapping.actionsFor(code)
        if (down) heldKeys += code else heldKeys -= code
        for (a in actions) if (!a.isDsButton) handleAction(a, down)
        recomputePadMask()
    }

    private fun handleAction(action: InputAction, down: Boolean) {
        if (!booted) return
        when (action) {
            InputAction.MENU -> if (down) showMenu()
            InputAction.FAST_FORWARD -> {
                if (settings.fastForwardToggle) { if (down) setFastForward(!fastForward) }
                else { fastForwardHeld = down; setFastForward(down) }
            }
            InputAction.SWAP_SCREENS -> if (down) { settings.swapScreens = !settings.swapScreens; updateLayout() }
            InputAction.CYCLE_LAYOUT -> if (down) cycleLayout()
            InputAction.QUICK_SAVE -> if (down) saveState(SaveStateSlot(paths.stateDir(stem), SaveStateSlot.QUICK))
            InputAction.QUICK_LOAD -> if (down) loadState(SaveStateSlot(paths.stateDir(stem), SaveStateSlot.QUICK))
            InputAction.MICROPHONE -> NativeLib.nativeSetMic(down)
            InputAction.LID -> if (down) toggleLid()
            InputAction.SCREENSHOT -> if (down) screenshot()
            else -> Unit
        }
    }

    private fun setFastForward(on: Boolean) {
        fastForward = on
        controls.fastForwardActive = on
        NativeLib.nativeSetFastForward(on)
    }

    private fun cycleLayout() {
        val next = ScreenLayout.ALL[(ScreenLayout.ALL.indexOf(currentLayoutMode()) + 1) % ScreenLayout.ALL.size]
        if (isPortrait()) settings.layoutPortrait = next else settings.layoutLandscape = next
        updateLayout()
        Toast.makeText(this, layoutName(next), Toast.LENGTH_SHORT).show()
    }

    private fun layoutName(mode: String): String {
        val values = resources.getStringArray(R.array.layout_values)
        val names = resources.getStringArray(R.array.layout_entries)
        return names.getOrElse(values.indexOf(mode)) { mode }
    }

    private fun toggleLid() {
        lidClosed = !lidClosed
        NativeLib.nativeSetLid(lidClosed)
        Toast.makeText(this, if (lidClosed) R.string.lid_closed else R.string.lid_opened, Toast.LENGTH_SHORT).show()
    }

    // ---- Menu and dialogs ------------------------------------------------------------------------

    /** Shows `dialog` with the game paused until it closes. */
    private fun present(dialog: AlertDialog) {
        dialogHolds++
        currentDialog = dialog
        updatePause()
        dialog.setOnDismissListener {
            dialogHolds--
            if (currentDialog === dialog) currentDialog = null
            updatePause()
        }
        dialog.show()
    }

    private fun showMenu() {
        if (!booted || currentDialog != null || isFinishing) return
        val content = LayoutInflater.from(this).inflate(R.layout.dialog_game_menu, null, false)
        content.findViewById<TextView>(R.id.menuTitle).text = gameTitle
        val dialog = MaterialAlertDialogBuilder(this).setView(content).create()
        fun button(id: Int, then: () -> Unit) = content.findViewById<MaterialButton>(id).setOnClickListener {
            // Open what comes next before closing the menu, so the game stays paused between them.
            then()
            dialog.dismiss()
        }
        content.findViewById<MaterialButton>(R.id.menuFastForward).text =
            getString(if (fastForward) R.string.menu_fast_forward_off else R.string.menu_fast_forward_on)
        content.findViewById<MaterialButton>(R.id.menuLid).text =
            getString(if (lidClosed) R.string.menu_open_lid else R.string.menu_close_lid)
        button(R.id.menuResume) {}
        button(R.id.menuSaveState) { showSlots(save = true) }
        button(R.id.menuLoadState) { showSlots(save = false) }
        button(R.id.menuFastForward) { setFastForward(!fastForward) }
        button(R.id.menuLayout) { showLayoutPicker() }
        button(R.id.menuSwap) { settings.swapScreens = !settings.swapScreens; updateLayout() }
        button(R.id.menuCheats) { showCheats() }
        button(R.id.menuScreenshot) { handler.postDelayed({ screenshot() }, 150) }
        button(R.id.menuLid) { toggleLid() }
        button(R.id.menuReset) { confirmReset() }
        button(R.id.menuSettings) { startActivity(Intent(this, SettingsActivity::class.java)) }
        button(R.id.menuExit) { exitGame() }
        present(dialog)
    }

    private fun showLayoutPicker() {
        val values = resources.getStringArray(R.array.layout_values)
        val current = values.indexOf(currentLayoutMode()).coerceAtLeast(0)
        present(
            MaterialAlertDialogBuilder(this)
                .setTitle(if (isPortrait()) R.string.pref_layout_portrait else R.string.pref_layout_landscape)
                .setSingleChoiceItems(R.array.layout_entries, current) { d, which ->
                    if (isPortrait()) settings.layoutPortrait = values[which] else settings.layoutLandscape = values[which]
                    updateLayout()
                    d.dismiss()
                }
                .create()
        )
    }

    private fun confirmReset() {
        present(
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.menu_reset)
                .setMessage(R.string.reset_confirm)
                .setPositiveButton(R.string.menu_reset) { _, _ -> NativeLib.nativeReset() }
                .setNegativeButton(android.R.string.cancel, null)
                .create()
        )
    }

    private fun showSlots(save: Boolean) {
        val inflater = LayoutInflater.from(this)
        val content = inflater.inflate(R.layout.dialog_state_slots, null, false)
        val list = content.findViewById<LinearLayout>(R.id.slotList)
        lateinit var dialog: AlertDialog
        for (slot in SaveStateSlot.all(paths.stateDir(stem))) {
            if (save && slot.isAuto) continue
            val row = inflater.inflate(R.layout.item_state_slot, list, false)
            row.findViewById<TextView>(R.id.slotLabel).text =
                if (slot.isAuto) getString(R.string.slot_auto) else getString(R.string.slot_label, slot.number)
            row.findViewById<TextView>(R.id.slotTime).text = if (slot.exists) {
                DateUtils.formatDateTime(this, slot.savedAt, DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME or DateUtils.FORMAT_ABBREV_MONTH)
            } else getString(R.string.slot_empty)
            val thumb = row.findViewById<ImageView>(R.id.slotThumb)
            slot.decodeThumbnail()?.let { thumb.setImageBitmap(it) }
            val usable = save || slot.exists
            row.isEnabled = usable
            row.alpha = if (usable) 1f else 0.45f
            if (usable) row.setOnClickListener {
                dialog.dismiss()
                if (save) saveState(slot) else loadState(slot)
            }
            list.addView(row)
        }
        dialog = MaterialAlertDialogBuilder(this)
            .setTitle(if (save) R.string.menu_save_state else R.string.menu_load_state)
            .setView(content)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        present(dialog)
    }

    private fun saveState(slot: SaveStateSlot) {
        val frame = NativeLib.nativeGetFrame()
        val msg = NativeLib.nativeSaveState(slot.state.path)
        if (msg.isEmpty()) slot.writeThumbnail(frame)
        Toast.makeText(this, msg.ifEmpty { getString(R.string.state_saved, slot.number) }, Toast.LENGTH_SHORT).show()
    }

    private fun loadState(slot: SaveStateSlot) {
        if (!slot.exists) { Toast.makeText(this, R.string.slot_empty, Toast.LENGTH_SHORT).show(); return }
        val msg = NativeLib.nativeLoadState(slot.state.path)
        Toast.makeText(this, msg.ifEmpty { getString(R.string.state_loaded) }, Toast.LENGTH_SHORT).show()
    }

    private fun showCheats() {
        val cheats = cheatStore.load()
        val names = cheats.map { it.name }.toTypedArray()
        val checked = cheats.map { it.enabled }.toBooleanArray()
        val builder = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.cheats_title)
            .setPositiveButton(R.string.cheats_apply) { _, _ ->
                cheatStore.save(cheats)
                cheatStore.apply(cheats)
            }
            .setNegativeButton(R.string.cheats_add) { _, _ -> showAddCheat(cheats) }
        if (paths.cheatDatabase.isFile) builder.setNeutralButton(R.string.cheats_database) { _, _ -> showDatabaseCheats(cheats) }
        if (cheats.isEmpty()) builder.setMessage(R.string.cheats_none)
        else builder.setMultiChoiceItems(names, checked) { _, which, on -> cheats[which].enabled = on }
        val dialog = builder.create()
        dialog.setOnShowListener {
            dialog.listView?.setOnItemLongClickListener { _, _, position, _ ->
                MaterialAlertDialogBuilder(this)
                    .setTitle(cheats[position].name)
                    .setMessage(R.string.cheats_delete_confirm)
                    .setPositiveButton(R.string.delete) { _, _ ->
                        cheats.removeAt(position)
                        cheatStore.save(cheats)
                        cheatStore.apply(cheats)
                        dialog.dismiss()
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                true
            }
        }
        present(dialog)
    }

    private fun showAddCheat(cheats: MutableList<Cheat>) {
        val content = LayoutInflater.from(this).inflate(R.layout.dialog_add_cheat, null, false)
        val name = content.findViewById<EditText>(R.id.cheatName)
        val code = content.findViewById<EditText>(R.id.cheatCode)
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.cheats_add)
            .setView(content)
            .setPositiveButton(R.string.cheats_add, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (Cheat.parse(code.text.toString()) == null) {
                    code.error = getString(R.string.cheats_bad_code)
                    return@setOnClickListener
                }
                cheats += Cheat(name.text.toString().ifBlank { getString(R.string.cheats_unnamed) }, code.text.toString().trim(), true)
                cheatStore.save(cheats)
                cheatStore.apply(cheats)
                dialog.dismiss()
            }
        }
        present(dialog)
    }

    private fun showDatabaseCheats(cheats: MutableList<Cheat>) {
        val found = NativeLib.nativeDatabaseCheats(paths.cheatDatabase.path)
        if (found.size == 1) {
            present(MaterialAlertDialogBuilder(this).setTitle(R.string.cheats_database).setMessage(found[0])
                .setPositiveButton(android.R.string.ok, null).create())
            return
        }
        val entries = found.toList().chunked(3).map { Cheat(it[0], it[2], false) }
        val have = cheats.map { it.name }.toSet()
        val picked = BooleanArray(entries.size) { entries[it].name in have }
        present(
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.cheats_database)
                .setMultiChoiceItems(entries.map { it.name }.toTypedArray(), picked) { _, which, on -> picked[which] = on }
                .setPositiveButton(R.string.cheats_import) { _, _ ->
                    entries.forEachIndexed { i, c -> if (picked[i] && c.name !in have) cheats += c.copy(enabled = true) }
                    cheatStore.save(cheats)
                    cheatStore.apply(cheats)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .create()
        )
    }

    private fun screenshot() {
        val frame = NativeLib.nativeGetFrame() ?: return
        val bmp = Bitmap.createBitmap(frame, 256, 384, Bitmap.Config.ARGB_8888)
        val name = "${stem}_${System.currentTimeMillis()}.png"
        val saved = runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/NinDS")
                }
                val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: error("no uri")
                contentResolver.openOutputStream(uri)?.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            } else {
                paths.screenshots.mkdirs()
                File(paths.screenshots, name).outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            }
        }.isSuccess
        Toast.makeText(this, if (saved) R.string.screenshot_saved else R.string.screenshot_failed, Toast.LENGTH_SHORT).show()
    }

    // ---- Leaving ------------------------------------------------------------------------------------

    private fun exitGame() {
        exitNow(save = true)
    }

    private fun shutdownNative(save: Boolean) {
        if (!booted) return
        if (save && settings.autoSaveState) {
            val slot = SaveStateSlot(paths.stateDir(stem), SaveStateSlot.AUTO)
            val frame = NativeLib.nativeGetFrame()
            if (NativeLib.nativeSaveState(slot.state.path).isEmpty()) slot.writeThumbnail(frame)
        }
        NativeLib.nativeShutdown()
        booted = false
        romFd?.close()
        romFd = null
    }

    private fun exitNow(save: Boolean) {
        exiting = true
        shutdownNative(save)
        finish()
    }

    companion object {
        private const val EXTRA_URI = "com.izzy2lost.ninds.extra.URI"
        private const val EXTRA_FILE_NAME = "com.izzy2lost.ninds.extra.FILE_NAME"
        private const val EXTRA_TITLE = "com.izzy2lost.ninds.extra.TITLE"
        private const val EXTRA_LOAD_SLOT = "com.izzy2lost.ninds.extra.LOAD_SLOT"

        fun intent(context: Context, game: Game, loadSlot: Int = -1): Intent {
            val intent = Intent(context, EmulatorActivity::class.java)
                .putExtra(EXTRA_URI, game.uri)
                .putExtra(EXTRA_FILE_NAME, game.fileName)
                .putExtra(EXTRA_TITLE, game.title)
                .putExtra(EXTRA_LOAD_SLOT, loadSlot)
            val uri = Uri.parse(game.uri)
            // Carries a one-off "Open with" grant over to the :emu process. A file:// URI
            // in an intent's data is refused outright, and needs no grant anyway.
            if (uri.scheme == "content") intent.setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            return intent
        }
    }
}
