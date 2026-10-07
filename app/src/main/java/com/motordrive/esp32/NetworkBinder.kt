package com.motordrive.esp32

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import java.net.URL

/**
 * Keeps the app's traffic on the Wi-Fi network when the ESP is on the LAN.
 *
 * The ESP's access point has no internet, so Android (and many phones'
 * "smart network switch") keeps mobile data / a VPN as the default route and
 * the request to 192.168.x.x leaves through the wrong interface
 * ("failed to connect to /192.168.4.1 … from /192.0.0.2").
 * Binding the process to the Wi-Fi network fixes that.
 *
 * Only applied while the configured ESP address is a private/LAN address, so a
 * public "server mode" URL keeps using the normal default network.
 */
object NetworkBinder {

    private var cm: ConnectivityManager? = null
    private var wifi: Network? = null
    private var wantLocal = true
    private var started = false

    private val callback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) { wifi = network; apply() }
        override fun onLost(network: Network) { if (wifi == network) { wifi = null; apply() } }
    }

    @Synchronized
    fun start(ctx: Context) {
        if (started) return
        val manager = ctx.applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        cm = manager
        runCatching {
            manager.registerNetworkCallback(
                NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .build(),
                callback
            )
            started = true
        }.onFailure { AppLogger.log("NET", "Wi-Fi binder unavailable: ${it.message}") }
    }

    /** Call whenever the configured base URL changes. */
    @Synchronized
    fun setTarget(baseUrl: String) {
        wantLocal = isLocalUrl(baseUrl)
        apply()
    }

    @Synchronized
    private fun apply() {
        val manager = cm ?: return
        val target = if (wantLocal) wifi else null
        runCatching { manager.bindProcessToNetwork(target) }
            .onSuccess {
                AppLogger.log("NET", if (target != null) "Bound to Wi-Fi network" else "Using default network")
            }
            .onFailure { AppLogger.log("NET", "Bind failed: ${it.message}") }
    }

    private fun isLocalUrl(baseUrl: String): Boolean {
        val host = runCatching { URL(baseUrl).host }.getOrNull() ?: return true
        if (host.endsWith(".local")) return true
        val p = host.split('.').mapNotNull { it.toIntOrNull() }
        if (p.size != 4) return false
        return p[0] == 10 ||
               (p[0] == 192 && p[1] == 168) ||
               (p[0] == 172 && p[1] in 16..31) ||
               (p[0] == 169 && p[1] == 254)
    }
}
