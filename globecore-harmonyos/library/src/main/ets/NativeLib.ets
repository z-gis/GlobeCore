// NativeLib.ets —— libglobecore.so NAPI 桥接门面（对应 Android NativeLib.kt + NativeSrs/NativeLayerInfo/NativeVector）。
//
// 采用与 Android 一致的「native 句柄」模型：nativeCreate 返回 C++ GlobeEngine 实例地址（number），
// 后续调用透传，避免全局单例、支持多实例。
// 与 Kotlin 版差异（1.1.0 双平台口径）：
//  - GL 生命周期不再透传：surfaceCreated/Changed/drawFrame/releaseGl 由 nativeAttach/nativeDetach
//    + XComponent EGL 渲染线程接管（Android 上由 GLSurfaceView 宿主驱动）；
//  - nativeSetRenderCallback（反射 requestRender）→ nativeRequestRender(handle) 显式刷帧，
//    引擎侧瓦片到位回调已在 MapRenderHost 构造时内部接线（脏标记唤醒渲染线程）；
//  - 数组类型：double[] ↔ Float64Array、ARGB 像素 ↔ Int32Array（napi_util.h 口径），
//    本门面统一做 number[] ↔ TypedArray 归一，宿主只见普通数组；
//  - bridge 三组符号（PROJ/GDAL 元信息/矢量 IO）并入同一模块（Android 分为三个 JNI 门面类）。
import gc from 'libglobecore.so';
import common from '@ohos.app.ability.common';
import fs from '@ohos.file.fs';

const TAG: string = 'globecore';

/// 颜色/像素等 #AARRGGBB 大值（>2^31-1）折算 int32 位模式（napi_get_value_int32 截断语义同 Kotlin Int）
function toInt32(v: number): number {
  return v | 0;
}

function toFloat64Array(v: number[] | undefined | null): Float64Array | null {
  if (v == null) {
    return null;
  }
  return Float64Array.from(v);
}

function toInt32Array(v: number[] | undefined | null): Int32Array | null {
  if (v == null) {
    return null;
  }
  const out = new Int32Array(v.length);
  for (let i = 0; i < v.length; i++) {
    out[i] = toInt32(v[i]);
  }
  return out;
}

function toIntArray(a: Float64Array | null): number[] | null {
  if (a == null) {
    return null;
  }
  return Array.from(a);
}

/**
 * NAPI 桥接入口（模块内部使用；宿主经 NativeMapView / NativeSrs / NativeLayerInfo / NativeVector
 * 门面访问，不直接依赖本文件与 libglobecore.so 类型声明）。
 */
export class NativeLib {
  private static loaded: boolean = false;
  private static loadFailed: boolean = false;

  /** 惰性加载 so；失败仅记录（对齐 Kotlin System.loadLibrary try/catch 口径），后续调用空转 */
  static ensureLoaded(): boolean {
    if (NativeLib.loaded) {
      return true;
    }
    if (NativeLib.loadFailed) {
      return false;
    }
    try {
      // import 顶层绑定即触发 dlopen + napi_module_register；此处触碰一次验证可用
      const ok: boolean = typeof gc.nativeCreate === 'function';
      if (!ok) {
        NativeLib.loadFailed = true;
        console.error(`${TAG}: libglobecore.so nativeCreate 缺失`);
        return false;
      }
      NativeLib.loaded = true;
      return true;
    } catch (e) {
      NativeLib.loadFailed = true;
      console.error(`${TAG}: Failed to load native library 'libglobecore.so': ${e}`);
      return false;
    }
  }

  // ── 生命周期 / GL 宿主 ──

  /** 创建 native GlobeEngine 实例，返回句柄（C++ 对象地址）；失败返回 0 */
  static nativeCreate(): number {
    if (!NativeLib.ensureLoaded()) {
      return 0;
    }
    return gc.nativeCreate();
  }

  /** 销毁 native GlobeEngine 实例（未 detach 的桥由 native 侧安全网先行拆除） */
  static nativeDestroy(handle: number): void {
    if (handle !== 0) {
      gc.nativeDestroy(handle);
    }
  }

  /** 绑定 XComponent surface：用 surfaceId 直建 OHNativeWindow 起渲染线程（不依赖 OnSurfaceCreated） */
  static nativeAttach(handle: number, surfaceId: number, width: number, height: number): boolean {
    if (handle === 0) {
      return false;
    }
    return gc.nativeAttach(handle, surfaceId, width, height);
  }

