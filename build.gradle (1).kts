plugins {
    id("com.android.application")
}

android {
    namespace = "com.matrixrain.app"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.matrixrain.app"
        minSdk = 30            // AImageDecoder (PNG nativo) + Vulkan 1.1 garantizado
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    // Firma: se activa si CI (o tú) pasa estas propiedades con -P...
    val storeFilePath = providers.gradleProperty("RELEASE_STORE_FILE").orNull
    signingConfigs {
        if (storeFilePath != null) {
            create("release") {
                storeFile = file(storeFilePath)
                storePassword = providers.gradleProperty("RELEASE_STORE_PASSWORD").get()
                keyAlias = providers.gradleProperty("RELEASE_KEY_ALIAS").get()
                keyPassword = providers.gradleProperty("RELEASE_KEY_PASSWORD").get()
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isDebuggable = false
            signingConfig = if (storeFilePath != null) signingConfigs.getByName("release")
                            else signingConfigs.getByName("debug")
        }
        debug {
            isDebuggable = true
        }
    }

    buildFeatures {
        prefab = true          // Oboe se consume como paquete Prefab (AAR)
        shaders = true         // AGP compila src/main/shaders/*.vert|frag -> assets/shaders/*.spv
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs { useLegacyPackaging = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.9.0")
}
