package com.motordrive.esp32.data

/**
 * All connection settings persisted in SharedPreferences.
 *
 * Direct Wi-Fi mode  →  app talks to http://<directIp>:<port>/api/...
 *   • Connect phone to the ESP32's AP  (default SSID: "MotorDrive")
 *   • Or connect both to the same home/farm Wi-Fi network
 *   • Default ESP32 soft-AP IP: 192.168.4.1
 *
 * Server mode (MODULE D)  →  app talks to <serverUrl>/api/...
 *   • ESP32 connects to home Wi-Fi and reports to your server
 *   • Phone can then be anywhere in the world
 *   • Set serverUrl to e.g. http://yourserver.com:8080
 */
data class ConnectionConfig(
    val directIp:           String  = "192.168.4.1",
    val port:               Int     = 80,
    val useServerMode:      Boolean = false,          // MODULE D
    val serverUrl:          String  = "",             // MODULE D
    val pollIntervalSeconds: Int    = 3
) {
    /** Base URL used for every API request */
    val baseUrl: String
        get() = if (useServerMode && serverUrl.isNotBlank())
            serverUrl.trimEnd('/')
        else
            "http://$directIp:$port"
}
