package com.motordrive.esp32.modules

import android.content.res.ColorStateList
import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleWaterFlowBinding

/**
 * MODULE C — Pipe-end water sensor.
 *
 * waterOk semantics (from receiver):
 *   true  = water confirmed at pipe end, OR motor is off (no check active)
 *   false = motor is ON but waiting for water (30 s auto cut-off in progress)
 *   null  = key absent in response (old firmware or sensor not connected)
 *
 * Cut-off logic (30 s) runs on the receiver, not here.
 * To DISABLE: set FeatureConfig.ENABLE_WATER_FLOW = false
 */
class WaterFlowModule(view: View) {

    private val b = ModuleWaterFlowBinding.bind(view)

    fun update(state: MotorState) {
        val waterOk = state.waterOk
        val motorOn = state.motorOn

        val (statusText, subText, colorAttr) = when {
            waterOk == null -> Triple(
                "No Data",
                "Sensor not connected or firmware too old",
                com.google.android.material.R.attr.colorOutline
            )
            !motorOn -> Triple(
                "—",
                "Motor is off — no check active",
                com.google.android.material.R.attr.colorOutline
            )
            waterOk -> Triple(
                "● Water OK",
                "Flow confirmed at pipe end",
                androidx.appcompat.R.attr.colorPrimary
            )
            else -> Triple(
                "⏳ Checking…",
                "Waiting for flow — auto cut-off at 30 s",
                androidx.appcompat.R.attr.colorError
            )
        }

        b.waterStatusText.text = statusText
        b.waterSubtext.text    = subText
        val color = MaterialColors.getColor(b.root, colorAttr, 0)
        b.waterIcon.imageTintList = ColorStateList.valueOf(color)
        b.waterStatusText.setTextColor(color)
    }
}
