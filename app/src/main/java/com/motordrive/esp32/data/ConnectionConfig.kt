package com.motordrive.esp32.data

/**
 * All connection settings persisted in SharedPreferences.
 *
 * Direct Wi-Fi mode  →  app talks to http://<directIp>:<port>/api/...
 *   • Connect phone to the ESP8266's AP  (default SSID: "MotorControl", pass: "motor1234")
 *   • Or put ESP on home Wi-Fi via POST /api/wifi/config then enter its STA IP here
 *   • Default ESP8266 soft-AP IP: 192.168.4.1
 *
 * Server mode  →  app sends requests to a custom URL instead of 192.168.4.1.
 *   • Use when the ESP is on the home network (set its LAN IP here) or reachable
 *     via port forwarding / VPN.
 *   • The ESP does NOT make outbound calls — it is always the HTTP server.
 *   • Set serverUrl to e.g. http://192.168.1.42  (ESP's LAN IP after STA connect)
 */
data class ConnectionConfig(
    val directIp:            String  = "192.168.4.1",
    val port:                Int     = 80,
    val useServerMode:       Boolean = false,
    val serverUrl:           String  = "",
    val pollIntervalSeconds: Int     = 3
) {
    /** Base URL used for every API request */
    val baseUrl: String
        get() = if (useServerMode && serverUrl.isNotBlank())
            serverUrl.trimEnd('/')
        else
            "http://$directIp:$port"
}
