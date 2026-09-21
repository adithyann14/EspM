package com.motordrive.esp32.data

/** Live status of the stop-timer as reported by the ESP. */
data class TimerStatus(
    val isActive:         Boolean = false,
    val remainingSeconds: Int     = 0,
    val totalSeconds:     Int     = 0,
    val autoRestart:      Boolean = false
)
