package com.motordrive.esp32

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * In-process logcat for the MotorDrive app.
 *
 * Usage:
 *   AppLogger.log("CONN", "Connected to ESP8266")
 *   AppLogger.log("MOTOR", "Command sent: ON")
 *
 * Observe live in UI:
 *   AppLogger.flow.collect { entries -> ... }
 *
 * Export as plain text:
 *   val text = AppLogger.export()
 *
 * Max [MAX_ENTRIES] lines; oldest are dropped when the buffer is full.
 */
data class AppLog(
    val timestamp: Long,
    val tag: String,
    val message: String
) {
    fun format(): String {
        val ts = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault()).format(Date(timestamp))
        return "[$ts] $tag: $message"
    }
}

object AppLogger {

    const val MAX_ENTRIES = 200

    private val _entries = ArrayDeque<AppLog>(MAX_ENTRIES + 1)

    private val _flow = MutableStateFlow<List<AppLog>>(emptyList())

    /** Observe this to get live updates whenever a new entry is logged. */
    val flow: StateFlow<List<AppLog>> = _flow.asStateFlow()

    @Synchronized
    fun log(tag: String, message: String) {
        _entries.addLast(AppLog(System.currentTimeMillis(), tag, message))
        if (_entries.size > MAX_ENTRIES) _entries.removeFirst()
        _flow.value = _entries.toList()
    }

    /** Clear all entries from the in-memory buffer. */
    @Synchronized
    fun clear() {
        _entries.clear()
        _flow.value = emptyList()
    }

    /** Return all entries as a single newline-separated string, suitable for sharing. */
    @Synchronized
    fun export(): String = _entries.joinToString("\n") { it.format() }
}
