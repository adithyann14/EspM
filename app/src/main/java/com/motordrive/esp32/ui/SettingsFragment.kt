package com.motordrive.esp32.ui

import android.animation.ObjectAnimator
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
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
import com.google.android.material.slider.Slider
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.data.Esp32Repository
import com.motordrive.esp32.databinding.FragmentSettingsBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class SettingsFragment : Fragment(R.layout.fragment_settings) {

    private var _b: FragmentSettingsBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    private val prefs by lazy {
        requireContext().getSharedPreferences("mdc_prefs", android.content.Context.MODE_PRIVATE)
    }

    // ── Section expand/collapse state (persisted across navigations) ───────
    private val sectionExpanded = mutableMapOf(
        "sensors"    to true,   // expanded by default — critical settings
        "connection" to true,   // expanded by default — primary use
        "ota"        to false,  // collapsed — rarely used
        "notif"      to false,  // collapsed
        "diag"       to false   // collapsed — developer / advanced
    )

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentSettingsBinding.bind(view)

        applyWindowInsets()
        setupToolbar()
        setupSections()               // collapsible headers
        setupConnectionModeToggle()
        configureSensorRows()
        populateFields()              // values first — no listeners yet
        setupSensorToggleListeners()
        setupDryRunSlider()
        setupNotifToggleListeners()
        setupSaveButton()
        setupWifi()
        setupDiagnostics()
        setupOta()
        observeVm()
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

    // ── Collapsible sections ───────────────────────────────────────────────
    private fun setupSections() {
        applySectionState("sensors",    b.contentSensors,    b.chevronSensors,    animate = false)
        applySectionState("connection", b.contentConnection, b.chevronConnection, animate = false)
        applySectionState("ota",        b.contentOta,        b.chevronOta,        animate = false)
        applySectionState("notif",      b.contentNotif,      b.chevronNotif,      animate = false)
        applySectionState("diag",       b.contentDiag,       b.chevronDiag,       animate = false)

        b.headerSensors.setOnClickListener {
            sectionExpanded["sensors"] = !(sectionExpanded["sensors"] ?: true)
            applySectionState("sensors", b.contentSensors, b.chevronSensors, animate = true)
        }
        b.headerConnection.setOnClickListener {
            sectionExpanded["connection"] = !(sectionExpanded["connection"] ?: true)
            applySectionState("connection", b.contentConnection, b.chevronConnection, animate = true)
        }
        b.headerOta.setOnClickListener {
            sectionExpanded["ota"] = !(sectionExpanded["ota"] ?: false)
            applySectionState("ota", b.contentOta, b.chevronOta, animate = true)
        }
        b.headerNotif.setOnClickListener {
            sectionExpanded["notif"] = !(sectionExpanded["notif"] ?: false)
            applySectionState("notif", b.contentNotif, b.chevronNotif, animate = true)
        }
        b.headerDiag.setOnClickListener {
            sectionExpanded["diag"] = !(sectionExpanded["diag"] ?: false)
            applySectionState("diag", b.contentDiag, b.chevronDiag, animate = true)
        }
    }

    private fun applySectionState(
        key: String,
        content: LinearLayout,
        chevron: ImageView,
        animate: Boolean
    ) {
        val expanded = sectionExpanded[key] ?: false
        content.isVisible = expanded
        val targetRotation = if (expanded) 0f else -90f
        if (animate) {
            ObjectAnimator.ofFloat(chevron, "rotation", chevron.rotation, targetRotation)
                .apply { duration = 200 }.start()
        } else {
            chevron.rotation = targetRotation
        }
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
        b.sensorDisplaySection.isVisible = true

        b.sensorVoltageRow.isVisible = FeatureConfig.ENABLE_VOLTAGE_SENSORS
        b.sensorCurrentRow.isVisible = FeatureConfig.ENABLE_CURRENT_SENSOR
        b.sensorWaterRow.isVisible   = FeatureConfig.ENABLE_WATER_FLOW
        b.sensorRssiRow.isVisible    = FeatureConfig.ENABLE_ESPNOW_RSSI

        val anySensorEnabled = FeatureConfig.ENABLE_VOLTAGE_SENSORS ||
                               FeatureConfig.ENABLE_CURRENT_SENSOR  ||
                               FeatureConfig.ENABLE_WATER_FLOW      ||
                               FeatureConfig.ENABLE_ESPNOW_RSSI
        b.dividerSensorModules.isVisible = anySensorEnabled

        b.dividerVoltageCurrent.isVisible =
            FeatureConfig.ENABLE_VOLTAGE_SENSORS && FeatureConfig.ENABLE_CURRENT_SENSOR
        b.dividerCurrentWater.isVisible =
            (FeatureConfig.ENABLE_VOLTAGE_SENSORS || FeatureConfig.ENABLE_CURRENT_SENSOR) &&
            FeatureConfig.ENABLE_WATER_FLOW
        b.dividerWaterRssi.isVisible =
            (FeatureConfig.ENABLE_VOLTAGE_SENSORS || FeatureConfig.ENABLE_CURRENT_SENSOR ||
             FeatureConfig.ENABLE_WATER_FLOW) && FeatureConfig.ENABLE_ESPNOW_RSSI
    }

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

        val vis = vm.sensorVisibility.value
        b.switchShowVoltage.isChecked = vis.showVoltage
        b.switchShowCurrent.isChecked = vis.showCurrent
        b.switchShowWater.isChecked   = vis.showWater
        b.switchShowRssi.isChecked    = vis.showRssi

        val ns = vm.notifSettings.value
        b.switchPersistentNotif.isChecked = ns.persistentEnabled
        b.switchAlertNotif.isChecked      = ns.alertEnabled

        b.editOtaPass.setText(prefs.getString("ota_pass", "motor123"))
        b.toggleOtaTarget.check(R.id.btnOtaSender)

        val dryRunSec = vm.dryRunSeconds.value.toFloat().coerceIn(5f, 60f)
        b.sliderDryRun.value  = dryRunSec
        b.tvDryRunValue.text  = "${dryRunSec.toInt()} s"
    }

    // ── Sensor module visibility toggles ──────────────────────────────────
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
        b.switchShowRssi.setOnCheckedChangeListener { _, c ->
            vm.updateSensorVisibility(vm.sensorVisibility.value.copy(showRssi = c))
        }
    }

    // ── Dry-run timeout slider ─────────────────────────────────────────────
    private fun setupDryRunSlider() {
        b.sliderDryRun.addOnChangeListener { _, value, _ ->
            b.tvDryRunValue.text = "${value.toInt()} s"
        }
        b.sliderDryRun.addOnSliderTouchListener(object : Slider.OnSliderTouchListener {
            override fun onStartTrackingTouch(slider: Slider) {}
            override fun onStopTrackingTouch(slider: Slider) {
                val secs = slider.value.toInt()
                vm.setDryRunTimeout(secs)
                Toast.makeText(requireContext(), "Dry-run cutoff set to ${secs} s", Toast.LENGTH_SHORT).show()
            }
        })
    }

    // ── Notification toggles ──────────────────────────────────────────────
    private fun setupNotifToggleListeners() {
        b.switchPersistentNotif.setOnCheckedChangeListener { _, c ->
            vm.updateNotifSettings(vm.notifSettings.value.copy(persistentEnabled = c))
        }
        b.switchAlertNotif.setOnCheckedChangeListener { _, c ->
            vm.updateNotifSettings(vm.notifSettings.value.copy(alertEnabled = c))
        }
    }

    // ── Connection save button ─────────────────────────────────────────────
    private fun setupSaveButton() {
        b.btnSave.setOnClickListener {
            val ip   = b.editIp.text?.toString()?.trim() ?: ""
            val port = b.editPort.text?.toString()?.toIntOrNull() ?: 80
            val poll = b.editPollInterval.text?.toString()?.toIntOrNull() ?: 3

            if (ip.isBlank()) {
                b.ipLayout.error = "Enter a valid IP or hostname"; return@setOnClickListener
            }
            b.ipLayout.error = null

            val useServer = FeatureConfig.ENABLE_SERVER_MODE &&
                            b.toggleConnectionMode.checkedButtonId == R.id.btnServerMode
            val serverUrl = b.editServerUrl.text?.toString()?.trim() ?: ""
            if (useServer && serverUrl.isBlank()) {
                b.serverUrlLayout.error = "Enter server URL"; return@setOnClickListener
            }
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

    // ── Home Wi-Fi (STA) config ───────────────────────────────────────────
    private fun setupWifi() {
        vm.motorState.value.staIp?.let { ip ->
            b.tvWifiStatus.text = "✅ STA connected: $ip"
        }

        b.btnSaveWifi.setOnClickListener {
            val ssid = b.editStaSsid.text?.toString()?.trim() ?: ""
            val pass = b.editStaPass.text?.toString() ?: ""
            if (ssid.isEmpty()) {
                b.layoutStaSsid.error = "Enter your home Wi-Fi network name"
                return@setOnClickListener
            }
            b.layoutStaSsid.error = null
            b.btnSaveWifi.isEnabled = false
            b.tvWifiStatus.text = "⏳ Sending to ESP…"

            vm.configureWifi(ssid, pass) { result ->
                viewLifecycleOwner.lifecycleScope.launch(kotlinx.coroutines.Dispatchers.Main) {
                    if (result.isFailure) {
                        b.btnSaveWifi.isEnabled = true
                        b.tvWifiStatus.text = "❌ ${result.exceptionOrNull()?.message}"
                        return@launch
                    }
                    b.tvWifiStatus.text = "⏳ ESP joining network… (checking in 12 s)"
                    kotlinx.coroutines.delay(12_000L)
                    vm.getWifiStatus { statusResult ->
                        viewLifecycleOwner.lifecycleScope.launch(kotlinx.coroutines.Dispatchers.Main) {
                            b.btnSaveWifi.isEnabled = true
                            statusResult.onSuccess { ws ->
                                if (ws.staConnected && ws.staIp != null) {
                                    b.tvWifiStatus.text = "✅ STA connected: ${ws.staIp}"
                                    b.editIp.setText(ws.staIp)
                                    Toast.makeText(
                                        requireContext(),
                                        "ESP joined ${ws.staSSID} — IP ${ws.staIp}. Tap Save & Connect to update the app.",
                                        Toast.LENGTH_LONG
                                    ).show()
                                } else {
                                    b.tvWifiStatus.text = "⚠️ STA not connected — check SSID / password"
                                }
                            }.onFailure {
                                b.tvWifiStatus.text = "⚠️ Couldn't read status: ${it.message}"
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Diagnostics ───────────────────────────────────────────────────────
    private fun setupDiagnostics() {
        b.cardSerial.setOnClickListener {
            findNavController().navigate(R.id.action_settings_to_full_serial)
        }
        b.cardLogcat.setOnClickListener {
            findNavController().navigate(R.id.action_settings_to_full_logcat)
        }
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
                }, "Export App Log"
            ))
        }
        b.btnClearLogcat.setOnClickListener {
            AppLogger.clear()
            Toast.makeText(requireContext(), "App log cleared", Toast.LENGTH_SHORT).show()
        }
    }

    // ── OTA ───────────────────────────────────────────────────────────────
    private fun setupOta() {
        b.toggleOtaTarget.addOnButtonCheckedListener { _, _, _ ->
            b.otaInfoCard.isVisible = false
        }
        b.btnEnableOta.setOnClickListener {
            val pass = b.editOtaPass.text?.toString()?.trim() ?: ""
            if (pass.isBlank()) {
                Toast.makeText(requireContext(), "Enter the OTA password", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            prefs.edit().putString("ota_pass", pass).apply()
            val isReceiver = b.toggleOtaTarget.checkedButtonId == R.id.btnOtaReceiver
            if (isReceiver) triggerReceiverOta(pass) else showSenderOtaInfo(pass)
        }
    }

    private fun showSenderOtaInfo(pass: String) {
        val cfg = vm.config.value
        val url = "${cfg.baseUrl}/update"
        b.tvOtaInfo.text =
            "Sender OTA is always ready.\n\n" +
            "Upload via browser or curl:\n" +
            "  URL:  $url\n" +
            "  User: admin\n" +
            "  Pass: $pass\n\n" +
            "curl -u admin:$pass -F \"image=@firmware.bin\" $url"
        b.otaInfoCard.isVisible = true
        AppLogger.log("OTA", "Sender OTA URL shown: $url")
    }

    private fun triggerReceiverOta(pass: String) {
        b.btnEnableOta.isEnabled = false
        b.tvOtaInfo.text = "Contacting sender…"
        b.otaInfoCard.isVisible = true

        viewLifecycleOwner.lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { Esp32Repository(vm.config.value).enableReceiverOta(pass) }
            }
            b.btnEnableOta.isEnabled = true

            if (result.isSuccess) {
                b.tvOtaInfo.text =
                    "✅ Receiver is now in OTA mode (5 min window).\n\n" +
                    "Steps:\n" +
                    "1. On your phone: connect Wi-Fi to\n" +
                    "     MCReceiver-OTA\n" +
                    "   Password: $pass\n\n" +
                    "2. Open browser →\n" +
                    "     http://192.168.4.1/update\n\n" +
                    "3. Upload receiver firmware .bin\n\n" +
                    "4. Reconnect phone to MotorControl\n" +
                    "   when done."
                AppLogger.log("OTA", "Receiver OTA triggered successfully")
            } else {
                val err = result.exceptionOrNull()?.message ?: "Unknown error"
                b.tvOtaInfo.text = "❌ Failed to reach sender:\n$err"
                AppLogger.log("OTA", "Receiver OTA failed: $err")
            }
        }
    }

    // ── Observe ViewModel ─────────────────────────────────────────────────
    private fun observeVm() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {

                launch {
                    vm.dryRunSeconds.collect { secs ->
                        val f = secs.toFloat().coerceIn(5f, 60f)
                        if (b.sliderDryRun.value != f) b.sliderDryRun.value = f
                        b.tvDryRunValue.text = "${secs} s"
                    }
                }

                launch {
                    vm.espLogs.collect { lines ->
                        if (lines.isEmpty()) {
                            b.serialTerminalText.text = "— tap card to open full Serial Monitor —"
                        } else {
                            b.serialTerminalText.setText(
                                buildMiniSerialSpan(lines),
                                TextView.BufferType.SPANNABLE
                            )
                        }
                        b.serialScrollView.post { b.serialScrollView.fullScroll(View.FOCUS_DOWN) }
                    }
                }

                launch {
                    AppLogger.flow.collect { entries ->
                        b.logcatText.text = if (entries.isEmpty()) "— no events yet —"
                        else entries.joinToString("\n") { it.format() }
                        b.logcatScrollView.post { b.logcatScrollView.fullScroll(View.FOCUS_DOWN) }
                    }
                }
            }
        }
    }

    // ── Mini serial colour helper ─────────────────────────────────────────
    private fun buildMiniSerialSpan(lines: List<String>): SpannableStringBuilder {
        val colSend = Color.parseColor("#4FC3F7")
        val colRecv = Color.parseColor("#69FF47")
        val colDim  = Color.parseColor("#888888")
        val sb = SpannableStringBuilder()
        for (line in lines) {
            val start = sb.length
            sb.append(line).append('\n')
            val color = when {
                line.contains("[SEND]") || line.contains("[LORA]") ||
                line.contains("[COMMS]") || line.contains("[SCHED]") ||
                line.contains("[TIMER]")  -> colSend
                line.contains("[RECV]")   -> colRecv
                else                      -> colDim
            }
            sb.setSpan(ForegroundColorSpan(color), start, sb.length,
                       Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        return sb
    }
}
