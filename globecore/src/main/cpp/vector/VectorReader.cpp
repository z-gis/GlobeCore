#include "vector/VectorReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>
#include <cctype>

#include "gdal/cpl_error.h"
#include "gdal/gdal_priv.h"
#include "gdal/ogrsf_frmts.h"

#include "util/Log.h"
#include "vector/GdalBootstrap.h"
#include "vector/KmlStyle.h"
#include "vector/srs_resolve.h"

namespace globecore {

// ==================== native OGR 矢量读取（承袭 bridge/vector_io.cpp 的读取逻辑，输出几何而非 JSON）====================

/// 要素数兜底上限（对齐 bridge/vector_io.cpp 常量位置）：顶点预算（MAX_VECTOR_VERTICES）是主约束，
/// 此值仅作极端密集碎数据的第二道兜底。并行 build + 分片流式上传时代单次可承载十万级要素
/// （真机已验证 4M 顶点 / 550MB GPU 的全国县界层可用），10000 旧值会把中等范围截成「缺一半/零星图斑」。
/// 实际单次预算由 readVectorFile 的 maxFeatures 入参控制（≤0 时取此上限）。
static const int MAX_VECTOR_FEATURES = 200000;

/// 顶点预算（坐标对，仅对屏幕过滤加载生效）：extent 相交命中要素的累计坐标对超此值即按要素边界截断。
/// 旧值 12 万源于单线程 build 时代（4.48M 顶点→约 200s）；现逐要素并行 build + 每帧预算内分片流式上传，
/// 硬约束只剩内存/GPU 吞吐：200 万坐标对≈全国县界层（4.03M 顶点，真机验证可跑）的一半量级，
/// 堆+GPU 约数百 MB 内。放大到实地时屏幕框小、命中远低于此值→完整；停在概览时也能铺满大半个视域，
/// 不再出现按文件序截断导致的「缺一半/零星图斑」。
static const int64_t MAX_VECTOR_VERTICES = 2000000;

/// 顶点不再做任何简化/去重（用户明确要求保留全部折点）：按源几何原始顶点序列逐一输出，
/// 依赖屏幕实际范围过滤将单次命中要素数压到很低，无需靠丢点控制顶点量。

/// 摊平一条 OGR 折线到 [lon0,lat0,lon1,lat1,...]：逐点原样输出（不简化、不丢弃），始终保留首末顶点。
/// [altOut] 非空时同步收集逐顶点高程（getZ，米），与 out 的坐标对一一对齐；为空（贴地/非高程模式）
/// 则不收集，巨层零额外开销。
static void appendLineCoords(std::vector<double> &out, std::vector<double> *altOut, OGRLineString *line) {
    if (line == nullptr) return;
    const int n = line->getNumPoints();
    if (n == 0) return;
    out.reserve(out.size() + static_cast<size_t>(n) * 2);
    if (altOut != nullptr) altOut->reserve(altOut->size() + static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        out.push_back(line->getX(i));
        out.push_back(line->getY(i));
        if (altOut != nullptr) altOut->push_back(line->getZ(i));
    }
}

/// 解析 KML altitudeMode 字段串 → 高程模式（大小写不敏感）；未识别/为空一律视为贴地。
static VecAltMode parseAltMode(const char *s) {
    if (s == nullptr) return VecAltMode::Clamp;
    std::string sLower;
    for (const char *p = s; *p; ++p)
        sLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    if (sLower.find("absolute") != std::string::npos) return VecAltMode::Absolute;
    if (sLower.find("relative") != std::string::npos) return VecAltMode::Relative;
    return VecAltMode::Clamp;
}

/// 统计一个摊平要素的坐标对数（顶点预算计量：Point=1，Line=各 parts 点对之和，Polygon=外环+各洞点对之和）。
static int64_t countFeatureVerts(const VectorFeatureData &f) {
    switch (f.type) {
        case VectorGeomType::Point: return 1;
        case VectorGeomType::Line: {
            int64_t n = 0;
            for (const auto &p : f.parts) n += static_cast<int64_t>(p.size() / 2);
            return n;
        }
        case VectorGeomType::Polygon: {
            int64_t n = static_cast<int64_t>(f.outer.size() / 2);
            for (const auto &h : f.holes) n += static_cast<int64_t>(h.size() / 2);
            return n;
        }
    }
    return 0;
}

/// 按几何类型摊平到要素列表（复刻 emitGeometry 的分派语义）：
/// 点/多点各一个 POINT 要素，多线合并为一个 LINE 要素（多 parts），多面拆多个 POLYGON 要素，集合递归。
/// [label] 为该要素标注文本（已 trim，可为空），写入每个派生要素的 f.label。
static void emitGeometry(std::vector<VectorFeatureData> &out, int &count, bool &truncated, int maxFeatures,
                         int64_t &vertexTotal, int64_t vertexCap,
                         OGRGeometry *g, long long fid, const std::string &label, VecAltMode mode, bool extrude,
                         const VecFeatureStyle &style) {
    if (g == nullptr || truncated) return;
    const bool elev = (mode != VecAltMode::Clamp); // 仅高程模式才收集逐顶点 Z
    // 预算到达即停：命中要素数上限 或 顶点预算（坐标对）上限，二者任一触发即置 truncated 令上层停止遍历。
    auto reachCap = [&]() { return count >= maxFeatures || (vertexCap > 0 && vertexTotal >= vertexCap); };
    // 收尾一个派生要素：计数 + 累加顶点预算 + move 入 out；超预算则置 truncated。
    auto emit = [&](VectorFeatureData &f) {
        f.extrude = extrude; // 逐派生要素统一携带 extrude 标志（是否立体仅在 VectorBuilder 按 featElev 门控）
        f.style = style;     // 逐要素 KML 配色（同一 placemark 派生的多几何共享其样式）
        count++;
        vertexTotal += countFeatureVerts(f);
        out.push_back(std::move(f));
        if (reachCap()) truncated = true;
    };
    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPoint: {
            if (reachCap()) { truncated = true; return; }
            OGRPoint *pt = (OGRPoint *) g;
            VectorFeatureData f;
            f.type = VectorGeomType::Point;
            f.fid = fid;
            f.label = label;
            f.altMode = mode;
            f.lon = pt->getX();
            f.lat = pt->getY();
            if (elev) f.alt = pt->getZ();
            emit(f);
            break;
        }
        case wkbMultiPoint: {
            OGRMultiPoint *mp = (OGRMultiPoint *) g;
            for (int i = 0; i < mp->getNumGeometries(); i++) {
                if (reachCap()) { truncated = true; return; }
                OGRPoint *pt = (OGRPoint *) mp->getGeometryRef(i);
                VectorFeatureData f;
                f.type = VectorGeomType::Point;
                f.fid = fid;
                f.label = label;
                f.altMode = mode;
                f.lon = pt->getX();
                f.lat = pt->getY();
                if (elev) f.alt = pt->getZ();
                emit(f);
                if (truncated) return;
            }
            break;
        }
        case wkbLineString: {
            if (reachCap()) { truncated = true; return; }
            VectorFeatureData f;
            f.type = VectorGeomType::Line;
            f.fid = fid;
            f.label = label;
            f.altMode = mode;
            f.parts.emplace_back();
            if (elev) {
                f.partsAlt.emplace_back();
                appendLineCoords(f.parts.back(), &f.partsAlt.back(), (OGRLineString *) g);
            } else {
                appendLineCoords(f.parts.back(), nullptr, (OGRLineString *) g);
            }
            emit(f);
            break;
        }
        case wkbMultiLineString: {
            if (reachCap()) { truncated = true; return; }
            OGRMultiLineString *ml = (OGRMultiLineString *) g;
            VectorFeatureData f;
            f.type = VectorGeomType::Line;
            f.fid = fid;
            f.label = label;
            f.altMode = mode;
            for (int i = 0; i < ml->getNumGeometries(); i++) {
                f.parts.emplace_back();
                if (elev) {
                    f.partsAlt.emplace_back();
                    appendLineCoords(f.parts.back(), &f.partsAlt.back(),
                                     (OGRLineString *) ml->getGeometryRef(i));
                } else {
                    appendLineCoords(f.parts.back(), nullptr,
                                     (OGRLineString *) ml->getGeometryRef(i));
                }
            }
            emit(f);
            break;
        }
        case wkbPolygon: {
            if (reachCap()) { truncated = true; return; }
            OGRPolygon *poly = (OGRPolygon *) g;
            OGRLineString *outer = poly->getExteriorRing();
            if (outer == nullptr) return;
            VectorFeatureData f;
            f.type = VectorGeomType::Polygon;
            f.fid = fid;
            f.label = label;
            f.altMode = mode;
            if (elev) {
                appendLineCoords(f.outer, &f.outerAlt, outer);
            } else {
                appendLineCoords(f.outer, nullptr, outer);
            }
            const int rings = poly->getNumInteriorRings();
            for (int i = 0; i < rings; i++) {
                f.holes.emplace_back();
                if (elev) {
                    f.holesAlt.emplace_back();
                    appendLineCoords(f.holes.back(), &f.holesAlt.back(), poly->getInteriorRing(i));
                } else {
                    appendLineCoords(f.holes.back(), nullptr, poly->getInteriorRing(i));
                }
            }
            emit(f);
            break;
        }
        case wkbMultiPolygon: {
            OGRMultiPolygon *mpoly = (OGRMultiPolygon *) g;
            for (int i = 0; i < mpoly->getNumGeometries(); i++) {
                if (reachCap()) { truncated = true; return; }
                OGRPolygon *poly = (OGRPolygon *) mpoly->getGeometryRef(i);
                OGRLineString *outer = poly->getExteriorRing();
                if (outer == nullptr) continue;
                VectorFeatureData f;
                f.type = VectorGeomType::Polygon;
                f.fid = fid;
                f.label = label;
                f.altMode = mode;
                if (elev) {
                    appendLineCoords(f.outer, &f.outerAlt, outer);
                } else {
                    appendLineCoords(f.outer, nullptr, outer);
                }
                const int rings = poly->getNumInteriorRings();
                for (int r = 0; r < rings; r++) {
                    f.holes.emplace_back();
                    if (elev) {
                        f.holesAlt.emplace_back();
                        appendLineCoords(f.holes.back(), &f.holesAlt.back(), poly->getInteriorRing(r));
                    } else {
                        appendLineCoords(f.holes.back(), nullptr, poly->getInteriorRing(r));
                    }
                }
                emit(f);
                if (truncated) return;
            }
            break;
        }
        case wkbGeometryCollection: {
            OGRGeometryCollection *gc = (OGRGeometryCollection *) g;
            for (int i = 0; i < gc->getNumGeometries(); i++) {
                emitGeometry(out, count, truncated, maxFeatures, vertexTotal, vertexCap,
                             gc->getGeometryRef(i), fid, label, mode, extrude, style);
                if (truncated) return;
            }
            break;
        }
        default:
            break;
    }
}

