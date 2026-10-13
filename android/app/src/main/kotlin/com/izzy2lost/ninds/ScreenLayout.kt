package com.izzy2lost.ninds

import android.graphics.Rect
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Where the two DS screens go in the game view. The presenter draws into these rectangles and
 * the touch overlay maps touches back through the bottom screen's one, so the two always agree.
 */
object ScreenLayout {
    const val VERTICAL = "vertical"
    const val HORIZONTAL = "horizontal"
    const val HYBRID = "hybrid"
    const val TOP_ONLY = "top"
    const val BOTTOM_ONLY = "bottom"

    val ALL = listOf(VERTICAL, HORIZONTAL, HYBRID, TOP_ONLY, BOTTOM_ONLY)

    private const val W = 256
    private const val H = 192

    data class Placement(val top: Rect, val bottom: Rect, val topShown: Boolean, val bottomShown: Boolean)

    /**
     * @param reserveBottom pixels at the bottom kept clear for the touch controls (portrait).
     */
    fun compute(
        viewW: Int,
        viewH: Int,
        mode: String,
        swap: Boolean,
        integer: Boolean,
        gap: Int,
        reserveBottom: Int = 0,
    ): Placement {
        val availH = max(1, viewH - reserveBottom)
        // Screens sit at the top when space is reserved below them, centred otherwise.
        val anchorTop = reserveBottom > 0
        fun fit(scale: Float): Float = if (integer && scale >= 1f) floor(scale) else scale
        fun rect(x: Float, y: Float, s: Float) =
            Rect(x.roundToInt(), y.roundToInt(), (x + W * s).roundToInt(), (y + H * s).roundToInt())

        return when (mode) {
            HORIZONTAL -> {
                val s = fit(min((viewW - gap).toFloat() / (2 * W), availH.toFloat() / H))
                val totalW = 2 * W * s + gap
                val x0 = (viewW - totalW) / 2f
                val y0 = if (anchorTop) 0f else (availH - H * s) / 2f
                val first = rect(x0, y0, s)
                val second = rect(x0 + W * s + gap, y0, s)
                if (swap) Placement(second, first, true, true) else Placement(first, second, true, true)
            }
            HYBRID -> {
                if (viewH > viewW) return compute(viewW, viewH, VERTICAL, swap, integer, gap, reserveBottom)
                // A large screen with the other at half its size beside it.
                val s = fit(min(availH.toFloat() / H, (viewW - gap).toFloat() / (W * 1.5f)))
                val small = s / 2f
                val totalW = W * s + gap + W * small
                val x0 = (viewW - totalW) / 2f
                val y0 = (availH - H * s) / 2f
                val big = rect(x0, y0, s)
                // The small screen sits at the bottom of the column, near the thumbs.
                val little = rect(x0 + W * s + gap, y0 + H * s - H * small, small)
                if (swap) Placement(little, big, true, true) else Placement(big, little, true, true)
            }
            TOP_ONLY, BOTTOM_ONLY -> {
                val s = fit(min(viewW.toFloat() / W, availH.toFloat() / H))
                val x0 = (viewW - W * s) / 2f
                val y0 = if (anchorTop) 0f else (availH - H * s) / 2f
                val r = rect(x0, y0, s)
                val showTop = (mode == TOP_ONLY) != swap
                Placement(Rect(r), Rect(r), showTop, !showTop)
            }
            else -> {
                val s = fit(min(viewW.toFloat() / W, (availH - gap).toFloat() / (2 * H)))
                val totalH = 2 * H * s + gap
                val x0 = (viewW - W * s) / 2f
                val y0 = if (anchorTop) 0f else (availH - totalH) / 2f
                val first = rect(x0, y0, s)
                val second = rect(x0, y0 + H * s + gap, s)
                if (swap) Placement(second, first, true, true) else Placement(first, second, true, true)
            }
        }
    }
}
