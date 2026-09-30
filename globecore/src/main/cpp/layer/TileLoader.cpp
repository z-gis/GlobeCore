#include "layer/TileLoader.h"

#include "layer/TileCache.h"
#include "globe/MercatorProjection.h"
#include "net/HttpClient.h"
#include "raster/RasterRenderer.h"
#include "util/Log.h"

#include <cmath>
#include <utility>

namespace globecore {

namespace {
/// 失败退避冷却时长：读盘+联网均失败后，同 key 请求在此窗口内直接忽略（见 TileLoader::request 注释）
constexpr auto kFailCooldown = std::chrono::seconds(30);

/// 就绪队列高水位：ready_ 是 GL 线程 drainReady（每帧仅取 kMaxDecodePerFrame）与工作线程 push 之间的
/// 缓冲。3D 倾斜视域下 coverage-first 使可见叶暴涨到数千，工作线程下载速度远超 GL 解码消费，无界 ready_
/// 会堆积上万块下载字节（每块 ~10-20KB）撑爆 native 堆（实测 Scudo exhausted 256M 崩溃）。超水位时丢弃
/// 本次交付——联网字节已 writeTile 落盘，后续重请求走读盘秒得，仅牺牲一批陈旧字节的即时性，杜绝 OOM。
constexpr size_t kMaxReadyTiles = 1024;
/// 请求队列上限：3D 连续手势下视角每帧一换（一帧可见集可达数百块），过期请求会在队列里
/// 大量堆积；超限时从队首丢弃最旧的（属于已滑出的旧视角，下帧起不再需要），为当前视角让路。
constexpr size_t kMaxQueue = 1200;

/// 图像格式魔数校验（PNG/JPEG/WEBP/GIF/BMP）：拦截图源「HTTP 200 + 异常 XML/HTML 错误页」
/// 被当作有效瓦片落盘污染缓存（实测天地图限流时回 200+约 100B 的 ExceptionReport）。
bool looksLikeImage(const std::vector<uint8_t> &b) {
    if (b.size() < 12) return false;
    const auto *p = b.data();
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return true;                       // PNG
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;                                     // JPEG
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8') return true;                         // GIF
    if (p[0] == 'B' && p[1] == 'M') return true;                                                       // BMP
    if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') return true;                       // WEBP
    return false;
}

/// 全球墨卡托瓦片 (z,x,y) → WGS84 经纬度四至（标准 XYZ，y 自北向南）。
/// 归一化世界坐标下瓦片覆盖 [x/2^z,(x+1)/2^z]×[y/2^z,(y+1)/2^z]；wy 小端为北侧（大纬度）。
void tileBounds(int z, int x, int y, double &minLon, double &minLat, double &maxLon, double &maxLat) {
    const double n = static_cast<double>(1LL << z); // 2^z（与 TileMatrix::tilesAtLevel 同口径）
    double lonNw = 0.0, latNw = 0.0, lonSe = 0.0, latSe = 0.0;
    MercatorProjection::worldToLonLat(x / n, y / n, lonNw, latNw);            // 西北角 → maxLat
    MercatorProjection::worldToLonLat((x + 1) / n, (y + 1) / n, lonSe, latSe); // 东南角 → minLat
    minLon = lonNw; maxLon = lonSe; maxLat = latNw; minLat = latSe;
}
} // namespace

uint64_t TileLoader::key(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 40) |
           (static_cast<uint64_t>(x) << 20) |
           static_cast<uint64_t>(y);
}

TileLoader::TileLoader(TileCache &cache, int numThreads)
    : cache_(cache), numThreads_(numThreads > 0 ? numThreads : 1) {
    HttpClient::globalInit();
    workers_.reserve(static_cast<size_t>(numThreads_));
    for (int i = 0; i < numThreads_; ++i) {
        workers_.emplace_back(&TileLoader::workerLoop, this);
    }
    LOGI("TileLoader started threads=%d", numThreads_);
}

TileLoader::~TileLoader() {
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable()) t.join();
    }
    LOGI("TileLoader stopped");
}

void TileLoader::setUrlTemplate(std::string urlTemplate) {
    std::lock_guard<std::mutex> lk(urlMtx_);
    urlTemplate_ = std::move(urlTemplate);
    LOGI("TileLoader urlTemplate set (len=%zu, net=%s)",
         urlTemplate_.size(), urlTemplate_.empty() ? "off" : "on");
}