/// 去除首尾空白（空格/制表符/换行/回车）；对齐原主界面 labelTextOf 的 trim 口径。
static std::string trimCopy(const char *s) {
    if (s == nullptr) return std::string();
    std::string r(s);
    const char *ws = " \t\n\r\f\v";
    const size_t b = r.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const size_t e = r.find_last_not_of(ws);
    return r.substr(b, e - b + 1);
}

/// 路径后缀是否为 .kml/.kmz（大小写不敏感），供 KML 可行性探针门控（仅这两种扩展名触发）。
static bool isKmlPath(const std::string &path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".kml" || ext == ".kmz";
}

/// 下降到几何集合的首个叶子几何（多级集合仅探第一支，够判维度/Z）。
static OGRGeometry *firstLeafGeom(OGRGeometry *g) {
    while (g != nullptr && wkbFlatten(g->getGeometryType()) == wkbGeometryCollection) {
        OGRGeometryCollection *gc = (OGRGeometryCollection *) g;
        g = gc->getNumGeometries() > 0 ? gc->getGeometryRef(0) : nullptr;
    }
    return g;
}

/// 取要素几何「前两个坐标点」的 lon/lat，量化为 "%.7f_%.7f[_%.7f_%.7f]"，与 KmlStyle::coordSig 同格式，用于逐要素配色兜底关联。
/// 下降到 Multi*/GeometryCollection 的首个子几何（合并时=首个子面）；KML 源为经纬度（X=lon、Y=lat）。
static std::string kmlGeomSig(OGRGeometry *g) {
    while (g != nullptr) {
        const OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
        if (t == wkbGeometryCollection || t == wkbMultiPoint || t == wkbMultiLineString || t == wkbMultiPolygon) {
            OGRGeometryCollection *gc = (OGRGeometryCollection *) g;
            g = gc->getNumGeometries() > 0 ? gc->getGeometryRef(0) : nullptr;
        } else break;
    }
    if (g == nullptr) return std::string();
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    int npts = 1;
    const OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
    if (t == wkbPoint) {
        OGRPoint *p = (OGRPoint *) g; ax = p->getX(); ay = p->getY();
    } else if (t == wkbLineString) {
        OGRLineString *l = (OGRLineString *) g;
        const int m = l->getNumPoints();
        if (m < 1) return std::string();
        ax = l->getX(0); ay = l->getY(0);
        if (m >= 2) { bx = l->getX(1); by = l->getY(1); npts = 2; }
    } else if (t == wkbPolygon) {
        OGRPolygon *poly = (OGRPolygon *) g;
        OGRLinearRing *r = poly->getExteriorRing();
        if (r == nullptr) return std::string();
        const int m = r->getNumPoints();
        if (m < 1) return std::string();
        ax = r->getX(0); ay = r->getY(0);
        if (m >= 2) { bx = r->getX(1); by = r->getY(1); npts = 2; }
    } else {
        return std::string();
    }
    char b[96];
    if (npts >= 2) snprintf(b, sizeof(b), "%.7f_%.7f_%.7f_%.7f", ax, ay, bx, by);
    else snprintf(b, sizeof(b), "%.7f_%.7f", ax, ay);
    return std::string(b);
}

