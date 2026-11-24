package com.izzy2lost.ninds

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.BitmapDrawable
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.LruCache
import android.view.KeyEvent
import android.view.LayoutInflater
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors
import android.view.ViewGroup
import android.widget.ImageView
import androidx.appcompat.widget.AppCompatImageView
import androidx.recyclerview.widget.RecyclerView
import com.izzy2lost.ninds.databinding.ItemCoverBinding
import com.izzy2lost.ninds.databinding.ItemGameBinding
import java.util.concurrent.Executors

class GameAdapter(
    private val covers: Covers,
    private val onClick: (Game) -> Unit,
    private val onLongClick: (Game) -> Unit,
) : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
    private var items: List<Game> = emptyList()
    private var titles: Map<String, String> = emptyMap()

    /** Box art instead of banner icons. */
    var showCovers = false
        @android.annotation.SuppressLint("NotifyDataSetChanged")
        set(value) {
            if (field != value) {
                field = value
                notifyDataSetChanged()
            }
        }

    class IconHolder(val binding: ItemGameBinding) : RecyclerView.ViewHolder(binding.root)
    class CoverHolder(val binding: ItemCoverBinding) : RecyclerView.ViewHolder(binding.root)

    /** The title to show: GameTDB's when it has the game, else the cartridge banner's. */
    fun titleOf(game: Game) = titles[game.gameCode] ?: game.title

    @android.annotation.SuppressLint("NotifyDataSetChanged")
    fun submit(list: List<Game>, titles: Map<String, String>) {
        items = list
        this.titles = titles
        notifyDataSetChanged()
    }

    /** Covers on disk changed (a download finished): drop the remembered misses and redraw. */
    @android.annotation.SuppressLint("NotifyDataSetChanged")
    fun coversChanged() {
        loader.forgetMisses()
        notifyDataSetChanged()
    }

    // Stable ids keep the same card (and a remote's focus on it) through a refresh.
    init { setHasStableIds(true) }

    override fun getItemId(position: Int): Long {
        var h = -0x340d631b7bdddcdbL   // FNV-1a, 64-bit, of the game's location
        for (c in items[position].uri) { h = (h xor c.code.toLong()) * 0x100000001b3L }
        return h
    }

    override fun getItemCount() = items.size

    override fun getItemViewType(position: Int) = if (showCovers) 1 else 0

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
        val inflater = LayoutInflater.from(parent.context)
        return if (viewType == 1) CoverHolder(ItemCoverBinding.inflate(inflater, parent, false))
        else IconHolder(ItemGameBinding.inflate(inflater, parent, false))
    }

    override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
        val game = items[position]
        when (holder) {
            is IconHolder -> {
                val b = holder.binding
                b.gameTitle.text = titleOf(game)
                b.gamePublisher.text = listOf(game.publisher, game.gameCode).filter { it.isNotBlank() }.joinToString(" · ")
                val bmp = game.iconPath?.let { BitmapFactory.decodeFile(it) }
                if (bmp != null) {
                    // 32x32 pixel art: scale it up without smoothing.
                    b.gameIcon.setImageDrawable(BitmapDrawable(b.root.resources, bmp).apply { isFilterBitmap = false })
                } else {
                    b.gameIcon.setImageResource(R.drawable.ic_cartridge)
                }
            }
            is CoverHolder -> {
                val b = holder.binding
                b.gameTitle.text = titleOf(game)
                loader.load(b.gameCover, game)
            }
        }
        holder.itemView.setOnClickListener { onClick(game) }
        holder.itemView.setOnLongClickListener { onLongClick(game); true }
        // A pad's Y opens the game's options, as a long press does (a remote's Menu: LibraryActivity).
        holder.itemView.setOnKeyListener { _, keyCode, event ->
            val menu = keyCode == KeyEvent.KEYCODE_BUTTON_Y
            if (menu && event.action == KeyEvent.ACTION_UP) onLongClick(game)
            menu
        }
        // The card under the D-pad stands out (Material's own focus tint is too faint for a TV).
        val card = holder.itemView as MaterialCardView
        fun showFocus(focused: Boolean) {
            card.strokeWidth = if (focused) (3 * card.resources.displayMetrics.density).toInt() else 0
            card.animate().scaleX(if (focused) 1.06f else 1f).scaleY(if (focused) 1.06f else 1f).setDuration(120).start()
        }
        card.strokeColor = MaterialColors.getColor(card, androidx.appcompat.R.attr.colorPrimary)
        card.setOnFocusChangeListener { _, focused -> showFocus(focused) }
        card.strokeWidth = 0
        card.scaleX = if (card.isFocused) 1.06f else 1f
        card.scaleY = card.scaleX
        if (card.isFocused) showFocus(true)
    }

    private val loader = CoverLoader(covers)
}

/**
 * Decodes covers off the main thread and keeps the recent ones in memory, so scrolling back
 * through the library does not decode them again. A game with no cover shows its banner icon.
 */
private class CoverLoader(private val covers: Covers) {
    private val cache = object : LruCache<String, Bitmap>((Runtime.getRuntime().maxMemory() / 8).toInt()) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val misses = HashSet<String>()
    private val pool = Executors.newFixedThreadPool(2)
    private val main = Handler(Looper.getMainLooper())

    fun forgetMisses() = misses.clear()

    fun load(view: ImageView, game: Game) {
        val key = game.gameCode
        view.tag = key
        cache.get(key)?.let { view.setImageBitmap(it); return }
        showFallback(view, game)
        if (key in misses) return
        pool.execute {
            val bmp = covers.coverFile(key)?.let { BitmapFactory.decodeFile(it.path) }
            main.post {
                if (bmp == null) misses += key else cache.put(key, bmp)
                if (view.tag == key && bmp != null) view.setImageBitmap(bmp)
            }
        }
    }

    private fun showFallback(view: ImageView, game: Game) {
        val bmp = game.iconPath?.let { BitmapFactory.decodeFile(it) }
        if (bmp != null) view.setImageDrawable(BitmapDrawable(view.resources, bmp).apply { isFilterBitmap = false })
        else view.setImageResource(R.drawable.ic_cartridge)
    }
}

/** An image view as tall as a GameTDB DS cover (10:9) for its width, so rows never jump. */
class CoverImageView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) :
    AppCompatImageView(context, attrs) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        setMeasuredDimension(w, w * 9 / 10)
    }
}
