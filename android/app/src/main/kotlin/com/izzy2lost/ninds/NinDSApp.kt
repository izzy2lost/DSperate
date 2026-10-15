package com.izzy2lost.ninds

import android.app.Activity
import android.app.Application
import android.graphics.drawable.ColorDrawable
import android.os.Bundle
import androidx.appcompat.app.AppCompatDelegate
import com.google.android.material.color.DynamicColors

/**
 * Applies the stored theme before any activity inflates, so the first frame is already in the
 * right mode rather than flashing the system default and recreating. The accent and the black
 * theme are overlays laid on each activity's theme as it is created.
 */
class NinDSApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AppCompatDelegate.setDefaultNightMode(nightMode(Settings.get(this).theme))
        registerActivityLifecycleCallbacks(object : ActivityLifecycleCallbacks {
            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = applyOverlays(activity)
            override fun onActivityStarted(activity: Activity) = Unit
            override fun onActivityResumed(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) = Unit
            override fun onActivityStopped(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
            override fun onActivityDestroyed(activity: Activity) = Unit
        })
    }

    private fun applyOverlays(activity: Activity) {
        val settings = Settings.get(activity)
        val accent = settings.accent
        if (accent == "wallpaper") DynamicColors.applyToActivityIfAvailable(activity)
        else ACCENTS[accent]?.let { activity.theme.applyStyle(it, true) }
        if (settings.theme == "black") {
            activity.theme.applyStyle(R.style.ThemeOverlay_NinDS_Black, true)
            if (activity !is EmulatorActivity) activity.window.setBackgroundDrawable(ColorDrawable(android.graphics.Color.BLACK))
        }
    }

    companion object {
        private val ACCENTS = mapOf(
            "blue" to R.style.ThemeOverlay_NinDS_Accent_Blue,
            "purple" to R.style.ThemeOverlay_NinDS_Accent_Purple,
            "green" to R.style.ThemeOverlay_NinDS_Accent_Green,
            "orange" to R.style.ThemeOverlay_NinDS_Accent_Orange,
            "pink" to R.style.ThemeOverlay_NinDS_Accent_Pink,
            "teal" to R.style.ThemeOverlay_NinDS_Accent_Teal,
            "gold" to R.style.ThemeOverlay_NinDS_Accent_Gold,
        )

        /** Bumped when the accent or black theme changes, so open activities know to recreate. */
        @Volatile
        var themeGeneration = 0

        fun nightMode(theme: String) = when (theme) {
            "light" -> AppCompatDelegate.MODE_NIGHT_NO
            "dark", "black" -> AppCompatDelegate.MODE_NIGHT_YES
            else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
        }
    }
}
