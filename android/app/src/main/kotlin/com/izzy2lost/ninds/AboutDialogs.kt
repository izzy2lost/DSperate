package com.izzy2lost.ninds

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.util.TypedValue
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.StringRes
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * Credits and open-source licence screens reached from the About section of Settings.
 *
 * Licence texts live in `assets/licenses/`. When a library is added to or removed from the
 * APK (vendored code under src/, or Gradle dependencies), update [components] and the files
 * there to match.
 */
object AboutDialogs {
    const val SOURCE_URL = "https://github.com/izzy2lost/DSperate"
    const val WEBSITE_URL = "https://www.izzy2lost.com/ninds"
    const val PRIVACY_POLICY_URL = "https://www.izzy2lost.com/ninds-privacy"

    private data class Credit(val name: String, val role: String, val url: String? = null)

    private data class Component(
        val name: String,
        val licence: String,
        val url: String,
        /** Shown in order, separated by a rule. */
        val assetFiles: List<String>,
    )

    private val credits = listOf(
        Credit("izzy2lost", "NinDS Android port", SOURCE_URL),
        Credit(
            "Noxwell (beebono)",
            "DSperate, the Nintendo DS emulator whose core runs every game here",
            "https://github.com/beebono/DSperate",
        ),
        Credit(
            "The melonDS team",
            "The reference DSperate is verified against, and a source of code with attribution",
            "https://melonds.kuribo64.net",
        ),
        Credit("Exophase", "DraStic, whose techniques DSperate reimplements from its public debug build"),
        Credit("Gilead Kutnick", "FreeBIOS, the built-in BIOS replacement", null),
        Credit("Kronos and Yabause authors", "6x xBRZ upscaling shader", "https://github.com/FCare/Kronos"),
        Credit("GameTDB", "Box art and game titles", "https://www.gametdb.com"),
        Credit("ds-covers", "Box art hosting for NinDS", "https://github.com/izzy2lost/ds-covers"),
        Credit("Wiimmfi", "Community replacement for Nintendo Wi-Fi Connection", "https://wiimmfi.de"),
    )

    private val components = listOf(
        Component("NinDS", "GPL-3.0", SOURCE_URL, listOf("GPL-3.0.txt")),
        Component("DSperate", "GPL-3.0", "https://github.com/beebono/DSperate", listOf("GPL-3.0.txt")),
        Component("FreeBIOS", "BSD-style", "https://github.com/beebono/DSperate", listOf("FreeBIOS-LICENSE.txt")),
        Component(
            "6x xBRZ shader (Kronos / Yabause)",
            "GPL-2.0-or-later",
            "https://github.com/FCare/Kronos",
            listOf("xBRZ-shader-NOTICE.txt", "GPL-2.0.txt"),
        ),
        Component("miniz", "MIT", "https://github.com/richgel999/miniz", listOf("miniz-LICENSE.txt")),
        Component("ENet", "MIT", "https://github.com/lsalzman/enet", listOf("enet-LICENSE.txt")),
        Component("libslirp", "BSD-3-Clause", "https://gitlab.freedesktop.org/slirp/libslirp", listOf("libslirp-LICENSE.txt")),
        Component("Noto Sans", "OFL-1.1", "https://fonts.google.com/noto", listOf("NotoSans-OFL-1.1.txt")),
        Component(
            "WenQuanYi Micro Hei",
            "GPL-3.0+ with font exception",
            "http://wenq.org",
            listOf("WenQuanYi-MicroHei-LICENSE.txt"),
        ),
        Component("AndroidX", "Apache-2.0", "https://developer.android.com/jetpack/androidx", listOf("Apache-2.0.txt")),
        Component(
            "Material Components for Android",
            "Apache-2.0",
            "https://github.com/material-components/material-components-android",
            listOf("Apache-2.0.txt"),
        ),
        Component("Kotlin standard library", "Apache-2.0", "https://kotlinlang.org", listOf("Apache-2.0.txt")),
    )

    fun showCredits(activity: Activity) {
        val list = listContainer(activity)
        for (credit in credits) {
            list.addView(entryView(activity, credit.name, credit.role, credit.url) {
                credit.url?.let { openUrl(activity, it) }
            })
        }
        list.addView(bodyText(activity, activity.getString(R.string.about_trademark_notice)).apply {
            setPadding(dp(activity, 24), dp(activity, 16), dp(activity, 24), dp(activity, 8))
            alpha = 0.7f
        })
        show(activity, R.string.about_credits, list)
    }

    fun showLicences(activity: Activity) {
        val list = listContainer(activity)
        for (component in components) {
            list.addView(entryView(activity, component.name, component.licence, component.url) {
                showLicenceText(activity, component)
            })
        }
        show(activity, R.string.about_licenses, list)
    }

    fun openSource(activity: Activity) = openUrl(activity, SOURCE_URL)

    fun openWebsite(activity: Activity) = openUrl(activity, WEBSITE_URL)

    fun openPrivacyPolicy(activity: Activity) = openUrl(activity, PRIVACY_POLICY_URL)

    private fun showLicenceText(activity: Activity, component: Component) {
        val text = component.assetFiles.joinToString("\n\n────────────────────\n\n") { file ->
            runCatching {
                activity.assets.open("licenses/$file").bufferedReader().use { it.readText().trimEnd() }
            }.getOrElse { "($file is missing from this build)" }
        }
        val body = TextView(activity).apply {
            this.text = text
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextIsSelectable(true)
            setPadding(dp(activity, 24), dp(activity, 8), dp(activity, 24), dp(activity, 8))
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle("${component.name} · ${component.licence}")
            .setView(ScrollView(activity).apply { addView(body) })
            .setNeutralButton(R.string.about_open_website) { _, _ -> openUrl(activity, component.url) }
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun show(activity: Activity, @StringRes title: Int, list: LinearLayout) {
        MaterialAlertDialogBuilder(activity)
            .setTitle(title)
            .setView(ScrollView(activity).apply { addView(list) })
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun listContainer(activity: Activity) = LinearLayout(activity).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(activity, 8), 0, dp(activity, 8))
    }

    private fun entryView(activity: Activity, title: String, subtitle: String, url: String?, onClick: () -> Unit): View {
        val row = LayoutInflater.from(activity).inflate(R.layout.item_about_entry, null, false)
        row.findViewById<TextView>(R.id.aboutEntryTitle).text = title
        row.findViewById<TextView>(R.id.aboutEntrySubtitle).text = subtitle
        row.findViewById<TextView>(R.id.aboutEntryLink).apply {
            text = url?.removePrefix("https://")?.removePrefix("http://")
            visibility = if (url != null) View.VISIBLE else View.GONE
        }
        row.setOnClickListener { onClick() }
        return row
    }

    private fun bodyText(activity: Activity, text: String) = TextView(activity).apply {
        this.text = text
        setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
    }

    private fun openUrl(activity: Activity, url: String) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: ActivityNotFoundException) {
            Toast.makeText(activity, url, Toast.LENGTH_LONG).show()
        }
    }

    private fun dp(activity: Activity, value: Int): Int =
        (value * activity.resources.displayMetrics.density).toInt()
}
