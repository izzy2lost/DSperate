package com.izzy2lost.ninds

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import androidx.core.graphics.ColorUtils
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.min

/**
 * The on-screen DS: D-pad, face and shoulder buttons, Start/Select and a few emulator buttons,
 * drawn over the game. Touches that land on the DS's bottom screen instead of a control become
 * stylus input. Hidden controls (a gamepad is connected) still pass the touchscreen through.
 */
class TouchControlsView(context: Context) : View(context) {
    var onButtons: (Int) -> Unit = {}
    var onTouchScreen: (x: Int, y: Int, down: Boolean) -> Unit = { _, _, _ -> }
    var onAction: (InputAction, Boolean) -> Unit = { _, _ -> }

    var controlsVisible = true
        set(value) { field = value; releaseAll(); invalidate() }
    var opacity = 0.55f
        set(value) { field = value; invalidate() }
    var scale = 1f
        set(value) { field = value; requestLayoutControls() }
    var haptics = true
    var fastForwardActive = false
        set(value) { field = value; invalidate() }
    var suspended = false
        set(value) { field = value; if (value) releaseAll() }

    var skin: ControlSkin = ControlSkins.get(context, ControlSkins.CLASSIC)
        set(value) { field = value; shellShader = null; invalidate() }
    /** SNES-style coloured A/B/X/Y over the skin's own. */
    var faceColours = false
        set(value) { field = value; invalidate() }
    /** Settings preview: lay out as portrait over the whole view, with no screens. */
    var preview = false

    /** Where the DS bottom screen is, in this view's pixels; null when it is not shown. */
    var touchScreenRect: Rect? = null
    /** Every DS screen shown, cut out of the console shell. */
    var screenRects: List<Rect> = emptyList()
        set(value) { field = value; invalidate() }
    /** Portrait keeps the controls in this band at the bottom (the screens sit above it). */
    var portraitBandTop = 0

    private enum class Kind { DPAD, FACE, BUTTON, SCREEN }

    private class Control(val id: String, val label: String, val bit: Int = -1, val action: InputAction? = null) {
        val bounds = RectF()
        var round = true
        var pressed = false
    }

    private val density = resources.displayMetrics.density
    private val dpadBounds = RectF()
    private var dpadMask = 0
    private val face = listOf(
        Control("a", "A", DsButton.A), Control("b", "B", DsButton.B),
        Control("x", "X", DsButton.X), Control("y", "Y", DsButton.Y),
    )
    private val buttons = listOf(
        Control("l", "L", DsButton.L), Control("r", "R", DsButton.R),
        Control("select", "SELECT", DsButton.SELECT), Control("start", "START", DsButton.START),
        Control("menu", "☰", action = InputAction.MENU),
        Control("ff", "▶▶", action = InputAction.FAST_FORWARD),
        Control("swap", "⇅", action = InputAction.SWAP_SCREENS),
        Control("mic", "MIC", action = InputAction.MICROPHONE),
    )
    private val faceCenter = RectF()

