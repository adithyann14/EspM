package com.motordrive.esp32.modules

import android.view.View
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleVoltageBinding

/**
 * MODULE A — 3-Phase Voltages
 *
 * Shows voltageR / Y / B. Turns the value orange when a phase is
 * outside the 180–260 V healthy range.
 *
 * To DISABLE: set FeatureConfig.ENABLE_VOLTAGE_SENSORS = false
 */
class VoltageModule(view: View) {

    private val b = ModuleVoltageBinding.bind(view)

    fun update(state: MotorState) {
        b.valueVoltageR.text = state.voltageR?.let { "%.1f V".format(it) } ?: "—"
        b.valueVoltageY.text = state.voltageY?.let { "%.1f V".format(it) } ?: "—"
        b.valueVoltageB.text = state.voltageB?.let { "%.1f V".format(it) } ?: "—"

        listOf(
            state.voltageR to b.valueVoltageR,
            state.voltageY to b.valueVoltageY,
            state.voltageB to b.valueVoltageB,
        ).forEach { (v, tv) ->
            if (v != null) {
                val ok = v in 180f..260f
                tv.setTextColor(
                    tv.context.getColor(if (ok) R.color.value_normal else R.color.value_warn)
                )
            }
        }
    }
}
