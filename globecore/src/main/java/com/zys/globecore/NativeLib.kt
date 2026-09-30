package com.zys.globecore

/**
 * globecore 的 JNI 桥接入口：所有渲染 / 瓦片 / 缓存逻辑均在 native（C++）实现，此处仅声明外部函数。
 * native 侧对应符号为 `Java_com_zys_globecore_NativeLib_*`（见 cpp/jni/GlobeEngineJni.cpp）。
 *
 * 采用「native 句柄」模型：[nativeCreate] 返回一个指向 C++ GlobeEngine 实例的地址（Long），
 * 后续调用透传该句柄，避免全局单例、支持多实例。
 */
internal object NativeLib {

    init {
        System.loadLibrary("globecore")
    }

    /** 创建 native GlobeEngine 实例，返回其句柄（C++ 对象地址）；失败返回 0 */
    external fun nativeCreate(): Long

    /** 销毁 native GlobeEngine 实例，释放 C++ 侧内存 */
    external fun nativeDestroy(handle: Long)

    /** GL 上下文创建，转发 `GLSurfaceView.Renderer#onSurfaceCreated` */
    external fun nativeSurfaceCreated(handle: Long)

    /** GL 视口尺寸变化，转发 `onSurfaceChanged` */
    external fun nativeSurfaceChanged(handle: Long, width: Int, height: Int)

    /** 绘制一帧，转发 `onDrawFrame` */
    external fun nativeDrawFrame(handle: Long)

    /** 在 GL 线程释放 GL 资源（着色器/VBO），须在 [nativeDestroy] 之前经 `queueEvent` 调用 */
    external fun nativeReleaseGl(handle: Long)

    /**
     * 设置相机完整姿态（对应 参考实现 Camera 唯一通道）：经纬度/高度 + heading/tilt/roll/fieldOfView（角度用度、高度用米）
     * + altitudeMode（[Camera.AltitudeMode] 的 ordinal）。由 [NativeMapView.setCamera] 驱动。
     */
    external fun nativeSetCamera(
        handle: Long, latitudeDeg: Double, longitudeDeg: Double, altitudeMeters: Double,
        headingDeg: Double, tiltDeg: Double, rollDeg: Double, fieldOfViewDeg: Double, altitudeMode: Int
    )

    /**
     * 读回相机完整姿态，返回 [latitude, longitude, altitude, heading, tilt, roll, fieldOfView, altitudeMode(ordinal)]
     * （长度 8）；句柄已释放返回 null。供 [NativeMapView.getCamera] 组装 [Camera]。
     */
    external fun nativeGetCamera(handle: Long): DoubleArray?

    /**
     * 设置视图模式：0=2D 平面墨卡托正交 / 1=3D 球体透视（口径同 native Navigator::ViewMode）。
     * 两模式共用相机状态（中心/高度/heading），切换即保持视角连续；内部已触发一帧重绘。
     * 3D 下瓦片球面 + 矢量层（面/描边/线/点/图标/标注）+ 定位标记均绘制且可拾取；screenToGeo 改射线∩椭球
     * （球外返回 null）。瓦片纹理缓存两模式按 (z,x,y) 键互通。
     */
    external fun nativeSetViewMode(handle: Long, mode: Int)

    /** 读回当前视图模式（0/1）；句柄已释放返回 0（2D）。 */
    external fun nativeGetViewMode(handle: Long): Int

    /** 手势平移：按屏幕像素位移拖动地图（dx 右为正、dy 下为正） */
    external fun nativePanBy(handle: Long, dxPx: Double, dyPx: Double)

    /** 手势缩放：以屏幕焦点为锚按 factor 缩放（factor>1 放大/拉近） */
    external fun nativeZoomBy(handle: Long, factor: Double, focusXpx: Double, focusYpx: Double)

    /**
     * 手势旋转：相机 heading 累加增量（度，顺时针自北为正；正=heading 增大、地图内容逆时针转），
     * native 侧环绕归一到 [-180,180)。仅 3D 透视通路消费（2D 恒正北，由 [NativeMapView] 手势侧门控不调用）。
     */
    external fun nativeRotateHeading(handle: Long, deltaDeg: Double)

