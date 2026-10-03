#ifndef GLOBECORE_CORE_GlobeEngine_H
#define GLOBECORE_CORE_GlobeEngine_H

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "layer/TileLayer.h"
#include "layer/VectorLayer.h"
#include "navigator/Navigator.h"
#include "render/Renderer.h"

namespace globecore {

/**
 * GlobeEngine：globecore 模块的顶层渲染引擎对象，合并了参考实现中视图窗口与渲染引擎两层职责。
 *
 * Kotlin 侧对外暴露的视图类（NativeMapView，继承 GLSurfaceView）持有本对象句柄（jlong），
 * 把 GL 生命周期与绘制事件转发到这里；相机、瓦片网格、取瓦片、缓存、GL 绘制等实现均位于 native。
 *
 * 组合关系：持有 [Navigator]（相机）、一组 [TileLayer]（每层 = 磁盘缓存 + 异步加载器，支持多图源叠加）
 * 与 [Renderer]（GL 绘制）。底图（影像）层在前不透明、注记 overlay 层在后开 alpha 混合。
 */
class GlobeEngine {
public:
    GlobeEngine();
    ~GlobeEngine();

    GlobeEngine(const GlobeEngine &) = delete;
    GlobeEngine &operator=(const GlobeEngine &) = delete;

    /// GL 上下文创建（转发到 Renderer）
    void surfaceCreated();

    /// 视口尺寸变化：同步相机视口 + 转发到 Renderer
    void surfaceChanged(int width, int height);

    /// 绘制一帧（转发到 Renderer）。若 Renderer 报告仍有瓦片在途/待上传且已注册重绘回调，
    /// 则回调一次 requestRender（供 RENDERMODE_WHEN_DIRTY 省电模式下异步瓦片到位时驱动下一帧）。
    void drawFrame();

    /// 设置相机（以 [Camera] 值对象整体写入，对应 参考实现 Camera：中心/高度 + heading/tilt/roll/fov/altitudeMode）
    void setCamera(const Camera &camera);

    /// 读回当前相机完整姿态（对应 参考实现 Camera），供宿主以 [Camera] 值对象读写视角
    Camera getCamera() const;

    /// 设置视图模式（0=2D 平面墨卡托正交 / 1=3D 球体透视，口径同 [Navigator::ViewMode]）。
    /// 两模式共用相机状态（中心/高度/heading），切换即保持视角连续；变更后触发一帧重绘即时生效。
    /// 3D 通路已接：瓦片球面/矢量贴地(面描边线点图标)/标注/定位标记/拾取。
    void setViewMode(int mode);

    /// 读回当前视图模式（0/1），供宿主 UI 态同步。
    int viewMode() const;

    /// 当前视口高（像素，与 panByPixels/zoomBy 位移入参同一单位口径）：供手势层按 参考实现
    /// 的 180·Δcy/视口高 换算俯仰灵敏度（手势位移与视口高同单位，免密度换算错配）。
    int viewportHeight() const;

    /// 手势平移（屏幕像素位移）。Phase D 由 JNI 暴露给 Kotlin 手势识别调用
    void panByPixels(double dxPx, double dyPx);

    /// 手势缩放（以屏幕焦点为锚，factor>1 放大）。Phase D 由 JNI 暴露给 Kotlin 手势识别调用
    void zoomBy(double factor, double focusXpx, double focusYpx);

    /// 手势旋转：相机 heading 累加增量（度，顺时针自北为正；正=heading 增大、内容逆时针转），仅 3D 通路消费。
    /// 由 Kotlin 手势识别调用（同 pan/zoom，重绘由手势侧 requestRender 驱动）
    void rotateHeading(double deltaDeg);

    /// 手势俯仰：相机 tilt 累加增量（度，正=向地平线方向倾视），钳制到 [0, MAX_TILT_DEG]，仅 3D 通路消费。
    /// 由 Kotlin 手势识别调用（同 pan/zoom/rotate，重绘由手势侧 requestRender 驱动）
    void rotateTilt(double deltaDeg);

