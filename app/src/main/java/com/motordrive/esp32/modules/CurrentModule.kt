package com.motordrive.esp32.modules

import android.view.View
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleCurrentBinding

/**
 * MODULE B — Current Sensor.
 *
 * Shows the ACS712 EMA current reading ONLY while the motor is commanded ON.
 *
 * Why hide it when motor is off:
 *   The ACS712 always produces a small non-zero output due to its internal
 *   zero-current offset and noise (typically ±0.05–0.08 A). Displaying these
 *   values while the motor is off is misleading — the user would see "0.06 A"
 *   when nothing is running. We gate on [MotorState.motorOn] so the card
 *   shows "—" at rest and the real reading only when current actually flows.
 *
 * To DISABLE entirely: set FeatureConfig.ENABLE_CURRENT_SENSOR = false
 */
class CurrentModule(view: View) {

    private val b = ModuleCurrentBinding.bind(view)

    fun update(state: MotorState) {
        // Show "—" when motor is off regardless of ACS712 noise reading
        val displayCurrent: Float? = if (state.motorOn) state.current else null
        b.valueCurrentA.text = displayCurrent?.let { "%.2f A".format(it) } ?: "—"
    }
}
