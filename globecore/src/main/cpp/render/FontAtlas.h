#ifndef GLOBECORE_RENDER_FONT_ATLAS_H
#define GLOBECORE_RENDER_FONT_ATLAS_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace globecore {

/**
 * 动态字形图集（stb_truetype）：按需把 UTF-8 文本用到的字形烘焙进一张固定尺寸的 RGBA 图集，
 * 供矢量要素标注（label）以屏幕空间 billboard 文本渲染（见 [Renderer] 的文本绘制通路）。
 *
 * 设计要点：
 *  - 字体来源为系统 CJK 字体文件：[detectSystemFontPath] 探测 /system/fonts 下的 Noto Sans CJK /
 *    思源黑体 / DroidSansFallback 等（TrueType(glyf)、OpenType(CFF/Type2) 与 .ttc 集合均支持），
 *    也可由宿主经 [load] 显式传入路径；探测/加载全在 native 完成，无需随包分发字体（零 APK 增量）。
 *  - 图集为 CPU 侧 RGBA 缓冲（rgb=255、a=覆盖率），GL 纹理由 [Renderer] 持有并在 [dirty] 置位时重新上传；
 *    GL 上下文重建只需重新上传，CPU 图集与字形表（[glyphs_]）保留，无需重新烘焙。
 *  - 字形按固定像素高 [kBakePx] 烘焙，渲染时按需缩放（标注显示尺寸通常小于烘焙尺寸 → 缩小清晰）；
 *    shelf 行式装箱，图集满则停止烘焙新字形（记一次告警，缺失字形不绘制，几何与其它标注不受影响）。
 *
 * 线程：[load]/[detectSystemFontPath] 可在任意线程调用（内部持锁，含一次性文件 IO 与字体解析）；
 * [glyph]/[pixels] 等烘焙相关只在 GL 线程调用（渲染期按需烘焙）。字体加载完成后 stbtt_fontinfo 只读，
 * 与烘焙不并发加载。
 */
class FontAtlas {
public:
    /// 图集边长（px）：1024² RGBA ≈ 4MB，容纳数百个 48px CJK 字形，足够单屏地图标注的不同字符数
    static constexpr int kAtlasSize = 1024;
    /// 字形烘焙像素高（px）：高于常见标注显示尺寸（16dp×density），渲染时缩小 → 清晰
    static constexpr int kBakePx = 48;

    /// 单个已烘焙字形的度量与图集 UV（坐标口径对齐 stb_truetype：y 向下，位图盒相对 pen/基线）
    struct Glyph {
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; // 图集内 UV 子矩形
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;               // 位图盒（px）：左上 (x0,y0)、右下 (x1,y1)，y0 通常为负（基线之上）
        float advanceX = 0.0f;                            // 水平步进（px）
        bool blank = false;                               // 空白字形（空格等）：无位图，仅按 advanceX 步进
    };

    FontAtlas() = default;
    ~FontAtlas();

    FontAtlas(const FontAtlas &) = delete;
    FontAtlas &operator=(const FontAtlas &) = delete;

    /// 探测系统 CJK 字体文件路径（返回首个可读文件）；无可用字体时返回空串。线程安全。
    static std::string detectSystemFontPath();

    /// 加载字体文件（幂等：已加载则直接返回 true）。[path] 为空时自动 [detectSystemFontPath]。
    /// 含一次性文件读入与字体解析，可在任意线程调用。失败（无字体 / 解析失败）返回 false 并记日志。
    bool load(const std::string &path);

    /// 字体是否已成功加载（未加载时 [glyph] 恒返回 nullptr，标注不绘制）。
    bool isLoaded() const;

    /// 取字形：命中字形表直接返回；未命中则按需烘焙后返回。返回 nullptr 表示字体未加载 / 图集已满 /
    /// 该码点无字形。仅 GL 线程调用（烘焙会写图集并置 [dirty]）。
    const Glyph *glyph(uint32_t codepoint);

    /// UTF-8 字节串解码为码点序列（跳过非法字节）。
    static void decodeUtf8(const std::string &s, std::vector<uint32_t> &out);

    // 字体垂直度量（px，随 [kBakePx] 换算）：ascent 为正（基线之上）、descent 为负（基线之下）
    float ascentPx() const { return ascentPx_; }
    float descentPx() const { return descentPx_; }
    int bakePx() const { return kBakePx; }

    // 图集像素与脏标记（供 [Renderer] 上传/重传 GL 纹理）
    const uint8_t *pixels() const { return rgba_.data(); }
    int size() const { return kAtlasSize; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    /// 烘焙单个码点到位图并装箱入图集，写出度量；成功返回 true（图集满 / 无字形返回 false）。持锁调用。
    bool bake(uint32_t codepoint, Glyph &out);

    mutable std::mutex mtx_;
    bool loaded_ = false;
    std::vector<unsigned char> fontData_; // stbtt 要求字体缓冲在 fontinfo 生命周期内常驻，故持有
    void *fontInfo_ = nullptr;            // stbtt_fontinfo*（隐藏于 void* 避免头文件引入 stb_truetype.h）
    float ascentPx_ = 0.0f;
    float descentPx_ = 0.0f;

    std::vector<uint8_t> rgba_;           // kAtlasSize² RGBA（rgb=255、a=覆盖率）
    int penX_ = 0;                        // 当前 shelf 行的水平游标（px）
    int penY_ = 0;                        // 当前 shelf 行的基线顶（px）
    int rowH_ = 0;                        // 当前行已用最大高度（px）
    bool full_ = false;                   // 图集已满：不再尝试烘焙新字形
    bool dirty_ = false;                  // 有新字形待 [Renderer] 上传
    std::unordered_map<uint32_t, Glyph> glyphs_;
};

} // namespace globecore

#endif // GLOBECORE_RENDER_FONT_ATLAS_H
