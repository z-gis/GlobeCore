#include "navigator/Navigator.h"

#include "globe/MercatorProjection.h"
#include "globe/Wgs84Globe.h"

#include <algorithm>
#include <cmath>

namespace globecore {

namespace {
constexpr double kPi = 3.14159265358979323846;
// zoom 推导中的常量 512 = 2 * 256（见 zoomUnlocked 注释）
constexpr double kZoomDenomFactor = 512.0;
// 相机高度固定下限（米）：与 app 显示级别口径同源的放大到底值
// （log2(赤道周长/38.2)≈20，与 app MercatorZoom.MIN_CAMERA_ALTITUDE 同源），
// 见 clampAltitude 注释
constexpr double kMinCameraAltitudeMeters = 38.2;

inline double tanHalfFov(double fovDeg) { return std::tan(fovDeg * 0.5 * kPi / 180.0); }
} // namespace

void Navigator::setViewport(int widthPx, int heightPx) {
    std::lock_guard<std::mutex> lk(mtx_);
    viewportWidth_ = widthPx > 0 ? widthPx : 1;
    viewportHeight_ = heightPx > 0 ? heightPx : 1;
    // 视口确定后才能据其换算高度区间，故此时补做一次钳制（setCamera 可能早于 Surface 创建）
    clampAltitude();
}

void Navigator::setCamera(const Camera &camera) {
    std::lock_guard<std::mutex> lk(mtx_);
    centerLon_ = camera.longitude;
    centerLat_ = MercatorProjection::clampLatitude(camera.latitude);
    altitude_ = camera.altitude > 0.0 ? camera.altitude : 1.0;
    // 姿态建模值：fieldOfView 参与后续 zoom 换算（限幅到合理区间避免除零/异常），
    // heading/tilt 随存供 3D 通路（camera3D）消费、roll/altitudeMode 暂仅留存，均供 camera() 完整回读。
    fieldOfViewDeg_ = (camera.fieldOfView > 0.0 && camera.fieldOfView < 180.0)
                          ? camera.fieldOfView
                          : FIELD_OF_VIEW_DEG;
    heading_ = camera.heading;
    tilt_ = camera.tilt;
    roll_ = camera.roll;
    altitudeMode_ = camera.altitudeMode;
    clampAltitude();
}

void Navigator::setDisplayDensity(double density) {
    std::lock_guard<std::mutex> lk(mtx_);
    densityFactor_ = density > 0.0 ? density : 1.0;
}

void Navigator::setDetailControl(double detailControl) {
    std::lock_guard<std::mutex> lk(mtx_);
    detailControl_ = detailControl > 0.0 ? detailControl : 1.0;
}

void Navigator::setViewMode(ViewMode mode) {
    std::lock_guard<std::mutex> lk(mtx_);
    mode_ = mode;
    // 切到 3D 立即按整球上限钳制高度（2D 可缩到远高于整球可见的高度），避免切换后首帧地球缩成一点
    clampAltitude();
}

Navigator::ViewMode Navigator::viewMode() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mode_;
}

double Navigator::fieldOfViewDeg() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return fieldOfViewDeg_;
}

Navigator::Camera3D Navigator::camera3D() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return camera3DUnlocked();
}

