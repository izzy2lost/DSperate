package com.izzy2lost.ninds

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.File

/**
 * Save-state slots for one game: `slotN.dss` with a `slotN.png` thumbnail of both screens.
 * Slot 0 is the automatic one written on exit; slot 1 doubles as the quick-save slot.
 */
class SaveStateSlot(dir: File, val number: Int) {
    val state = File(dir, "slot$number.dss")
    val thumbnail = File(dir, "slot$number.png")
    val exists get() = state.isFile
    val savedAt get() = state.lastModified()
    val isAuto get() = number == AUTO

    fun decodeThumbnail(): Bitmap? = if (thumbnail.isFile) BitmapFactory.decodeFile(thumbnail.path) else null

    fun writeThumbnail(frame: IntArray?) {
        if (frame == null || frame.size < 256 * 384) return
        val bmp = Bitmap.createBitmap(frame, 256, 384, Bitmap.Config.ARGB_8888)
        runCatching { thumbnail.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) } }
    }

    companion object {
        const val AUTO = 0
        const val QUICK = 1
        const val COUNT = 9

        fun all(dir: File) = (0 until COUNT).map { SaveStateSlot(dir, it) }
    }
}
