package com.motordrive.esp32.viewmodel

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.MotorNotificationManager
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.data.Esp32Repository
import com.motordrive.esp32.data.MotorEvent
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.data.PendingAlert
import com.motordrive.esp32.data.ScheduleEntry
import com.motordrive.esp32.data.TimerStatus
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class SensorVisibility(
    val showVoltage: Boolean = true,
    val showCurrent: Boolean = true,
    val showWater:   Boolean = true
)

data class NotificationSettings(
    val persistentEnabled: Boolean = true,
    val alertEnabled:      Boolean = true
)

class DashboardViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs    = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val notifMgr = MotorNotificationManager(app)

    // ── Core state ────────────────────────────────────────────────────────
    private val _motorState       = MutableStateFlow(MotorState())
    val motorState: StateFlow<MotorState> = _motorState.asStateFlow()

    private val _config           = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private val _isLoading        = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()

    private val _pendingAlerts    = MutableStateFlow<List<PendingAlert>>(emptyList())
    val pendingAlerts: StateFlow<List<PendingAlert>> = _pendingAlerts.asStateFlow()

    private val _sensorVisibility = MutableStateFlow(loadSensorVisibility())
    val sensorVisibility: StateFlow<SensorVisibility> = _sensorVisibility.asStateFlow()

    private val _notifSettings    = MutableStateFlow(loadNotifSettings())
    val notifSettings: StateFlow<NotificationSettings> = _notifSettings.asStateFlow()

    private val _espLogs          = MutableStateFlow<List<String>>(emptyList())
    val espLogs: StateFlow<List<String>> = _espLogs.asStateFlow()

    // ── Timer ─────────────────────────────────────────────────────────────
    private val _timerStatus = MutableStateFlow(TimerStatus())
    val timerStatus: StateFlow<TimerStatus> = _timerStatus.asStateFlow()

    // ── Scheduler ─────────────────────────────────────────────────────────
    private val _schedules   = MutableStateFlow(loadSchedulesLocal())
    val schedules: StateFlow<List<ScheduleEntry>> = _schedules.asStateFlow()

    // ── RTC ───────────────────────────────────────────────────────────────
    private val _rtcEnabled     = MutableStateFlow(prefs.getBoolean(K_RTC, false))
    val rtcEnabled: StateFlow<Boolean> = _rtcEnabled.asStateFlow()

    private val _phoneTimeLabel = MutableStateFlow("")
    val phoneTimeLabel: StateFlow<String> = _phoneTimeLabel.asStateFlow()

    // ── History ───────────────────────────────────────────────────────────
    private val _motorHistory = MutableStateFlow(loadHistoryLocal())
    val motorHistory: StateFlow<List<MotorEvent>> = _motorHistory.asStateFlow()

    /**
     * Set before sendCmd() so the next poll can label the cause correctly.
     * MANUAL if the app initiated it, null if we don't know (external change).
     */
    private var expectedTrigger: MotorEvent.Trigger? = null
    private var expectedMotorOn: Boolean?             = null

    // ── Jobs ──────────────────────────────────────────────────────────────
    private var pollJob:     Job? = null
    private var timeSyncJob: Job? = null

    init {
        AppLogger.log("VM", "ViewModel started")
        startPolling()
        startTimeSync()
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Polling
    // ═════════════════════════════════════════════════════════════════════

    fun startPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (isActive) { poll(); delay(_config.value.pollIntervalSeconds * 1_000L) }
        }
    }

    fun stopPolling() { pollJob?.cancel() }

    private suspend fun poll() {
        repo().getStatus()
            .onSuccess { new ->
                val prev = _motorState.value

                // ── Notifications ─────────────────────────────────────
                if (prev.isConnected && prev.motorOn != new.motorOn) {
                    notifMgr.showAlert(new.motorOn, _notifSettings.value.alertEnabled)
                }
                notifMgr.updatePersistent(new.motorOn, _notifSettings.value.persistentEnabled)

                // ── History tracking ──────────────────────────────────
                if (prev.isConnected && prev.motorOn != new.motorOn) {
                    val trigger = if (expectedMotorOn == new.motorOn && expectedTrigger != null) {
                        expectedTrigger!!
                    } else {
                        MotorEvent.Trigger.EXTERNAL
                    }
                    expectedMotorOn  = null
                    expectedTrigger  = null
                    recordMotorEvent(new.motorOn, trigger)
                }

                // ── AppLogger transitions ─────────────────────────────
                if (!prev.isConnected && new.isConnected)
                    AppLogger.log("CONN",    "Connected → ${_config.value.baseUrl}")
                if (prev.isConnected && !new.isConnected)
                    AppLogger.log("CONN",    "Connection lost")
                if (prev.motorOn != new.motorOn)
                    AppLogger.log("MOTOR",   "→ ${if (new.motorOn) "ON" else "OFF"}")
                if (!prev.stall && new.stall)
                    AppLogger.log("STALL",   "⚠ Stall — relay ON but no current for 5 s")
                if (prev.stall && !new.stall)
                    AppLogger.log("STALL",   "Cleared")
                if (prev.linkOk != new.linkOk)
                    AppLogger.log("ESP-NOW", "RF link ${if (new.linkOk) "UP ✓" else "DOWN ✗"}")

                _motorState.value = new
            }
            .onFailure { err ->
                val msg = err.message ?: "Connection failed"
                if (_motorState.value.isConnected) AppLogger.log("CONN", "Poll error: $msg")
                _motorState.value = _motorState.value.copy(
                    isConnected = false, errorMessage = msg
                )
            }

        repo().getTimerStatus().onSuccess { _timerStatus.value = it }

        repo().getAlerts()
            .onSuccess { alerts -> if (alerts.isNotEmpty()) _pendingAlerts.value = alerts }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Motor commands
    // ═════════════════════════════════════════════════════════════════════

    fun motorOn()  = sendCmd(true,  MotorEvent.Trigger.MANUAL)
    fun motorOff() = sendCmd(false, MotorEvent.Trigger.MANUAL)

    private fun sendCmd(on: Boolean, trigger: MotorEvent.Trigger) = viewModelScope.launch {
        if (_isLoading.value) return@launch
        _isLoading.value = true
        AppLogger.log("MOTOR", "Sending: ${if (on) "ON" else "OFF"} [$trigger]")

        // Mark expected change so poll() credits it correctly
        expectedMotorOn = on
        expectedTrigger = trigger

        notifMgr.updatePersistent(on, _notifSettings.value.persistentEnabled)
        val result = if (on) repo().motorOn() else repo().motorOff()
        if (result.isSuccess) { delay(400); poll() }
        else {
            expectedMotorOn = null; expectedTrigger = null
            val msg = result.exceptionOrNull()?.message ?: "Command failed"
            AppLogger.log("MOTOR", "Failed: $msg")
            _motorState.value = _motorState.value.copy(errorMessage = msg)
        }
        _isLoading.value = false
    }

    fun refresh() = viewModelScope.launch { poll() }

    fun clearAlerts() = viewModelScope.launch {
        repo().clearAlerts(); _pendingAlerts.value = emptyList()
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Timer
    // ═════════════════════════════════════════════════════════════════════

    fun setTimer(seconds: Int, autoRestart: Boolean) = viewModelScope.launch {
        AppLogger.log("TIMER", "Set ${seconds}s  auto=$autoRestart")
        repo().setTimer(seconds, autoRestart)
            .onSuccess { poll() }
            .onFailure { AppLogger.log("TIMER", "Set failed: ${it.message}") }
    }

    fun cancelTimer() = viewModelScope.launch {
        AppLogger.log("TIMER", "Cancel")
        repo().cancelTimer()
            .onSuccess { _timerStatus.value = TimerStatus(); poll() }
            .onFailure { AppLogger.log("TIMER", "Cancel failed: ${it.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Scheduler
    // ═════════════════════════════════════════════════════════════════════

    fun addSchedule(entry: ScheduleEntry) = viewModelScope.launch {
        val updated = _schedules.value + entry.copy(id = _schedules.value.size)
        saveSchedulesLocal(updated); _schedules.value = updated; pushSchedulesToEsp(updated)
    }

    fun updateScheduleEnabled(entry: ScheduleEntry, enabled: Boolean) = viewModelScope.launch {
        val updated = _schedules.value.map { if (it.id == entry.id) it.copy(enabled = enabled) else it }
        saveSchedulesLocal(updated); _schedules.value = updated; pushSchedulesToEsp(updated)
    }

    fun deleteSchedule(entry: ScheduleEntry) = viewModelScope.launch {
        val updated = _schedules.value.filter { it.id != entry.id }.mapIndexed { i, e -> e.copy(id = i) }
        saveSchedulesLocal(updated); _schedules.value = updated; pushSchedulesToEsp(updated)
    }

    fun clearAllSchedules() = viewModelScope.launch {
        repo().clearSchedules().onSuccess {
            saveSchedulesLocal(emptyList()); _schedules.value = emptyList()
            AppLogger.log("SCHED", "All cleared")
        }.onFailure { AppLogger.log("SCHED", "Clear failed: ${it.message}") }
    }

    private suspend fun pushSchedulesToEsp(entries: List<ScheduleEntry>) {
        repo().pushSchedules(entries)
            .onFailure { AppLogger.log("SCHED", "Push failed: ${it.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  RTC / Time sync
    // ═════════════════════════════════════════════════════════════════════

    fun setRtcEnabled(enabled: Boolean) {
        _rtcEnabled.value = enabled
        prefs.edit().putBoolean(K_RTC, enabled).apply()
        viewModelScope.launch { repo().syncTime(System.currentTimeMillis() / 1000L, enabled) }
    }

    private fun startTimeSync() {
        timeSyncJob?.cancel()
        timeSyncJob = viewModelScope.launch {
            val fmt = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
            while (isActive) {
                val epoch = System.currentTimeMillis() / 1000L
                _phoneTimeLabel.value = "Phone time: ${fmt.format(Date(epoch * 1000L))}"
                repo().syncTime(epoch, _rtcEnabled.value)
                delay(30_000L)
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  History
    // ═════════════════════════════════════════════════════════════════════

    private fun recordMotorEvent(motorOn: Boolean, trigger: MotorEvent.Trigger) {
        val event = MotorEvent(
            id        = System.currentTimeMillis(),
            timestamp = System.currentTimeMillis(),
            motorOn   = motorOn,
            trigger   = trigger
        )
        val updated = (_motorHistory.value + event).takeLast(MAX_HISTORY)
        _motorHistory.value = updated
        saveHistoryLocal(updated)
        AppLogger.log("HIST", "${if (motorOn) "ON" else "OFF"} — ${event.triggerLabel()}")
    }

    fun clearHistory() {
        _motorHistory.value = emptyList()
        prefs.edit().remove(K_HISTORY).apply()
        AppLogger.log("HIST", "History cleared")
    }

    // ═════════════════════════════════════════════════════════════════════
    //  ESP log fetch
    // ═════════════════════════════════════════════════════════════════════

    suspend fun fetchLogs() {
        AppLogger.log("LOGS", "Fetching /api/logs …")
        repo().getLogs()
            .onSuccess { lines -> _espLogs.value = lines; AppLogger.log("LOGS", "${lines.size} line(s)") }
            .onFailure { AppLogger.log("LOGS", "Error: ${it.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Settings
    // ═════════════════════════════════════════════════════════════════════

    fun updateConfig(cfg: ConnectionConfig) {
        _config.value = cfg; saveConfig(cfg); startPolling()
        AppLogger.log("CONN", "Config → ${cfg.baseUrl}")
    }

    fun updateSensorVisibility(v: SensorVisibility) { _sensorVisibility.value = v; saveSensorVisibility(v) }

    fun updateNotifSettings(s: NotificationSettings) {
        _notifSettings.value = s; saveNotifSettings(s)
        notifMgr.updatePersistent(_motorState.value.motorOn, s.persistentEnabled)
    }

    // ─────────────────────────────────────────────────────────────────────
    private fun repo() = Esp32Repository(_config.value)

    // ═════════════════════════════════════════════════════════════════════
    //  Persistence helpers
    // ═════════════════════════════════════════════════════════════════════

    private fun loadConfig() = ConnectionConfig(
        directIp = prefs.getString(K_IP, "192.168.4.1") ?: "192.168.4.1",
        port = prefs.getInt(K_PORT, 80),
        useServerMode = prefs.getBoolean(K_SRV_MODE, false),
        serverUrl = prefs.getString(K_SRV_URL, "") ?: "",
        pollIntervalSeconds = prefs.getInt(K_POLL, 3)
    )
    private fun saveConfig(c: ConnectionConfig) = prefs.edit()
        .putString(K_IP, c.directIp).putInt(K_PORT, c.port)
        .putBoolean(K_SRV_MODE, c.useServerMode).putString(K_SRV_URL, c.serverUrl)
        .putInt(K_POLL, c.pollIntervalSeconds).apply()

    private fun loadSensorVisibility() = SensorVisibility(
        showVoltage = prefs.getBoolean(K_SHOW_VOLTAGE, true),
        showCurrent = prefs.getBoolean(K_SHOW_CURRENT, true),
        showWater   = prefs.getBoolean(K_SHOW_WATER,   true)
    )
    private fun saveSensorVisibility(v: SensorVisibility) = prefs.edit()
        .putBoolean(K_SHOW_VOLTAGE, v.showVoltage)
        .putBoolean(K_SHOW_CURRENT, v.showCurrent)
        .putBoolean(K_SHOW_WATER,   v.showWater).apply()

    private fun loadNotifSettings() = NotificationSettings(
        persistentEnabled = prefs.getBoolean(K_NOTIF_PERSISTENT, true),
        alertEnabled      = prefs.getBoolean(K_NOTIF_ALERT,      true)
    )
    private fun saveNotifSettings(s: NotificationSettings) = prefs.edit()
        .putBoolean(K_NOTIF_PERSISTENT, s.persistentEnabled)
        .putBoolean(K_NOTIF_ALERT,      s.alertEnabled).apply()

    private fun loadSchedulesLocal(): List<ScheduleEntry> {
        val raw = prefs.getString(K_SCHEDULES, null) ?: return emptyList()
        return runCatching {
            val arr = JSONArray(raw)
            (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                ScheduleEntry(
                    id          = o.optInt("id", i),
                    startHour   = o.optInt("startH", 6),   startMinute = o.optInt("startM", 0),
                    stopHour    = o.optInt("stopH",  7),    stopMinute  = o.optInt("stopM",  0),
                    days        = o.optInt("days",   0x7F),
                    autoRestart = o.optInt("autoRestart", 1) != 0,
                    enabled     = o.optInt("enabled",     1) != 0
                )
            }
        }.getOrDefault(emptyList())
    }
    private fun saveSchedulesLocal(entries: List<ScheduleEntry>) {
        val arr = JSONArray()
        entries.forEachIndexed { i, e ->
            arr.put(JSONObject().apply {
                put("id", e.id); put("startH", e.startHour); put("startM", e.startMinute)
                put("stopH", e.stopHour); put("stopM", e.stopMinute); put("days", e.days)
                put("autoRestart", if (e.autoRestart) 1 else 0)
                put("enabled",     if (e.enabled)     1 else 0)
            })
        }
        prefs.edit().putString(K_SCHEDULES, arr.toString()).apply()
    }

    private fun loadHistoryLocal(): List<MotorEvent> {
        val raw = prefs.getString(K_HISTORY, null) ?: return emptyList()
        return runCatching {
            val arr = JSONArray(raw)
            (0 until arr.length()).mapNotNull { MotorEvent.fromJson(arr.getString(it)) }
        }.getOrDefault(emptyList())
    }
    private fun saveHistoryLocal(events: List<MotorEvent>) {
        val arr = JSONArray()
        events.forEach { arr.put(it.toJson()) }
        prefs.edit().putString(K_HISTORY, arr.toString()).apply()
    }

    companion object {
        private const val PREFS              = "mdc_prefs"
        private const val K_IP               = "ip"
        private const val K_PORT             = "port"
        private const val K_SRV_MODE         = "server_mode"
        private const val K_SRV_URL          = "server_url"
        private const val K_POLL             = "poll_sec"
        private const val K_SHOW_VOLTAGE     = "show_voltage"
        private const val K_SHOW_CURRENT     = "show_current"
        private const val K_SHOW_WATER       = "show_water"
        private const val K_NOTIF_PERSISTENT = "notif_persistent"
        private const val K_NOTIF_ALERT      = "notif_alert"
        private const val K_RTC              = "rtc_enabled"
        private const val K_SCHEDULES        = "schedules_json"
        private const val K_HISTORY          = "motor_history"
        private const val MAX_HISTORY        = 500
    }
}
