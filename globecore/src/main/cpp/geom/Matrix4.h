#ifndef GLOBECORE_GEOM_MATRIX4_H
#define GLOBECORE_GEOM_MATRIX4_H

#include <array>
#include <cmath>

#include "geom/Vec3.h"

namespace globecore {

/**
 * 视锥裁剪平面（Gribb-Hartmann 自 viewProj 提取），法向指向视锥内侧。平面方程 a·x+b·y+c·z+d=0，
 * (a,b,c) 为单位法向；点 p 到平面的带符号距离 = dot(p,n)+d，>=0 在内侧。用于瓦片包围球可见性测试。
 */
struct FrustumPlane {
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    /// 点 (x,y,z) 的带符号距离（正=内侧）
    double distanceTo(double x, double y, double z) const { return a * x + b * y + c * z + d; }
};

/**
 * 4x4 列主序（column-major）变换矩阵，直接用于 glUniformMatrix4fv（transpose=GL_FALSE）。
 * 对应 参考实现的 Matrix。2D 墨卡托用 ortho + model2D；3D 球体模式用 perspective + lookAt（见下）。
 *
 * 存储约定：m[col*4 + row]，与 OpenGL 一致。
 */
class Matrix4 {
public:
    float m[16] = {0};

    /// 正交投影：将世界矩形 [l,r]x[b,t] 映射到 NDC [-1,1]x[-1,1]。
    /// 注意：2D 墨卡托世界坐标 y 向下增长，调用方通过令 t<b（top 值更小）实现 y 轴翻转，
    /// 使纬度越北（世界 y 越小）显示在屏幕越上方。
    static Matrix4 ortho(float l, float r, float b, float t) {
        Matrix4 m{};
        const float rl = r - l;
        const float tb = t - b;
        // near=-1, far=1 时：-2/(f-n) = -1，-(f+n)/(f-n) = 0
        m.m[0] = (rl != 0.0f) ? (2.0f / rl) : 0.0f;
        m.m[5] = (tb != 0.0f) ? (2.0f / tb) : 0.0f;
        m.m[10] = -1.0f;
        m.m[15] = 1.0f;
        m.m[12] = (rl != 0.0f) ? (-(r + l) / rl) : 0.0f;
        m.m[13] = (tb != 0.0f) ? (-(t + b) / tb) : 0.0f;
        m.m[14] = 0.0f;
        return m;
    }

    const float *data() const { return m; }

    /// 矩阵乘法 a*b（列主序）。用于组合「全局正交投影 × 逐瓦片模型矩阵」
    static Matrix4 multiply(const Matrix4 &a, const Matrix4 &b) {
        Matrix4 r{};
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        }
        return r;
    }

    /// 2D 模型矩阵：先按 (sx,sy) 缩放，再平移到 (tx,ty)（z=0，单位四边形 → 瓦片世界矩形）
    static Matrix4 model2D(float tx, float ty, float sx, float sy) {
        Matrix4 m{};
        m.m[0] = sx;
        m.m[5] = sy;
        m.m[10] = 1.0f;
        m.m[15] = 1.0f;
        m.m[12] = tx;
        m.m[13] = ty;
        return m;
    }

    // ==================== 3D 球体模式：透视投影 + 轨道相机 ====================

    /// 恒等矩阵
    static Matrix4 identity() {
        Matrix4 m{};
        m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
        return m;
    }

    /**
     * 透视投影（右手系，NDC z∈[-1,1]，对齐 glFrustum）。fovYRad 为垂直视场角（弧度），aspect=宽/高，
     * near/far 为近/远裁剪面距离（米，正数）。3D 瓦片绘制与相机基向量共用此投影。
     */
    static Matrix4 perspective(float fovYRad, float aspect, float near, float far) {
        Matrix4 m{};
        const float f = (aspect != 0.0f && std::sin(fovYRad) != 0.0f)
                            ? 1.0f / std::tan(fovYRad * 0.5f)
                            : 1.0f;
        m.m[0] = f / aspect;                     // col0 row0
        m.m[5] = f;                              // col1 row1
        const float nf = near - far;
        m.m[10] = (far + near) / nf;             // col2 row2
        m.m[11] = -1.0f;                         // col2 row3
        m.m[14] = (2.0f * far * near) / nf;      // col3 row2
        return m;
    }

    /**
     * 视图矩阵（lookAt，右手系，-Z 为前）：相机位于 eye，注视 center，up 为上方向。返回 world→view 变换。
     * eye/center/up 均为 ECEF 米制坐标。用于 3D 球体相机（eye 在椭球外、center 为注视地表点）。
     */
    static Matrix4 lookAt(const Vec3 &eye, const Vec3 &center, const Vec3 &up) {
        const Vec3 f = (center - eye).normalized();       // forward
        const Vec3 s = f.cross(up).normalized();          // right
        const Vec3 u = s.cross(f);                        // 正交化后的 up
        Matrix4 m = identity();
        m.m[0] = static_cast<float>(s.x);  m.m[4] = static_cast<float>(s.y);  m.m[8] = static_cast<float>(s.z);
        m.m[1] = static_cast<float>(u.x);  m.m[5] = static_cast<float>(u.y);  m.m[9] = static_cast<float>(u.z);
        m.m[2] = static_cast<float>(-f.x); m.m[6] = static_cast<float>(-f.y); m.m[10] = static_cast<float>(-f.z);
        m.m[12] = static_cast<float>(-s.dot(eye));
        m.m[13] = static_cast<float>(-u.dot(eye));
        m.m[14] = static_cast<float>(f.dot(eye));
        return m;
    }

    /**
     * 自本矩阵（作为 viewProj 组合矩阵）提取 6 个视锥平面（左/右/底/顶/近/远），均归一化。
     * Gribb-Hartmann：平面取「行」组合（rowN = 各列的第 N 个分量）。列主序下 row r = (m[0*4+r],m[1*4+r],m[2*4+r],m[3*4+r])。
     */
    std::array<FrustumPlane, 6> extractFrustumPlanes() const {
        // 取第 r 行（4 分量）
        auto row = [this](int r) {
            return std::array<double, 4>{m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]};
        };
        const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        std::array<FrustumPlane, 6> planes{{
            {r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]}, // left
            {r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]}, // right
            {r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]}, // bottom
            {r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]}, // top
            {r3[0] + r2[0], r3[1] + r2[1], r3[2] + r2[2], r3[3] + r2[3]}, // near
            {r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]}, // far
        }};
        for (auto &p : planes) {
            const double len = std::sqrt(p.a * p.a + p.b * p.b + p.c * p.c);
            if (len > 1e-12) { p.a /= len; p.b /= len; p.c /= len; p.d /= len; }
        }
        return planes;
    }
};

} // namespace globecore

#endif // GLOBECORE_GEOM_MATRIX4_H
