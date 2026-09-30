package com.zys.globecore

/**
 * 相机姿态值对象，参照 参考实现 Camera 建模：
 * 位置（lat/lon/altitude）+
 * 姿态 （heading/tilt/roll）+
 * 视场角（fieldOfView）+
 * 高度模式（altitudeMode）。
 *
 * 单位口径对齐本模块既有约定：角度用「度」、高度用「米」（区别于 参考实现的 [Angle]/[Position] 包装对象）。
 * 与 native `globecore::Camera`（geom/Camera.h）字段一一对应，经
 * [NativeLib.nativeSetCameraPose]/[NativeLib.nativeGetCameraPose] 以标量数组跨边界读写。
 *
 * 说明：2D 墨卡托为正交恒正北视图，渲染仅消费 latitude/longitude/altitude/fieldOfView
 * （altitude 驱动缩放级别、fieldOfView 参与 altitude↔zoom 换算）；heading/tilt 由 3D 球体透视通路消费
 * （屏幕上方向/视线），roll/altitudeMode 为对齐 参考实现的建模字段，先随 [Camera] 读写留存。
 */
data class Camera(
    val latitude: Double = 0.0,
    val longitude: Double = 0.0,
    val altitude: Double = 0.0,
    val heading: Double = 0.0,
    val tilt: Double = 0.0,
    val roll: Double = 0.0,
    val fieldOfView: Double = DEFAULT_FIELD_OF_VIEW_DEG,
    val altitudeMode: AltitudeMode = AltitudeMode.ABSOLUTE,
) {
    /**
     * 高度模式，对应 参考实现 `geom.AltitudeMode`。ordinal 与 native `globecore::AltitudeMode` 严格一致，
     * 供 JNI 以 int 透传，勿调整顺序。
     */
    enum class AltitudeMode {
        ABSOLUTE,           // 相对椭球面（HAE）
        ABOVE_SEA_LEVEL,    // 相对海平面（ASL）
        CLAMP_TO_GROUND,    // 贴地，忽略 altitude
        RELATIVE_TO_GROUND, // 相对地面（AGL）
    }

    companion object {
        /** 默认垂直视场角（度），对齐 参考实现 Camera.fieldOfView 与 native Navigator.FIELD_OF_VIEW_DEG */
        const val DEFAULT_FIELD_OF_VIEW_DEG = 45.0
    }
}