    /// 添加一个瓦片图源图层（可多次调用叠加，按调用顺序绘制：底图在前、注记 overlay 在后）。
    /// cacheDir 为磁盘缓存目录（形如 /调查宝/tiles/<图源>）；urlTemplate 为联网 URL 模板
    /// （含 {x}/{y}/{z}/{rand=...} 占位符、Token 须已替换；空串则禁用联网只读缓存）；
    /// maxLevel 为图源数据最大级别（超出后拉伸末级瓦片）；overlay=true 为叠加层（注记，开 alpha 混合）。
    void addTileLayer(const std::string &cacheDir, const std::string &urlTemplate, int maxLevel, bool overlay);

    /// 设置指定图层的可见性（index 为 addTileLayer 的加入次序，0 起）。隐藏层不绘制、不取瓦片（对齐 参考实现 Layer.isEnabled）。
    /// index 越界则忽略；变更后若已注入重绘回调则触发一帧重绘（RENDERMODE_WHEN_DIRTY 下即时生效）。
    void setLayerVisible(int index, bool visible);

    /// 添加一个本地栅格图层（tif/img 等 GDAL 可打开格式）：native 用内建 GDAL 重投影，按全球墨卡托瓦片
    /// (z,x,y) 四至即时生成瓦片（仅磁盘缓存未命中时），生成后回写 [cacheDir] 复用。以 overlay 混合绘制在
    /// 底图之上、矢量层之下（范围外像素透明）；maxLevel 由栅格重投影分辨率自动推算（LOD 上限，避免过度拉伸）。
    /// 返回新图层在 layers_ 的索引（0 起，供 [setLayerVisible]）；打不开栅格或四至无效返回 -1。
    int addRasterLayer(const std::string &cacheDir, const std::string &path);

    /// 添加一个矢量图层（native 直接读文件：GDAL/OGR 读取 + 重投影 + earcut 三角剖分，异步加载）。
    /// 返回新图层的索引（0 起，供 [setVectorLayerVisible] 使用）。绘制在底图瓦片层之上、注记 overlay 层之下。
    /// [style] 为填充/描边/线/点颜色与线宽。与瓦片图层同口径：加入后不删除，只经可见性开关显隐。
    /// [iconRgba]/[iconW]/[iconH]：点要素图标位图（RGBA 像素 + 尺寸，由宿主解码矢量 drawable 后传入），
    /// 非空时点要素以该图标屏幕固定 billboard 渲染（中心锚点）；空则点回退画屏幕固定圆。
    /// [hasExtent]+[minLon,minLat,maxLon,maxLat]：初始屏幕过滤范围（大数据按视口加载）；false=整文件全量（小数据直显）。
    /// [maxFeatures]：单次加载要素预算（≤0 取硬上限）。
    int addVectorLayer(const std::string &path, const VectorStyle &style,
                       std::vector<uint8_t> iconRgba = {}, int iconW = 0, int iconH = 0,
                       bool hasExtent = false, double minLon = 0.0, double minLat = 0.0,
                       double maxLon = 0.0, double maxLat = 0.0, int maxFeatures = 0);

    /// 按新屏幕范围重载指定矢量层（index 为 [addVectorLayer] 次序）。转发到 VectorLoader::reload，
    /// 后台按新范围重读建新几何，就绪后由 GL 线程同帧替换旧几何（Swap-on-ready，无空窗）。
    /// index 越界、非文件矢量层（内存叠加层）或已墓碑则忽略；[hasExtent]=false 走整文件全量。
    /// 变更后若已注入重绘回调则触发一帧重绘。
    void updateVectorExtent(int index, bool hasExtent, double minLon, double minLat,
                            double maxLon, double maxLat, int maxFeatures);

    /// 设置指定矢量图层可见性（index 为 [addVectorLayer] 的加入次序，0 起）。越界忽略；变更后触发一帧重绘。
    void setVectorLayerVisible(int index, bool visible);

