package com.izzy2lost.ninds

import android.app.Application
import androidx.appcompat.app.AppCompatDelegate

/**
 * Applies the stored theme before any activity inflates, so the first frame is already in the
 * right mode rather than flashing the system default and recreating.
 */
class NinDSApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AppCompatDelegate.setDefaultNightMode(nightMode(Settings.get(this).theme))
    }

    companion object {
        fun nightMode(theme: String) = when (theme) {
            "light" -> AppCompatDelegate.MODE_NIGHT_NO
            "dark" -> AppCompatDelegate.MODE_NIGHT_YES
            else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
        }
    }
}
