package com.izzy2lost.ninds

import android.app.Activity.RESULT_OK
import android.content.ActivityNotFoundException
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.result.contract.ActivityResultContract
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
import androidx.recyclerview.widget.RecyclerView
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
    private var gamesFocusedOnce = false
    private val themeGeneration = NinDSApp.themeGeneration

    private val pickFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@registerForActivityResult
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        settings.romTreeUri = uri.toString()
        rescan()
    }

    /**
     * "Add games": files picked in whatever file manager the device has, copied into NinDS's own
     * Games folder. For Android TV and other devices whose folder picker is a stub; a copy keeps
     * working after a restart, where a one-off grant from a file manager would not.
     */
    private val importGames = registerForActivityResult(PickGames()) { uris ->
        if (uris.isNotEmpty()) importAll(uris)
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
                play(game)
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
        adapter = GameAdapter(covers, ::play, ::showGameOptions)
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

        if (!packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN)) {
            binding.libraryTipText.setText(R.string.library_tip_tv)
        }
        binding.libraryTipClose.setOnClickListener {
            settings.libraryTipDismissed = true
            binding.libraryTip.visibility = View.GONE
        }
        binding.gameList.layoutManager = GridLayoutManager(this, spanCount())
        binding.gameList.adapter = adapter
        // Only its games take focus: an empty list holding it would swallow a remote's OK.
        binding.gameList.isFocusable = false
        binding.refresh.setOnRefreshListener { rescan() }
        binding.chooseFolderButton.setOnClickListener { chooseFolder() }
        binding.openFileButton.setOnClickListener { if (folderPickerWorks) openFile.launch(arrayOf("*/*")) else addGames() }

        games = library.cached()
        render()
        worker.execute {
            val t = covers.titles()
            runOnUiThread { titles = t; render() }
        }
        if (savedInstanceState == null) relaunch(intent)
        if (savedInstanceState == null && intent?.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
        else if (games.isEmpty() && (settings.romTreeUri.isNotEmpty() || !folderPickerWorks)) rescan()
    }

    /**
     * Whether the system folder picker is real. Android TV ships a stub in its place
     * (com.android.tv.frameworkpackagestubs) that closes at once, so there the library is built
     * with "Add games" instead.
     */
    private val folderPickerWorks: Boolean by lazy {
        val r = packageManager.resolveActivity(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), 0)
        r != null && !r.activityInfo.packageName.startsWith(STUB_PACKAGE)
    }

    private fun chooseFolder() {
        if (!folderPickerWorks) { addGames(); return }
        try {
            pickFolder.launch(initialFolder())
        } catch (_: ActivityNotFoundException) {
            addGames()
        }
    }

    /** Opens a file manager for picking games to copy in, or says how to get one. */
    private fun addGames() {
        try {
            importGames.launch(Unit)
        } catch (_: ActivityNotFoundException) {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.add_games)
                .setMessage(getString(R.string.add_games_no_picker, paths.games.path))
                .setPositiveButton(android.R.string.ok, null)
                .show()
        }
    }

    private fun importAll(uris: List<Uri>) {
        if (scanning) return
        scanning = true
        binding.refresh.isRefreshing = true
        binding.emptyState.visibility = View.GONE
        worker.execute {
            var added = 0
            var rejected = 0
            var failed: String? = null
            uris.forEachIndexed { i, uri ->
                var lastPct = -1
                val r = runCatching {
                    library.importFile(uri) { copied, total ->
                        val pct = if (total > 0) (copied * 100 / total).toInt() else -1
                        if (pct != lastPct) {
                            lastPct = pct
                            runOnUiThread {
                                supportActionBar?.subtitle = if (pct >= 0) getString(R.string.add_games_progress_pct, i + 1, uris.size, pct)
                                else getString(R.string.add_games_progress, i + 1, uris.size)
                            }
                        }
                    }
                }
                r.onSuccess { if (it != null) added++ else rejected++ }.onFailure { failed = it.message ?: it.javaClass.simpleName }
            }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                scanning = false
                val msg = when {
                    failed != null -> getString(R.string.add_games_failed, failed)
                    rejected > 0 -> getString(R.string.add_games_done_skipped, added, rejected)
                    else -> resources.getQuantityString(R.plurals.add_games_done, added, added)
                }
                Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                rescan()
            }
        }
    }

    /** A game restarting to apply settings: started again once its old process has gone. */
    private fun relaunch(intent: Intent?) {
        @Suppress("DEPRECATION")
        val again = intent?.getParcelableExtra<Intent>(EXTRA_RELAUNCH) ?: return
        intent.removeExtra(EXTRA_RELAUNCH)
        // Started too early it would land on the old, dying instance (singleTask): wait for its
        // process to go, giving up the wait after a few seconds.
        val am = getSystemService(android.app.ActivityManager::class.java)
        val emu = "$packageName:emu"
        var tries = 0
        val check = object : Runnable {
            override fun run() {
                if (isFinishing) return
                val alive = am.runningAppProcesses?.any { it.processName == emu } == true
                if (alive && ++tries < 40) binding.root.postDelayed(this, 100) else startActivity(again)
            }
        }
        binding.root.postDelayed(check, 150)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        relaunch(intent)
        if (intent.action == Intent.ACTION_VIEW) intent.data?.let(::addAndLaunch)
    }

    override fun onDestroy() {
        worker.shutdown()
        downloader.shutdownNow()
        super.onDestroy()
    }

    /**
     * A remote's Menu key on a game opens its options, as a long press does. Here rather than on
     * the card: AppCompat takes Menu for the toolbar's overflow before any view sees it.
     */
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        val focused = currentFocus
        if (event.keyCode == android.view.KeyEvent.KEYCODE_MENU && focused?.parent == binding.gameList) {
            if (event.action == android.view.KeyEvent.ACTION_UP) focused.performLongClick()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // With a remote, start on the first game rather than the toolbar's first button.
        if (hasFocus && !gamesFocusedOnce) focusFirstGame()
    }

    /** Focuses the first game, waiting for the grid to lay its cards out if it has not yet. */
    private fun focusFirstGame() {
        val list = binding.gameList
        list.getChildAt(0)?.let { gamesFocusedOnce = it.requestFocus(); return }
        if (games.isEmpty()) return
        list.viewTreeObserver.addOnGlobalLayoutListener(object : android.view.ViewTreeObserver.OnGlobalLayoutListener {
            override fun onGlobalLayout() {
                val first = list.getChildAt(0) ?: return
                list.viewTreeObserver.removeOnGlobalLayoutListener(this)
                if (!gamesFocusedOnce) gamesFocusedOnce = first.requestFocus()
            }
        })
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
        menu.findItem(R.id.action_add_games)?.isVisible = !folderPickerWorks
        menu.findItem(R.id.action_folder)?.isVisible = folderPickerWorks
        menu.findItem(R.id.action_open_file)?.isVisible = folderPickerWorks
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
        R.id.action_folder -> { chooseFolder(); true }
        R.id.action_add_games -> { addGames(); true }
        R.id.action_open_file -> { openFile.launch(arrayOf("*/*")); true }
        R.id.action_settings -> { startActivity(Intent(this, SettingsActivity::class.java)); true }
        else -> super.onOptionsItemSelected(item)
    }

    private fun render() {
        val shown = (if (query.isBlank()) games else games.filter {
            it.title.contains(query, true) || it.fileName.contains(query, true) || it.gameCode.contains(query, true) ||
                titles[it.gameCode]?.contains(query, true) == true
        }).sortedBy { (titles[it.gameCode] ?: it.title).lowercase(Locale.ROOT) }
        val list = binding.gameList
        val focusedGame = currentFocus?.let { list.findContainingItemView(it) }?.let { list.getChildAdapterPosition(it) } ?: RecyclerView.NO_POSITION
        adapter.submit(shown, titles)
        // A remote's focus stays in the grid through the refresh (else it drops to the toolbar's first button).
        if (focusedGame != RecyclerView.NO_POSITION) list.post {
            if (currentFocus?.let { list.findContainingItemView(it) } == null && currentFocus !is android.widget.EditText && shown.isNotEmpty()) {
                val pos = focusedGame.coerceAtMost(shown.size - 1)
                (list.findViewHolderForAdapterPosition(pos)?.itemView ?: list.getChildAt(0))?.requestFocus()
            }
        }
        autoDownloadCovers()
        val noFolder = settings.romTreeUri.isEmpty()
        binding.emptyState.visibility = if (games.isEmpty() && !scanning) View.VISIBLE else View.GONE
        binding.libraryTip.visibility = if (games.isNotEmpty() && !settings.libraryTipDismissed) View.VISIBLE else View.GONE
        if (folderPickerWorks) {
            binding.emptyText.setText(if (noFolder) R.string.library_empty_no_folder else R.string.library_empty_no_games)
            binding.chooseFolderButton.setText(if (noFolder) R.string.choose_folder else R.string.choose_other_folder)
            binding.chooseFolderButton.setIconResource(R.drawable.ic_folder)
            binding.openFileButton.visibility = View.VISIBLE
        } else {
            binding.emptyText.text = getString(R.string.library_empty_add_games)
            binding.chooseFolderButton.setText(R.string.add_games)
            binding.chooseFolderButton.setIconResource(R.drawable.ic_cartridge)
            binding.openFileButton.visibility = View.GONE
        }
        // With a remote, focus has to start somewhere: on the button when the library is empty.
        if (binding.emptyState.visibility == View.VISIBLE && currentFocus?.parent != binding.emptyState) binding.chooseFolderButton.requestFocus()
        // ...and on the first game once there are games: the library can still be loading when the
        // window first gets focus, and focus can be lost (the button it was on went away). Never
        // away from the search box while typing.
        if (shown.isNotEmpty() && currentFocus !is android.widget.EditText) {
            if (gamesFocusedOnce && currentFocus?.isShown != true) gamesFocusedOnce = false
            if (!gamesFocusedOnce && hasWindowFocus()) focusFirstGame()
        }
    }

    private fun rescan() {
        val tree = settings.romTreeUri.takeIf { it.isNotEmpty() }?.let(Uri::parse)
        if (tree == null && folderPickerWorks && !library.hasLocalGames()) {
            binding.refresh.isRefreshing = false
            chooseFolder()
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

    private fun launch(game: Game) = start(game)

    /**
     * Starts the emulator on [game]. Android refuses the start when the app no longer holds
     * the ROM folder's grant: a reinstall drops it, while the settings (and with them the
     * folder) come back from the device backup. The folder is then asked for again.
     */
    private fun start(game: Game, loadSlot: Int = -1) {
        try {
            startActivity(EmulatorActivity.intent(this, game, loadSlot))
        } catch (_: SecurityException) {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.folder_access_lost_title)
                .setMessage(R.string.folder_access_lost)
                .setPositiveButton(R.string.choose_folder) { _, _ -> pickFolder.launch(initialFolder()) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    /**
     * A tap on a game. With an automatic save from last time, asks first: starting fresh by
     * accident would leave the player wondering where their session went.
     */
    private fun play(game: Game) {
        val auto = SaveStateSlot(paths.stateDir(game.stem), SaveStateSlot.AUTO)
        if (!auto.exists) { launch(game); return }
        val savedAgo = android.text.format.DateUtils.getRelativeTimeSpanString(
            auto.savedAt, System.currentTimeMillis(), android.text.format.DateUtils.MINUTE_IN_MILLIS,
        )
        val content = layoutInflater.inflate(R.layout.dialog_continue, null, false)
        auto.decodeThumbnail()?.let { content.findViewById<android.widget.ImageView>(R.id.continueThumbnail).setImageBitmap(it) }
            ?: run { content.findViewById<View>(R.id.continueThumbnail).visibility = View.GONE }
        content.findViewById<android.widget.TextView>(R.id.continueWhen).text = getString(R.string.continue_saved, savedAgo)
        MaterialAlertDialogBuilder(this)
            .setTitle(adapter.titleOf(game))
            .setView(content)
            .setPositiveButton(R.string.continue_yes) { _, _ -> start(game, SaveStateSlot.AUTO) }
            .setNegativeButton(R.string.continue_no) { _, _ -> launch(game) }
            .show()
            // A remote's OK picks Continue straight away (nothing has focus otherwise).
            .getButton(android.content.DialogInterface.BUTTON_POSITIVE)?.requestFocus()
    }

    private fun showGameOptions(game: Game) {
        val auto = SaveStateSlot(paths.stateDir(game.stem), SaveStateSlot.AUTO)
        val hasSave = paths.savFile(game.stem).isFile
        val options = mutableListOf<Pair<Int, () -> Unit>>()
        options += R.string.game_play to { launch(game) }
        if (auto.exists) options += R.string.game_resume to { start(game, SaveStateSlot.AUTO) }
        options += R.string.game_import_save to { pendingSaveGame = game; importSave.launch(arrayOf("*/*")) }
        if (hasSave) options += R.string.game_export_save to { pendingSaveGame = game; exportSave.launch("${game.stem}.sav") }
        if (hasSave) options += R.string.game_delete_save to { confirmDeleteSave(game) }
        if (library.isLocal(game)) options += R.string.game_remove_copy to { confirmRemoveCopy(game) }
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

    /** Deletes a game copied in with "Add games". Its save and save states stay. */
    private fun confirmRemoveCopy(game: Game) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.game_remove_copy)
            .setMessage(getString(R.string.remove_copy_confirm, adapter.titleOf(game)))
            .setPositiveButton(R.string.delete) { _, _ ->
                if (library.deleteLocal(game)) {
                    games = library.cached()
                    render()
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /**
     * GET_CONTENT for several files, with Android TV's stub picker left out of the chooser (it
     * would only close again). GET_CONTENT rather than OPEN_DOCUMENT: TV file managers (CX File
     * Explorer, MT Manager) answer it, and the grant need not outlive the copy.
     */
    private class PickGames : ActivityResultContract<Unit, List<Uri>>() {
        override fun createIntent(context: Context, input: Unit): Intent {
            val get = Intent(Intent.ACTION_GET_CONTENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            @Suppress("DEPRECATION")
            val handlers = context.packageManager.queryIntentActivities(get, 0)
            val real = handlers.filterNot { it.activityInfo.packageName.startsWith(STUB_PACKAGE) }
            if (real.isEmpty()) throw ActivityNotFoundException("no file picker")
            if (real.size == 1) return get.setClassName(real[0].activityInfo.packageName, real[0].activityInfo.name)
            val stubs = handlers.filter { it.activityInfo.packageName.startsWith(STUB_PACKAGE) }
                .map { ComponentName(it.activityInfo.packageName, it.activityInfo.name) }
            return Intent.createChooser(get, context.getString(R.string.add_games))
                .putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, stubs.toTypedArray())
        }

        override fun parseResult(resultCode: Int, intent: Intent?): List<Uri> {
            if (resultCode != RESULT_OK || intent == null) return emptyList()
            val clip = intent.clipData
            if (clip != null && clip.itemCount > 0) return List(clip.itemCount) { clip.getItemAt(it).uri }.filterNotNull()
            return listOfNotNull(intent.data)
        }
    }

    companion object {
        const val EXTRA_RELAUNCH = "com.izzy2lost.ninds.extra.RELAUNCH"
        /** Android TV's stand-ins for DocumentsUI, which only close again. */
        private const val STUB_PACKAGE = "com.android.tv.frameworkpackagestubs"
    }
}