  /** 解绑 surface：停渲染线程并注销回调，GlobeEngine 数据态保留（可再 attach） */
  static nativeDetach(handle: number): void {
    if (handle !== 0) {
      gc.nativeDetach(handle);
    }
  }

  /** 请求一帧重绘（对应 Kotlin view.requestRender()；surface 未就绪时忽略） */
  static nativeRequestRender(handle: number): void {
    if (handle !== 0) {
      gc.nativeRequestRender(handle);
    }
  }

  // ── 相机 / 视图模式 / 手势 ──

  static nativeSetCamera(handle: number, latitudeDeg: number, longitudeDeg: number,
    altitudeMeters: number, headingDeg: number, tiltDeg: number, rollDeg: number,
    fieldOfViewDeg: number, altitudeMode: number): void {
    if (handle !== 0) {
      gc.nativeSetCamera(handle, latitudeDeg, longitudeDeg, altitudeMeters,
        headingDeg, tiltDeg, rollDeg, fieldOfViewDeg, altitudeMode);
    }
  }

  /** 读回 [lat, lon, alt, heading, tilt, roll, fov, altMode]；句柄已释放返回 null */
  static nativeGetCamera(handle: number): number[] | null {
    if (handle === 0) {
      return null;
    }
    return toIntArray(gc.nativeGetCamera(handle));
  }

  static nativeSetViewMode(handle: number, mode: number): void {
    if (handle !== 0) {
      gc.nativeSetViewMode(handle, mode);
    }
  }

  static nativeGetViewMode(handle: number): number {
    if (handle === 0) {
      return 0;
    }
    return gc.nativeGetViewMode(handle);
  }

  static nativePanBy(handle: number, dxPx: number, dyPx: number): void {
    if (handle !== 0) {
      gc.nativePanBy(handle, dxPx, dyPx);
    }
  }

  static nativeZoomBy(handle: number, factor: number, focusXpx: number, focusYpx: number): void {
    if (handle !== 0) {
      gc.nativeZoomBy(handle, factor, focusXpx, focusYpx);
    }
  }

  static nativeRotateHeading(handle: number, deltaDeg: number): void {
    if (handle !== 0) {
      gc.nativeRotateHeading(handle, deltaDeg);
    }
  }

  static nativeRotateTilt(handle: number, deltaDeg: number): void {
    if (handle !== 0) {
      gc.nativeRotateTilt(handle, deltaDeg);
    }
  }

  // ── 瓦片 / 栅格图层 ──

  static nativeAddTileLayer(handle: number, cacheDir: string, urlTemplate: string,
    maxLevel: number, overlay: boolean): void {
    if (handle !== 0) {
      gc.nativeAddTileLayer(handle, cacheDir, urlTemplate, maxLevel, overlay);
    }
  }

  static nativeSetLayerVisible(handle: number, index: number, visible: boolean): void {
    if (handle !== 0) {
      gc.nativeSetLayerVisible(handle, index, visible);
    }
  }

  static nativeAddRasterLayer(handle: number, cacheDir: string, path: string): number {
    if (handle === 0) {
      return -1;
    }
    return gc.nativeAddRasterLayer(handle, cacheDir, path);
  }

  // ── 矢量图层 ──

