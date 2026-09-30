package com.zys.globecore

/**
 * 图层元信息门面：矢量/栅格文件的四至、坐标系、字段名与 GDAL 版本/驱动清单，
 * 符号实现见 cpp/bridge/layer_info.cpp 与 gdal_info.cpp（`Java_com_zys_globecore_NativeLayerInfo_*`）。
 *
 * 依赖方向约定：globecore 先于 app 存在，JNI 符号只挂本模块类名；
 * app 侧（图层管理对话框、主界面缩放到图层、关于页）经本门面委托调用。
 */
object NativeLayerInfo {

    init {
        // 加载失败仅记录：后续 external 调用抛 UnsatisfiedLinkError，由调用方各自兜底（与宿主原行为一致）
        try {
            System.loadLibrary("globecore")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e("NativeLayerInfo", "Failed to load native library 'globecore'", e)
        }
    }

    /** 图层四至 [minLon, minLat, maxLon, maxLat]（矢量优先，栅格兜底，非 WGS84 自动重投影）；失败返回 null */
    external fun getLayerExtent(path: String): DoubleArray?

    /** 图层坐标系描述文本（EPSG/PROJ 文本）；无法解析返回 null */
    external fun getLayerSrs(path: String): String?

    /** 矢量数据字段名清单（供 SQL 查询字段列表）；打开失败返回 null */
    external fun getVectorFieldNames(path: String): Array<String>?

    /** GDAL 版本字符串（如 "GDAL 3.9"），供关于页显示 */
    external fun getGdalVersion(): String?

    /** 已注册的矢量驱动清单文本（供关于页显示） */
    external fun getVectorDrivers(): String?
}
