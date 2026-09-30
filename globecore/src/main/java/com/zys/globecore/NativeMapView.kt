package com.zys.globecore

import android.content.Context
import android.opengl.GLSurfaceView
import android.util.AttributeSet
import android.view.MotionEvent

/**
 * NativeMapView：globecore 模块对外暴露的地图视图，对应参考实现中的视图窗口组件
 *
 * 它只是一个「接口外壳」：继承 [GLSurfaceView] 提供 GL 上下文，实际的渲染引擎
 * （相机、瓦片网格、取瓦片、缓存、GL 绘制）全部位于 native 的 C++ GlobeEngine 中。
 * GL 生命周期与绘制事件由 [NativeRenderer] 透传到 native 句柄；本类只负责创建/销毁
 * native 实例、相机设置与视图配置，符合迁移方案「视图层只是接口外壳」的约定。
 * 手势识别（平移/捏合/双指旋转/双指俯仰/单击/长按/惯性滑行）已拆出至 [MapGestures]，本类经 [GestureSink]
 * 把识别结果落到 native 相机操作。双指旋转与双指俯仰（tilt）仅 3D 生效（2D 正交恒正北/不消费 tilt，
 * 避免存入不可见姿态）；3D 下双指拖动让位 tilt，单指平移不变。
 *
 * 使用方（如 app 的测试界面）需：
 *  - 在 `onResume`/`onPause` 分别调用 [onResume]/[onPause]（GLSurfaceView 要求，驱动 GL 线程）；
 *  - 在 `onDestroy` 调用 [destroy] 释放 native 资源。
 */
class NativeMapView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : GLSurfaceView(context, attrs) {

    /** 地图视图模式（口径同 native Navigator::ViewMode）：2D 平面墨卡托正交 / 3D 球体透视。 */
    enum class ViewMode { TWO_D, THREE_D }

    /** native C++ GlobeEngine 实例句柄（对象地址）；0 表示已释放或创建失败 */
    private var nativeHandle: Long = NativeLib.nativeCreate()


    init {
        // OpenGL ES 2.0；必须在 setRenderer 之前设置
        setEGLContextClientVersion(2)
        setRenderer(NativeRenderer(nativeHandle))
        // 按需渲染（省电）：静止时不重绘；由手势/fling/相机与图源设置，以及 native 瓦片异步到位回调触发 requestRender
        renderMode = RENDERMODE_WHEN_DIRTY
        // 屏幕密度作为 LOD 细分判据的 densityFactor（对齐 参考实现 engine.setupViewport(w,h,displayMetrics.density)），
        // 仅更新 native 相机状态、不涉及 GL，可在 Surface 创建前调用
        if (nativeHandle != 0L) {
            NativeLib.nativeSetDisplayDensity(nativeHandle, resources.displayMetrics.density.toDouble())
            // 注册重绘回调：native 在瓦片异步加载/解码完成、需再画一帧时回调 requestRender（对齐 参考实现 资源到位触发重绘）
            NativeLib.nativeSetRenderCallback(nativeHandle, this)
        }
        // 触发首帧：WHEN_DIRTY 下 surface 创建后不会自动重绘，需显式请求（setCamera/addTileLayer 亦会再请求）
        requestRender()
    }