/**
 * KML/KMZ 可行性探针（一次性、纯只读日志）：实测 LIBKML 读出的
 *  1) 图层字段模式（暴露 altitudeMode/extrude/styleUrl 及可能的配色派生列）；
 *  2) 几何维度（原始/拍平类型 + 是否含 Z）；
 *  3) 逐顶点 Z 抽样（点/线/面首若干顶点，判高程是否真在几何里）；
 *  4) 首要素字段值（重点看 altitudeMode/extrude/styleUrl 取值）。
 * 不改几何、不参与后续渲染，仅 LOGI 一行 [KMLProbe]。由调用方按「首图层首要素一次」封顶。
 */
static void probeKmlFeature(OGRLayer *layer, OGRFeature *feat, OGRGeometry *g) {
    if (layer == nullptr || g == nullptr) return;
    std::string msg = "[KMLProbe] layer=";
    msg += (layer->GetName() ? layer->GetName() : "?");

    OGRFeatureDefn *defn = layer->GetLayerDefn();

    // 1) 图层字段模式 name(type)
    msg += " fields=[";
    if (defn != nullptr) {
        for (int i = 0; i < defn->GetFieldCount(); i++) {
            if (i) msg += ",";
            msg += defn->GetFieldDefn(i)->GetNameRef();
            msg += "(";
            msg += OGR_GetFieldTypeName(defn->GetFieldDefn(i)->GetType());
            msg += ")";
        }
    }
    msg += "]";

    // 2) 几何维度：原始类型（含 25D 位）、拍平类型、是否 3D
    msg += " geomRaw=" + std::to_string(static_cast<int>(g->getGeometryType()));
    msg += " geomFlat=" + std::to_string(static_cast<int>(wkbFlatten(g->getGeometryType())));
    msg += " is3D=" + std::to_string(g->Is3D() ? 1 : 0);

    // 3) 逐顶点 Z 抽样（下降到叶子几何）
    OGRGeometry *leaf = firstLeafGeom(g);
    if (leaf != nullptr) {
        char b[64];
        switch (wkbFlatten(leaf->getGeometryType())) {
            case wkbPoint: {
                OGRPoint *p = (OGRPoint *) leaf;
                snprintf(b, sizeof(b), " pointZ=[%.3f]", p->getZ());
                msg += b;
                break;
            }
            case wkbLineString: {
                OGRLineString *l = (OGRLineString *) leaf;
                const int n = l->getNumPoints();
                std::string zs;
                for (int i = 0; i < n && i < 3; i++) {
                    if (i) zs += ",";
                    snprintf(b, sizeof(b), "%.3f", l->getZ(i));
                    zs += b;
                }
                msg += " lineZfirst3=[" + zs + "] n=" + std::to_string(n);
                break;
            }
            case wkbPolygon: {
                OGRPolygon *poly = (OGRPolygon *) leaf;
                OGRLineString *r = poly->getExteriorRing();
                if (r != nullptr) {
                    const int n = r->getNumPoints();
                    std::string zs;
                    for (int i = 0; i < n && i < 3; i++) {
                        if (i) zs += ",";
                        snprintf(b, sizeof(b), "%.3f", r->getZ(i));
                        zs += b;
                    }
                    msg += " ringZfirst3=[" + zs + "] n=" + std::to_string(n);
                }
                break;
            }
            default:
                msg += " zSample=unhandled";
        }
    }

    // 4) 首要素字段值（前 14 条）
    if (defn != nullptr && feat != nullptr) {
        msg += " vals={";
        const int fc = defn->GetFieldCount();
        for (int i = 0; i < fc && i < 14; i++) {
            if (i) msg += "; ";
            msg += defn->GetFieldDefn(i)->GetNameRef();
            msg += "=";
            const char *v = feat->GetFieldAsString(i);
            if (v != nullptr) msg += v;
        }
        msg += "}";
    }

    LOGI("%s", msg.c_str());
}

