#ifndef GLOBECORE_GLOBE_MERCATOR_PROJECTION_H
#define GLOBECORE_GLOBE_MERCATOR_PROJECTION_H

namespace globecore {

/**
 * Web 墨卡托（EPSG:3857）投影数学，对应 参考实现 globe 的投影层。
 *
 * 世界坐标归一化到 [0,1]x[0,1]：
 *  - wx：经度 -180..180 映射到 0..1；
 *  - wy：纬度 +maxLat..-maxLat 映射到 0..1（0 在北极侧、1 在南极侧，即 y 向下增长）；
 * 该归一化世界坐标与缩放级别无关，瓦片 (tx,ty)@level 覆盖 [tx/2^level,(tx+1)/2^level]。
 */
class MercatorProjection {
public:
    /// 墨卡托有效纬度上限（约 85.0511°），超出会导致 y 发散
    static double maxLatitude();

    static double clampLatitude(double latDeg);

    /// 经纬度（度）→ 归一化世界坐标 [0,1]
    static void lonLatToWorld(double lonDeg, double latDeg, double &wx, double &wy);

    /// 归一化世界坐标 [0,1] → 经纬度（度）
    static void worldToLonLat(double wx, double wy, double &lonDeg, double &latDeg);
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_MERCATOR_PROJECTION_H
