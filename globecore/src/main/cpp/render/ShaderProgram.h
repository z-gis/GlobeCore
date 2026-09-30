#ifndef GLOBECORE_RENDER_SHADER_PROGRAM_H
#define GLOBECORE_RENDER_SHADER_PROGRAM_H

#include <GLES2/gl2.h>

namespace globecore {

/**
 * OpenGL ES 2.0 着色器程序封装，对应 参考实现 render 的 BasicShaderProgram 体系。
 * 负责编译顶点/片元着色器、链接程序、查询属性与 uniform 位置，并在失败时输出日志。
 * 不可拷贝（持有 GL 资源），随 Renderer 生命周期创建/释放。
 */
class ShaderProgram {
public:
    ShaderProgram() = default;
    // 注意：GL 资源必须在 GL 线程删除，故析构不调用 release()。
    // 由持有者（Renderer）在 GL 线程通过 release() 显式释放；EGL 上下文销毁亦会回收。
    ~ShaderProgram() = default;

    ShaderProgram(const ShaderProgram &) = delete;
    ShaderProgram &operator=(const ShaderProgram &) = delete;

    /// 编译并链接着色器程序；成功返回 true，program() 变为非 0
    bool init(const char *vertexSrc, const char *fragmentSrc);

    void use() const;
    void release();

    GLuint program() const { return program_; }
    GLint attribLocation(const char *name) const;
    GLint uniformLocation(const char *name) const;

private:
    GLuint program_ = 0;

    static GLuint compileShader(GLenum type, const char *src);
};

} // namespace globecore

#endif // GLOBECORE_RENDER_SHADER_PROGRAM_H
