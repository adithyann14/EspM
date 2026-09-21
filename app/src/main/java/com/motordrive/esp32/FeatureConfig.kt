package com.motordrive.esp32

/**
 * ════════════════════════════════════════════════════════════════
 *  FEATURE CONFIGURATION  —  toggle modules on / off HERE
 * ════════════════════════════════════════════════════════════════
 *
 *  MODULE A  ·  ENABLE_VOLTAGE_SENSORS
 *    Shows 3-phase voltages (R, Y, B) on the dashboard.
 *    Disable if voltage sensors are not physically fitted.
 *
 *  MODULE B  ·  ENABLE_CURRENT_SENSOR
 *    Shows ACS712 current reading (A) and derives motor running
 *    state from current > threshold. Replaces vibration sensor.
 *    Disable if ACS712 is not fitted.
 *
 *  MODULE C  ·  ENABLE_WATER_FLOW
 *    Shows a Water Flow card. Disable if sensor not connected.
 *
 *  MODULE D  ·  ENABLE_SERVER_MODE
 *    Adds "Server URL" field in Settings for global control.
 * ════════════════════════════════════════════════════════════════
 */
object FeatureConfig {
    const val ENABLE_VOLTAGE_SENSORS = true    // MODULE A
    const val ENABLE_CURRENT_SENSOR  = true    // MODULE B
    const val ENABLE_WATER_FLOW      = true    // MODULE C
    const val ENABLE_SERVER_MODE     = true    // MODULE D
}