Navigator::Camera3D Navigator::camera3DUnlocked() const {
    Camera3D c;
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return c; // valid=false
    const Wgs84Globe &g = Wgs84Globe::instance();
    g.geographicToCartesian(centerLon_, centerLat_, altitude_, c.eye);
    g.geographicToCartesian(centerLon_, centerLat_, 0.0, c.center);
    // 局部 ENU 基准（单位向量）：法向 n、东 e、北 north
    const double lat = centerLat_ * kPi / 180.0;
    const double lon = centerLon_ * kPi / 180.0;
    const double sinLat = std::sin(lat), cosLat = std::cos(lat);
    const double sinLon = std::sin(lon), cosLon = std::cos(lon);
    const Vec3 north{-sinLat * cosLon, -sinLat * sinLon, cosLat};
    const Vec3 east{-sinLon, cosLon, 0.0};
    // 屏幕上方向 = 北绕当地垂线旋转 heading（0=北向上）；heading 缺省时 up=north
    const double h = heading_ * kPi / 180.0;
    c.up = (north * std::cos(h) - east * std::sin(h)).normalized();
    // 仰角 tilt（度）：0=正下（nadir，eye 在目标正上方、视线沿 -n）。>0 采用「绕目标俯仰 + 保眼高」：
    // 目标地面点 T(=geo(centerLon/Lat,0)，即 c.center) 恒居屏心——倾视时中心不漂移（用户预期）。
    // 眼沿视线反方向后退，并把斜距拉长到 altitude/cos t，使眼的大地高≈altitude 不随倾角下沉：
    // 若固定斜距=altitude（纯 orbit）则眼高≈alt·cos t，倾视时眼骤低→地平线骤近→视锥指向的远景
    // 落到地平线下被 Tessellator 剔除→远处瓦片缺失。保眼高后地平线仍远、远景正常渲染。
    // north/east⟂地理法向 nrm ⟹ forward=u·sin t−nrm·cos t 与 up=u·cos t+nrm·sin t 均单位且正交；
    // |center−eye|=altitude/cos t（vecAlt 随倾角略增，已被 [2,500] clamp 兜住）。tilt=0 不进此分支、逐位同旧 nadir。
    if (tilt_ != 0.0) {
        Vec3 nrm;
        g.geographicNormal(centerLon_, centerLat_, nrm);
        double t = tilt_ * kPi / 180.0;
        // 眼点沉地保护：本模型把斜距拉长到 alt/cos t（保眼高），而眼沿中心法向的反方向后退——
        // alt/cos t 超过当地椭球半径时眼落到地面以下：horizonVisible 全假 → Tessellator 剔光全部
        // 瓦片 → 只剩纯黑背景（实测 3D 带倾角缩小到一定高度后整屏黑屏，tilt=0 永不触发：后退
        // 方向即法向本身）。故每帧按当前高度钳制「有效倾角」使斜距 ≤ 当地半径：只削渲染用
        // 角度、不动存储值，拉回后满倾角无缝恢复（同 rotateTilt 到底不响应、反向即回的语义）。
        const double latR = centerLat_ * kPi / 180.0;
        const double sinL = std::sin(latR), cosL = std::cos(latR);
        const double rSurf = std::sqrt(std::pow(Wgs84Globe::A * cosL, 2.0) +
                                       std::pow(Wgs84Globe::B * sinL, 2.0)); // 中心法向与椭球交点距地心距
        const double slant = altitude_ / std::cos(t);
        if (slant > rSurf) {
            const double ratio = altitude_ / rSurf; // 可能 >1：此时任意倾角均安全，不钳
            if (ratio < 1.0) t = std::acos(ratio);  // alt/cos t = rSurf 的临界倾角（acos 单调，削小 t）
        }
        const Vec3 uDir = c.up; // 水平屏幕上方（已归一、⟂ nrm）
        const Vec3 forward = uDir * std::sin(t) - nrm * std::cos(t); // 单位视线（由 -nrm 向地平线倾 t，指向 T）
        c.eye = c.center - forward * (altitude_ / std::cos(t));      // 目标不动，眼后退且保持眼高
        c.up = uDir * std::cos(t) + nrm * std::sin(t);
    }
    c.forward = (c.center - c.eye).normalized();
    c.view = Matrix4::lookAt(c.eye, c.center, c.up);
    const double aspect = static_cast<double>(viewportWidth_) / static_cast<double>(viewportHeight_);
    const double fovRad = fieldOfViewDeg_ * kPi / 180.0;
    double nearD = altitude_ > 200.0 ? altitude_ * 0.02 : 0.5;
    if (nearD <= 0.0) nearD = 1.0;
    const double farD = altitude_ + 2.2 * Wgs84Globe::A; // 覆盖到地平线及背面
    c.nearDistance = nearD;
    c.farDistance = farD;
    c.proj = Matrix4::perspective(static_cast<float>(fovRad), static_cast<float>(aspect),
                                  static_cast<float>(nearD), static_cast<float>(farD));
    // 视锥 6 平面 double 直构（不经过 float viewProj）：避免大 alt 下 row3±row2 抵消引发的
    // 归一化放大噪声（实测 leaves 在 alt 13M↔14M 间非单调翻转即此因）。
    // 基：right = forward × up，up2 = right × forward（严格正交）。水平/垂直半 fov：
    //   tanHy = tan(fovY/2)，tanHx = tanHy·aspect（与 proj m[0]=f/aspect 同口径）。
    //   sinH = tanH/sqrt(1+tanH²)，cosH = 1/sqrt(1+tanH²)。
    const Vec3 right = c.forward.cross(c.up).normalized();
    const Vec3 up2 = right.cross(c.forward);
    const double sinHy = std::sin(fovRad * 0.5);
    const double cosHy = std::cos(fovRad * 0.5);
    const double tanHy = std::tan(fovRad * 0.5);
    const double tanHx = tanHy * aspect;
    const double sinHx = tanHx / std::sqrt(1.0 + tanHx * tanHx);
    const double cosHx = 1.0 / std::sqrt(1.0 + tanHx * tanHx);
    auto mk = [](const Vec3 &n, const Vec3 &e) -> FrustumPlane {
        FrustumPlane fp;
        fp.a = n.x; fp.b = n.y; fp.c = n.z; fp.d = -n.dot(e);
        return fp;
    };
    // left/right/bottom/top：平面过眼点，d = -dot(eye, n)。inside(P) ⇔ dot(P-eye, n) ≥ 0。
    c.frustumPlanes[0] = mk(right * cosHx + c.forward * sinHx, c.eye); // left
    c.frustumPlanes[1] = mk(right * (-cosHx) + c.forward * sinHx, c.eye); // right
    c.frustumPlanes[2] = mk(up2 * cosHy + c.forward * sinHy, c.eye);   // bottom
    c.frustumPlanes[3] = mk(up2 * (-cosHy) + c.forward * sinHy, c.eye);   // top
    // near/far：平面不经过眼点，沿 forward 偏移 nearD / farD。inside 判据同形式。
    c.frustumPlanes[4] = mk(c.forward, c.eye + c.forward * nearD);       // near
    c.frustumPlanes[5] = mk(c.forward * (-1.0), c.eye + c.forward * farD); // far
    c.valid = true;
    return c;
}

