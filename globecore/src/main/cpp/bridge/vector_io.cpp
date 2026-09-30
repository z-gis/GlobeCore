// 矢量要素读取 / SQL 查询 / 统计 / 属性读写（GDAL/OGR → JSON）—— 核心实现见 bridge_api.h
// （双平台共享，JSON 文本作传输格式），Android JNI 导出为文末 #ifndef __OHOS__ 薄封装；
// 鸿蒙侧由 napi_vector_io.cpp 调同一核心。
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <strings.h>
#include <unistd.h>
#include <ctime>
#include <mutex>
#include <list>
#include <unordered_map>
#include <sys/stat.h>

#include "gdal/ogrsf_frmts.h"
#include "gdal/gdal_priv.h"
#include "gdal/cpl_error.h"

#include "srs_resolve.h"
#include "bridge_api.h"
#include "util/Log.h"

// ==================== 矢量要素读取（GDAL/OGR -> JSON，NativeVector 门面） ====================

/** 要素数上限，防大文件 OOM。
 *  已启用 SetSpatialFilterRect（走 .qix 索引）+ 图层四至预判 + 逐要素精确过滤三重防线，
 *  正常场景下命中数远小于此值；上限仅作为极端密集数据的兜底防护。
 *  10000 个 Polygon 约 160MB 堆，处于 Android 单进程安全边界内。 */
static const int MAX_VECTOR_FEATURES = 10000;

static void jsonEscape(std::string &out, const char *s) {
    if (s == nullptr) return;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char) *p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
}

static void appendXY(std::string &out, double x, double y) {
    char buf[48];
    // 6 位小数 ≈ 0.11m 精度，对地图显示完全足够，比 8 位减少 ~20% JSON 体积
    snprintf(buf, sizeof(buf), "[%.6f,%.6f]", x, y);
    out += buf;
}

/** 自适应顶点简化阈值（度）：由 emitLayerFeatures 根据屏幕 box 宽度动态设置。
 *  计算公式：(maxLon - minLon) / 2000 ≈ 0.5 像素（假设屏幕宽 1000 像素半宽）。
 *  alt=1098m 时：0.077°/2000 = 0.0000385° ≈ 4.3m（约 0.7 像素，不可见）
 *  alt=100m  时：0.007°/2000 = 0.0000035° ≈ 0.39m（仍亚像素）
 *  无过滤时（SQL 查询等）退化为固定 0.000002°（0.22m）保证精度 */
static double g_vertexTolerance = 0.000002;

static void appendLineCoords(std::string &out, OGRLineString *line) {
    out += "[";
    int n = line->getNumPoints();
    if (n == 0) { out += "]"; return; }
    // 始终保留第一个顶点
    double lastX = line->getX(0), lastY = line->getY(0);
    appendXY(out, lastX, lastY);
    // 中间顶点：跳过与前一个保留顶点距离 < g_vertexTolerance 的点
    for (int i = 1; i < n - 1; i++) {
        double x = line->getX(i), y = line->getY(i);
        if (fabs(x - lastX) < g_vertexTolerance && fabs(y - lastY) < g_vertexTolerance) {
            continue;
        }
        out += ",";
        appendXY(out, x, y);
        lastX = x; lastY = y;
    }
    // 始终保留最后一个顶点（保证环闭合）
    if (n > 1) {
        out += ",";
        appendXY(out, line->getX(n - 1), line->getY(n - 1));
    }
    out += "]";
}

/** 输出 fields 对象的 JSON 内容（不含 "fields": 前缀），格式 {"k":"v",...}。
 *  includeAll=false 时进入白名单模式，仅保留 labelField 匹配的字段（大小写不敏感），
 *  用于 shp/gpkg 延迟属性读取：加载阶段只传 labelField（供标注渲染），
 *  其余字段留到点击时按 FID 单独回取（nativeGetFeatureAttributes）。 */
static void appendFieldsObject(std::string &out, OGRFeature *feat,
                              bool includeAll = true, const char *labelField = nullptr) {
    out += "{";
    OGRFeatureDefn *defn = feat->GetDefnRef();
    bool first = true;
    for (int i = 0; i < defn->GetFieldCount(); i++) {
        if (!feat->IsFieldSet(i) || feat->IsFieldNull(i)) continue;
        const char *name = defn->GetFieldDefn(i)->GetNameRef();
        if (!includeAll) {
            if (labelField == nullptr || strcasecmp(name, labelField) != 0) continue;
        }
        if (!first) out += ",";
        first = false;
        out += "\"";
        jsonEscape(out, name);
        out += "\":\"";
        jsonEscape(out, feat->GetFieldAsString(i));
        out += "\"";
    }
    out += "}";
}

/** 便捷封装：追加 ,"fields":{...}（要素 emit 主路径使用） */
static void appendFields(std::string &out, OGRFeature *feat,
                        bool includeAll = true, const char *labelField = nullptr) {
    out += ",\"fields\":";
    appendFieldsObject(out, feat, includeAll, labelField);
}

