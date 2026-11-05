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
    /** Imported GBA games for slot 2, their saves beside them (<name>.sav). */
    val gba = File(root, "gba")

    init {
        listOf(saves, states, cheats, bios, icons, gba).forEach { it.mkdirs() }
    }

    fun savFile(stem: String) = File(saves, "$stem.sav")

    fun stateDir(stem: String) = File(states, stem).apply { mkdirs() }

    fun cheatFile(stem: String) = File(cheats, "$stem.json")

    fun gbaGames(): List<File> = gba.listFiles { f -> f.isFile && f.name.endsWith(".gba", ignoreCase = true) }?.sortedBy { it.name.lowercase() }.orEmpty()

    fun gbaSave(rom: File) = File(gba, rom.name.substringBeforeLast('.') + ".sav")

    companion object {
        /** A file-system-safe name for a game, from its file name without the extension. */
        fun stemOf(displayName: String): String =
            displayName.substringBeforeLast('.').replace(Regex("""[\\/:*?"<>|]"""), "_").trim().ifBlank { "game" }
    }
}
