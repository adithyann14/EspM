package com.motordrive.esp32.data

/**
 * Snapshot of everything GET /api/status returns.
 *
 * JSON shape from the sender ESP8266:
 * {
 *   "motorOn":     true,
 *   "current":     4.2,        // ACS712 EMA reading (A)
 *   "isRunning":   true,       // same as motorOn (sender mirrors relay state)
 *   "waterOk":     true,       // true = water confirmed, OR motor is off (no check)
 *                              // false = motor ON but waiting for water at pipe end
 *   "stall":       false,      // true = relay ON but current < 0.10 A for 5 s
 *   "linkOk":      true,       // true = receiver status packet arrived < 10 s ago
 *   "commsMode":   "LoRa",    // "LoRa" | "ESP-NOW" — active radio link
 *   "staIp":       "192.168.1.42"  // STA IP when on home Wi-Fi, absent otherwise
 * }
 *
 * Absent keys stay null; UI shows "—".
 */
data class MotorState(
    val motorOn: Boolean = false,

    // MODULE A: 3-phase voltages (hardware not fitted — keys absent → "—")
    val voltageR: Float? = null,
    val voltageY: Float? = null,
    val voltageB: Float? = null,

    // MODULE B: ACS712 EMA current
    val current: Float? = null,

    // MODULE C: Pipe-end water sensor (active-LOW, INPUT_PULLUP)
    // true  = water detected, OR motor is off (receiver not checking)
    // false = motor ON but no water at pipe end yet
    // null  = key absent in response
    val waterOk: Boolean? = null,

    // Fault flags
    val stall:  Boolean = false,   // relay ON but ≈0 A for 5 s → pump stalled
    val linkOk: Boolean = false,   // receiver heard within last 10 s

    // Radio link (parsed but not yet shown in UI — "LoRa" | "ESP-NOW")
    val commsMode: String? = null,

    // Network — STA IP when ESP joined home Wi-Fi, null otherwise
    val staIp: String? = null,

    // Connection metadata
    val isConnected:   Boolean = false,
    val lastUpdatedMs: Long    = 0L,
    val errorMessage:  String? = null
)
