#ifndef GLOBECORE_NAVIGATOR_NAVIGATOR_H
#define GLOBECORE_NAVIGATOR_NAVIGATOR_H

#include <array>
#include <mutex>

#include "geom/Camera.h"
#include "geom/Vec3.h"
#include "geom/Matrix4.h"

namespace globecore {

/**
 * 相机/导航器，对应 参考实现的 Navigator（LookAt）。2D 平面墨卡托通路只有中心经纬度与相机高度
 * （米，俯视时眼到地面的距离），恒正北；3D 透视通路消费 heading/tilt（见 [camera3D]）。
 *
 * 缩放口径严格对齐 参考实现：参考实现 用透视相机，其「米/屏幕像素」= alt·2·tan(fov/2)/视口高
 * （见 参考实现.pixelSizeAtDistance / RenderContext.pixelSizeAtDistance，fov 默认 45°）。
 * 本 2D 墨卡托视图的「米/像素」= 赤道周长/(TILE_PIXELS·2^zoom)。令两者相等解出
 *   zoom = log2( 周长·视口高 / (512·tan(fov/2)·alt) )
 * 从而显示范围与 参考实现 主界面一致。zoom 允许小数（连续缩放），渲染时由 [tileLevel]（复刻 参考实现
 * Tile.mustSubdivide 的 2D 判据）取叶级别 = clamp(ceil(zoom - log2(detailControl·densityFactor)), MIN_LEVEL, maxLevel)。
 * maxLevel 为各图层图源的最大级别（多图源叠加时逐层钳制，由 Renderer 传入 TileLayer::maxLevel）。
 */
class Navigator {
public:
    /// 单张瓦片的屏幕像素边长（业界标准 256）
    static constexpr double TILE_PIXELS = 256.0;
    /// 地球赤道半径（米），与 参考实现 Globe.equatorialRadius 一致
    static constexpr double EQUATORIAL_RADIUS = 6378137.0;
    /// 赤道周长（米）= 2πR
    static constexpr double CIRCUMFERENCE = 2.0 * 3.14159265358979323846 * EQUATORIAL_RADIUS;
    /// 参考实现 默认垂直视场角（度）
    static constexpr double FIELD_OF_VIEW_DEG = 45.0;

    Navigator() = default;

    /// 视图模式：2D 平面墨卡托（正交相机，现状默认）/ 3D 球体（透视轨道相机）。二者共存，界面切换。
    enum class ViewMode { MODE_2D = 0, MODE_3D = 1 };

    /// 设置视图模式（仅切标志，相机状态 lon/lat/alt/heading 两模式共用，切换即保持视角连续）。
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const;

    /// 3D 相机快照（透视轨道相机）：眼点/注视点/基向量 + view/proj 矩阵 + 近远裁剪面（均 ECEF 米）。
    /// 视口未就绪（宽高 <=0）时 valid=false。加锁快照，供 Renderer 每帧取用构建 viewProj 与视锥。
    struct Camera3D {
        bool valid = false;
        Vec3 eye;      // 眼点（ECEF 米）
        Vec3 center;   // 注视的地表点（ECEF 米，海拔 0）
        Vec3 up;       // 屏幕上方向（单位，含 heading 旋转）
        Vec3 forward;  // 视线前方向（单位，= normalize(center−eye)）
        Matrix4 view;  // world→view
        Matrix4 proj;  // view→clip（透视）
        double nearDistance = 1.0;
        double farDistance = 1.0;
        /// 视锥 6 平面（Gribb 序 left/right/bottom/top/near/far），**double 精度**，正=内侧。
        /// 由 eye + 基向量 + fov 几何直构，不经过 float viewProj：大 alt 下 far 平面提取
        /// 需 row3−row2（两个 |E|~1.7e7 相减得 1.4e5），归一化又除以 (1+m10)≈0.018 放大 55×
        /// float32 噪声，导致 leaves 在 alt 13M↔14M 间非单调翻转（实测 14.25M OK / 13.14M 全剔）。
        std::array<FrustumPlane, 6> frustumPlanes{};
    };
    Camera3D camera3D() const;

