package com.motordrive.esp32.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

class Esp32Repository(private val config: ConnectionConfig) {

    companion object {
        private const val CONNECT_TIMEOUT = 5_000
        private const val READ_TIMEOUT    = 5_000
    }

    // ── Motor control ─────────────────────────────────────────────────────
    suspend fun getStatus():   Result<MotorState>        = io { parseStatus(httpGet("${config.baseUrl}/api/status")) }
    suspend fun motorOn():     Result<Unit>               = io { httpPost("${config.baseUrl}/api/motor/on");    Unit }
    suspend fun motorOff():    Result<Unit>               = io { httpPost("${config.baseUrl}/api/motor/off");   Unit }

    // ── Alerts ────────────────────────────────────────────────────────────
    suspend fun getAlerts():   Result<List<PendingAlert>> = io { parseAlerts(httpGet("${config.baseUrl}/api/alerts")) }
    suspend fun clearAlerts(): Result<Unit>               = io { httpPost("${config.baseUrl}/api/alerts/clear"); Unit }

    // ── Serial log ring-buffer ────────────────────────────────────────────
    // Returns empty list gracefully if firmware is old / endpoint absent.
    suspend fun getLogs(): Result<List<String>> = io {
        try {
            val json = httpGet("${config.baseUrl}/api/logs")
            val arr  = JSONObject(json).optJSONArray("logs") ?: return@io emptyList()
            (0 until arr.length())
                .map { arr.optString(it, "").trim() }
                .filter { it.isNotBlank() }
        } catch (_: IOException) { emptyList() }
    }

    // ── Timer ─────────────────────────────────────────────────────────────
    suspend fun setTimer(seconds: Int, autoRestart: Boolean): Result<Unit> = io {
        val body = """{"seconds":$seconds,"autoRestart":${if (autoRestart) 1 else 0}}"""
        httpPostJson("${config.baseUrl}/api/timer/set", body); Unit
    }

    suspend fun cancelTimer(): Result<Unit> = io {
        httpPost("${config.baseUrl}/api/timer/cancel"); Unit
    }

    suspend fun getTimerStatus(): Result<TimerStatus> = io {
        val o = JSONObject(httpGet("${config.baseUrl}/api/timer/status"))
        TimerStatus(
            isActive         = o.optBoolean("active",      false),
            remainingSeconds = o.optInt("remaining",        0),
            totalSeconds     = o.optInt("total",            0),
            autoRestart      = o.optBoolean("autoRestart",  false)
        )
    }

    // ── Scheduler ─────────────────────────────────────────────────────────
    suspend fun pushSchedules(entries: List<ScheduleEntry>): Result<Unit> = io {
        val arr = JSONArray()
        entries.forEachIndexed { slot, e -> arr.put(JSONObject(e.toJson(slot))) }
        val body = """{"count":${entries.size},"entries":$arr}"""
        httpPostJson("${config.baseUrl}/api/schedule/push", body); Unit
    }

    suspend fun clearSchedules(): Result<Unit> = io {
        httpPost("${config.baseUrl}/api/schedule/clear"); Unit
    }

    // ── Time sync ─────────────────────────────────────────────────────────
    suspend fun syncTime(epochSeconds: Long, rtcEnabled: Boolean): Result<Unit> = io {
        val body = """{"epoch":$epochSeconds,"rtcEnabled":${if (rtcEnabled) 1 else 0}}"""
        httpPostJson("${config.baseUrl}/api/time/sync", body); Unit
    }

    // ── Dry-run timeout ───────────────────────────────────────────────────
    /**
     * Push the water-sensor dry-run cutoff duration to the ESP firmware.
     * Firmware endpoint: POST /api/config/drytimeout  {"dryRunSec":<int>}
     * Gracefully ignored if the firmware doesn't support it yet.
     */
    suspend fun setDryRunTimeout(seconds: Int): Result<Unit> = io {
        val body = """{"dryRunSec":$seconds}"""
        httpPostJson("${config.baseUrl}/api/config/drytimeout", body)
        Unit
    }

    // ── Wi-Fi STA config ───────────────────────────────────────────────────────────
    /** Push home Wi-Fi credentials to the ESP8266 so it can join the LAN. */
    suspend fun configureWifi(ssid: String, pass: String): Result<Unit> = io {
        val body = """{"ssid":"$ssid","pass":"$pass"}"""
        httpPostJson("${config.baseUrl}/api/wifi/config", body)
        Unit
    }

    data class WifiStatus(
        val staConnected: Boolean,
        val staIp:        String?,
        val apIp:         String,
        val staSSID:      String?
    )

