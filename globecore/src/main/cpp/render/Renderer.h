#ifndef GLOBECORE_RENDER_RENDERER_H
#define GLOBECORE_RENDER_RENDERER_H

#include <GLES2/gl2.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "geom/Matrix4.h"
#include "globe/Tessellator.h"
#include "layer/TileLayer.h"
#include "layer/VectorLayer.h"
#include "navigator/Navigator.h"
#include "render/FontAtlas.h"
#include "render/ShaderProgram.h"
#include "render/Texture.h"

namespace globecore {

/**
 * OpenGL ES 2.0 渲染器，对应 参考实现的渲染核心。
 *
 * GL 上下文由 Kotlin 侧 GLSurfaceView 创建并绑定，本类只在 GL 线程被调用，仅执行 GL 绘制指令。
 * 每帧依据 [Navigator]（相机）计算可视世界范围与可见瓦片，按 2D 平面墨卡托网格绘制。
 *
 * 多图源叠加（对齐 参考实现 多个 TiledSurfaceImage 图层）：持有 [layers_]（由 [GlobeEngine] 以 unique_ptr
 * 向量持有，地址稳定），按加入顺序逐层绘制——底图（影像）层在前不透明，overlay（注记）层在后开 alpha
 * 混合叠加其上。每层各自独立取瓦片（[TileLayer::loader]）、按 [TileLayer::maxLevel] 逐层钳制 LOD，
 * 并维护各自的持久 LRU 纹理缓存（[layerTextures_]，索引与 layers_ 对齐），层间纹理互不干扰。
 *
 * 瓦片获取全异步（对齐 参考实现）：本类在 GL 线程绝不同步读盘/联网，只通过 [TileLoader] 投递请求、
 * drainReady 取就绪字节解码上传；未就绪的瓦片以祖先纹理兜底或占位色填充（overlay 层不画占位，保持透明）。
 * 当前级瓦片纹理未就绪时，用最近的有效祖先纹理经 UV 子矩形变换拉伸铺满（对齐 参考实现 useAncestorTileTexture），
 * 消除缩放/加载途中的黑缝。LOD 叶级别由 [Navigator::tileLevel]（复刻 参考实现 mustSubdivide）给出。
 */
class Renderer {
public:
    Renderer(const Navigator &navigator, const std::vector<std::unique_ptr<TileLayer>> &layers,
             const std::vector<std::unique_ptr<VectorLayer>> &vectorLayers);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    /// GL 上下文创建：编译着色器、创建 VBO
    void onSurfaceCreated();

    /// 视口尺寸变化：更新 glViewport
    void onSurfaceChanged(int width, int height);

    /// 绘制一帧：清屏 + 按相机逐层绘制可见瓦片（纹理 / 占位）。
    /// 返回是否需再画一帧（= 任一层后台仍有瓦片在途/待上传），供 RENDERMODE_WHEN_DIRTY 省电模式下自请求重绘。
    bool onDrawFrame();

    /// 释放全部 GL 资源（须在 GL 线程调用）
    void release();

    /// 设置定位标记的地理坐标、可见性与移动方位角。UI 线程写、GL 线程读，用 markerMtx_ 防撕裂读。
    /// 标记在所有瓦片层之上、以屏幕固定尺寸绘制（不随缩放变化），位置更新后由 GlobeEngine 触发重绘。
    /// [headingDeg] 为移动方位角（顺时针自北 0..360）；传入负值表示无方向（不画方向箭头）。
    /// 若已通过 [setLocationMarkerIcon] 设置罗盘图标，则画图标纹理替代蓝点；否则画经典蓝点白边圆。
    void setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg);

    /// 设置定位标记罗盘图标（RGBA8888 像素 + 尺寸）。UI 线程调用（构造期只写，GL 线程只读，无竞争）。
    /// 图标在 drawLocationMarker 中作为屏幕固定尺寸纹理四边形绘制（中心锚点，替代蓝点），对齐主界面
    /// LocationModel 的 ic_compass 罗盘标记方式。空图标（rgba 空/尺寸非法）回退蓝点绘制。
    void setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h);

    /// 设置标注字体文件路径并加载字形图集（[path] 为空则自动探测系统 CJK 字体）。可在任意线程调用
    /// （FontAtlas::load 内部持锁、纯 CPU）；未加载成功时矢量标注不绘制，不影响其它渲染。
    void setFontPath(const std::string &path);

