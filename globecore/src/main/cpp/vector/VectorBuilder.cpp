#include "vector/VectorBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "globe/MercatorProjection.h"
#include "util/Log.h"

// earcut 为 header-only 模板实现（mapbox），以 #include 方式引入，非独立编译单元
#include "earcut.cpp"

namespace globecore {
namespace {

/// miter 接头长度上限（相对半宽倍数）：尖角处钳制，避免长尖刺
constexpr float kMiterLimit = 4.0f;

/// 并行阈值：要素数与源顶点量同时达阈才起多线程（小数据线程开销 > 收益，直接串行更快）。
constexpr size_t kParallelMinFeatures = 4;
constexpr size_t kParallelMinVerts = 20000;
/// 每线程目标工作量（源坐标对）：据此由顶点总量推算期望线程数，再被核数与令牌预算钳制。
constexpr size_t kVertsPerThread = 20000;

inline void pushVert(std::vector<float> &v, float x, float y, float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// 线/描边顶点：位置为中心线点 (x,y)，(mx,my) 为接头/端点法向（含长度系数），
// 着色器按 aPos + aMiter*uHalfWidthWorld 偏移 → 屏幕空间恒定像素宽
inline void pushLineVert(std::vector<float> &v, float x, float y, float mx, float my,
                         float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(mx);
    v.push_back(my);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

/**
 * 单线程 build 的中间输出分片：字段与 [VectorGeometry] 的顶点/区间/图元/标注一一对应，但【不含 origin】
 * （origin 由整层按 bbox 统一算出、各分片共享）。并行时每个分片由一个「块」（连续要素区间）独占写入，
 * 块间无共享可变状态；串行时仅一个分片。合并阶段按块序拼接并对 range 顶点索引做累计偏移重映射。
 */
struct GeoShard {
    std::vector<float> fillVerts;
    std::vector<float> outlineVerts;
    std::vector<std::pair<int, int>> outlineRanges;
    std::vector<float> lineVerts;
    std::vector<std::pair<int, int>> lineRanges;
    std::vector<float> pointCoords;
    // 逐顶点高程并行数组（仅 hasElev 时填充，与各顶点组顶点对齐；否则保持空）
    std::vector<float> fillAlt, fillMode;
    std::vector<float> outlineAlt, outlineMode;
    std::vector<float> lineAlt, lineMode;
    std::vector<float> pointAlt, pointMode;
    // extrude 侧墙顶点（[x,y,r,g,b,a]）+ 并行高程/模式：仅拉伸高程要素生成，走独立深度 pass。
    std::vector<float> extrudeVerts, extrudeAlt, extrudeMode;
    std::vector<PickPrimitive> pickPrims;
    std::vector<LabelItem> labels;
    // 逐要素顶点区间（与本块要素顺序一一对应，buildOne 前后量差记录，合并时重映射为全层偏移）
    std::vector<FeatureSpans> spans;
    // 分段累计耗时（毫秒）：并行合并后为各分片之和（=跨线程累计 CPU 时间，可大于单轮墙钟）。
    double msEar = 0.0, msFill = 0.0, msOutline = 0.0, msPick = 0.0;
};

/// 一个线程的要素烘焙器：仅持只读的 origin/style，把要素逐个烘焙进调用方给定的分片。
/// 所有方法都是纯计算、不改动自身状态；多线程各用独立 FeatureBaker + 独立分片 → 无数据竞争。
/// 计算口径与并行化前的串行 buildGeometry 完全一致（逐字搬移），确保合并结果与串行逐字节相同。
struct FeatureBaker {
    using BgClock = std::chrono::steady_clock;
    double originWx;
    double originWy;
    const VectorStyle &s;
    bool hasElev; // 本层是否含逐要素高程（决定是否为各顶点组额外烘焙 alt/mode）

    FeatureBaker(double ox, double oy, const VectorStyle &st, bool elev)
        : originWx(ox), originWy(oy), s(st), hasElev(elev) {}

    // 经纬度 → 相对原点世界坐标（double 精度，供 earcut 与顶点烘焙）
    void relD(double lon, double lat, double &rx, double &ry) const {
        double wx = 0.0, wy = 0.0;
        MercatorProjection::lonLatToWorld(lon, lat, wx, wy);
        rx = wx - originWx;
        ry = wy - originWy;
    }

    void relF(double lon, double lat, float &rx, float &ry) const {
        double dx = 0.0, dy = 0.0;
        relD(lon, lat, dx, dy);
        rx = static_cast<float>(dx);
        ry = static_cast<float>(dy);
    }

    // 追加一条屏幕空间等宽三角带（miter 接头）到指定顶点/区间组。
    // ring 为经纬度对；先转相对原点世界坐标，再逐角烘焙 miter 法向 [x,y,mx,my,r,g,b,a]（8 floats），
    // 每角生成 +miter / -miter 两顶点，以 GL_TRIANGLE_STRIP 绘制；闭合环（首尾重合）绕回接头并封口。
    // 高程：[ringAlt] 非空时按逐顶点高程收集，[mode] 为 0/1；altOut/modeOut 非空则与 verts 逐顶点同步
    // 追加（每角两顶点共享同角高程）；altOut 为空（非高程层）则不收集。
    void appendLineStrip(const std::vector<double> &ring, std::vector<float> &verts,
                         std::vector<std::pair<int, int>> &ranges,
                         float cr, float cg, float cb, float ca,
                         const std::vector<double> *ringAlt, float mode,
                         std::vector<float> *altOut, std::vector<float> *modeOut) const {
        if (ring.size() < 4) return; // 至少 2 个坐标对
        std::vector<float> pts;
        std::vector<float> alts;
        pts.reserve(ring.size());
        if (ringAlt != nullptr) alts.reserve(ring.size() / 2);
        size_t vk = 0;
        for (size_t i = 0; i + 1 < ring.size(); i += 2) {
            float rx = 0.0f, ry = 0.0f;
            relF(ring[i], ring[i + 1], rx, ry);
            pts.push_back(rx);
            pts.push_back(ry);
            if (ringAlt != nullptr)
                alts.push_back(vk < ringAlt->size() ? static_cast<float>((*ringAlt)[vk]) : 0.0f);
            vk++;
        }
        const int pc = static_cast<int>(pts.size() / 2); // 点数
        if (pc < 2) return;
        const bool closed = (std::fabs(pts[0] - pts[(pc - 1) * 2]) < 1e-6f &&
                             std::fabs(pts[1] - pts[(pc - 1) * 2 + 1]) < 1e-6f);
        const int n = closed ? (pc - 1) : pc; // 闭合环去掉重复末点
        if (n < 2) return;

        // 段 i（点 i → 点 i+1，闭合环绕回）的单位法向 perp(normalize(d))
        auto segNormal = [&](int i, float &nx, float &ny) -> bool {
            const int a = i % n, b = (i + 1) % n;
            const float dx = pts[b * 2] - pts[a * 2];
            const float dy = pts[b * 2 + 1] - pts[a * 2 + 1];
            const float L = std::sqrt(dx * dx + dy * dy);
            if (L < 1e-9f) return false;
            nx = -dy / L;
            ny = dx / L;
            return true;
        };

        const int first = static_cast<int>(verts.size() / kLineFloatsPerVertex);
        const int corners = closed ? (n + 1) : n; // 闭合环末尾重复角 0 以封口
        for (int c = 0; c < corners; ++c) {
            const int i = closed ? (c % n) : c;
            const float X = pts[i * 2], Y = pts[i * 2 + 1];
            float npx = 0.0f, npy = 0.0f, nnx = 0.0f, nny = 0.0f;
            const bool hasPrev = closed || (i > 0);
            const bool hasNext = closed || (i < n - 1);
            const bool okPrev = hasPrev && segNormal(closed ? (i - 1 + n) % n : (i - 1), npx, npy);
            const bool okNext = hasNext && segNormal(i, nnx, nny);
            float mx, my;
            if (okPrev && okNext) {
                float bx = npx + nnx, by = npy + nny; // 角平分线方向
                const float bl = std::sqrt(bx * bx + by * by);
                if (bl < 1e-6f) {
                    mx = nnx;
                    my = nny; // 近 180° 折返退化
                } else {
                    bx /= bl;
                    by /= bl;
                    const float d = bx * nnx + by * nny; // cos(半夹角)
                    float ml = (d > 1e-6f) ? (1.0f / d) : kMiterLimit;
                    if (ml > kMiterLimit) ml = kMiterLimit; // 尖角钳制
                    if (ml < 1.0f) ml = 1.0f;
                    mx = bx * ml;
                    my = by * ml;
                }
            } else if (okNext) {
                mx = nnx;
                my = nny; // 起点：用出段法向
            } else if (okPrev) {
                mx = npx;
                my = npy; // 终点：用入段法向
            } else {
                mx = 0.0f;
                my = 1.0f; // 全退化（重合点）
            }
            pushLineVert(verts, X, Y, mx, my, cr, cg, cb, ca);
            pushLineVert(verts, X, Y, -mx, -my, cr, cg, cb, ca);
            if (altOut != nullptr) {
                const float aCorner = alts.empty() ? 0.0f : alts[i];
                altOut->push_back(aCorner);
                modeOut->push_back(mode);
                altOut->push_back(aCorner);
                modeOut->push_back(mode);
            }
        }
        const int cnt = static_cast<int>(verts.size() / kLineFloatsPerVertex) - first;
        if (cnt >= 4) ranges.emplace_back(first, cnt); // 至少 2 角 = 4 顶点
    }

    // 把一条经纬度摊平环转相对原点坐标追加到拾取图元，并记录 (起始点对, 点对数) 区间（供命中检测）。
    // hasElev 时同步收集逐点对高程（rAlt 非空按顶点取，否则 0）/模式 mode，与 coords 点对对齐。
    void addPickRing(PickPrimitive &pp, const std::vector<double> &ring,
                     const std::vector<double> *rAlt, float mode) const {
        if (ring.size() < 4) return; // 至少 2 个点对
        const int start = static_cast<int>(pp.coords.size() / 2);
        int pcnt = 0;
        size_t vk = 0;
        for (size_t i = 0; i + 1 < ring.size(); i += 2) {
            double rx = 0.0, ry = 0.0;
            relD(ring[i], ring[i + 1], rx, ry);
            pp.coords.push_back(static_cast<float>(rx));
            pp.coords.push_back(static_cast<float>(ry));
            if (hasElev) {
                pp.alt.push_back((rAlt != nullptr && vk < rAlt->size())
                                     ? static_cast<float>((*rAlt)[vk]) : 0.0f);
                pp.mode.push_back(mode);
            }
            pcnt++;
            vk++;
        }
        if (pcnt >= 2) pp.ranges.emplace_back(start, pcnt);
    }

    // 构建要素标注锚点（仅 f.label 非空时）：点要素取点位置，线/面取顶点均值质心
    // （对齐原主界面 centroidOfFlat：经纬度均值后再投影），转为相对原点世界坐标。
    void addLabel(const VectorFeatureData &f, GeoShard &out) const {
        if (f.label.empty()) return;
        double aLon = 0.0, aLat = 0.0;
        if (f.type == VectorGeomType::Point) {
            aLon = f.lon;
            aLat = f.lat;
        } else {
            const std::vector<double> *ring = nullptr;
            if (f.type == VectorGeomType::Polygon) ring = &f.outer;
            else if (!f.parts.empty()) ring = &f.parts.front();
            if (ring == nullptr || ring->size() < 2) return;
            double sumLon = 0.0, sumLat = 0.0;
            int cnt = 0;
            for (size_t i = 0; i + 1 < ring->size(); i += 2) {
                sumLon += (*ring)[i];
                sumLat += (*ring)[i + 1];
                cnt++;
            }
            if (cnt == 0) return;
            aLon = sumLon / cnt;
            aLat = sumLat / cnt;
        }
        LabelItem li;
        relF(aLon, aLat, li.x, li.y);
        li.text = f.label;
        out.labels.push_back(std::move(li));
    }

    // 拉伸侧墙（Phase 2 extrude）：extrude 高程要素沿环逐段生成「地面(alt=0)→真实高程(环顶点 alt)」
    // 竖直四边形，写入独立 extrude 顶点数组，由 Renderer 的 extrude 深度写入 pass 绘制（非 fill 半透明通路）。
    //   墙顶墙底均 mode=1（baked 高程、不叠加整层 uVecAlt）：墙底落椭球地面、墙顶落真实高程，与实心屋顶
    //   拼成完整盒体；开深度写入使近墙遮挡远墙、建筑之间亦正确遮挡，消除「凹型外窥」。ca 传不透明(1.0)使盒体实心。
    // ring 为经纬度摊平，rAlt 逐顶点高程（米）。仅在 hasElev（extrudeAlt/extrudeMode 活跃）时被调用（featElev⇒层 hasElev），逐墙顶点三数组同序 push 保对齐。
    void appendExtrudeWalls(const std::vector<double> &ring, const std::vector<double> *rAlt,
                            float cr, float cg, float cb, float ca, GeoShard &out) const {
        if (!hasElev) return;
        const size_t pc = ring.size() / 2;
        if (pc < 2) return;
        std::vector<float> px(pc), py(pc), pa(pc, 0.0f);
        for (size_t i = 0; i < pc; ++i) {
            relF(ring[2 * i], ring[2 * i + 1], px[i], py[i]);
            if (rAlt != nullptr && i < rAlt->size()) pa[i] = static_cast<float>((*rAlt)[i]);
        }
        // 一段墙（点 a→点 b）：底(alt0)/顶(alt) 各两顶点 → 两个三角形构成竖直四边形。
        auto wallQuad = [&](size_t a, size_t b) {
            const auto pushV = [&](size_t idx, float alt) {
                pushVert(out.extrudeVerts, px[idx], py[idx], cr, cg, cb, ca);
                out.extrudeAlt.push_back(alt);
                out.extrudeMode.push_back(1.0f);
            };
            pushV(a, 0.0f);
            pushV(a, pa[a]);
            pushV(b, 0.0f);
            pushV(a, pa[a]);
            pushV(b, pa[b]);
            pushV(b, 0.0f);
        };
        for (size_t i = 0; i + 1 < pc; ++i) {
            if (pa[i] == 0.0f && pa[i + 1] == 0.0f) continue; // 两端均落地 → 无高度差，跳过
            wallQuad(i, i + 1);
        }
        // 未闭合环补首尾封口段（KML LinearRing 一般已闭合，此处兜底其它来源）
        const bool closed = (std::fabs(px[0] - px[pc - 1]) < 1e-6f && std::fabs(py[0] - py[pc - 1]) < 1e-6f);
        if (!closed && (pa[pc - 1] != 0.0f || pa[0] != 0.0f)) wallQuad(pc - 1, 0);
    }

    /// 烘焙单个要素进 out（体即并行化前 for-loop 的循环体，逐字搬移，不改任何数学）。
    void buildOne(const VectorFeatureData &f, GeoShard &out) const {
        addLabel(f, out);
        // 逐要素高程：mode 0/1（仅本层 hasElev 时才写入并行数组）；线/面逐顶点高程取自源 alt 数组。
        const bool featElev = (f.altMode != VecAltMode::Clamp);
        const float fmode = featElev ? 1.0f : 0.0f;
        // 逐要素 KML 配色（Phase 3）：以整层样式为底，命中 KML 通道则覆盖对应色（RGBA）。
        //   hasFill→面填充色；hasLine→线要素色，并同时作面描边色（KML 多边形边框取 LineStyle 色）。
        //   线宽不支持逐要素（渲染期按整层 uniform），宽度沿用 s。extrude 顶盖/侧墙取有效填充 RGB（alpha 仍强制实心）。
        VectorStyle es = s;
        if (f.style.hasFill) {
            es.fillR = f.style.fill[0]; es.fillG = f.style.fill[1];
            es.fillB = f.style.fill[2]; es.fillA = f.style.fill[3];
        }
        if (f.style.hasLine) {
            es.lineR = f.style.line[0]; es.lineG = f.style.line[1];
            es.lineB = f.style.line[2]; es.lineA = f.style.line[3];
            es.outlineR = es.lineR; es.outlineG = es.lineG;
            es.outlineB = es.lineB; es.outlineA = es.lineA;
        }
        if (f.type == VectorGeomType::Point) {
            float rx = 0.0f, ry = 0.0f;
            relF(f.lon, f.lat, rx, ry);
            out.pointCoords.push_back(rx);
            out.pointCoords.push_back(ry);
            if (hasElev) {
                out.pointAlt.push_back(featElev ? static_cast<float>(f.alt) : 0.0f);
                out.pointMode.push_back(fmode);
            }
            PickPrimitive pp;
            pp.fid = f.fid;
            pp.type = PickType::Point;
            pp.coords.push_back(rx);
            pp.coords.push_back(ry);
            if (hasElev) {
                pp.alt.push_back(featElev ? static_cast<float>(f.alt) : 0.0f);
                pp.mode.push_back(fmode);
            }
            out.pickPrims.push_back(std::move(pp));
        } else if (f.type == VectorGeomType::Line) {
            const auto lb0 = BgClock::now();
            PickPrimitive pp;
            pp.fid = f.fid;
            pp.type = PickType::Line;
            size_t lpair = 0;
            for (const auto &part : f.parts) lpair += part.size() / 2;
            pp.coords.reserve(lpair * 2);
            for (size_t k = 0; k < f.parts.size(); ++k) {
                const std::vector<double> *rAlt =
                        (hasElev && k < f.partsAlt.size()) ? &f.partsAlt[k] : nullptr;
                appendLineStrip(f.parts[k], out.lineVerts, out.lineRanges,
                                es.lineR, es.lineG, es.lineB, es.lineA,
                                rAlt, fmode, hasElev ? &out.lineAlt : nullptr,
                                hasElev ? &out.lineMode : nullptr);
                addPickRing(pp, f.parts[k], rAlt, fmode);
                // 立体拉伸：extrude 高程线 → 沿线生成地面→高程竖直 ribbon（写入 extrude 深度 pass）
                if (featElev && f.extrude)
                    appendExtrudeWalls(f.parts[k], rAlt,
                                       es.lineR * 0.72f, es.lineG * 0.72f, es.lineB * 0.72f, es.lineA, out);
            }
            if (!pp.ranges.empty()) out.pickPrims.push_back(std::move(pp));
            out.msOutline += std::chrono::duration<double, std::milli>(BgClock::now() - lb0).count();
        } else { // Polygon
            if (f.outer.size() < 6) return; // 至少 3 个坐标对

            // earcut 输入：环列表（外环在前、洞在后），点为相对原点 double 坐标；
            // 同时按相同顺序摊平到 flat，earcut 返回的索引即指向 flat。hasElev 时另建 flatAlt 与 flat 对齐。
            std::vector<std::vector<std::pair<double, double>>> rings;
            std::vector<std::pair<double, double>> flat;
            std::vector<float> flatAlt; // 仅 hasElev 时填充，与 flat 逐顶点对齐
            auto addRing = [&](const std::vector<double> &ring, const std::vector<double> *rAlt) {
                if (ring.size() < 6) return;
                std::vector<std::pair<double, double>> rp;
                rp.reserve(ring.size() / 2);
                size_t vk = 0;
                for (size_t i = 0; i + 1 < ring.size(); i += 2) {
                    double rx = 0.0, ry = 0.0;
                    relD(ring[i], ring[i + 1], rx, ry);
                    rp.emplace_back(rx, ry);
                    flat.emplace_back(rx, ry);
                    if (hasElev)
                        flatAlt.push_back((rAlt != nullptr && vk < rAlt->size())
                                              ? static_cast<float>((*rAlt)[vk]) : 0.0f);
                    vk++;
                }
                rings.push_back(std::move(rp));
            };
            addRing(f.outer, hasElev ? &f.outerAlt : nullptr);
            for (size_t hi = 0; hi < f.holes.size(); ++hi) {
                const std::vector<double> *hAlt =
                        (hasElev && hi < f.holesAlt.size()) ? &f.holesAlt[hi] : nullptr;
                addRing(f.holes[hi], hAlt);
            }
            if (rings.empty()) return;

            const auto e0 = BgClock::now();
            const std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(rings);
            out.msEar += std::chrono::duration<double, std::milli>(BgClock::now() - e0).count();
            const auto fb0 = BgClock::now();
            // 拉伸高程面屋顶：除常规 fill 外，额外以不透明·原色复制一份顶盖进 extrude 深度 pass（3D 实心、建筑间正确遮挡）。
            //   fill 侧保留→ 2D/平面模式仍显 footprint 填充；3D 下 opaque 顶盖后绘覆盖半透明 fill，无害。
            const bool roofToExtrude = (featElev && f.extrude);
            for (uint32_t ii : idx) {
                if (ii >= flat.size()) continue;
                pushVert(out.fillVerts, static_cast<float>(flat[ii].first), static_cast<float>(flat[ii].second),
                         es.fillR, es.fillG, es.fillB, es.fillA);
                if (hasElev) {
                    out.fillAlt.push_back(ii < flatAlt.size() ? flatAlt[ii] : 0.0f);
                    out.fillMode.push_back(fmode);
                }
                if (roofToExtrude) {
                    pushVert(out.extrudeVerts, static_cast<float>(flat[ii].first), static_cast<float>(flat[ii].second),
                             es.fillR, es.fillG, es.fillB, 1.0f);
                    out.extrudeAlt.push_back(ii < flatAlt.size() ? flatAlt[ii] : 0.0f);
                    out.extrudeMode.push_back(1.0f);
                }
            }
            out.msFill += std::chrono::duration<double, std::milli>(BgClock::now() - fb0).count();

            // 立体拉伸侧墙：extrude 高程面 → 沿外环与各洞环生成地面→高程墙（不透明·暗色侧墙 ×0.72），
            //   与上面的实心屋顶拼成完整盒体；墙顶/墙底均 mode=1 baked 真实高程。侧墙与屋顶同在 extrude 深度 pass。
            if (featElev && f.extrude) {
                const float wr = es.fillR * 0.72f, wg = es.fillG * 0.72f, wb = es.fillB * 0.72f;
                appendExtrudeWalls(f.outer, hasElev ? &f.outerAlt : nullptr, wr, wg, wb, 1.0f, out);
                for (size_t hi = 0; hi < f.holes.size(); ++hi) {
                    const std::vector<double> *hAlt =
                            (hasElev && hi < f.holesAlt.size()) ? &f.holesAlt[hi] : nullptr;
                    appendExtrudeWalls(f.holes[hi], hAlt, wr, wg, wb, 1.0f, out);
                }
            }

            // 面描边：外环 + 各洞环（白色/样式色，单独一组以用独立线宽）
            const auto ob0 = BgClock::now();
            appendLineStrip(f.outer, out.outlineVerts, out.outlineRanges,
                            es.outlineR, es.outlineG, es.outlineB, es.outlineA,
                            hasElev ? &f.outerAlt : nullptr, fmode,
                            hasElev ? &out.outlineAlt : nullptr, hasElev ? &out.outlineMode : nullptr);
            for (size_t hi = 0; hi < f.holes.size(); ++hi) {
                const std::vector<double> *hAlt =
                        (hasElev && hi < f.holesAlt.size()) ? &f.holesAlt[hi] : nullptr;
                appendLineStrip(f.holes[hi], out.outlineVerts, out.outlineRanges,
                                es.outlineR, es.outlineG, es.outlineB, es.outlineA,
                                hAlt, fmode,
                                hasElev ? &out.outlineAlt : nullptr, hasElev ? &out.outlineMode : nullptr);
            }
            out.msOutline += std::chrono::duration<double, std::milli>(BgClock::now() - ob0).count();

            // 拾取图元：外环为 ranges[0]、各洞环依次在后（命中判定「在外环内且不在任一洞内」）
            const auto pb0 = BgClock::now();
            PickPrimitive pp;
            pp.fid = f.fid;
            pp.type = PickType::Polygon;
            pp.coords.reserve(f.outer.size());
            addPickRing(pp, f.outer, hasElev ? &f.outerAlt : nullptr, fmode);
            for (size_t hi = 0; hi < f.holes.size(); ++hi) {
                const std::vector<double> *hAlt =
                        (hasElev && hi < f.holesAlt.size()) ? &f.holesAlt[hi] : nullptr;
                addPickRing(pp, f.holes[hi], hAlt, fmode);
            }
            if (!pp.ranges.empty()) out.pickPrims.push_back(std::move(pp));
            out.msPick += std::chrono::duration<double, std::milli>(BgClock::now() - pb0).count();
        }
    }
};

/// 烘焙一个要素并记录其在三组顶点中的区间（buildOne 前后量差）。退化要素（buildOne 内提前 return）
/// 各量为零 → 记为零长空区间，spans 与要素数始终一一对应；区间均为分片内局部值，合并时重映射。
void bakeWithSpan(const FeatureBaker &baker, const VectorFeatureData &f, GeoShard &sh) {
    const size_t f0 = sh.fillVerts.size();
    const size_t o0 = sh.outlineRanges.size();
    const size_t l0 = sh.lineRanges.size();
    baker.buildOne(f, sh);
    FeatureSpans s;
    s.fillStart = static_cast<int>(f0);
    s.fillCount = static_cast<int>(sh.fillVerts.size() - f0);
    s.outRangeStart = static_cast<int>(o0);
    s.outRangeCount = static_cast<int>(sh.outlineRanges.size() - o0);
    s.lineRangeStart = static_cast<int>(l0);
    s.lineRangeCount = static_cast<int>(sh.lineRanges.size() - l0);
    sh.spans.push_back(s);
}

/// 按块序把各分片拼接为最终几何：顶点/点/拾取/标注顺序拼接，描边与线的 range.first 顶点索引
/// 累加各块已产生的顶点数做重映射（块内 range.first 相对本块缓冲起点）。合并后与串行 build 完全一致。
void mergeShards(VectorGeometry &g, std::vector<GeoShard> &shards) {
    size_t fillF = 0, outV = 0, lineV = 0, pt = 0, pickN = 0, labN = 0, outR = 0, lineR = 0;
    for (const auto &sh : shards) {
        fillF += sh.fillVerts.size();
        outV += sh.outlineVerts.size();
        lineV += sh.lineVerts.size();
        pt += sh.pointCoords.size();
        pickN += sh.pickPrims.size();
        labN += sh.labels.size();
        outR += sh.outlineRanges.size();
        lineR += sh.lineRanges.size();
    }
    g.fillVerts.reserve(fillF);
    g.outlineVerts.reserve(outV);
    g.lineVerts.reserve(lineV);
    g.pointCoords.reserve(pt);
    g.pickPrims.reserve(pickN);
    g.labels.reserve(labN);
    g.outlineRanges.reserve(outR);
    g.lineRanges.reserve(lineR);
    size_t spanN = 0;
    for (const auto &sh : shards) spanN += sh.spans.size();
    g.featSpans.reserve(spanN);

    int outOff = 0, linOff = 0; // 各输出顶点组累计顶点数（非 float 数）
    int fillFOff = 0;          // fillVerts 累计 floats（spans 重映射用）
    int outROff = 0, linROff = 0; // outlineRanges / lineRanges 累计段数
    for (auto &sh : shards) {
        g.fillVerts.insert(g.fillVerts.end(), sh.fillVerts.begin(), sh.fillVerts.end());
        g.fillAlt.insert(g.fillAlt.end(), sh.fillAlt.begin(), sh.fillAlt.end());
        g.fillMode.insert(g.fillMode.end(), sh.fillMode.begin(), sh.fillMode.end());

        g.outlineVerts.insert(g.outlineVerts.end(), sh.outlineVerts.begin(), sh.outlineVerts.end());
        g.outlineAlt.insert(g.outlineAlt.end(), sh.outlineAlt.begin(), sh.outlineAlt.end());
        g.outlineMode.insert(g.outlineMode.end(), sh.outlineMode.begin(), sh.outlineMode.end());
        for (const auto &r : sh.outlineRanges) g.outlineRanges.emplace_back(r.first + outOff, r.second);
        outOff += static_cast<int>(sh.outlineVerts.size() / kLineFloatsPerVertex);

        g.lineVerts.insert(g.lineVerts.end(), sh.lineVerts.begin(), sh.lineVerts.end());
        g.lineAlt.insert(g.lineAlt.end(), sh.lineAlt.begin(), sh.lineAlt.end());
        g.lineMode.insert(g.lineMode.end(), sh.lineMode.begin(), sh.lineMode.end());
        for (const auto &r : sh.lineRanges) g.lineRanges.emplace_back(r.first + linOff, r.second);
        linOff += static_cast<int>(sh.lineVerts.size() / kLineFloatsPerVertex);

        g.pointCoords.insert(g.pointCoords.end(), sh.pointCoords.begin(), sh.pointCoords.end());
        g.pointAlt.insert(g.pointAlt.end(), sh.pointAlt.begin(), sh.pointAlt.end());
        g.pointMode.insert(g.pointMode.end(), sh.pointMode.begin(), sh.pointMode.end());
        g.extrudeVerts.insert(g.extrudeVerts.end(), sh.extrudeVerts.begin(), sh.extrudeVerts.end());
        g.extrudeAlt.insert(g.extrudeAlt.end(), sh.extrudeAlt.begin(), sh.extrudeAlt.end());
        g.extrudeMode.insert(g.extrudeMode.end(), sh.extrudeMode.begin(), sh.extrudeMode.end());
        for (auto &pp : sh.pickPrims) g.pickPrims.push_back(std::move(pp));
        for (auto &li : sh.labels) g.labels.push_back(std::move(li));

        // 逐要素区间：起点加各累计偏移（fill 为 floats、range 为段数），长度不变；块间保持要素序
        for (const auto &s : sh.spans) {
            FeatureSpans m = s;
            m.fillStart += fillFOff;
            m.outRangeStart += outROff;
            m.lineRangeStart += linROff;
            g.featSpans.push_back(m);
        }
        fillFOff += static_cast<int>(sh.fillVerts.size());
        outROff += static_cast<int>(sh.outlineRanges.size());
        linROff += static_cast<int>(sh.lineRanges.size());
    }
}

// ==================== 进程级并发 build 线程令牌预算 ====================
// 多个矢量层可同时因相机静止而各自重载，每层 build 若各自起满核数线程会线程爆炸。用一组全局原子
// 令牌约束「跨所有层的额外 worker 线程总数」≤ (核数−1)，各调用线程自身总保留 1 核。取不到令牌即回退
// 串行（本线程 build），永不等待、永不死锁；用完归还。核数只算一次。
std::atomic<int> g_maxThreads{0};      // 0=未初始化
std::atomic<int> g_activeWorkers{0};   // 当前已占用的额外 worker 线程数（不含各调用线程自身）

int maxBuildThreads() {
    int m = g_maxThreads.load(std::memory_order_relaxed);
    if (m == 0) {
        m = static_cast<int>(std::thread::hardware_concurrency());
        if (m < 1) m = 1;
        g_maxThreads.store(m, std::memory_order_relaxed);
    }
    return m;
}

/// 尝试占用最多 want 个额外 worker 线程，返回实际占用数（0 表示无令牌可用→调用方串行）。非阻塞。
int reserveWorkers(int want) {
    if (want <= 0) return 0;
    const int maxW = maxBuildThreads() - 1; // 留 1 核给各调用线程自身
    if (maxW <= 0) return 0;
    int cur = g_activeWorkers.load(std::memory_order_relaxed);
    for (;;) {
        const int avail = maxW - cur;
        if (avail <= 0) return 0;
        const int take = (want < avail) ? want : avail;
        if (g_activeWorkers.compare_exchange_weak(cur, cur + take,
                                                  std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return take;
        }
        // CAS 失败：cur 已被刷新，继续重试
    }
}

void releaseWorkers(int n) {
    if (n > 0) g_activeWorkers.fetch_sub(n, std::memory_order_acq_rel);
}

} // namespace

VectorGeometry buildGeometry(const VectorReadResult &r, const VectorStyle &s,
                             const std::atomic<bool> *cancel) {
    VectorGeometry g;
    const size_t N = r.features.size();

    // 图层原点 = bbox 中心（无 bbox 时退化到首要素点，仍给出稳定原点）。先于任何要素、且仅由读结果
    // 决定 → 并行各分片共享同一原点，与串行完全一致。
    double cLon = 0.0, cLat = 0.0;
    if (r.hasBBox) {
        cLon = (r.minLon + r.maxLon) * 0.5;
        cLat = (r.minLat + r.maxLat) * 0.5;
    } else if (N > 0) {
        const auto &f0 = r.features.front();
        if (f0.type == VectorGeomType::Point) {
            cLon = f0.lon;
            cLat = f0.lat;
        } else if (!f0.outer.empty()) {
            cLon = f0.outer[0];
            cLat = f0.outer[1];
        } else if (!f0.parts.empty() && f0.parts.front().size() >= 2) {
            cLon = f0.parts.front()[0];
            cLat = f0.parts.front()[1];
        }
    }
    MercatorProjection::lonLatToWorld(cLon, cLat, g.originWx, g.originWy);

    const FeatureBaker baker(g.originWx, g.originWy, s, r.hasElevation);

    // 估算源顶点量（坐标对）决定是否并行，并据其推算期望线程数。
    size_t estVerts = 0;
    for (const auto &f : r.features) {
        if (f.type == VectorGeomType::Point) estVerts += 1;
        else if (f.type == VectorGeomType::Line) {
            for (const auto &p : f.parts) estVerts += p.size() / 2;
        } else {
            estVerts += f.outer.size() / 2;
            for (const auto &h : f.holes) estVerts += h.size() / 2;
        }
    }

    const bool wantParallel = (N >= kParallelMinFeatures && estVerts >= kParallelMinVerts);
    int targetThreads = 1;
    if (wantParallel) {
        int byLoad = static_cast<int>(estVerts / kVertsPerThread);
        if (byLoad < 2) byLoad = 2;
        targetThreads = std::min(byLoad, std::min(maxBuildThreads(), static_cast<int>(N)));
    }
    const int extra = reserveWorkers(targetThreads - 1);

    // 未达并行门槛 或 抢不到任何令牌 → 当前线程串行 build（1 个分片）。
    if (extra <= 0) {
        std::vector<GeoShard> shards(1);
        for (size_t i = 0; i < N; ++i) {
            if (cancel != nullptr && cancel->load()) break; // [VecReload] 抢占：中断过期 build
            bakeWithSpan(baker, r.features[i], shards[0]);
        }
        mergeShards(g, shards);
        const auto &sh = shards[0];
        LOGI("[VecReload] buildGeometry(serial): fill=%zu outlineSeg=%zu lineSeg=%zu pts=%zu labels=%zu | ear=%.1f fill=%.1f outline=%.1f pick=%.1f ms",
             g.fillVerts.size() / kVecFloatsPerVertex, g.outlineRanges.size(),
             g.lineRanges.size(), g.pointCoords.size() / 2, g.labels.size(),
             sh.msEar, sh.msFill, sh.msOutline, sh.msPick);
        return g;
    }

    // 并行：按连续要素区间切块，块数远多于线程数以负载均衡；线程用原子游标动态取块，各写各分片。
    const int T = extra + 1; // 实际参与线程数（含当前调用线程）
    const size_t chunkSize = std::max<size_t>(1, N / static_cast<size_t>(T * 8));
    const size_t numChunks = (N + chunkSize - 1) / chunkSize;
    std::vector<GeoShard> shards(numChunks);
    std::atomic<size_t> nextChunk{0};
    std::atomic<bool> stopAll{false};

    auto worker = [&]() {
        for (;;) {
            if (stopAll.load(std::memory_order_relaxed)) return;
            if (cancel != nullptr && cancel->load()) { stopAll.store(true); return; }
            const size_t ci = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (ci >= numChunks) return;
            const size_t b = ci * chunkSize;
            const size_t e = std::min(N, b + chunkSize);
            GeoShard &sh = shards[ci]; // 本块独占分片，线程间无共享
            for (size_t i = b; i < e; ++i) {
                if (stopAll.load(std::memory_order_relaxed)) return;
                if (cancel != nullptr && cancel->load()) { stopAll.store(true); return; }
                bakeWithSpan(baker, r.features[i], sh);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(extra);
    for (int k = 0; k < extra; ++k) threads.emplace_back(worker);
    worker(); // 当前线程也参与一块，再收尾
    for (auto &t : threads) t.join();
    releaseWorkers(extra);

    mergeShards(g, shards); // 按块下标升序合并 → 与串行要素顺序一致

    double msEar = 0, msFill = 0, msOutline = 0, msPick = 0;
    for (const auto &sh : shards) {
        msEar += sh.msEar;
        msFill += sh.msFill;
        msOutline += sh.msOutline;
        msPick += sh.msPick;
    }
    LOGI("[VecReload] buildGeometry(parallel th=%d chunks=%zu): fill=%zu outlineSeg=%zu lineSeg=%zu pts=%zu labels=%zu | ear=%.1f fill=%.1f outline=%.1f pick=%.1f ms(跨线程累计CPU)",
         T, numChunks, g.fillVerts.size() / kVecFloatsPerVertex, g.outlineRanges.size(),
         g.lineRanges.size(), g.pointCoords.size() / 2, g.labels.size(),
         msEar, msFill, msOutline, msPick);
    return g;
}

} // namespace globecore
