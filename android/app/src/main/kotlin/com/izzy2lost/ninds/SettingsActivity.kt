package com.izzy2lost.ninds

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.enableEdgeToEdge
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.preference.EditTextPreference
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.izzy2lost.ninds.databinding.ActivitySettingsBinding
import java.io.File

class SettingsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        binding.toolbar.setNavigationOnClickListener { finish() }
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            binding.appBar.updatePadding(top = bars.top, left = bars.left, right = bars.right)
            binding.settingsContainer.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            insets
        }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction().replace(R.id.settingsContainer, SettingsFragment()).commit()
        }
    }

    class SettingsFragment : PreferenceFragmentCompat() {
        private lateinit var settings: Settings
        private lateinit var paths: AppPaths
        private var importTarget: BiosFile? = null

        private enum class BiosFile(val fileName: String, val sizes: Set<Long>, val key: String) {
            BIOS9("bios9.bin", setOf(4096L), "import_bios9"),
            BIOS7("bios7.bin", setOf(16384L), "import_bios7"),
            FIRMWARE("firmware.bin", setOf(128L * 1024, 256L * 1024, 512L * 1024), "import_firmware"),
        }

        private val pickBios = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            val target = importTarget ?: return@registerForActivityResult
            if (uri != null) importBios(uri, target)
        }

        private val pickCheatDb = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@registerForActivityResult
            val ok = runCatching {
                val head = ByteArray(12)
                requireContext().contentResolver.openInputStream(uri)!!.use { it.read(head) }
                require(String(head, Charsets.US_ASCII) == "R4 CheatCode")
                requireContext().contentResolver.openInputStream(uri)!!.use { input ->
                    paths.cheatDatabase.outputStream().use { input.copyTo(it) }
                }
            }.isSuccess
            toast(if (ok) R.string.cheat_db_imported else R.string.cheat_db_invalid)
            refreshSummaries()
        }

        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            settings = Settings.get(requireContext())
            paths = AppPaths(requireContext())
            preferenceManager.preferenceDataStore = settings.dataStore
            setPreferencesFromResource(R.xml.preferences, rootKey)

            for (b in BiosFile.entries) {
                findPreference<Preference>(b.key)?.setOnPreferenceClickListener {
                    importTarget = b
                    pickBios.launch(arrayOf("*/*"))
                    true
                }
            }
            findPreference<Preference>("clear_bios")?.setOnPreferenceClickListener {
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle(R.string.pref_clear_bios)
                    .setMessage(R.string.clear_bios_confirm)
                    .setPositiveButton(R.string.delete) { _, _ ->
                        BiosFile.entries.forEach { File(paths.bios, it.fileName).delete() }
                        File(paths.bios, "firmware.ovr").delete()
                        refreshSummaries()
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                true
            }
            findPreference<Preference>("import_cheat_db")?.setOnPreferenceClickListener {
                pickCheatDb.launch(arrayOf("*/*"))
                true
            }
            findPreference<Preference>("controller_mapping")?.setOnPreferenceClickListener {
                startActivity(Intent(requireContext(), ControllerMappingActivity::class.java))
                true
            }
            findPreference<Preference>("rom_folder")?.setOnPreferenceClickListener {
                settings.romTreeUri = ""
                toast(R.string.rom_folder_cleared)
                refreshSummaries()
                true
            }
            findPreference<Preference>("clear_cache")?.setOnPreferenceClickListener {
                val freed = listOf(File(paths.cache, "unpacked"), File(paths.cache, "rom/.dsperate"))
                    .sumOf { dir -> dir.walkBottomUp().filter { it.isFile }.sumOf { f -> f.length().also { f.delete() } } }
                toast(getString(R.string.cache_cleared, freed / (1024 * 1024)))
                true
            }
            findPreference<ListPreference>(Settings.THEME)?.setOnPreferenceChangeListener { _, value ->
                AppCompatDelegate.setDefaultNightMode(NinDSApp.nightMode(value as String))
                true
            }
            findPreference<EditTextPreference>(Settings.NICKNAME)?.setOnBindEditTextListener {
                it.filters = arrayOf(android.text.InputFilter.LengthFilter(10))
            }
            findPreference<EditTextPreference>(Settings.MESSAGE)?.setOnBindEditTextListener {
                it.filters = arrayOf(android.text.InputFilter.LengthFilter(26))
            }
            findPreference<Preference>("about")?.summary = getString(R.string.about_summary, BuildConfig.VERSION_NAME)
            refreshSummaries()
        }

        override fun onResume() {
            super.onResume()
            refreshSummaries()
        }

        private fun refreshSummaries() {
            for (b in BiosFile.entries) {
                findPreference<Preference>(b.key)?.summary =
                    getString(if (File(paths.bios, b.fileName).isFile) R.string.bios_present else R.string.bios_missing)
            }
            findPreference<Preference>("import_cheat_db")?.summary =
                getString(if (paths.cheatDatabase.isFile) R.string.cheat_db_present else R.string.cheat_db_missing)
            findPreference<Preference>("rom_folder")?.summary = settings.romTreeUri.takeIf { it.isNotEmpty() }
                ?.let { Uri.decode(Uri.parse(it).lastPathSegment ?: it) } ?: getString(R.string.rom_folder_none)
        }

        private fun importBios(uri: Uri, target: BiosFile) {
            val ctx = requireContext()
            val bytes = runCatching { ctx.contentResolver.openInputStream(uri)!!.use { it.readBytes() } }.getOrNull()
            if (bytes == null || bytes.size.toLong() !in target.sizes) {
                toast(getString(R.string.bios_wrong_size, target.fileName))
                return
            }
            File(paths.bios, target.fileName).writeBytes(bytes)
            if (target == BiosFile.FIRMWARE) File(paths.bios, "firmware.ovr").delete()
            toast(getString(R.string.bios_imported, target.fileName))
            refreshSummaries()
        }

        private fun toast(res: Int) = Toast.makeText(requireContext(), res, Toast.LENGTH_SHORT).show()
        private fun toast(text: String) = Toast.makeText(requireContext(), text, Toast.LENGTH_SHORT).show()
    }
}
