// NativeLayerInfo.ets —— 图层元信息门面（对应 Android NativeLayerInfo.kt，
// 实现见 engine bridge/layer_info.cpp + gdal_info.cpp 核心、ohos/napi_layer_info.cpp NAPI 导出）。
import gc from 'libglobecore.so';

export class NativeLayerInfo {

  /** 图层四至 [minLon, minLat, maxLon, maxLat]（矢量优先，栅格兜底，非 WGS84 自动重投影）；失败 null */
  static getLayerExtent(path: string): number[] | null {
    return gc.nativeGetLayerExtent(path);
  }

  /** 图层坐标系描述文本（"名称\nproj4定义"）；无法解析 null */
  static getLayerSrs(path: string): string | null {
    return gc.nativeGetLayerSrs(path);
  }

  /** 矢量数据字段名清单（供 SQL 查询字段列表）；打开失败空数组 */
  static getVectorFieldNames(path: string): string[] {
    return gc.nativeGetVectorFieldNames(path);
  }

  /** GDAL 版本字符串（如 "30700"），供关于页显示 */
  static getGdalVersion(): string {
    return gc.nativeGetGdalVersion();
  }

  /** 已注册的矢量驱动清单文本（逐行 "NAME（读/写|只读）"，供关于页显示） */
  static getVectorDrivers(): string {
    return gc.nativeGetVectorDrivers();
  }
}