bool TileLoader::hasUrlTemplate() const {
    std::lock_guard<std::mutex> lk(urlMtx_);
    return !urlTemplate_.empty();
}

void TileLoader::setRasterSource(std::string path) {
    std::lock_guard<std::mutex> lk(urlMtx_);
    rasterPath_ = std::move(path);
    LOGI("TileLoader rasterSource set (len=%zu, gen=%s)",
         rasterPath_.size(), rasterPath_.empty() ? "off" : "on");
}

bool TileLoader::hasRasterSource() const {
    std::lock_guard<std::mutex> lk(urlMtx_);
    return !rasterPath_.empty();
}

void TileLoader::request(int z, int x, int y) {
    // 不再因无 URL 模板而跳过：磁盘读取也走本异步管线（GL 线程绝不同步读盘，对齐 参考实现 getTexture 不阻塞渲染线程）。
    const uint64_t k = key(z, x, y);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        if (pending_.count(k) != 0) return; // 去重：已排队或进行中
        // 失败退避：冷却窗口内不重复请求；到期则放行本次重试（下次仍失败会重新记入）
        auto fit = failedUntil_.find(k);
        if (fit != failedUntil_.end()) {
            if (std::chrono::steady_clock::now() < fit->second) return;
            failedUntil_.erase(fit);
        }
        pending_.insert(k);
        queue_.push_back(Request{z, x, y});
        // 超限丢最旧：被丢者同步移出 pending_，若仍可见下一帧会重新入队（去重不受损）
        while (queue_.size() > kMaxQueue) {
            const Request &oldest = queue_.front();
            pending_.erase(key(oldest.z, oldest.x, oldest.y));
            queue_.pop_front();
        }
    }
    cv_.notify_one();
}

void TileLoader::drainReady(std::vector<ReadyTile> &out, int maxCount) {
    std::lock_guard<std::mutex> lk(readyMtx_);
    int taken = 0;
    while (!ready_.empty() && (maxCount < 0 || taken < maxCount)) {
        out.push_back(std::move(ready_.front()));
        ready_.pop_front();
        ++taken;
    }
}

bool TileLoader::hasPendingOrReady() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!pending_.empty()) return true;
    }
    {
        std::lock_guard<std::mutex> lk(readyMtx_);
        if (!ready_.empty()) return true;
    }
    return false;
}