    /**
     * 手势俯仰：相机 tilt 累加增量（度，正=向地平线方向倾视，0=正下 nadir），native 侧钳制到 [0, 75]。
     * 语义为「绕目标保眼高」俯仰（屏心地面点不漂移）。仅 3D 透视通路消费（2D 不消费 tilt，
     * 由 [NativeMapView] 手势侧门控不调用）。
     */
    external fun nativeRotateTilt(handle: Long, deltaDeg: Double)

    /** 添加一个瓦片图源图层（可多次调用叠加，按调用顺序绘制：底图在前、注记 overlay 在后）：
     * 磁盘缓存目录 + 联网 URL 模板（Token 已替换，空串禁用联网）+ 图源最大级别（超出后拉伸末级瓦片）+ 是否为叠加层，
     * native 按 `<cacheDir>/<z>/<x>_<y>.tile` 读写；overlay=true（注记，带透明 PNG）开 alpha 混合叠加其上 */
    external fun nativeAddTileLayer(handle: Long, cacheDir: String, urlTemplate: String, maxLevel: Int, overlay: Boolean)

    /** 设置指定图层可见性（index 为 [nativeAddTileLayer] 加入次序，0 起）：隐藏层不绘制、不取瓦片（如注记显隐开关） */
    external fun nativeSetLayerVisible(handle: Long, index: Int, visible: Boolean)

    /** 添加一个本地栅格图层（tif/img 等 GDAL 可打开格式）：native 用内建 GDAL 重投影按全球墨卡托瓦片
     * 四至即时生成瓦片（磁盘缓存未命中时），以 alpha 混合绘制在底图之上、矢量层之下（范围外透明）；maxLevel
     * 由栅格分辨率自动推算。返回新图层索引（0 起，与 [nativeAddTileLayer] 共享 layers_ 索引空间，供
     * [nativeSetLayerVisible]）；打不开栅格或句柄无效返回 -1。 */
    external fun nativeAddRasterLayer(handle: Long, cacheDir: String, path: String): Int

    /** 设置定位标记（蓝点）的地理坐标、可见性与移动方位角（headingDeg<0 不画箭头）：在所有瓦片层之上以屏幕固定尺寸绘制 */
    external fun nativeSetLocationMarker(handle: Long, lonDeg: Double, latDeg: Double, visible: Boolean, headingDeg: Double)

    /**
     * 设置定位标记罗盘图标（Android ARGB 像素 + 尺寸）：有图标时 native 画纹理四边形替代蓝点，
     * 对齐原主界面 LocationModel ic_compass 方式。复用 iconArgbToRgba 转换口径（ARGB→RGBA）。
     */
    external fun nativeSetLocationMarkerIcon(handle: Long, iconArgb: IntArray, iconW: Int, iconH: Int)

    /**
     * 添加一个矢量图层（shp/kml/kmz/dwg/dxf 等 GDAL/OGR 可打开的格式），native 侧直接读文件、
     * 重投影到 WGS84、earcut 三角剖分并异步上传 VBO；绘制在底图瓦片之上、注记 overlay 之下。
     * 颜色为 #AARRGGBB 打包 Int；线宽单位像素、点半径单位 dp。labelField 非空时逐要素取该字段值为标注
     * 文本（labelSize 为字号缩放、1.0=基准）。iconArgb 非空时（配合 iconW/iconH）为点要素图标的 Android
     * ARGB 像素，native 转 RGBA 上传纹理作中心锚点 billboard；为空则点要素回退画屏幕固定圆。
     * 返回图层 index（0 起，供 [nativeSetVectorLayerVisible]）。
     * [hasExtent]+[minLon,minLat,maxLon,maxLat]：初始屏幕过滤范围（大数据按视口只加载屏内要素）；false=整文件全量。
     * [maxFeatures]：单次加载要素预算（≤0 取硬上限）。
     */
    external fun nativeAddVectorLayer(
        handle: Long, path: String,
        fillColor: Int, outlineColor: Int, outlineWidth: Float,
        lineColor: Int, lineWidth: Float,
        pointColor: Int, pointRadiusDp: Float,
        labelField: String, labelColor: Int, labelSize: Float,
        labelOutline: Boolean, labelOutlineColor: Int,
        iconArgb: IntArray?, iconW: Int, iconH: Int,
        hasExtent: Boolean, minLon: Double, minLat: Double, maxLon: Double, maxLat: Double, maxFeatures: Int
    ): Int

