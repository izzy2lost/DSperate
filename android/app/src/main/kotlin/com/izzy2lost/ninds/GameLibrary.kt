package com.izzy2lost.ninds

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.provider.DocumentsContract
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import java.util.zip.ZipInputStream

data class Game(
    val uri: String,
    val fileName: String,
    val size: Long,
    val modified: Long,
    val title: String,
    val publisher: String,
    val gameCode: String,
    val iconPath: String?,
    /** Opened on its own rather than found in the ROM folder; kept across rescans. */
    val single: Boolean = false,
) {
    val stem: String get() = AppPaths.stemOf(fileName)

    fun toJson(): JSONObject = JSONObject()
        .put("uri", uri).put("fileName", fileName).put("size", size).put("modified", modified)
        .put("title", title).put("publisher", publisher).put("gameCode", gameCode).put("icon", iconPath ?: "").put("single", single)

    companion object {
        fun fromJson(o: JSONObject) = Game(
            o.getString("uri"), o.getString("fileName"), o.getLong("size"), o.getLong("modified"),
            o.optString("title"), o.optString("publisher"), o.optString("gameCode"),
            o.optString("icon").ifEmpty { null },
            o.optBoolean("single"),
        )
    }
}

/**
 * The games in the player's ROM folder (a Storage Access Framework tree), with titles and icons
 * read from each cartridge's banner. Results are cached so the library opens instantly; a rescan
 * reuses entries whose size and date have not changed.
 */
class GameLibrary(private val context: Context) {
    private val paths = AppPaths(context)
    private val cacheFile = File(context.filesDir, "library.json")

    fun cached(): List<Game> = runCatching {
        val arr = JSONArray(cacheFile.readText())
        List(arr.length()) { Game.fromJson(arr.getJSONObject(it)) }
    }.getOrDefault(emptyList())

    private fun store(games: List<Game>) {
        val arr = JSONArray()
        games.forEach { arr.put(it.toJson()) }
        val tmp = File(cacheFile.parentFile, cacheFile.name + ".tmp")
        tmp.writeText(arr.toString())
        tmp.renameTo(cacheFile)
    }

    private data class Doc(val uri: Uri, val name: String, val size: Long, val modified: Long)

    /** Walks the tree and reads new or changed banners. Call off the main thread. */
    fun scan(tree: Uri, progress: (done: Int, total: Int) -> Unit = { _, _ -> }): List<Game> {
        val docs = mutableListOf<Doc>()
        walk(tree, DocumentsContract.getTreeDocumentId(tree), docs, 0)
        val previous = cached().associateBy { it.uri }
        val games = mutableListOf<Game>()
        docs.forEachIndexed { i, doc ->
            progress(i, docs.size)
            val old = previous[doc.uri.toString()]
            val game = if (old != null && old.size == doc.size && old.modified == doc.modified &&
                (old.iconPath == null || File(old.iconPath).isFile)
            ) old else runCatching { readGame(doc) }.getOrNull()
            if (game != null) games += game
        }
        val found = games.map { it.uri }.toSet()
        games += previous.values.filter { it.single && it.uri !in found }
        games.sortBy { it.title.lowercase(Locale.ROOT) }
        store(games)
        return games
    }

    /** Adds one game file picked on its own (for devices whose folder picker refuses every folder). */
    fun addFile(uri: Uri): Game? {
        val columns = arrayOf(
            android.provider.OpenableColumns.DISPLAY_NAME,
            android.provider.OpenableColumns.SIZE,
        )
        val doc = if (uri.scheme == "file") {
            val f = File(uri.path ?: return null)
            Doc(uri, f.name, f.length(), f.lastModified())
        } else context.contentResolver.query(uri, columns, null, null, null)?.use { c ->
            if (!c.moveToFirst()) null else Doc(uri, c.getString(0) ?: "game.nds", c.getLong(1), 0L)
        } ?: return null
        val game = runCatching { readGame(doc) }.getOrNull()?.copy(single = true) ?: return null
        val games = cached().filter { it.uri != game.uri }.toMutableList()
        games += game
        games.sortBy { it.title.lowercase(Locale.ROOT) }
        store(games)
        return game
    }