    /// 3D 屏幕点 → 地理经纬度（射线∩椭球）：由眼点过该像素构造视线，与 [Wgs84Globe] 求交后反算。
    /// 视口未就绪或射线未命中椭球返回 false。供 3D 下 screenToGeo/拾取反算。
    bool screenToGeo3D(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const;

    /// 设置视口尺寸（像素），由 GLSurfaceView onSurfaceChanged 传入
    void setViewport(int widthPx, int heightPx);

    /// 设置相机：以 [Camera] 值对象整体写入（对应 参考实现 Camera）。除中心/高度外，
    /// 一并记录 heading/tilt/roll/altitudeMode（当前 2D 渲染暂未消费、仅建模留存），并用其 fieldOfView
    /// 参与 altitude↔zoom 换算。加锁，可任意线程调用。
    void setCamera(const Camera &camera);

    /// 手势平移：按屏幕像素位移拖动地图（dxPx 右为正、dyPx 下为正），地图跟随手指
    void panByPixels(double dxPx, double dyPx);

    /// 手势缩放：以屏幕焦点 (focusXpx, focusYpx) 为锚点按 factor 缩放（factor>1 放大/拉近，
    /// 相机高度降低）；焦点处的地理点在缩放前后保持不动（对应 参考实现 捏合缩放语义）
    void zoomBy(double factor, double focusXpx, double focusYpx);

    /// 手势旋转：相机 heading 累加 [deltaDeg] 度（顺时针自北，正=heading 增大、地图内容随之逆时针旋转），
    /// 累加后环绕归一到 [-180, 180)。仅 3D 透视通路消费（2D 正交恒正北，值留存不显效），
    /// 对应 参考实现 SwingPanZoomSupporter 的旋转分支
    void rotateHeading(double deltaDeg);

    /// 手势俯仰：相机 tilt 累加 [deltaDeg] 度（正=向地平线方向倾视，0=正下 nadir），
    /// 累加后钳制到 [0, MAX_TILT_DEG]。仅 3D 透视通路消费（2D 正交不消费 tilt），
    /// 语义为「绕目标保眼高」俯仰（见 camera3D 注释），屏心地面点不漂移。
    /// 注：保眼高模型把斜距拉长到 alt/cos t，高空大倾角时眼会落到地面以下（黑屏根因），
    /// 故 camera3D 每帧按当前高度钳制**有效倾角**使斜距 ≤ 当地半径；存储值不动，拉回即恢复。
    void rotateTilt(double deltaDeg);

    /// 设置屏幕密度（对齐参考实现的 setupViewport(w,h,displayMetrics.density) 口径），
    /// 作为 LOD 细分判据的 densityFactor（见 tileLevel）。默认 1.0。
    void setDisplayDensity(double density);

    /// 设置细节控制因子（对齐 参考实现 TiledSurfaceImage.detailControl，默认 1.0）：越大瓦片越粗、越省流量/内存。
    void setDetailControl(double detailControl);

    double centerLon() const { return centerLon_; }
    double centerLat() const { return centerLat_; }
    double altitude() const { return altitude_; }

    /// 由相机高度 + 视口高 + fov 换算的连续缩放级别（对齐 参考实现的地面分辨率）
    double zoom() const;

    /// 显示级别（整数）：与 app MercatorZoom.altitudeToLevel 逐字同源 level=clamp(floor(log2(周长/高度)),0,20)。
    /// 矢量级别可见性门控专用——zoom() 含视口高/fov 偏移项、比显示级别高约 log2(vh/212)≈3~4 级，
    /// 若用 zoom() 门控则用户在界面按显示级别设的下限永不达标（看似不生效）。瓦片 LOD 仍用 zoom()。
    int displayLevel() const;

    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// 屏幕密度（对齐 参考实现 displayMetrics.density）：供 Renderer 把屏幕固定尺寸标记的 dp 换算为 px
    double displayDensity() const { return densityFactor_; }

    /// 垂直视场角（度）：3D 通路构建透视投影/射线与 Tessellator 的 tanHalfFov 用（加锁快照）
    double fieldOfViewDeg() const;

    /// 3D 细分 LOD 阈值（detailControl·densityFactor，与 tileLevel 同源）：供 Tessellator
    /// 屏幕空间误差判据（越大瓦片越粗）。两因子仅初始化期写入，无锁读与 displayDensity 同口径。
    double lodDetailFactor() const { return detailControl_ * densityFactor_; }

    /// 相机中心的归一化世界坐标
    void worldCenter(double &wx, double &wy) const;

    /// 屏幕像素 → 归一化世界坐标（加锁快照，供拾取把点击点转世界坐标后做命中检测）
    void screenToWorld(double sxPx, double syPx, double &wx, double &wy) const;

    /// 读回当前相机完整姿态（对应 参考实现 Camera，含 heading/tilt/roll/fieldOfView/altitudeMode）：
    /// 加锁快照，供宿主以 [Camera] 值对象读写视角。中心取自经纬度/高度，姿态字段回读既有留存值。
    Camera camera() const;

    /// 可视范围的半宽/半高（归一化世界单位）
    double halfWorldWidth() const;
    double halfWorldHeight() const;

    /// 当前瓦片级别（LOD）：复刻 参考实现 Tile.mustSubdivide 的 2D 判据，叶级别 =
    /// clamp(ceil(zoom - log2(detailControl·densityFactor)), MIN_LEVEL, maxLevel)。
    /// [maxLevel] 为调用方（Renderer）传入的该图层图源最大级别；相机 zoom 超过它后不再选取更高级瓦片，
    /// 而是把末级瓦片纹理拉伸放大（复刻 参考实现 主界面「无限放大」），避免请求图源不存在的级别（404 → 深色占位块）。
    int tileLevel(int maxLevel) const;

    static constexpr int MIN_LEVEL = 0;
    /// 显示级别上限（与 app MercatorZoom.MAX_LEVEL 一致）：displayLevel 的钳制上限
    static constexpr int DISPLAY_MAX_LEVEL = 20;
    /// zoom 与相机高度的硬上限（投影放大极限，受 float 世界坐标精度约束）。
    /// 注：本 zoom 口径含视口高/FOV 项，与 app 显示级别 log2(周长/altitude) 相差约 log2(vh/212)≈3~4 级；
    /// 放大到底的实际限制由 clampAltitude 的固定相机高度下限（≈38.2m，显示级别恒钳 ≈20）承担，
    /// 与本值仅在日常视口下大致重合（更精细的精度钳制属后续优化项）。
    static constexpr int MAX_LEVEL = 24;
    /// 图源默认最大瓦片级别（多数在线底图源为 18，与 MapSource.DEFAULT_MAX_LEVEL 一致）
    static constexpr int DEFAULT_MAX_LEVEL = 18;
    /// 3D 俯仰角上限（度）：对齐参考实现 globe 控制器 applyLimits 口径的 maxTilt=80；
    /// 超过后近地平线视角退化（手势侧按 180·Δcy/视口高 逐帧累加，到底后同向拖动无效果、反向即回）
    static constexpr double MAX_TILT_DEG = 80.0;

private:
    // 以下 *Unlocked 版本不加锁，供已持锁的公开方法内部复用（避免非递归 mutex 重入死锁）
    double zoomUnlocked() const;
    double halfWorldWidthUnlocked() const;
    double halfWorldHeightUnlocked() const;
    void worldCenterUnlocked(double &wx, double &wy) const;
    /// 屏幕像素 → 归一化世界坐标
    void screenToWorldUnlocked(double sxPx, double syPx, double &wx, double &wy) const;
    /// 3D 相机快照/射线反算的免锁版（camera3D/screenToGeo3D 的实体，供 zoomBy 等持锁路径复用）
    Camera3D camera3DUnlocked() const;
    bool screenToGeo3DUnlocked(double sxPx, double syPx, double &outLonDeg, double &outLatDeg) const;
    /// 由归一化世界坐标设置中心经纬度
    void setCenterWorld(double wx, double wy);
    /// 将相机高度钳制到 zoom∈[MIN_LEVEL,MAX_LEVEL] 对应的高度区间（视口未知时跳过）
    void clampAltitude();

    // 视图模式：2D（默认，现有正交墨卡托通路零回归）/ 3D（透视球体通路，P1 新建）。
    ViewMode mode_ = ViewMode::MODE_2D;

    // 相机状态在 UI 线程（手势）写、GL 线程（绘制）读，用 mutex 防止撕裂读
    mutable std::mutex mtx_;

    // 默认相机：北京上空 20km（Phase B 固定视角验证网格数学；Phase D/E 由手势或 app 传入覆盖）
    double centerLon_ = 116.391;
    double centerLat_ = 39.907;
    double altitude_ = 20000.0; // 相机高度（米）
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;
    // 对齐 参考实现 Camera 的姿态建模值：fieldOfViewDeg_ 参与 2D altitude↔zoom 换算（默认 45 保持既有口径），
    // heading_/tilt_ 由 3D 透视通路消费（camera3D 的屏幕上方向/视线），roll_/altitudeMode 暂仅留存；
    // 2D 正交恒正北不消费 heading。均经 setCamera(const Camera&) 写入并随 camera() 回读。
    double heading_ = 0.0;
    double tilt_ = 0.0;
    double roll_ = 0.0;
    double fieldOfViewDeg_ = FIELD_OF_VIEW_DEG;
    AltitudeMode altitudeMode_ = AltitudeMode::ABSOLUTE;
    // LOD 细分判据参数（对齐 参考实现）：屏幕密度与细节控制，决定叶级别 = ceil(zoom - log2(detail·density))；
    // maxLevel 由各图层逐层传入 tileLevel(maxLevel)，不再存全局态。
    double densityFactor_ = 1.0;
    double detailControl_ = 1.0;
};

} // namespace globecore

#endif // GLOBECORE_NAVIGATOR_NAVIGATOR_H
