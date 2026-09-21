package com.motordrive.esp32

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.RingtoneManager
import androidx.core.app.NotificationCompat
import com.motordrive.esp32.ui.MainActivity

/**
 * Two notification types:
 *
 * ── CH_STATUS (persistent, no sound) ─────────────────────────────────────
 *   Ongoing shade notification: "● Motor ON" / "○ Motor OFF".
 *   Updated on every state change. Toggle in Settings → Notifications.
 *
 * ── CH_ALERT (one-shot, audible) ──────────────────────────────────────────
 *   Fired only on genuine transitions (ON→OFF or OFF→ON), never at startup.
 *   Sound + vibration via the default notification ringtone.
 *   Toggle in Settings → Notifications.
 *   (Users can also customise sound in system notification settings.)
 */
class MotorNotificationManager(private val ctx: Context) {

    companion object {
        const val CH_STATUS     = "motor_status"
        const val CH_ALERT      = "motor_alert"
        const val ID_PERSISTENT = 1001
        const val ID_ALERT      = 1002

        fun createChannels(context: Context) {
            val nm = context.getSystemService(NotificationManager::class.java)

            // Silent persistent channel
            nm.createNotificationChannel(
                NotificationChannel(CH_STATUS, "Motor Status",
                    NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Ongoing motor state indicator — no sound"
                    setSound(null, null)
                    enableVibration(false)
                }
            )

            // Audible alert channel
            val audioAttr = AudioAttributes.Builder()
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .setUsage(AudioAttributes.USAGE_NOTIFICATION).build()

            nm.createNotificationChannel(
                NotificationChannel(CH_ALERT, "Motor State Alerts",
                    NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Plays a sound when motor turns ON or OFF"
                    setSound(
                        RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION),
                        audioAttr
                    )
                    enableVibration(true)
                    vibrationPattern = longArrayOf(0, 200, 100, 200)
                }
            )
        }
    }

    private val nm = ctx.getSystemService(NotificationManager::class.java)

    private fun openAppIntent(): PendingIntent = PendingIntent.getActivity(
        ctx, 0,
        Intent(ctx, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        },
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
    )

    /** Update (or cancel) the ongoing persistent notification. */
    fun updatePersistent(motorOn: Boolean, enabled: Boolean) {
        if (!enabled) { nm.cancel(ID_PERSISTENT); return }

        val title = if (motorOn) "● Motor ON"      else "○ Motor OFF"
        val text  = if (motorOn) "Pump is running" else "Pump is stopped"
        val color = ctx.getColor(if (motorOn) R.color.motor_on else R.color.motor_off)

        nm.notify(ID_PERSISTENT,
            NotificationCompat.Builder(ctx, CH_STATUS)
                .setSmallIcon(R.drawable.ic_motor_notif)
                .setContentTitle(title).setContentText(text)
                .setColor(color).setColorized(true)
                .setOngoing(true).setOnlyAlertOnce(true).setSilent(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .setContentIntent(openAppIntent())
                .build()
        )
    }

    /**
     * One-shot audible alert. Called only on genuine ON↔OFF transitions,
     * never on the initial load (ViewModel guards against that).
     */
    fun showAlert(motorOn: Boolean, enabled: Boolean) {
        if (!enabled) return
        val title = if (motorOn) "Motor turned ON"        else "Motor turned OFF"
        val text  = if (motorOn) "Pump has started running" else "Pump has stopped"

        nm.notify(ID_ALERT,
            NotificationCompat.Builder(ctx, CH_ALERT)
                .setSmallIcon(R.drawable.ic_motor_notif)
                .setContentTitle(title).setContentText(text)
                .setAutoCancel(true)
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setContentIntent(openAppIntent())
                .build()
        )
    }

    fun cancelPersistent() = nm.cancel(ID_PERSISTENT)
}
