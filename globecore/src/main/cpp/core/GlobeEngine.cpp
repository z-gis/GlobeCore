#include "core/GlobeEngine.h"

#include <algorithm>
#include <cmath>

#include "globe/MercatorProjection.h"
#include "globe/Wgs84Globe.h"
#include "net/HttpClient.h"
#include "raster/RasterRenderer.h"
#include "util/Log.h"
#include "vector/GdalBootstrap.h"
#include "vector/VectorReader.h"

namespace globecore {

GlobeEngine::GlobeEngine() : renderer_(navigator_, layers_, vectorLayers_) {
    // 图层由 app 经 addTileLayer 逐个加入（影像底图 + 注记叠加）；相机/图源装配见 MainActivity。
    // 初始化 libcurl（幂等）：链接期强制拉入 curl 符号验证可用性，运行期从 logcat 打印版本自检。
    HttpClient::globalInit();
    // GDAL/OGR 全进程一次引导（幂等）：注册全部矢量驱动并打印版本自检，同时作为链接期强制
    // 拉入 GDAL 符号的锚点——若依赖链未正确链接，加载 .so 时即暴露未定义符号而非等到首次读矢量。
    ensureGdalRegistered();
    LOGI("GlobeEngine created");
}

GlobeEngine::~GlobeEngine() {
    LOGI("GlobeEngine destroyed");
}

void GlobeEngine::surfaceCreated() {
    renderer_.onSurfaceCreated();
}

void GlobeEngine::surfaceChanged(int width, int height) {
    navigator_.setViewport(width, height);
    renderer_.onSurfaceChanged(width, height);
}

void GlobeEngine::drawFrame() {
    // Renderer 返回「是否需再画一帧」（后台仍有瓦片在途/待上传）；省电模式下据此自请求下一帧，
    // 使异步到位的瓦片无需等待手势即可增量补齐；全部就绪后不再回调 → GL 线程停绘省电。
    if (renderer_.onDrawFrame() && renderCallback_) renderCallback_();
}

void GlobeEngine::setCamera(const Camera &camera) {
    navigator_.setCamera(camera);
}

Camera GlobeEngine::getCamera() const {
    return navigator_.camera();
}

void GlobeEngine::setViewMode(int mode) {
    navigator_.setViewMode(mode == 1 ? Navigator::ViewMode::MODE_3D : Navigator::ViewMode::MODE_2D);
    // 省电模式（WHEN_DIRTY）下主动请求一帧，使切换即时生效（2D/3D 通路在 Renderer::onDrawFrame 内分发）
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::viewMode() const {
    return navigator_.viewMode() == Navigator::ViewMode::MODE_3D ? 1 : 0;
}

int GlobeEngine::viewportHeight() const {
    return navigator_.viewportHeight();
}

void GlobeEngine::panByPixels(double dxPx, double dyPx) {
    navigator_.panByPixels(dxPx, dyPx);
}

void GlobeEngine::zoomBy(double factor, double focusXpx, double focusYpx) {
    navigator_.zoomBy(factor, focusXpx, focusYpx);
}

void GlobeEngine::rotateHeading(double deltaDeg) {
    navigator_.rotateHeading(deltaDeg);
}

void GlobeEngine::rotateTilt(double deltaDeg) {
    navigator_.rotateTilt(deltaDeg);
}

void GlobeEngine::addTileLayer(const std::string &cacheDir, const std::string &urlTemplate, int maxLevel, bool overlay) {
    // 每层自带 TileCache + TileLoader（独立线程池/URL 模板），按加入顺序绘制（底图在前、注记 overlay 在后）。
    layers_.push_back(std::make_unique<TileLayer>(cacheDir, urlTemplate, maxLevel, overlay));
    LOGI("GlobeEngine addTileLayer maxLevel=%d overlay=%d net=%s", maxLevel, overlay ? 1 : 0,
         urlTemplate.empty() ? "off" : "on");
}

void GlobeEngine::setLayerVisible(int index, bool visible) {
    if (index < 0 || static_cast<size_t>(index) >= layers_.size()) return;
    layers_[static_cast<size_t>(index)]->visible = visible;
    LOGI("GlobeEngine setLayerVisible index=%d visible=%d", index, visible ? 1 : 0);
    // 只改了可见性标志（不涉及 GL），省电模式下主动请求一帧使变更立即生效。
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::addRasterLayer(const std::string &cacheDir, const std::string &path) {
    // 先读四至 + 据重投影分辨率推算 maxLevel（LOD 上限）：会打开数据集并建重投影 VRT（一次性，入 RasterRenderer
    // 的 LRU 缓存，后续瓦片生成复用）；打不开或四至无效则不建层。与原主界面 GdalRasterLayer（已删除）构造同口径（同步读四至）。
    double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
    int maxLevel = 18;
    if (!RasterRenderer::info(path, minLon, minLat, maxLon, maxLat, maxLevel)) {
        LOGW("GlobeEngine addRasterLayer 无法解析栅格，跳过 path=%s", path.c_str());
        return -1;
    }
    // 栅格层：overlay=true（alpha 混合、缺失瓦片透明）+ raster=true（Renderer 据此后置于矢量之前绘制）；
    // urlTemplate 空（不联网），瓦片由 TileLoader 调 RasterRenderer 按四至即时生成。numThreads=2（GDAL 全局串行，无需多绪）。
    auto layer = std::make_unique<TileLayer>(cacheDir, "", maxLevel, /*overlay=*/true, /*numThreads=*/2);
    layer->raster = true;
    layer->rasterPath = path;
    layer->loader->setRasterSource(path);
    layers_.push_back(std::move(layer));
    const int index = static_cast<int>(layers_.size()) - 1;
    LOGI("GlobeEngine addRasterLayer index=%d maxLevel=%d extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
         index, maxLevel, minLon, minLat, maxLon, maxLat, path.c_str());
    if (renderCallback_) renderCallback_();
    return index;
}

int GlobeEngine::addVectorLayer(const std::string &path, const VectorStyle &style,
                                std::vector<uint8_t> iconRgba, int iconW, int iconH,
                                bool hasExtent, double minLon, double minLat,
                                double maxLon, double maxLat, int maxFeatures) {
    // 矢量层与瓦片层同口径：以 unique_ptr 持有保证地址稳定，加入后不删除只经可见性开关显隐。
    auto layer = std::make_unique<VectorLayer>(path, style, hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
    // 点要素图标（可选）：在加入前设定、之后只读，GL 线程懒上传纹理（避开 UI/GL 竞争）。
    if (!iconRgba.empty() && iconW > 0 && iconH > 0) {
        layer->iconRgba = std::move(iconRgba);
        layer->iconW = iconW;
        layer->iconH = iconH;
    }
    layer->loader->start(); // 后台线程读文件 + 重投影 + earcut 三角剖分，就绪后由 GL 线程上传
    vectorLayers_.push_back(std::move(layer));
    const int index = static_cast<int>(vectorLayers_.size()) - 1;
    LOGI("GlobeEngine addVectorLayer index=%d hasExtent=%d extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
         index, hasExtent ? 1 : 0, minLon, minLat, maxLon, maxLat, path.c_str());
    // 新层异步加载中，主动请求一帧启动绘制（加载就绪后 drawVectorLayers 会持续请求重绘直到上传）。
    if (renderCallback_) renderCallback_();
    return index;
}

void GlobeEngine::updateVectorExtent(int index, bool hasExtent, double minLon, double minLat,
                                     double maxLon, double maxLat, int maxFeatures) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    // 仅对存活的文件矢量层重载（内存叠加层 / 已墓碑层忽略）。
    if (vl.isOverlay || vl.dead || !vl.loader) return;
    LOGI("[VecReload] GlobeEngine updateVectorExtent index=%d hasExtent=%d extent=[%.6f,%.6f,%.6f,%.6f]",
         index, hasExtent ? 1 : 0, minLon, minLat, maxLon, maxLat);
    vl.loader->reload(hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
    // 后台重载完成后 drainReady 会驱动重绘；此处先请求一帧使 hasPendingOrReady 生效持续重绘。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setVectorLayerVisible(int index, bool visible) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    vectorLayers_[static_cast<size_t>(index)]->visible = visible;
    LOGI("GlobeEngine setVectorLayerVisible index=%d visible=%d", index, visible ? 1 : 0);
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setVectorMinLevel(int index, int minLevel) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (vl.isOverlay) return; // 内存叠加层不受级别门控（恒为不限）
    if (vl.minDisplayLevel == minLevel) return;
    vl.minDisplayLevel = minLevel;
    LOGI("GlobeEngine setVectorMinLevel index=%d minLevel=%d", index, minLevel);
    // 级别下限变化可能使层显/隐翻转，请求一帧使变更即时生效（与 setVectorLayerVisible 同口径）。
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::currentZoomLevel() const {
    // 显示级别（与 app 界面同源口径）：用户按界面级别设定的 effectiveMinDisplayLevel 经此比较/门控
    return navigator_.displayLevel();
}

void GlobeEngine::removeVectorLayer(int index) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    // 墓碑：置 dead + visible=false，不 erase 以保持其它矢量层 index 稳定（与 removeOverlayLayer 同口径）。
    // 其 VBO/图标纹理/CPU 几何由 Renderer 在下一帧于 GL 线程回收；后台加载线程若仍在读取，
    // VectorLoader 随 VectorLayer unique_ptr 释放（当 Renderer 不再引用后由本函数下一轮清理时自然析构）。
    vl.dead = true;
    vl.visible = false;
    LOGI("GlobeEngine removeVectorLayer index=%d", index);
    if (renderCallback_) renderCallback_();
}

int GlobeEngine::vectorLayerCount() const {
    return static_cast<int>(vectorLayers_.size());
}

bool GlobeEngine::hasVectorLoading() const {
    // 后台读/建几何阶段：loader 处于 Loading 或 Ready（已就绪待 GL 线程 drain），逐层查询（自身持锁、线程安全）。
    const int camLevel = navigator_.displayLevel();
    for (const auto &vl : vectorLayers_) {
        if (vl->dead || vl->isOverlay || !vl->loader) continue;
        // 隐藏/级别不足的层一切不计入（与绘制门控同口径）：其后台读取/上传挂起中，
        // 转「有效可见」后续传再计入——否则整层隐藏的慢读取会把加载提示胶囊长亮误弹（真实用户可见层并无进度）。
        if (!vectorLayerShown(*vl, camLevel)) continue;
        if (vl->loader->hasPendingOrReady()) return true;
        // drainReady 接管后转入逐帧预算的分片流式上传（巨层可达数秒）：streamingUpload 标志接续这段，
        // 提示直到全部 chunk 上传完成、新数据真正画出来才收。
        if (vl->streamingUpload.load()) return true;
    }
    return false;
}

namespace {

/// 由已填入 features 的叠加结果计算 WGS84 四至写入 r（供图层原点 RTC 计算）。
/// 无顶点时 hasBBox=false，buildGeometry 会退化到首要素点作为稳定原点。
inline void computeOverlayBBox(VectorReadResult &r) {
    double minLon = 180.0, minLat = 90.0, maxLon = -180.0, maxLat = -90.0;
    bool any = false;
    auto acc = [&](double lon, double lat) {
        if (lon < minLon) minLon = lon;
        if (lat < minLat) minLat = lat;
        if (lon > maxLon) maxLon = lon;
        if (lat > maxLat) maxLat = lat;
        any = true;
    };
    for (const auto &f : r.features) {
        if (f.type == VectorGeomType::Point) acc(f.lon, f.lat);
        for (const auto &p : f.parts)
            for (size_t i = 0; i + 1 < p.size(); i += 2) acc(p[i], p[i + 1]);
        for (size_t i = 0; i + 1 < f.outer.size(); i += 2) acc(f.outer[i], f.outer[i + 1]);
        for (const auto &h : f.holes)
            for (size_t i = 0; i + 1 < h.size(); i += 2) acc(h[i], h[i + 1]);
    }
    r.hasBBox = any;
    if (any) { r.minLon = minLon; r.minLat = minLat; r.maxLon = maxLon; r.maxLat = maxLat; }
}

} // namespace

int GlobeEngine::addOverlayLayer(const VectorStyle &style,
                                 std::vector<uint8_t> iconRgba, int iconW, int iconH) {
    // 叠加层与文件矢量层同口径：以 unique_ptr 持有保证地址稳定（Renderer 长期引用），加入后不 erase。
    auto layer = std::make_unique<VectorLayer>(style); // 内存模式 loader（不读文件、不起后台线程）
    if (!iconRgba.empty() && iconW > 0 && iconH > 0) {
        layer->iconRgba = std::move(iconRgba);
        layer->iconW = iconW;
        layer->iconH = iconH;
    }
    vectorLayers_.push_back(std::move(layer));
    const int index = static_cast<int>(vectorLayers_.size()) - 1;
    LOGI("GlobeEngine addOverlayLayer index=%d", index);
    if (renderCallback_) renderCallback_();
    return index;
}

void GlobeEngine::updateOverlayPoints(int index, const std::vector<double> &lonlat,
                                      const std::vector<long long> &fids,
                                      const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || vl.dead || !vl.loader) return;

    VectorReadResult r;
    r.ok = true;
    const size_t n = lonlat.size() / 2;
    r.features.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        VectorFeatureData f;
        f.type = VectorGeomType::Point;
        f.lon = lonlat[i * 2];
        f.lat = lonlat[i * 2 + 1];
        f.fid = (i < fids.size()) ? fids[i] : static_cast<long long>(i);
        if (i < labels.size()) f.label = labels[i];
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r)); // 调用线程同步建几何 → Ready；GL 线程下一帧 drainReady 上传
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::updateOverlayLines(int index, const std::vector<double> &lonlat,
                                     const std::vector<int> &vertexCounts,
                                     const std::vector<long long> &fids,
                                     const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || vl.dead || !vl.loader) return;

    VectorReadResult r;
    r.ok = true;
    const size_t totalPairs = lonlat.size() / 2;
    size_t cursor = 0; // lonlat 坐标对游标
    for (size_t fi = 0; fi < vertexCounts.size(); ++fi) {
        const int vc = vertexCounts[fi];
        if (vc < 1) continue;
        if (cursor + static_cast<size_t>(vc) > totalPairs) break; // 数据不足，截断
        VectorFeatureData f;
        f.type = VectorGeomType::Line;
        f.fid = (fi < fids.size()) ? fids[fi] : static_cast<long long>(fi);
        if (fi < labels.size()) f.label = labels[fi];
        std::vector<double> part;
        part.reserve(static_cast<size_t>(vc) * 2);
        for (int k = 0; k < vc; ++k) {
            part.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2]);
            part.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2 + 1]);
        }
        cursor += static_cast<size_t>(vc);
        f.parts.push_back(std::move(part));
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r));
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::updateOverlayPolygons(int index, const std::vector<double> &lonlat,
                                        const std::vector<int> &ringVertexCounts,
                                        const std::vector<int> &ringsPerFeature,
                                        const std::vector<long long> &fids,
                                        const std::vector<std::string> &labels) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay || vl.dead || !vl.loader) return;

    VectorReadResult r;
    r.ok = true;
    const size_t totalPairs = lonlat.size() / 2;
    size_t cursor = 0;   // lonlat 坐标对游标
    size_t ringIdx = 0;  // ringVertexCounts 游标
    auto readRing = [&](std::vector<double> &out) -> bool {
        if (ringIdx >= ringVertexCounts.size()) return false;
        const int vc = ringVertexCounts[ringIdx++];
        if (vc < 1 || cursor + static_cast<size_t>(vc) > totalPairs) return false;
        out.reserve(static_cast<size_t>(vc) * 2);
        for (int k = 0; k < vc; ++k) {
            out.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2]);
            out.push_back(lonlat[(cursor + static_cast<size_t>(k)) * 2 + 1]);
        }
        cursor += static_cast<size_t>(vc);
        return true;
    };
    for (size_t fi = 0; fi < ringsPerFeature.size(); ++fi) {
        const int ringCount = ringsPerFeature[fi];
        if (ringCount < 1) continue;
        VectorFeatureData f;
        f.type = VectorGeomType::Polygon;
        f.fid = (fi < fids.size()) ? fids[fi] : static_cast<long long>(fi);
        if (fi < labels.size()) f.label = labels[fi];
        if (!readRing(f.outer)) break; // 外环读取失败（数据耗尽）
        for (int hi = 1; hi < ringCount; ++hi) {
            std::vector<double> hole;
            if (!readRing(hole)) break;
            f.holes.push_back(std::move(hole));
        }
        r.features.push_back(std::move(f));
    }
    computeOverlayBBox(r);
    vl.loader->setData(std::move(r));
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::removeOverlayLayer(int index) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay) return; // 仅对叠加层生效；文件矢量层用 setVectorLayerVisible 显隐
    vl.dead = true;
    vl.visible = false;
    LOGI("GlobeEngine removeOverlayLayer index=%d", index);
    if (renderCallback_) renderCallback_(); // 触发一帧，让 GL 线程回收其 VBO/纹理/几何
}