private:
    /// 瓦片 (z,x,y) 打包为唯一键
    static uint64_t tileKey(int z, int x, int y);

    /// 纹理缓存条目：纹理 + 最近使用帧号（LRU 淘汰依据）。id==0 表示「已尝试但暂无图」的占位
    struct TileTexEntry {
        Texture tex;
        uint64_t lastUsed = 0;
    };

    /// 单个图层的持久纹理缓存：tileKey → TileTexEntry（含 id==0 占位，避免每帧重复读盘/请求）
    using TexMap = std::unordered_map<uint64_t, TileTexEntry>;

    /// 绘制一个图层（相机参数每帧算一次、各层共享）：drain 就绪瓦片解码上传 → 请求本级+祖先链 →
    /// Pass A 占位（overlay 跳过）→ Pass B 纹理绘制（overlay 开 alpha 混合）→ LRU 淘汰。
    /// 返回该层是否仍有瓦片在途/待上传（需再画一帧）。
    bool renderLayer(size_t layerIndex, double cwx, double cwy, double hw, double hh, const Matrix4 &ortho);

    // ==================== 3D 球体通路（P1：瓦片贴球面 + 深度测试，复用瓦片纹理缓存/祖先兜底） ====================

    /// 3D 模式一帧：清色+深度 → camera3D 快照 → 逐层 renderGlobeLayer3D → renderGlobeVectors3D。
    /// 入口在 onDrawFrame 按 nav_.viewMode()==MODE_3D 分发；图标/文本/marker 3D 下仍暂不绘（P2.b/c/d）。
    bool onDrawFrame3D();

    /// 3D 矢量层一帧：逐可见矢量层 drainReady（与 2D 共用 loader，接管后 2D chunk 上传自动继续）
    /// → 几何未全部上 GPU 则 uploadGlobeVecChunks 逐帧推进 → 逐层逐 chunk（视锥剔除）draw
    /// fill/outline/line/point/icon。返回是否仍有层在加载/待上传。
    bool renderGlobeVectors3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc);

    /// 把 vl.geom 的 2D RTC 顶点世界坐标→ ECEF(alt=0 贴球面)→ 减「图层固定锚点 globeAnchor（=几何原点 ECEF）」
    /// → float，按每帧时间预算以「要素组」流式上传为多张 chunk VBO（P2.e，对齐 2D uploadVectorChunks 语义）：
    /// fill/outline/line 按 geom.featSpans 要素组同组落块（图斑整块长出），组内描边各 range 退化桥合批；
    /// 点圆/图标按点数预算独立游标。每 chunk 缓存世界 ECEF AABB 供逐帧视锥剔除。顶点纯几何（不随 eye、
    /// 不随高度变）：平移/缩放零重建零重传，eye 经 uOffset=anchor−eye、贴地高度经 uVecAlt 在着色器补回。
    /// 仅新几何接管/上下文重建触发重传（resetVectorUpload 重置游标），巨层不再单帧冻结秒级。
    void uploadGlobeVecChunks(VectorLayer &vl);

    /// 绘制一个图层的 3D 球面瓦片：drain 上传 → Tessellator 取可见叶瓦片 → 请求本级+祖先链 →
    /// Pass A 占位（无图无祖先的瓦片画棋盘深色贴球面；overlay 跳过）→ Pass B 逐瓦片纹理网格
    /// （自身纹理或祖先 UV 子矩形兜底，同 2D 口径）→ LRU 淘汰。返回是否仍有瓦片在途。
    /// @param cam          3D 相机快照（eye 供 RTC；有效性与视口已在 onDrawFrame3D 校验）
    /// @param viewProjAbs  proj·view（绝对 ECEF）：仅供 Tessellator 提视锥平面（包围球为绝对坐标）
    /// @param viewProjRtc  proj·lookAt(0,center-eye,up)：绘制用——顶点已烘焙相对眼点坐标，
    ///                     若用绝对 view 会二次平移丢精度/错位（RTC 口径同 2D 相机相对渲染）。
    bool renderGlobeLayer3D(size_t layerIndex, const Navigator::Camera3D &cam,
                            const Matrix4 &viewProjAbs, const Matrix4 &viewProjRtc);

    /// 生成一个球面瓦片的Triangle Strip 网格顶点（逐行 (上,下) 顶点对，G×G 四边格）：
    /// 交错布局 [x,y,z,u,v,r,g,b,a]（kGlobeFloatsPerVertex=9）——位置为大地坐标减眼点（RTC，
    /// double 差后转 float），uv 按瓦片经纬度线性（v=0 对应北边=图像顶行，同 2D 单位四边形口径），
    /// 颜色为占位色（纹理绘制时忽略）。包围盒外约 0.75% 外扩盖住跨级 T 形接缝（同 2D texelPad 思路）。
    void buildGlobeTileMesh(const GlobeTile &tile, const Vec3 &eye, float r, float g, float b,
                            std::vector<float> &verts) const;

    /// 3D 定位 marker（P2.b）：把 marker 经纬度 CPU 投影到屏幕像素（复用 viewProjRtc，含相机朝向），
    /// 在屏幕像素空间用 screenOrtho 画罗盘图标 / 蓝点白边圆 + 移动方向箭头（箭头方向由「沿 heading
    /// 前移一小段的地理点」再投影得到 → 自动随相机旋转）。作为纯屏幕叠加画在最上层 → 关深度测试。
    void drawLocationMarker3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc);

    /// 3D：绝对 ECEF → 减眼(RTC) → viewProjRtc → 屏幕像素（y 向下、原点左上）。
    /// 相机背后（clip.w<=eps）或视口非法返回 false。供标注/图标 billboard 通路复用
    ///（地理坐标入口由调用方自行 geographicToCartesian 后传入，免本类保留冗余 helper）。
    bool projectEcef3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc,
                       const Vec3 &ecef, float &outSxPx, float &outSyPx) const;

    /// 确保 vl.labelLayouts[idx] 与 geom.labels[idx].text 一致（UTF-8 解码 + 字形查表，烘焙像素单位），
    /// 相机/缩放无关，2D/3D 标注通路共享。排版未完成（有码点未取到字形）或空文本 totalAdv<=0 返回 nullptr。
    const VectorLayer::LabelLayout *ensureLabelLayout(VectorLayer &vl, size_t idx);

    /// 3D 矢量标注（P2.d）：逐标注锚点 worldXYToCartesian→ECEF，CPU 地平线剔除（背面半球字不穿地球），
    /// projectEcef3D 投影到屏幕像素，在屏幕像素空间用 screenOrtho 喂现有 textProgram_（8 向描边）。
    /// 与 marker 同为纯屏幕叠加→内部关/复开深度测试。字号屏幕恒定（与 2D 同口径）。
    void drawVectorLabels3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc);

    /// 获取瓦片纹理缓存条目（对齐 参考实现 TiledSurfaceImage.getTexture，绝不阻塞渲染线程）：
    /// 命中缓存则刷新 lastUsed 并返回；未命中则插入 id==0 占位条目并 loader.request 异步取（先读盘后联网），
    /// 返回的条目在就绪前 isValid()==false（绘制时走祖先兜底或占位）。就绪字节经 drainReady 解码上传。
    TileTexEntry &getTexture(TexMap &texMap, TileLoader &loader, int z, int x, int y);

    /// 为缺失瓦片向上查找最近的有效祖先纹理（level-1..0）；命中返回其缓存条目并输出祖先级别，否则 nullptr
    TileTexEntry *findAncestorEntry(TexMap &texMap, int level, int tx, int ty, int &outAncestorLevel);

    /// LRU 淘汰：某层缓存条目超过 kMaxCachedTiles 时，按 lastUsed 升序释放最久未用的纹理（须在 GL 线程）
    void evictTileTextures(TexMap &texMap);

    void clearTileTextures();

    /// 在所有瓦片层之后叠加绘制定位标记：地理坐标 → 世界坐标（与瓦片同一 RTC 口径），
    /// 按「世界单位/像素」换算出屏幕固定半径，用 color 程序画：先移动方向箭头（若 heading>=0）、
    /// 再白边圆、最后蓝心圆（蓝点在上）。
    void drawLocationMarker(double cwx, double cwy, double hw, const Matrix4 &ortho);

    /// 绘制全部可见矢量层（在底图瓦片层之后、注记 overlay 层之前）：首次 drainReady 后上传 VBO，
    /// 之后每帧按 RTC（uOffset=图层原点−相机中心）绘制 面填充 → 面描边 → 线 → 点（屏幕固定圆）。
    /// 返回是否仍有矢量层在加载/待上传（需再画一帧）。
    bool drawVectorLayers(double cwx, double cwy, double hw, const Matrix4 &ortho);

    /// 在矢量层之后、注记 overlay 之前绘制矢量要素标注（屏幕空间 billboard 文本，字号恒定像素）。
    /// 逐可见矢量层把标注锚点（相对原点世界坐标 + RTC 偏移）周围排版字形四边形；按需烘焙新字形后
    /// 重传图集纹理；启用轮廓时先以 8 方向偏移画轮廓色、再画文字主体（poor-man's outline）。
    void drawVectorLabels(double cwx, double cwy, double hw, const Matrix4 &ortho);

    /// 在矢量层之后、标注之前绘制点要素图标（屏幕空间 billboard，屏幕固定像素尺寸、中心锚点）：
    /// 逐可见矢量层懒上传图标纹理（[VectorLayer::iconRgba] → iconTex，上下文重建后重传）与懒建静态
    /// 图标 VBO（中心+角点属性，几何重载/上下文重建后重建），每层仅设 uOffset/uHalf uniform + 1 次
    /// glDrawArrays（顶点不随相机变化，零逐帧 CPU 重建/上传）。无图标的层由 [drawVectorLayers] 画圆。
    void drawVectorIcons(double cwx, double cwy, double hw, const Matrix4 &ortho);

    /// 流式上传某矢量层的 CPU 几何（[VectorLayer::geom]）为 chunk VBO：按每帧时间预算推进
    /// [VectorLayer::uploadStage]/[VectorLayer::uploadPos] 游标（fill→outline→line 三阶段、每片约
    /// 4MB），巨层不再单帧冻结秒级；全部完成后置 glReady 并清 streamingUpload。
    /// 每片上传即新建 chunk VBO；旧 chunk 由 drainReady 换几何/resetVectorGl 统一清理，不误删失效名。
    void uploadVectorChunks(VectorLayer &vl);

    /// GL 上下文重建：把全部矢量层的 chunk 列表/iconVbo 作废清零、glReady 置 false（保留 CPU 几何），
    /// 使下一帧据 geom 重新流式上传（延续 onSurfaceCreated 的瓦片纹理/VBO 重建口径）。
    void resetVectorGl();

    /// 释放全部矢量层的 GL VBO（须在 GL 线程调用）。
    void releaseVectorGl();

    const Navigator &nav_;
    // 图层向量（由 GlobeEngine 持有，本类引用）：按加入顺序绘制，底图在前、注记 overlay 在后
    const std::vector<std::unique_ptr<TileLayer>> &layers_;
    // 矢量图层向量（由 GlobeEngine 持有，本类引用）：绘制在底图瓦片层与注记 overlay 层之间
    const std::vector<std::unique_ptr<VectorLayer>> &vectorLayers_;

    // 占位（纯色）绘制通路
    ShaderProgram colorProgram_;
    GLuint vboDynamic_ = 0; // 逐帧填充的占位四边形顶点
    GLint cAPos_ = -1;
    GLint cAColor_ = -1;
    GLint cUMvp_ = -1;
    GLint cUOffset_ = -1;   // RTC 偏移：矢量顶点为相对图层原点坐标，着色器做 aPos+uOffset；瓦片/标记传 (0,0)

    // 线（屏幕空间等宽三角带）绘制通路：矢量线/面描边用。顶点布局 [x,y,mx,my,r,g,b,a]，
    // (mx,my) 为加载期烘焙的 miter 接头法向（含长度系数）；着色器按 aPos + aMiter*uHalfWidthWorld 偏移，
    // uHalfWidthWorld 每帧按缩放（世界单位/像素 × 像素半宽）传入 → 恒定像素线宽（替代 glLineWidth）。
    ShaderProgram lineProgram_;
    GLint lAPos_ = -1;
    GLint lAMiter_ = -1;
    GLint lAColor_ = -1;
    GLint lUMvp_ = -1;
    GLint lUOffset_ = -1;
    GLint lUHalfWidthWorld_ = -1;

    // 纹理绘制通路
    ShaderProgram texProgram_;
    GLuint vboUnitQuad_ = 0; // 静态单位四边形（pos + uv）
    GLint tAPos_ = -1;
    GLint tATexCoord_ = -1;
    GLint tUMvp_ = -1;
    GLint tUTexture_ = -1;
    GLint tUUvOffset_ = -1; // 祖先兜底：把祖先纹理的 UV 子矩形映射到子瓦片（自身纹理时为恒等）
    GLint tUUvScale_ = -1;

    // 文本（矢量标注）绘制通路：屏幕空间 billboard 字形四边形，顶点布局 [x,y,u,v,r,g,b,a]（位置为
    // 每帧烘焙的相机相对世界坐标，无需 uOffset），片元输出 vec4(color.rgb, color.a × 图集覆盖率)。
    ShaderProgram textProgram_;
    GLuint vboText_ = 0;
    GLint txAPos_ = -1;
    GLint txATexCoord_ = -1;
    GLint txAColor_ = -1;
    GLint txUMvp_ = -1;
    GLint txUTexture_ = -1;

    // 图标（点要素 billboard）绘制通路：顶点布局 [cx,cy,mx,my,u,v,r,g,b,a]（10 floats/顶点）——中心坐标
    // （相对图层原点，配 uOffset 还原相机相对）+ 角点属性 (mx,my)∈[-1,1]，着色器按 aPos+uOffset+aCorner*uHalf
    // 展开四边形，uHalf 每帧按缩放传世界半宽/半高 → 屏幕固定尺寸，几何重载后顶点不再逐帧重建：
    // 每矢量层一张静态 VBO（[VectorLayer::iconVbo]，首帧懒建）+ 一张图标纹理（[VectorLayer::iconTex]）。
    // 定位标记罗盘图标仍走动态 vboIcon_（单四边形）。片元输出 图标纹理 RGBA × 顶点色（默认白=纯图标）。
    ShaderProgram iconProgram_;
    GLuint vboIcon_ = 0;   // 仅定位标记单四边形逐帧用（矢量图标用各层静态 iconVbo）
    GLint ixAPos_ = -1;
    GLint iACorner_ = -1;
    GLint ixATexCoord_ = -1;
    GLint ixAColor_ = -1;
    GLint ixUMvp_ = -1;
    GLint ixUTexture_ = -1;
    GLint ixUOffset_ = -1; // RTC 偏移：图标中心为相对图层原点坐标；定位标记已烘焙相机相对，传 (0,0)
    GLint ixUHalf_ = -1;   // 图标世界半宽/半高（像素半尺寸 × 世界单位/像素，每帧传 → 屏幕恒定尺寸）

    // 3D 瓦片纹理通路：顶点 [x,y,z,u,v(,r,g,b,a)]（与占位通路共用交错缓冲， stride=9 floats），
    // uUvOffset/uUvScale 同 2D 做祖先子矩形映射；uMvp 传 viewProjRtc（顶点已相对眼点）。
    ShaderProgram globeTexProgram_;
    GLint g3APos_ = -1;
    GLint g3ATexCoord_ = -1;
    GLint g3UMvp_ = -1;
    GLint g3UTexture_ = -1;
    GLint g3UUvOffset_ = -1;
    GLint g3UUvScale_ = -1;

    // 3D 占位通路：同布局取 pos3+color4，画无图无祖先瓦片的棋盘深色（overlay 层跳过）。
    ShaderProgram globeColorProgram_;
    GLint gc3APos_ = -1;
    GLint gc3AColor_ = -1;
    GLint gc3UMvp_ = -1;

    // 3D 矢量面填充通路（P2.a）：pos3+color4 = 7f/v，uMvp 传 viewProjRtc，uOffset=anchor−eye（平移零重建）；
    // uAnchor=图层固定锚点（绝对 ECEF）、uVecAlt=贴地高度（每帧连续）：顶点纯几何 alt=0 一次烘焙，
    // 高度在着色器沿表面法向 (normalize(aPos+uAnchor)) 抬升 → 缩放零重建。片元复用 kColorFragmentSrc。
    ShaderProgram globeFillProgram_;
    GLint gfAPos_ = -1;
    GLint gfAColor_ = -1;
    GLint gfAMode_ = -1;   // 逐顶点高程模式（0=贴地走 uVecAlt，1=用 baked 高程不叠加）
    GLint gfUMvp_ = -1;
    GLint gfUOffset_ = -1;
    GLint gfUAnchor_ = -1;
    GLint gfUVecAlt_ = -1;

    // 3D 矢量描边/线通路（P2.a）：参考实现 3-顶点模板屏幕空间 miter。参考
    // 参考实现Kotlin/render/program/TriangleShaderProgram.kt，简化掉 bevel 回退（靠 uMiterLimit 钳制），
    // 端点 AB/BC 退化在 shader 里归一。片元复用 kColorFragmentSrc。
    ShaderProgram globeStrokeProgram_;
    GLint gsAPrev_ = -1;
    GLint gsACur_ = -1;
    GLint gsANext_ = -1;
    GLint gsASide_ = -1;
    GLint gsAColor_ = -1;
    GLint gsAMode_ = -1;
    GLint gsUMvp_ = -1;
    GLint gsUOffset_ = -1;
    GLint gsUAnchor_ = -1;
    GLint gsUVecAlt_ = -1;
    GLint gsUHalfWidthPx_ = -1;
    GLint gsUViewportPx_ = -1;
    GLint gsUMiterLimit_ = -1;

    // 3D 点要素 billboard 通路（P2.b）：复用 viewProjRtc 投影，与 stroke 同为「3D→clip→屏幕像素偏移」。
    // 顶点 [pCur3, corner2, color4]=9f：pCur 为 RTC 中心，corner 为单位圆方向∈[-1,1]（中心点用 (0,0)），
    // 着色器 gl_Position.xy = pCS.w*(pC + corner*uHalfPx*2/viewport) → 屏幕恒定像素半径；z 保留 pCS.z
    // → 开深度测试后背半球点被地球剔除。片元复用 kColorFragmentSrc。
    ShaderProgram globePointProgram_;
    GLint gpAPos_ = -1;
    GLint gpACorner_ = -1;
    GLint gpAColor_ = -1;
    GLint gpAMode_ = -1;
    GLint gpUMvp_ = -1;
    GLint gpUOffset_ = -1;
    GLint gpUAnchor_ = -1;
    GLint gpUVecAlt_ = -1;
    GLint gpUHalfPx_ = -1;
    GLint gpUViewportPx_ = -1;

    // 3D 点要素图标 billboard 通路（P2.c）：与 globePoint 同源投影（viewProjRtc→clip→屏幕像素 corner 展开），
    // 多了 uv 采样图标纹理。顶点 [pos3, corner2, uv2, color4]=11f；片元复用 kIconFragmentSrc（纹理×顶点色）。
    // uHalfPx 逐层传图标像素半宽/半高（位图像素×scale×density×0.5）→ 屏幕恒定像素尺寸；zw 保留→背半球剔除。
    ShaderProgram globeIconProgram_;
    GLint giaPos_ = -1;
    GLint giACorner_ = -1;
    GLint giATexCoord_ = -1;
    GLint giAColor_ = -1;
    GLint giaMode_ = -1;
    GLint giUMvp_ = -1;
    GLint giUOffset_ = -1;
    GLint giUAnchor_ = -1;
    GLint giUVecAlt_ = -1;
    GLint giUHalfPx_ = -1;
    GLint giUViewportPx_ = -1;
    GLint giUTexture_ = -1;

    // 3D 贴地定位标识通路：图标顶点 [pos3, uv2, color4]=9f，uMvp 传 viewProjRtc（顶点已在 CPU 端沿
    // 局部切平面基铺成贴地矩形并 double 减眼点转 float）→ 图标平行于地球表面、指真北，随相机俯仰
    // 自然透视压扁（非正对屏幕的 billboard）。片元复用 kIconFragmentSrc。箭头/引线复用 globeColorProgram_。
    ShaderProgram globeFlatIconProgram_;
    GLint gfiPos_ = -1;
    GLint gfiTexCoord_ = -1;
    GLint gfiColor_ = -1;
    GLint gfiMvp_ = -1;
    GLint gfiTexture_ = -1;

    GLuint vboGlobeMesh_ = 0;              // 3D 网格动态 VBO（Pass A 批量一次上传；Pass B 逐瓦片重传）
    std::vector<GlobeTile> globeTilesScratch_; // 可见叶瓦片 scratch（逐层复用，避免反复堆分配）
    std::vector<float> globeMeshScratch_;      // 单瓦片网格 scratch（Pass B 逐瓦片上传用）

    // 字形图集：CPU 侧 RGBA 缓冲与字形表常驻（[fontAtlas_]），GL 纹理（[atlasTex_]）在图集脏/上下文重建后重传。
    FontAtlas fontAtlas_;
    Texture atlasTex_;

    // 每层各一份持久纹理 LRU 缓存（索引与 layers_ 对齐；addTileLayer 后在 onDrawFrame 内按需 resize）
    std::vector<TexMap> layerTextures_;
    uint64_t frameTick_ = 0;                       // 帧计数，用于 LRU lastUsed 标记
    // 单层 LRU 上限。对齐 参考实现 RenderResourceCache 的「按字节预算」口径：参考实现 默认兑底 256MB
    // = 1024×256×256px RGBA。取 1024 与 参考实现 默认量级一致，且需大于 kMaxTiles(640) 以容纳「叶自身 + 近景祖先链」
    // 去重后的工作集，避免「驱逐→下帧 miss→重请求→图源限流(429)」风暴（曾只剩棋盘）。
    // native OOM 已由 TileLoader.ready_ 高水位背压堆主，不靠本值；本值只决定远景能否驻留清晰。设备显存偏紧时可下调。
    static constexpr size_t kMaxCachedTiles = 1024;

    int viewportWidth_ = 0;
    int viewportHeight_ = 0;

    // 定位标记状态：UI 线程 setLocationMarker 写、GL 线程 drawLocationMarker 读
    mutable std::mutex markerMtx_;
    bool markerVisible_ = false;
    double markerLon_ = 0.0;
    double markerLat_ = 0.0;
    double markerHeading_ = -1.0; // 移动方位角（顺时针自北）；<0 表示无方向不画箭头

    // 定位罗盘图标（对齐主界面 ic_compass）：UI 线程 setLocationMarkerIcon 写 CPU 像素（构造后只读），
    // GL 线程 drawLocationMarker 懒上传纹理并画纹理四边形替代蓝点；上下文重建后据 markerIconRgba_ 重传
    std::vector<uint8_t> markerIconRgba_;
    int markerIconW_ = 0;
    int markerIconH_ = 0;
    Texture markerIconTex_;
};

} // namespace globecore

#endif // GLOBECORE_RENDER_RENDERER_H
