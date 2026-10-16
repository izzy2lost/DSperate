package com.izzy2lost.ninds

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.enableEdgeToEdge
import android.provider.DocumentsContract
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SearchView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.izzy2lost.ninds.databinding.ActivityLibraryBinding
import java.util.Locale
import java.util.concurrent.Executors

class LibraryActivity : AppCompatActivity() {
    private lateinit var binding: ActivityLibraryBinding
    private lateinit var settings: Settings
    private lateinit var library: GameLibrary
    private lateinit var paths: AppPaths
    private lateinit var covers: Covers
    private lateinit var adapter: GameAdapter
    private val worker = Executors.newSingleThreadExecutor()
    private val downloader = Executors.newSingleThreadExecutor()
    private var titles: Map<String, String> = emptyMap()
    private var downloading = false
    /** Box art is fetched by itself once per visit; after that only when asked from the menu. */
    private var autoDownloaded = false
    private var games: List<Game> = emptyList()
    private var query = ""
    private var scanning = false
    private var pendingSaveGame: Game? = null
    private val themeGeneration = NinDSApp.themeGeneration

    private val pickFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@registerForActivityResult
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        settings.romTreeUri = uri.toString()
        rescan()
    }

    private val openFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@registerForActivityResult
        runCatching { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        addAndLaunch(uri)
    }

    /** A game opened on its own: from the file picker, or "Open with" in a file manager. */
    private fun addAndLaunch(uri: Uri) {
        worker.execute {
            val game = library.addFile(uri)
            runOnUiThread {
                if (game == null) {
                    Toast.makeText(this, R.string.open_file_failed, Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                games = library.cached()
                render()
                launch(game)
            }
        }
    }

    private val importSave = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val game = pendingSaveGame ?: return@registerForActivityResult
        if (uri == null) return@registerForActivityResult
        val ok = runCatching {
            val target = paths.savFile(game.stem)
            if (target.isFile) target.copyTo(java.io.File(target.path + ".bak"), overwrite = true)
            contentResolver.openInputStream(uri)!!.use { input -> target.outputStream().use { input.copyTo(it) } }
        }.isSuccess
        Toast.makeText(this, if (ok) R.string.save_imported else R.string.save_import_failed, Toast.LENGTH_SHORT).show()
    }

    private val exportSave = registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val game = pendingSaveGame ?: return@registerForActivityResult
        if (uri == null) return@registerForActivityResult
        val ok = runCatching {
            contentResolver.openOutputStream(uri)!!.use { out -> paths.savFile(game.stem).inputStream().use { it.copyTo(out) } }
        }.isSuccess
        Toast.makeText(this, if (ok) R.string.save_exported else R.string.save_export_failed, Toast.LENGTH_SHORT).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        settings = Settings.get(this)
        library = GameLibrary(this)
        paths = AppPaths(this)
        covers = Covers(this)
        adapter = GameAdapter(covers, ::launch, ::showGameOptions)
        adapter.showCovers = settings.libraryCovers
        binding = ActivityLibraryBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)

        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            binding.appBar.updatePadding(top = bars.top, left = bars.left, right = bars.right)
            binding.gameList.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            binding.emptyState.updatePadding(bottom = bars.bottom)
            insets
        }

        binding.libraryTipClose.setOnClickListener {
            settings.libraryTipDismissed = true
            binding.libraryTip.visibility = View.GONE
        }
        binding.gameList.layoutManager = GridLayoutManager(this, spanCount())
        binding.gameList.adapter = adapter
        binding.refresh.setOnRefreshListener { rescan() }
        binding.chooseFolderButton.setOnClickListener { pickFolder.launch(initialFolder()) }
        binding.openFileButton.setOnClickListener { openFile.launch(arrayOf("*/*")) }

        games = library.cached()
        render()
        worker.execute {
            val t = covers.titles()
            runOnUiThread { titles = t; render() }
        }
        if (savedInstanceState == null && intent?.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
        else if (settings.romTreeUri.isNotEmpty() && games.isEmpty()) rescan()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
    }

    override fun onDestroy() {
        worker.shutdown()
        downloader.shutdownNow()
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        // The accent or black theme changed in Settings: pick it up.
        if (themeGeneration != NinDSApp.themeGeneration) recreate()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.library, menu)
        (menu.findItem(R.id.action_search).actionView as SearchView).apply {
            queryHint = getString(R.string.search_hint)
            setOnQueryTextListener(object : SearchView.OnQueryTextListener {
                override fun onQueryTextSubmit(q: String) = true
                override fun onQueryTextChange(q: String): Boolean {
                    this@LibraryActivity.query = q
                    render()
                    return true
                }
            })
        }
        updateViewItem(menu.findItem(R.id.action_view))
        return true
    }

    private fun updateViewItem(item: MenuItem?) {
        item ?: return
        item.setIcon(if (adapter.showCovers) R.drawable.ic_icons else R.drawable.ic_covers)
        item.setTitle(if (adapter.showCovers) R.string.action_show_icons else R.string.action_show_covers)
    }

    /** Covers are larger than banner icons, so they get narrower columns. */
    private fun spanCount() = maxOf(2, resources.configuration.screenWidthDp / if (adapter.showCovers) 150 else 170)

    private fun setShowCovers(show: Boolean) {
        settings.libraryCovers = show
        adapter.showCovers = show
        (binding.gameList.layoutManager as GridLayoutManager).spanCount = spanCount()
        invalidateOptionsMenu()
        if (show) autoDownloadCovers()
    }

    /** Fetches the box art the library is missing, once per visit, while box art is shown. */
    private fun autoDownloadCovers() {
        if (autoDownloaded || !adapter.showCovers || games.isEmpty()) return
        autoDownloaded = true
        worker.execute {
            val missing = games.map { it.gameCode }.filter { it.length == 4 && covers.coverFile(it) == null }
            if (missing.isNotEmpty()) runOnUiThread { downloadCovers(missing, refresh = false, quiet = true) }
        }
    }

    private fun downloadCovers(codes: List<String>, refresh: Boolean, quiet: Boolean) {
        if (downloading) return
        downloading = true
        downloader.execute {
            val result = runCatching {
                covers.download(codes, refresh) { done, total ->
                    runOnUiThread { if (!scanning) supportActionBar?.subtitle = getString(R.string.covers_progress, done, total) }
                }
            }
            val t = covers.titles()
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                downloading = false
                if (!scanning) supportActionBar?.subtitle = null
                titles = t
                adapter.coversChanged()
                render()
                result.onSuccess { if (!quiet) Toast.makeText(this, getString(R.string.covers_done, it.withCover, it.games), Toast.LENGTH_LONG).show() }
                    .onFailure { if (!quiet) Toast.makeText(this, getString(R.string.covers_failed, it.message ?: ""), Toast.LENGTH_LONG).show() }
            }
        }
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean = when (item.itemId) {
        R.id.action_refresh -> { rescan(); true }
        R.id.action_view -> { setShowCovers(!adapter.showCovers); true }
        R.id.action_download_covers -> { downloadCovers(games.map { it.gameCode }, refresh = true, quiet = false); true }
        R.id.action_folder -> { pickFolder.launch(initialFolder()); true }
        R.id.action_open_file -> { openFile.launch(arrayOf("*/*")); true }
        R.id.action_settings -> { startActivity(Intent(this, SettingsActivity::class.java)); true }
        else -> super.onOptionsItemSelected(item)
    }

    private fun render() {
        val shown = (if (query.isBlank()) games else games.filter {
            it.title.contains(query, true) || it.fileName.contains(query, true) || it.gameCode.contains(query, true) ||
                titles[it.gameCode]?.contains(query, true) == true
        }).sortedBy { (titles[it.gameCode] ?: it.title).lowercase(Locale.ROOT) }
        adapter.submit(shown, titles)
        autoDownloadCovers()
        val noFolder = settings.romTreeUri.isEmpty()
        binding.emptyState.visibility = if (games.isEmpty() && !scanning) View.VISIBLE else View.GONE
        binding.libraryTip.visibility = if (games.isNotEmpty() && !settings.libraryTipDismissed) View.VISIBLE else View.GONE
        binding.emptyText.setText(if (noFolder) R.string.library_empty_no_folder else R.string.library_empty_no_games)
        binding.chooseFolderButton.setText(if (noFolder) R.string.choose_folder else R.string.choose_other_folder)
    }

    private fun rescan() {
        val tree = settings.romTreeUri.takeIf { it.isNotEmpty() }?.let(Uri::parse)
        if (tree == null) {
            binding.refresh.isRefreshing = false
            pickFolder.launch(initialFolder())
            return
        }
        if (scanning) return
        scanning = true
        binding.refresh.isRefreshing = true
        binding.emptyState.visibility = View.GONE
        worker.execute {
            val result = runCatching {
                library.scan(tree) { done, total ->
                    runOnUiThread { supportActionBar?.subtitle = getString(R.string.scanning_progress, done + 1, total) }
                }
            }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                scanning = false
                binding.refresh.isRefreshing = false
                supportActionBar?.subtitle = null
                result.onSuccess { games = it; autoDownloaded = false }
                    .onFailure { Toast.makeText(this, getString(R.string.scan_failed, it.message ?: ""), Toast.LENGTH_LONG).show() }
                render()
            }
        }
    }

    /** Where the folder picker opens: the current ROM folder, else internal storage. */
    private fun initialFolder(): Uri? {
        settings.romTreeUri.takeIf { it.isNotEmpty() }?.let { tree ->
            return runCatching {
                val t = Uri.parse(tree)
                DocumentsContract.buildDocumentUriUsingTree(t, DocumentsContract.getTreeDocumentId(t))
            }.getOrNull()
        }
        return DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:")
    }

    private fun launch(game: Game) = startActivity(EmulatorActivity.intent(this, game))

    private fun showGameOptions(game: Game) {
        val auto = SaveStateSlot(paths.stateDir(game.stem), SaveStateSlot.AUTO)
        val hasSave = paths.savFile(game.stem).isFile
        val options = mutableListOf<Pair<Int, () -> Unit>>()
        options += R.string.game_play to { launch(game) }
        if (auto.exists) options += R.string.game_resume to { startActivity(EmulatorActivity.intent(this, game, SaveStateSlot.AUTO)) }
        options += R.string.game_import_save to { pendingSaveGame = game; importSave.launch(arrayOf("*/*")) }
        if (hasSave) options += R.string.game_export_save to { pendingSaveGame = game; exportSave.launch("${game.stem}.sav") }
        if (hasSave) options += R.string.game_delete_save to { confirmDeleteSave(game) }
        MaterialAlertDialogBuilder(this)
            .setTitle(adapter.titleOf(game))
            .setItems(options.map { getString(it.first) }.toTypedArray()) { _, which -> options[which].second() }
            .show()
    }

    private fun confirmDeleteSave(game: Game) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.game_delete_save)
            .setMessage(getString(R.string.delete_save_confirm, adapter.titleOf(game)))
            .setPositiveButton(R.string.delete) { _, _ ->
                // Kept as .bak rather than gone: one tap should not cost a hundred hours.
                val f = paths.savFile(game.stem)
                f.renameTo(java.io.File(f.path + ".bak"))
                Toast.makeText(this, R.string.save_deleted, Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }
}
