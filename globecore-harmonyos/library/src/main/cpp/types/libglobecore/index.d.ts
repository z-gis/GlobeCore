// libglobecore.so (NAPI) 类型声明 —— 与 ohos/napi_*.cpp 的导出描述符一一对应。
// 命名契约：so 名 libglobecore.so ↔ nm_modname "globecore" ↔ 本 import 名（大小写一致）。
// 类型映射（对齐 napi_util.h 口径）：
//   句柄 → number（int64，< 2^53 安全）
//   double[N] → Float64Array；ARGB 像素/计数 → Int32Array；字符串数组 → string[]
//   可空返回（对齐 JNI null）用 | null；可空入参用 undefined | null 联合。
// 内部门面（NativeLib.ets）统一做类型归一（Float64Array ↔ number[]），宿主不直接依赖本文件。

interface GlobeCore {
  // ── 生命周期 / GL 宿主 ──
  nativeCreate(): number;
  nativeDestroy(handle: number): void;
  /// surfaceId：XComponentController.getXComponentSurfaceId() 数值；width/height：surface 像素尺寸
  nativeAttach(handle: number, surfaceId: number, width: number, height: number): boolean;
  nativeDetach(handle: number): void;
  nativeRequestRender(handle: number): void;

  // ── 相机 / 视图模式 / 手势 ──
  nativeSetCamera(handle: number, latitudeDeg: number, longitudeDeg: number, altitudeMeters: number,
    headingDeg: number, tiltDeg: number, rollDeg: number, fieldOfViewDeg: number, altitudeMode: number): void;
  /// [lat, lon, alt, heading, tilt, roll, fov, altMode]
  nativeGetCamera(handle: number): Float64Array | null;
  nativeSetViewMode(handle: number, mode: number): void;
  nativeGetViewMode(handle: number): number;
  nativePanBy(handle: number, dxPx: number, dyPx: number): void;
  nativeZoomBy(handle: number, factor: number, focusXpx: number, focusYpx: number): void;
  nativeRotateHeading(handle: number, deltaDeg: number): void;
  nativeRotateTilt(handle: number, deltaDeg: number): void;

  // ── 瓦片 / 栅格图层 ──
  nativeAddTileLayer(handle: number, cacheDir: string, urlTemplate: string, maxLevel: number, overlay: boolean): void;
  nativeSetLayerVisible(handle: number, index: number, visible: boolean): void;
  nativeAddRasterLayer(handle: number, cacheDir: string, path: string): number;

  // ── 矢量图层 ──
  nativeAddVectorLayer(handle: number, path: string,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    labelField: string, labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    iconArgb: Int32Array | undefined | null, iconW: number, iconH: number,
    hasExtent: boolean, minLon: number, minLat: number, maxLon: number, maxLat: number,
    maxFeatures: number): number;
  nativeUpdateVectorExtent(handle: number, index: number, hasExtent: boolean,
    minLon: number, minLat: number, maxLon: number, maxLat: number, maxFeatures: number): void;
  nativeHasVectorLoading(handle: number): boolean;
  nativeSetVectorLayerVisible(handle: number, index: number, visible: boolean): void;
  nativeSetVectorMinLevel(handle: number, index: number, minLevel: number): void;
  nativeGetCameraZoomLevel(handle: number): number;
  nativeRemoveVectorLayer(handle: number, index: number): void;

  // ── 动态叠加层 ──
  nativeAddOverlayLayer(handle: number,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    iconArgb: Int32Array | undefined | null, iconW: number, iconH: number): number;
  nativeUpdateOverlayPoints(handle: number, index: number, lonlat: Float64Array,
    fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeUpdateOverlayLines(handle: number, index: number, lonlat: Float64Array,
    vertexCounts: Float64Array, fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeUpdateOverlayPolygons(handle: number, index: number, lonlat: Float64Array,
    ringVertexCounts: Float64Array, ringsPerFeature: Float64Array,
    fids: Float64Array | undefined | null, labels: string[] | undefined | null): void;
  nativeRemoveOverlayLayer(handle: number, index: number): void;
  nativeSetOverlayNoPick(handle: number, index: number, noPick: boolean): void;
  nativeClearOverlayLayers(handle: number): void;

  // ── 拾取 / 反算 / 要素几何 ──
  /// [layerIndex, fid]，未命中 null
  nativePickVector(handle: number, sxPx: number, syPx: number): Float64Array | null;
  /// 单击拾取回调（事件源 native 手势识别，坐标 vp）；传 null 反注册
  nativeSetTapCallback(handle: number, callback: ((x: number, y: number) => void) | null): void;
  /// [lon, lat]，视口未就绪 null
  nativeScreenToGeo(handle: number, sxPx: number, syPx: number): Float64Array | null;
  /// [ [type], ringCounts, ringsPerFeature, lonlat ]，未命中 null
  nativeFeatureGeometry(handle: number, layerIndex: number, fid: number): Float64Array[] | null;

  // ── 显示 / 标记 / 字体 ──
  nativeSetDisplayDensity(handle: number, density: number): void;
  nativeSetLocationMarker(handle: number, lonDeg: number, latDeg: number, visible: boolean, headingDeg: number): void;
  nativeSetLocationMarkerIcon(handle: number, iconArgb: Int32Array, iconW: number, iconH: number): void;
  nativeSetFontPath(handle: number, path: string): void;

  // ── 网络 / HTTPS 证书（全局，无句柄）──
  /// 设置 CA 证书包（cacert.pem）绝对路径供瓦片 HTTPS 校验；空串回退引擎默认（Android 系统目录）
  nativeSetCaBundle(path: string): void;

  // ── PROJ 坐标转换（bridge/srs）──
  nativeGetProjVersion(): number;
  nativeInitProjDataPath(path: string): void;
  /// [x, y]（目标坐标系），失败 null
  nativeConvert(x: number, y: number, srcCrs: string, tgtCrs: string): Float64Array | null;

  // ── GDAL / 图层元信息（bridge/gdal_info、layer_info）──
  nativeGetGdalVersion(): string;
  nativeGetVectorDrivers(): string;
  /// [minLon, minLat, maxLon, maxLat]，失败 null
  nativeGetLayerExtent(path: string): Float64Array | null;
  nativeGetVectorFieldNames(path: string): string[];
  /// "名称\nproj4定义"，无 SRS null
  nativeGetLayerSrs(path: string): string | null;

  // ── 矢量要素 IO（bridge/vector_io，JSON 传输）──
  nativeReadVectorFeatures(path: string, minLon: number, minLat: number, maxLon: number, maxLat: number,
    includeAllFields: boolean, labelField: string, simplifyGeometry: boolean, simplifyTolerance: number): string | null;
  nativeQueryVectorFeatures(path: string, sql: string): string | null;
  nativeCountVectorFeatures(path: string): number;
  nativeUpdateFeatureAttributes(path: string, featureId: number, keys: string[], values: string[]): boolean;
  nativeGetFeatureAttributes(path: string, featureId: number): string | null;
}

const globeCore: GlobeCore;
export default globeCore;
