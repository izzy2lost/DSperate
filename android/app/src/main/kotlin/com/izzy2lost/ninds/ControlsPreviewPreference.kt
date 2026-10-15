package com.izzy2lost.ninds

import android.content.Context
import android.util.AttributeSet
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder

/** A live, pressable sample of the on-screen controls in the chosen skin. */
class ControlsPreviewPreference(context: Context, attrs: AttributeSet?) : Preference(context, attrs) {
    init {
        layoutResource = R.layout.preference_controls_preview
        isSelectable = false
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        holder.itemView.isClickable = false
        val host = holder.findViewById(R.id.previewHost) as FrameLayout
        val view = host.getChildAt(0) as? TouchControlsView ?: TouchControlsView(context).also {
            it.preview = true
            it.scale = 0.7f
            host.addView(it, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        val settings = Settings.get(context)
        view.skin = ControlSkins.get(context, settings.controlsSkin)
        view.faceColours = settings.faceColours
        view.haptics = settings.haptics
        view.opacity = 1f
        view.relayout()
    }

    /** Redraw with whatever the settings now say. */
    fun refresh() = notifyChanged()
}
