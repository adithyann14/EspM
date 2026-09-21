plugins {
    alias(libs.plugins.android.application)
    // kotlin-android is NOT applied — AGP 9.0+ bundles Kotlin natively.
    // Applying it explicitly throws a fatal error. See: https://kotl.in/gradle/agp-built-in-kotlin
    alias(libs.plugins.navigation.safeargs)
}

android {
    namespace         = "com.motordrive.esp32"
    compileSdk        = 36
    buildToolsVersion = "36.1.0"

    defaultConfig {
        applicationId = "com.motordrive.esp32"
        minSdk        = 26
        targetSdk     = 36
        versionCode   = 1
        versionName   = "1.0"
    }

    // kotlinOptions { jvmTarget } is provided by the kotlin-android plugin which
    // cannot be applied on AGP 9.0+ (fatal error). compileOptions alone is enough —
    // AGP 9.0+ automatically mirrors targetCompatibility to the Kotlin JVM target.
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.ktx)
    implementation(libs.androidx.fragment.ktx)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.lifecycle.viewmodel.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.navigation.fragment.ktx)
    implementation(libs.androidx.navigation.ui.ktx)
    implementation(libs.kotlinx.coroutines.core)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.material)
}