    /**
     * 手势识别器：识别结果经 [GestureSink] 落到 native 相机（句柄守卫集中在此）。
     * 平移/缩放/单击/惯性与拆分前逐项等价（双击放大已移除，新增长按入口待功能；见 [MapGestures]）。
     */
    private val gestures = MapGestures(context, object : GestureSink {
        override fun isReady(): Boolean = nativeHandle != 0L

        override fun panBy(dxPx: Double, dyPx: Double) {
            val h = nativeHandle
            if (h != 0L) NativeLib.nativePanBy(h, dxPx, dyPx)
        }

        override fun zoomBy(scaleFactor: Double, focusX: Float, focusY: Float) {
            val h = nativeHandle
            if (h != 0L) NativeLib.nativeZoomBy(h, scaleFactor, focusX.toDouble(), focusY.toDouble())
        }

        override fun rotateBy(deltaDeg: Double) {
            val h = nativeHandle
            // 第一期仅 3D 消费旋转：2D 正交恒正北，手势直接不应用（避免 heading 被隐形写入而后续切 3D 突变）；
            // deltaDeg 已是相机 heading 增量（方向折算在 MapGestures 完成），透传 native 累加即可
            if (h != 0L && NativeLib.nativeGetViewMode(h) == 1) NativeLib.nativeRotateHeading(h, deltaDeg)
        }

        override fun is3DMode(): Boolean {
            val h = nativeHandle
            return h != 0L && NativeLib.nativeGetViewMode(h) == 1
        }

        override fun viewHeightPx(): Int = this@NativeMapView.height

        override fun tiltBy(deltaDeg: Double) {
            val h = nativeHandle
            // 仅 3D 消费俯仰（与 rotateBy 同口径门控）；deltaDeg 已是相机 tilt 增量（方向/灵敏度在
            // MapGestures 折算），native 钳 [0,80]，透传累加即可
            if (h != 0L && NativeLib.nativeGetViewMode(h) == 1) NativeLib.nativeRotateTilt(h, deltaDeg)
        }

        override fun requestRender() {
            this@NativeMapView.requestRender()
        }
    })

    /** 设置单击回调：在手势识别器 onSingleTapConfirmed 触发（避开拖动平移；双击放大已移除）。 */
    fun setOnTapListener(listener: ((x: Float, y: Float) -> Unit)?) {
        gestures.onTap = listener
    }

