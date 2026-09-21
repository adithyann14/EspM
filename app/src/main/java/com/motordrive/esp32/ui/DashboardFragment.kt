package com.motordrive.esp32.ui

import android.Manifest
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
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
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.data.PendingAlert
import com.motordrive.esp32.databinding.FragmentDashboardBinding
import com.motordrive.esp32.modules.CurrentModule
import com.motordrive.esp32.modules.VoltageModule
import com.motordrive.esp32.modules.WaterFlowModule
import com.motordrive.esp32.viewmodel.DashboardViewModel
import com.motordrive.esp32.viewmodel.SensorVisibility
import kotlinx.coroutines.launch

class DashboardFragment : Fragment(R.layout.fragment_dashboard) {

    private var _b: FragmentDashboardBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    private var voltageModule: VoltageModule?   = null
    private var currentModule: CurrentModule?   = null
    private var waterModule:   WaterFlowModule? = null
    private var alertsShown = false

    private val notifPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) Snackbar.make(
            requireView(),
            "Notifications blocked — enable in Settings → Apps → MotorDrive",
            Snackbar.LENGTH_LONG
        ).show()
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentDashboardBinding.bind(view)
        applyWindowInsets()
        setupToolbar()
        inflateModules()
        setupButtons()
        observeVm()
        requestNotifPermissionIfNeeded()
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }

    private fun applyWindowInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            v.updatePadding(top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top)
            insets
        }
    }

    private fun setupToolbar() {
        b.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> { findNavController().navigate(R.id.action_dashboard_to_settings); true }
                R.id.action_refresh  -> { vm.refresh(); true }
                else -> false
            }
        }
    }

    private fun inflateModules() {
        val inf = layoutInflater
        if (FeatureConfig.ENABLE_VOLTAGE_SENSORS) {
            val v = inf.inflate(R.layout.module_voltage, b.moduleVoltageContainer, false)
            b.moduleVoltageContainer.addView(v); voltageModule = VoltageModule(v)
        } else b.moduleVoltageContainer.isVisible = false

        if (FeatureConfig.ENABLE_CURRENT_SENSOR) {
            val v = inf.inflate(R.layout.module_current, b.moduleCurrentContainer, false)
            b.moduleCurrentContainer.addView(v); currentModule = CurrentModule(v)
        } else b.moduleCurrentContainer.isVisible = false

        if (FeatureConfig.ENABLE_WATER_FLOW) {
            val v = inf.inflate(R.layout.module_water_flow, b.moduleWaterContainer, false)
            b.moduleWaterContainer.addView(v); waterModule = WaterFlowModule(v)
        } else b.moduleWaterContainer.isVisible = false
    }

    private fun setupButtons() {
        b.btnMotorOn.setOnClickListener  { vm.motorOn()  }
        b.btnMotorOff.setOnClickListener { vm.motorOff() }
    }

    private fun observeVm() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { vm.motorState.collect { state ->
                    updateStatusCard(state)
                    voltageModule?.update(state)
                    currentModule?.update(state)
                    waterModule?.update(state)
                }}
                launch { vm.isLoading.collect { loading ->
                    b.progressBar.isVisible = loading
                    b.btnMotorOn.isEnabled  = !loading
                    b.btnMotorOff.isEnabled = !loading
                }}
                launch { vm.config.collect { cfg -> b.connectionUrlText.text = cfg.baseUrl }}
                launch { vm.sensorVisibility.collect { v -> applySensorVisibility(v) }}
                launch { vm.pendingAlerts.collect { alerts ->
                    if (alerts.isNotEmpty() && !alertsShown) { alertsShown = true; showAlertDialog(alerts) }
                }}
            }
        }
    }

    private fun applySensorVisibility(v: SensorVisibility) {
        if (FeatureConfig.ENABLE_VOLTAGE_SENSORS) b.moduleVoltageContainer.isVisible = v.showVoltage
        if (FeatureConfig.ENABLE_CURRENT_SENSOR)  b.moduleCurrentContainer.isVisible = v.showCurrent
        if (FeatureConfig.ENABLE_WATER_FLOW)      b.moduleWaterContainer.isVisible   = v.showWater
    }

    private fun updateStatusCard(state: MotorState) {
        // ── Connection chip ───────────────────────────────────────────────
        if (state.isConnected) {
            val linkIcon = if (state.linkOk) "● " else "◌ "
            b.chipConnection.text = "${linkIcon}Connected${if (!state.linkOk) " (RF down)" else ""}"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(
                    if (state.linkOk) R.color.status_connected_bg else R.color.status_error_bg
                )
            )
            b.chipConnection.setTextColor(requireContext().getColor(
                if (state.linkOk) R.color.status_connected_text else R.color.status_error_text
            ))
        } else {
            b.chipConnection.text = "○ ${state.errorMessage?.take(28) ?: "Disconnected"}"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(R.color.status_error_bg))
            b.chipConnection.setTextColor(requireContext().getColor(R.color.status_error_text))
        }

        // ── Last updated ──────────────────────────────────────────────────
        if (state.lastUpdatedMs > 0L) {
            val secs = (System.currentTimeMillis() - state.lastUpdatedMs) / 1000
            b.lastUpdatedText.text = "Updated ${secs}s ago"
        } else b.lastUpdatedText.text = ""

        // ── Motor state banner — includes stall warning ───────────────────
        when {
            state.motorOn && state.stall -> {
                b.motorStateBanner.text = "⚠  MOTOR ON  —  STALL DETECTED"
                b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.value_warn))
                b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
                b.btnMotorOn.isEnabled  = false
                b.btnMotorOff.isEnabled = true
            }
            state.motorOn -> {
                b.motorStateBanner.text = "● MOTOR ON"
                b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_on))
                b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
                b.btnMotorOn.isEnabled  = false
                b.btnMotorOff.isEnabled = true
            }
            else -> {
                b.motorStateBanner.text = "○ MOTOR OFF"
                b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_off))
                b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
                b.btnMotorOn.isEnabled  = true
                b.btnMotorOff.isEnabled = false
            }
        }
    }

    private fun showAlertDialog(alerts: List<PendingAlert>) {
        val icon = when (alerts.first().type) { "power_loss" -> "⚡"; "phase_fault" -> "⚠️"; "overload" -> "🔥"; else -> "ℹ️" }
        val body = alerts.joinToString("\n\n") { a ->
            val ts = if (a.timestamp > 0L)
                java.text.SimpleDateFormat("dd MMM HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date(a.timestamp * 1000L))
            else "Unknown time"
            "• ${a.message}\n  $ts"
        }
        MaterialAlertDialogBuilder(requireContext())
            .setTitle("$icon  Alert${if (alerts.size > 1) "s" else ""}")
            .setMessage(body)
            .setPositiveButton("Acknowledge & Clear") { _, _ -> alertsShown = false; vm.clearAlerts() }
            .setNegativeButton("Dismiss") { _, _ -> }
            .setCancelable(false).show()
    }

    private fun requestNotifPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED) {
            notifPermLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
}
