#ifndef GLOBECORE_LAYER_VECTOR_LAYER_H
#define GLOBECORE_LAYER_VECTOR_LAYER_H

#include <GLES2/gl2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "geom/Vec3.h"
#include "layer/VectorLoader.h"
#include "render/Texture.h"
#include "vector/VectorGeometry.h"

namespace globecore {

/**
 * 一个矢量图层，对应主界面的一个用户矢量层（shp/kml/dwg/dxf 等；原 参考实现 界面经 LocalVectorLoader 加载，现由 native 直读）。
 *
 * 与 [TileLayer] 并列由 [GlobeEngine] 以 unique_ptr 持有（地址稳定，供 Renderer 长期引用）。
 * 绘制顺序：底图瓦片层 → 矢量层 → 注记 overlay 层 → 定位标记（对齐主界面「注记置顶于矢量之上」）。
 *
 * 数据流：构造即启动 [loader] 后台读文件 + 建几何；GL 线程首次 drainReady 后把几何移入 [geom]，
 * 再按每帧时间预算把顶点分片（chunk）流式上传为多个 VBO（[fillChunks]/[outlineChunks]/[lineChunks]），
 * 巨层不再单帧一次性 glBufferData 冻结秒级，屏幕上「一片一片」长出要素。[geom] 长期保留，
 * 供 GL 上下文重建时重新流式上传（onSurfaceCreated 把 chunk 列表清空、glReady 置 false，
 * 下一帧起据 [geom] 重传，延续崩溃修复口径）。
 */
struct VectorLayer {
    /// 矢量源文件路径（shp/kml/kmz/dwg/dxf 等，GDAL/OGR 可打开的格式）
    std::string sourcePath;
    /// 渲染样式（填充/描边/线/点颜色与线宽）
    VectorStyle style;
    /// 图层可见性（对齐 参考实现 Layer.isEnabled）：false 时 Renderer 整层跳过绘制。默认 true。
    bool visible = true;
    /// 级别可见性下限（对齐文档 LayerInfo.effectiveMinDisplayLevel，按界面显示级别取值）：
    /// 相机显示级别 < 该值时整层隐藏（不绘制/不上传/不可拾取/不计入加载提示），避免小级别下
    /// 大范围无意义的读取与零散显示。≤0 为不限（默认；内存叠加层恒为不限）。
    int minDisplayLevel = 0;

    /// 是否为动态叠加层（内存数据源，由宿主经 GlobeEngine::updateOverlay* 推送 WGS84 几何）；
    /// 区别于读文件的矢量层。叠加层与文件矢量层同列于 vectorLayers_，复用同一套绘制/拾取通路。
    bool isOverlay = false;
    /// 叠加层墓碑标志：removeOverlay 置 true（不 erase，以保持其它叠加层 index 稳定）并同时 visible=false。
    /// Renderer 在 GL 线程回收其 VBO/图标纹理/CPU 几何后跳过绘制；pickVector 因 visible=false 亦跳过。
    bool dead = false;
    /// 不参与拾取标志（仅叠加层使用）：选中高亮/查询高亮等瞬态视觉层置 true——仍正常绘制，
    /// 但 pickVector/pickVector3D 整层跳过，命中直接落到下方源层，避免其大面积几何遮蔽
    /// 测量/拍照等业务叠加与源要素的点击（宿主无需再做命中穿透分流）。
    bool noPick = false;

    /// 后台异步加载器（读文件 + 投影 + earcut 三角剖分）；持有独立线程，故用 unique_ptr
    std::unique_ptr<VectorLoader> loader;

    /// CPU 几何（加载器产出后长期保留，供 GL 上下文重建时重新上传）
    VectorGeometry geom;

    // ── GL 资源（仅 GL 线程访问）：每类顶点按上传预算切为多个 chunk VBO，绘制逐 chunk 循环──
    struct GlChunk {
        GLuint vbo = 0;
        GLsizei vertexCount = 0;
    };
    // 面填充（GL_TRIANGLES）/ 面描边、线要素（各为合批后单条 GL_TRIANGLE_STRIP，段间 degenerate 连接）
    std::vector<GlChunk> fillChunks;
    std::vector<GlChunk> outlineChunks;
    std::vector<GlChunk> lineChunks;

