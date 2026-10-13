package com.izzy2lost.ninds

import android.graphics.BitmapFactory
import android.graphics.drawable.BitmapDrawable
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.izzy2lost.ninds.databinding.ItemGameBinding

class GameAdapter(
    private val onClick: (Game) -> Unit,
    private val onLongClick: (Game) -> Unit,
) : RecyclerView.Adapter<GameAdapter.Holder>() {
    private var items: List<Game> = emptyList()

    class Holder(val binding: ItemGameBinding) : RecyclerView.ViewHolder(binding.root)

    @android.annotation.SuppressLint("NotifyDataSetChanged")
    fun submit(list: List<Game>) {
        items = list
        notifyDataSetChanged()
    }

    override fun getItemCount() = items.size

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        Holder(ItemGameBinding.inflate(LayoutInflater.from(parent.context), parent, false))

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val game = items[position]
        val b = holder.binding
        b.gameTitle.text = game.title
        b.gamePublisher.text = listOf(game.publisher, game.gameCode).filter { it.isNotBlank() }.joinToString(" · ")
        val bmp = game.iconPath?.let { BitmapFactory.decodeFile(it) }
        if (bmp != null) {
            // 32x32 pixel art: scale it up without smoothing.
            b.gameIcon.setImageDrawable(BitmapDrawable(b.root.resources, bmp).apply { isFilterBitmap = false })
        } else {
            b.gameIcon.setImageResource(R.drawable.ic_cartridge)
        }
        b.root.setOnClickListener { onClick(game) }
        b.root.setOnLongClickListener { onLongClick(game); true }
    }
}
