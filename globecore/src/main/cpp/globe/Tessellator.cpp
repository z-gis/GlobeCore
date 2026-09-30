#include "globe/Tessellator.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace globecore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kDeg2Rad = kPi / 180.0;
/// 墨卡托有效纬度上限（与 MercatorProjection::maxLatitude 同口径，约 85.0511°）
constexpr double kMaxLat = 85.05112877980659;

/// Web 墨卡托瓦片 (z,x,y) → 经纬度包围盒（度）。lat 用墨卡托反投影（atan·sinh）。
inline void tileToBounds(int z, int x, int y,
                         double &lonMin, double &lonMax, double &latMin, double &latMax) {
    const double n = std::pow(2.0, static_cast<double>(z));
    lonMin = x / n * 360.0 - 180.0;
    lonMax = (x + 1.0) / n * 360.0 - 180.0;
    auto rowToLat = [n](double ty) {
        const double s = kPi * (1.0 - 2.0 * ty / n);
        double lat = std::atan(std::sinh(s)) * kRad2Deg;
        if (lat > kMaxLat) lat = kMaxLat;
        if (lat < -kMaxLat) lat = -kMaxLat; // 修前误写为 `lat = kMaxLat`，导致 n=1 时 rowToLat(1) 从 −85.05
                                            // 回正 +85.05，根瓦片 latMin=latMax，AABB 塔缩到 85°N 一圈，
                                            // 被 top 平面与地平线判据双重误剔（实测 leaves=0 根因）。
        return lat;
    };
    latMax = rowToLat(static_cast<double>(y));       // 上行 → 北
    latMin = rowToLat(static_cast<double>(y) + 1.0); // 下行 → 南
}

/// 每帧构建一次的常量上下文；子递归只读，避免重复提取视锥/反算眼点。
struct SubdivCtx {
    // 相机与投影
    Vec3 eye;                                  // ECEF 米
    Vec3 eyeDir;                               // 单位向径（|E| 方向）
    std::array<FrustumPlane, 6> planes;        // Gribb 6 面（单位法向，正=内侧）
    double eyeLonDeg = 0.0, eyeLatDeg = 0.0;   // 眼点地理经纬（度），供最近点 clamp
    double eyeLen = 0.0;                       // |E|；地平线判据按瓦片局部半径 |P|²/|E| 逐瓦片计算
    // 注：不能用 R²/|E|（R=赤道半径）作固定阈值——WGS84 扁率 1/298，中纬局部地表半径
    // 比赤道半径小 ~8000m，855m 眼高下根节点 nearest.dot(ê)≈6370300 m 会小于 R²/|E|≈6385281 m
    // 而错误剔除根（实测 leaves=0）。改用 |nearest|²/|E|：北京瓦通过、反面瓦仍被剔。
    // LOD 参数
    int targetLevel = 0;
    int maxLevel = 0;
    double tanHalfFov = 0.0;
    double viewportHeight = 1.0;
    double detailFactor = 1.0;
    // 地球
    double R = 0.0;
    const Globe *globe = nullptr;
};

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// 经度差回绕到 [-180,180]：跨日期变更线时 178° 与 -178° 实际相差 4° 而非 356°。
inline double wrapLonDelta(double deg) {
    while (deg > 180.0) deg -= 360.0;
    while (deg < -180.0) deg += 360.0;
    return deg;
}

/**
 * 瓦片矩形中「地理上最近眼点」的那点（经/纬度各自 clamp 到瓦片边界）→ ECEF + 距眼平方。
 * 对应 参考实现 AbstractTile.nearestPoint（我们只用海平面，忽略地形高程）。
 * 经度 clamp 须考虑 360° 回绕：眼点 lon=178° 对瓦片 lon∈[-180,0] 若按数值 clamp 会取
 * 0°（夹角 178°、在地球背面），地平线判据随之把跨日期变更线的整棵子树误剔（3D 只剩半球、
 * 切口为过 180° 经线的直线）。越界时改取回绕角距离较小的那条经度边界。
 */
