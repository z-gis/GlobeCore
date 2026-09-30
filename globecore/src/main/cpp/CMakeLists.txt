# globecore —— 独立实现的 c++ 渲染引擎（参照 参考实现 行为设计，非其源码移植）的 native 构建脚本
# GL 上下文由 Kotlin 侧 GLSurfaceView 创建并绑定，native 仅调用 OpenGL ES 2.0 绘制指令。
cmake_minimum_required(VERSION 3.22.1)

project("globecore")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 符号默认隐藏：libcurl.a 内部静态打包了 OpenSSL/zlib 实现，若默认导出会与同进程内
# 静态链接的 libz/gdal 等发生全局符号抢占。隐藏后仅 JNIEXPORT（visibility=default）的
# Java_* 桥接函数对外可见，避免冲突。
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# 头文件按 ${CMAKE_CURRENT_SOURCE_DIR} 为根引用（如 #include "core/GlobeEngine.h"），
# 目录分层对应 参考实现 结构：core / render / geom / globe / navigator / layer / net / util / jni / vector / raster
# bridge/ 为数据 IO 桥接（自 app 侧迁入），以 #include "gdal/xxx.h" 形式引用与本模块自有代码同源的 GDAL 头
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# 第三方头（curl/openssl/zlib/gdal/proj/sqlite）位于 include/，以 #include <curl/curl.h> 形式引用
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# 预编译 libcurl 静态库（内部已合并 OpenSSL + zlib，无需再单独链接 ssl/crypto/z）。
# 目录名须与 ANDROID_ABI 一致：arm64-v8a / x86_64。
add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/jniLibs/${ANDROID_ABI}/libcurl.a)

# ==================== GDAL/OGR 依赖链（native 侧直接读矢量，阶段三）====================
# globecore 自链 GDAL/OGR，在 C++ 里直接 GDALOpenEx 读矢量并重投影，不经 Kotlin 传顶点数组。
# 【迁移终态】所有静态库（libgdal/libproj/libsqlite3/libcurl/libz/libexpat/libminizip/liburiparser
# 及全套 kml*）已复制进本模块 jniLibs/${ANDROID_ABI}，模块完全独立可移植，不再相对引用 app 侧 .a。
# 目录名须与 ANDROID_ABI 一致：arm64-v8a / x86_64。头文件（gdal/proj/sqlite）位于 include/。
set(JNILIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/jniLibs/${ANDROID_ABI})

add_library(sqlite STATIC IMPORTED)
set_target_properties(sqlite PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libsqlite3.a)
add_library(proj STATIC IMPORTED)
set_target_properties(proj PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libproj.a)
add_library(gdal STATIC IMPORTED)
set_target_properties(gdal PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libgdal.a)
add_library(kmlbase STATIC IMPORTED)
set_target_properties(kmlbase PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmlbase.a)
add_library(kmldom STATIC IMPORTED)
set_target_properties(kmldom PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmldom.a)
add_library(kmlengine STATIC IMPORTED)
set_target_properties(kmlengine PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmlengine.a)
add_library(kmlxsd STATIC IMPORTED)
set_target_properties(kmlxsd PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmlxsd.a)
add_library(kmlconvenience STATIC IMPORTED)
set_target_properties(kmlconvenience PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmlconvenience.a)
add_library(kmlregionator STATIC IMPORTED)
set_target_properties(kmlregionator PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libkmlregionator.a)
add_library(uriparser STATIC IMPORTED)
set_target_properties(uriparser PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/liburiparser.a)
add_library(minizip STATIC IMPORTED)
set_target_properties(minizip PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libminizip.a)
add_library(expatstatic STATIC IMPORTED)
set_target_properties(expatstatic PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libexpat.a)
add_library(zstatic STATIC IMPORTED)
set_target_properties(zstatic PROPERTIES IMPORTED_LOCATION ${JNILIBS_DIR}/libz.a)

add_library(${CMAKE_PROJECT_NAME} SHARED
        # JNI 桥接：Java_com_zys_globecore_NativeLib_* 符号
        jni/GlobeEngineJni.cpp
        # 顶层渲染引擎（合并参考实现中视图窗口与渲染引擎两层职责）
        core/GlobeEngine.cpp
        # 相机/导航器（对应 参考实现的 Navigator）
        navigator/Navigator.cpp
        # 投影与瓦片网格数学（对应 参考实现 globe 投影层）
        globe/MercatorProjection.cpp
        # 3D 球体地基（P1）：WGS84 椭球地理↔笛卡尔/射线求交 + quadtree 可见瓦片细分
        globe/Wgs84Globe.cpp
        globe/Tessellator.cpp
        # 瓦片磁盘缓存读取（对应 参考实现 layer.cache 的 TileStore 只读部分）
        layer/TileCache.cpp
        # 后台瓦片加载器（线程池联网取瓦片 + 写缓存 + 交付 GL 上传，Phase C）
        layer/TileLoader.cpp
        # 后台矢量加载器（阶段三：读文件 + 投影 + earcut 三角剖分，交付 GL 上传）
        layer/VectorLoader.cpp
        # 网络取瓦片（libcurl 封装，Phase C）
        net/HttpClient.cpp
        # GDAL/OGR 全进程一次引导（阶段三：native 直接读矢量的链接锚点 + 驱动注册）
        vector/GdalBootstrap.cpp
        # native OGR 矢量读取器（阶段三：直接读文件 + 重投影到 WGS84 + 摊平几何）
        vector/VectorReader.cpp
        # KML 样式自建解析（Phase 3 逐要素配色）：GDAL/LIBKML 不返回颜色，定向扫描 <Style>/<StyleMap>/<Placemark> 补 name→颜色映射
        vector/KmlStyle.cpp
        # 矢量几何构建器（WGS84 几何 → RTC 顶点/earcut 三角化/miter 描边/拾取图元；逐要素并行分片）
        vector/VectorBuilder.cpp
        # native 栅格瓦片渲染器（栅格下沉：GDAL 重投影按墨卡托瓦片四至即时生成 PNG，供 TileLoader 消费）
        raster/RasterRenderer.cpp
        # OpenGL ES 渲染器、着色器、纹理与图片解码（对应 参考实现 渲染核心）
        render/Renderer.cpp
        render/ShaderProgram.cpp
        render/Texture.cpp
        render/ImageDecoder.cpp
        # 动态字形图集（stb_truetype）：矢量要素标注文本的屏幕空间 billboard 渲染基础
        render/FontAtlas.cpp
        # 数据 IO 桥接（自 app 侧迁入）：符号只挂本模块 Kotlin 门面类（NativeSrs/NativeLayerInfo/NativeVector），
        # app 依赖本模块而非反向；PROJ 坐标转换、GDAL 版本/驱动、图层四至/坐标系/字段名、矢量读写与 SQL 查询
        bridge/srs.cpp
        bridge/gdal_info.cpp
        bridge/layer_info.cpp
        bridge/vector_io.cpp)

find_library(log-lib log)

# GLESv2：GL 绘制；android/log：系统；curl：HTTP(S) 取瓦片（含内置 OpenSSL/zlib）；
# GDAL/OGR 依赖链：native 侧直接读矢量（阶段三）。链接顺序参照 app：gdal 在其依赖
# （proj/sqlite/kml*/uriparser/minizip/expat/z）之前。curl.a 内置 zlib 与 libz.a 并存时，
# 静态归档的重复符号由链接器择一解析，不报错（hidden 可见性已隔离对外符号）。
target_link_libraries(${CMAKE_PROJECT_NAME}
        GLESv2
        android
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
        ${log-lib})
