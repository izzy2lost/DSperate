package com.izzy2lost.ninds

import android.content.Context
import android.graphics.BitmapFactory
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale

/**
 * Box art and GameTDB titles, downloaded from the izzy2lost/ds-covers repository on GitHub.
 *
 * The repository holds covers/<GAMEID>.webp, index.txt (every game ID with a cover) and
 * titles/<LANG>.txt (GameTDB's "ID = Title" lists). The index is fetched first, so a game whose
 * own region has no cover can borrow another region's instead of probing for missing files.
 * Everything is kept in the app's files, so the library works offline once downloaded.
 */
class Covers(context: Context) {
    private val dir = File(context.filesDir, "covers").apply { mkdirs() }
    private val indexFile = File(dir, "index.txt")
    private val titlesFile = File(dir, "titles-$titlesLanguage.txt")

    /** The local cover for a game, if it has been downloaded. */
    fun coverFile(gameCode: String): File? {
        if (gameCode.length != 4) return null
        File(dir, "$gameCode.webp").takeIf { it.isFile }?.let { return it }
        return resolve(gameCode)?.let { File(dir, "$it.webp") }?.takeIf { it.isFile }
    }

    /** GameTDB's title for each game ID, in the device's language (English where it has none). */
    @Synchronized
    fun titles(): Map<String, String> {
        cachedTitles?.let { if (it.first == titlesFile.lastModified()) return it.second }
        val map = HashMap<String, String>()
        runCatching {
            titlesFile.forEachLine { line ->
                val eq = line.indexOf(" = ")
                if (eq == 4) map[line.substring(0, 4)] = line.substring(7).trim()
            }
        }
        cachedTitles = titlesFile.lastModified() to map
        return map
    }

    private var cachedTitles: Pair<Long, Map<String, String>>? = null
    private var cachedIndex: Pair<Long, Set<String>>? = null

    @Synchronized
    private fun index(): Set<String> {
        cachedIndex?.let { if (it.first == indexFile.lastModified()) return it.second }
        val set = runCatching { indexFile.readLines().map { it.trim() }.filter { it.length == 4 }.toHashSet() }
            .getOrDefault(hashSetOf())
        cachedIndex = indexFile.lastModified() to set
        return set
    }

    /** The cover to use for a game ID: its own, else the same game from another region. */
    private fun resolve(gameCode: String): String? {
        val index = index()
        if (gameCode in index) return gameCode
        val stem = gameCode.substring(0, 3)
        return REGION_PREFERENCE.map { stem + it }.firstOrNull { it in index }
            ?: index.firstOrNull { it.startsWith(stem) }
    }

    /**
     * Downloads the index, the title list and every missing cover for [gameCodes]. Blocking: call it
     * off the main thread. [progress] is called after each game. [refresh] re-fetches the index and
     * titles even when they are already here. Returns how many of the games now have a cover.
     */
    fun download(gameCodes: Collection<String>, refresh: Boolean, progress: (done: Int, total: Int) -> Unit): Result {
        if (refresh || !indexFile.isFile) fetch("index.txt", indexFile)
        if (refresh || !titlesFile.isFile) {
            runCatching { fetch("titles/$titlesLanguage.txt", titlesFile) }
                .recoverCatching { if (titlesLanguage != "EN") fetch("titles/EN.txt", titlesFile) else throw it }
        }
        val codes = gameCodes.filter { it.length == 4 }.distinct()
        var have = 0
        var failed = 0
        codes.forEachIndexed { i, code ->
            if (coverFile(code) != null) have++
            else {
                val id = resolve(code)
                if (id != null) {
                    if (runCatching { fetch("covers/$id.webp", File(dir, "$id.webp"), image = true) }.isSuccess) have++
                    else failed++
                }
            }
            progress(i + 1, codes.size)
        }
        return Result(codes.size, have, failed)
    }

    data class Result(val games: Int, val withCover: Int, val failed: Int)

    /** Fetches one repository file to [target], replacing it only once the whole file has arrived. */
    private fun fetch(path: String, target: File, image: Boolean = false) {
        val tmp = File(target.path + ".part")
        val conn = URL(BASE_URL + path).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 10_000
            conn.readTimeout = 20_000
            conn.setRequestProperty("User-Agent", "NinDS-Android")
            if (conn.responseCode != HttpURLConnection.HTTP_OK) throw IOException("HTTP ${conn.responseCode} for $path")
            val expected = conn.contentLengthLong
            val got = conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
            if (got == 0L || (expected >= 0 && got != expected)) throw IOException("Incomplete download of $path")
            if (image) {
                val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(tmp.path, opts)
                if (opts.outWidth <= 0 || opts.outHeight <= 0) throw IOException("$path is not an image")
            }
            if (!tmp.renameTo(target)) throw IOException("Could not store $path")
        } finally {
            conn.disconnect()
            tmp.delete()
        }
    }

    companion object {
        const val BASE_URL = "https://raw.githubusercontent.com/izzy2lost/ds-covers/main/"

        /** Region letters to borrow a cover from, most familiar first: USA, Europe, Japan... */
        private const val REGION_PREFERENCE = "EPJXYVDFISHUK"

        /** The GameTDB title list closest to the device's language. */
        private val titlesLanguage: String
            get() {
                val locale = Locale.getDefault()
                return when (locale.language) {
                    "de" -> "DE"; "es" -> "ES"; "fr" -> "FR"; "it" -> "IT"; "ja" -> "JA"; "ko" -> "KO"
                    "nl" -> "NL"; "pt" -> "PT"; "ru" -> "RU"
                    "zh" -> if (locale.script == "Hant" || locale.country in setOf("TW", "HK", "MO")) "ZHTW" else "ZHCN"
                    else -> "EN"
                }
            }
    }
}
