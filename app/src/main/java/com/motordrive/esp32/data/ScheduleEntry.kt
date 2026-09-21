package com.motordrive.esp32.data

/**
 * One schedule slot. Up to 8 stored in EEPROM on both ESPs.
 * [days] bitmask: bit-0 = Sunday … bit-6 = Saturday. 0x7F = every day.
 */
data class ScheduleEntry(
    val id:          Int     = 0,
    val startHour:   Int     = 6,
    val startMinute: Int     = 0,
    val stopHour:    Int     = 7,
    val stopMinute:  Int     = 0,
    val days:        Int     = 0x7F,
    val autoRestart: Boolean = true,
    val enabled:     Boolean = true
) {
    fun startLabel() = "%02d:%02d".format(startHour, startMinute)
    fun stopLabel()  = "%02d:%02d".format(stopHour,  stopMinute)

    fun daysLabel(): String {
        val names = listOf("Su","Mo","Tu","We","Th","Fr","Sa")
        return names.filterIndexed { i, _ -> days and (1 shl i) != 0 }
            .joinToString(" ").ifBlank { "—" }
    }

    fun toJson(slot: Int): String =
        """{"slot":$slot,"startH":$startHour,"startM":$startMinute,""" +
        """"stopH":$stopHour,"stopM":$stopMinute,"days":$days,""" +
        """"autoRestart":${if (autoRestart) 1 else 0},""" +
        """"enabled":${if (enabled) 1 else 0}}"""
}
