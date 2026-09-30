#ifndef GLOBECORE_RENDER_TEXTURE_H
#define GLOBECORE_RENDER_TEXTURE_H

#include <GLES2/gl2.h>

namespace globecore {

/**
 * OpenGL ES 2D 纹理封装，对应 参考实现 render 的 Texture。
 *
 * GL 资源必须在持有上下文的 GL 线程创建/删除：故析构不调用 release()，
 * 由 Renderer 在 GL 线程（release/清缓存）显式释放。可移动、不可拷贝。
 */
class Texture {
public:
    Texture() = default;
    ~Texture() = default;

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    Texture(Texture &&other) noexcept;
    Texture &operator=(Texture &&other) noexcept;

    /// 上传 RGBA8888 像素为纹理（自动生成 id、设置线性过滤 + CLAMP_TO_EDGE）
    bool uploadRGBA(const uint8_t *rgba, int width, int height);

    void bind() const;
    void release();

    /// 放弃当前 GL 句柄而不删除（GL 上下文重建时用：旧句柄已随上下文销毁，删除无意义
    /// 且可能误删新上下文同名纹理）。与瓦片缓存 clear()（依赖 Texture 默认析构不 glDelete）同一口径。
    void abandon() { id_ = 0; width_ = 0; height_ = 0; }

    bool isValid() const { return id_ != 0 && width_ > 0 && height_ > 0; }
    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace globecore

#endif // GLOBECORE_RENDER_TEXTURE_H
