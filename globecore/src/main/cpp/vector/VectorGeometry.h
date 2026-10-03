#ifndef GLOBECORE_VECTOR_VECTORGEOMETRY_H
#define GLOBECORE_VECTOR_VECTORGEOMETRY_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace globecore {

/**
 * 单要素样式覆盖（文档层 VectorStyle.featureStyles 下传子集）：键 = 要素 FID，仅承载渲染相关通道。
 * hasFill/hasLine 表示该通道是否由用户显式设过（true 才覆盖），未设通道沿用整层/源文件色。
 * 颜色为 [0,1] RGBA；填充关闭以 alpha=0 表达（仍 hasFill=true）。线宽不支持逐要素（整层 uniform），不在此列。
 */
struct FeatureStyleOverride {
    bool hasFill = false;
    float fill[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool hasLine = false;
    float line[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // 线要素色，同时作面描边色
    // 标注覆盖（逐要素，优先级最高）：hasLabel=true 表示该 fid 显式设了标注（labelField 为该要素要标注的字段名，
    // 由 VectorReader 按 fid 从属性表取文本；空串=显式关闭该要素标注，即便整层设了字段）。
    // 标注样式各子通道独立门控（hasX=true 才覆盖整层对应通道）：文字色/字号/轮廓开关/轮廓色。
    bool hasLabel = false;
    std::string labelField;
    bool hasLabelColor = false;
    float labelColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasLabelSize = false;
    float labelSize = 1.0f;
    bool hasLabelOutline = false;
    bool labelOutlineVal = false;
    bool hasLabelOutlineColor = false;
    float labelOutlineColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * 矢量图层样式（对应 app doc/VectorStyle.kt 的渲染相关子集，颜色为 [0,1] 浮点 RGBA）。
 * 默认值对齐原主界面 LocalVectorLoader（已删除）：面填充 (0.29,0.56,0.89,0.4)+ 白描边、线 (0.9,0.42,0.2,1)。
 * 标注（label）门控与样式对齐主界面：labelField 为空则整层不标注，非空时逐要素取该属性字段值为文本，
 * 颜色/字号/轮廓随样式（默认白字、无轮廓）。图标（icon）不在地基范围内，延后。
 */
struct VectorStyle {
    // 面填充色
    float fillR = 0.29f, fillG = 0.56f, fillB = 0.89f, fillA = 0.4f;
    // 面描边色 + 线宽（像素，屏幕空间等宽三角带）
    float outlineR = 1.0f, outlineG = 1.0f, outlineB = 1.0f, outlineA = 1.0f;
    float outlineWidth = 1.0f;
    // 线要素色 + 线宽（像素）
    float lineR = 0.9f, lineG = 0.42f, lineB = 0.2f, lineA = 1.0f;
    float lineWidth = 2.0f;
    // 逐要素源文件配色（KML styleUrl）门控：整层是否【显式】设了填充色 / 线·描边色。
    //   true  = 用户在图层管理设过该通道 → 建几何时以整层色覆盖源文件逐要素原色；
    //   false（默认）= 未设 → 保留源文件逐要素原色（取舍见 VectorBuilder::buildOne）。
    // 未显式设时保留原色，正是「无名/靠 styleUrl 型 KML 按原文件渲色」所需；改样式即覆盖对应通道。
    bool hasFillColor = false;
    bool hasLineColor = false;
    // 单要素样式覆盖（fid → 覆盖）：优先级最高，建几何时逐要素据 fid 查表，命中通道覆盖整层/KML 原色。
    // 空图（默认）= 无单要素覆盖，行为与旧一致（零回归）。
    std::unordered_map<long long, FeatureStyleOverride> featureOverrides;
    // 点要素色 + 屏幕固定半径（dp，不随缩放变化，复用定位标记口径）
    float pointR = 0.9f, pointG = 0.42f, pointB = 0.2f, pointA = 1.0f;
    float pointRadiusDp = 5.0f;
    // 标注：[labelField] 为空则整层不标注；非空时逐要素取该属性字段值为标注文本
    std::string labelField;
    float labelR = 1.0f, labelG = 1.0f, labelB = 1.0f, labelA = 1.0f;  // 标注文字色（默认白）
    float labelSize = 1.0f;                                             // 字号缩放（对齐 VectorStyle.labelSize）
    bool labelOutline = false;                                          // 是否描边
    float labelOutlineR = 0.0f, labelOutlineG = 0.0f, labelOutlineB = 0.0f, labelOutlineA = 1.0f;  // 轮廓色（默认黑）
};

/// 拾取图元几何类型（对齐 VectorReader 的 VectorGeomType，独立定义避免头文件耦合）
enum class PickType {
    Point,
    Line,
    Polygon,
};

/**
 * 单要素拾取图元：要素 FID +「相对图层原点世界坐标」的几何（RTC float32，与渲染顶点同口径），
 * 供点击命中检测（点/线/面）在 native 侧把屏幕点转世界坐标后逐要素判定，命中即回传 FID。
 *
 * coords 为摊平的相对坐标 [x0,y0,x1,y1,...]；ranges 为子环区间 (起始点对索引, 点对数)：
 *  - Point：coords 仅 1 个点对，ranges 空；
 *  - Line：每段折线一个 range；
 *  - Polygon：ranges[0] 为外环、其余为洞环（命中判定为「在外环内且不在任一洞内」）。
 *
 * alt/mode 为逐点对高程并行数组（与 coords 点对一一对齐：coords[2j],coords[2j+1] ↔ alt[j],mode[j]）：
 * 仅含高程的图层填充（否则为空，与 VectorGeometry 各顶点高程数组同规则）。供 3D 拾取与渲染同口径：
 * mode=1（高程要素）按 alt 烘焙真实 ECEF、不叠加贴地抬升；mode=0（贴地）alt=0 + 抬 vecAlt。
 */
struct PickPrimitive {
    long long fid = -1;
    PickType type = PickType::Point;
    std::vector<float> coords;
    std::vector<std::pair<int, int>> ranges;
    std::vector<float> alt, mode; // 逐点对高程（米）/模式（0=贴地,1=baked），仅高程层非空
    // 该要素本层实际渲染色（[0,1] RGBA，buildOne 按三级优先级算定后烘焙，与 VBO 顶点色一致）：
    // renderFill=面填充色（点/线为该要素主色），renderLine=描边/线色。供宿主点击回显所见即所得色块
    // （尤其 KML 逐要素原色，app 无从复现），据此作单要素样式对话框初值。
    float renderFill[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float renderLine[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * 单条矢量要素标注：锚点为「相对图层原点世界坐标」（RTC float32，与渲染顶点同口径），text 为标注文本（UTF-8）。
 * 由 VectorLoader 构建（点要素取点位置、线/面取顶点均值质心，对齐原主界面 centroidOfFlat），
 * 渲染时按屏幕空间 billboard 文本绘制（字号恒定像素、不随缩放变）。
 * 样式（色/字号/轮廓）逐条携带：buildOne 按「整层默认 → 单要素覆盖」解析写入，渲染器直接读逐条值
 * （无覆盖时即整层值，零回归）。
 */
struct LabelItem {
    float x = 0.0f;  // 相对图层原点的世界坐标 X
    float y = 0.0f;  // 相对图层原点的世界坐标 Y
    std::string text;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};            // 文字色（默认白）
    float size = 1.0f;                                    // 字号缩放
    bool outline = false;                                 // 是否描边
    float outlineColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};     // 轮廓色（默认黑）
};

/**
 * 单要素（读取结果的派生要素，与 features 顺序一一对应）在三组渲染顶点中的占用区间：
 *  - fillStart/fillCount：fillVerts 中的 floats 区间（6 floats/顶点）；
 *  - outRangeStart/outRangeCount：outlineRanges 中的段区间（一个要素的外环 + 各洞环 → 多个 strip 段）；
 *  - lineRangeStart/lineRangeCount：lineRanges 中的段区间（多段线的每个 part → 一段）。
 * 三组顶点均按要素顺序连续追加，故相邻要素的区间首尾相接、无缝无叠。
 * 用途：分片流式上传按「要素组」对齐三类顶点（Renderer::uploadVectorChunks），
 * 同一要素的填充/描边/线同批落块 → 巨层上传途中图斑整块整块长出，不出现半描边半填充的混合态。
 */
struct FeatureSpans {
    int fillStart = 0, fillCount = 0;
    int outRangeStart = 0, outRangeCount = 0;
    int lineRangeStart = 0, lineRangeCount = 0;
};

/**
 * 一个矢量层的 CPU 几何（由 VectorLoader 在后台线程构建，GL 线程上传为 VBO）。
 *
 * 顶点坐标为「相对图层原点（[originWx],[originWy]，世界坐标）的 float32」，即 RTC（relative-to-center）
 * 口径：小量级、高精度。渲染时每帧在 double 下算 uOffset=(origin − camCenter) 转 float 传 uniform，
 * 顶点着色器做 aPos+uOffset，等价于瓦片的 (world − camCenter)，避免高缩放下 float32 灾难性抵消。
 *
 * 面填充顶点布局与占位/标记通路一致：[x,y,r,g,b,a] 共 6 floats（颜色在构建期烘焙进顶点）。
 * 面描边与线要素为「屏幕空间等宽三角带」：加载期把中心线烘焙为带 miter 接头法向的三角带，
 * 顶点布局 [x,y,mx,my,r,g,b,a] 共 8 floats（(mx,my) 为接头/端点法向含长度系数，实际像素半宽由
 * 着色器 uniform 每帧施加 → 恒定像素线宽），以 GL_TRIANGLE_STRIP 分段绘制；面描边与线要素分两组存储
 * （各自段区间 + 各自线宽），点仅存相对坐标（每帧画屏幕固定圆）。
 */
struct VectorGeometry {
    double originWx = 0.0;
    double originWy = 0.0;

    // 面填充三角形（GL_TRIANGLES），[x,y,r,g,b,a]
    std::vector<float> fillVerts;

    // 面描边（GL_TRIANGLE_STRIP 分段，屏幕空间等宽 miter 三角带），[x,y,mx,my,r,g,b,a] + 段区间 (first,count)
    std::vector<float> outlineVerts;
    std::vector<std::pair<int, int>> outlineRanges;

    // 线要素（GL_TRIANGLE_STRIP 分段，屏幕空间等宽 miter 三角带），[x,y,mx,my,r,g,b,a] + 段区间 (first,count)
    std::vector<float> lineVerts;
    std::vector<std::pair<int, int>> lineRanges;

    // 点要素相对坐标 [x,y]（每点 2 floats；渲染时按样式画屏幕固定圆）
    std::vector<float> pointCoords;

    // ===== 逐顶点高程并行数组（仅含高程的图层填充，其余层为空）=====
    // 与上面各顶点组一一对齐（按顶点计数，非 float 计数）：alt 为米，mode 为 0/1（1=用 baked 高程、
    // 不再叠加整层 uVecAlt；0=贴地、走 uVecAlt 抬升）。仅 3D 球面通路（uploadGlobeVecChunks）消费：
    // CPU 端据 alt 烘焙真实 ECEF，逐顶点 mode 传着色器门控 uVecAlt。2D 通路不读这些数组。
    // 判据：fillAlt 等非空 ⇔ 该层有高程；空则 3D 侧按 alt=0/mode=0 处理（等价旧行为）。
    std::vector<float> fillAlt, fillMode;       // 与 fillVerts 顶点对齐（fillVerts.size()/kVecFloatsPerVertex 项）
    std::vector<float> outlineAlt, outlineMode; // 与 outlineVerts 顶点对齐
    std::vector<float> lineAlt, lineMode;       // 与 lineVerts 顶点对齐
    std::vector<float> pointAlt, pointMode;     // 与 pointCoords 点对齐（pointCoords.size()/2 项）

    // ===== extrude 侧墙（Phase 2 立体拉伸）——仅含高程且 extrude 的要素生成 =====
    // 沿要素环逐段「地面(alt=0)→真实高程(环顶点 alt)」的竖直四边形（GL_TRIANGLES），布局同 fillVerts
    // [x,y,r,g,b,a] + 并行 extrudeAlt/extrudeMode（恒 mode=1 baked）。与 fillVerts 分开存：屋顶留在 fill
    // 通路（无深度、作顶盖），侧墙走独立开深度写入的 pass → 墙面彼此正确遮挡成实心盒体（修「凹型」）。
    // 非拉伸/非高程层完全为空 → 零开销。仅 3D 球面通路消费（2D 不读）。
    std::vector<float> extrudeVerts, extrudeAlt, extrudeMode;

    // 逐要素顶点区间（与读取结果的要素顺序一一对应，含零面积的退化空区间）：供流式上传按要素组分片
    std::vector<FeatureSpans> featSpans;

    // 逐要素拾取图元（FID + 相对原点几何）：供点击命中检测，与渲染顶点独立存储（渲染不需 FID 关联）
    std::vector<PickPrimitive> pickPrims;

    // 逐要素标注（锚点 + 文本）：仅当 VectorStyle.labelField 非空且要素该字段有值时存在
    std::vector<LabelItem> labels;

    bool empty() const {
        return fillVerts.empty() && outlineVerts.empty() && lineVerts.empty() && pointCoords.empty();
    }

    /// 本层是否带逐顶点高程（任一并行高程数组非空即视为有）。
    bool hasElevation() const {
        return !fillAlt.empty() || !outlineAlt.empty() || !lineAlt.empty() || !pointAlt.empty();
    }
};

} // namespace globecore

#endif // GLOBECORE_VECTOR_VECTORGEOMETRY_H