  /** iconArgb 为 ARGB 像素数组（>2^31 大值经 toInt32 折算，口径同 Kotlin IntArray） */
  static nativeAddVectorLayer(handle: number, path: string,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    labelField: string, labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    iconArgb: number[] | undefined | null, iconW: number, iconH: number,
    hasExtent: boolean, minLon: number, minLat: number, maxLon: number, maxLat: number,
    maxFeatures: number): number {
    if (handle === 0) {
      return -1;
    }
    return gc.nativeAddVectorLayer(handle, path,
      toInt32(fillColor), toInt32(outlineColor), outlineWidth,
      toInt32(lineColor), lineWidth,
      toInt32(pointColor), pointRadiusDp,
      labelField, toInt32(labelColor), labelSize,
      labelOutline, toInt32(labelOutlineColor),
      toInt32Array(iconArgb), iconW, iconH,
      hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
  }

  static nativeUpdateVectorExtent(handle: number, index: number, hasExtent: boolean,
    minLon: number, minLat: number, maxLon: number, maxLat: number, maxFeatures: number): void {
    if (handle !== 0) {
      gc.nativeUpdateVectorExtent(handle, index, hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
    }
  }

  static nativeHasVectorLoading(handle: number): boolean {
    if (handle === 0) {
      return false;
    }
    return gc.nativeHasVectorLoading(handle);
  }

  static nativeSetVectorLayerVisible(handle: number, index: number, visible: boolean): void {
    if (handle !== 0) {
      gc.nativeSetVectorLayerVisible(handle, index, visible);
    }
  }

  static nativeSetVectorMinLevel(handle: number, index: number, minLevel: number): void {
    if (handle !== 0) {
      gc.nativeSetVectorMinLevel(handle, index, minLevel);
    }
  }

  static nativeGetCameraZoomLevel(handle: number): number {
    if (handle === 0) {
      return 0;
    }
    return gc.nativeGetCameraZoomLevel(handle);
  }

  static nativeRemoveVectorLayer(handle: number, index: number): void {
    if (handle !== 0) {
      gc.nativeRemoveVectorLayer(handle, index);
    }
  }

  // ── 动态叠加层 ──

  static nativeAddOverlayLayer(handle: number,
    fillColor: number, outlineColor: number, outlineWidth: number,
    lineColor: number, lineWidth: number,
    pointColor: number, pointRadiusDp: number,
    labelColor: number, labelSize: number,
    labelOutline: boolean, labelOutlineColor: number,
    iconArgb: number[] | undefined | null, iconW: number, iconH: number): number {
    if (handle === 0) {
      return -1;
    }
    return gc.nativeAddOverlayLayer(handle,
      toInt32(fillColor), toInt32(outlineColor), outlineWidth,
      toInt32(lineColor), lineWidth,
      toInt32(pointColor), pointRadiusDp,
      toInt32(labelColor), labelSize,
      labelOutline, toInt32(labelOutlineColor),
      toInt32Array(iconArgb), iconW, iconH);
  }

  static nativeUpdateOverlayPoints(handle: number, index: number, lonlat: number[],
    fids: number[] | undefined | null, labels: string[] | undefined | null): void {
    if (handle !== 0) {
      gc.nativeUpdateOverlayPoints(handle, index, toFloat64Array(lonlat),
        toFloat64Array(fids), labels);
    }
  }

  static nativeUpdateOverlayLines(handle: number, index: number, lonlat: number[],
    vertexCounts: number[], fids: number[] | undefined | null, labels: string[] | undefined | null): void {
    if (handle !== 0) {
      gc.nativeUpdateOverlayLines(handle, index, toFloat64Array(lonlat),
        toFloat64Array(vertexCounts), toFloat64Array(fids), labels);
    }
  }

  static nativeUpdateOverlayPolygons(handle: number, index: number, lonlat: number[],
    ringVertexCounts: number[], ringsPerFeature: number[],
    fids: number[] | undefined | null, labels: string[] | undefined | null): void {
    if (handle !== 0) {
      gc.nativeUpdateOverlayPolygons(handle, index, toFloat64Array(lonlat),
        toFloat64Array(ringVertexCounts), toFloat64Array(ringsPerFeature),
        toFloat64Array(fids), labels);
    }
  }

  static nativeRemoveOverlayLayer(handle: number, index: number): void {
    if (handle !== 0) {
      gc.nativeRemoveOverlayLayer(handle, index);
    }
  }

  static nativeSetOverlayNoPick(handle: number, index: number, noPick: boolean): void {
    if (handle !== 0) {
      gc.nativeSetOverlayNoPick(handle, index, noPick);
    }
  }

  static nativeClearOverlayLayers(handle: number): void {
    if (handle !== 0) {
      gc.nativeClearOverlayLayers(handle);
    }
  }

  // ── 拾取 / 反算 / 要素几何 ──

  /** 命中返回 [layerIndex, fid]；未命中 null */
  static nativePickVector(handle: number, sxPx: number, syPx: number): number[] | null {
    if (handle === 0) {
      return null;
    }
    return toIntArray(gc.nativePickVector(handle, sxPx, syPx));
  }

  /** 注册单击拾取回调（事件源 native 手势识别，坐标 vp）；传 null 反注册 */
  static nativeSetTapCallback(handle: number, callback: ((x: number, y: number) => void) | null): void {
    if (handle !== 0) {
      gc.nativeSetTapCallback(handle, callback);
    }
  }

  /** 命中返回 [lon, lat]；视口未就绪 null */
  static nativeScreenToGeo(handle: number, sxPx: number, syPx: number): number[] | null {
    if (handle === 0) {
      return null;
    }
    return toIntArray(gc.nativeScreenToGeo(handle, sxPx, syPx));
  }

  /** 命中返回 [[type], ringCounts, ringsPerFeature, lonlat]（普通数组）；未命中 null */
  static nativeFeatureGeometry(handle: number, layerIndex: number, fid: number): number[][] | null {
    if (handle === 0) {
      return null;
    }
    const res: Float64Array[] | null = gc.nativeFeatureGeometry(handle, layerIndex, fid);
    if (res == null || res.length < 4) {
      return null;
    }
    const out: number[][] = [];
    for (let i = 0; i < 4; i++) {
      out.push(Array.from(res[i]));
    }
    return out;
  }

  // ── 显示 / 标记 / 字体 ──

  static nativeSetDisplayDensity(handle: number, density: number): void {
    if (handle !== 0) {
      gc.nativeSetDisplayDensity(handle, density);
    }
  }

  static nativeSetLocationMarker(handle: number, lonDeg: number, latDeg: number,
    visible: boolean, headingDeg: number): void {
    if (handle !== 0) {
      gc.nativeSetLocationMarker(handle, lonDeg, latDeg, visible, headingDeg);
    }
  }

  static nativeSetLocationMarkerIcon(handle: number, iconArgb: number[], iconW: number, iconH: number): void {
    if (handle !== 0) {
      gc.nativeSetLocationMarkerIcon(handle, toInt32Array(iconArgb), iconW, iconH);
    }
  }

  static nativeSetFontPath(handle: number, path: string): void {
    if (handle !== 0) {
      gc.nativeSetFontPath(handle, path);
    }
  }

  /** 设置 HTTPS 校验用 CA 证书包绝对路径（全局）；一般经 initCaBundle 自动传入 */
  static nativeSetCaBundle(path: string): void {
    if (!NativeLib.ensureLoaded()) {
      return;
    }
    gc.nativeSetCaBundle(path);
  }

  /**
   * 初始化 HTTPS CA 证书包：把本 HAR rawfile 的 ca/cacert.pem 拷到沙箱 filesDir 并交给引擎 HttpClient。
   * OHOS 无 Android 系统 CA 目录（/system/etc/security/cacerts），未配置时瓦片 HTTPS 会逐块
   * “校验失败→降级不校验重试”，多一次 TLS 握手、时延翻倍（表现为缩放/旋转后瓦片迟迟不补齐）；
   * 配置后首握手即校验通过。幂等：目标已存在则跳过拷贝。失败仅记日志不抛出——引擎仍会走既有
   * 降级不校验路径，功能不受影响（对齐 Android 首握手即通过的效果）。宿主启动时调一次即可。
   */
  static initCaBundle(context: common.UIAbilityContext): void {
    if (!NativeLib.ensureLoaded()) {
      return;
    }
    try {
      const dest: string = `${context.filesDir}/cacert.pem`;
      if (!fs.accessSync(dest)) {
        const content: Uint8Array = context.resourceManager.getRawFileContentSync('ca/cacert.pem');
        const file = fs.openSync(dest, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
        fs.writeSync(file.fd, content.buffer);
        fs.closeSync(file);
      }
      NativeLib.nativeSetCaBundle(dest);
    } catch (e) {
      console.error(`${TAG}: Failed to init CA bundle: ${e}`);
    }
  }

  // ── PROJ / GDAL 桥（bridge 导出的直通包装，门面见 NativeSrs/NativeLayerInfo/NativeVector）──

  static nativeGetProjVersion(): number {
    return gc.nativeGetProjVersion();
  }

  static nativeInitProjDataPath(path: string): void {
    gc.nativeInitProjDataPath(path);
  }

  /** 命中返回 [x, y]（目标坐标系）；转换不可用 null */
  static nativeConvert(x: number, y: number, srcCrs: string, tgtCrs: string): number[] | null {
    return toIntArray(gc.nativeConvert(x, y, srcCrs, tgtCrs));
  }
}

