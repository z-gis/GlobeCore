#ifndef GLOBECORE_RENDER_IMAGE_DECODER_H
#define GLOBECORE_RENDER_IMAGE_DECODER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace globecore {

/**
 * 图片解码器（stb_image 封装），对应 参考实现 render.image 的解码职责。
 * 把编码字节（PNG/JPEG/BMP/... stb 支持的格式）解码为 RGBA8888，供 GL 纹理上传。
 *
 * 实现依赖单头文件 `stb_image.h`（已置于 cpp/ 根，include 路径含 cpp/）；
 * `STB_IMAGE_IMPLEMENTATION` 仅在 ImageDecoder.cpp 中定义一次。
 */
class ImageDecoder {
public:
    /**
     * 解码为 RGBA8888。
     * @param data 编码字节
     * @param len  字节数
     * @param outW/outH 输出宽高（像素）
     * @param outPixels 输出 w*h*4 的 RGBA 数据
     * @return 成功返回 true；失败（空数据/格式不支持）返回 false
     */
    static bool decodeRGBA(const uint8_t *data, size_t len,
                           int &outW, int &outH, std::vector<uint8_t> &outPixels);
};

} // namespace globecore

#endif // GLOBECORE_RENDER_IMAGE_DECODER_H