    /**
     * 按新屏幕范围重载指定矢量层（Swap-on-ready，无空窗）：index 为 [nativeAddVectorLayer] 加入次序。
     * hasExtent=false 走整文件全量；maxFeatures≤0 取硬上限。越界/非文件层忽略。
     */
    external fun nativeUpdateVectorExtent(
        handle: Long, index: Int, hasExtent: Boolean,
        minLon: Double, minLat: Double, maxLon: Double, maxLat: Double, maxFeatures: Int
    )

    /**
     * 文件矢量层是否仍有未完成的加载/上传（后台 GDAL 读取与并行建几何中，或可见层 chunk 正在逐帧
     * 流式上传中；隐藏/级别不足层挂起不计入）。纯状态查询不触发重绘，可 UI 线程随时调用，
     * 供宿主「矢量加载中」提示轮询显隐；句柄已释放返回 false。
     */
    external fun nativeHasVectorLoading(handle: Long): Boolean

    /** 设置矢量标注字体文件路径（path 为空则 native 自动探测系统 CJK 字体）：加载字形图集，供标注文本渲染。宜在 UI 线程调用。 */
    external fun nativeSetFontPath(handle: Long, path: String)

    /** 设置指定矢量图层可见性（index 为 [nativeAddVectorLayer] 加入次序，0 起）：隐藏层不绘制（对齐 参考实现 Layer.isEnabled） */
    external fun nativeSetVectorLayerVisible(handle: Long, index: Int, visible: Boolean)

    /**
     * 设置文件矢量层级别可见性下限（对齐文档 LayerInfo.effectiveMinDisplayLevel，按界面显示级别取值）：相机显示级别
     * < minLevel 时整层隐藏（不绘制/不上传/不可拾取/不计入加载提示），≤0 不限。越界/叠加层忽略；变更触发一帧重绘。
     */
    external fun nativeSetVectorMinLevel(handle: Long, index: Int, minLevel: Int)

    /** 当前相机显示级别（与矢量级别可见性同口径，同源 app MercatorZoom.altitudeToLevel）：供发出屏幕范围重载前判定级别达标。句柄无效返回 0。 */
    external fun nativeGetCameraZoomLevel(handle: Long): Int

    /**
     * 移除一个矢量图层（墓碑：native 置 dead + visible=false，GL 线程下一帧回收其 VBO/纹理/几何）：
     * 文件矢量层与叠加层均适用，index 越界忽略。不 erase vectorLayers_，其它层 index 保持稳定。
     * 用于宿主样式变更时就地换层（先 remove 旧层再 add 新层），避免整界面 recreate 造成闪动。
     */
    external fun nativeRemoveVectorLayer(handle: Long, index: Int)

    /**
     * 新建一个动态叠加层（运行时内存几何：测量/拍照标识/轨迹/样地等业务叠加），返回其 index（0 起，
     * 与 [nativeAddVectorLayer] 共享索引空间，[nativePickVector] 亦返回该 index）。样式参数同矢量层，但无
     * labelField（标注文本由 nativeUpdateOverlay* 逐要素传入）；iconArgb 语义同 [nativeAddVectorLayer]。
     * 新建后为空，须经 nativeUpdateOverlay* 推送几何方可见。
     */
    external fun nativeAddOverlayLayer(
        handle: Long,
        fillColor: Int, outlineColor: Int, outlineWidth: Float,
        lineColor: Int, lineWidth: Float,
        pointColor: Int, pointRadiusDp: Float,
        labelColor: Int, labelSize: Float,
        labelOutline: Boolean, labelOutlineColor: Int,
        iconArgb: IntArray?, iconW: Int, iconH: Int
    ): Int

    /** 更新叠加点要素（覆盖式）：lonlat=[lon0,lat0,lon1,lat1,...]；fids/labels 与点一一对应（labels 空串则该点不标注）。 */
    external fun nativeUpdateOverlayPoints(
        handle: Long, index: Int, lonlat: DoubleArray, fids: LongArray?, labels: Array<String>?
    )

