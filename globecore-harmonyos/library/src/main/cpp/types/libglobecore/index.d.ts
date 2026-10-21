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
  /// 移除瓦片图源图层（含栅格层，墓碑；index 为 addTileLayer/addRasterLayer 加入次序）
  nativeRemoveTileLayer(handle: number, index: number): void;
  /// 最近一次 nativeAddRasterLayer 失败原因（中文）；成功或从未失败 null
  nativeGetRasterAddError(handle: number): string | null;
  /// 全幅加载栅格层（后台整幅直读，独立索引空间）；返回新图层索引，无效 -1
  nativeAddRasterImageLayer(handle: number, cacheDir: string, path: string): number;
  nativeSetRasterImageVisible(handle: number, index: number, visible: boolean): void;
  /// 可见全幅栅格层是否仍有未完成的加载/纹理上传（纯查询不触发重绘）
  nativeHasRasterLoading(handle: number): boolean;
  /// 指定全幅栅格层的加载失败原因（中文）；未失败/越界/已成功 null
  nativeGetRasterImageError(handle: number, index: number): string | null;

  // ── 矢量图层 ──
  nativeAddVectorLayer(handle: number, path: string,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    fillExplicit: boolean, lineExplicit: boolean,
    labelField: string, labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    featureStyleOverride: string,
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
  /// 长按拾取回调（单指保持按住未越过拖动阈值达长按时长，坐标 vp）；传 null 反注册
  nativeSetLongPressCallback(handle: number, callback: ((x: number, y: number) => void) | null): void;
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

  // ── GeoPackage 工程数据读写（bridge/geopackage_core，JSON 与 Android 侧逐字节同构）──
  /// 读工程：返回与 Android Project.fromJson 兼容的 JSON；打不开/结构不符 null
  nativeReadGeoPackage(path: string): string | null;
  /// 整写工程（新建 + 老库 schema 升级共用；内部 tmp+rename 原子替换）
  nativeWriteGeoPackage(path: string, json: string): boolean;
  /// 业务七表与增量写所需列是否齐备（文件不存在 true，由新建流程负责建库）
  nativeGpkgSchemaCurrent(path: string): boolean;
  /// 写前准备（幂等）：补建附件 BLOB 附加表 photo_att_data
  nativeGpkgPrepareSchema(path: string): boolean;

  // ── GeoPackage 增量写（键与 Android 工程 JSON 逐字一致；id<=0 自动编号）──
  /// json: {"id","name","date","color","hidden","lineWidth","points":[[lon,lat,alt],...]}；返回 fid，失败 -1
  nativeGpkgInsertTrack(path: string, json: string): number;
  /// json: {"name","color","hidden","lineWidth"}（几何不动）
  nativeGpkgUpdateTrack(path: string, id: number, json: string): boolean;
  nativeGpkgDeleteTrack(path: string, id: number): boolean;
  /// json: {"id","name","date","type","result","color","hidden","closed","lineWidth","points"}
  /// type=point→点，area/polygon→面，其余→线；返回 fid，失败 -1
  nativeGpkgInsertMeasure(path: string, json: string): number;
  /// json: {"color","lineWidth"}（对齐 Android updateMeasureStyle）
  nativeGpkgUpdateMeasure(path: string, id: number, json: string): boolean;
  nativeGpkgDeleteMeasure(path: string, id: number): boolean;
  /// json: {"id","name","lon","lat","alt","timestamp","visible","groupId"}；返回 fid，失败 -1
  nativeGpkgInsertPhoto(path: string, json: string): number;
  /// json 键出现才更新：{"name"?,"groupId"?,"visible"?,"attachments"?,"photo_path"?}
  /// attachments 可传 string（原样存列）或数组（序列化存列）；空串写 NULL
  nativeGpkgUpdatePhoto(path: string, id: number, json: string): boolean;
  /// 删除照片（连同 photo_att_data 附件行）
  nativeGpkgDeletePhoto(path: string, id: number): boolean;
  /// 写照片 BLOB：idx=0 主照片 photos.photo_data；idx>=1 附件 photo_att_data（INSERT OR REPLACE）
  nativeGpkgAddPhotoAttachment(path: string, fid: number, idx: number, data: ArrayBuffer): boolean;
  /// 读照片 BLOB：idx=0 主照片，idx>=1 附件；无记录 null
  nativeGpkgReadPhotoBlob(path: string, fid: number, idx: number): ArrayBuffer | null;
  /// json: {"id","name","createdTime","visible"}（fid 即 id）
  nativeGpkgInsertGroup(path: string, json: string): boolean;
  /// json: {"id","name"}
  nativeGpkgUpdateGroup(path: string, json: string): boolean;
  /// 删除分组：组内照片移到未分组（group_id=-1）
  nativeGpkgDeleteGroup(path: string, id: number): boolean;
  /// 分组内照片批量显隐
  nativeGpkgSetGroupPhotosVisible(path: string, id: number, visible: boolean): boolean;
  /// 样地 upsert（fid 冲突替换）：{"id","name","plotType","attributes":object|string,"points"}
  nativeGpkgUpsertSurveyPlot(path: string, json: string): boolean;
  /// 删除样地（按 fid DeleteFeature）
  nativeGpkgDeleteSurveyPlot(path: string, id: number): boolean;
  /// 元数据一把梭：project_meta 五键 upsert（空值删行）+ layer_refs 全替换 + finish 元数据。
  /// json: {"name"?,"description"?,"srs"?,"createTime"?,"updateTime"?,"layerRefs"?:[string,...]}
  nativeGpkgSyncMeta(path: string, json: string): boolean;
  /// 照片文件清扫（幂等）：BLOB 已在库删磁盘原件；BLOB 缺失且原件存在则读入库后删。
  /// 仅处理 mediaRoot 目录下文件；返回是否发生任何清扫
  nativeGpkgSweepPhotoFiles(path: string, mediaRoot: string): boolean;
}

const globeCore: GlobeCore;
export default globeCore;