    /// 设置指定文件矢量层的级别可见性下限（对齐文档 LayerInfo.effectiveMinDisplayLevel，按显示级别取值）：
    /// 相机显示级别 < [minLevel] 时整层隐藏（不绘制/不上传/不可拾取/不计入加载提示），
    /// ≤0 为不限。内存叠加层不受影响（恒为不限）。越界忽略；变更后触发一帧重绘使即时生效。
    void setVectorMinLevel(int index, int minLevel);

    /// 当前相机显示级别（Navigator::displayLevel()，与 app 界面级别文本同一口径）：
    /// 供宿主在发出屏幕范围重载前判定级别是否达标，级别不足跳过无谓加载。可任意线程调用。
    int currentZoomLevel() const;

    /// 移除一个矢量图层（墓碑：置 dead + visible=false，不 erase 以保持其它矢量层 index 稳定）。
    /// 其 VBO/图标纹理/CPU 几何由 Renderer 在下一帧于 GL 线程回收。文件矢量层与叠加层均适用；
    /// pickVector/drawVectorLayers 因 visible=false 自动跳过。用于样式变更时就地换层（避免整界面 recreate）。
    void removeVectorLayer(int index);

    /// 当前矢量图层数量（供宿主做索引映射）。
    int vectorLayerCount() const;

    /// 文件矢量层是否仍有未完成的加载/上传（可见层后台读取与建几何中，或 chunk 正在流式上传中）。
    /// 纯状态查询、不触发重绘，可 UI 线程随时调用，供巨层首载「加载中」提示轮询显隐。
    /// 隐藏/级别不足层一切不计入（与绘制门控同口径，转「有效可见」后续传再计入，免隐藏层慢读取长亮提示）；
    /// 内存叠加层为小数据同步构建、不计入。
    bool hasVectorLoading() const;

    // ── 动态叠加层（运行时内存几何：测量/拍照标识/轨迹/样地等业务叠加）──
    // 叠加层与文件矢量层同列于 vectorLayers_、共享同一索引空间，复用同一套绘制/拾取/上下文重建通路；
    // 区别仅在数据源为宿主推送的 WGS84 几何（updateOverlay* → loader->setData 同步建几何 → drainReady 上传）。
    // 约定：每类叠加「加一次、就地更新多次」，仅整类拆除时 remove；勿逐要素 add/remove（以免图层数增长）。

    /// 新建一个空的动态叠加层，返回其索引（0 起，与文件矢量层共享索引空间，pickVector 亦返回该索引）。
    /// [style] 为叠加渲染样式；[iconRgba]/[iconW]/[iconH] 为点要素图标（可选，语义同 [addVectorLayer]）。
    /// 新建后为空，须经 updateOverlay* 推送几何方可见。变更后触发一帧重绘。
    int addOverlayLayer(const VectorStyle &style,
                        std::vector<uint8_t> iconRgba = {}, int iconW = 0, int iconH = 0);

    /// 更新叠加层的点要素（覆盖式）。lonlat 为 [lon0,lat0,lon1,lat1,...]；fids/labels 与点一一对应
    /// （fids 缺省则用序号；labels 缺省或空串表示该点不标注）。index 越界或非「存活的叠加层」则忽略。
    void updateOverlayPoints(int index, const std::vector<double> &lonlat,
                             const std::vector<long long> &fids,
                             const std::vector<std::string> &labels);

    /// 更新叠加层的线要素（覆盖式）。lonlat 为全部线顶点摊平；vertexCounts[f] 为第 f 条线的顶点数；
    /// fids/labels 与线一一对应。每条线作为单 part 折线渲染。数据不足则截断到可用顶点。
    void updateOverlayLines(int index, const std::vector<double> &lonlat,
                            const std::vector<int> &vertexCounts,
                            const std::vector<long long> &fids,
                            const std::vector<std::string> &labels);

