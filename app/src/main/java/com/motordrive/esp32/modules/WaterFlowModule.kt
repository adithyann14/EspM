package com.motordrive.esp32.modules

import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleWaterFlowBinding

/**
 * MODULE C — Pipe-end water sensor. Single-line display, no sub-text.
 *
 * waterOk semantics (from receiver):
 *   true  = water confirmed, OR motor is off (no active check)
 *   false = motor ON but waiting for flow (30 s cut-off in progress)
 *   null  = key absent — logged silently, shown as "—"
 */
class WaterFlowModule(view: View) {

    private val b = ModuleWaterFlowBinding.bind(view)

    fun update(state: MotorState) {
        val waterOk = state.waterOk
        val motorOn = state.motorOn

        when {
            waterOk == null -> {
                b.waterStatusText.text = "—"
                b.waterStatusText.setTextColor(neutral())
                // Only log the warning when we're actually connected and the
                // key is genuinely absent from a real response — not on every
                // UI refresh before the first successful poll.
                if (state.isConnected) {
                    AppLogger.log("WATER", "No sensor key in status response")
                }
            }
            !motorOn -> {
                b.waterStatusText.text = "—"
                b.waterStatusText.setTextColor(neutral())
            }
            waterOk -> {
                b.waterStatusText.text = "● Water OK"
                b.waterStatusText.setTextColor(b.root.context.getColor(R.color.motor_on))
            }
            else -> {
                b.waterStatusText.text = "⏳ Checking…"
                b.waterStatusText.setTextColor(
                    MaterialColors.getColor(b.root, androidx.appcompat.R.attr.colorError, 0)
                )
            }
        }
    }

    private fun neutral() =
        MaterialColors.getColor(b.root, com.google.android.material.R.attr.colorOutline, 0)
}
