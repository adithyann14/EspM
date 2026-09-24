package com.motordrive.esp32.data

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * One motor ON or OFF event stored in the History tab.
 *
 * [trigger] describes what caused the state change:
 *   MANUAL   — user tapped ON/OFF button in the app
 *   TIMER    — app's stop-timer reached zero
 *   SCHEDULE — app's scheduler fired a start/stop entry
 *   EXTERNAL — change detected by polling but not initiated by the app
 *              (physical button on sender, ESP-side timer/schedule, power-cut auto-restart)
 */
data class MotorEvent(
    val id:        Long,
    val timestamp: Long,
    val motorOn:   Boolean,
    val trigger:   Trigger
) {
    enum class Trigger { MANUAL, TIMER, SCHEDULE, EXTERNAL }

    fun triggerLabel() = when (trigger) {
        Trigger.MANUAL   -> "Manual (app)"
        Trigger.TIMER    -> "Timer expired"
        Trigger.SCHEDULE -> "Schedule"
        Trigger.EXTERNAL -> "External (button / ESP)"
    }

    fun timeLabel(): String =
        SimpleDateFormat("dd MMM  HH:mm:ss", Locale.getDefault())
            .format(Date(timestamp))

    fun toJson(): String =
        """{"id":$id,"ts":$timestamp,"on":${if (motorOn) 1 else 0},"tr":${trigger.ordinal}}"""

    companion object {
        fun fromJson(s: String): MotorEvent? = runCatching {
            fun field(key: String) = Regex(""""$key":(-?\d+)""").find(s)?.groupValues?.get(1)?.toLong() ?: 0L
            MotorEvent(
                id        = field("id"),
                timestamp = field("ts"),
                motorOn   = field("on") != 0L,
                trigger   = Trigger.entries.getOrElse(field("tr").toInt()) { Trigger.EXTERNAL }
            )
        }.getOrNull()
    }
}
