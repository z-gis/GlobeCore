#ifndef GLOBECORE_VECTOR_VECTORBUILDER_H
#define GLOBECORE_VECTOR_VECTORBUILDER_H

#include <atomic>

#include "vector/VectorGeometry.h"
#include "vector/VectorReader.h"

namespace globecore {

/// 面填充顶点布局 [x,y,r,g,b,a] 的 floats 数（与占位/标记通路一致）
inline constexpr int kVecFloatsPerVertex = 6;
/// 线/描边顶点布局 [x,y,mx,my,r,g,b,a] 的 floats 数（屏幕空间等宽 miter 三角带）
inline constexpr int kLineFloatsPerVertex = 8;

/**
 * 由 WGS84 读取结果构建渲染几何（纯 CPU，应在后台线程调用）：
 *  - 图层原点取 bbox 中心的墨卡托世界坐标（RTC 基准，先于任何要素确定性算出）；
 *  - 顶点转为「相对原点的 float32」（小量级高精度）；
 *  - 面经 earcut 三角剖分（含洞），填充 + 描边分两组；线要素单独一组；点仅存相对坐标。
 *
 * 【并行分片】逐要素烘焙彼此独立（仅共享只读的 origin/style，各写各的输出缓冲），故按要素切块
 * 分发到多个 worker 线程并行 build，再按「块下标升序 + 块内要素升序」有序合并为单一 VectorGeometry
 * ——合并结果与串行 build 逐字节一致（描边/线 range 的 first 顶点索引在合并时按累计顶点数重映射）。
 * 线程数受进程级全局令牌预算约束（≤ CPU 核数）：多图层并发重载时总 build 线程有界，取不到令牌即
 * 回退到当前线程串行 build，绝不死锁/饿死/线程爆炸。小数据（要素/顶点量低于阈值）直接串行，免线程开销。
 *
 * [cancel] 非空且置真时（latest-wins 抢占）在块/要素边界提前中断，返回不完整的几何，由调用方丢弃。
 */
VectorGeometry buildGeometry(const VectorReadResult &r, const VectorStyle &s,
                             const std::atomic<bool> *cancel = nullptr);

} // namespace globecore

#endif // GLOBECORE_VECTOR_VECTORBUILDER_H
