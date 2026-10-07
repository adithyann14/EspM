package com.motordrive.esp32.modules

import android.view.View
import android.widget.TextView
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState

/**
 * MODULE E — Wi-Fi Signal Strength
 *
 * Displays the Wi-Fi RSSI (Received Signal Strength Indicator) seen by the
 * motor-side ESP (receiver) when talking to the sender's Wi-Fi access point,
 * expressed in dBm.
 *
 * RSSI interpretation:
 *   ≥ -50 dBm  → Excellent  (very close range)
 *   -50..-65   → Good       (typical short range)
 *   -65..-75   → Fair       (borderline)
 *   -75..-85   → Weak       (reliability may drop)
 *   < -85 dBm  → Very Weak  (high packet-loss risk)
 */
class WifiRssiModule(root: View) {

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
            tvLinkStatus.text  = "Wi-Fi link is down"
            return
        }

        val rssi = state.rssi
        if (rssi == null) {
            tvRssiValue.text   = "—"
            tvRssiQuality.text = "Measuring…"
            tvRssiBars.text    = "○○○○○"
            tvLinkStatus.text  = "Waiting for first reading"
            return
        }

        tvRssiValue.text = "${rssi} dBm"
        tvLinkStatus.text = "Wi-Fi connected"

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