bool Navigator::screenToGeo3D(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return screenToGeo3DUnlocked(sxPx, syPx, outLonDeg, outLatDeg);
}

bool Navigator::screenToGeo3DUnlocked(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const {
    const Camera3D c = camera3DUnlocked();
    if (!c.valid) return false;
    const double vw = static_cast<double>(viewportWidth_);
    const double vh = static_cast<double>(viewportHeight_);
    const double fovDeg = fieldOfViewDeg_;
    const double ndcX = 2.0 * sxPx / vw - 1.0;
    const double ndcY = 1.0 - 2.0 * syPx / vh;
    const double aspect = vw / vh;
    const double tanHalf = std::tan(fovDeg * kPi / 180.0 * 0.5);
    const Vec3 right = c.forward.cross(c.up).normalized();
    const Vec3 up2 = right.cross(c.forward);
    const Vec3 dir = (c.forward + right * (ndcX * tanHalf * aspect) + up2 * (ndcY * tanHalf)).normalized();
    const Ray ray{c.eye, dir};
    Vec3 hit;
    if (!Wgs84Globe::instance().intersectRay(ray, hit)) return false;
    double alt = 0.0;
    Wgs84Globe::instance().cartesianToGeographic(hit, outLonDeg, outLatDeg, alt);
    return true;
}

void Navigator::panByPixels(double dxPx, double dyPx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return;
    if (mode_ == ViewMode::MODE_3D) {
        // 3D：把像素位移按「地面米/像素」换算为中心经纬度角位移，绕球滚动（地图跟随手指）。
        // 地面分辨率沿用近正下视角口径：mPerPx = 2·alt·tan(fov/2)/视口高（与 zoom 换算一致）。
        // 位移方向按相机屏幕轴折算（非正东/正北）：heading 旋转/tilt 倾视后屏幕上、右不再对齐北/东，
        // 视窗平移量 = (dyPx·屏幕上 − dxPx·屏幕右)·mPerPx（内容随指反向），取其当地东/北分量后
        // 逐轴换算角位移；heading=0、tilt=0 时与旧「dx → 经度、dy → 纬度」直折逐位等价。
        const double mPerPx = 2.0 * altitude_ * tanHalfFov(fieldOfViewDeg_) /
                              static_cast<double>(viewportHeight_);
        const double rad2Deg = 180.0 / kPi;
        const double lat = centerLat_ * kPi / 180.0;
        const double lon = centerLon_ * kPi / 180.0;
        const Vec3 northV{-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat)};
        const Vec3 eastV{-std::sin(lon), std::cos(lon), 0.0};
        // 屏幕基优先取自相机快照（含 heading/tilt）；视锥未就绪时回退正北/正东（与旧行为一致）
        Vec3 screenUp = northV, screenRight = eastV;
        const Camera3D c = camera3DUnlocked();
        if (c.valid) {
            screenUp = c.up;
            screenRight = c.forward.cross(c.up).normalized();
        }
        const Vec3 shift = screenUp * (dyPx * mPerPx) - screenRight * (dxPx * mPerPx);
        const double dNorth = shift.dot(northV);
        const double dEast = shift.dot(eastV);
        // 向下拖（dyPx>0）→ 视窗向屏幕上方移 → 正北时中心纬度增大；向右拖（dxPx>0）→ 视窗西移 → 经度减小
        centerLat_ = MercatorProjection::clampLatitude(centerLat_ + dNorth / EQUATORIAL_RADIUS * rad2Deg);
        // 高纬按 cos(lat) 收敛经线
        double cosLat = std::cos(lat);
        if (cosLat < 1e-3) cosLat = 1e-3;
        centerLon_ += dEast / (EQUATORIAL_RADIUS * cosLat) * rad2Deg;
        while (centerLon_ < -180.0) centerLon_ += 360.0;
        while (centerLon_ >= 180.0) centerLon_ -= 360.0;
        return;
    }
    double cwx = 0.0, cwy = 0.0;
    worldCenterUnlocked(cwx, cwy);
    const double spanW = 2.0 * halfWorldWidthUnlocked();
    const double spanH = 2.0 * halfWorldHeightUnlocked();
    // 地图跟随手指：中心向拖动反方向移动。世界 y 向南增长、且 wy=cwy-ndcY·hh，
    // 故屏幕向下拖动（dyPx>0，内容随之下移）→ 中心 wy 减小。
    cwx -= (dxPx / viewportWidth_) * spanW;
    cwy -= (dyPx / viewportHeight_) * spanH;
    setCenterWorld(cwx, cwy);
}

