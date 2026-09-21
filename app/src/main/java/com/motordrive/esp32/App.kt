package com.motordrive.esp32

import android.app.Application

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        MotorNotificationManager.createChannels(this)
    }
}
