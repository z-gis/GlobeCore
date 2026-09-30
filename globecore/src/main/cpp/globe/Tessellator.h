#ifndef GLOBECORE_GLOBE_TESSELLATOR_H
#define GLOBECORE_GLOBE_TESSELLATOR_H

#include <array>
#include <vector>

#include "geom/Matrix4.h"
#include "globe/Globe.h"

namespace globecore {

/// Tessellator 的轻量相机输入（解耦 Navigator：取眼点 + 预提取的 6 视锥平面）。
/// frustumPlanes 由 Navigator 在 double 下直构（不经过 float viewProj），避免大 alt
/// 下 row3±row2 抵消后归一化放大 float32 噪声导致 leaves 非单调翻转。
struct TessCamera {
    Vec3 eye;                                    // 眼点（ECEF 米）
    Matrix4 viewProj;                            // 预留，仅交给 Renderer 开 GPU 时参考
    std::array<FrustumPlane, 6> frustumPlanes{}; // left/right/bottom/top/near/far，double
};

/// 3D 球面瓦片：quadtree 输出的一个叶节点，带级别/行列 + 该瓦片的经纬度包围盒（度）。
/// lonMin<lonMax；latMin=南纬、latMax=北纬。供 Renderer 生成贴球面网格与请求纹理。
struct GlobeTile {
    int z = 0;
    int x = 0;
    int y = 0;
    double lonMin = 0.0, lonMax = 0.0, latMin = 0.0, latMax = 0.0;
};

/**
 * Tessellator：3D 球体模式下的可见瓦片选取器。**照搬 参考实现 BasicTessellator.addTileOrDescendants 的四步**：
 *
 *  1) 地平线剔除 —— **保守剪枝：「地理最近点 + 3×3 网格采样点 + 触极行两极补点」全部在地平线下才剔**
 *     （任一可见即保留）。最近点保住根/粗级瓦片不被稀疏网格误剔（否则 alt 降低时整棵根树被剔→黑屏）；
 *     网格+极冠补点保住远景跨地平线/极冠瓦片（否则远端极冠黑洞）。参考实现 本无此闸，隐藏部分交由深度测试裁决。
 *  2) `Tile.intersectsFrustum` 等价物 —— **视锥测试用世界轴 AABB**（非包围球），AABB 由
 *     瓦片 **3×3 地理网格** 9 个 Cartesian 采样点求 min/max；若瓦片跨经度 > 180°（仅根/一级），
 *     按 参考实现 `BoundingBox.setToSector` 补 centroid (lat, lon±90°) 两点。半球瓦片 AABB 略保守但
 *     绝不误剔，交由下一级细分收敛。
 *  3) `Tile.mustSubdivide` 等价物 —— **屏幕空间误差用最近点距**：
 *     texelSize = 2πR·cos(centroid lat)/(256·2^z)（逐纹素米数 = 瓦片宽 ÷ 256，对齐 参考实现 texelSizeFactor·R）；
 *     pixelSize = 2·dNearest·tan(fov/2)/viewportH；
 *     texelSize > pixelSize·detailFactor 才继续细分（同 参考实现 `Tile.mustSubdivide` 3D 分支，去掉 fog 项）。
 *     与 z>=targetLevel / z>=maxLevel / z>=30 任一成立则出叶。
 *  4) **DFS 子瓦片按 nearestDistance 升序递归**（对齐 参考实现 `currentTiles.sortBy{sortOrder}` 的"近侧先装"效果，
 *     但在递归时机就做，避免 kMaxTiles 被 x=0 子树先入吞掉——这是低空瓦片风暴的 DFS 顺序根因）。
 *
 * 纯 CPU、无 GL、无状态（结果写入调用方 out），供 Renderer 每帧调用。out 数量封顶 kMaxTiles 防高级别爆炸。
 */
class Tessellator {
public:
    /// 单次构建的叶瓦片上限。coverage-first 下配额满时粗叶作占位保留（无缝隙不留洞），且 DFS 近侧优先
    /// 递归——故本上限只约束「地平线附近多粗」，不影响中心带锐度（近景细级先被锁定）。
    /// 取值需≤纹理缓存 kMaxCachedTiles：否则叶数超缓存→「驱逐→下帧 miss→重请求→图源限流(429)」冲刷
    /// （曾只剩棋盘）。native OOM 已由 TileLoader.ready_ 高水位背堆主，不再靠本值防洪。
    static constexpr int kMaxTiles = 640;

    /**
     * 构建可见叶瓦片集合。
     * @param cam           3D 相机快照（须 valid）
     * @param targetLevel   期望叶级别（= Navigator::tileLevel(maxLevel)，地物最细级别）
     * @param maxLevel      该图源最大级别（不超过它）
     * @param tanHalfFov    垂直半视场角正切
     * @param viewportHeightPx 视口高（像素）
     * @param detailFactor  LOD 阈值（detailControl·densityFactor，越大越粗）
     * @param globe         地球投影（提供地理↔笛卡尔与包围球）
     * @param out           输出可见叶瓦片（调用方负责清空）
     */
    static void buildVisibleTiles(const TessCamera &cam, int targetLevel, int maxLevel,
                                  double tanHalfFov, int viewportHeightPx, double detailFactor,
                                  const Globe &globe, std::vector<GlobeTile> &out);

private:
    /// 内部实现已下沉到 .cpp 匿名 ns：subdivide(z,x,y, SubdivCtx&, out) 按 参考实现 四步执行。
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_TESSELLATOR_H
