package com.motordrive.esp32.ui

import android.content.res.ColorStateList
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
import com.motordrive.esp32.R
import com.motordrive.esp32.databinding.FragmentTimerBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.launch

/**
 * Tab 2 — Stop-Timer.
 *
 * Set a countdown after which the motor is automatically switched OFF.
 * Both sender and receiver store the config in EEPROM so that if
 * [autoRestart] is on, the remaining timer resumes after a power cut.
 *
 * Time source: phone time synced to ESP every 30 s (no RTC needed for timer).
 */
class TimerFragment : Fragment(R.layout.fragment_timer) {

    private var _b: FragmentTimerBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentTimerBinding.bind(view)
        applyWindowInsets()
        setupToolbar()
        setupButtons()
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
                R.id.action_settings -> { findNavController().navigate(R.id.action_timer_to_settings); true }
                R.id.action_refresh  -> { vm.refresh(); true }
                else -> false
            }
        }
    }

    private fun setupButtons() {
        b.btnStartTimer.setOnClickListener {
            val h   = b.editHours.text?.toString()?.toIntOrNull()   ?: 0
            val m   = b.editMinutes.text?.toString()?.toIntOrNull() ?: 0
            val s   = b.editSeconds.text?.toString()?.toIntOrNull() ?: 0
            val sec = h * 3600 + m * 60 + s
            if (sec <= 0) {
                Toast.makeText(requireContext(), "Set a duration greater than zero", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            vm.setTimer(sec, b.switchAutoRestart.isChecked)
            Toast.makeText(requireContext(), "Timer sent to ESP…", Toast.LENGTH_SHORT).show()
        }

        b.btnCancelTimer.setOnClickListener {
            vm.cancelTimer()
            Toast.makeText(requireContext(), "Timer cancelled", Toast.LENGTH_SHORT).show()
        }
    }

    private fun observeVm() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    vm.timerStatus.collect { status ->
                        val rem = status.remainingSeconds
                        b.timerCountdown.text = "%02d:%02d:%02d".format(
                            rem / 3600, (rem % 3600) / 60, rem % 60
                        )

                        if (status.isActive) {
                            b.chipTimerStatus.text = "● Running"
                            b.chipTimerStatus.chipBackgroundColor = ColorStateList.valueOf(
                                requireContext().getColor(R.color.status_connected_bg))
                            b.chipTimerStatus.setTextColor(
                                requireContext().getColor(R.color.status_connected_text))
                        } else {
                            b.chipTimerStatus.text = "○ Idle"
                            b.chipTimerStatus.chipBackgroundColor = ColorStateList.valueOf(
                                requireContext().getColor(R.color.status_error_bg))
                            b.chipTimerStatus.setTextColor(
                                requireContext().getColor(R.color.status_error_text))
                        }

                        if (status.isActive && status.totalSeconds > 0) {
                            b.timerProgress.isVisible = true
                            b.timerProgress.max       = status.totalSeconds
                            b.timerProgress.progress  = status.totalSeconds - rem
                        } else {
                            b.timerProgress.isVisible = false
                        }

                        val idle = !status.isActive
                        b.btnStartTimer.isEnabled     = idle
                        b.btnCancelTimer.isEnabled    = !idle
                        b.editHours.isEnabled         = idle
                        b.editMinutes.isEnabled       = idle
                        b.editSeconds.isEnabled       = idle
                        b.switchAutoRestart.isEnabled = idle

                        if (idle && status.totalSeconds > 0) {
                            val tot = status.totalSeconds
                            b.editHours.setText((tot / 3600).toString())
                            b.editMinutes.setText(((tot % 3600) / 60).toString())
                            b.editSeconds.setText((tot % 60).toString())
                            b.switchAutoRestart.isChecked = status.autoRestart
                        }
                    }
                }
            }
        }
    }
}
