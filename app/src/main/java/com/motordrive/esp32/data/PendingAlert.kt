package com.motordrive.esp32.data

/**
 * An event that the ESP32 recorded in non-volatile memory while the app was away.
 * Retrieved via GET /api/alerts when the app connects.
 *
 * JSON format from ESP32:
 * {
 *   "alerts": [
 *     { "type": "power_loss", "timestamp": 1718000000, "message": "Power lost at 14:32" },
 *     { "type": "phase_fault", "timestamp": 1718001234, "message": "Phase R missing at 14:52" }
 *   ]
 * }
 */
data class PendingAlert(
    val type: String,           // "power_loss" | "phase_fault" | "overload" | "custom"
    val timestamp: Long,        // Unix epoch seconds (from ESP32's RTC or millis-based estimate)
    val message: String         // Human-readable description
)
