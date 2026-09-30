plugins {
    alias(libs.plugins.android.library)
    id("maven-publish")
}

// globecore 库版本与 Maven 坐标：与 GitHub Release tag（v0.1.1.0）保持一致
group = "com.zys"
version = "0.1.1.0"

android {
    namespace = "com.zys.globecore"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        minSdk = 24

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // globecore 为独立实现的 C++ 渲染内核（经 JNI 暴露，参照公开行为独立设计），仅打包与 app 一致的 ABI
        ndk {
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
        consumerProguardFiles("consumer-rules.pro")
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
    // 发布 release 变体的 AAR 供 maven-publish 消费（自动生成 sources jar 与依赖元数据）
    publishing {
        singleVariant("release") {
            withSourcesJar()
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    ndkVersion = "28.2.13676358"
}

dependencies {
    implementation(libs.androidx.core.ktx)
}

// AAR 产物文件名带版本号：AGP 的 bundleReleaseAar 输出名由其内部 mappedOutput 反向驱动，直接改 archiveFileName 不可靠；
// 用标准 Copy 任务额外产出一份 globecore-<version>-release.aar。输出到独立目录 outputs/aar-versioned，
// 避免与 bundleReleaseAar 的 outputs/aar 重叠触发 Gradle 陈旧产物清理而误删原始 AAR（会破坏 :publish）。
tasks.register<Copy>("copyVersionedReleaseAar") {
    from(tasks.named("bundleReleaseAar"))
    into(layout.buildDirectory.dir("outputs/aar-versioned"))
    rename { "${project.name}-${project.version}-release.aar" }
}
// 以 finalizer 方式挂钩 bundleReleaseAar（用惰性 matching+configureEach，规避 bundleReleaseAar/assembleRelease
// 在配置期尚未注册而报 not found）：任何触发打包的流程（assemble / assembleRelease / publish）都会自动产出版本号副本。
tasks.matching { it.name == "bundleReleaseAar" }.configureEach {
    finalizedBy("copyVersionedReleaseAar")
}

// Maven 发布：坐标 com.zys:globecore:<version>。
// 执行 `gradlew :globecore:publish` 会先 assembleRelease 再将 maven 布局输出到 globecore/build/repo，
// 由 .github/workflows/publish-maven.yml 部署到 gh-pages 的 /maven 目录供消费者按坐标自动下载。
afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = "com.zys"
                artifactId = "globecore"
                version = project.version.toString()
                pom {
                    name.set("globecore")
                    description.set("独立实现的 C++ 地图渲染引擎（经 JNI 暴露；静态链接 GDAL/PROJ/libcurl 等）")
                    url.set("https://github.com/z-gis/GlobeCore")
                    scm {
                        connection.set("https://github.com/z-gis/GlobeCore.git")
                        url.set("https://github.com/z-gis/GlobeCore")
                    }
                }
            }
        }
        repositories {
            maven {
                name = "buildRepo"
                url = uri(layout.buildDirectory.dir("repo"))
            }
        }
    }
}
