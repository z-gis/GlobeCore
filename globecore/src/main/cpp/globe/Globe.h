#ifndef GLOBECORE_GLOBE_GLOBE_H
#define GLOBECORE_GLOBE_GLOBE_H

#include "geom/Vec3.h"

namespace globecore {

/**
 * Globe：地理坐标（WGS84 经纬度/海拔）↔ 引擎笛卡尔坐标（ECEF 米）之间的变换接口。
 * 对应 参考实现的 globe.projection.GeographicProjection——引擎只有唯一一条笛卡尔渲染管线，
 * 2D/3D 只是「用哪个投影把经纬度落到笛卡尔空间」的差异（见 doc/模块/3d球体调研.md 第八节）。
 *
 * 本接口是投影化改造（路线 A'）的收口点：3D 球体模式用 Wgs84Globe（经纬度→椭球直角坐标）；
 * 未来把 2D 也迁入统一笛卡尔管线时，新增一个平面墨卡托 Globe（米制 x=a·lon、y=墨卡托拉伸、z=海拔）
 * 实现本接口即可，切换只换投影对象 + 相机重置，渲染管线不动。
 *
 * P1 阶段：3D 通路消费本接口（Wgs84Globe）；现有 2D 通路仍走 MercatorProjection 归一化平面（零回归）。
 */
class Globe {
public:
    virtual ~Globe() = default;

    /// 是否为 2D 平面投影（Wgs84Globe 返回 false）。对应 参考实现 Globe.is2D。
    virtual bool is2D() const { return false; }

    /// 赤道半径（米）
    virtual double equatorialRadius() const = 0;

    /// 经纬度（度）+ 海拔（米）→ ECEF 直角坐标（米）。对应 参考实现 geographicToCartesian。
    virtual void geographicToCartesian(double lonDeg, double latDeg, double altMeters, Vec3 &out) const = 0;

    /// ECEF 直角坐标（米）→ 经纬度（度）+ 海拔（米）。对应 参考实现 cartesianToGeographic。
    virtual void cartesianToGeographic(const Vec3 &cart, double &lonDeg, double &latDeg, double &altMeters) const = 0;

    /// 某经纬度处的地表单位法向（椭球面外法线，ECEF 方向）。用作相机 up / 瓦片朝向基准。
    virtual void geographicNormal(double lonDeg, double latDeg, Vec3 &outNormal) const = 0;

    /**
     * 射线与椭球求交，取第一个正向交点（忽略起点后方）。用于 3D 下屏幕点拾取/反算地理坐标。
     * @param ray   起点 + 单位方向（ECEF 米）
     * @param out   命中点（ECEF 米）
     * @return 命中返回 true
     */
    virtual bool intersectRay(const Ray &ray, Vec3 &out) const = 0;
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_GLOBE_H