void Navigator::zoomBy(double factor, double focusXpx, double focusYpx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (factor <= 0.0 || viewportWidth_ <= 0 || viewportHeight_ <= 0) return;
    if (mode_ == ViewMode::MODE_3D) {
        // 3D 缩放锚定，对齐 参考实现 PivotAnchorState 纯缩放分支（L_new = A + s·(L_begin − A)）：
        // 焦点的地面交点 A 保持在屏幕原处，中心沿 A—中心 按 s=新高度/旧高度 滑动。
        // 勿用 2D 正交焦点重投影：其世界坐标口径在透视 3D 下无效，高空拉近一瞬间
        // hw 跳变使中心 ndcX·(hw_old−hw_new) 级大跳，视野被甩飞并伴随瓦片风暴。
        double aLon = 0.0, aLat = 0.0;
        const bool anchored = screenToGeo3DUnlocked(focusXpx, focusYpx, aLon, aLat);
        const double altBegin = altitude_;
        altitude_ /= factor;
        clampAltitude();
        if (anchored) {
            const double s = altitude_ / altBegin;
            centerLon_ = aLon + s * (centerLon_ - aLon);
            while (centerLon_ < -180.0) centerLon_ += 360.0;
            while (centerLon_ >= 180.0) centerLon_ -= 360.0;
            centerLat_ = MercatorProjection::clampLatitude(aLat + s * (centerLat_ - aLat));
        }
        // 焦点落在地平线外（射线未命中）时退化为只改高度不动中心，同 参考实现 capture 失败即 invalidate
        return;
    }
    // 1) 缩放前焦点处的世界坐标与其 NDC（缩放中屏幕位置不变）
    double fwx = 0.0, fwy = 0.0;
    screenToWorldUnlocked(focusXpx, focusYpx, fwx, fwy);
    const double ndcX = 2.0 * focusXpx / viewportWidth_ - 1.0;
    const double ndcY = 1.0 - 2.0 * focusYpx / viewportHeight_;
    // 2) 缩放：factor>1 放大/拉近 → 相机高度降低（zoom 随高度减小而增大）
    altitude_ /= factor;
    clampAltitude();
    // 3) 令焦点世界点在缩放后仍落在同一屏幕位置：wx=cwx+ndcX·hw, wy=cwy-ndcY·hh
    const double hw = halfWorldWidthUnlocked();
    const double hh = halfWorldHeightUnlocked();
    const double cwx = fwx - ndcX * hw;
    const double cwy = fwy + ndcY * hh;
    setCenterWorld(cwx, cwy);
}

