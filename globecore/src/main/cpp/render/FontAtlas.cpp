// stb_truetype 实现：全工程仅此一处定义 STB_TRUETYPE_IMPLEMENTATION。
// 关闭内部 assert，避免异常字体文件触发断言中止（缺失字形按失败路径优雅降级）。
#define STBTT_assert(x)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "render/FontAtlas.h"

#include <cstdio>

#include "util/Log.h"

namespace globecore {

namespace {
/// 字形/行间装箱留白（px）：LINEAR 过滤下相邻字形边缘不互相渗色
constexpr int kPad = 2;
} // namespace

FontAtlas::~FontAtlas() {
    delete static_cast<stbtt_fontinfo *>(fontInfo_);
    fontInfo_ = nullptr;
}

std::string FontAtlas::detectSystemFontPath() {
    // 覆盖主流 ROM 的系统 CJK 字体命名：Noto Sans CJK（.ttc 集合，API 26+ 常见）、
    // 思源黑体、旧机型 DroidSansFallback（TrueType）、鸿蒙 HarmonyOS Sans（含中文字形）。
    // 逐个探测首个可读文件；不存在的候选探测失败无副作用，故两端共用同一候选表。
    // 鸿蒙真机字体实名以 hdc shell ls /system/fonts 为准，ArkTS 侧亦可经 setFontPath 显式指定。
    static const char *const kCandidates[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSansCJKsc-Regular.otf",
        "/system/fonts/SourceHanSansCN-Regular.otf",
        "/system/fonts/SourceHanSansSC-Regular.otf",
        "/system/fonts/NotoSansCJKjp-Regular.otf",
        "/system/fonts/DroidSansFallbackFull.ttf",
        "/system/fonts/DroidSansFallback.ttf",
        "/system/fonts/HarmonyOS_Sans_SC.ttf",
        "/system/fonts/HarmonyOS_Sans_Regular.ttf",
    };
    for (const char *p : kCandidates) {
        FILE *f = std::fopen(p, "rb");
        if (f != nullptr) {
            std::fclose(f);
            return std::string(p);
        }
    }
    return std::string();
}

bool FontAtlas::load(const std::string &path) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (loaded_) return true;

    std::string p = path;
    if (p.empty()) p = detectSystemFontPath();
    if (p.empty()) {
        LOGW("FontAtlas: 未找到可用系统字体，矢量标注将不显示");
        return false;
    }

    FILE *f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) {
        LOGW("FontAtlas: 打开字体文件失败: %s", p.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        LOGW("FontAtlas: 字体文件为空: %s", p.c_str());
        return false;
    }
    fontData_.resize(static_cast<size_t>(sz));
    const size_t rd = std::fread(fontData_.data(), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    if (rd != static_cast<size_t>(sz)) {
        fontData_.clear();
        LOGW("FontAtlas: 字体文件读取不完整: %s", p.c_str());
        return false;
    }

    // .ttc 集合取首个字体；stbtt_InitFont 后 fontData_ 须常驻（fontinfo 仅存指针不拷贝）
    const int offset = stbtt_GetFontOffsetForIndex(fontData_.data(), 0);
    if (offset < 0) {
        fontData_.clear();
        LOGW("FontAtlas: 非字体文件或不支持的格式: %s", p.c_str());
        return false;
    }
    auto *info = new stbtt_fontinfo();
    if (!stbtt_InitFont(info, fontData_.data(), offset)) {
        delete info;
        fontData_.clear();
        LOGW("FontAtlas: stbtt_InitFont 失败: %s", p.c_str());
        return false;
    }
    fontInfo_ = info;

    const float scale = stbtt_ScaleForPixelHeight(info, static_cast<float>(kBakePx));
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(info, &a, &d, &lg);
    ascentPx_ = a * scale;
    descentPx_ = d * scale; // 负值（基线之下）

    rgba_.assign(static_cast<size_t>(kAtlasSize) * kAtlasSize * 4u, 0u);
    penX_ = kPad;
    penY_ = kPad;
    rowH_ = 0;
    full_ = false;
    dirty_ = false;
    glyphs_.clear();
    loaded_ = true;

    LOGI("FontAtlas: 字体加载成功 %s (%ld bytes, ascent=%.1f descent=%.1f)",
         p.c_str(), sz, ascentPx_, descentPx_);
    return true;
}

bool FontAtlas::isLoaded() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return loaded_;
}

bool FontAtlas::bake(uint32_t codepoint, Glyph &out) {
    auto *info = static_cast<stbtt_fontinfo *>(fontInfo_);
    const float scale = stbtt_ScaleForPixelHeight(info, static_cast<float>(kBakePx));

    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, static_cast<int>(codepoint), &advance, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(info, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    out.advanceX = advance * scale;
    out.x0 = x0;
    out.y0 = y0;
    out.x1 = x1;
    out.y1 = y1;

    // 空白/无位图字形（空格等）：仅步进，不占图集
    if (w <= 0 || h <= 0) {
        out.blank = true;
        return true;
    }

    // shelf 装箱：当前行放不下则换行；纵向放不下则图集满
    if (penX_ + w + kPad > kAtlasSize) {
        penX_ = kPad;
        penY_ += rowH_;
        rowH_ = 0;
    }
    if (penY_ + h + kPad > kAtlasSize) {
        full_ = true;
        return false;
    }

    std::vector<unsigned char> tmp(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    stbtt_MakeCodepointBitmap(info, tmp.data(), w, h, w, scale, scale, static_cast<int>(codepoint));
    // 覆盖率写入 alpha，rgb 置白（着色器输出 color.rgb，alpha=覆盖率×文字色 alpha）
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = &rgba_[(static_cast<size_t>(penY_ + y) * kAtlasSize + penX_) * 4u];
        const unsigned char *src = &tmp[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = 255;
            dst[x * 4 + 1] = 255;
            dst[x * 4 + 2] = 255;
            dst[x * 4 + 3] = src[x];
        }
    }
    out.u0 = static_cast<float>(penX_) / kAtlasSize;
    out.v0 = static_cast<float>(penY_) / kAtlasSize;
    out.u1 = static_cast<float>(penX_ + w) / kAtlasSize;
    out.v1 = static_cast<float>(penY_ + h) / kAtlasSize;
    out.blank = false;

    penX_ += w + kPad;
    if (h + kPad > rowH_) rowH_ = h + kPad;
    dirty_ = true;
    return true;
}

const FontAtlas::Glyph *FontAtlas::glyph(uint32_t codepoint) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!loaded_) return nullptr;
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return &it->second;
    if (full_) return nullptr;

    Glyph g;
    if (!bake(codepoint, g)) {
        if (full_) LOGW("FontAtlas: 字形图集已满（%d²），后续新字符不再烘焙", kAtlasSize);
        return nullptr;
    }
    // unordered_map 为节点式容器，元素地址在 rehash 后仍稳定，返回指针安全
    return &glyphs_.emplace(codepoint, g).first->second;
}

void FontAtlas::decodeUtf8(const std::string &s, std::vector<uint32_t> &out) {
    out.clear();
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            ++i; // 非法首字节，跳过
            continue;
        }
        if (extra > 0 && i + static_cast<size_t>(extra) >= n) {
            break; // 截断的多字节序列（后续字节不足）
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (ok) out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
}

} // namespace globecore
