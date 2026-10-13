package com.izzy2lost.ninds

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class Cheat(val name: String, val code: String, var enabled: Boolean) {
    /** The code's 32-bit words, or null when it is not a whole number of Action Replay lines. */
    fun words(): IntArray? = parse(code)

    companion object {
        fun parse(code: String): IntArray? {
            val tokens = code.split(Regex("""[\s,;]+""")).filter { it.isNotEmpty() }
            if (tokens.isEmpty() || tokens.size % 2 != 0) return null
            val words = IntArray(tokens.size)
            for ((i, t) in tokens.withIndex()) {
                if (t.length != 8) return null
                words[i] = t.toLongOrNull(16)?.toInt() ?: return null
            }
            return words
        }
    }
}

/** A game's Action Replay codes and which are switched on, as JSON beside its saves. */
class CheatStore(private val file: File) {
    fun load(): MutableList<Cheat> = runCatching {
        val arr = JSONArray(file.readText())
        MutableList(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            Cheat(o.getString("name"), o.getString("code"), o.optBoolean("enabled"))
        }
    }.getOrDefault(mutableListOf())

    fun save(cheats: List<Cheat>) {
        val arr = JSONArray()
        cheats.forEach { arr.put(JSONObject().put("name", it.name).put("code", it.code).put("enabled", it.enabled)) }
        file.parentFile?.mkdirs()
        file.writeText(arr.toString(2))
    }

    /** Hands the enabled codes to the running game. */
    fun apply(cheats: List<Cheat>) {
        val on = cheats.filter { it.enabled }.mapNotNull { c -> c.words()?.let { c.name to it } }
        NativeLib.nativeSetCheats(on.map { it.first }.toTypedArray(), on.map { it.second }.toTypedArray())
    }
}