inline void nearestPointOfTile(const SubdivCtx &c,
                               double lonMin, double lonMax, double latMin, double latMax,
                               Vec3 &outNearest, double &outDist2) {
    const double nLat = clampd(c.eyeLatDeg, latMin, latMax);
    double nLon = c.eyeLonDeg;
    if (nLon < lonMin || nLon > lonMax) {
        const double dMin = std::fabs(wrapLonDelta(c.eyeLonDeg - lonMin));
        const double dMax = std::fabs(wrapLonDelta(c.eyeLonDeg - lonMax));
        nLon = (dMin <= dMax) ? lonMin : lonMax;
    }
    c.globe->geographicToCartesian(nLon, nLat, 0.0, outNearest);
    const Vec3 d = outNearest - c.eye;
    outDist2 = d.dot(d);
}

/**
 * 递归细分。三道闸按 参考实现 BasicTessellator.addTileOrDescendants 口径：
 *   ① 地平线剔除（瓦片 3×3 网格采样点「全部」在地平线下才剪枝；跨极冠瓦片补两极采样点。
 *      旧版用「地理 clamp 单最近点」整块判可见即剔整块，对跨地平线/极冠瓦片产生整块误剔 →
 *      全地球远端极冠黑洞（仅当极冠处于视口上缘时发生，拉近推远皆消失，南北极对称））
 *   ② 视锥 AABB 剔除（3×3 网格 ± 半球修正点，正侧极值点带符号距离 < 0 → 剪枝）
 *   ③ 屏幕空间误差 mustSubdivide（用 nearest 距，非中心距）
 *   ④ 出叶 or 4 子瓦片按 nearestDist² 升序递归（近侧先入，配额优先给最需要的瓦片）
 * 覆盖优先（对齐 参考实现）：预算耗尽时不丢弃可见块，而是保留为粗级叶——见下方 outOfBudget。
 */
