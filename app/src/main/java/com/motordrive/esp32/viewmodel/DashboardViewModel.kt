package com.motordrive.esp32.viewmodel

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.MotorNotificationManager
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.data.Esp32Repository
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

// ── Preferences data classes ──────────────────────────────────────────────

data class SensorVisibility(
    val showVoltage: Boolean = true,
    val showCurrent: Boolean = true,
    val showWater:   Boolean = true
)

data class NotificationSettings(
    val persistentEnabled: Boolean = true,
    val alertEnabled:      Boolean = true
)

// ─────────────────────────────────────────────────────────────────────────

class DashboardViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs    = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val notifMgr = MotorNotificationManager(app)

    // ── Motor / connection state ──────────────────────────────────────────
    private val _motorState       = MutableStateFlow(MotorState())
    val motorState: StateFlow<MotorState> = _motorState.asStateFlow()

    private val _config           = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private val _isLoading        = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()

    private val _pendingAlerts    = MutableStateFlow<List<PendingAlert>>(emptyList())
    val pendingAlerts: StateFlow<List<PendingAlert>> = _pendingAlerts.asStateFlow()

    // ── Sensor visibility ─────────────────────────────────────────────────
    private val _sensorVisibility = MutableStateFlow(loadSensorVisibility())
    val sensorVisibility: StateFlow<SensorVisibility> = _sensorVisibility.asStateFlow()

    // ── Notifications ─────────────────────────────────────────────────────
    private val _notifSettings    = MutableStateFlow(loadNotifSettings())
    val notifSettings: StateFlow<NotificationSettings> = _notifSettings.asStateFlow()

    // ── ESP serial log ────────────────────────────────────────────────────
    private val _espLogs = MutableStateFlow<List<String>>(emptyList())
    val espLogs: StateFlow<List<String>> = _espLogs.asStateFlow()

    // ── Timer ─────────────────────────────────────────────────────────────
    private val _timerStatus = MutableStateFlow(TimerStatus())
    val timerStatus: StateFlow<TimerStatus> = _timerStatus.asStateFlow()

    // ── Scheduler ─────────────────────────────────────────────────────────
    private val _schedules   = MutableStateFlow(loadSchedulesLocal())
    val schedules: StateFlow<List<ScheduleEntry>> = _schedules.asStateFlow()

    // ── RTC + phone-time ──────────────────────────────────────────────────
    private val _rtcEnabled     = MutableStateFlow(prefs.getBoolean(K_RTC, false))
    val rtcEnabled: StateFlow<Boolean> = _rtcEnabled.asStateFlow()

    private val _phoneTimeLabel = MutableStateFlow("")
    val phoneTimeLabel: StateFlow<String> = _phoneTimeLabel.asStateFlow()

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
        // Motor status
        repo().getStatus()
            .onSuccess { new ->
                val prev = _motorState.value

                // Notifications — fire only on genuine ON↔OFF transitions
                if (prev.isConnected && prev.motorOn != new.motorOn) {
                    notifMgr.showAlert(new.motorOn, _notifSettings.value.alertEnabled)
                    AppLogger.log("NOTIF", "Alert fired: motor ${if (new.motorOn) "ON" else "OFF"}")
                }
                notifMgr.updatePersistent(new.motorOn, _notifSettings.value.persistentEnabled)

                // AppLogger: meaningful transitions
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
                if (prev.waterOk != new.waterOk && new.waterOk != null)
                    AppLogger.log("WATER", "→ ${
                        when {
                            new.waterOk == true  && new.motorOn -> "Confirmed"
                            new.waterOk == false               -> "Waiting / checking"
                            else                               -> "Idle"
                        }
                    }")

                _motorState.value = new
            }
            .onFailure { err ->
                val msg = err.message ?: "Connection failed"
                if (_motorState.value.isConnected) AppLogger.log("CONN", "Poll error: $msg")
                _motorState.value = _motorState.value.copy(
                    isConnected = false, errorMessage = msg
                )
            }

        // Timer status
        repo().getTimerStatus()
            .onSuccess { _timerStatus.value = it }

        // Alerts
        repo().getAlerts()
            .onSuccess { alerts -> if (alerts.isNotEmpty()) _pendingAlerts.value = alerts }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Motor commands
    // ═════════════════════════════════════════════════════════════════════

    fun motorOn()  = sendCmd(true)
    fun motorOff() = sendCmd(false)

    private fun sendCmd(on: Boolean) = viewModelScope.launch {
        if (_isLoading.value) return@launch
        _isLoading.value = true
        AppLogger.log("MOTOR", "Sending: ${if (on) "ON" else "OFF"}")
        notifMgr.updatePersistent(on, _notifSettings.value.persistentEnabled)
        val result = if (on) repo().motorOn() else repo().motorOff()
        if (result.isSuccess) { delay(400); poll() }
        else {
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
        AppLogger.log("TIMER", "Set ${seconds}s  autoRestart=$autoRestart")
        repo().setTimer(seconds, autoRestart)
            .onSuccess  { poll() }
            .onFailure  { AppLogger.log("TIMER", "Set failed: ${it.message}") }
    }

    fun cancelTimer() = viewModelScope.launch {
        AppLogger.log("TIMER", "Cancel")
        repo().cancelTimer()
            .onSuccess  { _timerStatus.value = TimerStatus(); poll() }
            .onFailure  { AppLogger.log("TIMER", "Cancel failed: ${it.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Scheduler
    // ═════════════════════════════════════════════════════════════════════

    fun addSchedule(entry: ScheduleEntry) = viewModelScope.launch {
        val updated = _schedules.value + entry.copy(id = _schedules.value.size)
        saveSchedulesLocal(updated)
        _schedules.value = updated
        pushSchedulesToEsp(updated)
    }

    fun updateScheduleEnabled(entry: ScheduleEntry, enabled: Boolean) = viewModelScope.launch {
        val updated = _schedules.value.map {
            if (it.id == entry.id) it.copy(enabled = enabled) else it
        }
        saveSchedulesLocal(updated)
        _schedules.value = updated
        pushSchedulesToEsp(updated)
    }

    fun deleteSchedule(entry: ScheduleEntry) = viewModelScope.launch {
        val updated = _schedules.value
            .filter { it.id != entry.id }
            .mapIndexed { i, e -> e.copy(id = i) }
        saveSchedulesLocal(updated)
        _schedules.value = updated
        pushSchedulesToEsp(updated)
    }

    fun clearAllSchedules() = viewModelScope.launch {
        AppLogger.log("SCHED", "Clear all")
        repo().clearSchedules()
            .onSuccess {
                saveSchedulesLocal(emptyList())
                _schedules.value = emptyList()
                AppLogger.log("SCHED", "Cleared")
            }
            .onFailure { AppLogger.log("SCHED", "Clear failed: ${it.message}") }
    }

    private suspend fun pushSchedulesToEsp(entries: List<ScheduleEntry>) {
        AppLogger.log("SCHED", "Pushing ${entries.size} schedule(s) to ESP")
        repo().pushSchedules(entries)
            .onFailure { AppLogger.log("SCHED", "Push failed: ${it.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  RTC / Time sync
    // ═════════════════════════════════════════════════════════════════════

    fun setRtcEnabled(enabled: Boolean) {
        _rtcEnabled.value = enabled
        prefs.edit().putBoolean(K_RTC, enabled).apply()
        AppLogger.log("RTC", if (enabled) "Hardware DS3231 enabled" else "Phone time sync mode")
        // Push immediately so ESP knows
        viewModelScope.launch {
            repo().syncTime(System.currentTimeMillis() / 1000L, enabled)
        }
    }

    /**
     * Pushes phone's Unix epoch to both ESPs every [TIME_SYNC_INTERVAL_MS].
     * The ESP uses this to advance its internal clock between syncs.
     * When RTC is enabled, this still runs — the first sync sets the DS3231.
     */
    private fun startTimeSync() {
        timeSyncJob?.cancel()
        timeSyncJob = viewModelScope.launch {
            val fmt = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
            while (isActive) {
                val epoch = System.currentTimeMillis() / 1000L
                _phoneTimeLabel.value = "Phone time: ${fmt.format(Date(epoch * 1000L))}"
                repo().syncTime(epoch, _rtcEnabled.value)
                    .onFailure { /* silently ignore — connectivity may be intermittent */ }
                delay(TIME_SYNC_INTERVAL_MS)
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  ESP log fetch (called from Settings serial-monitor card)
    // ═════════════════════════════════════════════════════════════════════

    suspend fun fetchLogs() {
        AppLogger.log("LOGS", "Fetching /api/logs …")
        repo().getLogs()
            .onSuccess { lines -> _espLogs.value = lines; AppLogger.log("LOGS", "${lines.size} line(s)") }
            .onFailure { err  -> AppLogger.log("LOGS", "Error: ${err.message}") }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  Settings updaters
    // ═════════════════════════════════════════════════════════════════════

    fun updateConfig(cfg: ConnectionConfig) {
        AppLogger.log("CONN", "Config → ${cfg.baseUrl}")
        _config.value = cfg; saveConfig(cfg); startPolling()
    }

    fun updateSensorVisibility(v: SensorVisibility) {
        _sensorVisibility.value = v; saveSensorVisibility(v)
    }

    fun updateNotifSettings(s: NotificationSettings) {
        _notifSettings.value = s; saveNotifSettings(s)
        notifMgr.updatePersistent(_motorState.value.motorOn, s.persistentEnabled)
    }

    override fun onCleared() {
        super.onCleared()
        // Persistent notification intentionally survives app backgrounding
    }

    // ─────────────────────────────────────────────────────────────────────
    private fun repo() = Esp32Repository(_config.value)

    // ═════════════════════════════════════════════════════════════════════
    //  SharedPreferences persistence
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
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                ScheduleEntry(
                    id          = o.optInt("id", i),
                    startHour   = o.optInt("startH", 6),
                    startMinute = o.optInt("startM", 0),
                    stopHour    = o.optInt("stopH",  7),
                    stopMinute  = o.optInt("stopM",  0),
                    days        = o.optInt("days",   0x7F),
                    autoRestart = o.optInt("autoRestart", 1) != 0,
                    enabled     = o.optInt("enabled",     1) != 0
                )
            }
        } catch (_: Exception) { emptyList() }
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
        private const val TIME_SYNC_INTERVAL_MS = 30_000L
    }
}
