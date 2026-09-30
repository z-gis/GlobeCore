// stb_image 实现：全工程仅此一处定义 STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "render/ImageDecoder.h"

#include "util/Log.h"

namespace globecore {

bool ImageDecoder::decodeRGBA(const uint8_t *data, size_t len,
                              int &outW, int &outH, std::vector<uint8_t> &outPixels) {
    if (data == nullptr || len == 0) return false;

    int w = 0, h = 0, channels = 0;
    // 强制 4 通道输出（RGBA），屏蔽源图 alpha/灰度差异，简化纹理上传
    unsigned char *px = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &channels, 4);
    if (px == nullptr) {
        LOGW("stbi_load_from_memory 解码失败 len=%zu reason=%s", len, stbi_failure_reason());
        return false;
    }

    outW = w;
    outH = h;
    const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    outPixels.assign(px, px + byteCount);
    stbi_image_free(px);
    return true;
}

} // namespace globecore
