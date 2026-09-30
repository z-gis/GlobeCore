#include "render/ShaderProgram.h"

#include "util/Log.h"

namespace globecore {

GLuint ShaderProgram::compileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        LOGE("glCreateShader failed type=%u", type);
        return 0;
    }
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char buf[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
        LOGE("Shader compile failed type=%u: %s", type, buf);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool ShaderProgram::init(const char *vertexSrc, const char *fragmentSrc) {
    release();

    const GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSrc);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        LOGE("glCreateProgram failed");
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    // 链接后着色器对象可删除（已并入程序）
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char buf[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        LOGE("Program link failed: %s", buf);
        glDeleteProgram(prog);
        return false;
    }

    program_ = prog;
    LOGI("ShaderProgram linked ok (program=%u)", program_);
    return true;
}

void ShaderProgram::use() const {
    if (program_ != 0) glUseProgram(program_);
}

void ShaderProgram::release() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

GLint ShaderProgram::attribLocation(const char *name) const {
    return program_ == 0 ? -1 : glGetAttribLocation(program_, name);
}

GLint ShaderProgram::uniformLocation(const char *name) const {
    return program_ == 0 ? -1 : glGetUniformLocation(program_, name);
}

} // namespace globecore
