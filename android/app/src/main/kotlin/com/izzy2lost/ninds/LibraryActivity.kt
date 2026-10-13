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
import java.util.concurrent.Executors

class LibraryActivity : AppCompatActivity() {
    private lateinit var binding: ActivityLibraryBinding
    private lateinit var settings: Settings
    private lateinit var library: GameLibrary
    private lateinit var paths: AppPaths
    private val adapter = GameAdapter(::launch, ::showGameOptions)
    private val worker = Executors.newSingleThreadExecutor()
    private var games: List<Game> = emptyList()
    private var query = ""
    private var scanning = false
    private var pendingSaveGame: Game? = null

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

        val span = maxOf(2, (resources.configuration.screenWidthDp / 170))
        binding.gameList.layoutManager = GridLayoutManager(this, span)
        binding.gameList.adapter = adapter
        binding.refresh.setOnRefreshListener { rescan() }
        binding.chooseFolderButton.setOnClickListener { pickFolder.launch(initialFolder()) }
        binding.openFileButton.setOnClickListener { openFile.launch(arrayOf("*/*")) }

        games = library.cached()
        render()
        if (savedInstanceState == null && intent?.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
        else if (settings.romTreeUri.isNotEmpty() && games.isEmpty()) rescan()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
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
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean = when (item.itemId) {
        R.id.action_refresh -> { rescan(); true }
        R.id.action_folder -> { pickFolder.launch(initialFolder()); true }
        R.id.action_open_file -> { openFile.launch(arrayOf("*/*")); true }
        R.id.action_settings -> { startActivity(Intent(this, SettingsActivity::class.java)); true }
        else -> super.onOptionsItemSelected(item)
    }

    private fun render() {
        val shown = if (query.isBlank()) games else games.filter {
            it.title.contains(query, true) || it.fileName.contains(query, true) || it.gameCode.contains(query, true)
        }
        adapter.submit(shown)
        val noFolder = settings.romTreeUri.isEmpty()
        binding.emptyState.visibility = if (games.isEmpty() && !scanning) View.VISIBLE else View.GONE
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
                result.onSuccess { games = it }
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
            .setTitle(game.title)
            .setItems(options.map { getString(it.first) }.toTypedArray()) { _, which -> options[which].second() }
            .show()
    }

    private fun confirmDeleteSave(game: Game) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.game_delete_save)
            .setMessage(getString(R.string.delete_save_confirm, game.title))
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