static void emitPointFeature(std::string &out, bool &firstFeature, double x, double y, OGRFeature *feat,
                             const char *layerName,
                             bool includeAll = true, const char *labelField = nullptr) {
    if (!firstFeature) out += ",";
    firstFeature = false;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"geom\":\"POINT\",\"x\":%.8f,\"y\":%.8f,\"fid\":%lld", x, y,
             (long long) feat->GetFID());
    out += buf;
    out += ",\"layer\":\"";
    jsonEscape(out, layerName);
    out += "\"";
    appendFields(out, feat, includeAll, labelField);
    out += "}";
}

static void emitLineFeature(std::string &out, bool &firstFeature,
                            const std::vector<OGRLineString *> &lines, OGRFeature *feat,
                            const char *layerName,
                            bool includeAll = true, const char *labelField = nullptr) {
    if (!firstFeature) out += ",";
    firstFeature = false;
    out += "{\"geom\":\"LINE\",\"parts\":[";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += ",";
        appendLineCoords(out, lines[i]);
    }
    out += "]";
    char buf[48];
    snprintf(buf, sizeof(buf), ",\"fid\":%lld", (long long) feat->GetFID());
    out += buf;
    out += ",\"layer\":\"";
    jsonEscape(out, layerName);
    out += "\"";
    appendFields(out, feat, includeAll, labelField);
    out += "}";
}

static void emitPolygonFeature(std::string &out, bool &firstFeature, OGRPolygon *poly, OGRFeature *feat,
                               const char *layerName,
                               bool includeAll = true, const char *labelField = nullptr) {
    OGRLineString *outer = poly->getExteriorRing();
    if (outer == nullptr) return;
    if (!firstFeature) out += ",";
    firstFeature = false;
    out += "{\"geom\":\"POLYGON\",\"outer\":";
    appendLineCoords(out, outer);
    out += ",\"holes\":[";
    int n = poly->getNumInteriorRings();
    for (int i = 0; i < n; i++) {
        if (i) out += ",";
        appendLineCoords(out, poly->getInteriorRing(i));
    }
    out += "]";
    char buf[48];
    snprintf(buf, sizeof(buf), ",\"fid\":%lld", (long long) feat->GetFID());
    out += buf;
    out += ",\"layer\":\"";
    jsonEscape(out, layerName);
    out += "\"";
    appendFields(out, feat, includeAll, labelField);
    out += "}";
}

/**
 * 按几何类型摊平输出要素：点/多点各一个 POINT 要素，多线合并为一个 LINE 要素（多 parts），
 * 多面拆分为多个 POLYGON 要素（外环+内环），几何集合递归处理。
 */
static void emitGeometry(std::string &out, bool &firstFeature, int &count, bool &truncated,
                         OGRGeometry *g, OGRFeature *feat, const char *layerName,
                         bool includeAll = true, const char *labelField = nullptr) {
    if (g == nullptr || truncated) return;
    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPoint: {
            if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
            count++;
            OGRPoint *pt = (OGRPoint *) g;
            emitPointFeature(out, firstFeature, pt->getX(), pt->getY(), feat, layerName,
                             includeAll, labelField);
            break;
        }
        case wkbMultiPoint: {
            OGRMultiPoint *mp = (OGRMultiPoint *) g;
            for (int i = 0; i < mp->getNumGeometries(); i++) {
                if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
                count++;
                OGRPoint *pt = (OGRPoint *) mp->getGeometryRef(i);
                emitPointFeature(out, firstFeature, pt->getX(), pt->getY(), feat, layerName,
                                 includeAll, labelField);
            }
            break;
        }
        case wkbLineString: {
            if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
            count++;
            std::vector<OGRLineString *> lines;
            lines.push_back((OGRLineString *) g);
            emitLineFeature(out, firstFeature, lines, feat, layerName, includeAll, labelField);
            break;
        }
        case wkbMultiLineString: {
            if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
            count++;
            OGRMultiLineString *ml = (OGRMultiLineString *) g;
            std::vector<OGRLineString *> lines;
            for (int i = 0; i < ml->getNumGeometries(); i++) {
                lines.push_back((OGRLineString *) ml->getGeometryRef(i));
            }
            emitLineFeature(out, firstFeature, lines, feat, layerName, includeAll, labelField);
            break;
        }
        case wkbPolygon: {
            if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
            count++;
            emitPolygonFeature(out, firstFeature, (OGRPolygon *) g, feat, layerName,
                               includeAll, labelField);
            break;
        }
        case wkbMultiPolygon: {
            OGRMultiPolygon *mpoly = (OGRMultiPolygon *) g;
            for (int i = 0; i < mpoly->getNumGeometries(); i++) {
                if (count >= MAX_VECTOR_FEATURES) { truncated = true; return; }
                count++;
                emitPolygonFeature(out, firstFeature, (OGRPolygon *) mpoly->getGeometryRef(i), feat, layerName,
                                   includeAll, labelField);
            }
            break;
        }
        case wkbGeometryCollection: {
            OGRGeometryCollection *gc = (OGRGeometryCollection *) g;
            for (int i = 0; i < gc->getNumGeometries(); i++) {
                emitGeometry(out, firstFeature, count, truncated, gc->getGeometryRef(i), feat, layerName,
                             includeAll, labelField);
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 读取并输出单个图层的全部要素：图层 SRS 非空时重投影到 WGS84（传统 GIS 轴序 经度,纬度）。
 *
 * hasFilter 为真时采用「四重防线」策略（不依赖 WGS84 → 源 SRS 的反向变换，
 *  因 CGCS2000 3度带高斯-克吕格等坐标系下 PROJ 反向变换会静默失败）：
 *  1) 图层四至预判：用正向 ct 将图层原始四至四角变换到 WGS84，与屏幕 box 不相交则整层跳过；
 *  2) 比例映射构造源坐标系 box：将屏幕 box 在图层 WGS84 四至中的归一化位置（fx/fy∈[0,1]）
 *     按同比例映射到源坐标系四至内，加 15% 缓冲后喂给 SetSpatialFilterRect，
 *     让 GDAL 走 .qix 空间索引仅返回候选要素（县级范围投影非线性失真 <1%，缓冲足以覆盖）；
 *  3) 逐要素精确过滤：候选要素正向变换到 WGS84 后取 envelope 与屏幕 box 判断，
 *     仅 emit 相交要素（修正比例映射的非线性误差）；
 *  4) MAX_VECTOR_FEATURES 兜底：极端密集数据命中仍超上限时截断防 OOM。
 *
 * 全量读取（hasFilter=false，如 SQL 查询结果）与过滤读取共用同一遍历代码，保证要素 JSON 结构与坐标处理一致。
 */