    private data class Pointer(val kind: Kind, var control: Control? = null)
    private val pointers = HashMap<Int, Pointer>()
    private var faceMask = 0
    private var buttonMask = 0
    private var lastSent = -1

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; isFakeBoldText = true }
    private val shellPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private var shellShader: LinearGradient? = null
    private val bezelPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    init {
        isClickable = true
    }

    private fun requestLayoutControls() {
        if (width > 0) layoutControls(width, height)
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        shellShader = null
        layoutControls(w, h)
    }

    fun relayout() = requestLayoutControls()

    private fun layoutControls(w: Int, h: Int) {
        val u = density * scale
        val portrait = h > w || preview
        val bandTop = if (portrait && !preview) portraitBandTop.toFloat().coerceIn(0f, h * 0.8f) else 0f
        val bandH = h - bandTop
        fun at(fx: Float, fy: Float) = floatArrayOf(w * fx, bandTop + bandH * fy)
        fun place(c: Control, cx: Float, cy: Float, rw: Float, rh: Float, round: Boolean) {
            c.bounds.set(cx - rw / 2, cy - rh / 2, cx + rw / 2, cy + rh / 2)
            c.round = round
        }
        val dpadR = 68 * u
        val faceR = 27 * u
        val faceGap = 50 * u
        val shoulderW = 92 * u
        val shoulderH = 42 * u
        val smallR = 21 * u
        val pillW = 72 * u
        val pillH = 30 * u

        val (dpad, faceC) = if (portrait) at(0.24f, 0.58f) to at(0.76f, 0.58f) else at(0.11f, 0.64f) to at(0.89f, 0.64f)
        // Keep the clusters on screen whatever the scale.
        val dx = dpad[0].coerceIn(dpadR + 8 * density, w - dpadR - 8 * density)
        val dy = dpad[1].coerceIn(bandTop + dpadR, h - dpadR - 8 * density)
        dpadBounds.set(dx - dpadR, dy - dpadR, dx + dpadR, dy + dpadR)
        val fx = faceC[0].coerceIn(faceGap + faceR + 8 * density, w - faceGap - faceR - 8 * density)
        val fy = faceC[1].coerceIn(bandTop + faceGap + faceR, h - faceGap - faceR - 8 * density)
        faceCenter.set(fx, fy, fx, fy)
        place(face[0], fx + faceGap, fy, faceR * 2, faceR * 2, true)   // A right
        place(face[1], fx, fy + faceGap, faceR * 2, faceR * 2, true)   // B bottom
        place(face[2], fx, fy - faceGap, faceR * 2, faceR * 2, true)   // X top
        place(face[3], fx - faceGap, fy, faceR * 2, faceR * 2, true)   // Y left

        val byId = buttons.associateBy { it.id }
        if (portrait) {
            val top = bandTop + shoulderH / 2 + 10 * density
            place(byId.getValue("l"), shoulderW / 2 + 12 * density, top, shoulderW, shoulderH, false)
            place(byId.getValue("r"), w - shoulderW / 2 - 12 * density, top, shoulderW, shoulderH, false)
            val bottom = h - pillH / 2 - 16 * density
            place(byId.getValue("select"), w * 0.5f - pillW * 0.6f, bottom, pillW, pillH, false)
            place(byId.getValue("start"), w * 0.5f + pillW * 0.6f, bottom, pillW, pillH, false)
            val mid = top
            place(byId.getValue("menu"), w * 0.5f, mid, smallR * 2, smallR * 2, true)
            place(byId.getValue("ff"), w * 0.5f - smallR * 2.6f, mid, smallR * 2, smallR * 2, true)
            place(byId.getValue("swap"), w * 0.5f + smallR * 2.6f, mid, smallR * 2, smallR * 2, true)
            place(byId.getValue("mic"), w * 0.5f, (mid + bottom) / 2, smallR * 2, smallR * 2, true)
        } else {
            val top = shoulderH / 2 + 12 * density
            place(byId.getValue("l"), shoulderW / 2 + 12 * density, top, shoulderW, shoulderH, false)
            place(byId.getValue("r"), w - shoulderW / 2 - 12 * density, top, shoulderW, shoulderH, false)
            val bottom = h - pillH / 2 - 12 * density
            place(byId.getValue("select"), pillW / 2 + 16 * density, bottom, pillW, pillH, false)
            place(byId.getValue("start"), w - pillW / 2 - 16 * density, bottom, pillW, pillH, false)
            val row = top + shoulderH / 2 + smallR + 14 * density
            place(byId.getValue("menu"), smallR + 16 * density, row, smallR * 2, smallR * 2, true)
            place(byId.getValue("mic"), smallR * 3.6f + 16 * density, row, smallR * 2, smallR * 2, true)
            place(byId.getValue("ff"), w - smallR - 16 * density, row, smallR * 2, smallR * 2, true)
            place(byId.getValue("swap"), w - smallR * 3.6f - 16 * density, row, smallR * 2, smallR * 2, true)
        }
    }

    /** The skin paints a console body here: portrait (or the preview) with the controls up. */
    private fun shellShown() = skin.shell != null && controlsVisible && (preview || height > width)

    override fun onDraw(canvas: Canvas) {
        if (!controlsVisible) {
            // The menu stays reachable with the controls hidden.
            drawButton(canvas, buttons.first { it.id == "menu" }, 0.35f)
            return
        }
        val shell = shellShown()
        if (shell) drawShell(canvas)
        // Over the game the controls are see-through; on the console body they are solid.
        val a = if (shell) 1f else opacity
        if (skin.glow) stroke.setShadowLayer(6 * density, 0f, 0f, skin.stroke) else stroke.clearShadowLayer()
        // D-pad: a cross with the held directions lit.
        val cx = dpadBounds.centerX()
        val cy = dpadBounds.centerY()
        val r = dpadBounds.width() / 2
        val arm = r * 0.36f
        fill.color = withAlpha(skin.dpadFill, a)
        stroke.color = withAlpha(skin.stroke, a)
        stroke.strokeWidth = 2 * density
        val corner = 8 * density
        val cross = Path().apply {
            addRoundRect(cx - arm, cy - r, cx + arm, cy + r, corner, corner, Path.Direction.CW)
            op(Path().apply { addRoundRect(cx - r, cy - arm, cx + r, cy + arm, corner, corner, Path.Direction.CW) }, Path.Op.UNION)
        }
        canvas.drawPath(cross, fill)
        canvas.drawPath(cross, stroke)
        fill.color = withAlpha(skin.pressedFill, a)
        fun lit(bit: Int, l: Float, t: Float, rr: Float, b: Float) {
            if (dpadMask and (1 shl bit) != 0) canvas.drawRoundRect(l, t, rr, b, 6 * density, 6 * density, fill)
        }
        lit(DsButton.UP, cx - arm, cy - r, cx + arm, cy - arm)
        lit(DsButton.DOWN, cx - arm, cy + arm, cx + arm, cy + r)
        lit(DsButton.LEFT, cx - r, cy - arm, cx - arm, cy + arm)
        lit(DsButton.RIGHT, cx + arm, cy - arm, cx + r, cy + arm)

        for (c in face) drawButton(canvas, c, a)
        for (c in buttons) {
            val on = c.pressed || (c.action == InputAction.FAST_FORWARD && fastForwardActive)
            drawButton(canvas, c, a, on)
        }
    }

    /** The console body: a gradient shell with a bezel round each screen and the screens cut out. */
    private fun drawShell(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val top = skin.shell!!
        val shader = shellShader ?: LinearGradient(0f, 0f, 0f, h, top, skin.shellShade ?: top, android.graphics.Shader.TileMode.CLAMP)
            .also { shellShader = it }
        val inset = 6 * density
        val round = 10 * density
        val shown = if (preview) emptyList() else screenRects
        canvas.save()
        for (r in shown) canvas.clipOutRect(r)
        shellPaint.shader = shader
        canvas.drawRect(0f, 0f, w, h, shellPaint)
        bezelPaint.color = skin.bezel
        for (r in shown) canvas.drawRoundRect(r.left - inset, r.top - inset, r.right + inset, r.bottom + inset, round, round, bezelPaint)
        canvas.restore()
        // The hinge: where the clamshell would fold, just above the controls.
        if (!preview && portraitBandTop > 0) {
            val y = portraitBandTop.toFloat()
            bezelPaint.color = ColorUtils.setAlphaComponent(Color.BLACK, 70)
            canvas.drawRect(0f, y, w, y + 3 * density, bezelPaint)
            bezelPaint.color = ColorUtils.setAlphaComponent(Color.WHITE, 50)
            canvas.drawRect(0f, y + 3 * density, w, y + 4 * density, bezelPaint)
        }
    }

    private fun withAlpha(colour: Int, a: Float) = ColorUtils.setAlphaComponent(colour, (Color.alpha(colour) * a).toInt().coerceIn(0, 255))

    private fun drawButton(canvas: Canvas, c: Control, a: Float, lit: Boolean = c.pressed) {
        val b = c.bounds
        val isFace = c in face
        val tint = if (isFace && faceColours) ControlSkins.FACE_COLOURS[c.id] else if (isFace) skin.faceFill else null
        fill.color = withAlpha(
            when {
                lit && tint != null -> ColorUtils.blendARGB(tint, Color.WHITE, 0.45f)
                lit -> skin.pressedFill
                tint != null -> tint
                else -> skin.fill
            }, a,
        )
        stroke.color = withAlpha(if (tint != null && !skin.glow) ColorUtils.blendARGB(tint, Color.BLACK, 0.3f) else skin.stroke, a)
        stroke.strokeWidth = 2 * density
        if (c.round) {
            canvas.drawCircle(b.centerX(), b.centerY(), b.width() / 2, fill)
            canvas.drawCircle(b.centerX(), b.centerY(), b.width() / 2, stroke)
        } else {
            val rad = b.height() / 2
            canvas.drawRoundRect(b, rad, rad, fill)
            canvas.drawRoundRect(b, rad, rad, stroke)
        }
        val labelColour = when {
            tint != null && lit -> ColorUtils.blendARGB(tint, Color.BLACK, 0.55f)
            tint != null -> if (faceColours) Color.WHITE else skin.faceLabel ?: skin.label
            lit -> skin.pressedLabel
            else -> skin.label
        }
        text.color = withAlpha(labelColour, a)
        if (skin.glow && !lit) text.setShadowLayer(5 * density, 0f, 0f, skin.label) else text.clearShadowLayer()
        text.textSize = min(b.height() * 0.42f, if (c.label.length > 3) b.width() / (c.label.length * 0.62f) else b.height())
        canvas.drawText(c.label, b.centerX(), b.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (suspended) return true
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                begin(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) {
                move(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> end(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        publish()
        return true
    }

    private fun hit(c: Control, x: Float, y: Float, slop: Float = 1.25f): Boolean {
        val b = c.bounds
        return if (c.round) hypot(x - b.centerX(), y - b.centerY()) <= b.width() / 2 * slop
        else x >= b.left - 6 * density && x <= b.right + 6 * density && y >= b.top - 8 * density && y <= b.bottom + 8 * density
    }

    private fun begin(id: Int, x: Float, y: Float) {
        val menu = buttons.first { it.id == "menu" }
        if (!controlsVisible) {
            if (hit(menu, x, y)) {
                pointers[id] = Pointer(Kind.BUTTON, menu)
                press(menu, true)
            } else beginScreen(id, x, y)
            return
        }
        if (hypot(x - dpadBounds.centerX(), y - dpadBounds.centerY()) <= dpadBounds.width() / 2 * 1.3f) {
            pointers[id] = Pointer(Kind.DPAD)
            dpad(x, y)
            return
        }
        buttons.firstOrNull { hit(it, x, y) }?.let {
            pointers[id] = Pointer(Kind.BUTTON, it)
            press(it, true)
            return
        }
        nearestFace(x, y)?.let {
            pointers[id] = Pointer(Kind.FACE, it)
            press(it, true)
            return
        }
        beginScreen(id, x, y)
    }

    private fun beginScreen(id: Int, x: Float, y: Float) {
        val r = touchScreenRect ?: return
        if (pointers.values.any { it.kind == Kind.SCREEN }) return
        if (!r.contains(x.toInt(), y.toInt())) return
        pointers[id] = Pointer(Kind.SCREEN)
        screen(r, x, y, true)
    }

    private fun move(id: Int, x: Float, y: Float) {
        val p = pointers[id] ?: return
        when (p.kind) {
            Kind.DPAD -> dpad(x, y)
            Kind.FACE -> {
                // Rolling a thumb across the face buttons follows it.
                val now = nearestFace(x, y)
                if (now != null && now != p.control) {
                    p.control?.let { press(it, false) }
                    p.control = now
                    press(now, true)
                }
            }
            Kind.SCREEN -> touchScreenRect?.let { screen(it, x, y, true) }
            Kind.BUTTON -> Unit
        }
    }

    private fun end(id: Int) {
        val p = pointers.remove(id) ?: return
        when (p.kind) {
            Kind.DPAD -> dpadMask = 0
            Kind.FACE, Kind.BUTTON -> p.control?.let { c -> if (pointers.values.none { it.control == c }) press(c, false) }
            Kind.SCREEN -> onTouchScreen(0, 0, false)
        }
        invalidate()
    }

    private fun releaseAll() {
        if (pointers.values.any { it.kind == Kind.SCREEN }) onTouchScreen(0, 0, false)
        pointers.clear()
        dpadMask = 0
        (face + buttons).forEach { if (it.pressed) press(it, false) }
        publish()
        invalidate()
    }

    private fun nearestFace(x: Float, y: Float): Control? {
        val reach = face[0].bounds.width() * 1.05f
        return face.minByOrNull { hypot(x - it.bounds.centerX(), y - it.bounds.centerY()) }
            ?.takeIf { hypot(x - it.bounds.centerX(), y - it.bounds.centerY()) <= reach }
    }

    private fun dpad(x: Float, y: Float) {
        val dx = x - dpadBounds.centerX()
        val dy = y - dpadBounds.centerY()
        val dead = dpadBounds.width() * 0.08f
        var mask = 0
        if (hypot(dx, dy) > dead) {
            // Eight 45-degree sectors, so diagonals are as easy to hit as straights.
            val sector = ((Math.toDegrees(atan2(dy.toDouble(), dx.toDouble())) + 360 + 22.5) % 360 / 45).toInt()
            mask = when (sector) {
                0 -> 1 shl DsButton.RIGHT
                1 -> (1 shl DsButton.RIGHT) or (1 shl DsButton.DOWN)
                2 -> 1 shl DsButton.DOWN
                3 -> (1 shl DsButton.DOWN) or (1 shl DsButton.LEFT)
                4 -> 1 shl DsButton.LEFT
                5 -> (1 shl DsButton.LEFT) or (1 shl DsButton.UP)
                6 -> 1 shl DsButton.UP
                else -> (1 shl DsButton.UP) or (1 shl DsButton.RIGHT)
            }
        }
        if (mask != dpadMask) {
            if (mask != 0 && haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
            dpadMask = mask
            invalidate()
        }
    }

    private fun screen(r: Rect, x: Float, y: Float, down: Boolean) {
        val sx = ((x - r.left) * 256f / r.width()).toInt().coerceIn(0, 255)
        val sy = ((y - r.top) * 192f / r.height()).toInt().coerceIn(0, 191)
        onTouchScreen(sx, sy, down)
    }

    private fun press(c: Control, down: Boolean) {
        if (c.pressed == down) return
        c.pressed = down
        if (down && haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        if (c.bit >= 0) {
            val m = 1 shl c.bit
            if (c in face) faceMask = if (down) faceMask or m else faceMask and m.inv()
            else buttonMask = if (down) buttonMask or m else buttonMask and m.inv()
        }
        c.action?.let { onAction(it, down) }
        invalidate()
    }

    private fun publish() {
        val mask = dpadMask or faceMask or buttonMask
        if (mask != lastSent) {
            lastSent = mask
            onButtons(mask)
        }
    }
}
