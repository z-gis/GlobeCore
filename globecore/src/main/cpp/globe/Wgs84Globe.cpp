#include "globe/Wgs84Globe.h"

#include <cmath>

namespace globecore {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
// 第二偏心率平方 e'² = e²/(1−e²)
constexpr double kEp2 = Wgs84Globe::E2 / (1.0 - Wgs84Globe::E2);
} // namespace

const Wgs84Globe &Wgs84Globe::instance() {
    static const Wgs84Globe kInstance;
    return kInstance;
}

void Wgs84Globe::geographicToCartesian(double lonDeg, double latDeg, double altMeters, Vec3 &out) const {
    const double lat = latDeg * kDeg2Rad;
    const double lon = lonDeg * kDeg2Rad;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    // 卯酉圈曲率半径 N
    const double N = A / std::sqrt(1.0 - E2 * sinLat * sinLat);
    const double r = N + altMeters;
    out.x = r * cosLat * std::cos(lon);
    out.y = r * cosLat * std::sin(lon);
    out.z = (N * (1.0 - E2) + altMeters) * sinLat;
}

void Wgs84Globe::geographicNormal(double lonDeg, double latDeg, Vec3 &outNormal) const {
    // 椭球面地理法向（以大地纬度/经度表示）：n = (cosφ·cosλ, cosφ·sinλ, sinφ)
    const double lat = latDeg * kDeg2Rad;
    const double lon = lonDeg * kDeg2Rad;
    const double cosLat = std::cos(lat);
    outNormal.x = cosLat * std::cos(lon);
    outNormal.y = cosLat * std::sin(lon);
    outNormal.z = std::sin(lat);
}

void Wgs84Globe::cartesianToGeographic(const Vec3 &cart, double &lonDeg, double &latDeg, double &altMeters) const {
    const double x = cart.x, y = cart.y, z = cart.z;
    const double lon = std::atan2(y, x);
    const double p = std::sqrt(x * x + y * y);
    // Bowring 初值：参量纬度 θ
    const double theta = std::atan2(z * A, p * B);
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);
    double lat = std::atan2(z + kEp2 * B * sinTheta * sinTheta * sinTheta,
                            p - E2 * A * cosTheta * cosTheta * cosTheta);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double N = A / std::sqrt(1.0 - E2 * sinLat * sinLat);
    // 海拔：赤道附近用 p/cosφ，极地附近用 z/sinφ，避免除零
    double alt;
    if (std::fabs(cosLat) > 1e-9) {
        alt = p / cosLat - N;
    } else {
        alt = std::fabs(z) / std::fabs(sinLat) - N * (1.0 - E2);
    }
    lonDeg = lon * kRad2Deg;
    latDeg = lat * kRad2Deg;
    altMeters = alt;
}

bool Wgs84Globe::intersectRay(const Ray &ray, Vec3 &out) const {
    // 把射线变换到「归一化椭球→单位球」空间求交：坐标除以对应半轴。
    const double ox = ray.origin.x / A, oy = ray.origin.y / A, oz = ray.origin.z / B;
    const double dx = ray.direction.x / A, dy = ray.direction.y / A, dz = ray.direction.z / B;
    const double qa = dx * dx + dy * dy + dz * dz;
    const double qb = 2.0 * (ox * dx + oy * dy + oz * dz);
    const double qc = ox * ox + oy * oy + oz * oz - 1.0;
    if (qa < 1e-18) return false; // 方向退化
    const double disc = qb * qb - 4.0 * qa * qc;
    if (disc < 0.0) return false; // 无实交点
    const double sq = std::sqrt(disc);
    // 取最近正向交点（近裁剪）：先试较小根，为负再试较大根
    double t = (-qb - sq) / (2.0 * qa);
    if (t < 0.0) t = (-qb + sq) / (2.0 * qa);
    if (t < 0.0) return false; // 交点全在起点后方
    out = ray.origin + ray.direction * t;
    return true;
}

} // namespace globecore
