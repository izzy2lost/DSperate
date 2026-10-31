package com.izzy2lost.ninds

import android.app.Activity
import android.app.Presentation
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.util.Log
import android.view.Display
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager

/**
 * DS screens on a second display: a dual-screen device's other panel, or an external monitor.
 * Android reports both as a secondary [Display]; a [Presentation] puts a SurfaceView on it, whose
 * surface the presenter draws into beside the main one. Which screens go there is the
 * "second display" setting; touches on it reach the DS touch screen when that is shown there.
 */
class SecondDisplay(
    private val activity: Activity,
    private val handler: Handler,
    private val callbacks: Callbacks,
) : DisplayManager.DisplayListener {
    interface Callbacks {
        /** The second display's surface, or null when it goes away. */
        fun onSecondSurface(surface: Surface?)
        /** The second display appeared, went away or changed size: lay both displays out again. */
        fun onSecondDisplayChanged()
        /** A touch at DS touch-screen coordinates (0-255, 0-191). */
        fun onSecondTouch(x: Int, y: Int, down: Boolean)
    }

    private val displayManager = activity.getSystemService(DisplayManager::class.java)
    private var presentation: Screen? = null
    private var started = false

    /** False turns the second display off (the "off" setting): everything stays on the device. */
    var enabled = true
        set(value) { if (field != value) { field = value; refresh() } }

    /** The second display's drawable size in pixels, 0 when there is none to draw on. */
    var width = 0
        private set
    var height = 0
        private set
    val active get() = presentation != null && width > 0 && height > 0

    /** Where the DS touch screen is on the second display, or null if it is not shown there. */
    var touchRect: Rect? = null

    fun start() {
        if (started) return
        started = true
        displayManager.registerDisplayListener(this, handler)
        refresh()
    }

    fun stop() {
        if (!started) return
        started = false
        displayManager.unregisterDisplayListener(this)
        dismiss()
    }

    override fun onDisplayAdded(displayId: Int) = refresh()
    override fun onDisplayRemoved(displayId: Int) = refresh()
    override fun onDisplayChanged(displayId: Int) = refresh()

    private fun refresh() {
        val target = if (started && enabled) pick() else null
        val current = presentation
        if (current != null && target != null && current.display.displayId == target.displayId) return
        if (current == null && target == null) return
        dismiss()
        if (target != null) show(target)
        callbacks.onSecondDisplayChanged()
    }

    /** A presentation display first (external monitors, most dual-screen devices), else any other. */
    private fun pick(): Display? {
        val own = activity.windowManager.defaultDisplay.displayId
        fun usable(d: Display) = d.displayId != own && d.state != Display.STATE_OFF && d.isValid
        return displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).firstOrNull(::usable)
            ?: displayManager.displays.firstOrNull(::usable)
    }

    private fun show(display: Display) {
        val p = Screen(display)
        try {
            p.show()
            presentation = p
        } catch (e: WindowManager.InvalidDisplayException) {
            // Android refuses a presentation on a display not meant for one.
            Log.w(TAG, "can't show on display ${display.displayId}: ${e.message}")
        }
    }

    private fun dismiss() {
        val p = presentation ?: return
        presentation = null
        width = 0
        height = 0
        touchRect = null
        callbacks.onSecondSurface(null)
        p.setOnDismissListener(null)
        p.dismiss()
    }

    private inner class Screen(display: Display) : Presentation(activity, display), SurfaceHolder.Callback {
        private var touching = false

        override fun onCreate(savedInstanceState: Bundle?) {
            super.onCreate(savedInstanceState)
            val view = SurfaceView(context)
            view.holder.addCallback(this)
            view.setOnTouchListener { _, e -> touch(e); true }
            setContentView(view)
            window?.decorView?.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            // The system took the display back (it was unplugged, or another app claimed it).
            setOnDismissListener {
                if (presentation !== this) return@setOnDismissListener
                presentation = null
                width = 0
                height = 0
                touchRect = null
                callbacks.onSecondSurface(null)
                callbacks.onSecondDisplayChanged()
            }
        }

        override fun surfaceCreated(holder: SurfaceHolder) {}

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {
            width = w
            height = h
            callbacks.onSecondSurface(holder.surface)
            callbacks.onSecondDisplayChanged()
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            width = 0
            height = 0
            callbacks.onSecondSurface(null)
        }

        private fun touch(e: MotionEvent) {
            val r = touchRect
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                    if (r == null) return
                    // A touch starts only on the screen, then follows the finger anywhere.
                    if (!touching && !r.contains(e.x.toInt(), e.y.toInt())) return
                    touching = true
                    val sx = ((e.x - r.left) * 256f / r.width()).toInt().coerceIn(0, 255)
                    val sy = ((e.y - r.top) * 192f / r.height()).toInt().coerceIn(0, 191)
                    callbacks.onSecondTouch(sx, sy, true)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (touching) callbacks.onSecondTouch(0, 0, false)
                    touching = false
                }
            }
        }
    }

    private companion object { const val TAG = "NinDS" }
}
