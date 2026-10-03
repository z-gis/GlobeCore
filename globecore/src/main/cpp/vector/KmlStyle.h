#ifndef GLOBECORE_VECTOR_KMLSTYLE_H
#define GLOBECORE_VECTOR_KMLSTYLE_H

#include <map>
#include <string>
#include <vector>

namespace globecore {

/**
 * 单个 KML <Style> 解析出的颜色（[0,1] RGBA）。仅取 PolyStyle（填充）与 LineStyle（线 / 面描边）的 <color>。
 * hasFill/hasLine 标记该样式是否定义了该通道；未定义的通道由调用方沿用整层默认。
 * 线宽不支持逐要素（渲染期按整层 uniform 施加于 miter 带），故此处不含宽度。
 */
struct KmlStyleColors {
    bool hasFill = false;
    float fill[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool hasLine = false;
    float line[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * 解析结果：在「Placemark name → 颜色」主路径之外，附带「几何首点坐标签名 → 颜色」兜底表，
 * 用于无名 / 靠 styleUrl 型导出（ArcGIS/MapInfo 落地的 kmz：每个 Placemark `<name>` 为空、颜色由
 * `<styleUrl>#id</styleUrl>` 驱动）的兜底关联。
 *
 * 为何用坐标签名而非 FID/序号：GDAL 对 <MultiGeometry> 的拆分（拆多要素 or 合并 MultiPolygon）、要素顺序、
 * 及屏幕空间过滤后的要素序号都不稳定，按 FID/序号对齐会在多几何 Placemark 处错位（实测个别图斑错色）。
 * 改用每个几何外环（线/点）前两个坐标点的量化签名 "%.7f_%.7f[_%.7f_%.7f]"（≈1cm）作键，与要素实际几何天然对应，对以上全部免疫；
 * 取前两点（而非仅首点）是为了避免相邻地块共享单个角点时的签名碰撞（实测宁德时代的深绿地块就因共首点被邻面占键）。
 */
struct KmlStyleIndex {
    std::map<std::string, KmlStyleColors> byName;   // 主路径：非空 name → 颜色
    // 兜底（仅当 byName 为空、即纯无名导出时启用）：几何首点坐标签名 → 颜色；同 Placemark 的每个子几何各登记一条。
    std::map<std::string, KmlStyleColors> byGeomSig;

    bool empty() const {
        return byName.empty() && byGeomSig.empty();
    }
};

/**
 * 解析 KML/KMZ 文档（Phase 3 逐要素配色）。
 *
 * 背景：GDAL/LIBKML 读 KML 不返回样式颜色、也不暴露 styleUrl 字段（真机探针实测字段仅
 * Name/description/…/altitudeMode/tessellate/extrude/visibility/drawOrder/icon），故在 Native 侧
 * 用无第三方依赖的定向文本扫描补齐（expat/libkml 头未随模块提供，无法直接调用）：
 *  1) 收集文档级 <Style id="X">…</Style> 内 PolyStyle/LineStyle 的 <color>（KML aabbggrr 十六进制 → RGBA）；
 *  2) 收集 <StyleMap id="Y"> 的 <key>normal</key> 所指向 styleUrl，解析时对 Y 做一次间接；
 *  3) 收集每个 <Placemark> 的 <name> 与其首个本地 <styleUrl>#id</styleUrl>，映射 name → 解析后样式颜色。
 *
 * 关联策略：① Placemark 有非空 <name> 且命中 → 走 byName 主路径（有名文件如 KML_Samples，行为同旧）；
 * ② 整份文档无任何有名要素（byName 为空，如 GIS 导出的空名 kmz）→ 按要素几何首点坐标签名查 byGeomSig 兜底；有名文件
 * 不启用兜底（避免无 styleUrl 的要素被错配颜色）。签名缺失或该位空色 → 回退整层默认，绝不产生错色。
 * 读文件走 GDAL VSI（.kml 直读、.kmz 经 /vsizip 取包内首个 .kml）。解析失败 / 非 KML / 打不开 → 返回空表，
 * 调用方据此整层沿用默认色（零副作用）。
 */
KmlStyleIndex parseKmlStyleMap(const std::string &path);

} // namespace globecore

#endif // GLOBECORE_VECTOR_KMLSTYLE_H
