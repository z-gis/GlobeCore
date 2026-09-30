#ifndef GLOBECORE_LAYER_VECTOR_LOADER_H
#define GLOBECORE_LAYER_VECTOR_LOADER_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "vector/VectorGeometry.h"
#include "vector/VectorReader.h"

namespace globecore {

/**
 * 后台矢量加载器（镜像 [TileLoader] 的异步口径，但为一次性整文件加载）。
 *
 * 职责：在后台线程读矢量文件（VectorReader：GDAL/OGR 读取 + 重投影到 WGS84）→ 投影到世界坐标 →
 * 计算图层原点（RTC 基准）→ earcut 三角剖分面 → 烘焙颜色，产出 [VectorGeometry]；GL 线程经
 * [drainReady] 一次性取走后上传 VBO。GL 线程绝不同步读盘/投影（对齐瓦片管线不阻塞渲染线程）。
 *
 * 状态机：Idle → (start) Loading → Ready（几何就绪待取）→ (drainReady) Consumed；
 * 读取失败（如 CAD 无坐标系）转 Failed，[error] 给出可展示原因。所有公开方法线程安全。
 */
class VectorLoader {
public:
    /// 文件模式：后台线程读 [path] 指向的矢量文件（GDAL/OGR）→ 建几何。构造后由宿主调用 [start]。
    /// [hasExtent]+[minLon,minLat,maxLon,maxLat] 给出初始屏幕过滤范围（false=整文件全量，小数据直显）；
    /// [maxFeatures] 为单次加载要素预算（≤0 取读取器硬上限，防密集数据 OOM）。
    VectorLoader(std::string path, VectorStyle style,
                 bool hasExtent = false, double minLon = 0.0, double minLat = 0.0,
                 double maxLon = 0.0, double maxLat = 0.0, int maxFeatures = 0);

    /// 内存数据源模式（动态叠加）：不读文件、不起后台线程；由宿主经 [setData] 推送 WGS84 几何，
    /// 在调用线程同步构建 [VectorGeometry]，再经与文件模式相同的状态机（Ready → drainReady）交接给
    /// GL 线程上传 VBO。测量/拍照标识/轨迹/样地等运行时叠加即走此模式，复用整条矢量渲染/拾取通路。
    explicit VectorLoader(VectorStyle style);

    ~VectorLoader();

    VectorLoader(const VectorLoader &) = delete;
    VectorLoader &operator=(const VectorLoader &) = delete;

    /// 启动后台加载（幂等：非 Idle 状态忽略）。构造后由宿主显式调用。内存模式为空操作。
    void start();

    /// 屏幕范围变化时按新范围重载（仅文件模式；内存模式空操作）。后台线程重读建新几何，就绪后经
    /// [drainReady] 由 GL 线程同帧替换旧几何（Swap-on-ready：替换前旧几何保持可绘制，重载期间无空窗）。
    /// 若后台正在加载：记录最新范围并置抢占标志，令当前读/建在下一要素边界提前中断、丢弃这份过期（可能不完整）
    /// 的结果，立即按最新范围重跑——避免一笔过期的巨型加载把最新的快速重载饿死（latest-wins）。
    void reload(bool hasExtent, double minLon, double minLat, double maxLon, double maxLat, int maxFeatures);

    /// 内存模式：推送/覆盖一批 WGS84 几何（动态叠加）。在调用线程同步 buildGeometry（纯 CPU、
    /// 数据量小），随后于锁内置 Ready 交接给 GL 线程（下一次 [drainReady] 取走上传）。可反复调用以
    /// 更新叠加内容（后到者覆盖）。文件模式下调用无效（忽略）。构建期间不持锁，避免阻塞 GL 线程。
    void setData(VectorReadResult data);

    /// GL 线程：几何就绪则移动取出（一次性），返回 true 并转 Consumed；否则 false。
    bool drainReady(VectorGeometry &out);

    /// 是否仍在加载或就绪待上传（供 Renderer 判断是否需再画一帧，驱动 RENDERMODE_WHEN_DIRTY 重绘）。
    bool hasPendingOrReady();

    /// 加载失败原因（Failed 状态时非空，供上层提示）。
    std::string error() const;

private:
    enum class State { Idle, Loading, Ready, Failed, Consumed };

    void run();

    /// 起/复用后台 worker（须持 mtx_ 调用）：worker 存活时仅置 rerunRequested_ 复用（完成后按最新范围
    /// 再来一轮）；否则回收上一轮已结束的线程后新建。避免并发双线程与后台读盘阻塞调用线程。
    void ensureWorkerLocked();

    std::string path_;
    VectorStyle style_;
    bool memMode_ = false;   // true=内存数据源（动态叠加，无文件/后台线程）；false=文件模式

    // 当前/最近一次请求的屏幕过滤范围（文件模式）；hasExtent_=false 表示整文件全量读取
    bool hasExtent_ = false;
    double minLon_ = 0.0, minLat_ = 0.0, maxLon_ = 0.0, maxLat_ = 0.0;
    int maxFeatures_ = 0;
    bool workerRunning_ = false;   // 后台线程正在读/建（持 mtx_ 读写）
    bool rerunRequested_ = false;  // 加载期间又收到新范围，完成后按最新范围再来一轮
    // 抢占标志（lock-free，供 buildGeometry 逐要素轮询）：reload 命中「worker 正忙」时置真，
    // worker 在读取/建几何各阶段边界检测到即提前中断本轮过期加载并立即按最新范围重跑；每轮开始清假。
    std::atomic<bool> stale_{false};

    mutable std::mutex mtx_;
    State state_ = State::Idle;
    VectorGeometry geom_;
    std::string error_;

    std::thread thread_;
    std::atomic<bool> abort_{false};
};

} // namespace globecore

#endif // GLOBECORE_LAYER_VECTOR_LOADER_H
