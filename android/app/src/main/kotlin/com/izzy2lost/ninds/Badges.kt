package com.izzy2lost.ninds

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.widget.ImageView
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.Executors

/** RetroAchievements badge images: memory, then disk (cacheDir/badges), then the network. */
object Badges {
    private val memory = LruCache<String, Bitmap>(200)
    private val pool = Executors.newFixedThreadPool(3)
    private val main = Handler(Looper.getMainLooper())

    /** Shows [url] in [view] once it is at hand; a recycled view keeps only its latest request. */
    fun load(context: Context, url: String, view: ImageView) {
        view.tag = url
        if (url.isEmpty()) { view.setImageDrawable(null); return }
        memory.get(url)?.let { view.setImageBitmap(it); return }
        view.setImageDrawable(null)
        val dir = File(context.cacheDir, "badges")
        pool.execute {
            val bitmap = runCatching { fetch(dir, url) }.getOrNull() ?: return@execute
            memory.put(url, bitmap)
            main.post { if (view.tag == url) view.setImageBitmap(bitmap) }
        }
    }

    private fun fetch(dir: File, url: String): Bitmap? {
        val name = MessageDigest.getInstance("SHA-1").digest(url.toByteArray()).joinToString("") { "%02x".format(it) }
        val file = File(dir, "$name.png")
        if (!file.isFile) {
            dir.mkdirs()
            val conn = URL(url).openConnection() as HttpURLConnection
            try {
                conn.connectTimeout = 10_000
                conn.readTimeout = 15_000
                if (conn.responseCode != HttpURLConnection.HTTP_OK) return null
                val tmp = File(dir, "$name.tmp")
                conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                tmp.renameTo(file)
            } finally {
                conn.disconnect()
            }
        }
        return BitmapFactory.decodeFile(file.path)
    }
}
