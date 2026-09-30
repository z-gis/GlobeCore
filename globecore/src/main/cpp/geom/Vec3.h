#ifndef GLOBECORE_GEOM_VEC3_H
#define GLOBECORE_GEOM_VEC3_H

#include <cmath>

namespace globecore {

/**
 * 三维向量（double），用于 3D 球体模式下的地心直角坐标（ECEF，米）、相机基向量、
 * 射线与椭球求交等。对应 参考实现 geom.Vec3 的标量实现口径（本模块不引入包装对象，直接用 POD 风格）。
 *
 * 单位约定：坐标以米计（椭球半径量级 ~6.4e6），故涉及坐标差值运算一律先在 double 下完成，
 * 再转 float 上传 GPU（RTC/相对眼点，避免 float32 大数抵消丢精度）。
 */
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3 &r) const { return {x + r.x, y + r.y, z + r.z}; }
    Vec3 operator-(const Vec3 &r) const { return {x - r.x, y - r.y, z - r.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3 &operator+=(const Vec3 &r) { x += r.x; y += r.y; z += r.z; return *this; }
    Vec3 &operator-=(const Vec3 &r) { x -= r.x; y -= r.y; z -= r.z; return *this; }

    double dot(const Vec3 &r) const { return x * r.x + y * r.y + z * r.z; }
    Vec3 cross(const Vec3 &r) const {
        return {y * r.z - z * r.y, z * r.x - x * r.z, x * r.y - y * r.x};
    }
    double lengthSquared() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(lengthSquared()); }

    /// 归一化并返回；长度过小时返回零向量（避免除零）
    Vec3 normalized() const {
        const double len = length();
        return len > 1e-12 ? Vec3{x / len, y / len, z / len} : Vec3{};
    }

    /// 逐分量取负
    Vec3 negated() const { return {-x, -y, -z}; }
};

inline Vec3 operator*(double s, const Vec3 &v) { return v * s; }

/**
 * 射线（起点 + 单位方向），用于 3D 下的屏幕点→地理反算（拾取）与射线∩椭球求交。
 * 对应 参考实现 geom.Line。方向假定已归一。
 */
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

} // namespace globecore

#endif // GLOBECORE_GEOM_VEC3_H
