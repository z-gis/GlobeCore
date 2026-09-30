package com.zys.globecore

/**
 * 矢量数据 IO 门面：要素读取、属性回取/写回与 SQL 查询（GDAL/OGR 直接读文件），
 * 符号实现见 cpp/bridge/vector_io.cpp（`Java_com_zys_globecore_NativeVector_*`）。
 *
 * 依赖方向约定：globecore 先于 app 存在，JNI 符号只挂本模块类名；
 * app 侧 GdalVectorReader 负责 JSON 解析与业务包装，经本门面委托调用。
 *
 * 返回值统一为 JSON 字符串（native 与 Kotlin 间以 JSON 传递，要素数上限在 C++ 层控制防 OOM）。
 */
object NativeVector {

    init {
        // 加载失败仅记录：后续 external 调用抛 UnsatisfiedLinkError，由调用方各自兜底（与宿主原行为一致）
        try {
            System.loadLibrary("globecore")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e("NativeVector", "Failed to load native library 'globecore'", e)
        }
    }

    /**
     * 读取要素列表：extent 四分量任一为 NaN 时读全部，否则按 WGS84 范围空间过滤。
     * includeAllFields=false 时 C++ 仅保留 labelField 白名单字段（延迟属性读取模式）；
     * simplifyGeometry=true 时重投影后经 OGRGeometry::Simplify(tolerance) 简化顶点。
     */
    external fun readVectorFeatures(
        path: String, minLon: Double, minLat: Double, maxLon: Double, maxLat: Double,
        includeAllFields: Boolean, labelField: String?,
        simplifyGeometry: Boolean, simplifyTolerance: Double
    ): String?

    /** 按 FID 单要素回取全字段（延迟属性读取模式配套），未找到返回 null */
    external fun getFeatureAttributes(path: String, featureId: Long): String?

    /** 执行属性 SQL 查询，返回命中要素 JSON（语句错误含 {"error":"…"}） */
    external fun queryVectorFeatures(path: String, sql: String): String?

    /** 快速统计要素总数（遍历全部子图层求和），打开失败返回 -1 */
    external fun countVectorFeatures(path: String): Int

    /** 按 FID 就地写回属性字段到源文件；只读驱动/字段不存在返回 false */
    external fun updateFeatureAttributes(
        path: String, featureId: Long, keys: Array<String>, values: Array<String>
    ): Boolean
}