void Navigator::rotateHeading(double deltaDeg) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (deltaDeg == 0.0) return;
    heading_ += deltaDeg;
    // 环绕归一到 [-180, 180)：手势逐帧累加防无界增长（三角函数渲染不受影响，但回读/持久化保持紧凑）
    while (heading_ >= 180.0) heading_ -= 360.0;
    while (heading_ < -180.0) heading_ += 360.0;
}

void Navigator::rotateTilt(double deltaDeg) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (deltaDeg == 0.0) return;
    tilt_ += deltaDeg;
    // 钳制到 [0, MAX_TILT_DEG]：0=正下 nadir（2D 语义回正），上限防近地平线退化视角（手势侧逐帧累加，
    // 到底后继续同向拖动无效果，反向即回，无需额外状态）
    if (tilt_ < 0.0) tilt_ = 0.0;
    if (tilt_ > MAX_TILT_DEG) tilt_ = MAX_TILT_DEG;
}

double Navigator::zoom() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return zoomUnlocked();
}

int Navigator::displayLevel() const {
    std::lock_guard<std::mutex> lk(mtx_);
    // 与 app MercatorZoom.altitudeToLevel 逐字同源：level = floor(log2(赤道周长 / 相机高度))，钳 0..20。
    // 仅依赖高度（不含视口高/fov 项），与用户界面看到并设定的级别一致，供矢量级别可见性门控。
    if (altitude_ <= 0.0) return 0;
    double level = std::log2(CIRCUMFERENCE / altitude_);
    if (level < 0.0) level = 0.0;
    if (level > static_cast<double>(DISPLAY_MAX_LEVEL)) level = static_cast<double>(DISPLAY_MAX_LEVEL);
    return static_cast<int>(level);
}

double Navigator::zoomUnlocked() const {
    // 2^zoom = 周长·视口高 / (512·tan(fov/2)·alt)，对齐 参考实现 pixelSizeAtDistance 的地面分辨率
    const double denom = kZoomDenomFactor * tanHalfFov(fieldOfViewDeg_) * altitude_;
    if (denom <= 0.0) return static_cast<double>(MAX_LEVEL);
    const double pow2 = CIRCUMFERENCE * static_cast<double>(viewportHeight_) / denom;
    if (pow2 <= 0.0) return static_cast<double>(MIN_LEVEL);
    double z = std::log2(pow2);
    if (z < static_cast<double>(MIN_LEVEL)) z = static_cast<double>(MIN_LEVEL);
    if (z > static_cast<double>(MAX_LEVEL)) z = static_cast<double>(MAX_LEVEL);
    return z;
}

void Navigator::worldCenter(double &wx, double &wy) const {
    std::lock_guard<std::mutex> lk(mtx_);
    worldCenterUnlocked(wx, wy);
}

void Navigator::screenToWorld(double sxPx, double syPx, double &wx, double &wy) const {
    std::lock_guard<std::mutex> lk(mtx_);
    screenToWorldUnlocked(sxPx, syPx, wx, wy);
}

void Navigator::worldCenterUnlocked(double &wx, double &wy) const {
    MercatorProjection::lonLatToWorld(centerLon_, centerLat_, wx, wy);
}

Camera Navigator::camera() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Camera c;
    c.latitude = centerLat_;
    c.longitude = centerLon_;
    c.altitude = altitude_;
    c.heading = heading_;
    c.tilt = tilt_;
    c.roll = roll_;
    c.fieldOfView = fieldOfViewDeg_;
    c.altitudeMode = altitudeMode_;
    return c;
}

void Navigator::screenToWorldUnlocked(double sxPx, double syPx, double &wx, double &wy) const {
    double cwx = 0.0, cwy = 0.0;
    worldCenterUnlocked(cwx, cwy);
    const double ndcX = (viewportWidth_ > 0) ? (2.0 * sxPx / viewportWidth_ - 1.0) : 0.0;
    const double ndcY = (viewportHeight_ > 0) ? (1.0 - 2.0 * syPx / viewportHeight_) : 0.0;
    wx = cwx + ndcX * halfWorldWidthUnlocked();
    wy = cwy - ndcY * halfWorldHeightUnlocked(); // wy = cwy - ndcY·hh（世界 y 向南增长）
}

void Navigator::setCenterWorld(double wx, double wy) {
    double lon = 0.0, lat = 0.0;
    MercatorProjection::worldToLonLat(wx, wy, lon, lat);
    // 经度环绕到 [-180, 180)
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    centerLon_ = lon;
    centerLat_ = MercatorProjection::clampLatitude(lat);
}

