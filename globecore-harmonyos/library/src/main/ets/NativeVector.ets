// NativeVector.ets —— 矢量数据 IO 门面（对应 Android NativeVector.kt，
// 实现见 engine bridge/vector_io.cpp wwbridge 核心 + ohos/napi_vector_io.cpp NAPI 导出）。
//
// 返回值统一为 JSON 字符串（native 与 ArkTS 间以 JSON 传递，要素数上限在 C++ 层控制防 OOM）。
// 宿主侧 JSON 解析与业务包装在 app 层（对齐 Android GdalVectorReader）。
import ww from 'libglobecore.so';

export class NativeVector {

  /**
   * 读取要素列表：extent 四分量任一为 NaN 时读全部，否则按 WGS84 范围空间过滤
   * （NaN 归一在 NAPI 层完成，口径同 JNI wrapper）。
   * includeAllFields=false 时 C++ 仅保留 labelField 白名单字段（延迟属性读取模式）；
   * simplifyGeometry=true 时重投影后经 OGRGeometry::Simplify(tolerance) 简化顶点。
   */
  static readVectorFeatures(path: string, minLon: number, minLat: number,
    maxLon: number, maxLat: number, includeAllFields: boolean, labelField: string,
    simplifyGeometry: boolean, simplifyTolerance: number): string | null {
    return ww.nativeReadVectorFeatures(path, minLon, minLat, maxLon, maxLat,
      includeAllFields, labelField, simplifyGeometry, simplifyTolerance);
  }

  /** 按 FID 单要素回取全字段（延迟属性读取模式配套），未找到 null */
  static getFeatureAttributes(path: string, featureId: number): string | null {
    return ww.nativeGetFeatureAttributes(path, featureId);
  }

  /** 执行属性 SQL 查询，返回命中要素 JSON（语句错误含 {"error":"…"}） */
  static queryVectorFeatures(path: string, sql: string): string | null {
    return ww.nativeQueryVectorFeatures(path, sql);
  }

  /** 快速统计要素总数（遍历全部子图层求和），打开失败 -1 */
  static countVectorFeatures(path: string): number {
    return ww.nativeCountVectorFeatures(path);
  }

  /** 按 FID 就地写回属性字段到源文件；只读驱动/字段不存在 false */
  static updateFeatureAttributes(path: string, featureId: number,
    keys: string[], values: string[]): boolean {
    return ww.nativeUpdateFeatureAttributes(path, featureId, keys, values);
  }
}