void TileLoader::workerLoop() {
    while (true) {
        Request req{0, 0, 0};
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (queue_.empty()) continue;
            // LIFO 取最新请求：连续手势下视角每帧一换，FIFO 会让工作线程一直下载已滑出
            // 屏幕的旧视角瓦片，当前视角排在队尾迟迟轮不上（3D 移动时"慢、停下才补齐"）。
            // 队首的陈旧项由 kMaxQueue 超限丢弃兜底，不会无限积压。
            req = queue_.back();
            queue_.pop_back();
        }

        // 1) 先读磁盘缓存（异步，避免 GL 线程阻塞）；命中则直接交付，不联网。
        //    魔数校验防坏文件（历史污染的异常 XML 瓦片）：检出即删除自愈（removeTile），按未命中
        //    继续走下载/失败冷却——若不过滤，读盘命中坏瓦会被解码失败却计为成功，令退避失效并逐帧重刷。
        std::vector<uint8_t> bytes;
        bool haveBytes = cache_.readTile(req.z, req.x, req.y, bytes) && !bytes.empty();
        if (haveBytes && !looksLikeImage(bytes)) {
            LOGW("磁盘瓦片损坏，已清除 z=%d x=%d y=%d size=%zu", req.z, req.x, req.y, bytes.size());
            cache_.removeTile(req.z, req.x, req.y);
            bytes.clear();
            haveBytes = false;
        }
        // 2) 磁盘未命中 → 栅格层：按 (z,x,y) 四至用 GDAL 重投影即时生成 PNG 字节，成功则回写缓存
        if (!haveBytes) {
            std::string rasterPath;
            {
                std::lock_guard<std::mutex> lk(urlMtx_);
                rasterPath = rasterPath_;
            }
            if (!rasterPath.empty()) {
                bytes.clear();
                double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
                tileBounds(req.z, req.x, req.y, minLon, minLat, maxLon, maxLat);
                if (RasterRenderer::renderTile(rasterPath, minLon, minLat, maxLon, maxLat, 256, bytes)
                    && !bytes.empty()) {
                    cache_.writeTile(req.z, req.x, req.y, bytes);
                    haveBytes = true;
                }
                // 无交集/生成失败：不联网（栅格层无 URL），本瓦片无字节，由祖先兜底或保持透明
            }
        }
        // 3) 仍未命中 → 联网拉取（可被 abort_ 中断），成功则原子写盘。
        //    响应体须过图像魔数校验才落盘/交付：图源限流时以 HTTP 200 回异常 XML，不可当瓦片污染缓存。
        if (!haveBytes) {
            bytes.clear();
            const std::string url = buildUrl(req.z, req.x, req.y);
            bool ok = !url.empty() &&
                      HttpClient::get(url, bytes, 15000, &abort_) && !bytes.empty();
            if (ok && !looksLikeImage(bytes)) {
                if (!abort_.load()) {
                    LOGW("响应非图像（图源异常/限流页）z=%d x=%d y=%d size=%zu 不落盘",
                         req.z, req.x, req.y, bytes.size());
                }
                bytes.clear();
                ok = false;
            }
            if (ok) {
                cache_.writeTile(req.z, req.x, req.y, bytes);
                haveBytes = true;
            } else if (!abort_.load()) {
                LOGW("瓦片读盘/下载均失败 z=%d x=%d y=%d", req.z, req.x, req.y);
            }
        }
        if (haveBytes) {
            std::lock_guard<std::mutex> lk(readyMtx_);
            // 高水位背压：ready_ 逼近上限时丢弃本次交付（字节已落盘，下帧重请求读盘秒得），防无界堆积 OOM
            if (ready_.size() < kMaxReadyTiles) {
                ReadyTile rt;
                rt.z = req.z;
                rt.x = req.x;
                rt.y = req.y;
                rt.bytes = std::move(bytes);
                ready_.push_back(std::move(rt));
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            const uint64_t k = key(req.z, req.x, req.y);
            pending_.erase(k);
            // 失败退避以「取字节是否成功」为准（非是否交付）：高水位丢弃的瓦片字节已在盘，
            // 不应误记冷却——否则会在 30s 内压制其重请求，远景兜底图迟迟补不上。
            if (haveBytes) {
                failedUntil_.erase(k); // 成功取到字节：清除旧失败标记（下帧可立即重请求读盘秒得）
            } else if (!abort_.load()) {
                // 失败退避：记录冷却截止时刻，窗口内 request() 直接忽略（防限流风暴死循环）
                failedUntil_[k] = std::chrono::steady_clock::now() + kFailCooldown;
            }
        }
    }
}

std::string TileLoader::buildUrl(int z, int x, int y) {
    std::string tmpl;
    {
        std::lock_guard<std::mutex> lk(urlMtx_);
        tmpl = urlTemplate_;
    }
    if (tmpl.empty()) return std::string();

    std::string url = std::move(tmpl);

    // {rand=a,b,c,...} → 轮询取一个子域（均衡负载，避免随机碰撞）
    const size_t rp = url.find("{rand=");
    if (rp != std::string::npos) {
        const size_t rpEnd = url.find('}', rp);
        if (rpEnd != std::string::npos) {
            const std::string list = url.substr(rp + 6, rpEnd - (rp + 6));
            std::vector<std::string> opts;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                if (comma == std::string::npos) {
                    opts.push_back(list.substr(start));
                    break;
                }
                opts.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
            std::string chosen;
            if (!opts.empty()) {
                const uint32_t idx = randCounter_.fetch_add(1) %
                                     static_cast<uint32_t>(opts.size());
                chosen = opts[idx];
            }
            url.replace(rp, rpEnd - rp + 1, chosen);
        }
    }

    // {z} / {x} / {y} → 瓦片坐标
    auto replaceAll = [](std::string &s, const char *from, const std::string &to) {
        const size_t flen = std::string(from).size();
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, flen, to);
            pos += to.size();
        }
    };
    replaceAll(url, "{z}", std::to_string(z));
    replaceAll(url, "{x}", std::to_string(x));
    replaceAll(url, "{y}", std::to_string(y));
    return url;
}

} // namespace globecore
