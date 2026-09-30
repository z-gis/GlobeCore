# globecore-harmonyos/library —— HarmonyOS NEXT native 构建脚本（对应 Android globecore cpp/CMakeLists.txt）
#
# 架构口径（1.1.0 双平台改造）：一套引擎源码，两端产物。
# 引擎源（core/globe/navigator/layer/net/vector/raster/render/geom/util/bridge）不复制不搬迁，
# 经 GLOBECORE_ENGINE_ROOT 直接引用 ../globecore/src/main/cpp；本目录只保留 ohos/ 鸿蒙专属源
# （EGL 上下文 / XComponent 桥 / NAPI 导出）。平台差异仅三处：Log.h 分派、GL 宿主、JNI↔NAPI 桥接。
# 第三方 .a 由 `build-scripts/build-all.sh --target ohos` 产出到 build-scripts/out/ohos/<abi>/，
# 三方头与引擎共用 globecore cpp/include/（install_headers 同一落位，与 .a 版本配套）。
cmake_minimum_required(VERSION 3.22.1)

project("globecore")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 符号默认隐藏（与 Android 侧同口径）：libcurl.a 内部静态打包 OpenSSL/zlib，
# 默认导出会与 libgdal 依赖链发生全局符号抢占。NAPI 注册经 constructor 属性显式导出，不受影响。
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# 引擎源根：从本模块 cpp 目录向上派生 5 级（cpp → main → src → library → globecore-harmonyos → 仓库根），
# 与 Android 模块相对位置固定，不依赖构建时 cwd。可用 -DGLOBECORE_ENGINE_ROOT 覆盖。
if(NOT DEFINED GLOBECORE_ENGINE_ROOT)
    get_filename_component(GLOBECORE_ENGINE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../globecore/src/main/cpp" ABSOLUTE)
endif()
if(NOT EXISTS "${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.h")
    message(FATAL_ERROR "GLOBECORE_ENGINE_ROOT 未指向引擎源：${GLOBECORE_ENGINE_ROOT}（检查 globecore 模块是否在同仓库）")
endif()
message(STATUS "globecore-harmonyos: GLOBECORE_ENGINE_ROOT=${GLOBECORE_ENGINE_ROOT}")

# 引擎内 #include "core/GlobeEngine.h" 等以引擎根为基准（与 Android 构建完全一致）
include_directories(${GLOBECORE_ENGINE_ROOT})
# 第三方头（curl/gdal/proj/sqlite/zlib 等），双平台共用同一份
include_directories(${GLOBECORE_ENGINE_ROOT}/include)

# 目标 ABI 变量名差异：Android NDK 用 ANDROID_ABI，OHOS toolchain 用 OHOS_ARCH
set(GLOBECORE_ABI ${OHOS_ARCH})

# ==================== 第三方静态库（与 Android 侧同名同序，目录换 ohos 产物）====================
# 目录名须与 OHOS_ARCH 一致：arm64-v8a / x86_64
if(NOT DEFINED GLOBECORE_THIRD_PARTY_DIR)
    get_filename_component(GLOBECORE_THIRD_PARTY_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../build-scripts/out/ohos" ABSOLUTE)
endif()
set(OHOS_LIBS_DIR ${GLOBECORE_THIRD_PARTY_DIR}/${GLOBECORE_ABI})

add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libcurl.a)
add_library(sqlite STATIC IMPORTED)
set_target_properties(sqlite PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libsqlite3.a)
add_library(proj STATIC IMPORTED)
set_target_properties(proj PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libproj.a)
add_library(gdal STATIC IMPORTED)
set_target_properties(gdal PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libgdal.a)
add_library(kmlbase STATIC IMPORTED)
set_target_properties(kmlbase PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlbase.a)
add_library(kmldom STATIC IMPORTED)
set_target_properties(kmldom PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmldom.a)
add_library(kmlengine STATIC IMPORTED)
set_target_properties(kmlengine PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlengine.a)
add_library(kmlxsd STATIC IMPORTED)
set_target_properties(kmlxsd PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlxsd.a)
add_library(kmlconvenience STATIC IMPORTED)
set_target_properties(kmlconvenience PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlconvenience.a)
add_library(kmlregionator STATIC IMPORTED)
set_target_properties(kmlregionator PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlregionator.a)
add_library(uriparser STATIC IMPORTED)
set_target_properties(uriparser PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/liburiparser.a)
add_library(minizip STATIC IMPORTED)
set_target_properties(minizip PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libminizip.a)
add_library(expatstatic STATIC IMPORTED)
set_target_properties(expatstatic PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libexpat.a)
add_library(zstatic STATIC IMPORTED)
set_target_properties(zstatic PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libz.a)

add_library(${CMAKE_PROJECT_NAME} SHARED
        # ── 鸿蒙专属源（承担 Android 上 GLSurfaceView + JNI_OnLoad 的职责）──
        # EGL display/surface/context 封装 + MapRenderHost 渲染线程
        ohos/EglContext.cpp
        # OH_NativeXComponent surface 回调 ↔ 渲染线程接线
        ohos/XComponentBridge.cpp
        # NAPI 模块注册（对应 JNI_OnLoad）
        ohos/napi_init.cpp
        # GlobeEngine 全导出 NAPI 版（对应 jni/GlobeEngineJni.cpp，GL 生命周期改由 attach/detach 接管）
        ohos/napi_globe_engine.cpp
        # bridge 的 NAPI 版（核心在引擎 wwbridge，此处仅编解码薄封装）
        ohos/napi_srs.cpp
        ohos/napi_gdal_info.cpp
        ohos/napi_layer_info.cpp
        ohos/napi_vector_io.cpp
        # ── 引擎源：与 Android CMakeLists 同一清单，仅去掉 jni/ 桥接 ──
        # 顶层渲染引擎 + 相机/导航器
        ${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.cpp
        ${GLOBECORE_ENGINE_ROOT}/navigator/Navigator.cpp
        # 投影与瓦片网格数学 + 3D 球体地基
        ${GLOBECORE_ENGINE_ROOT}/globe/MercatorProjection.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Wgs84Globe.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Tessellator.cpp
        # 瓦片缓存/加载器 + 矢量加载器
        ${GLOBECORE_ENGINE_ROOT}/layer/TileCache.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/TileLoader.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/VectorLoader.cpp
        # 网络取瓦片（libcurl 封装）
        ${GLOBECORE_ENGINE_ROOT}/net/HttpClient.cpp
        # GDAL 引导 + OGR 矢量读取 + KML 样式 + 几何构建
        ${GLOBECORE_ENGINE_ROOT}/vector/GdalBootstrap.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorReader.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/KmlStyle.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorBuilder.cpp
        # 栅格渲染器（GDAL 重投影即时生成瓦片 PNG）
        ${GLOBECORE_ENGINE_ROOT}/raster/RasterRenderer.cpp
        # OpenGL ES 渲染器、着色器、纹理、图片解码、字形图集
        ${GLOBECORE_ENGINE_ROOT}/render/Renderer.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ShaderProgram.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/Texture.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ImageDecoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/FontAtlas.cpp
        # 数据 IO 桥核心（PROJ/GDAL）：JNI 导出段在 __OHOS__ 下编译为空，仅余 wwbridge 核心
        ${GLOBECORE_ENGINE_ROOT}/bridge/srs.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/gdal_info.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/layer_info.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/vector_io.cpp)

# XComponent 桩生成器感知：本目标直接产出全量引擎库，无需 "世界窗口" 桩 .so
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GLOBECORE_LIBRARY_FULL)

find_library(ace-napi-lib libace_napi.z.so)
find_library(ace-ndk-lib libace_ndk.z.so)
find_library(native-window-lib libnative_window.so)
find_library(native-buffer-lib libnative_buffer.so)
find_library(hilog-lib libhilog_ndk.z.so)
find_library(egl-lib libEGL.so)
find_library(gles-lib libGLESv3.so)

# GLESv3（向下兼容 ES 2.0 绘制指令，render/ 层零改动）+ EGL：鸿蒙 GL 宿主；
# ace_napi：NAPI 运行时；ace_ndk + native_window/buffer：XComponent 与 OHNativeWindow 格式设置；
# hilog_ndk：Log.h 鸿蒙分支。GDAL 依赖链顺序参照 Android：gdal 在其依赖之前。
# curl.a 内置 zlib 与 libz.a 并存的重复符号：静态归档择一解析，hidden 可见性已隔离对外符号；
# OHOS 链接器对多重定义默认报错，追加 --allow-multiple-definition 兜底（与 Android lld 行为一致）。
target_link_libraries(${CMAKE_PROJECT_NAME}
        ${egl-lib}
        ${gles-lib}
        ${ace-napi-lib}
        ${ace-ndk-lib}
        ${native-window-lib}
        ${native-buffer-lib}
        ${hilog-lib}
        curl
        gdal
        proj
        sqlite
        kmlengine
        kmldom
        kmlbase
        kmlxsd
        kmlconvenience
        kmlregionator
        uriparser
        minizip
        expatstatic
        zstatic
        m
        pthread)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)

# libglobecore 产物名：entry/HAR 依赖 "libglobecore": "file:../library"，
# ArkTS import 'libglobecore' 解析到 libglobecore.so（nm_modname=globecore 与之对应）
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME globecore)