void GlobeEngine::setOverlayNoPick(int index, bool noPick) {
    if (index < 0 || static_cast<size_t>(index) >= vectorLayers_.size()) return;
    VectorLayer &vl = *vectorLayers_[static_cast<size_t>(index)];
    if (!vl.isOverlay) return; // 仅对叠加层生效；文件矢量层恒可拾取
    vl.noPick = noPick;
}

void GlobeEngine::clearOverlayLayers() {
    for (auto &vlp : vectorLayers_) {
        if (vlp->isOverlay) { vlp->dead = true; vlp->visible = false; }
    }
    LOGI("GlobeEngine clearOverlayLayers");
    if (renderCallback_) renderCallback_();
}

namespace {

/// 点到线段最短距离的平方（相对世界坐标平面）
inline double segDistSq(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = (len2 > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    const double cx = ax + t * dx, cy = ay + t * dy;
    const double ex = px - cx, ey = py - cy;
    return ex * ex + ey * ey;
}

/// 射线法判定点是否在环内（coords 为相对坐标点对序列，取 [start,start+count) 点对）
inline bool pointInRing(double px, double py, const std::vector<float> &coords, int start, int count) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const double xi = coords[(start + i) * 2], yi = coords[(start + i) * 2 + 1];
        const double xj = coords[(start + j) * 2], yj = coords[(start + j) * 2 + 1];
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi + 1e-30) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

/// 点击拾取容差策略（像素）：2D/3D 共用单一来源，避免一处调整另一处漂移
/// （本轮 3D 拾取多处 bug 即因复制 2D 后各自演化所致）。返回均为「像素」容差：
/// 2D 再按 worldPerPx 折算到世界坐标，3D 直接在屏幕像素空间判定。
constexpr double kTapSlopPx = 8.0; // 固定点击余量（像素），保证细小要素也可点中
inline double pickPointTolPx(double pointRadiusDp, double density) {
    return pointRadiusDp * density + kTapSlopPx;
}
inline double pickLineTolPx(double lineWidth) {
    return std::max(lineWidth * 0.5, 2.0) + kTapSlopPx;
}

} // namespace