void subdivide(int z, int x, int y, const SubdivCtx &c, std::vector<GlobeTile> &out) {
    double lonMin, lonMax, latMin, latMax;
    tileToBounds(z, x, y, lonMin, lonMax, latMin, latMax);

    // 瓦片地理最近点：供 SSE 判据、子块 DFS 排序，并作为 ① 地平线闸的一个「可见候选」。
    // 最近点（眼经纬 clamp 进瓦片边界）是瓦片上角距星下点最小的点：它在地平线上 ⟹ 瓦片确有可见部分。
    // 粗级瓦片（尤其根）的 3×3 网格点稀疏、可能全落在可见帽之外而被误剔（实测 alt≈1.4M leaves=0 黑屏），
    // 故必须把最近点并入地平线判据，与网格/极冠补点取「任一可见即保留」的并集。
    Vec3 nearest;
    double nearestDist2 = 0.0;
    nearestPointOfTile(c, lonMin, lonMax, latMin, latMax, nearest, nearestDist2);

    // ② 视锥测试用世界轴 AABB：3×3 地理网格 9 采样（对齐 参考实现 BoundingBox.setToSector 的 NUM_LAT×NUM_LON=3×3），
    //    跨经度 > 180° 时按 参考实现 补 (centroid lat, lon±90°) 两点。半球瓦片的 AABB 会比真实表面略"薄"，
    //    但因为测试是保守拒绝（AABB 完全在平面外侧才剔），略保守=少剔=多留=安全；下一级细分自然修正。
    //    同批采样点兼作 ① 地平线测试：任一采样点在地平线之上即保留（被剔部分的隐藏交由 GPU 深度测试，
    //    对齐 参考实现——参考实现 根本没有地平线剔除闸，只靠视锥 + 深度）。
    double xmin = 1e300, ymin = 1e300, zmin = 1e300;
    double xmax = -1e300, ymax = -1e300, zmax = -1e300;
    bool horizonVisible = false;
    {
        const double lonArr[3] = {lonMin, 0.5 * (lonMin + lonMax), lonMax};
        const double latArr[3] = {latMin, 0.5 * (latMin + latMax), latMax};
        Vec3 p;
        auto testHorizon = [&c, &horizonVisible](const Vec3 &q) {
            if (horizonVisible || c.eyeLen <= 0.0) return;
            // 以 q 处局部半径 |q| 为球近似：dot(q, ê)·|E| ≥ |q|² 即该点在地平线之上（可见）。
            if (q.dot(c.eyeDir) * c.eyeLen >= q.dot(q)) horizonVisible = true;
        };
        // 先以地理最近点起判：它必在瓦片实际表面上且角距星下点最小，是「瓦片是否有任何可见部分」最
        // 可靠的单点指标——保住根/粗级瓦片不被稀疏网格误剔（黑屏根因）。再叠加网格与极冠补点。
        testHorizon(nearest);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                c.globe->geographicToCartesian(lonArr[i], latArr[j], 0.0, p);
                if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
                if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
                testHorizon(p);
            }
        }
        if ((lonMax - lonMin) > 180.0) {
            const double latC = 0.5 * (latMin + latMax);
            const double lonC = 0.5 * (lonMin + lonMax);
            c.globe->geographicToCartesian(lonC + 90.0, latC, 0.0, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            testHorizon(p);
            c.globe->geographicToCartesian(lonC - 90.0, latC, 0.0, p);
            if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
            if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            if (p.z < zmin) zmin = p.z; if (p.z > zmax) zmax = p.z;
            testHorizon(p);
        }
        // 极冠补点：墨卡托把网格纬度截到 ±85.0511°，触极行瓦片（latMax/latMin 钳到界值）在 85.05°~90°
        // 之间的极冠区没有采样点。眼在 |lat|<~50° 的全地球视图下，9 个网格点可全部落在地平线下而
        // 极冠本身仍可见（实测北极冠瓦片整块误剔 = 远端黑洞且不与地球同圆心的根因），故补两极采样。
        if (latMax >= kMaxLat - 1e-9) {
            c.globe->geographicToCartesian(0.0, 90.0, 0.0, p);
            testHorizon(p);
        }
        if (latMin <= -kMaxLat + 1e-9) {
            c.globe->geographicToCartesian(0.0, -90.0, 0.0, p);
            testHorizon(p);
        }
    }

    // ① 地平线剔除：仅当「最近点 + 网格点 + 极冠补点」全部在地平线以下才剪枝（任一可见即保留；
    //    存在误剔即留洞/黑屏，留洞比多画严重）。置于视锥测试前，背半球整棵子树照常在此剪掉（性能不回退）。
    if (c.eyeLen > 0.0 && !horizonVisible) return;
    for (int pi = 0; pi < 6; ++pi) {
        const FrustumPlane &pl = c.planes[pi];
        // 平面外侧最远点：取 AABB 上让 ax+by+cz 最大的那一角（"positive vertex"）。
        const double px = (pl.a >= 0.0) ? xmax : xmin;
        const double py = (pl.b >= 0.0) ? ymax : ymin;
        const double pz = (pl.c >= 0.0) ? zmax : zmin;
        const double v = pl.a * px + pl.b * py + pl.c * pz + pl.d;
        if (v < 0.0) return;
    }

    // ③ 屏幕空间误差 mustSubdivide（参考实现 Tile.mustSubdivide 3D 分支去 fog 项）：
    //    参考实现: texelSize = texelSizeFactor·R = 单纹理米数 = 瓦片宽 ÷ 瓦片像素(256)；
    //         pixelSize = 2·dNearest·tanHalfFov / viewportHeight（针孔近似，与 参考实现 pixelSizeAtDistance 等价）。
    //    旧版误用「整瓦片宽」作 texelSize（未 ÷256），使 texelSize 恒 ≫ pixelSize·detailFactor、mustSubdiv
    //    永真 → 每片一律细分到 targetLevel 上限（实测全地球 z=5..5、443 叶、远景无粗级祖先可兜底而留黑洞）。
    //    改回逐纹素口径后恢复 参考实现的近细远粗梯度：全地球中心止于 z≈2~3（叶数骤减、共享粗级皆在），
    //    近景仍由 targetLevel 上限驱动到满级细节。
    const double latC = 0.5 * (latMin + latMax);
    double cosLat = std::cos(latC * kDeg2Rad);
    if (cosLat < 0.05) cosLat = 0.05;
    constexpr double kTileTexels = 256.0; // 标准 256px 瓦片：瓦片宽 ÷ 256 = 单纹素地面尺寸
    const double texelSize = 2.0 * kPi * c.R * cosLat / (std::pow(2.0, static_cast<double>(z)) * kTileTexels);
    const double nearestDist = std::sqrt(nearestDist2);
    const double pixelSize = 2.0 * nearestDist * c.tanHalfFov / c.viewportHeight;
    const bool mustSubdiv = texelSize > pixelSize * c.detailFactor;

    // 覆盖优先（参考实现 BasicTessellator 从不为省配额丢弃可见块）：配额耗尽时不再向下细分，
    // 而是把「当前」这块作为粗级叶保留。quadtree 叶集天然构成被覆盖球面的无缝划分（不重叠、不留洞），
    // 于是远景从「缺失」降级为「模糊拉伸」，近景在耗尽前已细分到位。地平线/视锥外的块仍在①②正常剔除（不可见，不算洞）。
    const bool outOfBudget = static_cast<int>(out.size()) >= Tessellator::kMaxTiles;
    const bool leaf = outOfBudget || (!mustSubdiv) || (z >= c.targetLevel) || (z >= c.maxLevel) || (z >= 30);
    if (leaf) {
        GlobeTile t;
        t.z = z;
        t.x = x;
        t.y = y;
        t.lonMin = lonMin;
        t.lonMax = lonMax;
        t.latMin = latMin;
        t.latMax = latMax;
        out.push_back(t);
        return;
    }

    // ④ 4 子瓦片按各自 nearestDist² 升序递归——保证 DFS 优先展开眼点正下方子树，
    //    kMaxTiles 先被最需要的瓦片占据，不被 x=0 子树先入吞掉（低空瓦片风暴的 DFS 顺序根因）。
    //    注意：不再在子循环中途 return 丢弃后续子块——那会在远景留洞；每个子块自顶重新判 outOfBudget，
    //    耗尽则各自作为粗级叶返回（覆盖优先）。
    struct Child {
        int cx, cy;
        double d2;
    };
    Child ch[4];
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            Child &cc = ch[dy * 2 + dx];
            cc.cx = 2 * x + dx;
            cc.cy = 2 * y + dy;
            double clonMin, clonMax, clatMin, clatMax;
            tileToBounds(z + 1, cc.cx, cc.cy, clonMin, clonMax, clatMin, clatMax);
            Vec3 np;
            double nd2 = 0.0;
            nearestPointOfTile(c, clonMin, clonMax, clatMin, clatMax, np, nd2);
            cc.d2 = nd2;
        }
    }
    // 4 元素选择排序，最小改动、无堆分配
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            if (ch[j].d2 < ch[i].d2) std::swap(ch[i], ch[j]);
        }
    }
    for (int i = 0; i < 4; ++i) {
        subdivide(z + 1, ch[i].cx, ch[i].cy, c, out); // 预算耗尽时子块在入口即作粗叶返回，无需在此中途中止
    }
}

} // namespace