/// 读取单个图层的要素：解析源 SRS，非空时重投影到 WGS84 后摊平。
/// [hasExtent] 为 true 时按屏幕 WGS84 矩形 [minLon,minLat,maxLon,maxLat] 空间过滤，复刻
/// bridge/vector_io.cpp 的「四重防线」（图层四至预判整层跳过 + SetSpatialFilterRect 走 .qix/R-tree 索引 +
/// 逐要素 envelope 精确过滤 + maxFeatures 兜底）；[labelField] 非空且图层含该字段时逐要素取标注文本。
static void emitLayerFeatures(std::vector<VectorFeatureData> &out, OGRLayer *layer, const char *path,
                              int &count, bool &truncated, const std::string &labelField,
                              bool hasExtent, double minLon, double minLat, double maxLon, double maxLat,
                              int maxFeatures, int64_t &vertexTotal, int64_t vertexCap, bool &doProbe,
                              const KmlStyleIndex *kmlIdx,
                              const std::unordered_map<long long, std::string> *labelFieldOverrides) {
    if (layer == nullptr || truncated) return;

    // 标注字段索引：labelField 为空或图层无该字段时为 -1（不读标注）。
    const int labelIdx = labelField.empty() ? -1
                                            : layer->GetLayerDefn()->GetFieldIndex(labelField.c_str());
    // 逐要素标注覆盖字段索引缓存（按字段名，图层级）：避免每要素对同一覆盖字段重复 GetFieldIndex。
    OGRFeatureDefn *featDefn = layer->GetLayerDefn();
    std::map<std::string, int> fieldIdxCache;
    if (labelIdx >= 0) fieldIdxCache[labelField] = labelIdx;
    auto fieldIdx = [&](const std::string &name) -> int {
        const auto it = fieldIdxCache.find(name);
        if (it != fieldIdxCache.end()) return it->second;
        const int idx = featDefn ? featDefn->GetFieldIndex(name.c_str()) : -1;
        fieldIdxCache[name] = idx;
        return idx;
    };
    // 高程模式字段索引（KML/LIBKML 暴露 altitudeMode；非 KML 无此字段 → -1，恒按贴地）。
    const int altModeIdx = layer->GetLayerDefn()->GetFieldIndex("altitudeMode");
    // 拉伸字段索引（KML/LIBKML 暴露 extrude，Integer）；非 KML → -1，恒不拉伸。
    const int extrudeIdx = layer->GetLayerDefn()->GetFieldIndex("extrude");
    // Name 字段索引（Phase 3 逐要素配色的关联键）：仅当有 kmlStyles 时才用；非 KML → -1。
    const int nameIdx = layer->GetLayerDefn()->GetFieldIndex("Name");

    // 源 SRS → EPSG:4326 的坐标变换（传统 GIS 轴序）。解析不到 SRS 时 ct=nullptr，坐标按原样输出。
    OGRCoordinateTransformation *ct = nullptr;
    OGRSpatialReference *srcRef = resolveSourceSrs(path, layer);
    if (srcRef != nullptr) {
        OGRSpatialReference dstRef;
        dstRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dstRef.SetWellKnownGeogCS("WGS84");
        ct = OGRCreateCoordinateTransformation(srcRef, &dstRef);
        if (ct == nullptr) {
            LOGE("图层[%s]创建坐标变换失败，坐标将按源坐标系原样输出", layer->GetName());
        }
    }

    // 防线 1、2：图层四至预判 + 比例映射构造源坐标系过滤 box（喂 SetSpatialFilterRect 走索引）。
    // 不依赖 WGS84→源 SRS 反向变换（CGCS2000 高斯-克吕格等投影下反向变换会静默失败），
    // 改用正向 ct 把图层原始四至变换到 WGS84 与屏幕比对、再按归一化比例反投到源坐标系 box。
    bool skipLayer = false;
    if (hasExtent) {
        OGREnvelope rawExt;
        bool haveRawExt = (layer->GetExtent(&rawExt, TRUE) == OGRERR_NONE);
        double lMinLon = 0, lMaxLon = 0, lMinLat = 0, lMaxLat = 0;
        bool haveWgs84Ext = false;
        if (haveRawExt) {
            if (ct != nullptr && srcRef != nullptr && !srcRef->IsGeographic()) {
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
                lMinLon = rawExt.MinX; lMaxLon = rawExt.MaxX;
                lMinLat = rawExt.MinY; lMaxLat = rawExt.MaxY;
                haveWgs84Ext = true;
            }
        }
        if (haveWgs84Ext) {
            if (lMaxLon < minLon || lMinLon > maxLon || lMaxLat < minLat || lMinLat > maxLat) {
                skipLayer = true;  // 防线 1：整层与屏幕不相交，跳过
                LOGI("图层[%s]与屏幕四至不相交，整层跳过", layer->GetName());
            } else if (haveRawExt) {
                // 防线 2：屏幕 box 在图层 WGS84 四至内的归一化位置按比例映射到源坐标系四至，加缓冲
                double fMinX, fMinY, fMaxX, fMaxY;
                if (srcRef != nullptr && srcRef->IsGeographic()) {
                    fMinX = minLon; fMaxX = maxLon; fMinY = minLat; fMaxY = maxLat;
                } else {
                    const double wLon = lMaxLon - lMinLon;
                    const double wLat = lMaxLat - lMinLat;
                    if (wLon <= 0 || wLat <= 0) {
                        fMinX = fMinY = fMaxX = fMaxY = 0;  // 退化四至（点/线图层）：不设源过滤，靠防线 3
                    } else {
                        const double fx1 = std::max(0.0, std::min(1.0, (minLon - lMinLon) / wLon));
                        const double fx2 = std::max(0.0, std::min(1.0, (maxLon - lMinLon) / wLon));
                        const double fy1 = std::max(0.0, std::min(1.0, (minLat - lMinLat) / wLat));
                        const double fy2 = std::max(0.0, std::min(1.0, (maxLat - lMinLat) / wLat));
                        const double wX = rawExt.MaxX - rawExt.MinX;
                        const double wY = rawExt.MaxY - rawExt.MinY;
                        fMinX = rawExt.MinX + fx1 * wX;
                        fMaxX = rawExt.MinX + fx2 * wX;
                        fMinY = rawExt.MinY + fy1 * wY;
                        fMaxY = rawExt.MinY + fy2 * wY;
                        // 缓冲 15% + 500m（取大者）覆盖比例映射的非线性误差，多余候选由防线 3 精确跳过
                        const double bufX = std::max((fMaxX - fMinX) * 0.15, 500.0);
                        const double bufY = std::max((fMaxY - fMinY) * 0.15, 500.0);
                        fMinX -= bufX; fMaxX += bufX;
                        fMinY -= bufY; fMaxY += bufY;
                        fMinX = std::max(fMinX, rawExt.MinX);
                        fMaxX = std::min(fMaxX, rawExt.MaxX);
                        fMinY = std::max(fMinY, rawExt.MinY);
                        fMaxY = std::min(fMaxY, rawExt.MaxY);
                    }
                }
                if (fMaxX > fMinX && fMaxY > fMinY) {
                    layer->SetSpatialFilterRect(fMinX, fMinY, fMaxX, fMaxY);
                }
            }
        }
    }

    if (!skipLayer) {
        layer->ResetReading();
        OGRFeature *feat;
        while (!truncated && (feat = layer->GetNextFeature()) != nullptr) {
            OGRGeometry *geom = feat->GetGeometryRef();
            if (geom != nullptr) {
                // 兜底配色关联键：几何首点坐标签名（须在重投影前取，KML 源即 WGS84 经纬度）。
                //   仅当整层无名字（byName 为空、纯无名导出）时才需，避免给有名文件做无用计算。
                std::string ksig;
                if (kmlIdx != nullptr && kmlIdx->byName.empty()) ksig = kmlGeomSig(geom);
                bool pass = true;
                // 防线 3：候选要素仅变换 envelope 4 角到 WGS84 判相交，跳过屏外要素避免整几何 transform
                if (hasExtent && ct != nullptr) {
                    OGREnvelope srcEnv;
                    geom->getEnvelope(&srcEnv);
                    double xs[4] = {srcEnv.MinX, srcEnv.MaxX, srcEnv.MinX, srcEnv.MaxX};
                    double ys[4] = {srcEnv.MinY, srcEnv.MinY, srcEnv.MaxY, srcEnv.MaxY};
                    if (ct->Transform(4, xs, ys)) {
                        const double wMinX = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
                        const double wMaxX = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
                        const double wMinY = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
                        const double wMaxY = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
                        if (wMaxX < minLon || wMinX > maxLon || wMaxY < minLat || wMinY > maxLat) {
                            pass = false;
                        }
                    }
                    // Transform 失败时保守通过，由后续全变换 + 渲染兜底
                }
                if (pass) {
                    // 就地变换到 WGS84（几何归要素所有，DestroyFeature 时一并释放）
                    if (ct != nullptr) geom->transform(ct);
                    // 标注文本：命中单要素标注覆盖（按 fid）优先——非空字段名取该字段值、空串显式关闭；
                    // 未命中覆盖则沿用整层 labelField（fid 键与 buildOne/拾取同源，同一 GDAL 链路内稳定）。
                    std::string label;
                    const long long featFid = (long long) feat->GetFID();
                    const std::string *ovField = nullptr;
                    if (labelFieldOverrides != nullptr) {
                        const auto oit = labelFieldOverrides->find(featFid);
                        if (oit != labelFieldOverrides->end()) ovField = &oit->second;
                    }
                    if (ovField != nullptr) {
                        if (!ovField->empty()) {
                            const int oi = fieldIdx(*ovField);
                            if (oi >= 0 && feat->IsFieldSet(oi)) label = trimCopy(feat->GetFieldAsString(oi));
                        }
                    } else if (labelIdx >= 0 && feat->IsFieldSet(labelIdx)) {
                        label = trimCopy(feat->GetFieldAsString(labelIdx));
                    }
                    // 逐要素高程模式（读 altitudeMode 字段；无字段/未设 → 贴地）
                    VecAltMode mode = VecAltMode::Clamp;
                    if (altModeIdx >= 0 && feat->IsFieldSetAndNotNull(altModeIdx)) {
                        mode = parseAltMode(feat->GetFieldAsString(altModeIdx));
                    }
                    // 逐要素拉伸标志（读 extrude 字段，1=拉伸）；仅在 mode 非 Clamp 时后续才有意义。
                    bool extrude = false;
                    if (extrudeIdx >= 0 && feat->IsFieldSetAndNotNull(extrudeIdx)) {
                        extrude = feat->GetFieldAsInteger(extrudeIdx) == 1;
                    }
                    // KML/KMZ 可行性探针：仅首图层首通过要素触发一次（doProbe 跨层共享，触发即归零），
                    // 实测 LIBKML 是否交付 Z 高程与 altitudeMode/extrude/styleUrl 字段，不改几何。
                    if (doProbe) {
                        probeKmlFeature(layer, feat, geom);
                        doProbe = false;
                    }
                    // 逐要素 KML 配色（Phase 3+）：GDAL 不暴露 styleUrl/颜色。
                    //   主路径按 Placemark name 关联（有名文件如 KML_Samples，行为同旧）；
                    //   仅当整层无有名要素（byName 为空，如宁德时代的空名+styleUrl 导出）时，按几何首点坐标签名查 byGeomSig 兜底。
                    //   签名缺失或该位空色 → 不采用，回退整层默认，绝不产生错色。
                    VecFeatureStyle fs;
                    if (kmlIdx != nullptr) {
                        const KmlStyleColors *picked = nullptr;
                        if (!kmlIdx->byName.empty() && nameIdx >= 0 && feat->IsFieldSetAndNotNull(nameIdx)) {
                            const std::string nm = trimCopy(feat->GetFieldAsString(nameIdx));
                            if (!nm.empty()) {
                                const auto it = kmlIdx->byName.find(nm);
                                if (it != kmlIdx->byName.end()) picked = &it->second;
                            }
                        }
                        // 无名兜底：按几何首点坐标签名查表（对 GDAL 的 MultiGeometry 拆分/要素顺序/屏幕过滤全免疫；仅在整层无名字时）。
                        if (picked == nullptr && kmlIdx->byName.empty() && !ksig.empty()) {
                            const auto it = kmlIdx->byGeomSig.find(ksig);
                            if (it != kmlIdx->byGeomSig.end() && (it->second.hasFill || it->second.hasLine))
                                picked = &it->second;
                        }
                        if (picked != nullptr) {
                            fs.hasFill = picked->hasFill;
                            fs.hasLine = picked->hasLine;
                            for (int ci = 0; ci < 4; ++ci) { fs.fill[ci] = picked->fill[ci]; fs.line[ci] = picked->line[ci]; }
                            fs.valid = picked->hasFill || picked->hasLine;
                        }
                    }
                    // 防线 4：maxFeatures 兜底 + 顶点预算（emitGeometry 内命中任一上限置 truncated）
                    emitGeometry(out, count, truncated, maxFeatures, vertexTotal, vertexCap, geom,
                                 featFid, label, mode, extrude, fs);
                }
            }
            OGRFeature::DestroyFeature(feat);
        }
    }

    // 清理本层设置的空间过滤，避免影响同一 dataset 的其他图层（如 dwg 多子图层）
    if (hasExtent) layer->SetSpatialFilter(nullptr);
    if (ct != nullptr) OGRCoordinateTransformation::DestroyCT(ct);
    if (srcRef != nullptr) srcRef->Release();
}