bool GlobeEngine::screenToGeo(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const {
    if (navigator_.viewportWidth() <= 0 || navigator_.viewportHeight() <= 0) return false;
    // 3D：屏幕射线∩椭球反算（指向球外空间时无命中返回 false，不同于 2D 平面恒可反算）
    if (navigator_.viewMode() == Navigator::ViewMode::MODE_3D) {
        return navigator_.screenToGeo3D(sxPx, syPx, outLonDeg, outLatDeg);
    }
    double wx = 0.0, wy = 0.0;
    navigator_.screenToWorld(sxPx, syPx, wx, wy);
    double lon = 0.0, lat = 0.0;
    MercatorProjection::worldToLonLat(wx, wy, lon, lat);
    // 经度环绕到 [-180, 180)、纬度夹取到墨卡托有效范围（与 Navigator::setCenterWorld 同口径）
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    outLonDeg = lon;
    outLatDeg = MercatorProjection::clampLatitude(lat);
    return true;
}

bool GlobeEngine::featureGeometry(int layerIndex, long long fid, int &outType,
                                  std::vector<double> &outLonLat,
                                  std::vector<int> &outRingCounts,
                                  std::vector<int> &outRingsPerFeature) const {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(vectorLayers_.size())) return false;
    const VectorGeometry &g = vectorLayers_[static_cast<size_t>(layerIndex)]->geom;
    // 定位命中要素的拾取图元（pickVector 命中回传的 fid 即来自此）
    const PickPrimitive *found = nullptr;
    for (const auto &pp : g.pickPrims) {
        if (pp.fid == fid) { found = &pp; break; }
    }
    if (found == nullptr) return false;

    // pickPrims 坐标为「相对图层原点」的 RTC 世界坐标：加回原点得绝对世界坐标，再墨卡托反投影为经纬度
    // （与 screenToGeo 同口径：经度环绕到 [-180,180)、纬度夹取到墨卡托有效范围）。
    const double ox = g.originWx, oy = g.originWy;
    outLonLat.clear();
    outRingCounts.clear();
    outRingsPerFeature.clear();
    const std::vector<float> &c = found->coords;
    auto pushLonLat = [&](float rx, float ry) {
        double lon = 0.0, lat = 0.0;
        MercatorProjection::worldToLonLat(ox + rx, oy + ry, lon, lat);
        while (lon < -180.0) lon += 360.0;
        while (lon >= 180.0) lon -= 360.0;
        outLonLat.push_back(lon);
        outLonLat.push_back(MercatorProjection::clampLatitude(lat));
    };
    // 逐环/段摊平：ranges 为空（点）时直接取 coords 首点对；否则按 (起始点对, 点对数) 区间遍历
    auto pushRanges = [&]() {
        int rings = 0;
        for (const auto &rg : found->ranges) {
            const int start = rg.first, count = rg.second;
            for (int i = 0; i < count; ++i) pushLonLat(c[(start + i) * 2], c[(start + i) * 2 + 1]);
            outRingCounts.push_back(count);
            ++rings;
        }
        return rings;
    };

    if (found->type == PickType::Point) {
        if (c.size() < 2) return false;
        outType = 0;
        pushLonLat(c[0], c[1]);
    } else if (found->type == PickType::Line) {
        outType = 1;
        pushRanges();  // 每段一条折线，outRingCounts 记各段顶点数
    } else {           // Polygon：ranges[0] 外环、其余洞环
        outType = 2;
        outRingsPerFeature.push_back(pushRanges());
    }
    return true;
}