    private fun walk(tree: Uri, docId: String, out: MutableList<Doc>, depth: Int) {
        if (depth > 8) return
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        context.contentResolver.query(children, columns, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getString(0)
                val name = c.getString(1) ?: continue
                val mime = c.getString(2)
                if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                    if (!name.startsWith(".")) walk(tree, id, out, depth + 1)
                    continue
                }
                if (name.substringAfterLast('.', "").lowercase(Locale.ROOT) !in EXTENSIONS) continue
                out += Doc(DocumentsContract.buildDocumentUriUsingTree(tree, id), name, c.getLong(3), c.getLong(4))
            }
        }
    }

    private fun readGame(doc: Doc): Game? {
        val isZip = doc.name.lowercase(Locale.ROOT).endsWith(".zip")
        // A zip with no DS game inside is somebody else's archive.
        val banner = (if (isZip) readZipBanner(doc.uri) else readBanner(doc.uri)) ?: return null
        val iconPath = banner.icon?.let { bmp ->
            val f = File(paths.icons, "${doc.uri.toString().hashCode().toUInt()}.png")
            runCatching { f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) } }
            f.absolutePath
        }
        return Game(
            doc.uri.toString(), doc.name, doc.size, doc.modified,
            banner.title.ifBlank { banner.headerTitle.ifBlank { AppPaths.stemOf(doc.name) } },
            banner.publisher, banner.gameCode, iconPath,
        )
    }

    class Banner(val headerTitle: String, val gameCode: String, val title: String, val publisher: String, val icon: Bitmap?)

    private fun readBanner(uri: Uri): Banner? =
        context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
            FileInputStream(pfd.fileDescriptor).channel.use { ch ->
                fun at(pos: Long, n: Int): ByteArray? {
                    val buf = ByteBuffer.allocate(n)
                    var p = pos
                    while (buf.hasRemaining()) {
                        val r = ch.read(buf, p)
                        if (r <= 0) break
                        p += r
                    }
                    return if (buf.position() == n) buf.array() else null
                }
                val header = at(0, 0x200) ?: return@use null
                val bannerAt = le32(header, 0x68)
                parse(header, if (bannerAt in 0x200L..0x20000000L) at(bannerAt, 0x840) else null)
            }
        }

    private fun readZipBanner(uri: Uri): Banner? {
        val raw = context.contentResolver.openInputStream(uri) ?: return null
        ZipInputStream(raw.buffered()).use { zip ->
            var e = zip.nextEntry
            while (e != null) {
                val ext = e.name.substringAfterLast('.', "").lowercase(Locale.ROOT)
                if (!e.isDirectory && ext in ROM_EXTENSIONS) {
                    val header = readFully(zip, 0x200) ?: return null
                    val bannerAt = le32(header, 0x68)
                    // Inflating up to the banner is cheap for most games, whose banner sits
                    // in the first few megabytes; past 64 MB the icon is not worth the wait.
                    val banner = if (bannerAt in 0x200L..(64L shl 20)) {
                        skipFully(zip, bannerAt - 0x200)
                        readFully(zip, 0x840)
                    } else null
                    return parse(header, banner)
                }
                e = zip.nextEntry
            }
        }
        return null
    }

    private fun parse(header: ByteArray, banner: ByteArray?): Banner {
        val headerTitle = String(header, 0, 12, Charsets.US_ASCII).trim('\u0000', ' ')
        val code = String(header, 12, 4, Charsets.US_ASCII).filter { it.isLetterOrDigit() }
        var title = ""
        var publisher = ""
        var icon: Bitmap? = null
        if (banner != null && (banner[0].toInt() or banner[1].toInt()) != 0) {
            icon = decodeIcon(banner)
            val preferred = when (Locale.getDefault().language) {
                "ja" -> 0; "fr" -> 2; "de" -> 3; "it" -> 4; "es" -> 5; else -> 1
            }
            val text = listOf(preferred, 1, 0).map { lang ->
                String(banner, 0x240 + lang * 0x100, 0x100, Charsets.UTF_16LE).substringBefore('\u0000').trim()
            }.firstOrNull { it.isNotBlank() } ?: ""
            val lines = text.lines().map { it.trim() }.filter { it.isNotEmpty() }
            if (lines.size > 1) {
                title = lines.dropLast(1).joinToString(" ")
                publisher = lines.last()
            } else title = lines.firstOrNull() ?: ""
        }
        return Banner(headerTitle, code, title, publisher, icon)
    }

    /** 32x32, 4 bits per pixel in 8x8 tiles, with a 16-colour BGR555 palette (colour 0 clear). */
    private fun decodeIcon(b: ByteArray): Bitmap {
        val palette = IntArray(16) { i ->
            val c = (b[0x220 + i * 2].toInt() and 0xFF) or ((b[0x221 + i * 2].toInt() and 0xFF) shl 8)
            if (i == 0) 0 else {
                val r = (c and 31) * 255 / 31
                val g = ((c shr 5) and 31) * 255 / 31
                val bl = ((c shr 10) and 31) * 255 / 31
                (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
            }
        }
        val px = IntArray(32 * 32)
        for (tile in 0 until 16) {
            val tx = (tile % 4) * 8
            val ty = (tile / 4) * 8
            for (y in 0 until 8) for (x in 0 until 8) {
                val byte = b[0x20 + tile * 32 + y * 4 + x / 2].toInt() and 0xFF
                val idx = if (x and 1 == 0) byte and 15 else byte shr 4
                px[(ty + y) * 32 + tx + x] = palette[idx]
            }
        }
        return Bitmap.createBitmap(px, 32, 32, Bitmap.Config.ARGB_8888)
    }

    companion object {
        private val ROM_EXTENSIONS = setOf("nds", "dsi", "srl")
        private val EXTENSIONS = ROM_EXTENSIONS + "zip"

        private fun le32(b: ByteArray, off: Int): Long =
            ByteBuffer.wrap(b, off, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFFFFFFL

        private fun readFully(input: InputStream, n: Int): ByteArray? {
            val out = ByteArray(n)
            var got = 0
            while (got < n) {
                val r = input.read(out, got, n - got)
                if (r <= 0) return null
                got += r
            }
            return out
        }

        private fun skipFully(input: InputStream, n: Long) {
            var left = n
            val scratch = ByteArray(64 * 1024)
            while (left > 0) {
                val r = input.read(scratch, 0, minOf(left, scratch.size.toLong()).toInt())
                if (r <= 0) return
                left -= r
            }
        }
    }
}
