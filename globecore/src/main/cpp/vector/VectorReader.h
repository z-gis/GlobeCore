#ifndef GLOBECORE_VECTOR_VECTORREADER_H
#define GLOBECORE_VECTOR_VECTORREADER_H

#include <string>
#include <vector>

namespace globecore {

/**
 * native 侧矢量读取器（阶段三）：globecore 自链 GDAL/OGR，直接在 C++ 里读矢量文件、
 * 解析源坐标系并重投影到 WGS84（EPSG:4326），提取顶点为 CPU 几何，不经 Kotlin 传数组。
 *
 * 承袭原 app 侧 jni_vector_reader.cpp（现为本模块 bridge/vector_io.cpp）的读取/重投影/摊平逻辑，但输出 native 结构而非 JSON：
 *   - 源 SRS 解析走 srs_resolve.h（图层自身 SRS → 同名 .prj → CAD 按 CGCS2000 3 度带带号推断）；
 *   - 逐要素 geom->transform(ct) 变换到 WGS84（传统 GIS 轴序 经度,纬度）；
 *   - 几何摊平口径对齐 app VectorFeature：POINT 单点、LINE 多段 parts（每段摊平）、
 *     POLYGON 外环 outer + 内环 holes（均摊平）；多点拆多个 POINT、多线合并为一个 LINE、
 *     多面拆多个 POLYGON、几何集合递归。
 *
 * 屏幕范围加载：[hasExtent]=true 时仅读取与 WGS84 屏幕矩形 [minLon,minLat,maxLon,maxLat] 相交的
 * 要素（图层四至预判 + SetSpatialFilterRect 走 .qix/R-tree 索引 + 逐要素 envelope 精确过滤 +
 * [maxFeatures] 兜底，复刻 bridge/vector_io.cpp 已验证的「四重防线」口径）；[hasExtent]=false 时整文件
 * 全量读取（小数据直接渲染场景）。世界坐标投影（lon/lat → 墨卡托世界系）与三角剖分在加载器/渲染器侧完成，
 * 本读取器只出 WGS84 几何。
 */

/// 几何类型（对齐 app 的 geom 字符串 "POINT"/"LINE"/"POLYGON"）
enum class VectorGeomType {
    Point,
    Line,
    Polygon,
};

/**
 * 高程语义模式（源自 KML 的 altitudeMode，探针实测 LIBKML 以字符串字段暴露）。
 * 仅对含 Z 的格式（KML/KMZ）有意义，其它格式一律 Clamp（贴地，忽略 Z）。
 *  - Clamp：贴地，忽略逐顶点 Z，渲染沿用整层 uVecAlt 抬升（防 z-fighting）；
 *  - Relative / Absolute：按逐顶点 Z 抬升。当前无地形（DEM），相对地面与绝对海拔等价，均取 Z 为离椭球高度。
 */
enum class VecAltMode {
    Clamp,
    Relative,
    Absolute,
};

/// 逐要素 KML 样式覆盖（Phase 3）：命中 KML <Style> 时以 KML 色覆盖对应通道（RGBA，[0,1]），未命中通道沿用整层默认。
/// hasFill 驱动面填充色；hasLine 同时驱动线要素色与面描边色（KML 多边形边框取 LineStyle 色）。
/// 线宽不支持逐要素（渲染期按整层 uniform 施加于 miter 带），故只带颜色。valid=false 表示无覆盖（零影响）。
struct VecFeatureStyle {
    bool valid = false;
    bool hasFill = false;
    float fill[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool hasLine = false;
    float line[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/// 单个矢量要素的摊平几何（坐标均为 WGS84 经度/纬度）
struct VectorFeatureData {
    VectorGeomType type = VectorGeomType::Point;
    long long fid = -1;

    // 高程模式（逐要素，取自 altitudeMode 字段）；非高程层恒为 Clamp。
    VecAltMode altMode = VecAltMode::Clamp;

    // 是否拉伸成立体体（取自 KML extrude 字段，1=true）；仅在 altMode 非 Clamp 时生效，
    // 令 VectorBuilder 沿环生成「地面→真实高程」侧墙（复用填充顶点通路）。
    bool extrude = false;

    // 逐要素 KML 配色覆盖（Phase 3）：由 VectorReader 按 Placemark name 关联 parseKmlStyleMap 结果填入；
    // 非 KML / 未命中恒 valid=false → VectorBuilder 沿用整层样式。
    VecFeatureStyle style;

    // POINT：单点坐标 + 高程（米，读自几何 Z；Clamp 或未含 Z 时为 0）
    double lon = 0.0;
    double lat = 0.0;
    double alt = 0.0;

    // LINE：多段折线，每段摊平为 [lon0,lat0,lon1,lat1,...]；partsAlt 与之逐顶点对齐（高程米，可空）
    std::vector<std::vector<double>> parts;
    std::vector<std::vector<double>> partsAlt;

    // POLYGON：外环摊平 [lon0,lat0,...]，outerAlt 逐顶点对齐的高程；内环（洞）列表同样各带对齐高程
    std::vector<double> outer;
    std::vector<double> outerAlt;
    std::vector<std::vector<double>> holes;
    std::vector<std::vector<double>> holesAlt;

    // 标注文本（labelField 非空且该要素字段有值时非空；已 trim），空串表示该要素不标注
    std::string label;
};

/// 读取结果：成功时 features 为全部要素、bbox 为 WGS84 四至（供图层原点 RTC 计算）；
/// 失败时 ok=false 且 error 为可展示的中文原因（如 CAD 无坐标系）。
struct VectorReadResult {
    bool ok = false;
    std::string error;
    std::vector<VectorFeatureData> features;

    bool hasBBox = false;
    double minLon = 0.0;
    double minLat = 0.0;
    double maxLon = 0.0;
    double maxLat = 0.0;

    // 本层是否含逐要素高程（任一要素 altMode 非 Clamp）。buildGeometry 据此决定是否烘焙并行高程数组，
    // 非高程层完全跳过 → 巨层零额外内存/耗时。
    bool hasElevation = false;
};

/**
 * 读取矢量文件（遍历全部子图层，dwg 等格式实体分布在多子图层）。
 * 内部确保 GDAL 已注册；打开失败 / CAD 无坐标系时返回 ok=false + error。
 * [labelField] 非空时逐要素读该属性字段值（trim 后）写入 VectorFeatureData.label；为空则不读标注。
 * [hasExtent] 为 true 时按屏幕四至 [minLon,minLat,maxLon,maxLat] 空间过滤（shp 无 .qix 时自动创建），
 * 否则整文件全量读取；[maxFeatures] 为单次加载要素预算（≤0 取硬上限 10000，命中超出则截断防 OOM）。
 * 本函数为纯 CPU 计算（含磁盘 IO 与投影变换），应在后台线程调用，勿在 GL 线程执行。
 */
VectorReadResult readVectorFile(const std::string &path, const std::string &labelField = std::string(),
                                bool hasExtent = false,
                                double minLon = 0.0, double minLat = 0.0,
                                double maxLon = 0.0, double maxLat = 0.0,
                                int maxFeatures = 0);

} // namespace globecore

#endif // GLOBECORE_VECTOR_VECTORREADER_H