/// 用一组摊平坐标 [x0,y0,x1,y1,...] 扩展 bbox
static void expandBBox(const std::vector<double> &flat, bool &has, double &minX, double &minY,
                       double &maxX, double &maxY) {
    for (size_t i = 0; i + 1 < flat.size(); i += 2) {
        const double x = flat[i];
        const double y = flat[i + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        if (!has) { minX = maxX = x; minY = maxY = y; has = true; continue; }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
}

VectorReadResult readVectorFile(const std::string &path, const std::string &labelField,
                                bool hasExtent, double minLon, double minLat,
                                double maxLon, double maxLat, int maxFeatures,
                                const std::unordered_map<long long, std::string> *labelFieldOverrides) {
    VectorReadResult result;
    ensureGdalRegistered();

    // 单次加载预算：入参 >0 时按其收敛到硬上限内，≤0 时直接取硬上限（小数据全量/兜底）。
    const int cap = (maxFeatures > 0)
                    ? std::min(maxFeatures, MAX_VECTOR_FEATURES) : MAX_VECTOR_FEATURES;

    // ── .qix 空间索引自动创建（仅 shp + 屏幕过滤时）──
    // 无 .qix 时 SetSpatialFilterRect 仍需顺序扫描全部记录头，10 万级耗时 ~0.5–1s；
    // 创建后后续重载可走 R-tree 索引。仅首次缺失时以 UPDATE 打开建索引，失败静默回退顺序扫描。
    if (hasExtent && path.size() > 4) {
        std::string ext = path.substr(path.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".shp") {
            const std::string qixPath = path.substr(0, path.size() - 4) + ".qix";
            if (access(qixPath.c_str(), F_OK) != 0) {
                GDALDataset *dsW = (GDALDataset *) GDALOpenEx(path.c_str(),
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
                        if (access(qixPath.c_str(), F_OK) == 0)
                            LOGI("已为 [%s] 创建 .qix 空间索引（后续加载将更快）", lyr->GetName());
                    }
                    GDALClose(dsW);
                }
            }
        }
    }

    // 耗时打点：打开/读取分段记录，便于真机核对大文件加载慢点（KML/DXF 打开即全文件解析）
    const auto t0 = std::chrono::steady_clock::now();
    GDALDataset *ds = (GDALDataset *) GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                 nullptr, nullptr, nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    if (ds == nullptr) {
        LOGW("GDALOpenEx 打开矢量数据失败: %s", path.c_str());
        result.error = "打开矢量数据失败";
        return result;
    }

    // CAD 无坐标系（无同名 .prj 且坐标不含投影带号）：米制坐标无法定位，跳过加载并回传原因（复刻 app 逻辑）。
    if (cadLacksSrs(path.c_str(), ds->GetLayer(0))) {
        GDALClose(ds);
        result.error = "该 CAD 数据无坐标系信息（无同名 .prj 且坐标不含投影带号），无法定位";
        return result;
    }

    int count = 0;
    bool truncated = false;
    // 顶点预算：仅屏幕过滤加载时启用（概览密集数据防单次 build 顶点爆炸）；direct 全量层不设限（=0）。
    int64_t vertexTotal = 0;
    const int64_t vertexCap = hasExtent ? MAX_VECTOR_VERTICES : 0;
    const int layerCount = ds->GetLayerCount();
    // KML/KMZ 可行性探针开关：仅本次加载生效，跨图层共享，首个通过要素打印后即关闭。
    const bool isKml = isKmlPath(path);
    bool doProbe = isKml;
    // Phase 3 逐要素配色：KML/KMZ 才自建解析样式（GDAL/LIBKML 不返回颜色/styleUrl）；非 KML 或解析空 → 整层默认。
    KmlStyleIndex kmlStyles;
    if (isKml) kmlStyles = parseKmlStyleMap(path);
    const KmlStyleIndex *kmlStylesPtr = kmlStyles.empty() ? nullptr : &kmlStyles;
    // 要素容量预留：仅全量模式（无屏幕过滤）按各图层要素总数一次性扩容，免数千要素逐个
    // push_back 反复 realloc 搬迁；屏幕过滤模式命中量取决于视口，不据此预留（避免大文件高估）。
    if (!hasExtent) {
        long long est = 0;
        for (int li = 0; li < layerCount; li++) {
            const long long c = ds->GetLayer(li)->GetFeatureCount(false);
            if (c > 0) est += c;
        }
        if (est > 0)
            result.features.reserve(static_cast<size_t>(std::min<long long>(est, cap)));
    }
    for (int li = 0; li < layerCount && !truncated; li++) {
        emitLayerFeatures(result.features, ds->GetLayer(li), path.c_str(), count, truncated, labelField,
                          hasExtent, minLon, minLat, maxLon, maxLat, cap, vertexTotal, vertexCap, doProbe,
                          kmlStylesPtr, labelFieldOverrides);
    }
    const auto t2 = std::chrono::steady_clock::now();
    if (truncated) {
        if (vertexCap > 0 && vertexTotal >= vertexCap)
            LOGW("[VecReload] 顶点预算 %lld 命中已截断: verts=%lld features=%d path=%s",
                 static_cast<long long>(vertexCap), static_cast<long long>(vertexTotal), count, path.c_str());
        else
            LOGW("要素数超过上限 %d，已截断: %s", cap, path.c_str());
    }
    GDALClose(ds);

    // 计算 WGS84 四至（供图层原点 RTC）：遍历全部要素坐标取极值
    bool has = false;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const auto &f : result.features) {
        if (f.type == VectorGeomType::Point) {
            if (std::isfinite(f.lon) && std::isfinite(f.lat)) {
                if (!has) { minX = maxX = f.lon; minY = maxY = f.lat; has = true; }
                else {
                    minX = std::min(minX, f.lon); maxX = std::max(maxX, f.lon);
                    minY = std::min(minY, f.lat); maxY = std::max(maxY, f.lat);
                }
            }
        } else if (f.type == VectorGeomType::Line) {
            for (const auto &part : f.parts) expandBBox(part, has, minX, minY, maxX, maxY);
        } else { // Polygon
            expandBBox(f.outer, has, minX, minY, maxX, maxY);
            for (const auto &hole : f.holes) expandBBox(hole, has, minX, minY, maxX, maxY);
        }
    }
    result.hasBBox = has;
    result.hasElevation = false;
    for (const auto &f : result.features) {
        if (f.altMode != VecAltMode::Clamp) { result.hasElevation = true; break; }
    }
    result.minLon = minX;
    result.minLat = minY;
    result.maxLon = maxX;
    result.maxLat = maxY;

    result.ok = true;
    LOGI("[VecReload] readVectorFile 完成 open=%lldms read=%lldms features=%zu verts=%lld bbox=[%.6f,%.6f,%.6f,%.6f] path=%s",
         std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
         std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
         result.features.size(), static_cast<long long>(vertexTotal), minX, minY, maxX, maxY, path.c_str());
    return result;
}

} // namespace globecore