    // 流式上传进度（仅 GL 线程读写）：按「要素组」推进（geom.featSpans），uploadFeat = 下一个待上传
    // 要素下标；同一要素的 fill/outline/line 同组落块 → 上传途中图斑整块长出，无线框/填充混合态。
    size_t uploadFeat = 0;
    bool uploadDone = false; // featSpans 已消耗完（全部要素上传完成）
    // 本轮流式上传起始时刻（新几何接管时重置），仅用于完成时的诊断耗时统计
    std::chrono::steady_clock::time_point uploadStartedAt{};
    // 上传接管中（loader 已转 Consumed 而 chunk 未全部上传）：GL 线程写，UI 线程经
    // GlobeEngine::hasVectorLoading 读，使加载提示胶囊覆盖整个分片上传过程。
    std::atomic<bool> streamingUpload{false};

    bool glReady = false;    // 全部 chunk 已上传（= uploadDone）；上下文重建后置 false 触发据 geom 重传

    // ── 点要素图标（billboard）资源 ──
    // CPU RGBA 像素 + 尺寸：由 GlobeEngine::addVectorLayer 在构造后设定、之后只读（GL 线程懒上传，无竞争）。
    // 空表示无图标 → 点要素回退画屏幕固定圆（见 Renderer::drawVectorLayers 点段）。图标为中心锚点、
    // 按位图像素尺寸屏幕固定渲染（对齐原主界面 Placemark，图 imageOffset 默认居中）。
    std::vector<uint8_t> iconRgba;
    int iconW = 0;
    int iconH = 0;
    Texture iconTex;         // 图标 GL 纹理（仅 GL 线程）；上下文重建后 abandon，据 iconRgba 重传
    // 图标静态 VBO（仅 GL 线程，首帧懒建）：每点 6 顶点 [cx,cy,mx,my,u,v,r,g,b,a]，中心为相对图层原点
    // 坐标、(mx,my) 为角点属性∈[-1,1]，不随相机变化 → 零逐帧重建；几何重载/上下文重建时清零触发重建。
    GLuint iconVbo = 0;
    GLsizei iconVertexCount = 0;

    // ── 3D 球面矢量通路 chunk 资源（P2.e：分帧流式上传 + 视锥剔除，仅 GL 线程）──
    // 旧「每类一张整层 VBO + 描边逐 range draw」在县域 10 万图斑级巨层下 = 首传冻结秒级 +
    // 每帧 10 万次 draw call。现对齐 2D 流式口径按要素组分帧：每帧时间预算内转换一组要素
    // （2D RTC → ECEF − 固定锚点）上传一个 chunk VBO；描边/线 chunk 内各 range 条带以「上段末顶点 +
    // 本段首顶点」两重复顶点退化桥合批（同 2D buildStripBatch 配方；桥三角形恒含两个完全相同顶点
    // → 零面积，stroke 着色器的 cur 塌缩救不活它，旧配方「末顶点×2」的跨 range 撕裂不会复发），
    // 整 chunk 单次 glDrawArrays。每 chunk 缓存世界 ECEF AABB，逐帧与瓦片同口径的 Gribb 6 面保守剔除
    // （整盒在任一平面外侧才剔）。顶点布局同旧：fill 7f / stroke 14f / point 9f / icon 11f。
    struct GlobeChunk {
        GLuint vbo = 0;
        GLsizei vertexCount = 0;
        // 世界 ECEF 绝对坐标 AABB（米，转换期累加）：供视锥平面盒测试（alt=0 贴球烘焙）
        double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    };
    std::vector<GlobeChunk> globeFillChunks;    // GL_TRIANGLES
    std::vector<GlobeChunk> globeOutlineChunks; // GL_TRIANGLE_STRIP（组内退化桥合批）
    std::vector<GlobeChunk> globeLineChunks;    // GL_TRIANGLE_STRIP
    std::vector<GlobeChunk> globePointChunks;   // GL_TRIANGLES（无图标层屏幕固定圆）
    std::vector<GlobeChunk> globeIconChunks;    // GL_TRIANGLES（图标层 billboard，与点圆互斥共用游标）
    std::vector<GlobeChunk> globeExtrudeChunks; // GL_TRIANGLES（extrude 侧墙，独立开深度写入 pass）
    // 流式进度游标（仅 GL 线程）：globeUploadFeat = 下一待上传要素下标（geom.featSpans）；
    // globeUploadPoint = 下一待转换点要素下标（pointCoords 点对索引，点圆/图标两路互斥同游标推进）；
    // globeUploadExtrude = 下一待转换侧墙顶点的 geom.extrudeVerts float 下标（独立流式，不属 featSpans）。
    size_t globeUploadFeat = 0;
    size_t globeUploadPoint = 0;
    size_t globeUploadExtrude = 0;
    bool globeUploadDone = false; // 两游标均到尾部：几何已全部上 GPU（此前逐帧续传+续绘）
    // 3D 顶点相对「图层固定锚点」烘焙（不相对 eye）：globeAnchor = 几何原点 ECEF（只随几何变），顶点
    // 纯几何 alt=0。每帧把 uOffset = globeAnchor − eye、uVecAlt = 贴地高度传进着色器（高度沿表面法向抬升）
    // → 平移与缩放时顶点都不变、零重建零重传（3D 平移/缩放卡顿根治）。
    Vec3 globeAnchor{0.0, 0.0, 0.0};

