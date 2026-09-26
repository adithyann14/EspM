package com.motordrive.esp32

/**
 * ════════════════════════════════════════════════════════════════
 *  FEATURE CONFIGURATION  —  toggle modules on / off HERE
 * ════════════════════════════════════════════════════════════════
 *
 *  MODULE A  ·  ENABLE_VOLTAGE_SENSORS
 *    Shows 3-phase voltages (R, Y, B) on the dashboard.
 *    ⚠️  No ADC voltage-sensing code exists in the ESP8266 firmware.
 *        Keys voltageR/Y/B are never emitted — cards always show "—".
 *        Set to TRUE only after adding ADC hardware + firmware support.
 *
 *  MODULE B  ·  ENABLE_CURRENT_SENSOR
 *    Shows ACS712 current reading (A) on the dashboard.
 *    Firmware reads A0 on the receiver and forwards the value.
 *    Set FALSE only if ACS712 hardware is not fitted.
 *
 *  MODULE C  ·  ENABLE_WATER_FLOW
 *    Shows pipe-end water sensor card. Active-LOW, INPUT_PULLUP.
 *    HIGH = no water (pin floating to VCC), LOW = water detected.
 *    Set FALSE only if sensor is not wired.
 *
 *  MODULE D  ·  ENABLE_SERVER_MODE
 *    Adds "Server URL" field in Settings → Connection.
 *    Use when phone accesses ESP via LAN IP or port-forwarding.
 *    (ESP does not make outbound calls — it is always the HTTP server.)
 * ════════════════════════════════════════════════════════════════
 */
object FeatureConfig {
    const val ENABLE_VOLTAGE_SENSORS = false   // MODULE A — no firmware ADC support yet
    const val ENABLE_CURRENT_SENSOR  = true    // MODULE B — ACS712 on A0, works
    const val ENABLE_WATER_FLOW      = true    // MODULE C — active-LOW sensor
    const val ENABLE_SERVER_MODE     = true    // MODULE D — custom base URL
}