static void emitLayerFeatures(std::string &out, OGRLayer *layer, const char *path,
                              bool &firstFeature, int &count, bool &truncated,
                              bool hasFilter, double minLon, double minLat,
                              double maxLon, double maxLat,
                              bool includeAll = true, const char *labelField = nullptr,
                              bool simplify = false, double simplifyTolerance = 0.0) {
    if (layer == nullptr || truncated) return;

    // 设置自适应顶点简化阈值：屏幕 box 宽度 / 2000 ≈ 0.5 像素（假设 1000px 半宽）
    // alt=1098m: 0.077°/2000 = 3.85e-5° ≈ 4.3m（0.7像素，不可见）
    // alt=100m:  0.007°/2000 = 3.5e-6° ≈ 0.39m（仍亚像素）
    if (hasFilter) {
        g_vertexTolerance = std::max((maxLon - minLon) / 2000.0, 0.000002);
    } else {
        g_vertexTolerance = 0.000002;  // SQL 查询等无过滤场景保守处理
    }

    OGRCoordinateTransformation *ct = nullptr;
    OGRSpatialReference *srcRef = resolveSourceSrs(path, layer);
    if (srcRef != nullptr) {
        OGRSpatialReference dstRef;
        dstRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dstRef.SetWellKnownGeogCS("WGS84");
        ct = OGRCreateCoordinateTransformation(srcRef, &dstRef);
        if (ct == nullptr) {
            // 创建失败（常见：proj.db 搜索路径未配置）时坐标将按源坐标系原样输出，
            // 投影米制被当经纬度渲染导致位置错误，必须提示而非静默
            LOGE("图层[%s]创建坐标变换失败（proj.db 未配置？），坐标将按源坐标系原样输出",
                 layer->GetName());
        }
    }

    // 防线 1、2：图层四至预判 + 比例映射构造源坐标系过滤 box
    // 仅在 hasFilter 且 ct 可用且源为投影坐标系时生效；其他情况退化为防线 3 的逐要素手工过滤。
    bool skipLayer = false;
    if (hasFilter) {
        OGREnvelope rawExt;
        bool haveRawExt = (layer->GetExtent(&rawExt, TRUE) == OGRERR_NONE);
        double lMinLon = 0, lMaxLon = 0, lMinLat = 0, lMaxLat = 0;
        bool haveWgs84Ext = false;
        if (haveRawExt) {
            if (ct != nullptr && !srcRef->IsGeographic()) {
                // 投影坐标系：四角变换到 WGS84 后取极值（兼容轴序翻转）
                double xs[4] = {rawExt.MinX, rawExt.MaxX, rawExt.MinX, rawExt.MaxX};
                double ys[4] = {rawExt.MinY, rawExt.MinY, rawExt.MaxY, rawExt.MaxY};
                if (ct->Transform(4, xs, ys)) {
                    lMinLon = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
                    lMaxLon = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
                    lMinLat = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
                    lMaxLat = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
                    haveWgs84Ext = true;
                }
            } else if (srcRef != nullptr && srcRef->IsGeographic()) {
                // 地理坐标系：原始四至即 WGS84
                lMinLon = rawExt.MinX; lMaxLon = rawExt.MaxX;
                lMinLat = rawExt.MinY; lMaxLat = rawExt.MaxY;
                haveWgs84Ext = true;
            }
            // 无 SRS 时 haveWgs84Ext=false，不预判不过滤（退化全量读取）
        }
        if (haveWgs84Ext) {
            LOGI("[DIAG] layer[%s] WGS84 extent=[%.6f,%.6f,%.6f,%.6f] vs screen=[%.6f,%.6f,%.6f,%.6f]",
                 layer->GetName(), lMinLon, lMinLat, lMaxLon, lMaxLat,
                 minLon, minLat, maxLon, maxLat);
            // 防线 1：矩形不相交判定（容许边界接触）
            if (lMaxLon < minLon || lMinLon > maxLon || lMaxLat < minLat || lMinLat > maxLat) {
                skipLayer = true;
                LOGI("[DIAG] layer[%s] disjoint from screen -> SKIP whole layer (0 features)",
                     layer->GetName());
            } else if (haveRawExt) {
                // 防线 2：比例映射构造源坐标系过滤 box，喂给 SetSpatialFilterRect（走 .qix 索引）
                double fMinX, fMinY, fMaxX, fMaxY;
                double fx1 = 0, fx2 = 0, fy1 = 0, fy2 = 0;
                if (srcRef != nullptr && srcRef->IsGeographic()) {
                    // 地理坐标系：屏幕 box 直接就是源坐标系 box
                    fMinX = minLon; fMaxX = maxLon; fMinY = minLat; fMaxY = maxLat;
                } else {
                    // 投影坐标系：将屏幕 box 在图层 WGS84 四至内归一化后，按同比例映射到源坐标系四至内
                    double wLon = lMaxLon - lMinLon;
                    double wLat = lMaxLat - lMinLat;
                    if (wLon <= 0 || wLat <= 0) {
                        // 退化四至（点/线图层）：不设源坐标系过滤，仅靠防线 3
                        fMinX = fMinY = fMaxX = fMaxY = 0;
                    } else {
                        fx1 = std::max(0.0, std::min(1.0, (minLon - lMinLon) / wLon));
                        fx2 = std::max(0.0, std::min(1.0, (maxLon - lMinLon) / wLon));
                        fy1 = std::max(0.0, std::min(1.0, (minLat - lMinLat) / wLat));
                        fy2 = std::max(0.0, std::min(1.0, (maxLat - lMinLat) / wLat));
                        double wX = rawExt.MaxX - rawExt.MinX;
                        double wY = rawExt.MaxY - rawExt.MinY;
                        fMinX = rawExt.MinX + fx1 * wX;
                        fMaxX = rawExt.MinX + fx2 * wX;
                        fMinY = rawExt.MinY + fy1 * wY;
                        fMaxY = rawExt.MinY + fy2 * wY;
                        // 缓冲策略：15% 相对缓冲 + 500 米最小绝对缓冲（取大者）
                        // 高斯-克吕格 X-lon 比例因子随 lat 变化，县级范围 X 方向映射误差可达 300~600 米；
                        // 15%+500m 保证小屏幕（6km）每边余量 ~990m，扣除 600m 误差后仍有 390m 安全边际。
                        // 多余候选由 envelope-first 预检快速跳过（仅变换 4 角，开销极低）。
                        double bufX = std::max((fMaxX - fMinX) * 0.15, 500.0);
                        double bufY = std::max((fMaxY - fMinY) * 0.15, 500.0);
                        fMinX -= bufX; fMaxX += bufX;
                        fMinY -= bufY; fMaxY += bufY;
                        // 限幅到图层原始四至内，避免缓冲后超出图层范围影响 .qix 索引命中
                        fMinX = std::max(fMinX, rawExt.MinX);
                        fMaxX = std::min(fMaxX, rawExt.MaxX);
                        fMinY = std::max(fMinY, rawExt.MinY);
                        fMaxY = std::min(fMaxY, rawExt.MaxY);
                    }
                }
                if (fMaxX > fMinX && fMaxY > fMinY) {
                    layer->SetSpatialFilterRect(fMinX, fMinY, fMaxX, fMaxY);
                    LOGI("[DIAG] layer[%s] SetSpatialFilterRect src=[%.3f,%.3f,%.3f,%.3f] "
                         "(fx=[%.4f,%.4f] fy=[%.4f,%.4f] buf=15%%+500m)",
                         layer->GetName(), fMinX, fMinY, fMaxX, fMaxY,
                         fx1, fx2, fy1, fy2);
                }
            }
        }
    }

    if (!skipLayer) {
        layer->ResetReading();
        OGRFeature *feat;
        int skippedByBox = 0;
        int candidates = 0;
        bool firstLogged = false;
        while (!truncated && (feat = layer->GetNextFeature()) != nullptr) {
            candidates++;
            OGRGeometry *geom = feat->GetGeometryRef();
            if (geom != nullptr) {
                bool pass = true;

                // ── 性能关键：envelope-first 预检 ──
                // 先仅变换 envelope 4 角到 WGS84（4 点 vs 整个几何可能数百~数千点），
                // 快速判断是否与屏幕 box 相交；不相交则跳过，避免对整个几何做 transform。
                // 对场景 1（6866 候选 / 4125 通过）可省去 2741 次完整几何变换，提速 30~40%。
                if (hasFilter && ct != nullptr) {
                    OGREnvelope srcEnv;
                    geom->getEnvelope(&srcEnv);
                    double xs[4] = {srcEnv.MinX, srcEnv.MaxX, srcEnv.MinX, srcEnv.MaxX};
                    double ys[4] = {srcEnv.MinY, srcEnv.MinY, srcEnv.MaxY, srcEnv.MaxY};
                    if (ct->Transform(4, xs, ys)) {
                        double wMinX = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
                        double wMaxX = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
                        double wMinY = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
                        double wMaxY = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
                        if (wMaxX < minLon || wMinX > maxLon ||
                            wMaxY < minLat || wMinY > maxLat) {
                            pass = false;
                            skippedByBox++;
                        }
                    }
                    // Transform 失败时保守通过，由后续全变换 + 精确判断兜底
                }

                if (pass) {
                    // 通过预检：执行完整几何变换（源坐标系 → WGS84）
                    if (ct != nullptr) {
                        geom->transform(ct);
                    }
                    // 注：不再做二次 envelope 复核——4 角预检对单个要素（通常跨度 <500m）
                    // 的投影非线性误差 <1m，远小于屏幕 box 尺度，不会漏判。
                }

                if (pass) {
                    if (!firstLogged) {
                        OGREnvelope ge;
                        geom->getEnvelope(&ge);
                        LOGI("[DIAG] layer[%s] first emit fid=%lld WGS84 envelope=[%.6f,%.6f,%.6f,%.6f]",
                             layer->GetName(), (long long) feat->GetFID(),
                             ge.MinX, ge.MinY, ge.MaxX, ge.MaxY);
                        firstLogged = true;
                    }
                    // Phase 5：几何简化（Douglas-Peucker）——重投影到 WGS84 后按容差简化顶点，
                    // 减少 JSON 体积与下游解析/渲染开销。Simplify 返回新几何（需手动释放），
                    // 失败或返回空时回退原始几何。点要素无顶点可简，Simplify 也安全。
                    OGRGeometry *emitGeom = geom;
                    OGRGeometry *simplified = nullptr;
                    if (simplify && simplifyTolerance > 0.0) {
                        simplified = geom->Simplify(simplifyTolerance);
                        if (simplified != nullptr) emitGeom = simplified;
                    }
                    emitGeometry(out, firstFeature, count, truncated, emitGeom, feat, layer->GetName(),
                                 includeAll, labelField);
                    if (simplified != nullptr) {
                        OGRGeometryFactory::destroyGeometry(simplified);
                    }
                }
            }
            OGRFeature::DestroyFeature(feat);
        }
        LOGI("[DIAG] layer[%s] emit done candidates=%d count=%d skippedByBox=%d truncated=%d tolerance=%.7f",
             layer->GetName(), candidates, count, skippedByBox, truncated ? 1 : 0,
             g_vertexTolerance);
    }

    // 清理本层设置的空间过滤，避免影响同一 dataset 的其他图层（如 dwg 多子图层）
    if (hasFilter) {
        layer->SetSpatialFilter(nullptr);
    }

    if (ct != nullptr) {
        OGRCoordinateTransformation::DestroyCT(ct);
    }
    if (srcRef != nullptr) {
        srcRef->Release();
    }
}

