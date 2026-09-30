#include "render/Renderer.h"

#include "geom/Matrix4.h"
#include "globe/MercatorProjection.h"
#include "globe/Tessellator.h"
#include "globe/TileMatrix.h"
#include "globe/Wgs84Globe.h"
#include "render/FontAtlas.h"
#include "render/ImageDecoder.h"
#include "util/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

namespace globecore {

namespace {

// ── 占位（纯色）着色器：世界坐标顶点 + 颜色，正交投影输出 ──────────────────────
// uOffset 为 RTC 偏移：矢量顶点存为「相对图层原点」的 float32，着色器做 aPos+uOffset 还原到
// 相机相对坐标（等价瓦片的 world−camCenter）；瓦片占位与定位标记的顶点已烘焙相机相对坐标，故传 (0,0)。
const char *const kColorVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos + uOffset, 0.0, 1.0);
    vColor = aColor;
}
)GLSL";

const char *const kColorFragmentSrc = R"GLSL(
precision mediump float;
varying vec4 vColor;
void main() {
    gl_FragColor = vColor;
}
)GLSL";

// ── 线（屏幕空间等宽三角带）顶点着色器：中心线顶点 + miter 法向，按像素半宽（换算世界单位）偏移 ──
// aMiter 为加载期烘焙的接头/端点法向（含 miter 长度系数），uHalfWidthWorld = 像素半宽 × 世界单位/像素，
// 每帧按缩放传入 → 屏幕空间恒定像素线宽；片元复用 kColorFragmentSrc（同为 varying vec4 vColor）。
const char *const kLineVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aMiter;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
uniform float uHalfWidthWorld;
varying vec4 vColor;
void main() {
    vec2 p = aPos + uOffset + aMiter * uHalfWidthWorld;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
    vColor = aColor;
}
)GLSL";

// ── 纹理着色器：单位四边形 + uv，逐瓦片模型矩阵定位，采样瓦片纹理 ─────────────────
const char *const kTexVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aTexCoord;
uniform mat4 uMvp;
uniform vec2 uUvOffset;
uniform vec2 uUvScale;
varying vec2 vTexCoord;
void main() {
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
    // 自身纹理时 uUvOffset=(0,0)、uUvScale=(1,1) 为恒等；祖先兜底时映射到祖先纹理的 UV 子矩形
    vTexCoord = uUvOffset + aTexCoord * uUvScale;
}
)GLSL";

const char *const kTexFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)GLSL";

// ── 文本（矢量标注）着色器：屏幕空间 billboard 字形四边形 ──────────────────
// 顶点位置已为相机相对世界坐标（每帧烘焙），无需 uOffset；片元以图集覆盖率（alpha 通道）× 文字色 alpha 输出，
// rgb 取顶点颜色（图集 rgb 恒为白，仅用其 alpha 作覆盖率蒙版）。
const char *const kTextVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

const char *const kTextFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
varying vec4 vColor;
uniform sampler2D uTexture;
void main() {
    float cov = texture2D(uTexture, vTexCoord).a;
    gl_FragColor = vec4(vColor.rgb, vColor.a * cov);
}
)GLSL";

// 图标顶点：矢量点要素图标 billboard 用——顶点存「中心坐标 + 角点属性 (mx,my)∈[-1,1]」，
// 着色器按 aPos + uOffset + aCorner*uHalf 展开四边形：uOffset 为图层 RTC 偏移、uHalf 为世界半尺寸
// （像素半尺寸 × 世界单位/像素，每帧传）→ 屏幕固定尺寸；顶点不随相机变化，可常驻静态 VBO，
// 免逐帧 CPU 重建 + glBufferData（原每帧逐层重建全部四边形顶点）。片元见 kIconFragmentSrc。
const char *const kIconVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aCorner;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
uniform vec2 uHalf;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    vec2 p = aPos + uOffset + aCorner * uHalf;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// 图标片元：矢量点要素图标 billboard 用——输出 图标纹理 RGBA × 顶点色（默认白，即纯图标）。
// 与文本片元（仅取图集 alpha 作覆盖率）不同，图标位图含完整彩色，须保留 rgb。顶点着色器见 kIconVertexSrc。
const char *const kIconFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
varying vec4 vColor;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord) * vColor;
}
)GLSL";

// ── 3D 球面瓦片纹理着色器：pos3 + uv（交错 9 floats，见 kGlobeFloatsPerVertex），透视 MVP ──
// uMvp 传 viewProjRtc（顶点已在 CPU 端 double 减眼点，RTC 口径同 2D 相机相对渲染，否则 ECEF ~6.4e6 m
// 量级下 float32 仅 0.5~1 m 误差，近地视角顶点拖动/自阴影条纹）；uUvOffset/uUvScale 同 2D 做祖先子矩形。
// 片元复用 kTexFragmentSrc。
const char *const kGlobeTexVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aTexCoord;
uniform mat4 uMvp;
uniform vec2 uUvOffset;
uniform vec2 uUvScale;
varying vec2 vTexCoord;
void main() {
    gl_Position = uMvp * vec4(aPos, 1.0);
    vTexCoord = uUvOffset + aTexCoord * uUvScale;
}
)GLSL";

// 3D 占位着色器：同交错缓冲取 pos3 + color4，画无图无祖先瓦片的棋盘深色。片元复用 kColorFragmentSrc。
const char *const kGlobeColorVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 1.0);
    vColor = aColor;
}
)GLSL";

// ── P2.a 3D 矢量面填充顶点着色器 ──────────────────────────────────────
// 顶点在 CPU 端 worldXYToCartesian(逐顶点高程) − 图层固定锚点 anchor（纯几何，不随相机/高度变）；
// uOffset=anchor−eye 补 eye 平移、uAnchor=anchor 供恢复 ecef 求法向。
// 高度：aMode=1（高程要素）→ 已 baked 在 aPos 的真实高度，不再叠加 uVecAlt；aMode=0（贴地）→ 沿用 uVecAlt
// 沿法向抬升（防与瓦片 z-fighting、随缩放零重建）。即 抬升 = uVecAlt*(1-aMode)。uMvp 传 viewProjRtc。
const char *const kGlobeFillVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);   // 表面法向（球近似：ecef 方向 = aPos+anchor）
    gl_Position = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    vColor = aColor;
}
)GLSL";

// ── P2.a 3D 矢量描边/线顶点着色器（参考实现 3-顶点模板屏幕空间 miter 移植）─────────────
// 参考 参考实现Kotlin/render/program/TriangleShaderProgram.kt，与本项目差异：
//   1) corner 从独立 attribute aSide 传入（±±），不再利 参考实现 将 corner 编到 pointB.w（需 cornerX 回退）；
//   2) 去掉 bevel 回退（仅靠 uMiterLimit 钳制）；无 enableTexture/lighting/shadows 分支；
//   3) 顶点位置已 RTC 减眼，无 modelMatrix。
// 流程同 参考实现：三顶点过 MVP → NDC → 视口像素空差归一→ AB/BC→tangent→miter→miterLength（钳住）；
// 端点 prev==cur 或 next==cur 时 AB/BC 退化为 0，归一到同侧使 miter = 段法向；
// gl_Position.xy = pCur.w * (pCur.xy + side * miter * uHalfWidthPx * 2.0/uViewport) → 恒定像素半宽。
const char *const kGlobeStrokeVertexSrc = R"GLSL(
attribute vec3 aPrev;
attribute vec3 aCur;
attribute vec3 aNext;
attribute float aSide;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform float uHalfWidthPx;
uniform vec2 uViewport;
uniform float uMiterLimit;
varying vec4 vColor;
void main() {
    float lift = uVecAlt * (1.0 - aMode);   // 贴地走 uVecAlt，高程要素（已 baked 高度）不叠加
    vec4 pPS = uMvp * vec4(aPrev + uOffset + normalize(aPrev + uAnchor) * lift, 1.0);
    vec4 pCS = uMvp * vec4(aCur  + uOffset + normalize(aCur  + uAnchor) * lift, 1.0);
    vec4 pNS = uMvp * vec4(aNext + uOffset + normalize(aNext + uAnchor) * lift, 1.0);
    float eps = 1e-4;
    // 当前顶点 cur 在相机背后/近平面上（w<=eps）时，屏幕空间 miter 无意义：若只把 w 强设 eps，
    // clip.xy 不变 → pC = clip.xy/eps 爆到天际 → 与相邻正常顶点构成的 strip 三角斜穿屏幕（重叠区/
    // 地平线附近撕裂的真因）。正确做法：cur 塌缩到任一在前的邻点使该段零宽退化（前后邻都在后则整顶点
    // 丢到裁剪外）。cur 塌缩后 w 变为邻点的有效 w，后续 pC 除法安全。
    if (pCS.w < eps) {
        if (pPS.w >= eps)      pCS = pPS;
        else if (pNS.w >= eps) pCS = pNS;
        else { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); vColor = aColor; return; }
    }
    // prev/next 在 cur 之后仍可能在背后：退化到 cur（已保证 w>=eps）→ AB/BC 零长，下方归一到同侧法向。
    if (pPS.w < eps) pPS = pCS;
    if (pNS.w < eps) pNS = pCS;
    vec2 pP = pPS.xy / pPS.w;
    vec2 pC = pCS.xy / pCS.w;
    vec2 pN = pNS.xy / pNS.w;
    // 屏幕像素空间的段方向（NDC 差 * viewport/2，非方形视口下 miter 方向才欧氏正确）
    vec2 ABraw = (pC - pP) * uViewport * 0.5;
    vec2 BCraw = (pN - pC) * uViewport * 0.5;
    // 端点/重合点退化：把零长向量归一到同侧（tangent=normalize(AB+BC) 仍为段方向，miter = 段法向）
    if (dot(ABraw, ABraw) < 1e-8) ABraw = BCraw;
    if (dot(BCraw, BCraw) < 1e-8) BCraw = ABraw;
    if (dot(ABraw, ABraw) < 1e-8) ABraw = vec2(1.0, 0.0); // 三点全重合兵至 anchor→ 退化方向
    vec2 AB = normalize(ABraw);
    vec2 BC = normalize(BCraw);
    vec2 tangent = normalize(AB + BC);
    vec2 miter = vec2(-tangent.y, tangent.x);
    vec2 normalA = vec2(-AB.y, AB.x);
    float d = dot(miter, normalA);
    float cutoff = 1.0 / max(uMiterLimit, 1.0);
    float miterLength = 1.0 / max(d, cutoff);
    // 偏移在像素空间 = side * miter * halfWidthPx * miterLength；回 NDC 需乘 2/viewport；再乘 w 回 clip
    vec2 offsetNdc = aSide * miter * uHalfWidthPx * miterLength * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vColor = aColor;
}
)GLSL";

// ── P2.b 3D 点要素 billboard 顶点着色器（屏幕固定圆，投影驱动单管线）───────────────
// 与 stroke 同源：同一 viewProjRtc 把 RTC 中心投影到 clip，再在屏幕像素空间按 corner 展开。
//   corner=0 的顶点落在圆心，corner=单位方向∈[-1,1] 的顶点沿该方向外扩 uHalfPx 像素 → 圆环。
// 偏移量口径：像素 → NDC 乘 (2.0/uViewport)，再乘 pCS.w 回 clip（同 stroke 恒定像素宽推导）。
// 深度 gl_Position.z 保留中心投影 pCS.z → 开 GL_DEPTH_TEST 后，背面半球的点被地球瓦片深度剔除。
// 中心点在相机背后（w<=eps）无意义：整顶点丢到裁剪外（点无相邻点可塌缩，直接裁掉）。
const char *const kGlobePointVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aCorner;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform vec2 uHalfPx;
uniform vec2 uViewport;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);
    vec4 pCS = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    float eps = 1e-4;
    if (pCS.w < eps) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        vColor = aColor;
        return;
    }
    vec2 pC = pCS.xy / pCS.w;
    vec2 offsetNdc = aCorner * uHalfPx * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vColor = aColor;
}
)GLSL";

// ── P2.c 3D 点要素图标 billboard 顶点着色器（屏幕固定尺寸纹理四边形，投影驱动单管线）───
// 与 globePoint 同源：同一 viewProjRtc 把 RTC 中心投影到 clip，再在屏幕像素空间按 corner 展开；
// 仅多一条 uv 采样图标纹理。uHalfPx 为图标像素半宽/半高（各向异性），每帧逐层传 → 屏幕恒定尺寸。
// 中心点在相机背后（w<=eps）无意义：整顶点丢到裁剪外。深度 zw 保留 → 开 GL_DEPTH_TEST 后背半球图标被地球剔除。
const char *const kGlobeIconVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aCorner;
attribute vec2 aTexCoord;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform vec2 uHalfPx;
uniform vec2 uViewport;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);
    vec4 pCS = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    float eps = 1e-4;
    if (pCS.w < eps) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        vTexCoord = aTexCoord;
        vColor = aColor;
        return;
    }
    vec2 pC = pCS.xy / pCS.w;
    vec2 offsetNdc = aCorner * uHalfPx * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// ── 3D 贴地定位标识纹理四边形顶点着色器（透视、无屏幕展开，平行于地球表面）───
// 与 globeIcon（屏幕展开 billboard，恒正对屏幕→ tilt 时变「立面」）不同：本通路顶点已在 CPU 端
// 沿局部切平面基 east/north 铺成贴地矩形并 double 减眼点转 float，直接走 viewProjRtc 透视投影 →
// 图标平铺于地表、指真北（RELATIVE_TO_GLOBE），随相机俯仰自然透视压扁。uMvp 传 viewProjRtc。
const char *const kGlobeFlatIconVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// 占位顶点：vec2 pos + vec4 color = 6 floats
constexpr int kColorFloatsPerVertex = 6;
// 线顶点：vec2 pos + vec2 miter + vec4 color = 8 floats
constexpr int kLineFloatsPerVertex = 8;
// 纹理单位四边形顶点：vec2 pos + vec2 uv = 4 floats
constexpr int kTexFloatsPerVertex = 4;
// 文本字形顶点：vec2 pos + vec2 uv + vec4 color = 8 floats
constexpr int kTextFloatsPerVertex = 8;
// 图标顶点：vec2 center + vec2 corner + vec2 uv + vec4 color = 10 floats
constexpr int kIconFloatsPerVertex = 10;
// 3D 球面瓦片顶点：vec3 pos + vec2 uv + vec4 color = 9 floats（纹理/占位两通路共用交错缓冲，
// 各自取自己的属性段，免为占位色另建一份网格）
constexpr int kGlobeFloatsPerVertex = 9;
// P2.a 3D 面填充顶点：vec3 pos + vec4 color + float mode = 8 floats
constexpr int kGlobeFillFloatsPerVertex = 8;
// P2.a 3D 描边/线顶点：vec3 prev + vec3 cur + vec3 next + float side + vec4 color + float mode = 15 floats
constexpr int kGlobeStrokeFloatsPerVertex = 15;
// P2.b 3D 点 billboard 顶点：vec3 pos + vec2 corner + vec4 color + float mode = 10 floats
constexpr int kGlobePointFloatsPerVertex = 10;
// P2.c 3D 图标 billboard 顶点：vec3 pos + vec2 corner + vec2 uv + vec4 color + float mode = 12 floats
constexpr int kGlobeIconFloatsPerVertex = 12;
// 3D 贴地定位标识图标顶点：vec3 pos + vec2 uv + vec4 color = 9 floats（pos 已 RTC 减眼点）
constexpr int kGlobeFlatIconFloatsPerVertex = 9;
// 3D 纯色顶点（贴地箭头/引线，复用 globeColorProgram_）：vec3 pos + vec4 color = 7 floats
constexpr int kGlobeColorFloatsPerVertex = 7;
// P2.a 3D 矢量贴球高度上夹（米）：face 与 line/outline 同值。**fill 开 glPolygonOffset(+2,+2) 把面深度 push 更远**，
// 保证同高度的 stroke 后画时能通过 GL_LEQUAL（否则 interior 描边会被自己那层 fill 挡回。—见 Pass 2 注释）；
// fill 与瓦片（alt=0）的深度差恒大于 2 units 偏移量级，不会反过来被瓦片吃掉。
// 抬升不可为 0：g2c 只变换顶点，线段/三角形在球上是平弦，中段陷入球面以下的「矢高」≈ L²/(8R)
//（L=顶点间大圆距）。动态高度按 camAlt²/2e7 取（≈ 视角内最长弦 L≈1.4·camAlt 的矢高需求），
// 钳 [2, 上夹]：低空（tilt/贴地视角的惯用工作高度）仅米级，视觉即贴地——旧线性 camAlt·0.1
// 在 camAlt≥5km 即触顶 500m，tilt 后视差使贴地层明显「悬浮」，故改回贴地口径；
// 深视角（camAlt≥100km）仍须百米级抬升容纳百公里跨径弦切。
// 边跨径远超视野的超大要素（如低空视角下的国界）弦中点矢高会超出动态高度而被深度剔除，
// 需配套 CPU 边细分（P2.e）；当前林业调查数据集无此类要素，真机 tilt 验证无缺块回归。
constexpr double kGlobeVecAltMeters = 500.0;
// P2.a 3D 描边/线 miter 长度上限制（与 2D VectorBuilder 中 kMiterLimit 同口径，尖角鉗制）
constexpr float kGlobeMiterLimit = 4.0f;
// 球面瓦片网格每边分段数（G×G 四边格 → 每瓦片 2·G·(G+1) 顶点）：G=8 时根瓦片（90°跨径）
// 棱弦矢高约 6km（球半径的 0.1%，球视轮廓不可见），近地视角瓦片跨径极小更无感；
// 逐帧 CPU 开销≈每瓦片 144 次三角函数，200 瓦片内毫秒级（后续可按 (z,x,y) 缓存网格优化）
constexpr int kGlobeGridDiv = 8;

// 单位四边形 [0,1]x[0,1]（pos 与 uv 相同：uv.v=0 对应世界北边=图像顶行），2 个三角形
const float kUnitQuad[] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 1.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
};

inline void pushColorVertex(std::vector<float> &v, float x, float y, float r, float g, float b) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(1.0f);
}

