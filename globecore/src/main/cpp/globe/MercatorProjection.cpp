#include "globe/MercatorProjection.h"

#include <cmath>

namespace globecore {

namespace {
constexpr double kPi = 3.14159265358979323846;
// atan(sinh(pi)) 的度数：墨卡托投影的纬度极限
constexpr double kMaxLat = 85.05112877980659;
} // namespace

double MercatorProjection::maxLatitude() { return kMaxLat; }

double MercatorProjection::clampLatitude(double latDeg) {
    if (latDeg > kMaxLat) return kMaxLat;
    if (latDeg < -kMaxLat) return -kMaxLat;
    return latDeg;
}

void MercatorProjection::lonLatToWorld(double lonDeg, double latDeg, double &wx, double &wy) {
    wx = (lonDeg + 180.0) / 360.0;
    const double latRad = clampLatitude(latDeg) * kPi / 180.0;
    // y = (1 - ln(tan(lat) + sec(lat)) / pi) / 2
    wy = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / kPi) / 2.0;
}

void MercatorProjection::worldToLonLat(double wx, double wy, double &lonDeg, double &latDeg) {
    lonDeg = wx * 360.0 - 180.0;
    const double n = kPi - 2.0 * kPi * wy;
    latDeg = 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

} // namespace globecore