namespace wwbridge {

std::string readVectorFeatures(const std::string &pathStr,
                               double minLon, double minLat, double maxLon, double maxLat,
                               bool hasFilter, bool includeAll, const std::string &labelField,
                               bool simplify, double simplifyTol) {
    GDALAllRegister();
    // 返回约定：空串 = 数据源打开失败（对应 JNI null）；{"error":...} = 业务错误文本

    // ── .qix 空间索引自动创建（仅 shp 格式）──
    // 没有 .qix 时 GDAL 的 SetSpatialFilterRect 仍需顺序扫描全部记录 header，
    // 10 万要素级扫描耗时 ~0.5–1s；创建后后续加载（含动态重载）可省此开销。
    // 仅首次加载时创建，后续检测到 .qix 存在则跳过。
    if (pathStr.size() > 4) {
        std::string ext = pathStr.substr(pathStr.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".shp") {
            std::string qixPath = pathStr.substr(0, pathStr.size() - 4) + ".qix";
            if (access(qixPath.c_str(), F_OK) != 0) {
                // .qix 不存在，尝试以更新模式打开并创建索引
                GDALDataset *dsW = (GDALDataset *) GDALOpenEx(pathStr.c_str(),
                                                             GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                                             nullptr, nullptr, nullptr);
                if (dsW != nullptr) {
                    OGRLayer *lyr = dsW->GetLayer(0);
                    if (lyr != nullptr) {
                        std::string sql = "CREATE SPATIAL INDEX ON \"";
                        sql += lyr->GetName();
                        sql += "\"";
                        OGRLayer *res = dsW->ExecuteSQL(sql.c_str(), nullptr, nullptr);
                        if (res) dsW->ReleaseResultSet(res);
                        if (access(qixPath.c_str(), F_OK) == 0) {
                            LOGI("已为 [%s] 创建 .qix 空间索引（后续加载将更快）",
                                 lyr->GetName());
                        }
                    }
                    GDALClose(dsW);
                }
            }
        }
    }

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(pathStr.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    if (ds == nullptr) {
        LOGW("GDALOpenEx 打开矢量数据失败");
        return "";
    }

    // CAD 无坐标系（无同名 .prj 且坐标不含投影带号）：米制坐标无法定位，不按 WGS84 原样输出，
    // 回传错误文本由界面提示并跳过加载（避免错位显示）。
    if (cadLacksSrs(pathStr.c_str(), ds->GetLayer(0))) {
        GDALClose(ds);
        return "{\"error\":\"该 CAD 数据无坐标系信息（无同名 .prj 且坐标不含投影带号），无法定位，已跳过加载\"}";
    }

    std::string out = "{\"features\":[";
    out.reserve(1 << 20);  // 预分配 1MB，减少大要素集 JSON 拼接时频繁 realloc 的开销
    bool firstFeature = true;
    int count = 0;
    bool truncated = false;
    // hasFilter：仅读屏幕 box 内要素（含缓冲），从源头限制读取量；
    // 入口层以「任一为 NaN 即不过滤」口径归一后传入（Android/鸿蒙一致）。

    // 遍历全部图层（dwg 等格式的实体分布在多个子图层）
    int layerCount = ds->GetLayerCount();
    for (int li = 0; li < layerCount && !truncated; li++) {
        emitLayerFeatures(out, ds->GetLayer(li), pathStr.c_str(), firstFeature, count, truncated,
                          hasFilter, minLon, minLat, maxLon, maxLat,
                          includeAll, labelField.empty() ? nullptr : labelField.c_str(),
                          simplify, simplifyTol);
    }

    if (truncated) {
        LOGW("要素数超过上限 %d，已截断", MAX_VECTOR_FEATURES);
    }

    out += "]}";
    GDALClose(ds);
    return out;
}

/**
 * 对矢量数据执行 SQL 查询（OGR SQL / SQLite 方言，由驱动能力决定）并输出命中要素。
 * 返回 JSON：成功 {"features":[…]}（命中为空则数组为空），失败 {"error":"…"}。
 * 查询结果图层同样按源坐标系重投影到 WGS84，不做空间过滤（查询结果需完整呈现）。
 */
std::string queryVectorFeatures(const std::string &pathStr, const std::string &sqlText) {
    GDALAllRegister();

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(pathStr.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    if (ds == nullptr) {
        LOGW("GDALOpenEx 打开矢量数据失败");
        return "";
    }

    // 语法/字段错误经 CPL 错误处理器上报，静默处理后取错误文本回传界面提示
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer *result = ds->ExecuteSQL(sqlText.c_str(), nullptr, nullptr);
    const char *errMsg = CPLGetLastErrorMsg();
    std::string errText = (errMsg != nullptr) ? errMsg : "";
    CPLPopErrorHandler();

    if (result == nullptr) {
        LOGW("SQL 执行失败: %s", errText.empty() ? "(无错误信息)" : errText.c_str());
        GDALClose(ds);
        std::string out = "{\"error\":\"";
        jsonEscape(out, errText.empty() ? "SQL 语句执行失败" : errText.c_str());
        out += "\"}";
        return out;
    }

    // CAD 无坐标系：查询结果同样无法定位，回传错误文本（界面按查询失败提示），不做错位高亮。
    if (cadLacksSrs(pathStr.c_str(), result)) {
        ds->ReleaseResultSet(result);
        GDALClose(ds);
        return "{\"error\":\"该 CAD 数据无坐标系信息（无同名 .prj 且坐标不含投影带号），无法定位查询结果\"}";
    }

    std::string out = "{\"features\":[";
    bool firstFeature = true;
    int count = 0;
    bool truncated = false;
    emitLayerFeatures(out, result, pathStr.c_str(), firstFeature, count, truncated, false, 0, 0, 0, 0);

    if (truncated) {
        LOGW("查询命中要素数超过上限 %d，已截断", MAX_VECTOR_FEATURES);
    }

    out += "]}";
    // 结果集由数据集拥有，须经 ReleaseResultSet 释放（不可 delete）
    ds->ReleaseResultSet(result);
    GDALClose(ds);
    return out;
}

/**
 * 快速统计矢量数据的要素总数（遍历全部子图层求和），不做几何序列化、不做空间过滤。
 * 供上层判定小数据集是否直接全量渲染（避开按屏幕四至过滤+动态重载的开销）。
 * 打开失败返回 -1。
 */
long long countVectorFeatures(const std::string &path) {
    if (path.empty()) return -1;
    GDALAllRegister();

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    if (ds == nullptr) return -1;

    GIntBig total = 0;
    int layerCount = ds->GetLayerCount();
    for (int li = 0; li < layerCount; li++) {
        OGRLayer *layer = ds->GetLayer(li);
        if (layer == nullptr) continue;
        // bForce=TRUE 强制精确计数（部分驱动的近似值不可靠）
        GIntBig c = layer->GetFeatureCount(TRUE);
        if (c > 0) total += c;
    }
    GDALClose(ds);
    if (total > 0x7FFFFFFFLL) total = 0x7FFFFFFFLL;  // jint 溢出兑底（双平台统一口径）
    return total;
}

/** 单调时钟毫秒差（耗时打点用） */
static long elapsedMs(const timespec &from) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - from.tv_sec) * 1000L + (now.tv_nsec - from.tv_nsec) / 1000000L;
}

// ── 属性回取数据集句柄复用缓存 ──────────────────────────────────────────────
// 点击要素时每次 GDALOpenEx 全量打开文件：shp（读 .shx/.dbf 头）开销小，但 KML/DXF/GML 类
// 格式打开即解析整个文件（大文件秒级），是「点击后属性读取慢」的主要来源。
// 此处按路径缓存少量 READONLY 数据集句柄：连续点击直接复用（第二次起毫秒级）；
// 文件 mtime/size 变化（外部替换文件）时失效重开；属性写回后同步失效该路径。
// 全部读取在同一锁下串行（GDAL 数据集非线程安全），大格式常驻内存以缓存上限兜底。
namespace {
    struct AttrCacheEntry {
        GDALDataset *ds;
        time_t mtime;
        off_t size;
    };
    std::mutex gAttrCacheMtx;
    std::list<std::string> gAttrCacheLru;   // 最近使用在前
    std::unordered_map<std::string, AttrCacheEntry> gAttrCache;
    constexpr size_t kAttrCacheMax = 4;