bool GlobeEngine::pickVector(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const {
    // 3D：矢量已贴地绘制（P2），命中检测改走屏幕空间投影通路（pickVector3D，与渲染同口径）；
    // 2D 正交下的射线∩椭球反算不适用于平面 RTC 判据，不可复用。
    if (navigator_.viewMode() == Navigator::ViewMode::MODE_3D)
        return pickVector3D(sxPx, syPx, outLayerIndex, outFid);
    // 屏幕点 → 世界坐标（与相机同口径），再转各图层「相对原点」坐标做命中检测
    double wx = 0.0, wy = 0.0;
    navigator_.screenToWorld(sxPx, syPx, wx, wy);

    const int vw = navigator_.viewportWidth();
    const double worldPerPx = (vw > 0) ? (2.0 * navigator_.halfWorldWidth()) / vw : 0.0;
    if (worldPerPx <= 0.0) return false;
    const double density = navigator_.displayDensity();

    // 两阶段判据：pass 0 仅扫动态叠加层（测量/轨迹/拍照/样地），pass 1 仅扫文件矢量层；
    // 各阶段内索引逆序（后加入优先）。叠加层无论何时加入均优先于文件矢量层命中——旧单趟逆序
    // 在相机静止后补加入大数据矢量层（晚于叠加层）时会把矢量层排到叠加层之上，遮蔽测量/拍照点击。
    // 层内点/线优先于面（小目标优先）；noPick 瞬态层（选中/查询高亮）不参与拾取。
    const int camLevel = navigator_.displayLevel();
    for (int pass = 0; pass < 2; ++pass)
    for (int li = static_cast<int>(vectorLayers_.size()) - 1; li >= 0; --li) {
        const VectorLayer &vl = *vectorLayers_[static_cast<size_t>(li)];
        if ((pass == 0) != vl.isOverlay) continue; // 本趟只扫本类别层：pass 0 叠加层、pass 1 文件矢量层
        // 级别不足/隐藏/墓碑层不可拾取（与绘制同进同退，免点到看不见的图斑）；noPick 瞬态层整层跳过
        if (!vectorLayerShown(vl, camLevel) || vl.noPick) continue;
        const VectorGeometry &g = vl.geom;
        if (g.pickPrims.empty()) continue;

        const double relX = wx - g.originWx;
        const double relY = wy - g.originWy;
        const double pointRadWorld = pickPointTolPx(vl.style.pointRadiusDp, density) * worldPerPx;
        const double lineTolWorld = pickLineTolPx(vl.style.lineWidth) * worldPerPx;

        long long pointLineFid = -1;
        long long polygonFid = -1;
        for (int pi = static_cast<int>(g.pickPrims.size()) - 1; pi >= 0; --pi) {
            const PickPrimitive &pp = g.pickPrims[static_cast<size_t>(pi)];
            if (pp.type == PickType::Point) {
                if (pointLineFid >= 0 || pp.coords.size() < 2) continue;
                const double dx = pp.coords[0] - relX, dy = pp.coords[1] - relY;
                if (dx * dx + dy * dy <= pointRadWorld * pointRadWorld) pointLineFid = pp.fid;
            } else if (pp.type == PickType::Line) {
                if (pointLineFid >= 0) continue;
                const double tol2 = lineTolWorld * lineTolWorld;
                for (const auto &rg : pp.ranges) {
                    const int start = rg.first, count = rg.second;
                    for (int i = 0; i + 1 < count; ++i) {
                        const double ax = pp.coords[(start + i) * 2], ay = pp.coords[(start + i) * 2 + 1];
                        const double bx = pp.coords[(start + i + 1) * 2], by = pp.coords[(start + i + 1) * 2 + 1];
                        if (segDistSq(relX, relY, ax, ay, bx, by) <= tol2) { pointLineFid = pp.fid; break; }
                    }
                    if (pointLineFid >= 0) break;
                }
            } else { // Polygon
                if (polygonFid >= 0 || pp.ranges.empty()) continue;
                const auto outer = pp.ranges[0];
                if (!pointInRing(relX, relY, pp.coords, outer.first, outer.second)) continue;
                bool inHole = false;
                for (size_t hi = 1; hi < pp.ranges.size(); ++hi) {
                    const auto hole = pp.ranges[hi];
                    if (pointInRing(relX, relY, pp.coords, hole.first, hole.second)) { inHole = true; break; }
                }
                if (!inHole) polygonFid = pp.fid;
            }
        }
        const long long hit = (pointLineFid >= 0) ? pointLineFid : polygonFid;
        if (hit >= 0) {
            outLayerIndex = li;
            outFid = hit;
            return true;
        }
    }
    return false;
}

bool GlobeEngine::pickVector3D(double sxPx, double syPx, int &outLayerIndex, long long &outFid) const {
    // 1) 眼点过屏幕点击点的射线 ∩ 椭球：未命中（点击指向天空/屏外）不可拾取
    const Navigator::Camera3D cam = navigator_.camera3D();
    if (!cam.valid) return false;
    double tapLon = 0.0, tapLat = 0.0;
    if (!navigator_.screenToGeo3D(sxPx, syPx, tapLon, tapLat)) return false;
    // tapLon/tapLat 仅作 screenToGeo3D 命中守卫（返回 false 即指向天空不可拾取）；后续命中判据
    // 用绝对 ECEF，无需点击点坐标，故不再换算 tapEcef。
    const Wgs84Globe &globe = Wgs84Globe::instance();
    (void) tapLon; (void) tapLat;

    // 2) RTC 投影矩阵（与 Renderer::projectEcef3D 同口径：列主序，顶点 double 减眼后转 float）
    const Matrix4 viewRtc = Matrix4::lookAt(Vec3{0, 0, 0}, cam.center - cam.eye, cam.up);
    const Matrix4 viewProjRtc = Matrix4::multiply(cam.proj, viewRtc);
    const float *m = viewProjRtc.data();
    const double vw = static_cast<double>(navigator_.viewportWidth());
    const double vh = static_cast<double>(navigator_.viewportHeight());
    if (vw <= 0.0 || vh <= 0.0) return false;
    const double density = navigator_.displayDensity();

    // 与渲染端 renderGlobeVectors3D 同口径的贴地抬升（米）：矢量绘制时 shader 沿地心法向抬 uVecAlt
    //（按弦切矢高口径 camAlt²/2e7 钳 [2,500]，见 Renderer kGlobeVecAltMeters），拾取须同样抬升方能与屏幕绘制位置对齐；
    // 否则斜视下命中点系统性偏离。两式须逐字同步（改其一必改另一）。
    const double camAlt = (cam.eye - cam.center).length();
    double vecAlt = camAlt * camAlt / 2.0e7;
    if (vecAlt > 500.0) vecAlt = 500.0; // 同步 Renderer kGlobeVecAltMeters
    if (vecAlt < 2.0) vecAlt = 2.0;

    // 屏幕点击点是否落在投影四至内（含容差 r 像素；屏外顶点仍参与投影计算，与渲染视口裁剪同效）
    auto boxHit = [&](double bx0, double by0, double bx1, double by1, double r) {
        return bx0 - r <= sxPx && sxPx <= bx1 + r && by0 - r <= syPx && syPx <= by1 + r;
    };

    // 候选集：(层类别优先序 0=叠加层 1=文件矢量层, 类型优先序 0=点线 1=面, 眼距, layerIndex, fid)；
    // 与 2D 同口径两阶段优先（叠加层恒先于文件矢量层，见 pickVector 注释），但 3D 跨层不一经命中
    // 即停（斜视下背半球图元可能先遍历到），全量收集后按 (层类别, 类型, 眼距) 三级择优。
    struct Cand { int pass; int kind; double dist; int li; long long fid; };
    std::vector<Cand> cands;

    const int camLevel = navigator_.displayLevel();
    for (int pass = 0; pass < 2; ++pass)
    for (int li = static_cast<int>(vectorLayers_.size()) - 1; li >= 0; --li) {
        const VectorLayer &vl = *vectorLayers_[static_cast<size_t>(li)];
        if ((pass == 0) != vl.isOverlay) continue; // 本趟只扫本类别层：pass 0 叠加层、pass 1 文件矢量层
        // 与绘制同进同退（含隐藏/级别不足/墓碑）；noPick 瞬态层整层跳过
        if (!vectorLayerShown(vl, camLevel) || vl.noPick) continue;
        const VectorGeometry &g = vl.geom;
        if (g.pickPrims.empty()) continue;

        // RTC coords 是「相对图层原点的归一化墨卡托世界偏移」，须与渲染端 worldXYToCartesian 同口径
        // 精确反算：绝对世界 (originWx+rx, originWy+ry) → 经纬度（墨卡托逆）→ ECEF。
        // 旧实现按局部切平面米制线性化（dLon/=cosLat、dLat 漏 *cosLat），与渲染不一致，点离图层
        // 原点稍远即命中位置错位（表现为“点不中”，仅原点附近能命中）。
        const double oWx = g.originWx;
        const double oWy = g.originWy;
        auto rtcToEcef = [&](float rx, float ry, float alt, float mode) {
            double lon = 0.0, lat = 0.0;
            MercatorProjection::worldToLonLat(oWx + static_cast<double>(rx),
                                              oWy + static_cast<double>(ry), lon, lat);
            Vec3 p{0.0, 0.0, 0.0};
            // 与渲染端同口径：mode=1（高程要素）按真实 alt 烘焙 ECEF、不叠加贴地抬升；
            // mode=0（贴地）alt=0 + 沿地心法向抬 vecAlt（与非高程层旧行为一致）。
            const double useAlt = (mode > 0.5f) ? static_cast<double>(alt) : 0.0;
            globe.geographicToCartesian(lon, MercatorProjection::clampLatitude(lat), useAlt, p);
            if (mode <= 0.5f) p = p + p.normalized() * vecAlt;
            return p;
        };
        // 逐点对高程安全取：非高程层 alt/mode 为空 → 退化到 alt=0/mode=0（贴地抬升）。
        auto vertEcef = [&](const PickPrimitive &pp, size_t j) {
            const float a = j < pp.alt.size() ? pp.alt[j] : 0.0f;
            const float md = j < pp.mode.size() ? pp.mode[j] : 0.0f;
            return rtcToEcef(pp.coords[2 * j], pp.coords[2 * j + 1], a, md);
        };
        // ECEF → 屏幕像素；相机背后（clip w≤0）返回 false
        auto projectEcef = [&](const Vec3 &p, double &ox, double &oy) {
            const float x = static_cast<float>(p.x - cam.eye.x);
            const float y = static_cast<float>(p.y - cam.eye.y);
            const float z = static_cast<float>(p.z - cam.eye.z);
            const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
            if (cw <= 1e-4f) return false;
            const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
            const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            ox = (static_cast<double>(cx) / cw * 0.5 + 0.5) * vw;
            oy = (0.5 - static_cast<double>(cy) / cw * 0.5) * vh;
            return true;
        };
        // 顶点可见性（地球遮挡判据）：地表点 P 位于朝向眼点 E 的可见半球 ⇔ 外法向(≈地心径向 P̂)·(E−P) > 0，
        // 即 dot(P,E) > dot(P,P)=|P|²（等号即切点地平线，与瓦片网格深度剔除同效，避免拾到被地球挡住的背面图元）。
        // 阈值必须取「该点自身地心距平方 |P|²」而非固定赤道半径 A²：WGS84 是椭球，纬度 φ≠0 处地面点地心距
        // r<A，用 A² 作阈值会把镜头正下方的可见点也误判为背面——需相机高 alt > (A²−r²)/r（40°N 约 18km）才放过，
        // 故拉近到样地级别（低空）时 3D 拾取全灭（点不中标识）。用 |P|² 判据任意高度/纬度均正确，远半球点仍被剔。
        auto frontSide = [&](const Vec3 &p) {
            return p.dot(cam.eye - p) > 0.0;
        };
        // 图元眼距（近侧候选优先）：逐顶点最小值（顶点少，开销可忽）
        auto minEyeDist = [&](const PickPrimitive &pp) {
            double best = 1e300;
            for (size_t j = 0; j * 2 + 1 < pp.coords.size(); ++j) {
                const double d = (vertEcef(pp, j) - cam.eye).length();
                if (d < best) best = d;
            }
            return best;
        };

        const double pointRadPx = pickPointTolPx(vl.style.pointRadiusDp, density);
        const double lineTolPx = pickLineTolPx(vl.style.lineWidth);

        for (const PickPrimitive &pp : g.pickPrims) {
            if (pp.coords.empty()) continue;
            if (pp.type == PickType::Point) {
                if (pp.coords.size() < 2) continue;
                const Vec3 p = vertEcef(pp, 0);
                if (!frontSide(p)) continue; // 背半球遮挡（与渲染深度剔除同效）
                double px, py;
                if (!projectEcef(p, px, py)) continue;
                const double dx = px - sxPx, dy = py - syPx;
                if (dx * dx + dy * dy <= pointRadPx * pointRadPx)
                    cands.push_back({pass, 0, (p - cam.eye).length(), li, pp.fid});
                continue;
            }
            if (pp.type == PickType::Line) {
                const double tol2 = lineTolPx * lineTolPx;
                bool hit = false;
                for (const auto &rg : pp.ranges) {
                    if (hit) break;
                    const int start = rg.first, count = rg.second;
                    if (count < 2) continue;
                    // 逐段：先四至快速排除（段屏包围盒含点击点±容差才算垂距），再剔被地球遮挡的整段
                    for (int i = 0; i + 1 < count && !hit; ++i) {
                        const Vec3 a = vertEcef(pp, static_cast<size_t>(start + i));
                        const Vec3 b = vertEcef(pp, static_cast<size_t>(start + i + 1));
                        if (!frontSide(a) && !frontSide(b)) continue; // 整段被地球遮挡
                        double ax, ay, bx, by;
                        if (!projectEcef(a, ax, ay) || !projectEcef(b, bx, by)) continue;
                        if (ax > bx) { std::swap(ax, bx); }
                        if (ay > by) { std::swap(ay, by); }
                        if (sxPx < ax - lineTolPx || sxPx > bx + lineTolPx ||
                            syPx < ay - lineTolPx || syPx > by + lineTolPx) continue; // 段四至排除
                        if (segDistSq(sxPx, syPx, ax, ay, bx, by) <= tol2) hit = true;
                    }
                }
                if (hit) cands.push_back({pass, 0, minEyeDist(pp), li, pp.fid});
                continue;
            }
            // Polygon：外环包含 && 各洞不含（屏幕空间 PIP，投影坐标为透视真值）。
            // 单趟逐环：先投影外环累计全图元四至并排除，再逐环（含外环）PIP（环顶点量小，重投可忽）。
            if (pp.ranges.empty()) continue;
            const auto outer = pp.ranges[0];
            if (outer.second < 3) continue;
            const int oStart = outer.first, oCount = outer.second;
            std::vector<float> oscr;
            oscr.reserve(static_cast<size_t>(oCount) * 2);
            double b0x = 1e300, b0y = 1e300, b1x = -1e300, b1y = -1e300;
            bool projOk = true;
            for (int i = 0; i < oCount; ++i) {
                const Vec3 p = vertEcef(pp, static_cast<size_t>(oStart + i));
                double px, py;
                if (!projectEcef(p, px, py)) { projOk = false; break; } // 有顶点在相机背后：整图元弃判
                oscr.push_back(static_cast<float>(px));
                oscr.push_back(static_cast<float>(py));
                if (px < b0x) b0x = px; else if (px > b1x) b1x = px;
                if (py < b0y) b0y = py; else if (py > b1y) b1y = py;
            }
            if (!projOk) continue;
            if (!boxHit(b0x, b0y, b1x, b1y, 0.0)) continue; // 四至排除
            if (!pointInRing(sxPx, syPx, oscr, 0, oCount)) continue;
            bool inHole = false;
            for (size_t hi = 1; hi < pp.ranges.size() && !inHole; ++hi) {
                const int hStart = pp.ranges[hi].first, hCount = pp.ranges[hi].second;
                if (hCount < 3) continue;
                std::vector<float> hscr;
                hscr.reserve(static_cast<size_t>(hCount) * 2);
                bool hOk = true;
                for (int i = 0; i < hCount; ++i) {
                    const Vec3 p = vertEcef(pp, static_cast<size_t>(hStart + i));
                    double px, py;
                    if (!projectEcef(p, px, py)) { hOk = false; break; }
                    hscr.push_back(static_cast<float>(px));
                    hscr.push_back(static_cast<float>(py));
                }
                // 洞投影失败(-)保守不作洞（外环已含，宁可多判命中）
                if (hOk && pointInRing(sxPx, syPx, hscr, 0, hCount)) inHole = true;
            }
            if (!inHole) cands.push_back({pass, 1, minEyeDist(pp), li, pp.fid});
        }
    }

    if (cands.empty()) return false;
    // 择优：叠加层候选恒先于文件矢量层候选（同 2D 两阶段）；再点/线优先于面（小目标优先）；
    // 同类取离眼点最近（斜视下背半球穿地图元让位近侧）
    const Cand *best = &cands[0];
    for (const Cand &c : cands) {
        if (c.pass != best->pass) { if (c.pass < best->pass) best = &c; continue; }
        if (c.kind < best->kind || (c.kind == best->kind && c.dist < best->dist)) best = &c;
    }
    outLayerIndex = best->li;
    outFid = best->fid;
    return true;
}

void GlobeEngine::setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg) {
    renderer_.setLocationMarker(lonDeg, latDeg, visible, headingDeg);
    // 只改了标记状态（不涉及 GL 资源），省电模式下主动请求一帧使其立即生效。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h) {
    renderer_.setLocationMarkerIcon(std::move(rgba), w, h);
    // 图标像素已更新，旧纹理已失效；请求一帧使 GL 线程据新像素重传纹理并画罗盘图标。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setFontPath(const std::string &path) {
    renderer_.setFontPath(path); // FontAtlas::load 幂等、纯 CPU、线程安全（path 空则自动探测）
    // 字体就绪后主动请求一帧，使已有矢量标注立即显示（RENDERMODE_WHEN_DIRTY 下）。
    if (renderCallback_) renderCallback_();
}

void GlobeEngine::setDisplayDensity(double density) {
    navigator_.setDisplayDensity(density); // LOD densityFactor（对齐 参考实现 displayMetrics.density）
}

void GlobeEngine::setRenderCallback(std::function<void()> cb) {
    renderCallback_ = std::move(cb);
}

void GlobeEngine::releaseGl() {
    renderer_.release();
}

} // namespace globecore