void Navigator::clampAltitude() {
    // 视口未知（Surface 未创建）时无法换算高度区间，跳过；setViewport 后会补做
    if (viewportHeight_ <= 1) return;
    // alt = 周长·视口高 / (512·tan(fov/2)·2^zoom)，zoom∈[MIN_LEVEL,MAX_LEVEL]
    const double k = CIRCUMFERENCE * static_cast<double>(viewportHeight_) /
                     (kZoomDenomFactor * tanHalfFov(fieldOfViewDeg_));
    // 高度下限取「zoom=MAX_LEVEL 反推」与「固定 38.2m」的较大者：
    // 本 zoom 口径含视口高/FOV 项，与 app 显示级别 log2(周长/alt) 相差 log2(vh/212)≈3~4 级，
    // 仅按 zoom 反推时到底高度随视口高漂移（显示级别随之漂移）；固定下限使任意视口下
    // 显示级别恒钳在 ≈20（与原主界面放大量对齐，MAX_LEVEL=24 仅作 zoom 硬上限兜底）。
    const double altMin = std::max(k / std::pow(2.0, static_cast<double>(MAX_LEVEL)),
                                   kMinCameraAltitudeMeters);
    // 高度上限（缩小极限）：
    //  · 3D：对齐参考实现 globe 控制器 applyLimits 口径的 maxRange = distanceToViewGlobeExtents × 2，
    //    其中 distanceToViewGlobeExtents = R/sin(fov/2) − R（整球恰好竖直填满视口的高度）。×2 后整球约占
    //    视口高 60%，对应 app 显示级别恰为 0——缩到「看到整个地球」即止，不再作无意义的缩小。
    //  · 2D：沿用 zoom=MIN_LEVEL 反推（2^MIN_LEVEL=1，整幅墨卡托世界可见即止），避免 2D 缩小回归。
    double altMax = k;
    if (mode_ == ViewMode::MODE_3D) {
        const double sinHalfFov = std::sin(fieldOfViewDeg_ * kPi / 180.0 * 0.5);
        if (sinHalfFov > 1e-6) {
            altMax = (EQUATORIAL_RADIUS / sinHalfFov - EQUATORIAL_RADIUS) * 2.0;
        }
    }
    if (altitude_ < altMin) altitude_ = altMin;
    if (altitude_ > altMax) altitude_ = altMax;
}

double Navigator::halfWorldWidth() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return halfWorldWidthUnlocked();
}

double Navigator::halfWorldWidthUnlocked() const {
    const double worldPx = TILE_PIXELS * std::pow(2.0, zoomUnlocked());
    return viewportWidth_ / (2.0 * worldPx);
}

double Navigator::halfWorldHeight() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return halfWorldHeightUnlocked();
}

double Navigator::halfWorldHeightUnlocked() const {
    const double worldPx = TILE_PIXELS * std::pow(2.0, zoomUnlocked());
    return viewportHeight_ / (2.0 * worldPx);
}

int Navigator::tileLevel(int maxLevel) const {
    std::lock_guard<std::mutex> lk(mtx_);
    // 复刻 参考实现 Tile.mustSubdivide 的 2D 分支（在 cos(纬度) 调整前返回，故与纬度无关）：
    //   texelSize(L)·equatorialRadius > pixelSize·detailControl·densityFactor 时细分。
    // 其中 texelSize(L)=周长/(256·2^L)、pixelSize=周长/(256·2^zoom)（见 zoomUnlocked），代入化简得
    //   细分 ⇔ zoom > L + log2(detailControl·densityFactor)。
    // 自顶向下递归，叶级别 = 最小的 L 使 zoom ≤ L + log2(threshold) = ceil(zoom - log2(threshold))。
    const double threshold = detailControl_ * densityFactor_;
    const double z = zoomUnlocked();
    int level = (threshold > 0.0)
                    ? static_cast<int>(std::ceil(z - std::log2(threshold)))
                    : static_cast<int>(std::ceil(z));
    if (level < MIN_LEVEL) level = MIN_LEVEL;
    // 末级兜底：不超过调用方传入的图源 maxLevel（逐层钳制）；zoom 继续增大时本值恒为 maxLevel，由投影拉伸末级瓦片纹理。
    int maxLv = maxLevel;
    if (maxLv < MIN_LEVEL) maxLv = MIN_LEVEL;
    if (maxLv > MAX_LEVEL) maxLv = MAX_LEVEL;
    if (level > maxLv) level = maxLv;
    return level;
}

} // namespace globecore