    /// 更新叠加层的面要素（覆盖式）。lonlat 为全部环顶点摊平；ringVertexCounts 为每个环的顶点数
    /// （跨要素依次摊平）；ringsPerFeature[f] 为第 f 个面的环数（首环为外环、其余为洞）；fids/labels 与面一一对应。
    void updateOverlayPolygons(int index, const std::vector<double> &lonlat,
                               const std::vector<int> &ringVertexCounts,
                               const std::vector<int> &ringsPerFeature,
                               const std::vector<long long> &fids,
                               const std::vector<std::string> &labels);

    /// 移除叠加层（墓碑：置 dead + visible=false，不 erase 以保持其它叠加层 index 稳定）。其 VBO/图标纹理/
    /// CPU 几何由 Renderer 在下一帧于 GL 线程回收。仅对叠加层生效（文件矢量层请用 [setVectorLayerVisible]）。
    void removeOverlayLayer(int index);

    /// 设置叠加层「不参与拾取」标志（仅对叠加层生效，文件矢量层忽略）：noPick=true 时层仍正常绘制，
    /// 但 pickVector/pickVector3D 整层跳过。选中高亮/查询高亮等瞬态视觉层经此退出拾取竞争，
    /// 命中直接落到源矢量层/业务叠加层，宿主无需再做命中穿透分流。越界/非叠加层忽略。
    void setOverlayNoPick(int index, bool noPick);

    /// 移除全部叠加层（墓碑所有 isOverlay 层）。文件矢量层不受影响。变更后触发一帧重绘。
    void clearOverlayLayers();

    /// 拾取矢量要素：命中检测分两阶段——阶段 1 仅动态叠加层（测量/轨迹/拍照/样地等业务叠加）、
    /// 阶段 2 仅文件矢量层，各阶段内均按索引逆序（后加入优先）；叠加层无论何时加入（含相机静止后
    /// 补加入的大数据矢量层晚于叠加层的情形）恒优先于文件矢量层命中。noPick 瞬态层（选中/查询高亮）跳过。
    /// 同层内点/线优先于面，2D 为屏幕点反算世界坐标的平面 RTC 判据，3D 为屏幕空间投影判据
    /// （见 [pickVector3D]，与贴地渲染同口径）。命中时写出 [outLayerIndex]（addVectorLayer 次序）与
    /// [outFid]（源数据 FID）并返回 true；未命中返回 false。
    /// 仅读 CPU 几何（pickPrims），不涉及 GL；供宿主单击回调把点击转成要素属性详情。
    bool pickVector(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const;

    /// 屏幕点 → 地理经纬度（WGS84 度）：先经 [Navigator] 反算世界坐标，再由墨卡托反投影为经纬度。
    /// 视口未就绪（宽高 <=0）返回 false。供采集交互（测量/轨迹）把地图单击转成加点坐标。
    bool screenToGeo(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const;

    /// 取指定矢量层某 FID 要素的经纬度几何（WGS84 度），供宿主点击选中后画高亮叠加层（对齐主界面选中态高亮）。
    /// 命中返回 true 并写出：[outType]（0=点,1=线,2=面）、[outLonLat]（摊平 [lon,lat,...]）、
    /// [outRingCounts]（线为每段顶点数、面为每环顶点数，点为空）、[outRingsPerFeature]（面专用：本要素环数，点/线为空）。
    /// 几何取自拾取图元 pickPrims（RTC 世界坐标）经墨卡托反投影，无文件 IO；layerIndex 越界或 fid 未命中返回 false。
    bool featureGeometry(int layerIndex, long long fid, int &outType,
                         std::vector<double> &outLonLat,
                         std::vector<int> &outRingCounts,
                         std::vector<int> &outRingsPerFeature) const;

    /// 取指定矢量层某 FID 要素的当前实际渲染色（#AARRGGBB 打包无符号整数），供宿主点击要素弹层
    /// 色块「所见即所得」回显——尤其 KML 逐要素原色（app 无从复现，仅 native 建几何时按三级优先级算定）。
    /// 取自拾取图元 pickPrims 的 renderFill/renderLine（与 VBO 顶点色一致），无文件 IO。
    /// 命中写出 [outFillArgb]/[outLineArgb] 并返回 true；layerIndex 越界、几何未就绪或 fid 未命中返回 false。
    bool featureRenderColor(int layerIndex, long long fid, unsigned int &outFillArgb,
                            unsigned int &outLineArgb) const;

    /// 设置定位标记的地理坐标、可见性与移动方位角（转发到 [Renderer]，在所有瓦片层之上以屏幕固定尺寸绘制）。
    /// [headingDeg] 顺时针自北 0..360，传入负值则不画方向箭头。变更后若已注入重绘回调则触发一帧重绘。
    void setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg);

    /// 设置定位标记罗盘图标（RGBA8888 像素 + 尺寸），转发到 [Renderer]。
    /// 有图标时 drawLocationMarker 画纹理四边形替代蓝点（对齐原主界面 ic_compass 方式）。
    void setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h);

