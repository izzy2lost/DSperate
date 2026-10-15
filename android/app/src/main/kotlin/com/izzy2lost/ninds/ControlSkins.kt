package com.izzy2lost.ninds

import android.content.Context
import androidx.annotation.ColorInt
import com.google.android.material.color.MaterialColors

/**
 * How the on-screen controls look. A skin with a shell also paints the console body around the
 * screens in portrait, so the phone reads as a DS; without one the game's black shows through.
 */
class ControlSkin(
    val id: String,
    @ColorInt val shell: Int? = null,      // portrait body, top of the gradient
    @ColorInt val shellShade: Int? = null, // portrait body, bottom of the gradient
    @ColorInt val bezel: Int = 0xFF101012.toInt(),
    @ColorInt val fill: Int,
    @ColorInt val stroke: Int,
    @ColorInt val label: Int,
    @ColorInt val pressedFill: Int,
    @ColorInt val pressedLabel: Int,
    @ColorInt val dpadFill: Int = fill,
    @ColorInt val faceFill: Int? = null,   // A/B/X/Y, when they differ from the other buttons
    @ColorInt val faceLabel: Int? = null,
    val glow: Boolean = false,
)

object ControlSkins {
    const val CLASSIC = "classic"
    const val ACCENT = "accent"

    /** SNES-style A/B/X/Y, for the "coloured face buttons" option. */
    val FACE_COLOURS = mapOf(
        "a" to 0xFFD7263D.toInt(), "b" to 0xFFF2B33D.toInt(),
        "x" to 0xFF2E86DE.toInt(), "y" to 0xFF3BB273.toInt(),
    )

    private val fixed = listOf(
        // The original look: dark glass over the game.
        ControlSkin(
            CLASSIC,
            fill = 0x961E1E22.toInt(), stroke = 0xFFEBEBF0.toInt(), label = 0xFFF5F5FA.toInt(),
            pressedFill = 0xEBFFFFFF.toInt(), pressedLabel = 0xFF141418.toInt(),
        ),
        ControlSkin(
            "neon",
            fill = 0x30000000, stroke = 0xFF00E5FF.toInt(), label = 0xFF00E5FF.toInt(),
            pressedFill = 0xFF00E5FF.toInt(), pressedLabel = 0xFF001418.toInt(), glow = true,
        ),
        ControlSkin(
            "polar_white",
            shell = 0xFFF4F4F2.toInt(), shellShade = 0xFFD9D9D5.toInt(), bezel = 0xFF1C1C1F.toInt(),
            fill = 0xFFE9E9E6.toInt(), stroke = 0xFFB4B4B0.toInt(), label = 0xFF68686E.toInt(),
            pressedFill = 0xFF9A9AA0.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
        ),
        ControlSkin(
            "onyx",
            shell = 0xFF2A2B30.toInt(), shellShade = 0xFF16161A.toInt(), bezel = 0xFF09090A.toInt(),
            fill = 0xFF36373D.toInt(), stroke = 0xFF4C4D55.toInt(), label = 0xFFC9C9D0.toInt(),
            pressedFill = 0xFF74757E.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
        ),
        ControlSkin(
            "cobalt",
            shell = 0xFF2C62C2.toInt(), shellShade = 0xFF1A408E.toInt(), bezel = 0xFF0C1730.toInt(),
            fill = 0xFF183C7E.toInt(), stroke = 0xFF5E8AD8.toInt(), label = 0xFFE6EEFF.toInt(),
            pressedFill = 0xFFD2E0FF.toInt(), pressedLabel = 0xFF0C1730.toInt(),
        ),
        ControlSkin(
            "crimson",
            shell = 0xFFC4292F.toInt(), shellShade = 0xFF8C191D.toInt(), bezel = 0xFF280607.toInt(),
            fill = 0xFF7A1418.toInt(), stroke = 0xFFE2686C.toInt(), label = 0xFFFFE6E6.toInt(),
            pressedFill = 0xFFFFD3D5.toInt(), pressedLabel = 0xFF3A0A0B.toInt(),
        ),
        ControlSkin(
            "coral_pink",
            shell = 0xFFF6AFC5.toInt(), shellShade = 0xFFE184A1.toInt(), bezel = 0xFF3A1421.toInt(),
            fill = 0xFFFCDDE6.toInt(), stroke = 0xFFC46686.toInt(), label = 0xFF8A3555.toInt(),
            pressedFill = 0xFF8A3555.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
        ),
        ControlSkin(
            "ice_blue",
            shell = 0xFFBFE3F2.toInt(), shellShade = 0xFF93C8DE.toInt(), bezel = 0xFF14303C.toInt(),
            fill = 0xFFE6F5FB.toInt(), stroke = 0xFF6FA9C2.toInt(), label = 0xFF2C6680.toInt(),
            pressedFill = 0xFF2C6680.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
        ),
        ControlSkin(
            "famicom",
            shell = 0xFFE6DCC4.toInt(), shellShade = 0xFFCDBF9F.toInt(), bezel = 0xFF3A281C.toInt(),
            fill = 0xFF2C2A28.toInt(), stroke = 0xFF161514.toInt(), label = 0xFFE6DCC4.toInt(),
            pressedFill = 0xFF6A6560.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
            faceFill = 0xFFA81C30.toInt(), faceLabel = 0xFFF3E9D2.toInt(),
        ),
        ControlSkin(
            "brick",
            shell = 0xFFC9C5B9.toInt(), shellShade = 0xFFA8A396.toInt(), bezel = 0xFF4E4D58.toInt(),
            fill = 0xFF8D8B94.toInt(), stroke = 0xFF6D6B74.toInt(), label = 0xFF2E2E6A.toInt(),
            pressedFill = 0xFF4E4D58.toInt(), pressedLabel = 0xFFFFFFFF.toInt(),
            dpadFill = 0xFF2F2F35.toInt(), faceFill = 0xFF9B2358.toInt(), faceLabel = 0xFFF2E6EC.toInt(),
        ),
    )

    /** The skin [id] names, the classic one if it names none. [context] themes the accent skin. */
    fun get(context: Context, id: String): ControlSkin {
        if (id == ACCENT) {
            fun c(attr: Int) = MaterialColors.getColor(context, attr, 0)
            return ControlSkin(
                ACCENT,
                shell = c(com.google.android.material.R.attr.colorPrimaryContainer),
                shellShade = MaterialColors.layer(
                    c(com.google.android.material.R.attr.colorPrimaryContainer),
                    c(com.google.android.material.R.attr.colorPrimary), 0.25f,
                ),
                bezel = c(com.google.android.material.R.attr.colorOnPrimaryContainer),
                fill = c(com.google.android.material.R.attr.colorPrimary),
                stroke = c(com.google.android.material.R.attr.colorOnPrimaryContainer),
                label = c(com.google.android.material.R.attr.colorOnPrimary),
                pressedFill = c(com.google.android.material.R.attr.colorOnPrimaryContainer),
                pressedLabel = c(com.google.android.material.R.attr.colorPrimaryContainer),
            )
        }
        return fixed.firstOrNull { it.id == id } ?: fixed.first()
    }
}
