package com.izzy2lost.ninds

import android.content.Context
import java.io.File

/**
 * Where NinDS keeps its files. App-specific external storage, so saves can be reached with a
 * file manager (Android/data/com.izzy2lost.ninds/files) and go away with the app.
 */
class AppPaths(context: Context) {
    val root: File = context.getExternalFilesDir(null) ?: context.filesDir
    val saves = File(root, "saves")
    val states = File(root, "states")
    val cheats = File(root, "cheats")
    val bios = File(root, "bios")
    val screenshots = File(root, "screenshots")
    val icons = File(context.filesDir, "icons")
    val cache: File = context.cacheDir
    val cheatDatabase = File(root, "usrcheat.dat")

    init {
        listOf(saves, states, cheats, bios, icons).forEach { it.mkdirs() }
    }

    fun savFile(stem: String) = File(saves, "$stem.sav")

    fun stateDir(stem: String) = File(states, stem).apply { mkdirs() }

    fun cheatFile(stem: String) = File(cheats, "$stem.json")

    companion object {
        /** A file-system-safe name for a game, from its file name without the extension. */
        fun stemOf(displayName: String): String =
            displayName.substringBeforeLast('.').replace(Regex("""[\\/:*?"<>|]"""), "_").trim().ifBlank { "game" }
    }
}
