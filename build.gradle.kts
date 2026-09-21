plugins {
    alias(libs.plugins.android.application) apply false
    // kotlin-android removed — AGP 9.0+ has built-in Kotlin. Applying it throws an error.
    alias(libs.plugins.navigation.safeargs) apply false
}
