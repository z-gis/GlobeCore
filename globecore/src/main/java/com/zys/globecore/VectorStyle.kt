package com.zys.globecore

/**
 * 矢量/叠加图层渲染样式值对象，参照 参考实现 `Style`（渲染层）并一一对应 native `globecore::VectorStyle`
 * （见 cpp/vector/VectorGeometry.h）的字段集：面填充、面描边、线、点、标注。
 *
 * 用于 [NativeMapView.addVectorLayer] / [NativeMapView.addOverlayLayer] —— 此前它们以十余个「散参数」
 * 传样式（颜色/线宽/点半径/标注），现收敛为本约定类传参（对齐 参考实现 用 Style 对象承载样式的口径）。
 *
 * 颜色为 #AARRGGBB 打包 Int；线宽单位像素、点半径单位 dp。默认值对齐 native VectorStyle 与原主界面
 * LocalVectorLoader（已删除；半透明蓝填充 + 白描边、橙红线/点）。[NativeMapView] 在内部把本对象拆解为
 * JNI 基元参数传入 native（值类仅在 Kotlin API 层，不跨 JNI 边界传对象）。
 *
 * 注：与文档层 `com.zys.mobilemap.doc.VectorStyle`（可空覆盖 + JSON 序列化 + 颜色字符串）不同，
 * 本类是「渲染时已解析」的完整样式，字段非空、颜色为 Int。
 */
data class VectorStyle(
    /** 面填充色（#AARRGGBB） */
    val fillColor: Int = DEFAULT_FILL_COLOR,
    /** 面描边色（#AARRGGBB） */
    val outlineColor: Int = DEFAULT_OUTLINE_COLOR,
    /** 面描边线宽（像素，屏幕空间等宽） */
    val outlineWidth: Float = 1.0f,
    /** 线要素色（#AARRGGBB） */
    val lineColor: Int = DEFAULT_LINE_COLOR,
    /** 线要素线宽（像素） */
    val lineWidth: Float = 2.0f,
    /** 点要素色（#AARRGGBB） */
    val pointColor: Int = DEFAULT_LINE_COLOR,
    /** 点要素屏幕固定半径（dp，不随缩放变化） */
    val pointRadiusDp: Float = 5.0f,
    /** 标注字段名：空则整层不标注（对齐主界面门控） */
    val labelField: String = "",
    /** 标注文字色（#AARRGGBB，默认白） */
    val labelColor: Int = 0xFFFFFFFF.toInt(),
    /** 标注字号缩放（1.0 = 基准字号） */
    val labelSize: Float = 1.0f,
    /** 标注是否描边 */
    val labelOutline: Boolean = false,
    /** 标注轮廓色（#AARRGGBB，默认黑） */
    val labelOutlineColor: Int = 0xFF000000.toInt(),
) {
    companion object {
        /** 面填充默认色：半透明蓝 (0.29,0.56,0.89,0.4) → #AARRGGBB，对齐 native VectorStyle 默认 */
        const val DEFAULT_FILL_COLOR = 0x664A8FE3.toInt()
        /** 面描边默认色：白 */
        const val DEFAULT_OUTLINE_COLOR = 0xFFFFFFFF.toInt()
        /** 线/点默认色：橙红 (0.9,0.42,0.2,1) → #AARRGGBB，对齐 native VectorStyle 默认 */
        const val DEFAULT_LINE_COLOR = 0xFFE56B33.toInt()
    }
}
