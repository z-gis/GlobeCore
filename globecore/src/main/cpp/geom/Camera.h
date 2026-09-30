#ifndef GLOBECORE_GEOM_CAMERA_H
#define GLOBECORE_GEOM_CAMERA_H

namespace globecore {

/**
 * 高度模式，对应 参考实现 geom.AltitudeMode 的四值（顺序与 Kotlin 侧枚举 ordinal 严格一致，供 JNI 以 int 透传）。
 * 说明：当前 2D 墨卡托渲染不解释高度模式，字段先行建模、留待相机模型统一时接入。
 */
enum class AltitudeMode {
    ABSOLUTE = 0,           ///< 相对椭球面（HAE），对应 参考实现 ABSOLUTE
    ABOVE_SEA_LEVEL = 1,    ///< 相对海平面（ASL），对应 参考实现 ABOVE_SEA_LEVEL
    CLAMP_TO_GROUND = 2,    ///< 贴地，忽略 altitude，对应 参考实现 CLAMP_TO_GROUND
    RELATIVE_TO_GROUND = 3, ///< 相对地面（AGL），对应 参考实现 RELATIVE_TO_GROUND
};

/**
 * 相机姿态值对象，对应 参考实现 geom.Camera（其 Position 的 latitude/longitude/altitude 展开为标量成员）。
 *
 * 单位口径沿用本模块 native 既有约定：角度用「度」、高度用「米」，不引入 参考实现的 Angle/Position 包装对象
 * （见 Navigator 以 double 表示相机中心经纬度与相机高度）。默认值对齐 参考实现 Camera（姿态归零、fov=45°、ABSOLUTE）。
 *
 * 当前 2D 墨卡托渲染仅消费 latitude/longitude/altitude（altitude 经 zoom 口径驱动缩放）；
 * heading/tilt/roll/altitudeMode 为对齐 参考实现的姿态建模占位值，渲染接入留待相机模型统一（见迁移文档待优化项）。
 * fieldOfView 与 Navigator 的 altitude↔zoom 换算联动（默认 45 保持既有显示范围不变）。
 */
struct Camera {
    double latitude = 0.0;       ///< 纬度（度）
    double longitude = 0.0;      ///< 经度（度）
    double altitude = 0.0;       ///< 相机高度（米，俯视时眼到地面的距离）
    double heading = 0.0;        ///< 朝向角（度，顺时针自北），对应 参考实现 heading
    double tilt = 0.0;           ///< 倾斜角（度），对应 参考实现 tilt（即 pitch）
    double roll = 0.0;           ///< 翻滚角（度），对应 参考实现 roll
    double fieldOfView = 45.0;   ///< 垂直视场角（度），默认 45 对齐 参考实现 Camera.fieldOfView
    AltitudeMode altitudeMode = AltitudeMode::ABSOLUTE; ///< 高度解释模式
};

} // namespace globecore

#endif // GLOBECORE_GEOM_CAMERA_H