    /** Read the ESP’s current Wi-Fi state (AP always up, STA when joined). */
    suspend fun getWifiStatus(): Result<WifiStatus> = io {
        val o = JSONObject(httpGet("${config.baseUrl}/api/wifi/status"))
        WifiStatus(
            staConnected = o.optBoolean("staConnected", false),
            staIp        = o.optString("staIp").takeIf  { it.isNotEmpty() && it != "null" },
            apIp         = o.optString("apIp", "192.168.4.1"),
            staSSID      = o.optString("staSSID").takeIf { it.isNotEmpty() && it != "null" }
        )
    }

    // ── OTA ───────────────────────────────────────────────────────────────
    /**
     * Asks the Sender to send an ESP-NOW OTA-enable packet to the Receiver.
     * The Receiver switches to AP mode "MCReceiver-OTA" and starts a
     * password-protected HTTP update server for 5 minutes.
     */
    suspend fun enableReceiverOta(password: String): Result<Unit> = io {
        val body = """{"pass":"${password}"}"""
        httpPostJson("${config.baseUrl}/api/ota/receiver", body)
        Unit
    }

    // ── HTTP helpers ──────────────────────────────────────────────────────
    private suspend fun <T> io(block: () -> T): Result<T> =
        withContext(Dispatchers.IO) { runCatching(block) }

    @Throws(IOException::class)
    private fun httpGet(urlStr: String): String {
        val conn = open(urlStr, "GET")
        try {
            if (conn.responseCode !in 200..299) throw IOException("HTTP ${conn.responseCode}")
            return conn.inputStream.bufferedReader().readText()
        } finally { conn.disconnect() }
    }

    @Throws(IOException::class)
    private fun httpPost(urlStr: String): String {
        val conn = open(urlStr, "POST").apply {
            doOutput = true; setRequestProperty("Content-Length", "0"); outputStream.close()
        }
        try {
            if (conn.responseCode !in 200..299) throw IOException("HTTP ${conn.responseCode}")
            return runCatching { conn.inputStream.bufferedReader().readText() }.getOrDefault("")
        } finally { conn.disconnect() }
    }

    @Throws(IOException::class)
    private fun httpPostJson(urlStr: String, jsonBody: String): String {
        val bytes = jsonBody.toByteArray(Charsets.UTF_8)
        val conn  = open(urlStr, "POST").apply {
            doOutput = true
            setRequestProperty("Content-Type",   "application/json")
            setRequestProperty("Content-Length",  bytes.size.toString())
            outputStream.write(bytes); outputStream.close()
        }
        try {
            if (conn.responseCode !in 200..299) throw IOException("HTTP ${conn.responseCode}")
            return runCatching { conn.inputStream.bufferedReader().readText() }.getOrDefault("")
        } finally { conn.disconnect() }
    }

    private fun open(urlStr: String, method: String): HttpURLConnection =
        (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = CONNECT_TIMEOUT; readTimeout = READ_TIMEOUT
            setRequestProperty("Accept", "application/json")
        }

    // ── JSON parsers ──────────────────────────────────────────────────────
    private fun parseStatus(json: String): MotorState {
        val o = JSONObject(json)
        return MotorState(
            motorOn       = o.optBoolean("motorOn", false),
            voltageR      = o.floatOrNull("voltageR"),
            voltageY      = o.floatOrNull("voltageY"),
            voltageB      = o.floatOrNull("voltageB"),
            current       = o.floatOrNull("current"),
            waterOk       = o.boolOrNull("waterOk"),
            stall         = o.optBoolean("stall",  false),
            linkOk        = o.optBoolean("linkOk", false),
            commsMode     = o.optString("commsMode").takeIf { it.isNotEmpty() },
            staIp         = o.optString("staIp").takeIf { it.isNotEmpty() && it != "null" },
            isConnected   = true,
            lastUpdatedMs = System.currentTimeMillis(),
            errorMessage  = null
        )
    }

    private fun parseAlerts(json: String): List<PendingAlert> {
        val arr = JSONObject(json).optJSONArray("alerts") ?: return emptyList()
        return (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            PendingAlert(o.optString("type","unknown"), o.optLong("timestamp",0L), o.optString("message","?"))
        }
    }

    private fun JSONObject.floatOrNull(key: String): Float? {
        if (!has(key) || isNull(key)) return null
        val d = optDouble(key, Double.NaN); return if (d.isNaN()) null else d.toFloat()
    }
    private fun JSONObject.boolOrNull(key: String): Boolean? =
        if (has(key) && !isNull(key)) optBoolean(key) else null
}
