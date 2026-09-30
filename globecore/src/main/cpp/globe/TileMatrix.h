#ifndef GLOBECORE_GLOBE_TILE_MATRIX_H
#define GLOBECORE_GLOBE_TILE_MATRIX_H

#include <cmath>

namespace globecore {

/** 瓦片标识（对应 参考实现的 Tile，含级别与行列号） */
struct TileId {
    int z = 0;
    int x = 0;
    int y = 0;
};

/**
 * 瓦片网格数学，对应 参考实现 globe 的 Tesselator/TileMatrix 职责（第一版只做平面网格，不做球面细分）。
 *
 * 级别 level 下全球为 2^level x 2^level 张瓦片；瓦片 (x,y) 覆盖归一化世界矩形
 * [x/2^level,(x+1)/2^level] x [y/2^level,(y+1)/2^level]。
 */
class TileMatrix {
public:
    /// 级别 level 的单边瓦片数（2^level）
    static int tilesAtLevel(int level) { return 1 << level; }

    /**
     * 计算可见瓦片行列范围（含端点）。
     * @param cwx,cwy 相机中心的归一化世界坐标
     * @param hw,hh   可视范围的半宽/半高（归一化世界单位）
     * @param level   瓦片级别
     * @param txMin..tyMax 输出：可见瓦片行列闭区间（y 已裁剪到 [0,n-1]，x 同样裁剪，暂不做跨经度 180° 环绕）
     */
    static void visibleTileRange(double cwx, double cwy, double hw, double hh, int level,
                                 int &txMin, int &txMax, int &tyMin, int &tyMax) {
        const int n = tilesAtLevel(level);
        const double nd = static_cast<double>(n);

        const double wxMin = cwx - hw;
        const double wxMax = cwx + hw;
        const double wyMin = cwy - hh;
        const double wyMax = cwy + hh;

        txMin = static_cast<int>(std::floor(wxMin * nd));
        txMax = static_cast<int>(std::floor(wxMax * nd));
        tyMin = static_cast<int>(std::floor(wyMin * nd));
        tyMax = static_cast<int>(std::floor(wyMax * nd));

        // 纬度方向裁剪到有效范围
        if (tyMin < 0) tyMin = 0;
        if (tyMax > n - 1) tyMax = n - 1;
        // 经度方向：Phase B 先裁剪，跨 180° 环绕留待后续（避免初期复杂度）
        if (txMin < 0) txMin = 0;
        if (txMax > n - 1) txMax = n - 1;
    }
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_TILE_MATRIX_H
