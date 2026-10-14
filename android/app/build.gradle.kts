import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
}

// Release signing: android/key.properties (gitignored; see key.properties.example), or the
// KEYSTORE_FILE / KEYSTORE_PASSWORD / KEY_ALIAS / KEY_PASSWORD environment variables. With
// neither, a release build is left unsigned (Android Studio's "Generate Signed Bundle" wizard
// signs it with its own key either way).
val keystoreProperties = Properties().apply {
    val file = rootProject.file("key.properties")
    if (file.isFile) file.inputStream().use { load(it) }
}
val hasReleaseKeystore =
    keystoreProperties.getProperty("storeFile") != null || System.getenv("KEYSTORE_FILE") != null

android {
    namespace = "com.izzy2lost.ninds"
    compileSdk = 36
    buildToolsVersion = "36.1.0"
    ndkVersion = "30.0.15729638"

    defaultConfig {
        applicationId = "com.izzy2lost.ninds"
        minSdk = 28
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            // The recompiler and the NEON kernels are AArch64 code; every device worth
            // emulating a DS on is arm64.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
            }
        }
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                val configuredStore = keystoreProperties.getProperty("storeFile")
                if (configuredStore != null) {
                    storeFile = rootProject.file(configuredStore)
                    storePassword = keystoreProperties.getProperty("storePassword")
                    keyAlias = keystoreProperties.getProperty("keyAlias")
                    keyPassword = keystoreProperties.getProperty("keyPassword")
                } else {
                    storeFile = file(System.getenv("KEYSTORE_FILE"))
                    storePassword = System.getenv("KEYSTORE_PASSWORD")
                    keyAlias = System.getenv("KEY_ALIAS")
                    keyPassword = System.getenv("KEY_PASSWORD")
                }
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            // The emulator is unusably slow at -O0; debug the Kotlin side with an optimised core.
            externalNativeBuild {
                cmake {
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                }
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            if (hasReleaseKeystore) signingConfig = signingConfigs.getByName("release")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // The packaged .so is stripped, so keep a symbol file alongside the build for
            // symbolicating native crash reports.
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget.set(JvmTarget.JVM_17)
        }
    }

    sourceSets.named("main") {
        kotlin.directories.add("src/main/kotlin")
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.3"
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    implementation("com.google.android.material:material:1.12.0")
}
