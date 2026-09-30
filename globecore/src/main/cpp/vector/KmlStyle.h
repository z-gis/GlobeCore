#ifndef GLOBECORE_VECTOR_KMLSTYLE_H
#define GLOBECORE_VECTOR_KMLSTYLE_H

#include <map>
#include <string>

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
 * 解析 KML/KMZ 文档，构建「Placemark name → 样式颜色」映射（Phase 3 逐要素配色）。
 *
 * 背景：GDAL/LIBKML 读 KML 不返回样式颜色、也不暴露 styleUrl 字段（真机探针实测字段仅
 * Name/description/…/altitudeMode/tessellate/extrude/visibility/drawOrder/icon），故在 Native 侧
 * 用无第三方依赖的定向文本扫描补齐（expat/libkml 头未随模块提供，无法直接调用）：
 *  1) 收集文档级 <Style id="X">…</Style> 内 PolyStyle/LineStyle 的 <color>（KML aabbggrr 十六进制 → RGBA）；
 *  2) 收集 <StyleMap id="Y"> 的 <key>normal</key> 所指向 styleUrl，解析时对 Y 做一次间接；
 *  3) 收集每个 <Placemark> 的 <name> 与其首个本地 <styleUrl>#id</styleUrl>，映射 name → 解析后样式颜色。
 *
 * 局限（可接受，与既有「description 按 name 补齐」同源）：按 name 关联 → 空名/重名 Placemark 匹配不到或取首个；
 * Placemark 内联 InlineStyle、外部（http）styleUrl、IconStyle/LabelStyle/BalloonStyle 颜色暂不处理。
 * 读文件走 GDAL VSI（.kml 直读、.kmz 经 /vsizip 取包内首个 .kml）。解析失败 / 非 KML / 打不开 → 返回空表，
 * 调用方据此整层沿用默认色（零副作用）。
 */
std::map<std::string, KmlStyleColors> parseKmlStyleMap(const std::string &path);

} // namespace globecore

#endif // GLOBECORE_VECTOR_KMLSTYLE_H
