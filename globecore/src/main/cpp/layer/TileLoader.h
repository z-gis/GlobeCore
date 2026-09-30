#ifndef GLOBECORE_LAYER_TILE_LOADER_H
#define GLOBECORE_LAYER_TILE_LOADER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace globecore {

class TileCache;

/**
 * 后台瓦片加载器，对应 参考实现 layer.cache 的「磁盘/网络 TileStore + 异步加载管线」。
 *
 * 职责：异步取瓦片字节（先读磁盘缓存，未命中再联网下载并写盘）→ 交付 GL 线程上传纹理。
 * 关键：渲染（GL）线程绝不同步读盘/联网，只通过 [request] 投递需求、[drainReady] 取就绪字节（对齐 参考实现
 * getTexture 不阻塞渲染线程）；未就绪的瓦片由 Renderer 用祖先纹理兜底显示。
 *
 * 线程模型：构造时启动 numThreads 个工作线程共享一个请求队列；[request] 按 (z,x,y) 去重，
 * 已在队列或进行中的瓦片不会重复请求。工作线程先用 [TileCache] 读盘，未命中再用 [HttpClient] 拉取，
 * 成功则（联网时）原子写盘并放入 ready 队列；GL 线程每帧调用 [drainReady] 取走就绪字节做解码上传。
 * 所有公开方法线程安全。
 *
 * URL 模板：形如天地图 WMTS，含 `{x}`/`{y}`/`{z}` 与 `{rand=0,1,...}`（子域轮询）；
 * `$tiandituToken` 等 Token 由上层（Kotlin resolveUrl）替换后再经 [setUrlTemplate] 传入。
 * 空模板表示禁用联网（Phase B 行为：只读磁盘缓存）。
 */
class TileLoader {
public:
    /// 一块下载就绪的瓦片字节，交由 GL 线程解码上传
    struct ReadyTile {
        int z = 0;
        int x = 0;
        int y = 0;
        std::vector<uint8_t> bytes;
    };

    explicit TileLoader(TileCache &cache, int numThreads = 8);
    ~TileLoader();

    TileLoader(const TileLoader &) = delete;
    TileLoader &operator=(const TileLoader &) = delete;

    /// 设置 URL 模板（含占位符，Token 已替换）。空模板禁用联网。线程安全。
    void setUrlTemplate(std::string urlTemplate);
    bool hasUrlTemplate() const;

    /// 设置栅格数据源路径（本地 tif/img）。非空时，磁盘缓存未命中的瓦片由 [RasterRenderer] 按 (z,x,y)
    /// 的全球墨卡托经纬度四至即时重投影生成 PNG 字节（GDAL），成功后回写缓存。空路径禁用栅格生成。
    /// 与 URL 模板互斥使用（栅格层不设 URL）。线程安全。
    void setRasterSource(std::string path);
    bool hasRasterSource() const;

    /// 请求某瓦片（后台异步拉取）。已排队/进行中的会去重跳过；无模板或关机中直接忽略。
    /// 失败退避：读盘+联网（或栅格生成）均失败的瓦片在冷却窗口内（kFailCooldown）直接忽略重复请求，
    /// 防止大范围视图（3D 球面）触发服务限流（HTTP 429）后「失败→纹理驱逐→重请求→再失败」请求风暴死循环。
    void request(int z, int x, int y);

    /// 取走就绪瓦片（GL 线程每帧调用）。out 追加，不清空原有内容。
    /// [maxCount] 为本次最多取走的数量（<0 表示全部）：限制单帧解码上传量，避免一批瓦片同时就绪时
    /// GL 线程集中解码造成掉帧（时快时慢）；未取走的留在队列，下一帧继续（渐进补齐，视觉连续）。
    void drainReady(std::vector<ReadyTile> &out, int maxCount = -1);

    /// 是否仍有未完成的瓦片工作（排队/下载中 或 就绪待上传）。供 Renderer 判断是否需再画一帧：
    /// RENDERMODE_WHEN_DIRTY 省电模式下，瓦片异步到位需据此触发重绘；全部就绪后返回 false 停绘。
    bool hasPendingOrReady();

private:
    struct Request {
        int z, x, y;
    };

    static uint64_t key(int z, int x, int y);
    void workerLoop();
    std::string buildUrl(int z, int x, int y);

    TileCache &cache_;
    const int numThreads_;

    // 请求队列 + 去重集合（同一把锁保护）
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<uint64_t> pending_;
    // 失败退避表（mtx_ 保护）：key → 冷却截止时刻；成功交付或到期后移除
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> failedUntil_;
    bool stop_ = false;
    std::vector<std::thread> workers_;

    // 下载就绪队列（独立锁，GL 线程 drain、工作线程 push）
    std::mutex readyMtx_;
    std::deque<ReadyTile> ready_;

    // URL 模板（独立锁）+ 子域轮询计数 + 栅格数据源路径（共用 urlMtx_：二者互斥、均后台读）
    mutable std::mutex urlMtx_;
    std::string urlTemplate_;
    std::string rasterPath_;
    std::atomic<uint32_t> randCounter_{0};

    // 关机中断标志：置位后 curl 传输尽快中断，令 join 不长时间阻塞
    std::atomic<bool> abort_{false};
};

} // namespace globecore

#endif // GLOBECORE_LAYER_TILE_LOADER_H
