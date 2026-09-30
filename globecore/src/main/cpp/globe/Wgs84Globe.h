#ifndef GLOBECORE_GLOBE_WGS84GLOBE_H
#define GLOBECORE_GLOBE_WGS84GLOBE_H

#include "globe/Globe.h"

namespace globecore {

/**
 * Wgs84Globe：标准 WGS84 椭球（长半轴 6378137、扁率 1/298.257223563）的地理↔笛卡尔变换。
 * 3D 球体模式的 Globe 实现（对应 参考实现 projection.Wgs84Projection）——经纬度落到地心 ECEF 直角坐标，
 * 渲染管线在其上以透视相机绘制整个球面。无状态，进程内以 [instance] 单例共享。
 */
class Wgs84Globe final : public Globe {
public:
    /// 赤道半径（米），与 Navigator::EQUATORIAL_RADIUS 一致
    static constexpr double A = 6378137.0;
    /// 扁率
    static constexpr double F = 1.0 / 298.257223563;
    /// 第一偏心率平方 e² = f(2−f)
    static constexpr double E2 = F * (2.0 - F);
    /// 极半径 b = a(1−f)
    static constexpr double B = A * (1.0 - F);

    /// 进程内单例（无状态、线程安全构造）
    static const Wgs84Globe &instance();

    bool is2D() const override { return false; }
    double equatorialRadius() const override { return A; }

    void geographicToCartesian(double lonDeg, double latDeg, double altMeters, Vec3 &out) const override;
    void cartesianToGeographic(const Vec3 &cart, double &lonDeg, double &latDeg, double &altMeters) const override;
    void geographicNormal(double lonDeg, double latDeg, Vec3 &outNormal) const override;
    bool intersectRay(const Ray &ray, Vec3 &out) const override;

private:
    Wgs84Globe() = default;
};

} // namespace globecore

#endif // GLOBECORE_GLOBE_WGS84GLOBE_H