    /// 设置矢量标注字体文件路径并加载字形图集（[path] 为空则 native 自动探测系统 CJK 字体）。
    /// 转发到 [Renderer]（FontAtlas::load 幂等、纯 CPU、线程安全）；宜在 UI 线程调用以避开 GL 线程文件 IO 卡顿。
    void setFontPath(const std::string &path);

    /// 设置屏幕密度（对齐参考实现的 setupViewport(w,h,displayMetrics.density) 口径），
    /// 作为 LOD 细分判据的 densityFactor 传入 [Navigator]。由 Kotlin 侧 displayMetrics.density 传入。
    void setDisplayDensity(double density);

    /// 设置「需重绘」回调（由 JNI 层注入，内部调用 Kotlin GLSurfaceView.requestRender）。
    /// RENDERMODE_WHEN_DIRTY 下，native 在瓦片异步加载/解码完成、需再画一帧时回调它触发重绘
    /// （对齐 参考实现 资源到位触发重绘）。回调可能在 GL 线程被调用，实现须线程安全（requestRender 是）。
    void setRenderCallback(std::function<void()> cb);

    /// 释放 GL 资源（应在 GL 上下文销毁前调用）
    void releaseGl();

private:
    Navigator navigator_;
    // 瓦片图源图层（按加入顺序绘制）；以 unique_ptr 持有保证地址稳定，供 Renderer 长期引用
    std::vector<std::unique_ptr<TileLayer>> layers_;
    // 矢量图层（native 直接读 OGR）；以 unique_ptr 持有保证地址稳定，供 Renderer 长期引用。
    // 须声明在 renderer_ 之前：Renderer 构造时引用本向量。
    std::vector<std::unique_ptr<VectorLayer>> vectorLayers_;
    Renderer renderer_;     // 依赖 navigator_、layers_、vectorLayers_，须在其后声明以保证构造顺序
    std::function<void()> renderCallback_; // 「需重绘」回调（由 JNI 注入，指向 requestRender）

    /// 3D 矢量拾取实体（pickVector 的 MODE_3D 分支）：屏幕空间投影命中检测。与 2D 同口径分两阶段
    /// （先叠加层后文件矢量层，noPick 跳过）。逐图元把 RTC 世界坐标
    /// 经局部切平面（原点 ECEF + 东/北基，免逐点三角函数）→ ECEF → RTC viewProj 投影到屏幕像素，
    /// 复用与 2D 同口径的像素容差（点半径+tapSlop、线半宽下限 2px+tapSlop）与点/线优先于面判据；
    /// 逐图元投影四至快速排除。跨层命中以「离眼点最近者」胜出（背半球穿地图元以射线交点邻域近侧优先）。
    bool pickVector3D(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const;
};

} // namespace globecore

#endif // GLOBECORE_CORE_GlobeEngine_H