    // ── 标注排版缓存（仅 GL 线程，Renderer::drawVectorLabels 维护）──
    // 每标注一条「烘焙像素单位」的字形排版结果（与缩放/相机无关），免每帧全量 UTF-8 解码 +
    // 字形查表（轮廓层每帧重复 9 趟排版是标注层卡顿主因）；text 不一致即重建，覆盖图层重载。
    struct GlyphBox {
        float adv = 0.0f;  // 水平推进（烘焙像素）
        // 位图盒相对 pen/基线（烘焙像素，x1<=x0 表示空白不画）与图集 UV
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };
    struct LabelLayout {
        std::string text;            // 构建时的标注文本（与 geom.labels[i].text 比对判失效）
        float totalAdv = 0.0f;       // 总推进（水平居中用）
        std::vector<GlyphBox> boxes; // 逐码点
    };
    std::vector<LabelLayout> labelLayouts;

    VectorLayer(std::string path, VectorStyle style_,
                bool hasExtent = false, double minLon = 0.0, double minLat = 0.0,
                double maxLon = 0.0, double maxLat = 0.0, int maxFeatures = 0)
        : sourcePath(std::move(path)), style(style_) {
        loader = std::make_unique<VectorLoader>(sourcePath, style, hasExtent, minLon, minLat,
                                                maxLon, maxLat, maxFeatures);
    }

    /// 动态叠加层构造（内存数据源）：建内存模式 loader（不读文件、不起后台线程），isOverlay=true。
    /// 几何由宿主经 GlobeEngine::updateOverlay* → loader->setData 推送；sourcePath 留空。
    explicit VectorLayer(VectorStyle style_) : style(style_), isOverlay(true) {
        loader = std::make_unique<VectorLoader>(style);
    }

    VectorLayer(const VectorLayer &) = delete;
    VectorLayer &operator=(const VectorLayer &) = delete;
};

/// 矢量层「有效可见」统一判据：文档可见 && 相机显示级别 [camLevel] 达级别下限。
/// 绘制/上传调度、拾取、加载提示、上下文重建接管全部走本口径，避免各门控点语义漂移。
/// camLevel 由调用方取 Navigator::displayLevel()（与 app 界面显示级别同源，
/// 勿用 floor(zoom())：其含视口高/fov 偏移、比显示级别高约 3~4 级，会使门控看似失效）。
inline bool vectorLayerShown(const VectorLayer &vl, int camLevel) {
    return vl.visible && (vl.minDisplayLevel <= 0 || camLevel >= vl.minDisplayLevel);
}

} // namespace globecore

#endif // GLOBECORE_LAYER_VECTOR_LAYER_H
