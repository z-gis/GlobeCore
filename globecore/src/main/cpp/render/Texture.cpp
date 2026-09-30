#include "render/Texture.h"

#include "util/Log.h"

#include <cstdint>

namespace globecore {

Texture::Texture(Texture &&other) noexcept
    : id_(other.id_), width_(other.width_), height_(other.height_) {
    other.id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

Texture &Texture::operator=(Texture &&other) noexcept {
    if (this != &other) {
        release();
        id_ = other.id_;
        width_ = other.width_;
        height_ = other.height_;
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool Texture::uploadRGBA(const uint8_t *rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) return false;

    if (id_ == 0) glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    // 瓦片按 1:1 或放大显示，用线性过滤；边缘 CLAMP 避免相邻瓦片采样溢出
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    width_ = width;
    height_ = height;
    return true;
}

void Texture::bind() const {
    if (id_ != 0) glBindTexture(GL_TEXTURE_2D, id_);
}

void Texture::release() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

} // namespace globecore