// 带 alpha 的占位顶点（矢量点要素屏幕固定圆用，颜色含透明度）
inline void pushColorVertexA(std::vector<float> &v, float x, float y, float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// 文本字形顶点 [x,y,u,v,r,g,b,a]（pos 为相机相对世界坐标，uv 为图集子矩形）
inline void pushTextVert(std::vector<float> &v, float x, float y, float u, float t,
                         float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(u);
    v.push_back(t);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// 共享落位：把已排版 lay 的字形四边形发射到 verts。坐标系为任一「y 向下」的 2D 空间（2D=世界单位、
// 3D=屏幕像素）：(ax,ay) 为锚点（含 RTC/轮廓偏移，已叠加），s 为该空间「单位/烘焙像素」比例。
// 水平居中于锚点，基线 = ay + vCenterPx*s 使视觉中心对齐锚点（y 向下→基线在锚点下方，同 2D 口径）。
inline void emitLabelQuads(const VectorLayer::LabelLayout &lay, float ax, float ay, float s,
                           float vCenterPx, float cr, float cg, float cb, float ca,
                           std::vector<float> &verts) {
    float penX = ax - lay.totalAdv * s * 0.5f;
    const float baseline = ay + vCenterPx * s;
    for (const auto &gb : lay.boxes) {
        if (gb.x1 > gb.x0) {
            const float x0 = penX + gb.x0 * s, x1 = penX + gb.x1 * s;
            const float y0 = baseline + gb.y0 * s, y1 = baseline + gb.y1 * s;
            pushTextVert(verts, x0, y0, gb.u0, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y0, gb.u1, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y1, gb.u1, gb.v1, cr, cg, cb, ca);
            pushTextVert(verts, x0, y0, gb.u0, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y1, gb.u1, gb.v1, cr, cg, cb, ca);
            pushTextVert(verts, x0, y1, gb.u0, gb.v1, cr, cg, cb, ca);
        }
        penX += gb.adv * s;
    }
}

// 图标顶点 [cx,cy,mx,my,u,v,r,g,b,a]：中心 + 角点属性∈[-1,1] + uv + 色（着色器按角点×uHalf 展开）
inline void pushIconVert(std::vector<float> &v, float cx, float cy, float mx, float my,
                         float u, float t, float r, float g, float b, float a) {
    v.push_back(cx);
    v.push_back(cy);
    v.push_back(mx);
    v.push_back(my);
    v.push_back(u);
    v.push_back(t);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// P2.a 3D 面填充顶点 [x,y,z,r,g,b,a,mode]（位置已在 CPU 端 worldXYToCartesian(逐顶点高程) 后减 anchor、
// double 差后转 float；mode 0/1 门控着色器 uVecAlt 抬升）
inline void pushGlobeFillVert(std::vector<float> &v, float x, float y, float z,
                              float r, float g, float b, float a, float mode) {
    v.push_back(x); v.push_back(y); v.push_back(z);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.a 3D 描边/线顶点 [pPrev3, pCur3, pNext3, side, color4, mode]（同 参考实现 3-顶点模板， RTC 已减 eye）
inline void pushGlobeStrokeVert(std::vector<float> &v,
                                const Vec3 &prev, const Vec3 &cur, const Vec3 &next, float side,
                                float r, float g, float b, float a, float mode) {
    v.push_back(prev.x); v.push_back(prev.y); v.push_back(prev.z);
    v.push_back(cur.x);  v.push_back(cur.y);  v.push_back(cur.z);
    v.push_back(next.x); v.push_back(next.y); v.push_back(next.z);
    v.push_back(side);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.b 3D 点 billboard 顶点 [pos3, corner2, color4, mode]（pos 已 RTC 减 eye；corner 为单位圆方向或 (0,0)）
inline void pushGlobePointVert(std::vector<float> &v, const Vec3 &pos, float cx, float cy,
                               float r, float g, float b, float a, float mode) {
    v.push_back(pos.x); v.push_back(pos.y); v.push_back(pos.z);
    v.push_back(cx); v.push_back(cy);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.c 3D 图标 billboard 顶点 [pos3, corner2, uv2, color4, mode]（pos 已 RTC 减 eye；corner 为四边形角点∈[-1,1]）
inline void pushGlobeIconVert(std::vector<float> &v, const Vec3 &pos, float cx, float cy,
                              float u, float t, float r, float g, float b, float a, float mode) {
    v.push_back(pos.x); v.push_back(pos.y); v.push_back(pos.z);
    v.push_back(cx); v.push_back(cy);
    v.push_back(u); v.push_back(t);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// 3D 贴地定位标识图标顶点 [pos3, uv2, color4]（pos 传入已 RTC 减 eye 的坐标，内部转 float 上传）
inline void pushGlobeFlatIconVert(std::vector<float> &v, const Vec3 &pos, float u, float t,
                                  float r, float g, float b, float a) {
    v.push_back(static_cast<float>(pos.x)); v.push_back(static_cast<float>(pos.y)); v.push_back(static_cast<float>(pos.z));
    v.push_back(u); v.push_back(t);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
}

// 3D 纯色顶点 [pos3, color4]（复用 globeColorProgram_；pos 传入已 RTC 减 eye 的坐标，贴地箭头/引线用）
inline void pushGlobeColorVert(std::vector<float> &v, const Vec3 &pos,
                               float r, float g, float b, float a) {
    v.push_back(static_cast<float>(pos.x)); v.push_back(static_cast<float>(pos.y)); v.push_back(static_cast<float>(pos.z));
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
}

// P2.a 归一化世界坐标 (wx, wy) → 地理 (lon°, lat°)。与 Tessellator::rowToLat 同公式的逆口径：
// lon = wx*360 − 180（wx=0 西边缘 −180°、wx=1 东边缘 +180°）；
// lat = atan(sinh(π(1−2·wy))) 度，wy=0 顶=北 +85.05、wy=1 底=南 −85.05。钳到 ±85.05°。
inline void worldXYToGeographic(double wx, double wy, double &outLonDeg, double &outLatDeg) {
    outLonDeg = wx * 360.0 - 180.0;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRad2Deg = 180.0 / kPi;
    constexpr double kMaxLat = 85.0511287798066; // = atan(sinh(π)) · rad2deg，与 rowToLat 同口径
    double lat = std::atan(std::sinh(kPi * (1.0 - 2.0 * wy))) * kRad2Deg;
    if (lat > kMaxLat) lat = kMaxLat;
    if (lat < -kMaxLat) lat = -kMaxLat;
    outLatDeg = lat;
}

// P2.a 归一化世界坐标 → ECEF（米）：worldXYToGeographic 后交 Wgs84Globe。alt 传入面/线贴球高度。
inline Vec3 worldXYToCartesian(double wx, double wy, double altMeters) {
    double lon = 0.0, lat = 0.0;
    worldXYToGeographic(wx, wy, lon, lat);
    Vec3 out{0.0, 0.0, 0.0};
    Wgs84Globe::instance().geographicToCartesian(lon, lat, altMeters, out);
    return out;
}

// 定位标记尺寸：屏幕固定 dp（乘屏幕密度得 px），三角扇圆分段数
constexpr int kMarkerSegments = 32;
constexpr double kMarkerDotRadiusDp = 6.0;  // 蓝心半径（dp）
constexpr double kMarkerRingRadiusDp = 9.0; // 白边半径（dp）
// 罗盘图标显示缩放：原主界面 COMPASS_IMAGE_SCALE=0.5f × density，原图 150px；
// 此处按图标原始像素 × 该比例显示，效果对齐主界面（约 75px/density=1）
constexpr double kMarkerIconScale = 0.5;
// 3D 贴地标示的引线屏幕长度（dp，乘 density 得 px）：图标沿地表法向抬高该屏幕长度悬于地面点上方，
// 斜视时显出一条连回地面点的引线（俯视时沿视线坍缩为点、不可见）。对齐 参考实现 leader-line Placemark。
constexpr double kMarkerLeaderLenDp = 100.0;
// 移动方向箭头颜色（淡蓝，对齐原主界面 LocationModel.ARROW_COLOR = 0xFF57ACFD）
constexpr float kArrowR = 0x57 / 255.0f;
constexpr float kArrowG = 0xAC / 255.0f;
constexpr float kArrowB = 0xFD / 255.0f;
// 移动方向箭头几何（dp，乘 density 得 px）——直接移植 0.2.8 主界面 LocationModel.createHeadingArrowIcon：
// 128px 画布 + imageScale 0.5×density（位图坐标 ×0.5 即 dp 尺寸），Path 为凹口导航箭头：
// (center, center-0.16S) 箭尖 → (center±0.14S, center+0.16S) 底角 → (center, center+0.08S) 凹口。
// 箭头以定位点为中心固定屏幕尺寸绘制，不随罗盘图标放大（旧版按图标半宽 1.35/2.2 倍拉伸过大）。
constexpr double kArrowTipDp = 10.24;   // 箭尖到定位点距离（前）= 128×0.16×0.5
constexpr double kArrowBackDp = 10.24;  // 底角到定位点距离（后）
constexpr double kArrowHalfDp = 8.96;   // 底角横向半宽 = 128×0.14×0.5
constexpr double kArrowNotchDp = 5.12;  // 凹口到定位点距离（后）= 128×0.08×0.5

/**
 * 把多条 GL_TRIANGLE_STRIP 段（ranges 为 (first,count) 顶点区间）的 [fromRange,toRange) 切片合批为
 * 单一顶点序列：相邻两段之间插入「上段末顶点 + 本段首顶点」两个重复顶点构成 degenerate 桥，
 * 消除跨段产生的错误填充三角形（段首尾不相连时 GPU 会桥接出跨越三角形，插入退化顶点后
 * 该桥接三角形面积退化为 0 不可见）。未启用面剔除，各段绕序翻转无视觉影响，故无需处理顶点奇偶对齐。
 * 顶点布局为 kLineFloatsPerVertex floats/顶点（[x,y,mx,my,r,g,b,a]）。返回合并后顶点数组。
 * 切片间各自独立成带（首段不加桥、落在不同 chunk VBO），流式上传逐片合批时跨片无粘连。
 */
std::vector<float> buildStripBatch(const std::vector<float> &verts,
                                    const std::vector<std::pair<int, int>> &ranges,
                                    size_t fromRange, size_t toRange) {
    constexpr int fpv = kLineFloatsPerVertex;
    const int total = static_cast<int>(verts.size() / fpv);
    toRange = std::min(toRange, ranges.size());
    // 预留：切片顶点 + 每段接缝至多 2 个重复顶点（fpv floats/顶点）
    std::vector<float> out;
    size_t est = 0;
    for (size_t ri = fromRange; ri < toRange; ++ri)
        if (ranges[ri].second > 0) est += static_cast<size_t>(ranges[ri].second) + 2;
    out.reserve(est * fpv);
    bool first = true;
    for (size_t ri = fromRange; ri < toRange; ++ri) {
        const auto &rg = ranges[ri];
        const int f = rg.first;
        const int c = rg.second;
        if (c <= 0 || f < 0 || f + c > total) continue; // 越界保护
        if (!first) {
            // degenerate 桥：复制 out 中末顶点（上段末）+ 本段首顶点
            const int prevLast = static_cast<int>(out.size() / fpv - 1) * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(out[prevLast + k]);
            const int curFirst = f * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(verts[curFirst + k]);
        }
        for (int i = 0; i < c; ++i) {
            const int base = (f + i) * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(verts[base + k]);
        }
        first = false;
    }
    return out;
}

// 流式上传单组要素的总浮点数预算（三组顶点合计 ≈ 4MB）：按真机 glBufferData 吞吐（≈550MB/4.7s）
// 单组耗时≈十毫秒级，以「每组必成 + 超时即停」的节奏把巨层秒级单帧冻结摊薄到逐帧小卡顿，
// 画面按「整块图斑」一片一片长出（同组 fill/outline/line 同帧落块，无混合态）。
constexpr size_t kUploadChunkFloats = 1u << 20;
// 单层单帧流式上传时间预算：当前进行片必定完成（故每帧至少推进一片），完成后超预算则留到下一帧。
constexpr int kUploadFrameBudgetMs = 6;

/// 以 [data, data+floats) 新建单个 chunk VBO（GL_STATIC_DRAW）；fpv = 每顶点浮点数。仅 GL 线程调用。
VectorLayer::GlChunk uploadVectorChunk(const float *data, size_t floats, int fpv) {
    VectorLayer::GlChunk c;
    if (floats == 0) return c;
    glGenBuffers(1, &c.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)), data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    c.vertexCount = static_cast<GLsizei>(floats / fpv);
    return c;
}

// ── P2.e 3D 矢量分帧流式上传 + 视锥剔除参数 ──
// 3D 要素组预算按「源(2D RTC) floats」计：转换后 fill 7/6、stroke 14/8 倍膨胀再加桥顶点，
// 0.5M 源 floats ≈ ≤1M 转换 floats ≈ ≤4MB/chunk，单块 glBufferData 十毫秒级、不拆要素。
constexpr size_t kGlobeChunkSrcFloats = 1u << 19;
// 点圆/图标 chunk 每组点数：点圆每点 96 顶点×9f≈3.4KB → 1024 点 ≈3.5MB 上限；图标 6 顶点×11f 远小于此。
constexpr size_t kGlobeChunkPoints = 1024;
// 视锥剔除外扩余量（米）：顶点按 alt=0 烘焙、着色器沿法向抬 uVecAlt≤500m，AABB 不含抬升量，
// 余量覆盖抬升与误差；保守剔除（整盒出平面 ≥ 余量才剔）→ 最坏少剔不误剔。
constexpr double kGlobeCullMarginM = 1000.0;
// 每层每类 chunk 数硬上限（失控安全网：预算逻辑已随几何有界，此处仅防程序错误致 chunk 爆炸）。
constexpr size_t kGlobeMaxChunks = 8192;

/// 3D chunk 可见性：世界 ECEF AABB 对 Navigator 预构的 Gribb 6 面做保守盒测试（与瓦片视锥剔除
/// 同判据：取盒上使 a·x+b·y+c·z 最大的正顶点，其在某平面外侧 → 整盒在外 → 剔）。margin 见常量注释。
inline bool globeChunkVisible(const VectorLayer::GlobeChunk &ch,
                              const std::array<FrustumPlane, 6> &planes) {
    for (const auto &pl : planes) {
        const double px = (pl.a >= 0.0) ? ch.xmax : ch.xmin;
        const double py = (pl.b >= 0.0) ? ch.ymax : ch.ymin;
        const double pz = (pl.c >= 0.0) ? ch.zmax : ch.zmin;
        if (pl.distanceTo(px, py, pz) < -kGlobeCullMarginM) return false;
    }
    return true;
}

/// AABB（double[6]: xmin,xmax,ymin,ymax,zmin,zmax 顺序见 gGlobeAabb*）展开/更新用：并入一个 ECEF 点。
inline void globeAabbExtend(double *bb, const Vec3 &p) {
    if (p.x < bb[0]) bb[0] = p.x;
    if (p.x > bb[1]) bb[1] = p.x;
    if (p.y < bb[2]) bb[2] = p.y;
    if (p.y > bb[3]) bb[3] = p.y;
    if (p.z < bb[4]) bb[4] = p.z;
    if (p.z > bb[5]) bb[5] = p.z;
}

/// 以转换完成的顶点数组新建 3D chunk VBO 并携带其世界 ECEF AABB（bb 序：xmin,xmax,ymin,ymax,zmin,zmax）。
/// 仅 GL 线程调用。
VectorLayer::GlobeChunk uploadGlobeChunk(const std::vector<float> &verts, const double *bb, int fpv) {
    VectorLayer::GlobeChunk c;
    if (verts.empty()) return c;
    glGenBuffers(1, &c.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    c.vertexCount = static_cast<GLsizei>(verts.size() / fpv);
    c.xmin = bb[0]; c.xmax = bb[1]; c.ymin = bb[2]; c.ymax = bb[3]; c.zmin = bb[4]; c.zmax = bb[5];
    return c;
}

/// 新建 AABB 初值（无效盒，首次 Extend 即覆盖）。
inline void globeAabbInit(double *bb) {
    bb[0] = 1e300;  bb[1] = -1e300;
    bb[2] = 1e300;  bb[3] = -1e300;
    bb[4] = 1e300;  bb[5] = -1e300;
}
/// AABB 是否仍为无效盒（无顶点时跳过上传）。
inline bool globeAabbEmpty(const double *bb) { return bb[0] > bb[1]; }

/// 回收某矢量层的全部 chunk VBO 与图标静态 VBO、重置流式上传进度（回到首要素）。
/// [del]=false 仅放弃句柄不 glDelete（GL 上下文已重建、旧句柄失效场景）。仅 GL 线程调用。
void resetVectorUpload(VectorLayer &vl, bool del) {
    auto delAll = [del](std::vector<VectorLayer::GlChunk> &chunks) {
        for (auto &c : chunks) if (del && c.vbo != 0) glDeleteBuffers(1, &c.vbo);
        chunks.clear();
    };
    delAll(vl.fillChunks);
    delAll(vl.outlineChunks);
    delAll(vl.lineChunks);
    if (del && vl.iconVbo != 0) glDeleteBuffers(1, &vl.iconVbo);
    vl.iconVbo = 0;
    vl.iconVertexCount = 0;
    // P2.e 3D chunk 与 2D chunk 同生命周期：新几何接管 / 上下文重建 / 层销毁三条路径统一回收，
    // 游标与完成位重置，下帧据 geom 重新流式上传。del=false 仅放弃句柄（旧上下文失效时 glDelete
    // 无意义且可能误删同名对象）。
    auto delGlobe = [del](std::vector<VectorLayer::GlobeChunk> &chunks) {
        for (auto &c : chunks) if (del && c.vbo != 0) glDeleteBuffers(1, &c.vbo);
        chunks.clear();
    };
    delGlobe(vl.globeFillChunks);
    delGlobe(vl.globeOutlineChunks);
    delGlobe(vl.globeLineChunks);
    delGlobe(vl.globePointChunks);
    delGlobe(vl.globeIconChunks);
    delGlobe(vl.globeExtrudeChunks);
    vl.globeUploadFeat = 0;
    vl.globeUploadPoint = 0;
    vl.globeUploadExtrude = 0;
    vl.globeUploadDone = false;
    vl.uploadFeat = 0;
    vl.uploadDone = false;
    vl.glReady = false;
}

} // namespace

Renderer::Renderer(const Navigator &navigator, const std::vector<std::unique_ptr<TileLayer>> &layers,
                   const std::vector<std::unique_ptr<VectorLayer>> &vectorLayers)
    : nav_(navigator), layers_(layers), vectorLayers_(vectorLayers) {}

Renderer::~Renderer() {
    // GL 资源必须在 GL 线程删除，故此处不调用 release()（off-thread 调用 glDelete* 会出错）。
    // 正常路径由 GlobeEngine::releaseGl() 在 GL 线程触发；EGL 上下文销毁时亦会回收。
}

uint64_t Renderer::tileKey(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 40) |
           (static_cast<uint64_t>(x) << 20) |
           static_cast<uint64_t>(y);
}

void Renderer::onSurfaceCreated() {
    // EGL 上下文可能是「重建」的：本界面被不透明界面（如设置页）遮挡 → onStop → GL surface 销毁 →
    // EGL 上下文丢失；返回时 surface 重新创建，GLSurfaceView 会再次回调本方法（按 Home 键返回同理）。
    // 旧上下文里的 VBO 名与纹理名在新上下文中已全部失效，必须先丢弃再按「全新上下文」重建，否则崩溃：
    // glBindBuffer 绑到失效 VBO 名会使 ARRAY_BUFFER 绑定退化为 0，glVertexAttribPointer 随之转为读
    // 客户端内存地址 0，glDrawArrays 解引用空指针触发 SIGSEGV，拖垮进程（模拟器表现为整机崩溃需重启）。
    // 纹理缓存直接 clear 即可（TileTexEntry/Texture 析构为 default，不调 glDeleteTextures），后续帧会重新
    // 解码上传（瓦片磁盘缓存仍在，重载很快）；VBO 句柄清零后由下方守卫重新 glGenBuffers 并上传数据。
    // 此处绝不能对旧句柄调 glDelete*——旧上下文已销毁，删除失效名无意义，且可能误删新上下文中的同名对象。
    layerTextures_.clear();
    vboDynamic_ = 0;
    vboUnitQuad_ = 0;
    // 字形图集 GL 纹理属旧上下文：放弃旧句柄（不 glDelete），保留 CPU 图集与字形表；
    // 下一帧据 dirty/invalid 重新上传（与新上下文重建同一口径，避免绑失效纹理名）。
    atlasTex_.abandon();
    markerIconTex_.abandon(); // 罗盘图标纹理属旧上下文：放弃句柄，下一帧据 markerIconRgba_ 重传
    vboText_ = 0;
    vboIcon_ = 0;
    vboGlobeMesh_ = 0; // 3D 网格动态 VBO 属旧上下文：放弃句柄，下帧逐瓦片重传
    // 矢量层 GL 资源同样属于旧上下文，已失效：清零 VBO 句柄 + glReady（保留 CPU 几何），
    // 下一帧据 geom 重新上传（与瓦片纹理/VBO 重建同一口径，避免绑失效 VBO 导致崩溃）。
    resetVectorGl();

    if (!colorProgram_.init(kColorVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: color shader init failed");
    } else {
        cAPos_ = colorProgram_.attribLocation("aPos");
        cAColor_ = colorProgram_.attribLocation("aColor");
        cUMvp_ = colorProgram_.uniformLocation("uMvp");
        cUOffset_ = colorProgram_.uniformLocation("uOffset");
    }

    if (!lineProgram_.init(kLineVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: line shader init failed");
    } else {
        lAPos_ = lineProgram_.attribLocation("aPos");
        lAMiter_ = lineProgram_.attribLocation("aMiter");
        lAColor_ = lineProgram_.attribLocation("aColor");
        lUMvp_ = lineProgram_.uniformLocation("uMvp");
        lUOffset_ = lineProgram_.uniformLocation("uOffset");
        lUHalfWidthWorld_ = lineProgram_.uniformLocation("uHalfWidthWorld");
    }

    if (!texProgram_.init(kTexVertexSrc, kTexFragmentSrc)) {
        LOGE("Renderer: texture shader init failed");
    } else {
        tAPos_ = texProgram_.attribLocation("aPos");
        tATexCoord_ = texProgram_.attribLocation("aTexCoord");
        tUMvp_ = texProgram_.uniformLocation("uMvp");
        tUTexture_ = texProgram_.uniformLocation("uTexture");
        tUUvOffset_ = texProgram_.uniformLocation("uUvOffset");
        tUUvScale_ = texProgram_.uniformLocation("uUvScale");
    }

    if (!textProgram_.init(kTextVertexSrc, kTextFragmentSrc)) {
        LOGE("Renderer: text shader init failed");
    } else {
        txAPos_ = textProgram_.attribLocation("aPos");
        txATexCoord_ = textProgram_.attribLocation("aTexCoord");
        txAColor_ = textProgram_.attribLocation("aColor");
        txUMvp_ = textProgram_.uniformLocation("uMvp");
        txUTexture_ = textProgram_.uniformLocation("uTexture");
    }

    if (!iconProgram_.init(kIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: icon shader init failed");
    } else {
        ixAPos_ = iconProgram_.attribLocation("aPos");
        iACorner_ = iconProgram_.attribLocation("aCorner");
        ixATexCoord_ = iconProgram_.attribLocation("aTexCoord");
        ixAColor_ = iconProgram_.attribLocation("aColor");
        ixUMvp_ = iconProgram_.uniformLocation("uMvp");
        ixUTexture_ = iconProgram_.uniformLocation("uTexture");
        ixUOffset_ = iconProgram_.uniformLocation("uOffset");
        ixUHalf_ = iconProgram_.uniformLocation("uHalf");
    }

    if (vboDynamic_ == 0) glGenBuffers(1, &vboDynamic_);

    // 3D 球体通路：透视瓦片纹理/占位两程序（片元复用 2D 对应片元着色器）
    if (!globeTexProgram_.init(kGlobeTexVertexSrc, kTexFragmentSrc)) {
        LOGE("Renderer: globe texture shader init failed");
    } else {
        g3APos_ = globeTexProgram_.attribLocation("aPos");
        g3ATexCoord_ = globeTexProgram_.attribLocation("aTexCoord");
        g3UMvp_ = globeTexProgram_.uniformLocation("uMvp");
        g3UTexture_ = globeTexProgram_.uniformLocation("uTexture");
        g3UUvOffset_ = globeTexProgram_.uniformLocation("uUvOffset");
        g3UUvScale_ = globeTexProgram_.uniformLocation("uUvScale");
    }
    if (!globeColorProgram_.init(kGlobeColorVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe color shader init failed");
    } else {
        gc3APos_ = globeColorProgram_.attribLocation("aPos");
        gc3AColor_ = globeColorProgram_.attribLocation("aColor");
        gc3UMvp_ = globeColorProgram_.uniformLocation("uMvp");
    }
    // P2.a 3D 矢量 fill/stroke 两程序（片元均复用 kColorFragmentSrc）
    if (!globeFillProgram_.init(kGlobeFillVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe fill shader init failed");
    } else {
        gfAPos_ = globeFillProgram_.attribLocation("aPos");
        gfAColor_ = globeFillProgram_.attribLocation("aColor");
        gfAMode_ = globeFillProgram_.attribLocation("aMode");
        gfUMvp_ = globeFillProgram_.uniformLocation("uMvp");
        gfUOffset_ = globeFillProgram_.uniformLocation("uOffset");
        gfUAnchor_ = globeFillProgram_.uniformLocation("uAnchor");
        gfUVecAlt_ = globeFillProgram_.uniformLocation("uVecAlt");
    }
    if (!globeStrokeProgram_.init(kGlobeStrokeVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe stroke shader init failed");
    } else {
        gsAPrev_ = globeStrokeProgram_.attribLocation("aPrev");
        gsACur_ = globeStrokeProgram_.attribLocation("aCur");
        gsANext_ = globeStrokeProgram_.attribLocation("aNext");
        gsASide_ = globeStrokeProgram_.attribLocation("aSide");
        gsAColor_ = globeStrokeProgram_.attribLocation("aColor");
        gsAMode_ = globeStrokeProgram_.attribLocation("aMode");
        gsUMvp_ = globeStrokeProgram_.uniformLocation("uMvp");
        gsUOffset_ = globeStrokeProgram_.uniformLocation("uOffset");
        gsUAnchor_ = globeStrokeProgram_.uniformLocation("uAnchor");
        gsUVecAlt_ = globeStrokeProgram_.uniformLocation("uVecAlt");
        gsUHalfWidthPx_ = globeStrokeProgram_.uniformLocation("uHalfWidthPx");
        gsUViewportPx_ = globeStrokeProgram_.uniformLocation("uViewport");
        gsUMiterLimit_ = globeStrokeProgram_.uniformLocation("uMiterLimit");
    }
    // P2.b 3D 点要素 billboard 程序（片元复用 kColorFragmentSrc）
    if (!globePointProgram_.init(kGlobePointVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe point shader init failed");
    } else {
        gpAPos_ = globePointProgram_.attribLocation("aPos");
        gpACorner_ = globePointProgram_.attribLocation("aCorner");
        gpAColor_ = globePointProgram_.attribLocation("aColor");
        gpAMode_ = globePointProgram_.attribLocation("aMode");
        gpUMvp_ = globePointProgram_.uniformLocation("uMvp");
        gpUOffset_ = globePointProgram_.uniformLocation("uOffset");
        gpUAnchor_ = globePointProgram_.uniformLocation("uAnchor");
        gpUVecAlt_ = globePointProgram_.uniformLocation("uVecAlt");
        gpUHalfPx_ = globePointProgram_.uniformLocation("uHalfPx");
        gpUViewportPx_ = globePointProgram_.uniformLocation("uViewport");
    }
    // P2.c 3D 点要素图标 billboard 程序（片元复用 kIconFragmentSrc：纹理×顶点色，保留图标彩色）
    if (!globeIconProgram_.init(kGlobeIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: globe icon shader init failed");
    } else {
        giaPos_ = globeIconProgram_.attribLocation("aPos");
        giACorner_ = globeIconProgram_.attribLocation("aCorner");
        giATexCoord_ = globeIconProgram_.attribLocation("aTexCoord");
        giAColor_ = globeIconProgram_.attribLocation("aColor");
        giaMode_ = globeIconProgram_.attribLocation("aMode");
        giUMvp_ = globeIconProgram_.uniformLocation("uMvp");
        giUOffset_ = globeIconProgram_.uniformLocation("uOffset");
        giUAnchor_ = globeIconProgram_.uniformLocation("uAnchor");
        giUVecAlt_ = globeIconProgram_.uniformLocation("uVecAlt");
        giUHalfPx_ = globeIconProgram_.uniformLocation("uHalfPx");
        giUViewportPx_ = globeIconProgram_.uniformLocation("uViewport");
        giUTexture_ = globeIconProgram_.uniformLocation("uTexture");
    }
    // 3D 贴地定位标识图标通路（片元复用 kIconFragmentSrc：纹理×顶点色，保留图标彩色；透视无屏幕展开）
    if (!globeFlatIconProgram_.init(kGlobeFlatIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: globe flat icon shader init failed");
    } else {
        gfiPos_ = globeFlatIconProgram_.attribLocation("aPos");
        gfiTexCoord_ = globeFlatIconProgram_.attribLocation("aTexCoord");
        gfiColor_ = globeFlatIconProgram_.attribLocation("aColor");
        gfiMvp_ = globeFlatIconProgram_.uniformLocation("uMvp");
        gfiTexture_ = globeFlatIconProgram_.uniformLocation("uTexture");
    }
    if (vboGlobeMesh_ == 0) glGenBuffers(1, &vboGlobeMesh_);

    // 文本标注逐帧填充的动态 VBO
    if (vboText_ == 0) glGenBuffers(1, &vboText_);

    // 点要素图标逐帧填充的动态 VBO
    if (vboIcon_ == 0) glGenBuffers(1, &vboIcon_);

    // 静态单位四边形（纹理瓦片复用，逐瓦片仅换模型矩阵与纹理）
    if (vboUnitQuad_ == 0) {
        glGenBuffers(1, &vboUnitQuad_);
        glBindBuffer(GL_ARRAY_BUFFER, vboUnitQuad_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glClearColor(0.05f, 0.08f, 0.12f, 1.0f);
    // 纹理上下文的 GL 状态在绘制时按需设置
    LOGI("Renderer::onSurfaceCreated ok");
}

void Renderer::onSurfaceChanged(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
    glViewport(0, 0, width, height);
    LOGI("Renderer::onSurfaceChanged %d x %d", width, height);
}

Renderer::TileTexEntry &Renderer::getTexture(TexMap &texMap, TileLoader &loader, int z, int x, int y) {
    const uint64_t key = tileKey(z, x, y);
    auto it = texMap.find(key);
    if (it != texMap.end()) {
        it->second.lastUsed = frameTick_; // 命中即刷新 LRU
        // 占位条目（纹理未就绪）幂等重请求：其唯一一次 request 的交付若被 ready_ 高水位丢弃、或遇瞬时
        // 失败、或 EGL 上下文重建后 layerTextures_ 清空引发的一次性全量重请求洪峰中被丢，则该无效占位
        // 既不重请求、又因 lastUsed 每帧刷新而不被 LRU 驱逐 → 永久棋盘、缩放也不自愈。TileLoader::request
        // 以 pending_/failedUntil_ 去重：在途/冷却期为廉价 no-op，交付丢失时重新入队、读盘秒得 → 占位最终填满。
        if (!it->second.tex.isValid()) loader.request(z, x, y);
        return it->second;
    }
    // 缓存未命中：插入 id==0 占位条目并交后台加载器异步取（先读盘后联网）；GL 线程绝不阻塞于磁盘 I/O。
    // 就绪后经 renderLayer 的 drainReady 解码上传；未就绪期间该瓦片走祖先兜底或占位（对齐 参考实现 getTexture 返回 null）。
    // 占位条目一旦插入，后续帧命中即返回，不会对同一瓦片重复请求。
    loader.request(z, x, y);
    TileTexEntry entry;
    entry.lastUsed = frameTick_;
    return texMap.emplace(key, std::move(entry)).first->second;
}

Renderer::TileTexEntry *Renderer::findAncestorEntry(TexMap &texMap, int level, int tx, int ty, int &outAncestorLevel) {
    // 自 level-1 向上逐级查找最近的、纹理有效的祖先瓦片 (L, tx>>k, ty>>k)，k=level-L。
    // tx/ty 已由 visibleTileRange 裁剪到 [0,n-1] 非负，右移安全。
    for (int L = level - 1; L >= 0; --L) {
        const int k = level - L;
        auto found = texMap.find(tileKey(L, tx >> k, ty >> k));
        if (found != texMap.end() && found->second.tex.isValid()) {
            outAncestorLevel = L;
            return &found->second;
        }
    }
    outAncestorLevel = -1;
    return nullptr;
}

void Renderer::evictTileTextures(TexMap &texMap) {
    if (texMap.size() <= kMaxCachedTiles) return;
    // 超容量驱逐排序：先驱逐无纹理的占位条目（已上传纹理受保护），再按 lastUsed 升序淘汰最久未用项。
    // 若不排除占位优先：3D 大范围视图叶瓦片数超容量时，成功纹理被挤掉→下帧 miss 重请求→
    // 触发服务限流（HTTP 429）的请求风暴死循环（切 3D 只剩棋盘、切回 2D 也异常的真因）。
    std::vector<std::tuple<int, uint64_t, uint64_t>> order; // (有纹理?1:0, lastUsed, key)
    order.reserve(texMap.size());
    for (const auto &kv : texMap)
        order.emplace_back(kv.second.tex.isValid() ? 1 : 0, kv.second.lastUsed, kv.first);
    std::sort(order.begin(), order.end());
    const size_t toRemove = texMap.size() - kMaxCachedTiles;
    for (size_t i = 0; i < toRemove && i < order.size(); ++i) {
        auto it = texMap.find(std::get<2>(order[i]));
        if (it == texMap.end()) continue;
        it->second.tex.release();
        texMap.erase(it);
    }
}

void Renderer::clearTileTextures() {
    for (auto &texMap : layerTextures_) {
        for (auto &kv : texMap) kv.second.tex.release();
    }
    layerTextures_.clear();
}

bool Renderer::onDrawFrame() {
    // 视图模式分发：3D 球体走独立管线（透视+深度+贴球网格，复用瓦片纹理缓存），2D 正交通路零回归
    if (nav_.viewMode() == Navigator::ViewMode::MODE_3D) return onDrawFrame3D();
    glClear(GL_COLOR_BUFFER_BIT);
    if (colorProgram_.program() == 0 || texProgram_.program() == 0) return false;

    ++frameTick_; // LRU 帧标记：本帧用到的纹理 lastUsed 置此值，淘汰时最久未用者先出

    if (layers_.empty()) return false;
    // 每层一份纹理缓存，索引与 layers_ 对齐；addTileLayer 在运行期追加图层时按需扩容
    if (layerTextures_.size() != layers_.size()) layerTextures_.resize(layers_.size());

    // 相机 → 世界中心与可视半跨度（每帧算一次，各层共享；瓦片级别按各层 maxLevel 分别钳制）
    double cwx = 0.0, cwy = 0.0;
    nav_.worldCenter(cwx, cwy);
    const double hw = nav_.halfWorldWidth();
    const double hh = nav_.halfWorldHeight();

    // 正交投影（相机相对渲染 RTC，对齐 参考实现 globe.offset）：世界可视矩形 → NDC；top<bottom 翻转 y 轴。
    // 所有瓦片坐标先在 double 下减去相机中心 (cwx,cwy) 再转 float，投影对称于原点 [-hw,hw]x[-hh,hh]。
    // 否则高缩放级别下世界坐标（~0.12）与视口半跨度（~1e-5）量级悬殊，float32 有效位被大偏移占满，
    // 瓦片边缘定位误差被 ortho 大比例放大到亚像素级 → 相邻瓦片留缝露出深色清屏形成「黑线」（仅高级别可见）。
    const Matrix4 ortho = Matrix4::ortho(
        static_cast<float>(-hw), static_cast<float>(hw),
        static_cast<float>(hh), static_cast<float>(-hh));

    // 绘制顺序（对齐主界面「注记置顶于矢量之上」）：底图瓦片层（非 overlay）→ 矢量层 →
    // 注记 overlay 层 → 定位标记。任一层仍有资源在途则需再画一帧。
    bool needRedraw = false;
    for (size_t li = 0; li < layers_.size(); ++li) {
        // 注记 overlay 层留到矢量之后；栅格层虽 overlay=true（alpha 混合）但须与底图同趟画在矢量之前
        if (layers_[li]->overlay && !layers_[li]->raster) continue;
        if (renderLayer(li, cwx, cwy, hw, hh, ortho)) needRedraw = true;
    }
    // 矢量层：绘制在底图之上、注记之下
    if (drawVectorLayers(cwx, cwy, hw, ortho)) needRedraw = true;
    // 点要素图标：屏幕固定 billboard，绘制在矢量层之上、标注之下
    drawVectorIcons(cwx, cwy, hw, ortho);
    // 矢量要素标注：绘制在矢量层之上、注记 overlay 之下（不影响 needRedraw：字体加载/几何就绪由其它路径驱动重绘）
    drawVectorLabels(cwx, cwy, hw, ortho);
    for (size_t li = 0; li < layers_.size(); ++li) {
        if (!layers_[li]->overlay || layers_[li]->raster) continue; // 仅注记 overlay 层（栅格已在矢量前绘制）
        if (renderLayer(li, cwx, cwy, hw, hh, ortho)) needRedraw = true;
    }
    // 定位标记（蓝点）叠加在所有图层之上，以屏幕固定尺寸绘制（不影响 needRedraw：其位置更新由外部触发重绘）。
    drawLocationMarker(cwx, cwy, hw, ortho);
    return needRedraw;
}

bool Renderer::renderLayer(size_t layerIndex, double cwx, double cwy, double hw, double hh, const Matrix4 &ortho) {
    TileLayer &layer = *layers_[layerIndex];
    // 隐藏层整层跳过（对齐 参考实现 Layer.isEnabled=false）：不绘制、不请求瓦片，也不需要因它再画一帧。
    if (!layer.visible) return false;
    TileLoader &loader = *layer.loader;
    TexMap &texMap = layerTextures_[layerIndex];

    // 交付后台下载就绪的瓦片：解码上传到本层持久缓存（不按级别过滤，供当前级与祖先兜底复用）。
    // 限制单帧解码上传量：一批瓦片同时就绪（如跨级/联网返回）时若全部在本帧解码会集中占用 GL 线程
    // 造成掉帧（时快时慢）；取预算内的量、其余留到后续帧渐进补齐，缩放时表现为「由粗到细」连续变清晰。
    {
        constexpr int kMaxDecodePerFrame = 12;
        std::vector<TileLoader::ReadyTile> ready;
        loader.drainReady(ready, kMaxDecodePerFrame);
        for (auto &rt : ready) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (!ImageDecoder::decodeRGBA(rt.bytes.data(), rt.bytes.size(), w, h, rgba)) continue;
            Texture tex;
            if (!tex.uploadRGBA(rgba.data(), w, h)) continue;
            TileTexEntry &e = texMap[tileKey(rt.z, rt.x, rt.y)];
            e.tex.release(); // 若已有占位/旧纹理，先释放再替换
            e.tex = std::move(tex);
            e.lastUsed = frameTick_;
        }
    }

    // LOD 叶级别：按本层图源 maxLevel 逐层钳制（zoom 超过后恒为 maxLevel，由投影拉伸末级瓦片纹理）。
    const int level = nav_.tileLevel(layer.maxLevel);

    // 可见瓦片行列范围
    int txMin = 0, txMax = -1, tyMin = 0, tyMax = -1;
    TileMatrix::visibleTileRange(cwx, cwy, hw, hh, level, txMin, txMax, tyMin, tyMax);
    if (txMax < txMin || tyMax < tyMin) {
        evictTileTextures(texMap);
        return loader.hasPendingOrReady();
    }

    const double n = static_cast<double>(TileMatrix::tilesAtLevel(level));
    const float tileWorld = static_cast<float>(1.0 / n);
    // 纹理瓦片外扩半纹素做重叠（overlap/bleed）：Pass B 用「平移+缩放」的模型矩阵定位，
    // 相邻瓦片的公共边在浮点上不严格相等（x0+tileWorld ≠ float((tx+1)/n)），会留亚像素缝隙
    // 露出深色清屏背景形成「黑边」。外扩后邻块重叠约 1px，彻底盖住缝隙；放大显示下拉伸 <0.5% 不可见。
    const float texelPad = tileWorld * (0.5f / 256.0f);

    // 请求可见叶瓦片及其祖先链的纹理（异步）。对齐 参考实现 沿金字塔路径 getTexture + retrieveTopLevelTiles：
    // 确保放大/缩小时各级祖先都在缓存中可兜底，不露黑洞。getTexture 命中缓存即返回，不会重复请求。
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            getTexture(texMap, loader, level, tx, ty);
            // 祖先链：level-1 .. 0；(tx,ty) 右移 k 位即第 k 级祖先的行列
            for (int L = level - 1, k = 1; L >= 0; --L, ++k) {
                getTexture(texMap, loader, L, tx >> k, ty >> k);
            }
        }
    }

    // Pass A：占位（无纹理）瓦片批量纯色绘制，保留网格可见性。
    // overlay（注记）层跳过：注记瓦片为带透明 PNG，缺失处应保持透明露出下方底图，不画深色占位块。
    if (!layer.overlay) {
        std::vector<float> verts;
        for (int ty = tyMin; ty <= tyMax; ++ty) {
            const float y0 = static_cast<float>(ty / n - cwy);
            const float y1 = static_cast<float>((ty + 1) / n - cwy);
            for (int tx = txMin; tx <= txMax; ++tx) {
                auto it = texMap.find(tileKey(level, tx, ty));
                if (it != texMap.end() && it->second.tex.isValid()) continue; // 自身有纹理 → Pass B
                int ancLevelA = -1;
                if (findAncestorEntry(texMap, level, tx, ty, ancLevelA) != nullptr) continue; // 有祖先兜底 → Pass B
                const float x0 = static_cast<float>(tx / n - cwx);
                const float x1 = static_cast<float>((tx + 1) / n - cwx);
                const bool even = (((tx + ty) & 1) == 0);
                const float r = even ? 0.16f : 0.24f;
                const float g = even ? 0.34f : 0.44f;
                const float b = even ? 0.32f : 0.40f;
                pushColorVertex(verts, x0, y0, r, g, b);
                pushColorVertex(verts, x1, y0, r, g, b);
                pushColorVertex(verts, x1, y1, r, g, b);
                pushColorVertex(verts, x0, y0, r, g, b);
                pushColorVertex(verts, x1, y1, r, g, b);
                pushColorVertex(verts, x0, y1, r, g, b);
            }
        }
        if (!verts.empty()) {
            colorProgram_.use();
            glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(cUOffset_, 0.0f, 0.0f); // 瓦片占位顶点已烘焙相机相对坐标，无需 RTC 偏移
            glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(cAPos_);
            glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                                  reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(cAColor_);
            glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                                  reinterpret_cast<const void *>(2 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / kColorFloatsPerVertex));
            glDisableVertexAttribArray(cAPos_);
            glDisableVertexAttribArray(cAColor_);
        }
    }

    // Pass B：纹理瓦片逐瓦片绘制（各自绑定纹理 + 模型矩阵，复用单位四边形）。
    // overlay（注记）层开 alpha 混合，使 PNG 透明区露出下方底图、非透明注记叠加其上（对齐主界面注记置顶）。
    if (layer.overlay) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    texProgram_.use();
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(tUTexture_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vboUnitQuad_);
    glEnableVertexAttribArray(tAPos_);
    glVertexAttribPointer(tAPos_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(tATexCoord_);
    glVertexAttribPointer(tATexCoord_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(2 * sizeof(float)));

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        const float y0 = static_cast<float>(ty / n - cwy);
        for (int tx = txMin; tx <= txMax; ++tx) {
            const float x0 = static_cast<float>(tx / n - cwx);
            const Matrix4 model = Matrix4::model2D(x0 - texelPad, y0 - texelPad,
                                                   tileWorld + 2.0f * texelPad,
                                                   tileWorld + 2.0f * texelPad);
            const Matrix4 mvp = Matrix4::multiply(ortho, model);
            glUniformMatrix4fv(tUMvp_, 1, GL_FALSE, mvp.data());

            auto it = texMap.find(tileKey(level, tx, ty));
            if (it != texMap.end() && it->second.tex.isValid()) {
                // 自身纹理：UV 恒等映射
                glUniform2f(tUUvOffset_, 0.0f, 0.0f);
                glUniform2f(tUUvScale_, 1.0f, 1.0f);
                it->second.tex.bind();
                glDrawArrays(GL_TRIANGLES, 0, 6);
                continue;
            }
            // 祖先兜底（对齐 参考实现 useAncestorTileTexture）：用最近有效祖先纹理的 UV 子矩形拉伸铺满本子瓦片，
            // 覆盖跨级/加载途中未就绪的瓦片，消除黑缝。子瓦片在祖先内的行列 = (tx,ty) 的低 k 位。
            int ancLevel = -1;
            TileTexEntry *anc = findAncestorEntry(texMap, level, tx, ty, ancLevel);
            if (anc == nullptr) continue; // 无自身也无祖先 → overlay 保持透明 / 底图已在 Pass A 占位
            anc->lastUsed = frameTick_;   // 触碰祖先，避免被 LRU 淘汰
            const int subInt = 1 << (level - ancLevel);
            const float sub = static_cast<float>(subInt);
            const float u0 = static_cast<float>(tx & (subInt - 1)) / sub;
            const float v0 = static_cast<float>(ty & (subInt - 1)) / sub;
            glUniform2f(tUUvOffset_, u0, v0);
            glUniform2f(tUUvScale_, 1.0f / sub, 1.0f / sub);
            anc->tex.bind();
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    glDisableVertexAttribArray(tAPos_);
    glDisableVertexAttribArray(tATexCoord_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (layer.overlay) glDisable(GL_BLEND);

    // LRU 淘汰：限制本层持久纹理缓存上限，防止跨级别累积无界增长
    evictTileTextures(texMap);

    // 返回本层是否需再画一帧：后台仍有瓦片排队/下载/就绪待上传时需继续刷新增量补齐。
    return loader.hasPendingOrReady();
}

// ==================== 3D 球体通路（P1） ====================

bool Renderer::onDrawFrame3D() {
    // 清颜色+深度（深度缓冲由 GLSurfaceView 默认 EGLConfig 提供 16bit）；背景仍为深色=太空
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (globeTexProgram_.program() == 0 || globeColorProgram_.program() == 0) return false;

    ++frameTick_; // 与 2D 共用 LRU 帧标记：切换模式后缓存继续按帧号淘汰

    const Navigator::Camera3D cam = nav_.camera3D();
    if (!cam.valid) return false;

    // 绝对 viewProj 仅供 Tessellator 提视锥平面（包围球心为绝对 ECEF）；绘制用 RTC 版：
    // 顶点已在 CPU 端 double 减眼点，若再套含 -s·eye 平移的绝对 view 会二次错位。
    const Matrix4 viewProjAbs = Matrix4::multiply(cam.proj, cam.view);
    const Matrix4 viewRtc = Matrix4::lookAt(Vec3{}, cam.center - cam.eye, cam.up);
    const Matrix4 viewProjRtc = Matrix4::multiply(cam.proj, viewRtc);

    bool needRedraw = false;

    // Pass A：瓦片球面（开深度）——只画底图与栅格 overlay（栅格须垫矢量之下，2D 同口径）；
    // 注记 overlay（非栅格）留到矢量之后画，对齐 2D「注记置顶于矢量之上」（onDrawFrame 分发序）。
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL); // 贴球网格间允许共面覆盖（外扩重叠区后画者胜，免大面片 early-z 拒绘）

    if (!layers_.empty()) {
        if (layerTextures_.size() != layers_.size()) layerTextures_.resize(layers_.size());
        for (size_t li = 0; li < layers_.size(); ++li) {
            if (layers_[li]->overlay && !layers_[li]->raster) continue; // 注记 overlay 留到矢量后
            if (renderGlobeLayer3D(li, cam, viewProjAbs, viewProjRtc)) needRedraw = true;
        }
    }

    // Pass B：3D 矢量层（面/描边/线/点/图标），仍开深度、启 polygon offset（实现内局部开关）。
    // drainReady 接管几何，与 2D 共用 loader/glReady 口径；相机未变则绑旧 VBO 直接画。
    if (globeFillProgram_.program() != 0 && globeStrokeProgram_.program() != 0) {
        if (renderGlobeVectors3D(cam, viewProjRtc)) needRedraw = true;
    }

    // Pass B2：注记 overlay 贴球面画在矢量之上（矢量不写深度 + 抬 vecAlt，若先画会被注记的深度测试挡掉）。
    // 本 Pass 内 fill 的 polygonOffset(+2) 已失效——注记与矢量填充共面斜视时的轻度 z-fight 留待后续优化。
    for (size_t li = 0; li < layers_.size(); ++li) {
        if (!layers_[li]->overlay || layers_[li]->raster) continue; // 仅注记 overlay（栅格已在矢量前绘制）
        if (renderGlobeLayer3D(li, cam, viewProjAbs, viewProjRtc)) needRedraw = true;
    }

    // Pass C：3D 矢量标注（P2.d）——逐锚点投影到屏幕像素后作为纯屏幕叠加（内部关/复开深度），
    // 画在矢量层之上、marker 之下（marker 始终置顶）。
    drawVectorLabels3D(cam, viewProjRtc);

    // Pass D：3D 定位 marker（罗盘图标/蓝点 + 方向箭头）——投影到屏幕像素后作为纯屏幕叠加画在最上层
    // （内部自关/复开深度测试）。
    drawLocationMarker3D(cam, viewProjRtc);

    glDisable(GL_DEPTH_TEST);
    return needRedraw;
}

bool Renderer::renderGlobeLayer3D(size_t layerIndex, const Navigator::Camera3D &cam,
                                  const Matrix4 &viewProjAbs, const Matrix4 &viewProjRtc) {
    TileLayer &layer = *layers_[layerIndex];
    if (!layer.visible) return false; // 隐藏层整层跳过（同 2D）
    TileLoader &loader = *layer.loader;
    TexMap &texMap = layerTextures_[layerIndex];

    // drain 就绪瓦片解码上传（与 2D renderLayer 同预算）：纹理缓存按 (z,x,y) 键共用，
    // 2D 下载的瓦片切 3D 直接命中，反之亦然；单帧解码限额防 GL 线程尖峰。
    {
        constexpr int kMaxDecodePerFrame = 12;
        std::vector<TileLoader::ReadyTile> ready;
        loader.drainReady(ready, kMaxDecodePerFrame);
        for (auto &rt : ready) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (!ImageDecoder::decodeRGBA(rt.bytes.data(), rt.bytes.size(), w, h, rgba)) continue;
            Texture tex;
            if (!tex.uploadRGBA(rgba.data(), w, h)) continue;
            TileTexEntry &e = texMap[tileKey(rt.z, rt.x, rt.y)];
            e.tex.release();
            e.tex = std::move(tex);
            e.lastUsed = frameTick_;
        }
    }

    // 可见叶瓦片：目标级别与 2D LOD 同源（tileLevel(maxLevel) 保切换时地物粗细连续），
    // 更远处（地平线/背面）由 Tessellator 按屏幕空间误差自动取粗级；墨卡托 (z,x,y) 键与 2D 互通。
    const int targetLevel = nav_.tileLevel(layer.maxLevel);
    const double tanHalf = std::tan(nav_.fieldOfViewDeg() * 0.5 * 3.14159265358979323846 / 180.0);
    TessCamera tc;
    tc.eye = cam.eye;
    tc.viewProj = viewProjAbs;
    tc.frustumPlanes = cam.frustumPlanes; // 直接采用 Navigator 预提的 double 平面
    Tessellator::buildVisibleTiles(tc, targetLevel, layer.maxLevel, tanHalf, viewportHeight_,
                                   nav_.lodDetailFactor(), Wgs84Globe::instance(), globeTilesScratch_);

    // 请求预算：可见叶数可远超纹理缓存容量（3D 全景下千级），若全部插入占位又被驱逐，会逐帧
    // 重复「驱逐→miss 重请求→服务端限流(429)→失败」的风暴。按 z 降序（z 越大距视线越近）
    // 仅对前段叶瓦片请求自身；其余远侧瓦片只请求祖先链（链上各级多与近侧共享，去重后增量很小），
    // 它们绘制时经祖先 UV 子矩形兜底，损失仅在球缘斜视区的精细度。
    constexpr size_t kGlobeLeafRequestBudget = 192; // < kMaxCachedTiles，留余量给祖先链
    std::stable_sort(globeTilesScratch_.begin(), globeTilesScratch_.end(),
                     [](const GlobeTile &a, const GlobeTile &b) { return a.z > b.z; });
    const size_t leafBudget = std::min(globeTilesScratch_.size(), kGlobeLeafRequestBudget);

    // 请求策略（对齐 参考实现「每枚可见叶都请求自身那一层级」）：
    //  · 自身纹理：对「全部」可见叶请求（近景 z16、向地平线平滑降到粗级）——远景因此显示自己真正的影像，
    //    而非被全球粗级(z=0..3)拉伸，这正是 参考实现 远景更清晰的根因（也是上一版「只请求近景 192 叶」致糊的教训）。
    //  · 祖先链：仅对近景预算叶（z 降序前段）铺，作渐进加载期的兜底；链是数量倍增器，远景叶不铺链——
    //    其祖先多为已被近景链铺过并缓存的共享粗级，findAncestorEntry 向上走即可命中。
    // OOM 安全性：worker 仅 4 线程（在途下载天然 ≤4，等价 参考实现 RetrievalLanes 的并发闸），且 TileLoader.ready_
    // 有高水位背压（超 kMaxReadyTiles 丢弃本次交付、字节已落盘下帧读盘秒得），故数千叶也不会撑爆 native 堆。
    for (size_t i = 0; i < globeTilesScratch_.size(); ++i) {
        const GlobeTile &t = globeTilesScratch_[i];
        getTexture(texMap, loader, t.z, t.x, t.y);
        if (i < leafBudget) {
            for (int L = t.z - 1, k = 1; L >= 0; --L, ++k) {
                getTexture(texMap, loader, L, t.x >> k, t.y >> k);
            }
        }
    }

    // Pass A 占位：无图且无祖先的瓦片画棋盘深色贴球面（首帧/断网时呈现球体轮廓）；
    // overlay（注记）层跳过保持透明（同 2D）。批量累积一次上传一次绘制。
    if (!layer.overlay) {
        std::vector<float> verts;
        for (const GlobeTile &t : globeTilesScratch_) {
            auto it = texMap.find(tileKey(t.z, t.x, t.y));
            if (it != texMap.end() && it->second.tex.isValid()) continue;
            int ancLevelA = -1;
            if (findAncestorEntry(texMap, t.z, t.x, t.y, ancLevelA) != nullptr) continue;
            const bool even = (((t.x + t.y) & 1) == 0);
            buildGlobeTileMesh(t, cam.eye, even ? 0.16f : 0.24f, even ? 0.34f : 0.44f,
                               even ? 0.32f : 0.40f, verts);
        }
        if (!verts.empty()) {
            globeColorProgram_.use();
            glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
            glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_DYNAMIC_DRAW);
            const GLsizei stride = kGlobeFloatsPerVertex * sizeof(float);
            glEnableVertexAttribArray(gc3APos_);
            glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(gc3AColor_);
            glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<const void *>(5 * sizeof(float)));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(verts.size() / kGlobeFloatsPerVertex));
            glDisableVertexAttribArray(gc3APos_);
            glDisableVertexAttribArray(gc3AColor_);
        }
    }

    // Pass B 纹理：逐瓦片生成网格上传绘制（纹理绑定/UV uniform 本就逐瓦片，共享单一动态 VBO）。
    // 自身纹理 UV 恒等；未就绪用最近有效祖先子矩形拉伸兜底（同 2D useAncestorTileTexture）。
    if (layer.overlay) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    globeTexProgram_.use();
    glUniformMatrix4fv(g3UMvp_, 1, GL_FALSE, viewProjRtc.data());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(g3UTexture_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
    const GLsizei stride = kGlobeFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(g3APos_);
    glVertexAttribPointer(g3APos_, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(g3ATexCoord_);
    glVertexAttribPointer(g3ATexCoord_, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void *>(3 * sizeof(float)));
    for (const GlobeTile &t : globeTilesScratch_) {
        TileTexEntry *use = nullptr;
        float u0 = 0.0f, v0 = 0.0f, sc = 1.0f;
        auto it = texMap.find(tileKey(t.z, t.x, t.y));
        if (it != texMap.end() && it->second.tex.isValid()) {
            use = &it->second;
        } else {
            int ancLevel = -1;
            TileTexEntry *anc = findAncestorEntry(texMap, t.z, t.x, t.y, ancLevel);
            if (anc == nullptr) continue; // 无图无祖先 → Pass A 已占位 / overlay 保持透明
            anc->lastUsed = frameTick_;
            const int subInt = 1 << (t.z - ancLevel);
            const float sub = static_cast<float>(subInt);
            u0 = static_cast<float>(t.x & (subInt - 1)) / sub;
            v0 = static_cast<float>(t.y & (subInt - 1)) / sub;
            sc = 1.0f / sub;
            use = anc;
        }
        globeMeshScratch_.clear();
        buildGlobeTileMesh(t, cam.eye, 1.0f, 1.0f, 1.0f, globeMeshScratch_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(globeMeshScratch_.size() * sizeof(float)),
                     globeMeshScratch_.data(), GL_DYNAMIC_DRAW);
        glUniform2f(g3UUvOffset_, u0, v0);
        glUniform2f(g3UUvScale_, sc, sc);
        use->tex.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0,
                     static_cast<GLsizei>(globeMeshScratch_.size() / kGlobeFloatsPerVertex));
    }
    glDisableVertexAttribArray(g3APos_);
    glDisableVertexAttribArray(g3ATexCoord_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (layer.overlay) glDisable(GL_BLEND);

    evictTileTextures(texMap);
    return loader.hasPendingOrReady();
}

void Renderer::buildGlobeTileMesh(const GlobeTile &tile, const Vec3 &eye, float r, float g, float b,
                                  std::vector<float> &verts) const {
    // 包围盒外约 0.75% 外扩：盖住不同级别邻瓦片的 T 形缝隙（深度测试裁决前后，轻微 z-fighting
    // 可接受，同 2D Pass B texelPad 思路）；极地附近经度收敛，外扩仅按跨径比例不改变形状
    const double padLon = (tile.lonMax - tile.lonMin) * 0.0075;
    const double padLat = (tile.latMax - tile.latMin) * 0.0075;
    const double lon0 = tile.lonMin - padLon, lon1 = tile.lonMax + padLon;
    const double latN = tile.latMax + padLat, latS = tile.latMin - padLat;
    const Wgs84Globe &globe = Wgs84Globe::instance();
    constexpr int G = kGlobeGridDiv;
    for (int row = 0; row < G; ++row) {
        for (int c = 0; c <= G; ++c) {
            const double u = static_cast<double>(c) / G; // 0=西（lonMin）
            const double lon = lon0 + (lon1 - lon0) * u;
            for (int e = 0; e < 2; ++e) {
                // 逐行 (上,下) 顶点对→ GL_TRIANGLE_STRIP；跨带缠绕方向交替但未开背面剪裁，无影响
                const double v = static_cast<double>(row + e) / G; // 0=北边=图像顶行（同 2D 单位四边形口径）
                const double lat = latN + (latS - latN) * v;
                Vec3 p;
                globe.geographicToCartesian(lon, lat, 0.0, p);
                p -= eye; // RTC：double 差后转 float（ECEF 量级 ~6.4e6 m，绝对坐标不可直上 float）
                verts.push_back(static_cast<float>(p.x));
                verts.push_back(static_cast<float>(p.y));
                verts.push_back(static_cast<float>(p.z));
                verts.push_back(static_cast<float>(u));
                verts.push_back(static_cast<float>(v));
                verts.push_back(r);
                verts.push_back(g);
                verts.push_back(b);
                verts.push_back(1.0f);
            }
        }
    }
}

void Renderer::setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg) {
    std::lock_guard<std::mutex> lock(markerMtx_);
    markerLon_ = lonDeg;
    markerLat_ = latDeg;
    markerVisible_ = visible;
    markerHeading_ = headingDeg;
}

void Renderer::setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h) {
    // UI 线程写（构造期只写），GL 线程只读（drawLocationMarker），无竞争
    markerIconRgba_ = std::move(rgba);
    markerIconW_ = w;
    markerIconH_ = h;
    markerIconTex_.release(); // 旧纹理失效，下一帧据新 markerIconRgba_ 重传
}

void Renderer::drawLocationMarker(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    bool visible;
    double lon, lat, heading;
    {
        std::lock_guard<std::mutex> lock(markerMtx_);
        visible = markerVisible_;
        lon = markerLon_;
        lat = markerLat_;
        heading = markerHeading_;
    }
    if (!visible || viewportWidth_ <= 0) return;

    // 地理坐标 → 归一化世界坐标，再减去相机中心（与瓦片同一 RTC 口径）
    double mwx = 0.0, mwy = 0.0;
    MercatorProjection::lonLatToWorld(lon, lat, mwx, mwy);
    const float cx = static_cast<float>(mwx - cwx);
    const float cy = static_cast<float>(mwy - cwy);

    // 屏幕固定尺寸：世界单位/像素 = 2*hw / viewportWidth（hw/hh 与视口同比，x/y 一致）；半径按屏幕密度 dp→px
    const double worldPerPx = (2.0 * hw) / viewportWidth_;
    const double density = nav_.displayDensity();
    const float ringR = static_cast<float>(worldPerPx * kMarkerRingRadiusDp * density);
    const float dotR = static_cast<float>(worldPerPx * kMarkerDotRadiusDp * density);
    if (ringR <= 0.0f) return;

    colorProgram_.use();
    glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
    glUniform2f(cUOffset_, 0.0f, 0.0f); // 定位标记顶点已烘焙相机相对坐标，无需 RTC 偏移
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
    glEnableVertexAttribArray(cAPos_);
    glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(cAColor_);
    glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(2 * sizeof(float)));

    // 先画罗盘图标（或蓝点白边圆），再画移动方向箭头（箭头在最上层，不被图标遮盖）。
    // 方位角顺时针自北；世界坐标中北=-y、东=+x（wy 向南增长），故方向向量 dir=(sinθ, -cosθ)。
    float iconHalfW = 0.0f, iconHalfH = 0.0f;

    // 有罗盘图标时画纹理四边形替代蓝点（对齐原主界面 LocationModel ic_compass 方式）；
    // 无图标时回退经典蓝点白边圆
    if (!markerIconRgba_.empty() && markerIconW_ > 0 && markerIconH_ > 0) {
        // 懒上传/上下文重建后重传罗盘图标纹理（GL 线程；CPU 像素构造后只读，无竞争）
        if (!markerIconTex_.isValid() &&
            !markerIconTex_.uploadRGBA(markerIconRgba_.data(), markerIconW_, markerIconH_)) {
            // 上传失败回退蓝点
            goto drawBlueDot;
        }
        // 图标屏幕固定尺寸：原始像素 × kMarkerIconScale × density → 世界半宽/半高
        iconHalfW = static_cast<float>(markerIconW_ * kMarkerIconScale * density * worldPerPx * 0.5);
        iconHalfH = static_cast<float>(markerIconH_ * kMarkerIconScale * density * worldPerPx * 0.5);
        if (iconHalfW <= 0.0f || iconHalfH <= 0.0f) goto drawBlueDot;
        // 中心锚点四边形（与 drawVectorIcons 同口径：顶点存中心 + 角点属性，uHalf 每帧传尺寸；
        // 顶点色白=纯图标，v=0 图像顶行 → 正立）。定位标记仅单四边形，仍走动态 vboIcon_。
        std::vector<float> quad;
        quad.reserve(6 * kIconFloatsPerVertex);
        pushIconVert(quad, cx, cy, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        // 切换到 icon 程序绘制罗盘图标（纹理 × 白色 = 纯图标）
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        iconProgram_.use();
        glUniformMatrix4fv(ixUMvp_, 1, GL_FALSE, ortho.data());
        glUniform2f(ixUOffset_, 0.0f, 0.0f); // 定位标记中心已烘焙相机相对坐标，无需 RTC 偏移
        glUniform2f(ixUHalf_, iconHalfW, iconHalfH);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(ixUTexture_, 0);
        markerIconTex_.bind();
        glBindBuffer(GL_ARRAY_BUFFER, vboIcon_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(quad.size() * sizeof(float)),
                     quad.data(), GL_DYNAMIC_DRAW);
        const GLsizei istride = kIconFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(ixAPos_);
        glVertexAttribPointer(ixAPos_, 2, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(iACorner_);
        glVertexAttribPointer(iACorner_, 2, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glEnableVertexAttribArray(ixATexCoord_);
        glVertexAttribPointer(ixATexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(4 * sizeof(float)));
        glEnableVertexAttribArray(ixAColor_);
        glVertexAttribPointer(ixAColor_, 4, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(6 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(ixAPos_);
        glDisableVertexAttribArray(iACorner_);
        glDisableVertexAttribArray(ixATexCoord_);
        glDisableVertexAttribArray(ixAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
drawBlueDot:
        // 无罗盘图标：回退经典蓝点白边圆（color 程序已绑定、顶点属性已启用）
        auto drawFan = [&](float radius, float r, float g, float b) {
            std::vector<float> verts;
            verts.reserve(static_cast<size_t>(kMarkerSegments + 2) * kColorFloatsPerVertex);
            pushColorVertex(verts, cx, cy, r, g, b); // 扇心
            for (int i = 0; i <= kMarkerSegments; ++i) {
                const float a = static_cast<float>(2.0 * 3.14159265358979 * i / kMarkerSegments);
                pushColorVertex(verts, cx + radius * std::cos(a), cy + radius * std::sin(a), r, g, b);
            }
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size() / kColorFloatsPerVertex));
        };
        drawFan(ringR, 1.0f, 1.0f, 1.0f);     // 白边
        drawFan(dotR, 0.20f, 0.47f, 0.96f);   // 蓝心（≈ #3378F5）
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 移动方向箭头（画在图标/蓝点之上，若 heading>=0）：0.2.8 主界面 createHeadingArrowIcon 同款
    // 凹口箭头（箭尖/两底角/凹口四顶点，两三角形填充），以定位点为中心固定 dp 尺寸，
    // 不随罗盘图标放大（旧版按图标尺寸拉伸导致三角过大）。
    if (heading >= 0.0) {
        const float th = static_cast<float>(heading * 3.14159265358979 / 180.0);
        const float dirX = std::sin(th);
        const float dirY = -std::cos(th);
        const float perpX = std::cos(th);  // 与 dir 垂直（dir·perp = sinθcosθ - cosθsinθ = 0）
        const float perpY = std::sin(th);
        const double dpPx = density * worldPerPx; // dp→px→世界单位
        const float fwd = static_cast<float>(kArrowTipDp * dpPx);   // 箭尖（前）
        const float back = static_cast<float>(kArrowBackDp * dpPx); // 底角（后）
        const float half = static_cast<float>(kArrowHalfDp * dpPx); // 底角横向半宽
        const float notch = static_cast<float>(kArrowNotchDp * dpPx); // 凹口（后）
        const float tipX = cx + dirX * fwd, tipY = cy + dirY * fwd;   // 箭尖
        const float brX = cx - dirX * back + perpX * half,
                    brY = cy - dirY * back + perpY * half;            // 右底角
        const float blX = cx - dirX * back - perpX * half,
                    blY = cy - dirY * back - perpY * half;            // 左底角
        const float nkX = cx - dirX * notch, nkY = cy - dirY * notch; // 凹口
        std::vector<float> arrow;
        arrow.reserve(6 * kColorFloatsPerVertex);
        pushColorVertex(arrow, tipX, tipY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, brX, brY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, nkX, nkY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, tipX, tipY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, nkX, nkY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, blX, blY, kArrowR, kArrowG, kArrowB);
        // 切回 color 程序（icon 程序或蓝点绘制后可能已切换）
        colorProgram_.use();
        glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
        glUniform2f(cUOffset_, 0.0f, 0.0f);
        glEnableVertexAttribArray(cAPos_);
        glEnableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
        glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                              reinterpret_cast<const void *>(0));
        glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(arrow.size() * sizeof(float)),
                     arrow.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_BLEND);
}

void Renderer::uploadVectorChunks(VectorLayer &vl) {
    // 要素组分片流式上传：时间预算内循环推进（每组必成 + 超时即停）。按 geom.featSpans 以要素序
    // 累计 fill+描边+线三组总 floats ≈ 预算为一组，整组一次上传（空段跳过）：同一图斑的填充与
    // 描边/线同帧落块 → 上传途中图斑整块整块长出，不再出现「前半要素带白描边、后半只剩淡填充」的混合态。
    // 三组顶点均按要素序连续追加 → 组内区间 = 首要素起点→尾要素终点（中间零长要素自动跨越），
    // 描边/线沿用 buildStripBatch(fromRange,toRange) 切片合批（CPU 合批与上传同组，免整层临时大带）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUploadFrameBudgetMs);
    const VectorGeometry &geom = vl.geom;
    const size_t nSpans = geom.featSpans.size();
    for (;;) {
        if (vl.uploadFeat >= nSpans) {
            vl.uploadDone = true;
            break;
        }
        // 组界 [uploadFeat, next)：总 floats ≈ 预算；至少含一个要素（巨型单要素超预算也自成一组，
        // 单帧多超有限、保证每帧有进展），越预算即停不拆要素。
        const size_t first = vl.uploadFeat;
        size_t next = first;
        size_t floats = 0;
        do {
            const FeatureSpans &s = geom.featSpans[next];
            size_t add = static_cast<size_t>(s.fillCount);
            for (int i = 0; i < s.outRangeCount; ++i)
                add += static_cast<size_t>(geom.outlineRanges[s.outRangeStart + i].second) * kLineFloatsPerVertex;
            for (int i = 0; i < s.lineRangeCount; ++i)
                add += static_cast<size_t>(geom.lineRanges[s.lineRangeStart + i].second) * kLineFloatsPerVertex;
            if (next > first && floats + add > kUploadChunkFloats) break;
            floats += add;
            ++next;
        } while (next < nSpans && floats < kUploadChunkFloats);
        // 三组顶点按要素序连续追加 → 组切片直接取首尾要素区间（零长要素无数据，跨越无害）
        const FeatureSpans &sF = geom.featSpans[first];
        const FeatureSpans &sL = geom.featSpans[next - 1];
        const size_t fStart = static_cast<size_t>(sF.fillStart);
        const size_t fEnd = static_cast<size_t>(sL.fillStart) + static_cast<size_t>(sL.fillCount);
        if (fEnd > fStart) {
            vl.fillChunks.push_back(
                uploadVectorChunk(geom.fillVerts.data() + fStart, fEnd - fStart, kColorFloatsPerVertex));
        }
        const size_t oStart = static_cast<size_t>(sF.outRangeStart);
        const size_t oEnd = static_cast<size_t>(sL.outRangeStart) + static_cast<size_t>(sL.outRangeCount);
        if (oEnd > oStart) {
            const std::vector<float> batch = buildStripBatch(geom.outlineVerts, geom.outlineRanges, oStart, oEnd);
            vl.outlineChunks.push_back(uploadVectorChunk(batch.data(), batch.size(), kLineFloatsPerVertex));
        }
        const size_t lStart = static_cast<size_t>(sF.lineRangeStart);
        const size_t lEnd = static_cast<size_t>(sL.lineRangeStart) + static_cast<size_t>(sL.lineRangeCount);
        if (lEnd > lStart) {
            const std::vector<float> batch = buildStripBatch(geom.lineVerts, geom.lineRanges, lStart, lEnd);
            vl.lineChunks.push_back(uploadVectorChunk(batch.data(), batch.size(), kLineFloatsPerVertex));
        }
        vl.uploadFeat = next;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    if (vl.uploadDone) {
        vl.glReady = true;
        vl.streamingUpload.store(false);
        // [诊断] 流式上传收尾：chunk 数与总耗时（跨帧累计）；巨层耗时主要在 glBufferData 吞吐。
        LOGI("[VecReload] 流式上传完成: feats=%zu fillChunks=%zu outlineChunks=%zu lineChunks=%zu pts=%zu cost=%lldms",
             nSpans, vl.fillChunks.size(), vl.outlineChunks.size(), vl.lineChunks.size(),
             geom.pointCoords.size() / 2,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - vl.uploadStartedAt).count());
    }
}

void Renderer::resetVectorGl() {
    const int camLevel = nav_.displayLevel();
    for (auto &vlp : vectorLayers_) {
        // 旧上下文的 VBO 名已失效：只放弃句柄不 glDelete，进度重置回首要素，
        // 下一帧据保留的 CPU 几何重新流式上传（与瓦片纹理/VBO 重建同一口径，避免绑失效 VBO 导致崩溃）。
        resetVectorUpload(*vlp, /*del=*/false);
        // 图标纹理属旧上下文：放弃句柄（不 glDelete），保留 CPU 像素，下一帧据 iconRgba 重传
        vlp->iconTex.abandon();
        // 重传即重新接管：有几何待传且「有效可见」（文档可见 + 级别达标）的层保持提示标志
        // （与新几何接管口径一致；级别不足隐藏的层不计入，达标续传后再计入）
        if (!vlp->geom.empty() && vectorLayerShown(*vlp, camLevel)) vlp->streamingUpload.store(true);
    }
}

void Renderer::releaseVectorGl() {
    for (auto &vlp : vectorLayers_) {
        resetVectorUpload(*vlp, /*del=*/true);
        vlp->iconTex.release();
        vlp->streamingUpload.store(false);
    }
}

bool Renderer::drawVectorLayers(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty()) return false;
    bool needRedraw = false;
    // 相机整数级别（级别可见性门控共用一帧一值）：低于 minDisplayLevel 的层整层隐藏，
    // 上传/绘制/提示接管均不生效（与文档 visible 同口径，级别恢复后自然续传）。
    const int camLevel = nav_.displayLevel();
    const GLsizei stride = kColorFloatsPerVertex * sizeof(float);    // 面填充/点：6 floats
    const GLsizei lineStride = kLineFloatsPerVertex * sizeof(float); // 线/描边：8 floats

    for (auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        // 墓碑叠加层：在 GL 线程回收其 VBO/图标纹理/CPU 几何（JNI 线程不可安全释放 GL 资源），随后跳过。
        // 与 releaseVectorGl 同款释放口径，幂等：句柄已清零/纹理已失效时为空操作，可反复进入不重复删除。
        if (vl.dead) {
            // 与 releaseVectorGl 同款释放口径（chunk/图标 VBO + 纹理 + 进度重置），
            // 幂等：句柄已清零/纹理已失效时为空操作，可反复进入不重复删除。
            resetVectorUpload(vl, /*del=*/true);
            vl.iconTex.release();
            vl.streamingUpload.store(false);
            vl.geom = VectorGeometry{};
            continue;
        }
        // 取走后台就绪几何（一次性），并据加载状态判断是否需再画一帧
        if (vl.loader) {
            VectorGeometry g;
            if (vl.loader->drainReady(g)) {
                // 新几何接管（首次 / 静止重载换入）：删旧 chunk VBO 并重置进度（图标静态 VBO 随几何
                // 作废懒重建）；loader 已转 Consumed，由 streamingUpload 接续向 UI 报「仍在工作」
                // 直到全部 chunk 上传完成（提示胶囊覆盖整个流式上传过程）。
                resetVectorUpload(vl, /*del=*/true);
                vl.geom = std::move(g);
                // 仅「有效可见」时立即接管上传并报 working；级别不足/隐藏的层照旧 drain（免 loader 积压），
                // 但不置 streamingUpload（胶囊不误计入），进门后由上传调度自然续跑并接续提示。
                if (!vl.geom.empty() && vectorLayerShown(vl, camLevel)) {
                    vl.uploadStartedAt = std::chrono::steady_clock::now();
                    vl.streamingUpload.store(true);
                }
            }
            if (vl.loader->hasPendingOrReady()) needRedraw = true;
        }
        if (!vectorLayerShown(vl, camLevel)) continue;
        // 流式上传（据保留的 CPU 几何）：每帧时间预算内按要素组推进上传，未传部分按已上传 chunk
        // 绘制 → 画面「整块图斑一片一片」长出；未完成期间持续请求重绘（WHEN_DIRTY 下自触发）。
        // 隐藏层上传挂起，转可见后再续跑（streamingUpload 判定含 visible，提示不误留）。
        if (!vl.uploadDone && !vl.geom.empty()) {
            uploadVectorChunks(vl);
            needRedraw = true;
        }
        if (vl.geom.empty()) continue;

        // RTC 偏移：图层原点 − 相机中心（double 算后转 float），与瓦片 world−camCenter 口径一致
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // 世界单位/像素（与点半径同口径）：把像素半宽换算为世界偏移 → 屏幕空间恒定像素线宽
        const float worldPerPx = viewportWidth_ > 0 ? static_cast<float>((2.0 * hw) / viewportWidth_) : 0.0f;

        // 1) 面填充（color 程序，GL_TRIANGLES）：逐已上传 chunk 循环画（重绑 buffer 后须重设顶点属性指针）
        if (!vl.fillChunks.empty()) {
            colorProgram_.use();
            glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(cUOffset_, offX, offY);
            glEnableVertexAttribArray(cAPos_);
            glEnableVertexAttribArray(cAColor_);
            for (const auto &c : vl.fillChunks) {
                glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(2 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, c.vertexCount);
            }
            glDisableVertexAttribArray(cAPos_);
            glDisableVertexAttribArray(cAColor_);
        }

        // 2)+3) 面描边 + 线要素（line 程序，GL_TRIANGLE_STRIP，屏幕空间等宽 + miter 接头）：
        // 每 chunk 已在上传时 degenerate 合批为单条带，此处逐 chunk 画（程序/uniform 每层仅设一次）
        if (!vl.outlineChunks.empty() || !vl.lineChunks.empty()) {
            lineProgram_.use();
            glUniformMatrix4fv(lUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(lUOffset_, offX, offY);
            glEnableVertexAttribArray(lAPos_);
            glEnableVertexAttribArray(lAMiter_);
            glEnableVertexAttribArray(lAColor_);
            const auto drawStrips = [&](const std::vector<VectorLayer::GlChunk> &chunks, float halfPx) {
                glUniform1f(lUHalfWidthWorld_, halfPx * worldPerPx);
                for (const auto &c : chunks) {
                    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                    glVertexAttribPointer(lAPos_, 2, GL_FLOAT, GL_FALSE, lineStride, reinterpret_cast<const void *>(0));
                    glVertexAttribPointer(lAMiter_, 2, GL_FLOAT, GL_FALSE, lineStride,
                                          reinterpret_cast<const void *>(2 * sizeof(float)));
                    glVertexAttribPointer(lAColor_, 4, GL_FLOAT, GL_FALSE, lineStride,
                                          reinterpret_cast<const void *>(4 * sizeof(float)));
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, c.vertexCount);
                }
            };
            drawStrips(vl.outlineChunks, vl.style.outlineWidth * 0.5f);
            drawStrips(vl.lineChunks, vl.style.lineWidth * 0.5f);
            glDisableVertexAttribArray(lAPos_);
            glDisableVertexAttribArray(lAMiter_);
            glDisableVertexAttribArray(lAColor_);
        }

        // 4) 点要素：无图标层画屏幕固定圆（color 程序，复用 vboDynamic_，相对坐标 + uOffset，与定位标记同口径）；
        // 有图标层由 drawVectorIcons 以 billboard 渲染，此处跳过避免圆与图标重叠。
        // 批量口径：全部点展开为 GL_TRIANGLES 单缓冲，每层仅 1 次 glBufferData + 1 次 draw
        //（原逐点 fan.clear+上传+draw，2000 点≈2000 次上传/draw，是点层卡顿主因）。
        // 点数据量小、无独立 VBO（逐帧动态展开），保持「全部上传完成后才显」口径（原 glReady 门控等价）。
        if (vl.glReady && vl.iconRgba.empty() && !vl.geom.pointCoords.empty() && viewportWidth_ > 0) {
            const float radius = static_cast<float>(worldPerPx * vl.style.pointRadiusDp * nav_.displayDensity());
            if (radius > 0.0f) {
                // 单位圆顶点表（一次构建跨帧复用）：免每点每帧 cos/sin
                static const std::vector<std::pair<float, float>> unitCircle = [] {
                    std::vector<std::pair<float, float>> pts;
                    pts.reserve(kMarkerSegments + 1);
                    for (int s = 0; s <= kMarkerSegments; ++s) {
                        const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                        pts.emplace_back(std::cos(a), std::sin(a));
                    }
                    return pts;
                }();
                colorProgram_.use();
                glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
                glUniform2f(cUOffset_, offX, offY);
                glEnableVertexAttribArray(cAPos_);
                glEnableVertexAttribArray(cAColor_);
                glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
                glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(2 * sizeof(float)));
                std::vector<float> tris;
                tris.reserve(vl.geom.pointCoords.size() / 2 * kMarkerSegments * 3 * kColorFloatsPerVertex);
                for (size_t i = 0; i + 1 < vl.geom.pointCoords.size(); i += 2) {
                    const float px = vl.geom.pointCoords[i];
                    const float py = vl.geom.pointCoords[i + 1];
                    for (int s = 0; s < kMarkerSegments; ++s) {
                        pushColorVertexA(tris, px, py,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                        pushColorVertexA(tris, px + radius * unitCircle[s].first, py + radius * unitCircle[s].second,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                        pushColorVertexA(tris, px + radius * unitCircle[s + 1].first,
                                         py + radius * unitCircle[s + 1].second,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                    }
                }
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)),
                             tris.data(), GL_DYNAMIC_DRAW);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / kColorFloatsPerVertex));
                glDisableVertexAttribArray(cAPos_);
                glDisableVertexAttribArray(cAColor_);
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_BLEND);
    }
    return needRedraw;
}

void Renderer::setFontPath(const std::string &path) {
    // FontAtlas::load 幂等、内部持锁、纯 CPU（含一次文件 IO），可在任意线程调用（早于 GL 上下文亦可）。
    // 加载后图集为 dirty（尚无字形），首帧排版按需烘焙并上传纹理。
    fontAtlas_.load(path);
}

const VectorLayer::LabelLayout *Renderer::ensureLabelLayout(VectorLayer &vl, size_t idx) {
    const LabelItem &li = vl.geom.labels[idx];
    if (vl.labelLayouts.size() != vl.geom.labels.size())
        vl.labelLayouts.resize(vl.geom.labels.size()); // 标注数变化（图层重载）：扩容/缩容后逐条按 text 判失效
    VectorLayer::LabelLayout &lay = vl.labelLayouts[idx];
    if (lay.text != li.text) {
        // 重建排版（烘焙像素单位）：UTF-8 解码 + 字形查表（按需烘焙，可能置 atlas dirty）。
        // 有码点未取到字形（如字体未加载/图集满）时不标记完成，下帧重试。
        std::vector<uint32_t> cps;
        FontAtlas::decodeUtf8(li.text, cps);
        lay.totalAdv = 0.0f;
        lay.boxes.clear();
        lay.boxes.reserve(cps.size());
        bool allResolved = true;
        for (uint32_t cp : cps) {
            const FontAtlas::Glyph *g = fontAtlas_.glyph(cp);
            VectorLayer::GlyphBox gb;
            if (g != nullptr) {
                gb.adv = g->advanceX;
                if (!g->blank) {
                    gb.x0 = g->x0; gb.y0 = g->y0; gb.x1 = g->x1; gb.y1 = g->y1;
                    gb.u0 = g->u0; gb.v0 = g->v0; gb.u1 = g->u1; gb.v1 = g->v1;
                }
            } else {
                allResolved = false;
            }
            lay.totalAdv += gb.adv;
            lay.boxes.push_back(gb);
        }
        lay.text = (allResolved && !cps.empty()) ? li.text : ""; // 未完成→留空串下帧重试
        if (cps.empty()) lay.text = li.text; // 空文本为确定结果，不反复重建
    }
    if (lay.totalAdv <= 0.0f) return nullptr;
    return &lay;
}

void Renderer::drawVectorLabels(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0) return;
    if (!fontAtlas_.isLoaded() || textProgram_.program() == 0 || vboText_ == 0) return;

    // 世界单位/像素（与点/线同口径）：把像素尺寸换算为世界偏移 → 屏幕恒定字号
    const float worldPerPx = static_cast<float>((2.0 * hw) / viewportWidth_);
    if (worldPerPx <= 0.0f) return;
    const float density = static_cast<float>(nav_.displayDensity());
    const float bakePx = static_cast<float>(fontAtlas_.bakePx());
    const float vCenterPx = (fontAtlas_.ascentPx() + fontAtlas_.descentPx()) * 0.5f; // 视觉中心相对基线（上正）

    // 把一个矢量层的全部标注字形四边形追加到 verts：颜色 (cr,cg,cb,ca)、整体屏幕偏移 (dx,dy) 世界单位。
    // 排版结果按层缓存在 [VectorLayer::labelLayouts]（烘焙像素单位，与缩放/相机无关）：
    // 免每帧全量 UTF-8 解码 + 字形查表（轮廓层每帧重复 9 趟，是标注层卡顿主因）；
    // 仅余每帧必要的坐标变换（乘 ws + 平移）。锚点水平居中、垂直居中于锚点。
    auto appendLayer = [&](std::vector<float> &verts, VectorLayer &vl,
                           float offX, float offY, float dx, float dy,
                           float cr, float cg, float cb, float ca) {
        const VectorStyle &s = vl.style;
        // 期望文字像素高（屏幕恒定）：16dp × density × labelSize；每烘焙像素 → 世界单位
        const float ws = (16.0f * density * s.labelSize) / bakePx * worldPerPx;
        if (ws <= 0.0f) return;
        for (size_t idx = 0; idx < vl.geom.labels.size(); ++idx) {
            const LabelItem &li = vl.geom.labels[idx];
            const VectorLayer::LabelLayout *lay = ensureLabelLayout(vl, idx);
            if (lay == nullptr) continue;
            // 锚点（相机相对世界坐标，含 RTC 偏移与轮廓偏移）+ 世界单位/烘焙像素比例 ws
            emitLabelQuads(*lay, li.x + offX + dx, li.y + offY + dy, ws, vCenterPx,
                           cr, cg, cb, ca, verts);
        }
    };

    // 相机整数级别：级别不足隐藏的层其标注一并隐藏（与填充/描边同进同退，免出现无体只剩字的幽灵标注）。
    const int camLevel = nav_.displayLevel();

    // Pass 1：轮廓（poor-man's：8 方向偏移描边色，仅对启用轮廓的层），画在文字之下
    std::vector<float> outlineVerts;
    const float o = 1.0f * density * worldPerPx; // 轮廓偏移 ≈ 1dp（世界单位）
    static const float kDir[8][2] = {
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };
    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty() || !vl.style.labelOutline) continue;
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);
        for (const auto &d : kDir) {
            appendLayer(outlineVerts, vl, offX, offY, d[0] * o, d[1] * o,
                        vl.style.labelOutlineR, vl.style.labelOutlineG,
                        vl.style.labelOutlineB, vl.style.labelOutlineA);
        }
    }

    // Pass 2：文字主体（全部可见且有标注的层）
    std::vector<float> fillVerts;
    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty()) continue;
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);
        appendLayer(fillVerts, vl, offX, offY, 0.0f, 0.0f,
                    vl.style.labelR, vl.style.labelG, vl.style.labelB, vl.style.labelA);
    }

    if (outlineVerts.empty() && fillVerts.empty()) return;

    // 上传/重传图集：排版期间可能烘焙了新字形（dirty），或上下文重建后 atlasTex_ 已 abandon（invalid）
    if (!atlasTex_.isValid() || fontAtlas_.dirty()) {
        atlasTex_.uploadRGBA(fontAtlas_.pixels(), fontAtlas_.size(), fontAtlas_.size());
        fontAtlas_.clearDirty();
    }
    if (!atlasTex_.isValid()) return;

    textProgram_.use();
    glUniformMatrix4fv(txUMvp_, 1, GL_FALSE, ortho.data());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(txUTexture_, 0);
    atlasTex_.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboText_);
    const GLsizei tstride = kTextFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(txAPos_);
    glEnableVertexAttribArray(txATexCoord_);
    glEnableVertexAttribArray(txAColor_);
    glVertexAttribPointer(txAPos_, 2, GL_FLOAT, GL_FALSE, tstride, reinterpret_cast<const void *>(0));
    glVertexAttribPointer(txATexCoord_, 2, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glVertexAttribPointer(txAColor_, 4, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(4 * sizeof(float)));

    auto drawBuf = [&](const std::vector<float> &buf) {
        if (buf.empty()) return;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / kTextFloatsPerVertex));
    };
    drawBuf(outlineVerts); // 轮廓在下
    drawBuf(fillVerts);    // 文字在上

    glDisableVertexAttribArray(txAPos_);
    glDisableVertexAttribArray(txATexCoord_);
    glDisableVertexAttribArray(txAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

void Renderer::drawVectorIcons(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0) return;
    if (iconProgram_.program() == 0) return;
    // 世界单位/像素（与点/线/标注同口径）：把图标的像素尺寸换算为世界偏移 → 屏幕恒定像素尺寸
    const float worldPerPx = static_cast<float>((2.0 * hw) / viewportWidth_);
    if (worldPerPx <= 0.0f) return;

    iconProgram_.use();
    glUniformMatrix4fv(ixUMvp_, 1, GL_FALSE, ortho.data());
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLsizei stride = kIconFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(ixAPos_);
    glEnableVertexAttribArray(iACorner_);
    glEnableVertexAttribArray(ixATexCoord_);
    glEnableVertexAttribArray(ixAColor_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(ixUTexture_, 0);

    // 相机整数级别：级别不足隐藏的层其点要素图标一并隐藏（与其余绘制通路同口径）。
    const int camLevel = nav_.displayLevel();

    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.glReady == false ||
            vl.geom.pointCoords.empty() || vl.iconRgba.empty()) continue;
        // 懒建静态图标 VBO（首帧 / 几何重载 / 上下文重建后）：顶点存「中心相对坐标 + 角点属性」，
        // 不随相机变化，建一次长期复用（原路径逐帧重建全部四边形 + glBufferData，是图标层卡顿主因）
        if (vl.iconVbo == 0) {
            std::vector<float> verts;
            verts.reserve(vl.geom.pointCoords.size() / 2 * 6 * kIconFloatsPerVertex);
            for (size_t i = 0; i + 1 < vl.geom.pointCoords.size(); i += 2) {
                // 中心锚点（对齐 参考实现 Placemark imageOffset 默认居中）；v=0 对应图像顶行（世界 y 较小侧），
                // 与文本/瓦片同口径 → 图标正立。顶点色白（不染色，片元 texture × color = 纯图标）。
                const float px = vl.geom.pointCoords[i];
                const float py = vl.geom.pointCoords[i + 1];
                pushIconVert(verts, px, py, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            if (verts.empty()) continue;
            glGenBuffers(1, &vl.iconVbo);
            glBindBuffer(GL_ARRAY_BUFFER, vl.iconVbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_STATIC_DRAW);
            vl.iconVertexCount = static_cast<GLsizei>(verts.size() / kIconFloatsPerVertex);
        }
        // 懒上传/上下文重建后重传图标纹理（GL 线程；iconRgba 构造后只读，无竞争）
        if (!vl.iconTex.isValid() &&
            !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH)) {
            continue;
        }
        // 图标屏幕固定尺寸：位图像素（已含密度，由宿主解码矢量 drawable 得到）→ 世界半宽/半高
        const float halfW = vl.iconW * 0.5f * worldPerPx;
        const float halfH = vl.iconH * 0.5f * worldPerPx;
        if (halfW <= 0.0f || halfH <= 0.0f) continue;
        // RTC 偏移在着色器内施加（顶点存相对坐标，免逐帧烘焙）
        glUniform2f(ixUOffset_, static_cast<float>(vl.geom.originWx - cwx),
                    static_cast<float>(vl.geom.originWy - cwy));
        glUniform2f(ixUHalf_, halfW, halfH);
        vl.iconTex.bind();
        // 顶点属性指针记录「设置时绑定的 buffer」，逐层切 VBO 须在绑定后重设
        glBindBuffer(GL_ARRAY_BUFFER, vl.iconVbo);
        glVertexAttribPointer(ixAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
        glVertexAttribPointer(iACorner_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glVertexAttribPointer(ixATexCoord_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(4 * sizeof(float)));
        glVertexAttribPointer(ixAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(6 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, vl.iconVertexCount);
    }

    glDisableVertexAttribArray(ixAPos_);
    glDisableVertexAttribArray(iACorner_);
    glDisableVertexAttribArray(ixATexCoord_);
    glDisableVertexAttribArray(ixAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

// ==================== P2.e 3D 矢量层：分帧流式上传（chunk + 世界 ECEF AABB，绘制侧视锥剔除，口径对齐 2D uploadVectorChunks） ====================

void Renderer::uploadGlobeVecChunks(VectorLayer &vl) {
    if (vl.globeUploadDone) return;
    const VectorGeometry &geom = vl.geom;
    const double ox = geom.originWx;
    const double oy = geom.originWy;
    // 固定锚点 = 几何原点的 ECEF（贴地 alt=0，与相机/高度完全无关）：顶点烘焙为 (ecef − anchor)，纯几何，
    // 平移与缩放都零重建零重传；eye 平移经每帧 uOffset=anchor−eye、贴地高度经 uVecAlt 沿法向在着色器补回
    // （shader 位置 = (ecef−anchor)+(anchor−eye)+normalize(ecef)*uVecAlt = (ecef−eye)+法向*vecAlt，与旧「减 eye +
    //   g2c(alt=vecAlt)」等价；ecef≈aPos+anchor 仅用于求单位法向，float 量级 0.5m 误差对法向无感）。
    const Vec3 anchor = worldXYToCartesian(ox, oy, 0.0);
    vl.globeAnchor = anchor;
    // 本层是否带逐顶点高程：true 时按真实 alt 烘焙 ECEF、并逐顶点传 mode 门控着色器 uVecAlt；
    // false 时并行高程数组皆空，走 alt=0/mode=0（等价旧「整层贴地抬升」，零额外内存/耗时）。
    const bool useElev = geom.hasElevation();

    // 分帧预算：与 2D 同「每组必成 + 超时即停」——每帧至少完整推进一个要素组（或点组）再查
    // deadline，超了让位本帧，由调用侧（renderGlobeVectors3D Pass 1）置 needRedraw 下帧续传。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUploadFrameBudgetMs);
    const size_t nSpans = geom.featSpans.size();

    // ---- 路 A：要素组流式上传（按 geom.featSpans 推进，同要素 fill/描边/线同组落块 → 图斑整块长出）----
    for (;;) {
        if (vl.globeUploadFeat >= nSpans) break;
        // 组界 [first, next)：按源(2D RTC) floats 累计 ≈ kGlobeChunkSrcFloats；至少含一个要素
        //（巨型单要素超预算也自成一组，保证每帧有进展），越预算即停不拆要素。
        const size_t first = vl.globeUploadFeat;
        size_t next = first;
        size_t srcFloats = 0;
        do {
            const FeatureSpans &s = geom.featSpans[next];
            size_t add = static_cast<size_t>(s.fillCount);
            for (int i = 0; i < s.outRangeCount; ++i)
                add += static_cast<size_t>(geom.outlineRanges[s.outRangeStart + i].second) * kLineFloatsPerVertex;
            for (int i = 0; i < s.lineRangeCount; ++i)
                add += static_cast<size_t>(geom.lineRanges[s.lineRangeStart + i].second) * kLineFloatsPerVertex;
            if (next > first && srcFloats + add > kGlobeChunkSrcFloats) break;
            srcFloats += add;
            ++next;
        } while (next < nSpans && srcFloats < kGlobeChunkSrcFloats);
        const FeatureSpans &sF = geom.featSpans[first];
        const FeatureSpans &sL = geom.featSpans[next - 1];

        // fill: [x,y,z,r,g,b,a] GL_TRIANGLES——组切片逐顶点转 (ECEF − anchor)，同步累加世界 ECEF AABB
        const size_t fStart = static_cast<size_t>(sF.fillStart);
        const size_t fEnd = static_cast<size_t>(sL.fillStart) + static_cast<size_t>(sL.fillCount);
        if (fEnd > fStart) {
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((fEnd - fStart) / kColorFloatsPerVertex * kGlobeFillFloatsPerVertex);
            for (size_t i = fStart; i + (size_t)kColorFloatsPerVertex - 1 < fEnd; i += kColorFloatsPerVertex) {
                const size_t vi = i / kColorFloatsPerVertex;
                const float alt = useElev ? geom.fillAlt[vi] : 0.0f;
                const float md = useElev ? geom.fillMode[vi] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.fillVerts[i]),
                                                  oy + static_cast<double>(geom.fillVerts[i + 1]), alt);
                globeAabbExtend(bb, p);
                pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                                  static_cast<float>(p.y - anchor.y),
                                  static_cast<float>(p.z - anchor.z), geom.fillVerts[i + 2],
                                  geom.fillVerts[i + 3], geom.fillVerts[i + 4], geom.fillVerts[i + 5], md);
            }
            // 硬上限安全网：触顶丢块但游标照常推进（预算已随几何有界，此处仅防程序错误）
            if (vl.globeFillChunks.size() < kGlobeMaxChunks)
                vl.globeFillChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
        }

        // stroke (outline / line): [pPrev3, pCur3, pNext3, side, color4] GL_TRIANGLE_STRIP——从 2D miter
        // 三角带反推 corner 中心序列（每角 2 顶点同 (x,y)，side=±(mx,my)），逐 range 转换后组内合批：
        // 相邻 range 间插「上段末顶点 + 本段首顶点」两重复顶点退化桥（同 2D buildStripBatch 配方），
        // 整 chunk 单条 strip 一次 draw（替代旧逐 range draw：10 万图斑 = 每帧 10 万次 draw call）。
        // 桥三角形恒含两个 gl_Position 完全相同的顶点 → 零面积；stroke 着色器的 cur 塌缩只移动第三个
        // 顶点救不活它，故旧配方「末顶点点重复×2」的跨 range 斜穿撕裂不会复发。
        // 闭合环检测/封口/端点退化沿用旧版：首末 corner 同点规为 closed，只发 cornerN-1 个角再重复
        // 角 0 封口、prev/next 环绕；开放线端点 prev/next 退化到 cur。
        auto buildStrokeGroup = [&](const std::vector<float> &srcVerts,
                                    const std::vector<std::pair<int, int>> &ranges,
                                    size_t fromRange, size_t toRange,
                                    const std::vector<float> &altArr,
                                    const std::vector<float> &modeArr,
                                    std::vector<VectorLayer::GlobeChunk> &chunks) {
            std::vector<float> verts;
            std::vector<float> rangeVerts;
            std::vector<Vec3> pts; // 单 range 的 corner 序列（逐 range 复用）
            double bb[6];
            globeAabbInit(bb);
            bool firstRange = true;
            for (size_t ri = fromRange; ri < toRange; ++ri) {
                const int firstVert = ranges[ri].first;
                const int vertCount = ranges[ri].second;
                const int cornerN = vertCount / 2;
                if (cornerN < 2) continue; // 退化段（零面积要素的空 range）：整体跳过
                rangeVerts.clear();
                const float *vFirst = srcVerts.data() + size_t(firstVert) * kLineFloatsPerVertex;
                const float *vLast = srcVerts.data() + size_t(firstVert + 2 * (cornerN - 1)) * kLineFloatsPerVertex;
                const bool closed = (cornerN >= 3) &&
                                    (vFirst[0] == vLast[0]) && (vFirst[1] == vLast[1]);
                const int emitN = closed ? (cornerN - 1) : cornerN;
                pts.resize(static_cast<size_t>(cornerN));
                for (int c = 0; c < cornerN; ++c) {
                    const int vertIdx = firstVert + 2 * c; // 每角首顶点（角两顶点共享同高程）
                    const float *v = srcVerts.data() + size_t(vertIdx) * kLineFloatsPerVertex;
                    const float alt = useElev ? altArr[vertIdx] : 0.0f;
                    const Vec3 p = worldXYToCartesian(ox + static_cast<double>(v[0]),
                                                      oy + static_cast<double>(v[1]), alt);
                    globeAabbExtend(bb, p);
                    pts[c] = Vec3{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                }
                // 闭合环封口：strip 线性序列 n 角只生 n-1 段 ribbon，缺「末角→首角」封口边；闭合时
                // 额外再发角 0（与角 0 同三元组 → 带尾四边形把 pts[n-1] 连回 pts[0]），对齐 2D 口径。
                const int emitCount = closed ? (emitN + 1) : emitN;
                for (int c = 0; c < emitCount; ++c) {
                    const int ci = closed ? (c % emitN) : c; // 闭合的最后一帧 c==emitN → ci=0（重复首角）
                    const Vec3 &cur = pts[ci];
                    Vec3 prevV, nextV;
                    if (closed) {
                        prevV = (ci > 0) ? pts[ci - 1] : pts[emitN - 1];   // 开头环回到倒数第二个真实角
                        nextV = (ci + 1 < emitN) ? pts[ci + 1] : pts[0];   // 末尾环回到角 0
                    } else {
                        prevV = (ci > 0) ? pts[ci - 1] : cur;              // 开放线：退化到 cur（shader AB=0→同 BC）
                        nextV = (ci + 1 < emitN) ? pts[ci + 1] : cur;
                    }
                    const int vertIdx = firstVert + 2 * ci;
                    const float *v = srcVerts.data() + size_t(vertIdx) * kLineFloatsPerVertex;
                    const float md = useElev ? modeArr[vertIdx] : 0.0f;
                    pushGlobeStrokeVert(rangeVerts, prevV, cur, nextV, +1.0f, v[4], v[5], v[6], v[7], md);
                    pushGlobeStrokeVert(rangeVerts, prevV, cur, nextV, -1.0f, v[4], v[5], v[6], v[7], md);
                }
                if (rangeVerts.empty()) continue;
                verts.reserve(verts.size() + rangeVerts.size() + 2u * kGlobeStrokeFloatsPerVertex);
                if (!firstRange) {
                    // 退化桥：复制「上段末顶点（verts 尾部）+ 本段首顶点（rangeVerts 头部）」。用
                    // push_back 尾索引读取（读恒落旧元素范围，即便扩容中途也安全），不用自范围 insert。
                    const size_t prevLast = verts.size() - kGlobeStrokeFloatsPerVertex;
                    for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k)
                        verts.push_back(verts[prevLast + k]);
                    for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k)
                        verts.push_back(rangeVerts[k]);
                }
                verts.insert(verts.end(), rangeVerts.begin(), rangeVerts.end());
                firstRange = false;
            }
            if (!verts.empty() && chunks.size() < kGlobeMaxChunks)
                chunks.push_back(uploadGlobeChunk(verts, bb, kGlobeStrokeFloatsPerVertex));
        };
        // 描边/线组切片 = 首末要素的 range 区间（零长要素无数据，跨越无害，同 2D 口径）
        const size_t oStart = static_cast<size_t>(sF.outRangeStart);
        const size_t oEnd = static_cast<size_t>(sL.outRangeStart) + static_cast<size_t>(sL.outRangeCount);
        buildStrokeGroup(geom.outlineVerts, geom.outlineRanges, oStart, oEnd,
                         geom.outlineAlt, geom.outlineMode, vl.globeOutlineChunks);
        const size_t lStart = static_cast<size_t>(sF.lineRangeStart);
        const size_t lEnd = static_cast<size_t>(sL.lineRangeStart) + static_cast<size_t>(sL.lineRangeCount);
        buildStrokeGroup(geom.lineVerts, geom.lineRanges, lStart, lEnd,
                         geom.lineAlt, geom.lineMode, vl.globeLineChunks);
        vl.globeUploadFeat = next;
        if (std::chrono::steady_clock::now() >= deadline) break; // 超时即停：已成组不回退
    }

    // ---- 路 B：点圆 / 图标流式（两路互斥，共用 globeUploadPoint 游标逐点推进）----
    // 点圆：每点展开 kMarkerSegments 个三角形（圆心 + 相邻两环点），pos = 点 ECEF − anchor、corner = 单位
    // 方向，屏幕半径/贴地高度每帧 uniform → 顶点纯几何，平移缩放零重建（口径同旧）。
    const size_t ptN = geom.pointCoords.size() / 2;
    if (vl.iconRgba.empty()) {
        static const std::vector<std::pair<float, float>> unitCircle = [] {
            std::vector<std::pair<float, float>> pts;
            pts.reserve(kMarkerSegments + 1);
            for (int s = 0; s <= kMarkerSegments; ++s) {
                const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                pts.emplace_back(std::cos(a), std::sin(a));
            }
            return pts;
        }();
        const float pr = vl.style.pointR, pg = vl.style.pointG, pb = vl.style.pointB, pa = vl.style.pointA;
        while (vl.globeUploadPoint < ptN) {
            const size_t ptFirst = vl.globeUploadPoint;
            const size_t ptEnd = std::min(ptFirst + kGlobeChunkPoints, ptN);
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((ptEnd - ptFirst) * kMarkerSegments * 3 * kGlobePointFloatsPerVertex);
            for (size_t i = ptFirst; i < ptEnd; ++i) {
                const float alt = useElev ? geom.pointAlt[i] : 0.0f;
                const float md = useElev ? geom.pointMode[i] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.pointCoords[2 * i]),
                                                  oy + static_cast<double>(geom.pointCoords[2 * i + 1]), alt);
                globeAabbExtend(bb, p);
                const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                for (int s = 0; s < kMarkerSegments; ++s) {
                    pushGlobePointVert(verts, c, 0.0f, 0.0f, pr, pg, pb, pa, md);
                    pushGlobePointVert(verts, c, unitCircle[s].first, unitCircle[s].second, pr, pg, pb, pa, md);
                    pushGlobePointVert(verts, c, unitCircle[s + 1].first, unitCircle[s + 1].second, pr, pg, pb, pa, md);
                }
            }
            vl.globeUploadPoint = ptEnd;
            if (vl.globePointChunks.size() < kGlobeMaxChunks)
                vl.globePointChunks.push_back(uploadGlobeChunk(verts, bb, kGlobePointFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    } else {
        // 图标 billboard [pos3, corner2, uv2, color4]：每点 2 三角形 6 顶点（角点∈[-1,1]，uv v=0 配
        // corner.y=+1 保正立），仅 iconRgba 非空层进入（与点圆互斥，分流口径同旧版）。
        while (vl.globeUploadPoint < ptN) {
            const size_t ptFirst = vl.globeUploadPoint;
            const size_t ptEnd = std::min(ptFirst + kGlobeChunkPoints, ptN);
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((ptEnd - ptFirst) * 6 * kGlobeIconFloatsPerVertex);
            for (size_t i = ptFirst; i < ptEnd; ++i) {
                const float alt = useElev ? geom.pointAlt[i] : 0.0f;
                const float md = useElev ? geom.pointMode[i] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.pointCoords[2 * i]),
                                                  oy + static_cast<double>(geom.pointCoords[2 * i + 1]), alt);
                globeAabbExtend(bb, p);
                const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                pushGlobeIconVert(verts, c, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左上
                pushGlobeIconVert(verts, c, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md);  // 右上
                pushGlobeIconVert(verts, c, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 右下
                pushGlobeIconVert(verts, c, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左上
                pushGlobeIconVert(verts, c, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 右下
                pushGlobeIconVert(verts, c, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左下
            }
            vl.globeUploadPoint = ptEnd;
            if (vl.globeIconChunks.size() < kGlobeMaxChunks)
                vl.globeIconChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeIconFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    // ---- 路 C：extrude 侧墙流式（独立游标 globeUploadExtrude，按 geom.extrudeVerts float 数推进）----
    // 布局同 fill：[x,y,r,g,b,a] + 并行 alt/mode → 逐顶点 worldXYToCartesian(alt) 烘 ECEF−anchor，
    // mode 恒 1（baked 真实高程，不叠加 uVecAlt：墙底 alt0 落地面、墙顶落真实高程）。数据量通常很小
    //（一栋建筑几十顶点），仍按 kGlobeChunkSrcFloats 分块保证大数据不冻结单帧。
    {
        const size_t exN = geom.extrudeVerts.size();
        while (vl.globeUploadExtrude < exN) {
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            size_t i = vl.globeUploadExtrude;
            size_t budget = 0;
            for (; i + (size_t)kColorFloatsPerVertex - 1 < exN; i += kColorFloatsPerVertex) {
                const size_t vi = i / kColorFloatsPerVertex;
                const float alt = geom.extrudeAlt[vi];
                const float md = geom.extrudeMode[vi];
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.extrudeVerts[i]),
                                                  oy + static_cast<double>(geom.extrudeVerts[i + 1]), alt);
                globeAabbExtend(bb, p);
                pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                                  static_cast<float>(p.y - anchor.y),
                                  static_cast<float>(p.z - anchor.z), geom.extrudeVerts[i + 2],
                                  geom.extrudeVerts[i + 3], geom.extrudeVerts[i + 4], geom.extrudeVerts[i + 5], md);
                budget += kColorFloatsPerVertex;
                if (budget >= kGlobeChunkSrcFloats) break;
            }
            vl.globeUploadExtrude = i;
            if (!verts.empty() && vl.globeExtrudeChunks.size() < kGlobeMaxChunks)
                vl.globeExtrudeChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    // ---- 完成判定：要素组与点组两游标均到尾（不动 2D uploadDone/glReady，回 2D 仍按原口径流式）----
    if (vl.globeUploadFeat >= nSpans && vl.globeUploadPoint >= ptN &&
        vl.globeUploadExtrude >= geom.extrudeVerts.size()) {
        vl.globeUploadDone = true;
        // streamingUpload 为两通路共用的 UI 标志：几何接管时置位，此处归零 → 加载胶囊覆盖 3D 流式全程
        vl.streamingUpload.store(false);
        LOGI("[GlobeVec] 3D 流式上传完成: feats=%zu fillChunks=%zu outlineChunks=%zu lineChunks=%zu pointChunks=%zu iconChunks=%zu pts=%zu cost=%lldms",
             nSpans, vl.globeFillChunks.size(), vl.globeOutlineChunks.size(),
             vl.globeLineChunks.size(), vl.globePointChunks.size(), vl.globeIconChunks.size(), ptN,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - vl.uploadStartedAt).count());
    }
}

bool Renderer::renderGlobeVectors3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    if (vectorLayers_.empty()) return false;
    bool needRedraw = false;
    const int camLevel = nav_.displayLevel();
    const float halfVpX = viewportWidth_ > 0 ? static_cast<float>(viewportWidth_) : 1.0f;
    const float halfVpY = viewportHeight_ > 0 ? static_cast<float>(viewportHeight_) : 1.0f;

    // 贴地高度（米）按弦切矢高口径动态取 camAlt²/2e7 钳 [2,500]（缘由见 kGlobeVecAltMeters 注释），
    // 每帧作 uniform uVecAlt 传着色器沿法向抬升（顶点纯几何 alt=0 不含此高度）→ 缩放零重建。
    // 低空仅米级：tilt 斜视下贴地层视觉落在椭球面上，不再「悬浮」。
    const double camAlt = (cam.eye - cam.center).length();
    double rawVecAlt = camAlt * camAlt / 2.0e7;
    if (rawVecAlt > kGlobeVecAltMeters) rawVecAlt = kGlobeVecAltMeters;
    if (rawVecAlt < 2.0) rawVecAlt = 2.0;
    const float vecAlt = static_cast<float>(rawVecAlt);

    // Pass 1：逐可见矢量层 drainReady → P2.e 分帧流式上传（未完每帧推进一个预算片并续画一帧）
    for (auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (vl.dead) continue; // 墓碑层交给 2D drawVectorLayers 回收（同一条 resetVectorUpload）
        if (vl.loader) {
            VectorGeometry g;
            if (vl.loader->drainReady(g)) {
                resetVectorUpload(vl, /*del=*/true); // 2D/3D chunk 与游标一并回收，下帧起据新 geom 流式上传
                vl.geom = std::move(g);
                if (!vl.geom.empty() && vectorLayerShown(vl, camLevel)) {
                    vl.uploadStartedAt = std::chrono::steady_clock::now();
                    vl.streamingUpload.store(true);
                }
            }
            if (vl.loader->hasPendingOrReady()) needRedraw = true;
        }
        if (!vectorLayerShown(vl, camLevel) || vl.geom.empty()) continue;
        // P2.e 分帧流式上传：未完成每帧推进一个预算片并请求续绘（图斑整块整块长出）；
        // 完成后顶点纯几何（相对固定锚点、alt=0 烘焙），平移与缩放均零重建零重传。
        if (!vl.globeUploadDone) {
            uploadGlobeVecChunks(vl);
            needRedraw = true;
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    // 参照 参考实现 DrawableShape 的 enableDepthWrite=false + 画家序原理：矢量层之间**关深度写入、开深度测试**。
    // 深度测试仍对着地球瓦片网格 → 背半球/穿地球的矢量照样被挡；但矢量彼此不写深度，重叠区不再逐像素
    // 随机胜负（旧 bug：两层 fill 压到同一 vecAlt 面且都写深度 → GL_LEQUAL 共面 fight），改为纯按图层
    // 加入顺序后画覆盖前画合成。fill 的 polygonOffset(+2) 仅作 fill 相对瓦片的稳定偏置（stroke 因不测
    // 矢量深度恒在 fill 上，双保险）。
    glDepthMask(GL_FALSE);

    // Pass 2：fill（GL_TRIANGLES，开 polygon offset）
    if (globeFillProgram_.program() != 0) {
        globeFillProgram_.use();
        glUniformMatrix4fv(gfUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glEnable(GL_POLYGON_OFFSET_FILL);
        // 方向关键：GL 深度 0=near 1=far，polygonOffset 把 (factor*slope + units*min_res) **加**到
        // 片元深度上。fill 与 stroke 同 alt=500m，Pass 2 先画 fill 写 depth=fill_actual+offset，
        // Pass 3 后画 stroke 用 GL_LEQUAL 比较 stroke_actual ≤ stored 才通得过——必须 stored 稍
        // 远（正 offset），否则 stroke 被自己那层 fill 挡回去（现象：多边形 interior 描边全丢，
        // 只剩最外圈贴瓦片的那条边可见，看起来像 "要素撕裂")。
        // 之前用 (-1, -1) 意图 "fill 推得更近避免与瓦片 z-fight" 是错的方向：fill 已经在 alt=500m
        // 远高于瓦片 alt=0，本就不需负 offset 兜底；反而害了 stroke。改成 (+2, +2)：正 push 让 fill
        // 稍远，stroke 稳过；量级 2 units + 2*slope 远小于 500m alt 对应的深度差，不会被瓦片吃回。
        glPolygonOffset(2.0f, 2.0f);
        const GLsizei stride = kGlobeFillFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gfAPos_);
        glEnableVertexAttribArray(gfAColor_);
        glEnableVertexAttribArray(gfAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel) || vl.globeFillChunks.empty()) continue;
            glUniform3f(gfUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gfUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gfUVecAlt_, vecAlt);
            // 逐 chunk + 世界 ECEF AABB 对 Gribb 6 面保守剔除（含 uVecAlt 抬升余量）：
            // 背半球/视外远处 chunk 不进 draw call，拉近俯仰视角下收益最大
            for (const auto &ch : vl.globeFillChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gfAPos_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gfAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gfAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisableVertexAttribArray(gfAPos_);
        glDisableVertexAttribArray(gfAColor_);
        glDisableVertexAttribArray(gfAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 2b：extrude 侧墙（GL_TRIANGLES，复用 fill 程序/布局）。与 flat 填充唯一区别：开【深度写入】，
    // 使同一盒体近墙遮挡远墙 → 实心不外窥（修「凹型」）。深度测试本就对着地球瓦片网格开启，背面
    // 半球/穿地的墙自然被挡；画完立即把 depthMask 复位 FALSE，后续 stroke/point 画家序合成不变。
    if (globeFillProgram_.program() != 0) {
        globeFillProgram_.use();
        glUniformMatrix4fv(gfUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glDepthMask(GL_TRUE); // 仅此 pass 写深度（当前深度函数 GL_LEQUAL、深度测试已开，与瓦片同口径）
        const GLsizei stride = kGlobeFillFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gfAPos_);
        glEnableVertexAttribArray(gfAColor_);
        glEnableVertexAttribArray(gfAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel) || vl.globeExtrudeChunks.empty()) continue;
            glUniform3f(gfUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gfUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gfUVecAlt_, vecAlt);
            for (const auto &ch : vl.globeExtrudeChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gfAPos_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gfAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gfAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(gfAPos_);
        glDisableVertexAttribArray(gfAColor_);
        glDisableVertexAttribArray(gfAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDepthMask(GL_FALSE); // 复位：与 fill/stroke/point 画家序保持一致
    }

    // Pass 3：outline + line（GL_TRIANGLE_STRIP，不开 polygon offset；miter 靠 shader 屏幕空间）
    if (globeStrokeProgram_.program() != 0) {
        globeStrokeProgram_.use();
        glUniformMatrix4fv(gsUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(gsUViewportPx_, halfVpX, halfVpY);
        glUniform1f(gsUMiterLimit_, kGlobeMiterLimit);
        const GLsizei stride = kGlobeStrokeFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gsAPrev_);
        glEnableVertexAttribArray(gsACur_);
        glEnableVertexAttribArray(gsANext_);
        glEnableVertexAttribArray(gsASide_);
        glEnableVertexAttribArray(gsAColor_);
        glEnableVertexAttribArray(gsAMode_);
        // 逐层画 outline 与 line（各自线宽）；stroke program 内 side 已带符号，线宽以像素传入。
        // P2.e：chunk 内各 range 已在上传期以退化桥合批 → 整 chunk 单次 glDrawArrays（旧逐 range
        // draw 在 10 万图斑层 = 每帧 10 万次 draw call，驱动开销即帧率主瓶颈）。
        auto drawStripChunks = [&](std::vector<VectorLayer::GlobeChunk> &chunks, float halfWidthPx) {
            if (chunks.empty() || halfWidthPx <= 0.0f) return;
            glUniform1f(gsUHalfWidthPx_, halfWidthPx);
            for (const auto &ch : chunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gsAPrev_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gsACur_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gsANext_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(6 * sizeof(float)));
                glVertexAttribPointer(gsASide_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(9 * sizeof(float)));
                glVertexAttribPointer(gsAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(10 * sizeof(float)));
                glVertexAttribPointer(gsAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(14 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, ch.vertexCount);
            }
        };
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            // 描边/线宽与 2D 同口径：style.outlineWidth/lineWidth 直接当像素宽度处理（无 density），
            // 取一半得像素半宽，shader 里乘 side 与 miter 后除视口得 NDC 偏移。
            // 逐层 uOffset = 本层固定锚点 − eye（平移只改此 uniform，不重建顶点）
            glUniform3f(gsUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gsUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gsUVecAlt_, vecAlt);
            const float oHalfPx = static_cast<float>(vl.style.outlineWidth) * 0.5f;
            const float lHalfPx = static_cast<float>(vl.style.lineWidth) * 0.5f;
            drawStripChunks(vl.globeOutlineChunks, oHalfPx);
            drawStripChunks(vl.globeLineChunks, lHalfPx);
        }
        glDisableVertexAttribArray(gsAPrev_);
        glDisableVertexAttribArray(gsACur_);
        glDisableVertexAttribArray(gsANext_);
        glDisableVertexAttribArray(gsASide_);
        glDisableVertexAttribArray(gsAColor_);
        glDisableVertexAttribArray(gsAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 4：点要素（屏幕固定圆 billboard，GL_TRIANGLES）。与 fill/stroke 同处 depthMask=FALSE（彼此
    // 不写深度、画家序合成），GL_DEPTH_TEST 仍开→背面半球点被地球瓦片深度剔除；圆心深度取 vecAlt
    // （与 fill 同高且 fill 未写深度），不会被自家 fill 遮挡。屏幕半径 = pointRadiusDp × density（px）。
    if (globePointProgram_.program() != 0) {
        globePointProgram_.use();
        glUniformMatrix4fv(gpUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(gpUViewportPx_, halfVpX, halfVpY);
        const float density = static_cast<float>(nav_.displayDensity());
        const GLsizei pstride = kGlobePointFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gpAPos_);
        glEnableVertexAttribArray(gpACorner_);
        glEnableVertexAttribArray(gpAColor_);
        glEnableVertexAttribArray(gpAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            if (!vl.iconRgba.empty()) continue; // 有图标层交给图标 billboard，此处不画圆
            if (vl.globePointChunks.empty()) continue;
            const float radiusPx = static_cast<float>(vl.style.pointRadiusDp) * density;
            if (radiusPx <= 0.0f) continue;
            glUniform2f(gpUHalfPx_, radiusPx, radiusPx);
            glUniform3f(gpUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gpUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gpUVecAlt_, vecAlt);
            // AABB 仅含圆心（corner 展开在屏幕空间），剔除余量覆盖屏幕半径量级
            for (const auto &ch : vl.globePointChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gpAPos_, 3, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gpACorner_, 2, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gpAColor_, 4, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(5 * sizeof(float)));
                glVertexAttribPointer(gpAMode_, 1, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(9 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(gpAPos_);
        glDisableVertexAttribArray(gpACorner_);
        glDisableVertexAttribArray(gpAColor_);
        glDisableVertexAttribArray(gpAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 5：点要素图标（屏幕固定像素尺寸 billboard，GL_TRIANGLES）。同处 depthMask=FALSE（彼此画家序、
    // 不写深度），GL_DEPTH_TEST 仍开→背半球图标被地球瓦片剔除。逐层懒上传图标纹理 + 逐 chunk 绘制。
    // 像素半尺寸 = iconW×0.5（位图像素已含密度，与 2D drawVectorIcons 同口径，无额外 density）。
    if (globeIconProgram_.program() != 0) {
        globeIconProgram_.use();
        glUniformMatrix4fv(giUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(giUViewportPx_, halfVpX, halfVpY);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(giUTexture_, 0);
        const GLsizei istride = kGlobeIconFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(giaPos_);
        glEnableVertexAttribArray(giACorner_);
        glEnableVertexAttribArray(giATexCoord_);
        glEnableVertexAttribArray(giAColor_);
        glEnableVertexAttribArray(giaMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            if (vl.iconRgba.empty() || vl.globeIconChunks.empty()) continue;
            // 懒上传/上下文重建后重传图标纹理（与 2D 同口径；iconRgba 构造后只读、无竞争）
            if (!vl.iconTex.isValid() &&
                !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH))
                continue;
            const float halfWpx = static_cast<float>(vl.iconW) * 0.5f;
            const float halfHpx = static_cast<float>(vl.iconH) * 0.5f;
            if (halfWpx <= 0.0f || halfHpx <= 0.0f) continue;
            glUniform2f(giUHalfPx_, halfWpx, halfHpx);
            glUniform3f(giUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(giUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(giUVecAlt_, vecAlt);
            vl.iconTex.bind();
            for (const auto &ch : vl.globeIconChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(giaPos_, 3, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(giACorner_, 2, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(giATexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(5 * sizeof(float)));
                glVertexAttribPointer(giAColor_, 4, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glVertexAttribPointer(giaMode_, 1, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(11 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(giaPos_);
        glDisableVertexAttribArray(giACorner_);
        glDisableVertexAttribArray(giATexCoord_);
        glDisableVertexAttribArray(giAColor_);
        glDisableVertexAttribArray(giaMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE); // 恢复深度写入，交回瓦片/后续通路
    return needRedraw;
}

// 3D 绝对 ECEF→屏幕像素投影 helper（供标注通路；地理坐标入口由调用方自行 g2c 后传入）。
bool Renderer::projectEcef3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc,
                             const Vec3 &ecef, float &outSxPx, float &outSyPx) const {
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return false;
    // RTC：绝对 ECEF 减眼点（double 差后转 float，同瓦片/矢量通路口径），再走 viewProjRtc 到 clip。
    const float x = static_cast<float>(ecef.x - cam.eye.x);
    const float y = static_cast<float>(ecef.y - cam.eye.y);
    const float z = static_cast<float>(ecef.z - cam.eye.z);
    const float *m = viewProjRtc.data(); // 列主序 m[col*4+row]；clip_row_i = Σ_j m[j*4+i]*v_j，v=(x,y,z,1)
    const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-4f) return false; // 相机背后/近平面外
    const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const float ndcx = cx / cw;
    const float ndcy = cy / cw;
    outSxPx = (ndcx * 0.5f + 0.5f) * static_cast<float>(viewportWidth_);
    outSyPx = (0.5f - ndcy * 0.5f) * static_cast<float>(viewportHeight_); // y 向下、原点左上
    return true;
}

void Renderer::drawLocationMarker3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    bool visible;
    double lon, lat, heading;
    {
        std::lock_guard<std::mutex> lock(markerMtx_);
        visible = markerVisible_;
        lon = markerLon_;
        lat = markerLat_;
        heading = markerHeading_;
    }
    if (!visible || viewportHeight_ <= 0) return;

    const double density = nav_.displayDensity();
    const Wgs84Globe &globe = Wgs84Globe::instance();

    // 地表点 ECEF（海拔 0）+ 椭球单位法向（局部「上」）。
    Vec3 ground{0.0, 0.0, 0.0};
    globe.geographicToCartesian(lon, lat, 0.0, ground);
    Vec3 up{0.0, 0.0, 0.0};
    globe.geographicNormal(lon, lat, up);
    up = up.normalized();

    // 背半球剔除：定位点随球面转到地球背面（法向与眼向夹角 > 90°）→ 整块不画，免穿透地球显示。
    if (up.dot(cam.eye - ground) <= 0.0) return;

    // 局部切平面正交基：east = 极轴 × up（+东），north = up × east（+北）。图标沿 east/north 铺成
    // 平行于地表的贴地矩形（指真北），随相机俯仰自然透视压扁（根治旧版正对屏幕的「立面」观感）。
    const Vec3 east = Vec3{0.0, 0.0, 1.0}.cross(up).normalized();
    const Vec3 north = up.cross(east).normalized();

    // 屏幕恒定尺寸换算（对齐 参考实现 pixelSizeAtDistance）：该处「米/像素」= 眼距·2·tan(fov/2)/视口高。
    const double dist = (ground - cam.eye).length();
    const double fovRad = nav_.fieldOfViewDeg() * 3.14159265358979323846 / 180.0;
    const double metersPerPx = dist * 2.0 * std::tan(fovRad * 0.5) / static_cast<double>(viewportHeight_);
    if (!(metersPerPx > 0.0)) return;

    // 引线：图标平面沿法向抬高固定屏幕长度悬于地面点上方 → 斜视显出连线、俯视沿视线坍缩为点不可见。
    const Vec3 center = ground + up * (kMarkerLeaderLenDp * density * metersPerPx);

    // 贴地顶点统一在 CPU 端沿切平面铺成后 double 减眼点转 float，走 viewProjRtc 透视投影。
    auto toRtc = [&](const Vec3 &p) { return p - cam.eye; };

    glDisable(GL_DEPTH_TEST); // 定位标识始终画在最上层（同旧版纯屏幕叠加），收尾复开
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float iconHalfWm = 0.0f, iconHalfHmm = 0.0f;
    bool hasIcon = false;

    // 1) 引线（地面点 → 图标中心，箭蓝色，画在最底）：单条 GL_LINES，俯视沿视线投影为点故不可见。
    {
        std::vector<float> line;
        line.reserve(2 * kGlobeColorFloatsPerVertex);
        pushGlobeColorVert(line, toRtc(ground), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(line, toRtc(center), kArrowR, kArrowG, kArrowB, 1.0f);
        globeColorProgram_.use();
        glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
        glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(line.size() * sizeof(float)),
                     line.data(), GL_DYNAMIC_DRAW);
        const GLsizei lstride = kGlobeColorFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gc3APos_);
        glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, lstride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(gc3AColor_);
        glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, lstride,
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, 2);
        glLineWidth(1.0f);
        glDisableVertexAttribArray(gc3APos_);
        glDisableVertexAttribArray(gc3AColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 2) 图标贴地四边形（有罗盘图标）：图像顶行(v=0)映射 +north → 指真北、贴地正立，随俯仰透视压扁。
    if (!markerIconRgba_.empty() && markerIconW_ > 0 && markerIconH_ > 0) {
        if (!markerIconTex_.isValid()) {
            markerIconTex_.uploadRGBA(markerIconRgba_.data(), markerIconW_, markerIconH_);
        }
        iconHalfWm = static_cast<float>(markerIconW_ * kMarkerIconScale * density * 0.5 * metersPerPx);
        iconHalfHmm = static_cast<float>(markerIconH_ * kMarkerIconScale * density * 0.5 * metersPerPx);
        if (markerIconTex_.isValid() && iconHalfWm > 0.0f && iconHalfHmm > 0.0f) {
            hasIcon = true;
            static const float kBase[6][4] = {
                {-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                {-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 1.0f},
            };
            std::vector<float> quad;
            quad.reserve(6 * kGlobeFlatIconFloatsPerVertex);
            for (int i = 0; i < 6; ++i) {
                const float sx = kBase[i][0], sy = kBase[i][1];
                const Vec3 w = center + east * (static_cast<double>(sx) * iconHalfWm)
                                       + north * (static_cast<double>(-sy) * iconHalfHmm);
                pushGlobeFlatIconVert(quad, toRtc(w), kBase[i][2], kBase[i][3],
                                      1.0f, 1.0f, 1.0f, 1.0f);
            }
            globeFlatIconProgram_.use();
            glUniformMatrix4fv(gfiMvp_, 1, GL_FALSE, viewProjRtc.data());
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(gfiTexture_, 0);
            markerIconTex_.bind();
            glBindBuffer(GL_ARRAY_BUFFER, vboIcon_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(quad.size() * sizeof(float)),
                         quad.data(), GL_DYNAMIC_DRAW);
            const GLsizei istride = kGlobeFlatIconFloatsPerVertex * sizeof(float);
            glEnableVertexAttribArray(gfiPos_);
            glVertexAttribPointer(gfiPos_, 3, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(gfiTexCoord_);
            glVertexAttribPointer(gfiTexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                                  reinterpret_cast<const void *>(3 * sizeof(float)));
            glEnableVertexAttribArray(gfiColor_);
            glVertexAttribPointer(gfiColor_, 4, GL_FLOAT, GL_FALSE, istride,
                                  reinterpret_cast<const void *>(5 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDisableVertexAttribArray(gfiPos_);
            glDisableVertexAttribArray(gfiTexCoord_);
            glDisableVertexAttribArray(gfiColor_);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // 3) 无图标回退：贴地蓝点白边圆（切平面内三角扇，半径 = dp·density·metersPerPx）。
    if (!hasIcon) {
        const float ringRm = static_cast<float>(kMarkerRingRadiusDp * density * metersPerPx);
        const float dotRm = static_cast<float>(kMarkerDotRadiusDp * density * metersPerPx);
        if (ringRm > 0.0f) {
            static const std::vector<std::pair<float, float>> unitCircle = [] {
                std::vector<std::pair<float, float>> pts;
                pts.reserve(kMarkerSegments + 1);
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                    pts.emplace_back(std::cos(a), std::sin(a));
                }
                return pts;
            }();
            globeColorProgram_.use();
            glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
            glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
            glEnableVertexAttribArray(gc3APos_);
            glEnableVertexAttribArray(gc3AColor_);
            const GLsizei cstride = kGlobeColorFloatsPerVertex * sizeof(float);
            auto drawDisc = [&](float radiusM, float r, float g, float b) {
                std::vector<float> verts;
                verts.reserve(static_cast<size_t>(kMarkerSegments + 2) * kGlobeColorFloatsPerVertex);
                pushGlobeColorVert(verts, toRtc(center), r, g, b, 1.0f); // 扇心
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const Vec3 w = center + east * (static_cast<double>(unitCircle[s].first) * radiusM)
                                           + north * (static_cast<double>(unitCircle[s].second) * radiusM);
                    pushGlobeColorVert(verts, toRtc(w), r, g, b, 1.0f);
                }
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                             verts.data(), GL_DYNAMIC_DRAW);
                glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, cstride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, cstride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size() / kGlobeColorFloatsPerVertex));
            };
            drawDisc(ringRm, 1.0f, 1.0f, 1.0f);   // 白边
            drawDisc(dotRm, 0.20f, 0.47f, 0.96f); // 蓝心（≈ #3378F5）
            glDisableVertexAttribArray(gc3APos_);
            glDisableVertexAttribArray(gc3AColor_);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // 4) 移动方向箭头（贴地，画在图标之上，heading>=0）：切平面内沿 heading（顺时针自北）方向的
    //    凹口箭头，几何同 2D drawLocationMarker（0.2.8 createHeadingArrowIcon 同款），以标示中心
    //    固定 dp 尺寸，不随罗盘图标放大。
    if (heading >= 0.0) {
        const double hr = heading * 3.14159265358979323846 / 180.0;
        const Vec3 dirH = north * std::cos(hr) + east * std::sin(hr);
        const Vec3 perpH = up.cross(dirH); // 切平面内垂直（单位）
        const double dpM = density * metersPerPx; // dp→px→米
        const double fwdM = kArrowTipDp * dpM;    // 箭尖（前）
        const double backM = kArrowBackDp * dpM;  // 底角（后）
        const double halfM = kArrowHalfDp * dpM;  // 底角横向半宽
        const double notchM = kArrowNotchDp * dpM; // 凹口（后）
        const Vec3 tipW = center + dirH * fwdM;
        const Vec3 brW = center - dirH * backM + perpH * halfM;
        const Vec3 blW = center - dirH * backM - perpH * halfM;
        const Vec3 nkW = center - dirH * notchM;
        std::vector<float> arrow;
        arrow.reserve(6 * kGlobeColorFloatsPerVertex);
        pushGlobeColorVert(arrow, toRtc(tipW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(brW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(nkW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(tipW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(nkW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(blW), kArrowR, kArrowG, kArrowB, 1.0f);
        globeColorProgram_.use();
        glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
        glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(arrow.size() * sizeof(float)),
                     arrow.data(), GL_DYNAMIC_DRAW);
        const GLsizei astride = kGlobeColorFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gc3APos_);
        glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, astride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(gc3AColor_);
        glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, astride,
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(gc3APos_);
        glDisableVertexAttribArray(gc3AColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); // 交回默认态（onDrawFrame3D 收尾会再关，此处显式恢复避免泄漏禁用态）
}

void Renderer::drawVectorLabels3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0 || viewportHeight_ <= 0) return;
    if (!fontAtlas_.isLoaded() || textProgram_.program() == 0 || vboText_ == 0) return;

    const float density = static_cast<float>(nav_.displayDensity());
    const float bakePx = static_cast<float>(fontAtlas_.bakePx());
    if (bakePx <= 0.0f) return;
    const float vCenterPx = (fontAtlas_.ascentPx() + fontAtlas_.descentPx()) * 0.5f; // 视觉中心相对基线（上正）

    // 相机整数级别：级别不足隐藏的层其标注一并隐藏（与 2D / 矢量层同口径，免幽灵标注）。
    const int camLevel = nav_.displayLevel();

    // 屏幕像素空间正交投影（y 向下、原点左上），字形坐标直接用像素（与 marker 同范式）。
    const Matrix4 screenOrtho = Matrix4::ortho(0.0f, static_cast<float>(viewportWidth_),
                                               static_cast<float>(viewportHeight_), 0.0f);

    // poor-man's 描边：屏幕空间 8 向偏移（偏移量 ≈ 1dp 像素，无 worldPerPx）。
    std::vector<float> outlineVerts;
    std::vector<float> fillVerts;
    const float o = 1.0f * density; // 轮廓偏移（像素）
    static const float kDir[8][2] = {
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };

    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty()) continue;
        // 屏幕像素/烘焙像素比例：使文字屏幕高 = 16dp×density×labelSize（与 2D 同口径，不随缩放变）
        const float sPx = (16.0f * density * vl.style.labelSize) / bakePx;
        if (sPx <= 0.0f) continue;
        const double ox = vl.geom.originWx;
        const double oy = vl.geom.originWy;
        for (size_t idx = 0; idx < vl.geom.labels.size(); ++idx) {
            const LabelItem &li = vl.geom.labels[idx];
            const VectorLayer::LabelLayout *lay = ensureLabelLayout(vl, idx);
            if (lay == nullptr) continue;
            // 锚点（相对图层原点世界坐标）→ ECEF（贴球面 alt=0）
            const Vec3 p = worldXYToCartesian(ox + static_cast<double>(li.x),
                                              oy + static_cast<double>(li.y), 0.0);
            // CPU 地平线剔除：背面半球（表面法向背向相机）的字不画，免纯屏幕叠加穿地球。
            // 球近似：法向≈normalize(p)，可见当且仅当 dot(p, eye−p)>0（两侧长度均正，比符号即可）。
            const Vec3 toEye{cam.eye.x - p.x, cam.eye.y - p.y, cam.eye.z - p.z};
            if (p.x * toEye.x + p.y * toEye.y + p.z * toEye.z <= 0.0) continue;
            float axp, ayp;
            if (!projectEcef3D(cam, viewProjRtc, p, axp, ayp)) continue; // 相机背后/近平面外
            if (vl.style.labelOutline) {
                for (const auto &d : kDir)
                    emitLabelQuads(*lay, axp + d[0] * o, ayp + d[1] * o, sPx, vCenterPx,
                                   vl.style.labelOutlineR, vl.style.labelOutlineG,
                                   vl.style.labelOutlineB, vl.style.labelOutlineA, outlineVerts);
            }
            emitLabelQuads(*lay, axp, ayp, sPx, vCenterPx,
                           vl.style.labelR, vl.style.labelG, vl.style.labelB, vl.style.labelA, fillVerts);
        }
    }

    if (outlineVerts.empty() && fillVerts.empty()) return;

    // 图集上传/重传：排版期可能烘焙了新字形（dirty），或上下文重建后 atlasTex_ 已失效
    if (!atlasTex_.isValid() || fontAtlas_.dirty()) {
        atlasTex_.uploadRGBA(fontAtlas_.pixels(), fontAtlas_.size(), fontAtlas_.size());
        fontAtlas_.clearDirty();
    }
    if (!atlasTex_.isValid()) return;

    // 纯屏幕叠加：关深度测试，画在最上层（与 marker 一致）；收尾复开避免泄漏禁用态。
    glDisable(GL_DEPTH_TEST);
    textProgram_.use();
    glUniformMatrix4fv(txUMvp_, 1, GL_FALSE, screenOrtho.data());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(txUTexture_, 0);
    atlasTex_.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboText_);
    const GLsizei tstride = kTextFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(txAPos_);
    glEnableVertexAttribArray(txATexCoord_);
    glEnableVertexAttribArray(txAColor_);
    glVertexAttribPointer(txAPos_, 2, GL_FLOAT, GL_FALSE, tstride, reinterpret_cast<const void *>(0));
    glVertexAttribPointer(txATexCoord_, 2, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glVertexAttribPointer(txAColor_, 4, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(4 * sizeof(float)));
    auto drawBuf = [&](const std::vector<float> &buf) {
        if (buf.empty()) return;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / kTextFloatsPerVertex));
    };
    drawBuf(outlineVerts); // 轮廓在下
    drawBuf(fillVerts);    // 文字在上
    glDisableVertexAttribArray(txAPos_);
    glDisableVertexAttribArray(txATexCoord_);
    glDisableVertexAttribArray(txAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::release() {
    clearTileTextures();
    releaseVectorGl();
    if (vboDynamic_ != 0) {
        glDeleteBuffers(1, &vboDynamic_);
        vboDynamic_ = 0;
    }
    if (vboUnitQuad_ != 0) {
        glDeleteBuffers(1, &vboUnitQuad_);
        vboUnitQuad_ = 0;
    }
    if (vboText_ != 0) {
        glDeleteBuffers(1, &vboText_);
        vboText_ = 0;
    }
    if (vboIcon_ != 0) {
        glDeleteBuffers(1, &vboIcon_);
        vboIcon_ = 0;
    }
    if (vboGlobeMesh_ != 0) {
        glDeleteBuffers(1, &vboGlobeMesh_);
        vboGlobeMesh_ = 0;
    }
    atlasTex_.release();
    markerIconTex_.release();
    colorProgram_.release();
    lineProgram_.release();
    texProgram_.release();
    textProgram_.release();
    iconProgram_.release();
    globeTexProgram_.release();
    globeColorProgram_.release();
    globeFillProgram_.release();
    globeStrokeProgram_.release();
    globePointProgram_.release();
    globeIconProgram_.release();
    globeFlatIconProgram_.release();
}

} // namespace globecore
