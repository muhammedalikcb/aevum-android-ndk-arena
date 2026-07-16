// SPDX-License-Identifier: Apache-2.0
// Aevum library module - packages libaevum.so (arm64-v8a + armeabi-v7a) and the
// Kotlin API into a single consumable .aar, exporting the native headers via
// CMake Prefab so downstream NDK code can link the C ABI too.
//
// Produces: aevum-release.aar  (::assembleRelease)
// Consume:  implementation("dev.aevum:aevum:1.0.0")

plugins {
    id("com.android.library") version "8.5.2"
    id("org.jetbrains.kotlin.android") version "2.0.20"
    id("maven-publish")
}

android {
    namespace = "dev.aevum"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        minSdk = 26                     // ASharedMemory / memfd_create baseline
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                cppFlags += listOf("-std=c++20", "-fno-exceptions", "-fno-rtti")
            }
        }
        consumerProguardFiles("consumer-rules.pro")
    }

    externalNativeBuild {
        cmake {
            path = file("native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // 16 KB page-size compatibility for Android 15+ devices.
    packaging {
        jniLibs.useLegacyPackaging = false
    }

    buildFeatures {
        prefabPublishing = true
    }
    prefab {
        create("aevum") {
            headers = "native/include"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    sourceSets["main"].java.srcDirs("kotlin")

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "dev.aevum"
            artifactId = "aevum"
            version = "1.0.0"
            afterEvaluate { from(components["release"]) }
        }
    }
}