    /**
     * 触摸事件：分发给手势识别器 [MapGestures]（拖动/捏合/单击/长按/惯性）。
     * 手势只更新 native 相机状态（不涉及 GL），连续渲染模式下下一帧自动生效。
     */
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (nativeHandle == 0L) return false
        return gestures.onTouchEvent(event)
    }

    /**
     * 设置相机（[Camera] 值对象为唯一入口，参照 参考实现 Camera：经纬度/高度 + heading/tilt/roll/fieldOfView/altitudeMode）。
     * 仅更新 native 相机状态、不涉及 GL 调用，可任意线程调用，也可在 Surface 创建前调用（首帧绘制时生效）。
     * native 内部按 参考实现 口径（fieldOfView + 视口高）将高度换算为连续缩放级别，使显示范围与原 参考实现 主界面一致；
     * heading/tilt 由 3D 透视通路消费（2D 正交恒正北），roll/altitudeMode 随 native 留存供 [getCamera] 回读。
     */
    fun setCamera(camera: Camera) {
        val h = nativeHandle
        if (h != 0L) {
            NativeLib.nativeSetCamera(
                h,
                camera.latitude, camera.longitude, camera.altitude,
                camera.heading, camera.tilt, camera.roll,
                camera.fieldOfView, camera.altitudeMode.ordinal
            )
            requestRender()
        }
    }

    /**
     * 读回当前相机（[Camera]，含 heading/tilt/roll/fieldOfView/altitudeMode）；句柄已释放返回 null。
     * 供宿主 Activity 在 onPause 时把视角完整持久化。仅读 native 相机状态、不涉 GL，可主线程调用。
     */
    fun getCamera(): Camera? {
        val h = nativeHandle
        if (h == 0L) return null
        val pose = NativeLib.nativeGetCamera(h) ?: return null
        if (pose.size < 8) return null
        val modes = Camera.AltitudeMode.entries
        val modeIdx = pose[7].toInt().coerceIn(0, modes.lastIndex)
        return Camera(
            latitude = pose[0],
            longitude = pose[1],
            altitude = pose[2],
            heading = pose[3],
            tilt = pose[4],
            roll = pose[5],
            fieldOfView = pose[6],
            altitudeMode = modes[modeIdx],
        )
    }

    /**
     * 设置视图模式（[ViewMode.TWO_D] 平面正交 / [ViewMode.THREE_D] 球体透视）。两模式共用相机状态
     * （中心/高度/heading），切换即保持视角连续；瓦片纹理缓存两模式按 (z,x,y) 键互通。
     * 3D 下瓦片球面 + 矢量层（面/描边/线/点/图标/标注）+ 定位标记均绘制且可拾取（与 2D 同优先级、同命中语义）；
     * screenToGeo 改射线∩椭球（球外返回 null）。瓦片纹理缓存两模式按 (z,x,y) 键互通。
     * 仅更新 native 状态、不涉 GL，可任意线程调用；native 内部已触发重绘。
     */
    fun setViewMode(mode: ViewMode) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetViewMode(h, mode.ordinal)
        requestRender()
    }

    /** 读回当前视图模式；句柄已释放返回 [ViewMode.TWO_D]。 */
    fun getViewMode(): ViewMode {
        val h = nativeHandle
        if (h == 0L) return ViewMode.TWO_D
        return if (NativeLib.nativeGetViewMode(h) == 1) ViewMode.THREE_D else ViewMode.TWO_D
    }

    /**
     * 添加一个瓦片图源图层（可多次调用叠加，按调用顺序绘制：底图在前、注记 overlay 在后）。
     * 每层自带磁盘缓存目录（形如 `/调查宝/tiles/<图源>`）+ 联网 URL 模板 + 图源最大级别，各层独立取瓦片。
     * native 按 `<cacheDir>/<z>/<x>_<y>.tile` 读写（与原 app `FileTileStore`（已删除）布局一致，缓存互通）；
     * 磁盘未命中的瓦片由后台线程按 [urlTemplate] 联网拉取、写盘并异步上传纹理，未就绪时底图以占位色、overlay 以透明显示。
     * [urlTemplate] 含 `{x}/{y}/{z}/{rand=...}` 占位符、Token 须已替换；传空串则禁用联网（只读缓存）。
     * [maxLevel] 为图源数据最大瓦片级别（对应 `MapSource.maxLevel`）：相机 zoom 超过后不再请求更高级
     * 瓦片，而是拉伸末级瓦片纹理（与原主界面“无限放大”一致，避免 404 占位块）。
     * [overlay]=true 为叠加层（注记，带透明 PNG）：绘制在上层并开 alpha 混合，缺失瓦片不画占位（保持透明露出底图）。
     * 仅更新 native 状态、不涉及 GL，可任意线程调用。
     */
    fun addTileLayer(cacheDir: String, urlTemplate: String, maxLevel: Int, overlay: Boolean = false) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeAddTileLayer(h, cacheDir, urlTemplate, maxLevel, overlay)
        requestRender()
    }

    /**
     * 设置指定图层可见性（[index] 为 [addTileLayer] 的加入次序，0 起）：隐藏层不绘制、不取瓦片（对齐 参考实现 Layer.isEnabled）。
     * 用于注记显隐开关等；index 越界时 native 忽略。仅更新 native 状态，内部会触发一帧重绘使变更立即生效。
     */
    fun setLayerVisible(index: Int, visible: Boolean) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetLayerVisible(h, index, visible)
        requestRender()
    }

    /**
     * 添加一个本地栅格图层（tif/img 等 GDAL 可打开格式），可多次调用叠加。native 用内建 GDAL 重投影，
     * 按全球墨卡托瓦片 (z,x,y) 四至即时生成瓦片（仅磁盘缓存未命中时），生成后回写 [cacheDir] 复用；
     * 以 alpha 混合绘制在底图之上、矢量层之下（范围外像素透明），maxLevel 由栅格分辨率自动推算。
     * [cacheDir] 为该栅格瓦片的磁盘缓存目录（形如 `/调查宝/tiles/raster-jni/<hash>`）。返回图层 index
     * （0 起，与 [addTileLayer] 共享索引空间，供 [setLayerVisible]）；打不开栅格或句柄已释放返回 -1。
     * 仅更新 native 状态、不涉 GL，可任意线程调用；图层加入后不删除，只经可见性开关显隐。
     */
    fun addRasterLayer(cacheDir: String, path: String): Int {
        val h = nativeHandle
        val index = if (h != 0L) NativeLib.nativeAddRasterLayer(h, cacheDir, path) else -1
        requestRender()
        return index
    }

    /**
     * 设置定位标记（蓝点）的地理坐标（[Position]）、可见性与移动方位角：标记在所有瓦片层之上、以屏幕固定尺寸绘制（不随缩放变化）。
     * [headingDeg] 为移动方位角（顺时针自北 0..360），默认 -1.0 表示无方向、不画方向箭头。
     * 由定位回调（约 1Hz）驱动更新；仅更新 native 状态，内部会触发一帧重绘使位置立即生效。
     */
    fun setLocationMarker(position: Position, visible: Boolean, headingDeg: Double = -1.0) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetLocationMarker(h, position.longitude, position.latitude, visible, headingDeg)
        requestRender()
    }

    /**
     * 设置定位标记罗盘图标（Android ARGB 像素 + 尺寸）：有图标时 native 画纹理四边形替代蓝点，
     * 对齐原主界面 LocationModel ic_compass 罗盘标记方式。宜在初始化时调用一次（图标不变无需重复设置）。
     * 内部把 ARGB 转 RGBA 上传纹理，GL 线程懒生效；句柄已释放时忽略。
     */
    fun setLocationMarkerIcon(iconArgb: IntArray, iconW: Int, iconH: Int) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetLocationMarkerIcon(h, iconArgb, iconW, iconH)
        requestRender()
    }

    /**
     * 添加一个矢量图层（shp/kml/kmz/dwg/dxf 等 GDAL/OGR 可打开的格式），可多次调用叠加。
     * native 侧直接读文件、重投影到 WGS84、earcut 三角剖分并异步上传 VBO；绘制在底图瓦片之上、
     * 注记 overlay 之下（对齐主界面「注记置顶于矢量之上」）。样式由约定类 [VectorStyle] 承载
     * （面填充/描边/线/点/标注，颜色 #AARRGGBB、线宽像素、点半径 dp）；labelField 非空时逐要素取该字段值为标注。
     * [iconArgb] 非空时（配合 [iconW]/[iconH]）为点要素图标的 Android ARGB 像素（由宿主解码矢量 drawable 得到），
     * native 转 RGBA 上传纹理作中心锚点、屏幕固定尺寸 billboard；为空则点要素回退画屏幕固定圆。
     * 返回图层 index（0 起，供 [setVectorLayerVisible]）；句柄已释放返回 -1。仅更新 native 状态、不涉 GL，可任意线程调用；图层加入后不删除，只经可见性开关显隐。
     * [extent] 非空时按该屏幕范围空间相交只加载屏内要素（大数据渐进加载）；为空则整文件全量（小数据直显、不参与相机重载）。
     * [maxFeatures] 为单次加载要素预算（≤0 取 native 硬上限）。
     */
    fun addVectorLayer(
        path: String,
        style: VectorStyle,
        iconArgb: IntArray? = null,
        iconW: Int = 0,
        iconH: Int = 0,
        extent: VectorExtent? = null,
        maxFeatures: Int = 0
    ): Int {
        val h = nativeHandle
        val index = if (h != 0L) NativeLib.nativeAddVectorLayer(
            h, path, style.fillColor, style.outlineColor, style.outlineWidth,
            style.lineColor, style.lineWidth, style.pointColor, style.pointRadiusDp,
            style.labelField, style.labelColor, style.labelSize, style.labelOutline, style.labelOutlineColor,
            iconArgb, iconW, iconH,
            extent != null,
            extent?.minLon ?: 0.0, extent?.minLat ?: 0.0,
            extent?.maxLon ?: 0.0, extent?.maxLat ?: 0.0, maxFeatures
        ) else -1
        requestRender()
        return index
    }

    /**
     * 按新屏幕范围重载指定矢量层（[index] 为 [addVectorLayer] 加入次序）。native 后台按新范围重读建新几何，
     * 就绪后由 GL 线程同帧替换旧几何（Swap-on-ready：停下即有数据、重载期间无空窗）。
     * [extent] 为空时走整文件全量；[maxFeatures]≤0 取硬上限。仅更新 native 状态，内部触发一帧重绘。
     */
    fun updateVectorExtent(index: Int, extent: VectorExtent?, maxFeatures: Int = 0) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeUpdateVectorExtent(
            h, index, extent != null,
            extent?.minLon ?: 0.0, extent?.minLat ?: 0.0,
            extent?.maxLon ?: 0.0, extent?.maxLat ?: 0.0, maxFeatures
        )
        requestRender()
    }

    /**
     * 文件矢量层是否仍有未完成的加载/上传（后台 GDAL 读取与并行建几何中，或 GL 线程正一次性上传大几何，
     * 巨层上传可达秒级）。纯状态查询、不触发重绘，可 UI 线程调用，供「矢量加载中」提示轮询显隐。
     * 内存叠加层（测量/轨迹/拍照）为小数据同步构建，不计入。
     */
    fun hasVectorLoading(): Boolean {
        val h = nativeHandle
        return h != 0L && NativeLib.nativeHasVectorLoading(h)
    }

    /**
     * 设置矢量标注字体文件路径（[path] 为空则 native 自动探测系统 CJK 字体，零 APK 增量）：加载字形图集，
     * 供矢量要素标注文本渲染。加载含一次文件 IO（纯 CPU、线程安全），宜在 UI 线程、添加矢量图层前调用一次。
     */
    fun setFontPath(path: String) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetFontPath(h, path)
        requestRender()
    }

    /**
     * 设置指定矢量图层可见性（[index] 为 [addVectorLayer] 的加入次序，0 起）：隐藏层不绘制（对齐 参考实现 Layer.isEnabled）。
     * index 越界时 native 忽略。仅更新 native 状态，内部会触发一帧重绘使变更立即生效。
     */
    fun setVectorLayerVisible(index: Int, visible: Boolean) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetVectorLayerVisible(h, index, visible)
        requestRender()
    }

    /**
     * 设置文件矢量层级别可见性下限（对齐文档 LayerInfo.effectiveMinDisplayLevel，按界面显示级别取值）：
     * 相机显示级别低于 [minLevel] 时整层隐藏（不绘制/不上传/不可拾取/不计入 [hasVectorLoading]），
     * ≤0 不限——避免小级别下大范围无意义的读取与零散显示。内存叠加层不受影响。越界忽略；触发一帧重绘即时生效。
     */
    fun setVectorMinLevel(index: Int, minLevel: Int) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetVectorMinLevel(h, index, minLevel)
        requestRender()
    }

    /**
     * 当前相机显示级别（native 统一口径 Navigator::displayLevel，与 app 界面级别文本/矢量级别可见性一致）：
     * 供宿主在相机静止发出屏幕范围重载前判定级别达标，级别不足跳过无谓加载。句柄已释放返回 0。
     */
    fun cameraZoomLevel(): Int {
        val h = nativeHandle
        return if (h != 0L) NativeLib.nativeGetCameraZoomLevel(h) else 0
    }

    /**
     * 移除一个矢量图层（墓碑：native 置 dead + 隐藏，GL 线程下一帧回收其 VBO/图标纹理/CPU 几何）：
     * 文件矢量层与叠加层均适用，[index] 越界时 native 忽略。不 erase 底层 vector 容器，其它层 index 保持稳定。
     * 用于宿主样式变更时就地换层（先 [removeVectorLayer] 旧层再 [addVectorLayer] 新层），
     * 避免整界面 recreate 造成地图闪动。仅更新 native 状态，内部会触发一帧重绘使变更立即生效。
     */
    fun removeVectorLayer(index: Int) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeRemoveVectorLayer(h, index)
        requestRender()
    }

    /**
     * 新建一个动态叠加层（运行时内存几何：测量/拍照标识/轨迹/样地等业务叠加），返回其 index（0 起，
     * 与 [addVectorLayer] 共享索引空间，[pickVector] 亦返回该 index）；句柄已释放返回 -1。
     * 样式由约定类 [VectorStyle] 承载（同 [addVectorLayer]，但 labelField 不生效——标注文本由 updateOverlay* 逐要素传入）；
     * [iconArgb]/[iconW]/[iconH] 语义同 [addVectorLayer]。新建后为空，须经 updateOverlay* 推送几何方可见。
     * 约定：每类叠加「加一次、就地更新多次」，仅整类拆除时 [removeOverlayLayer]。仅更新 native 状态，可任意线程调用。
     */
    fun addOverlayLayer(
        style: VectorStyle,
        iconArgb: IntArray? = null,
        iconW: Int = 0,
        iconH: Int = 0
    ): Int {
        val h = nativeHandle
        val index = if (h != 0L) NativeLib.nativeAddOverlayLayer(
            h, style.fillColor, style.outlineColor, style.outlineWidth,
            style.lineColor, style.lineWidth, style.pointColor, style.pointRadiusDp,
            style.labelColor, style.labelSize, style.labelOutline, style.labelOutlineColor,
            iconArgb, iconW, iconH
        ) else -1
        requestRender()
        return index
    }

    /**
     * 更新叠加层的点要素（覆盖式）。[lonlat] 为 [lon0,lat0,lon1,lat1,...]；[fids]/[labels] 与点一一对应
     * （fids 为 null 则用序号；labels 为 null 或空串则该点不标注）。[index] 为 [addOverlayLayer] 返回值。
     */
    fun updateOverlayPoints(index: Int, lonlat: DoubleArray, fids: LongArray? = null, labels: Array<String>? = null) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeUpdateOverlayPoints(h, index, lonlat, fids, labels)
        requestRender()
    }

    /**
     * 更新叠加层的线要素（覆盖式）。[lonlat] 为全部线顶点摊平；[vertexCounts] 第 f 项为第 f 条线的顶点数；
     * [fids]/[labels] 与线一一对应。每条线作为单折线渲染。
     */
    fun updateOverlayLines(
        index: Int, lonlat: DoubleArray, vertexCounts: IntArray,
        fids: LongArray? = null, labels: Array<String>? = null
    ) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeUpdateOverlayLines(h, index, lonlat, vertexCounts, fids, labels)
        requestRender()
    }

    /**
     * 更新叠加层的面要素（覆盖式）。[lonlat] 为全部环顶点摊平；[ringVertexCounts] 为每环顶点数（跨要素摊平）；
     * [ringsPerFeature] 第 f 项为第 f 个面的环数（首环外环、余为洞）；[fids]/[labels] 与面一一对应。
     */
    fun updateOverlayPolygons(
        index: Int, lonlat: DoubleArray, ringVertexCounts: IntArray, ringsPerFeature: IntArray,
        fids: LongArray? = null, labels: Array<String>? = null
    ) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeUpdateOverlayPolygons(h, index, lonlat, ringVertexCounts, ringsPerFeature, fids, labels)
        requestRender()
    }

    /** 移除叠加层（墓碑：native 置 dead + 隐藏，GL 线程下一帧回收其资源）：仅对叠加层生效，越界忽略。 */
    fun removeOverlayLayer(index: Int) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeRemoveOverlayLayer(h, index)
        requestRender()
    }

    /**
     * 设置叠加层「不参与拾取」标志（仅对叠加层生效，越界/非叠加层忽略）：[noPick]=true 时层仍正常绘制，
     * 但 [pickVector] 整层跳过。选中高亮/查询高亮等瞬态视觉层经此退出拾取竞争，命中直接落到
     * 源矢量层/业务叠加层（测量/拍照等），宿主无需再做命中穿透分流。纯 CPU 标志位、不涉 GL。
     */
    fun setOverlayNoPick(index: Int, noPick: Boolean) {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeSetOverlayNoPick(h, index, noPick)
    }

    /** 移除全部叠加层（墓碑所有叠加层）：文件矢量层不受影响。 */
    fun clearOverlayLayers() {
        val h = nativeHandle
        if (h != 0L) NativeLib.nativeClearOverlayLayers(h)
        requestRender()
    }

    /**
     * 拾取矢量要素：屏幕点命中检测可见矢量层，命中返回 [layerIndex, fid]（LongArray 长度 2），未命中返回 null。
     * 命中分两阶段：先动态叠加层（测量/轨迹/拍照/样地）后文件矢量层，各阶段内后加入优先；
     * noPick 瞬态层（见 [setOverlayNoPick]）跳过。
     * layerIndex 为 [addVectorLayer] 的加入次序，fid 为源数据要素 ID（供上层按 FID 回取属性）。
     * 仅读 native CPU 几何、不涉及 GL，可在主线程调用（通常由 [setOnTapListener] 单击回调驱动）。
     */
    fun pickVector(sxPx: Float, syPx: Float): LongArray? {
        val h = nativeHandle
        return if (h != 0L) NativeLib.nativePickVector(h, sxPx.toDouble(), syPx.toDouble()) else null
    }

    /**
     * 屏幕点 → 地理坐标（[Position]，WGS84 度）：命中返回经纬度（altitude=0），视口未就绪或句柄已释放返回 null。
     * 供采集交互（测量/轨迹）把地图单击转成加点坐标。仅读 native 相机状态、不涉及 GL，可在主线程调用。
     */
    fun screenToGeo(sxPx: Float, syPx: Float): Position? {
        val h = nativeHandle
        if (h == 0L) return null
        val geo = NativeLib.nativeScreenToGeo(h, sxPx.toDouble(), syPx.toDouble()) ?: return null
        if (geo.size < 2) return null
        return Position(latitude = geo[1], longitude = geo[0])
    }

    /**
     * 取指定矢量层某 FID 要素的经纬度几何（WGS84 度），供宿主点击选中后画高亮叠加层（对齐主界面选中态高亮）。
     * 命中返回 [FeatureGeometry]（几何类型 + 摊平经纬度 + 环/段顶点数 + 每要素环数），未命中或句柄已释放返回 null。
     * 仅读 native CPU 几何、不涉 GL，可在主线程调用（通常由 [pickVector] 命中后驱动）。
     */
    fun featureGeometry(layerIndex: Int, fid: Long): FeatureGeometry? {
        val h = nativeHandle
        if (h == 0L) return null
        val res = NativeLib.nativeFeatureGeometry(h, layerIndex, fid) ?: return null
        if (res.size < 4) return null
        val type = res[0].firstOrNull()?.toInt() ?: return null
        val ringCounts = IntArray(res[1].size) { res[1][it].toInt() }
        val ringsPerFeature = IntArray(res[2].size) { res[2][it].toInt() }
        return FeatureGeometry(type, res[3], ringCounts, ringsPerFeature)
    }

    /**
     * 释放 native GlobeEngine 实例（删除 C++ 对象）。应在宿主 Activity/Fragment 的 `onDestroy` 调用。
     * GL 资源（着色器/VBO）的删除在 [onDetachedFromWindow] 经 GL 线程完成，此处只回收 C++ 内存。
     * 幂等：重复调用安全。
     */
    fun destroy() {
        // 停掉惯性滑行的逐帧回调，避免销毁后继续驱动 native
        gestures.cancelFling()
        val h = nativeHandle
        if (h != 0L) {
            nativeHandle = 0L
            NativeLib.nativeDestroy(h)
        }
    }

    /**
     * 视图从窗口移除时，在 GL 线程释放 GL 资源（`glDelete*` 必须在持有上下文的 GL 线程执行）。
     * 通过 [queueEvent] 投递到 GL 线程，避免在主线程误调 GL。
     */
    override fun onDetachedFromWindow() {
        val h = nativeHandle
        if (h != 0L) {
            queueEvent { NativeLib.nativeReleaseGl(h) }
        }
        super.onDetachedFromWindow()
    }
}
