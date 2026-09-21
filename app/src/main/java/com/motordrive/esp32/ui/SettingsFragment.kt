package com.motordrive.esp32.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.databinding.FragmentSettingsBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import com.motordrive.esp32.viewmodel.NotificationSettings
import com.motordrive.esp32.viewmodel.SensorVisibility
import kotlinx.coroutines.launch

class SettingsFragment : Fragment(R.layout.fragment_settings) {

    private var _b: FragmentSettingsBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentSettingsBinding.bind(view)

        applyWindowInsets()
        setupToolbar()
        setupConnectionModeToggle()
        configureSensorRows()
        populateFields()              // values first — no listeners yet
        setupSensorToggleListeners()  // instant-save, after populate
        setupNotifToggleListeners()   // instant-save, after populate
        setupSaveButton()
        setupDiagnostics()
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }

    private fun applyWindowInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            v.updatePadding(top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top)
            insets
        }
    }

    private fun setupToolbar() {
        b.toolbar.setNavigationOnClickListener { findNavController().navigateUp() }
    }

    private fun setupConnectionModeToggle() {
        b.toggleConnectionMode.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (!isChecked) return@addOnButtonCheckedListener
            val server = checkedId == R.id.btnServerMode
            b.directWifiSection.isVisible = !server
            b.serverSection.isVisible     = server
        }
        if (!FeatureConfig.ENABLE_SERVER_MODE) {
            b.btnServerMode.isVisible = false
            b.serverSection.isVisible = false
        }
    }

    private fun configureSensorRows() {
        b.sensorVoltageRow.isVisible = FeatureConfig.ENABLE_VOLTAGE_SENSORS
        b.sensorCurrentRow.isVisible = FeatureConfig.ENABLE_CURRENT_SENSOR
        b.sensorWaterRow.isVisible   = FeatureConfig.ENABLE_WATER_FLOW
        b.sensorDisplaySection.isVisible =
            FeatureConfig.ENABLE_VOLTAGE_SENSORS ||
            FeatureConfig.ENABLE_CURRENT_SENSOR  ||
            FeatureConfig.ENABLE_WATER_FLOW
    }

    // ── Populate saved values BEFORE any listeners are attached ───────────
    private fun populateFields() {
        val cfg = vm.config.value
        b.editIp.setText(cfg.directIp)
        b.editPort.setText(cfg.port.toString())
        b.editPollInterval.setText(cfg.pollIntervalSeconds.toString())

        if (FeatureConfig.ENABLE_SERVER_MODE) {
            if (cfg.useServerMode) {
                b.toggleConnectionMode.check(R.id.btnServerMode)
                b.directWifiSection.isVisible = false
                b.serverSection.isVisible     = true
            } else {
                b.toggleConnectionMode.check(R.id.btnDirectWifi)
                b.directWifiSection.isVisible = true
                b.serverSection.isVisible     = false
            }
            b.editServerUrl.setText(cfg.serverUrl)
        } else {
            b.toggleConnectionMode.check(R.id.btnDirectWifi)
        }

        // Sensor toggles (no listeners yet — no spurious saves)
        val vis = vm.sensorVisibility.value
        b.switchShowVoltage.isChecked = vis.showVoltage
        b.switchShowCurrent.isChecked = vis.showCurrent
        b.switchShowWater.isChecked   = vis.showWater

        // Notification toggles (no listeners yet)
        val ns = vm.notifSettings.value
        b.switchPersistentNotif.isChecked = ns.persistentEnabled
        b.switchAlertNotif.isChecked      = ns.alertEnabled
    }

    // ── Sensor toggles — apply immediately ────────────────────────────────
    private fun setupSensorToggleListeners() {
        b.switchShowVoltage.setOnCheckedChangeListener { _, c ->
            vm.updateSensorVisibility(vm.sensorVisibility.value.copy(showVoltage = c))
        }
        b.switchShowCurrent.setOnCheckedChangeListener { _, c ->
            vm.updateSensorVisibility(vm.sensorVisibility.value.copy(showCurrent = c))
        }
        b.switchShowWater.setOnCheckedChangeListener { _, c ->
            vm.updateSensorVisibility(vm.sensorVisibility.value.copy(showWater = c))
        }
    }

    // ── Notification toggles — apply immediately ──────────────────────────
    private fun setupNotifToggleListeners() {
        b.switchPersistentNotif.setOnCheckedChangeListener { _, c ->
            vm.updateNotifSettings(vm.notifSettings.value.copy(persistentEnabled = c))
        }
        b.switchAlertNotif.setOnCheckedChangeListener { _, c ->
            vm.updateNotifSettings(vm.notifSettings.value.copy(alertEnabled = c))
        }
    }

    // ── Save & Connect — connection settings only ─────────────────────────
    private fun setupSaveButton() {
        b.btnSave.setOnClickListener {
            val ip   = b.editIp.text?.toString()?.trim() ?: ""
            val port = b.editPort.text?.toString()?.toIntOrNull() ?: 80
            val poll = b.editPollInterval.text?.toString()?.toIntOrNull() ?: 3

            if (ip.isBlank()) { b.ipLayout.error = "Enter a valid IP or hostname"; return@setOnClickListener }
            b.ipLayout.error = null

            val useServer = FeatureConfig.ENABLE_SERVER_MODE &&
                            b.toggleConnectionMode.checkedButtonId == R.id.btnServerMode
            val serverUrl = b.editServerUrl.text?.toString()?.trim() ?: ""
            if (useServer && serverUrl.isBlank()) { b.serverUrlLayout.error = "Enter the server URL"; return@setOnClickListener }
            b.serverUrlLayout.error = null

            vm.updateConfig(ConnectionConfig(
                directIp            = ip,
                port                = port.coerceIn(1, 65535),
                useServerMode       = useServer,
                serverUrl           = serverUrl,
                pollIntervalSeconds = poll.coerceIn(1, 60)
            ))
            Toast.makeText(requireContext(), "Settings saved", Toast.LENGTH_SHORT).show()
            findNavController().navigateUp()
        }
    }

    // ── Diagnostics: Serial Monitor + App Logcat ──────────────────────────
    private fun setupDiagnostics() {

        // Serial monitor — manual refresh fetches /api/logs from ESP
        b.btnRefreshSerial.setOnClickListener {
            b.btnRefreshSerial.isEnabled = false
            viewLifecycleOwner.lifecycleScope.launch {
                vm.fetchLogs()
                b.btnRefreshSerial.isEnabled = true
            }
        }
        b.btnClearSerial.setOnClickListener {
            b.serialTerminalText.text = "— display cleared (ESP buffer intact) —"
        }

        // App logcat — export text, clear
        b.btnExportLogcat.setOnClickListener {
            val text = AppLogger.export()
            if (text.isBlank()) {
                Toast.makeText(requireContext(), "Nothing to export yet", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            startActivity(Intent.createChooser(
                Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_SUBJECT, "MotorDrive App Log")
                    putExtra(Intent.EXTRA_TEXT, text)
                },
                "Export App Log as…"
            ))
        }
        b.btnClearLogcat.setOnClickListener {
            AppLogger.clear()
            Toast.makeText(requireContext(), "App log cleared", Toast.LENGTH_SHORT).show()
        }

        // Live observation
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {

                launch {
                    vm.espLogs.collect { lines ->
                        b.serialTerminalText.text = if (lines.isEmpty())
                            "— tap Refresh to fetch ESP log —"
                        else
                            lines.joinToString("\n")
                        b.serialScrollView.post { b.serialScrollView.fullScroll(View.FOCUS_DOWN) }
                    }
                }

                launch {
                    AppLogger.flow.collect { entries ->
                        b.logcatText.text = if (entries.isEmpty())
                            "— no events yet —"
                        else
                            entries.joinToString("\n") { it.format() }
                        b.logcatScrollView.post { b.logcatScrollView.fullScroll(View.FOCUS_DOWN) }
                    }
                }
            }
        }
    }
}
