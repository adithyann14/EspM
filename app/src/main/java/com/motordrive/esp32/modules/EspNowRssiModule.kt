package com.motordrive.esp32.modules

import android.view.View
import android.widget.TextView
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState

/**
 * MODULE E — ESP-NOW Link Signal Strength
 *
 * Displays the RSSI (Received Signal Strength Indicator) of the ESP-NOW
 * radio link between the sender and receiver, expressed in dBm.
 *
 * RSSI interpretation:
 *   ≥ -50 dBm  → Excellent  (very close range)
 *   -50..-65   → Good       (typical short range)
 *   -65..-75   → Fair       (borderline)
 *   -75..-85   → Weak       (reliability may drop)
 *   < -85 dBm  → Very Weak  (high packet-loss risk)
 */
class EspNowRssiModule(root: View) {

    private val tvRssiValue:   TextView = root.findViewById(R.id.tvRssiValue)
    private val tvRssiQuality: TextView = root.findViewById(R.id.tvRssiQuality)
    private val tvRssiBars:    TextView = root.findViewById(R.id.tvRssiBars)
    private val tvLinkStatus:  TextView = root.findViewById(R.id.tvRssiLinkStatus)

    fun update(state: MotorState) {
        if (!state.isConnected) {
            tvRssiValue.text   = "—"
            tvRssiQuality.text = "No Connection"
            tvRssiBars.text    = "○○○○○"
            tvLinkStatus.text  = "Disconnected"
            return
        }

        if (!state.linkOk) {
            tvRssiValue.text   = "—"
            tvRssiQuality.text = "Link Down"
            tvRssiBars.text    = "○○○○○"
            tvLinkStatus.text  = "ESP-NOW link is down"
            return
        }

        val rssi = state.rssi
        if (rssi == null) {
            tvRssiValue.text   = "—"
            tvRssiQuality.text = "Measuring…"
            tvRssiBars.text    = "○○○○○"
            tvLinkStatus.text  = "Waiting for first packet"
            return
        }

        tvRssiValue.text = "${rssi} dBm"
        tvLinkStatus.text = "ESP-NOW link active"

        when {
            rssi >= -50 -> {
                tvRssiQuality.text = "Excellent"
                tvRssiBars.text    = "●●●●●"
            }
            rssi >= -60 -> {
                tvRssiQuality.text = "Good"
                tvRssiBars.text    = "●●●●○"
            }
            rssi >= -70 -> {
                tvRssiQuality.text = "Fair"
                tvRssiBars.text    = "●●●○○"
            }
            rssi >= -80 -> {
                tvRssiQuality.text = "Weak"
                tvRssiBars.text    = "●●○○○"
            }
            else -> {
                tvRssiQuality.text = "Very Weak"
                tvRssiBars.text    = "●○○○○"
            }
        }
    }
}
