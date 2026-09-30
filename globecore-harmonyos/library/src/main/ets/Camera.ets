// Camera.ets —— 相机姿态值对象（对应 Android com.zys.globecore.Camera.kt）。
//
// 位置（lat/lon/altitude）+ 姿态（heading/tilt/roll）+ 视场角 + 高度模式，
// 与 native globecore::Camera（geom/Camera.h）字段一一对应，经 NativeLib 以标量数组读写。
// 2D 正交仅消费 lat/lon/altitude/fieldOfView；heading/tilt 由 3D 透视通路消费。

/// 高度模式：枚举值与 native globecore::AltitudeMode 严格一致（勿调整顺序与取值）
export enum AltitudeMode {
  ABSOLUTE = 0,           // 相对椭球面（HAE）
  ABOVE_SEA_LEVEL = 1,    // 相对海平面（ASL）
  CLAMP_TO_GROUND = 2,    // 贴地，忽略 altitude
  RELATIVE_TO_GROUND = 3, // 相对地面（AGL）
}

export class Camera {
  static readonly DEFAULT_FIELD_OF_VIEW_DEG: number = 45.0;

  latitude: number;
  longitude: number;
  altitude: number;
  heading: number;
  tilt: number;
  roll: number;
  fieldOfView: number;
  altitudeMode: AltitudeMode;

  constructor(latitude: number = 0.0, longitude: number = 0.0, altitude: number = 0.0,
    heading: number = 0.0, tilt: number = 0.0, roll: number = 0.0,
    fieldOfView: number = Camera.DEFAULT_FIELD_OF_VIEW_DEG,
    altitudeMode: AltitudeMode = AltitudeMode.ABSOLUTE) {
    this.latitude = latitude;
    this.longitude = longitude;
    this.altitude = altitude;
    this.heading = heading;
    this.tilt = tilt;
    this.roll = roll;
    this.fieldOfView = fieldOfView;
    this.altitudeMode = altitudeMode;
  }
}
