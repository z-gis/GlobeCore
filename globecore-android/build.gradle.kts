plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.zys.globecore.android"
    compileSdk {
        version = release(37) // 与 globecore 库模块保持一致
    }

    defaultConfig {
        applicationId = "com.zys.globecore.android"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        // 与库模块 ABI 口径一致，避免运行期找不到 .so
        ndk {
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    // 演示应用与库同仓，直接用 project 依赖（外部宿主按仓库根 README「在宿主 App 中集成」以 AAR 集成）
    implementation(project(":globecore"))
    // 库模块改名后的坐标 com.zys:globecore 尚未发布到 gh-pages:/maven，发布后可切回此行：
    //implementation("com.zys:globecore:0.1.1.0")
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
}