void Tessellator::buildVisibleTiles(const TessCamera &cam, int targetLevel, int maxLevel,
                                    double tanHalfFov, int viewportHeightPx, double detailFactor,
                                    const Globe &globe, std::vector<GlobeTile> &out) {
    out.clear();
    if (viewportHeightPx <= 0) return;
    if (targetLevel < 0) targetLevel = 0;
    if (maxLevel < 0) maxLevel = 0;

    SubdivCtx c;
    c.eye = cam.eye;
    c.planes = cam.frustumPlanes; // 直接采用 Navigator 预提的 double 平面，不再走 float viewProj
    double eyeAlt = 0.0;
    globe.cartesianToGeographic(cam.eye, c.eyeLonDeg, c.eyeLatDeg, eyeAlt);
    const double eyeLen = cam.eye.length();
    c.R = globe.equatorialRadius();
    if (eyeLen > 1e-12) c.eyeDir = cam.eye * (1.0 / eyeLen);
    c.eyeLen = eyeLen;
    c.targetLevel = targetLevel;
    c.maxLevel = maxLevel;
    c.tanHalfFov = tanHalfFov;
    c.viewportHeight = static_cast<double>(viewportHeightPx);
    c.detailFactor = detailFactor;
    c.globe = &globe;

    // Web 墨卡托根瓦片：level 0 全球 1 张 (0,0,0)
    subdivide(0, 0, 0, c, out);
}

} // namespace globecore