    /// 移除并关闭指定路径的缓存句柄（调用方须持有 gAttrCacheMtx）
    void attrCacheErase(const std::string &key) {
        auto it = gAttrCache.find(key);
        if (it == gAttrCache.end()) return;
        GDALClose(it->second.ds);
        gAttrCache.erase(it);
        gAttrCacheLru.remove(key);
    }

    /// 取（或打开并入缓存）路径对应的只读数据集句柄（调用方须持有 gAttrCacheMtx）；
    /// 文件 stat 失败/打开失败返回 nullptr
    GDALDataset *attrCacheOpen(const std::string &key) {
        struct stat st{};
        if (stat(key.c_str(), &st) != 0) return nullptr;
        auto it = gAttrCache.find(key);
        if (it != gAttrCache.end()) {
            if (it->second.mtime == st.st_mtime && it->second.size == st.st_size) {
                gAttrCacheLru.remove(key);
                gAttrCacheLru.push_front(key);
                return it->second.ds;
            }
            attrCacheErase(key); // 文件已被替换/修改：弃旧句柄重开
        }
        GDALDataset *ds = (GDALDataset *) GDALOpenEx(key.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                     nullptr, nullptr, nullptr);
        if (ds == nullptr) return nullptr;
        gAttrCache.emplace(key, AttrCacheEntry{ds, st.st_mtime, st.st_size});
        gAttrCacheLru.push_front(key);
        while (gAttrCacheLru.size() > kAttrCacheMax) {
            attrCacheErase(gAttrCacheLru.back());
        }
        return ds;
    }
} // namespace

/**
 * 按 FID 更新矢量数据某要素的属性字段（就地写回源文件）。
 * 仅修改现有字段值，不支持新增字段/修改几何。dwg 等只读驱动 GDALOpenEx(GDAL_OF_UPDATE) 会失败，
 * 返回 false 由上层提示。keys/values 长度必须一致。
 */
bool updateFeatureAttributes(const std::string &pathStr, long long featureId,
                             const std::vector<std::string> &keyList,
                             const std::vector<std::string> &valueList) {
    if (pathStr.empty() || featureId < 0) return false;
    if (keyList.empty() || keyList.size() != valueList.size()) return false;

    GDALAllRegister();

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(pathStr.c_str(),
                                                 GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                                 nullptr, nullptr, nullptr);
    if (ds == nullptr) {
        LOGW("updateAttributes: 无法以更新模式打开数据源（格式可能只读）: %s", pathStr.c_str());
        return false;
    }

    bool updated = false;
    int layerCount = ds->GetLayerCount();
    for (int li = 0; li < layerCount && !updated; li++) {
        OGRLayer *layer = ds->GetLayer(li);
        if (layer == nullptr) continue;
        OGRFeature *feature = layer->GetFeature((GIntBig) featureId);
        if (feature == nullptr) continue;

        bool hasAnyField = false;
        OGRFeatureDefn *defn = layer->GetLayerDefn();
        if (defn != nullptr) {
            for (size_t i = 0; i < keyList.size(); i++) {
                if (keyList[i].empty()) continue;
                int idx = defn->GetFieldIndex(keyList[i].c_str());
                if (idx < 0) continue;
                hasAnyField = true;
                feature->SetField(idx, valueList[i].c_str());
            }
        }
        if (hasAnyField && layer->SetFeature(feature) == OGRERR_NONE) {
            updated = true;
        }
        OGRFeature::DestroyFeature(feature);
    }

    GDALClose(ds);
    if (updated) {
        // 写回后失效缓存句柄：下次属性回取重新打开，读到新值
        std::lock_guard<std::mutex> lk(gAttrCacheMtx);
        attrCacheErase(pathStr);
    }
    return updated;
}

/**
 * 按 FID 单要素属性回取（延迟属性读取模式配套）：
 * 加载阶段 nativeReadVectorFeatures(includeAllFields=false) 仅传 labelField 白名单字段，
 * 用户点击时按 FID 二次取全字段。数据集句柄经 attrCacheOpen 复用（见上方缓存注释），
 * 避免每次 GDALOpenEx 全量打开文件；遍历全部图层查找 fid 命中的要素，
 * emit {"fields":{...}}；未找到或打开失败返回 nullptr。
 * shp 经 .shx 随机读 seek ~1-3ms，gpkg 经 SQLite 主键 B-tree seek 同量级；
 * KML/DXF 首次仍需整文件解析（弹层先行异步回填兜住感知），第二次起命中句柄缓存为毫秒级。
 */
std::string getFeatureAttributes(const std::string &pathStr, long long featureId) {
    if (pathStr.empty() || featureId < 0) return "";
    GDALAllRegister();

    timespec t0{};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    std::lock_guard<std::mutex> lk(gAttrCacheMtx);
    GDALDataset *ds = attrCacheOpen(pathStr);
    if (ds == nullptr) return "";

    std::string out;
    bool found = false;
    int layerCount = ds->GetLayerCount();
    for (int li = 0; li < layerCount && !found; li++) {
        OGRLayer *layer = ds->GetLayer(li);
        if (layer == nullptr) continue;
        OGRFeature *feature = layer->GetFeature((GIntBig) featureId);
        if (feature == nullptr) continue;
        out = "{\"fields\":";
        appendFieldsObject(out, feature, true, nullptr);
        out += "}";
        OGRFeature::DestroyFeature(feature);
        found = true;
    }

    // 句柄归缓存所有：不关闭，供后续点击直接复用（文件变化/写回时失效）
    if (!found) return "";
    LOGD("getFeatureAttributes fid=%lld cost=%ldms path=%s",
         (long long) featureId, elapsedMs(t0), pathStr.c_str());
    return out;
}

} // namespace wwbridge

