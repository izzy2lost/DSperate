package com.izzy2lost.ninds

import android.os.Bundle
import androidx.activity.enableEdgeToEdge
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.Menu
import android.view.MenuItem
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.izzy2lost.ninds.databinding.ActivityControllerMappingBinding
import com.izzy2lost.ninds.databinding.ItemMappingBinding

/** Binds gamepad and keyboard buttons to DS buttons and emulator hotkeys. */
class ControllerMappingActivity : AppCompatActivity() {
    private lateinit var mapping: InputMapping
    private val adapter = Adapter()

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        mapping = InputMapping(Settings.get(this))
        val binding = ActivityControllerMappingBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        binding.toolbar.setNavigationOnClickListener { finish() }
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            binding.appBar.updatePadding(top = bars.top, left = bars.left, right = bars.right)
            binding.mappingList.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            insets
        }
        binding.mappingList.layoutManager = LinearLayoutManager(this)
        binding.mappingList.adapter = adapter
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menu.add(0, 1, 0, R.string.mapping_reset)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId != 1) return super.onOptionsItemSelected(item)
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.mapping_reset)
            .setMessage(R.string.mapping_reset_confirm)
            .setPositiveButton(R.string.mapping_reset) { _, _ -> mapping.resetDefaults(); refresh() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        return true
    }

    @android.annotation.SuppressLint("NotifyDataSetChanged")
    private fun refresh() = adapter.notifyDataSetChanged()

    private fun listen(action: InputAction) {
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(action.label)
            .setMessage(R.string.mapping_press)
            .setNegativeButton(android.R.string.cancel, null)
            .setNeutralButton(R.string.mapping_clear) { _, _ -> mapping.clear(action); refresh() }
            .create()
        dialog.setOnKeyListener { d, keyCode, event ->
            // Back stays the way out of the dialog, unless a gamepad sent it.
            if (keyCode == KeyEvent.KEYCODE_BACK && !InputMapping.isGamepad(event.device)) return@setOnKeyListener false
            if (event.action == KeyEvent.ACTION_UP) {
                mapping.bind(action, keyCode)
                refresh()
                d.dismiss()
            }
            true
        }
        dialog.show()
    }

    private inner class Adapter : RecyclerView.Adapter<Adapter.Holder>() {
        inner class Holder(val b: ItemMappingBinding) : RecyclerView.ViewHolder(b.root)

        override fun getItemCount() = InputAction.entries.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            Holder(ItemMappingBinding.inflate(LayoutInflater.from(parent.context), parent, false))

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val action = InputAction.entries[position]
            holder.b.mappingAction.text = action.label
            val keys = mapping.keysFor(action)
            holder.b.mappingKeys.text =
                if (keys.isEmpty()) getString(R.string.mapping_unbound) else keys.joinToString(", ") { InputMapping.keyName(it) }
            holder.b.root.setOnClickListener { listen(action) }
        }
    }
}
