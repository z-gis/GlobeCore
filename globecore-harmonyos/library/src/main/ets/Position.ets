// Position.ets —— 地理位置值对象（对应 Android com.zys.globecore.Position.kt）。
//
// 单位口径：经纬度「度」（WGS84）、高度「米」。用于 setLocationMarker 入参
// 与 screenToGeo 返回（altitude 默认 0，字段先行建模）。
export class Position {
  latitude: number;
  longitude: number;
  altitude: number;

  constructor(latitude: number, longitude: number, altitude: number = 0.0) {
    this.latitude = latitude;
    this.longitude = longitude;
    this.altitude = altitude;
  }
}