    /** 更新叠加线要素（覆盖式）：lonlat 为全部线顶点摊平；vertexCounts[f] 为第 f 条线顶点数；fids/labels 与线对应。 */
    external fun nativeUpdateOverlayLines(
        handle: Long, index: Int, lonlat: DoubleArray, vertexCounts: IntArray, fids: LongArray?, labels: Array<String>?
    )

    /**
     * 更新叠加面要素（覆盖式）：lonlat 为全部环顶点摊平；ringVertexCounts 为每环顶点数（跨要素摊平）；
     * ringsPerFeature[f] 为第 f 个面环数（首环外环、余为洞）；fids/labels 与面对应。
     */
    external fun nativeUpdateOverlayPolygons(
        handle: Long, index: Int, lonlat: DoubleArray, ringVertexCounts: IntArray, ringsPerFeature: IntArray,
        fids: LongArray?, labels: Array<String>?
    )

    /** 移除叠加层（墓碑，GL 线程下一帧回收资源）：仅对叠加层生效，越界忽略。 */
    external fun nativeRemoveOverlayLayer(handle: Long, index: Int)

    /**
     * 设置叠加层「不参与拾取」标志（仅对叠加层生效，越界/非叠加层忽略）：[noPick]=true 时层仍正常绘制，
     * 但 [nativePickVector] 整层跳过。选中高亮/查询高亮等瞬态视觉层经此退出拾取竞争，
     * 命中直接落到源矢量层/业务叠加层，宿主无需再做命中穿透分流。
     */
    external fun nativeSetOverlayNoPick(handle: Long, index: Int, noPick: Boolean)

    /** 移除全部叠加层（墓碑所有叠加层）：文件矢量层不受影响。 */
    external fun nativeClearOverlayLayers(handle: Long)

    /**
     * 拾取矢量要素：屏幕点 (sxPx,syPx) 命中检测可见矢量层，命中返回 [layerIndex, fid]（LongArray 长度 2），
     * 未命中返回 null。命中分两阶段：先动态叠加层（测量/轨迹/拍照/样地）后文件矢量层，各阶段内后加入优先；
     * noPick 瞬态层（见 [nativeSetOverlayNoPick]）跳过。layerIndex 为 [nativeAddVectorLayer] 加入次序，
     * fid 为源数据要素 ID。仅读 native CPU 几何、不涉及 GL，可在主线程调用。
     */
    external fun nativePickVector(handle: Long, sxPx: Double, syPx: Double): LongArray?

    /**
     * 屏幕点 → 地理经纬度（WGS84 度）：命中返回 [lon, lat]（DoubleArray 长度 2），视口未就绪返回 null。
     * 供采集交互（测量/轨迹）把地图单击转成加点坐标。仅读 native 相机状态、不涉及 GL，可在主线程调用。
     */
    external fun nativeScreenToGeo(handle: Long, sxPx: Double, syPx: Double): DoubleArray?

    /**
     * 取指定矢量层某 FID 要素的经纬度几何（WGS84 度）：命中返回 `DoubleArray` 数组
     * `[ {type}, ringCounts, ringsPerFeature, lonlat ]`（type：0=点,1=线,2=面；lonlat 摊平 [lon,lat,...]），
     * 未命中或句柄已释放返回 null。供宿主点击选中后画高亮叠加层。仅读 native CPU 几何、不涉 GL，可在主线程调用。
     */
    external fun nativeFeatureGeometry(handle: Long, layerIndex: Int, fid: Long): Array<DoubleArray>?

    /** 设置屏幕密度（`displayMetrics.density`），作为 LOD 细分判据的 densityFactor（对齐 参考实现 `setupViewport(w,h,density)`） */
    external fun nativeSetDisplayDensity(handle: Long, density: Double)

    /**
     * 注册「需重绘」回调：RENDERMODE_WHEN_DIRTY 省电模式下，native 在瓦片异步加载/解码完成、
     * 需再画一帧时回调 [view] 的 `requestRender`（对齐 参考实现 资源到位触发重绘）。[view] 传 [NativeMapView] 实例。
     */
    external fun nativeSetRenderCallback(handle: Long, view: Any)
}