// ── Android JNI 导出薄封装（com.zys.globecore.NativeVector 门面）──
#if !defined(__OHOS__)
#include <jni.h>

/** jstring 数组 -> std::vector<std::string>（属性写回用） */
static std::vector<std::string> JStringArrayToVector(JNIEnv *env, jobjectArray arr) {
    std::vector<std::string> out;
    if (arr == nullptr) return out;
    jsize n = env->GetArrayLength(arr);
    out.reserve((size_t) n);
    for (jsize i = 0; i < n; i++) {
        auto *s = (jstring) env->GetObjectArrayElement(arr, i);
        if (s == nullptr) { out.emplace_back(""); continue; }
        const char *c = env->GetStringUTFChars(s, nullptr);
        out.emplace_back(c ? c : "");
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
    }
    return out;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeVector_readVectorFeatures(
        JNIEnv *env, jobject /*thiz*/, jstring path,
        jdouble minLon, jdouble minLat, jdouble maxLon, jdouble maxLat,
        jboolean includeAllFields, jstring labelField,
        jboolean simplifyGeometry, jdouble simplifyTolerance) {

    if (path == nullptr) return nullptr;
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    const std::string pathStr(p);
    env->ReleaseStringUTFChars(path, p);

    // 延迟属性读取：includeAllFields=false 时仅保留 labelField 白名单字段（可空），
    // 其余字段留到点击时按 FID 回取。shp/gpkg 启用，kml/kmz/dwg/dxf 保持全字段。
    const bool includeAll = (includeAllFields == JNI_TRUE);
    std::string labelFieldStr;
    if (labelField != nullptr) {
        const char *lf = env->GetStringUTFChars(labelField, nullptr);
        if (lf != nullptr) { labelFieldStr = lf; env->ReleaseStringUTFChars(labelField, lf); }
    }
    // Phase 5：几何简化开关（默认 false），容差单位 WGS84 度
    const bool simplify = (simplifyGeometry == JNI_TRUE);
    const double simplifyTol = simplify ? simplifyTolerance : 0.0;
    // 任一为 NaN 表示不过滤（Kotlin 侧传入口径，归一后交核心）
    const bool hasFilter = !isnan(minLon) && !isnan(minLat) && !isnan(maxLon) && !isnan(maxLat);

    std::string json = wwbridge::readVectorFeatures(pathStr, minLon, minLat, maxLon, maxLat,
                                                    hasFilter, includeAll, labelFieldStr,
                                                    simplify, simplifyTol);
    if (json.empty()) return nullptr;
    return env->NewStringUTF(json.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeVector_queryVectorFeatures(
        JNIEnv *env, jobject /*thiz*/, jstring path, jstring sql) {

    if (path == nullptr || sql == nullptr) return nullptr;
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    const std::string pathStr(p);
    env->ReleaseStringUTFChars(path, p);
    const char *q = env->GetStringUTFChars(sql, nullptr);
    if (q == nullptr) return nullptr;
    const std::string sqlText(q);
    env->ReleaseStringUTFChars(sql, q);

    std::string json = wwbridge::queryVectorFeatures(pathStr, sqlText);
    if (json.empty()) return nullptr;
    return env->NewStringUTF(json.c_str());
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeVector_countVectorFeatures(
        JNIEnv *env, jobject /*thiz*/, jstring path) {

    if (path == nullptr) return -1;
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return -1;
    const std::string pathStr(p);
    env->ReleaseStringUTFChars(path, p);
    return (jint) wwbridge::countVectorFeatures(pathStr);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_zys_globecore_NativeVector_updateFeatureAttributes(
        JNIEnv *env, jobject /*thiz*/, jstring path, jlong featureId,
        jobjectArray keys, jobjectArray values) {

    if (path == nullptr || featureId < 0 || keys == nullptr || values == nullptr) return JNI_FALSE;
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return JNI_FALSE;
    const std::string pathStr(p);
    env->ReleaseStringUTFChars(path, p);
    std::vector<std::string> keyList = JStringArrayToVector(env, keys);
    std::vector<std::string> valueList = JStringArrayToVector(env, values);
    return wwbridge::updateFeatureAttributes(pathStr, (long long) featureId, keyList, valueList)
               ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeVector_getFeatureAttributes(
        JNIEnv *env, jobject /*thiz*/, jstring path, jlong featureId) {

    if (path == nullptr || featureId < 0) return nullptr;
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p == nullptr) return nullptr;
    const std::string pathStr(p);
    env->ReleaseStringUTFChars(path, p);
    std::string json = wwbridge::getFeatureAttributes(pathStr, (long long) featureId);
    if (json.empty()) return nullptr;
    return env->NewStringUTF(json.c_str());
}

#endif // !__OHOS__
