package com.motordrive.esp32.ui

import android.app.TimePickerDialog
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
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.motordrive.esp32.R
import com.motordrive.esp32.data.ScheduleEntry
import com.motordrive.esp32.databinding.DialogAddScheduleBinding
import com.motordrive.esp32.databinding.FragmentSchedulerBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.launch

/**
 * Tab 3 — Scheduler.
 *
 * Up to 8 start/stop schedule entries, stored in EEPROM on both ESPs.
 * When RTC is OFF (default), the app syncs phone time to the ESP every 30 s.
 * When RTC is ON, the ESP uses its DS3231 module after the first sync.
 */
class SchedulerFragment : Fragment(R.layout.fragment_scheduler) {

    private var _b: FragmentSchedulerBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()
    private lateinit var adapter: ScheduleAdapter

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentSchedulerBinding.bind(view)
        applyWindowInsets()
        setupToolbar()
        setupRecycler()
        setupControls()
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
        b.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> { findNavController().navigate(R.id.action_scheduler_to_settings); true }
                else -> false
            }
        }
    }

    private fun setupRecycler() {
        adapter = ScheduleAdapter(
            onToggle = { entry, enabled -> vm.updateScheduleEnabled(entry, enabled) },
            onDelete = { entry -> vm.deleteSchedule(entry) }
        )
        b.rvSchedules.layoutManager = LinearLayoutManager(requireContext())
        b.rvSchedules.adapter = adapter
    }

    private fun setupControls() {
        b.btnAddSchedule.setOnClickListener {
            if (vm.schedules.value.size >= 8) {
                Toast.makeText(requireContext(), "Maximum 8 schedules reached", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            showAddDialog()
        }

        b.btnRevokeAll.setOnClickListener {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle("Revoke All Schedules")
                .setMessage("This will delete all schedules from both ESPs and clear EEPROM. Proceed?")
                .setPositiveButton("Revoke All") { _, _ -> vm.clearAllSchedules() }
                .setNegativeButton("Cancel", null)
                .show()
        }

        // RTC toggle — immediately persists and notifies ESP
        b.switchRtcEnabled.setOnCheckedChangeListener { _, checked ->
            vm.setRtcEnabled(checked)
            val msg = if (checked)
                "RTC enabled — firmware will read DS3231 for time"
            else
                "RTC disabled — app syncs phone time to ESP every 30 s"
            Toast.makeText(requireContext(), msg, Toast.LENGTH_SHORT).show()
        }
    }

    private fun showAddDialog() {
        val db = DialogAddScheduleBinding.inflate(layoutInflater)
        var startH = 6; var startM = 0
        var stopH  = 7; var stopM  = 0

        fun fmt(h: Int, m: Int) = "%02d:%02d".format(h, m)
        db.btnPickStart.text = fmt(startH, startM)
        db.btnPickStop.text  = fmt(stopH,  stopM)

        db.btnPickStart.setOnClickListener {
            TimePickerDialog(requireContext(), { _, h, m ->
                startH = h; startM = m; db.btnPickStart.text = fmt(h, m)
            }, startH, startM, true).show()
        }
        db.btnPickStop.setOnClickListener {
            TimePickerDialog(requireContext(), { _, h, m ->
                stopH = h; stopM = m; db.btnPickStop.text = fmt(h, m)
            }, stopH, stopM, true).show()
        }

        MaterialAlertDialogBuilder(requireContext())
            .setTitle("Add Schedule")
            .setView(db.root)
            .setPositiveButton("Add") { _, _ ->
                val days = listOf(db.cbSun, db.cbMon, db.cbTue,
                                  db.cbWed, db.cbThu, db.cbFri, db.cbSat)
                    .mapIndexed { i, cb -> if (cb.isChecked) (1 shl i) else 0 }
                    .fold(0) { acc, v -> acc or v }

                if (days == 0) {
                    Toast.makeText(requireContext(), "Select at least one day", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                if (stopH * 60 + stopM <= startH * 60 + startM) {
                    Toast.makeText(requireContext(), "Stop time must be after start time", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                vm.addSchedule(ScheduleEntry(
                    id          = System.currentTimeMillis().toInt(),
                    startHour   = startH, startMinute = startM,
                    stopHour    = stopH,  stopMinute  = stopM,
                    days        = days,
                    autoRestart = db.switchSchedAutoRestart.isChecked,
                    enabled     = true
                ))
                Toast.makeText(requireContext(), "Schedule saved to ESP", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun observeVm() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    vm.schedules.collect { entries ->
                        adapter.submitList(entries)
                        b.tvNoSchedules.isVisible = entries.isEmpty()
                        b.btnRevokeAll.isEnabled  = entries.isNotEmpty()
                    }
                }
                launch {
                    vm.rtcEnabled.collect { enabled ->
                        // Suppress listener while we set the switch programmatically
                        b.switchRtcEnabled.setOnCheckedChangeListener(null)
                        b.switchRtcEnabled.isChecked = enabled
                        b.switchRtcEnabled.setOnCheckedChangeListener { _, checked ->
                            vm.setRtcEnabled(checked)
                        }
                        b.tvRtcHint.text = if (enabled)
                            "Hardware DS3231 RTC active — time is read from I²C"
                        else
                            "No RTC — app syncs phone time to ESP every 30 s"
                        b.tvPhoneTime.isVisible = !enabled
                    }
                }
                launch {
                    vm.phoneTimeLabel.collect { label -> b.tvPhoneTime.text = label }
                }
            }
        }
    }
}
