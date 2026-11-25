plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.jetbrains.kotlin.android)
}

android {
    namespace = "com.mio.plugin.renderer"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.mio.plugin.renderer"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
        configureEach {
            //应用名
            //app name
            resValue("string","app_name","PanVK 1.1.9-rc7")
            //包名后缀
            //package name Suffix
            applicationIdSuffix = ".panvk"

            //驱动在启动器内显示的名称
            //The name displayed by the driver in the launcher
            manifestPlaceholders["driver"] = "PanVK 1.1.9-rc7"
            //驱动库文件名（须存在于 src/main/jniLibs/<abi>/ 下）
            //Driver library file name (must exist under src/main/jniLibs/<abi>/)
            manifestPlaceholders["driverLib"] = "libvulkan_panfrost.so"
            //驱动适配的 GPU 家族：adreno / mali / any
            //The GPU family the driver targets: adreno / mali / any
            manifestPlaceholders["driverGpu"] = "mali"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
    kotlinOptions {
        jvmTarget = "1.8"
    }
}

dependencies {
}
// ACS_SIGNING_CONFIG_START
val keystorePropertiesFile = file("keystore.properties")
val acsProps = if (keystorePropertiesFile.exists()) {
    keystorePropertiesFile.readLines()
        .filter { '=' in it }
        .associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() }
} else emptyMap()

android {
    signingConfigs {
        maybeCreate("release").apply {
            val sf = acsProps["storeFile"]
            val sp = acsProps["storePassword"]
            val ka = acsProps["keyAlias"]
            val kp = acsProps["keyPassword"]
            if (!sf.isNullOrBlank()) storeFile = file(sf)
            if (!sp.isNullOrBlank()) storePassword = sp
            if (!ka.isNullOrBlank()) keyAlias = ka
            if (!kp.isNullOrBlank()) keyPassword = kp
        }
    }
    buildTypes {
        getByName("release") {
            signingConfig = signingConfigs.getByName("release")
        }
    }
}
// ACS_SIGNING_CONFIG_END
