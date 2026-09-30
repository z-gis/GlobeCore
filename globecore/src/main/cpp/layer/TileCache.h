#ifndef GLOBECORE_LAYER_TILE_CACHE_H
#define GLOBECORE_LAYER_TILE_CACHE_H

#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * 文件式瓦片缓存读取，对应 参考实现 layer.cache 的 TileStore（只读部分）。
 *
 * 直接沿用原 app `FileTileStore`（已删除）的磁盘布局：`<baseDir>/<z>/<x>_<y>.tile`，
 * 其中 baseDir 为「某图源」的缓存目录（形如 `/调查宝/tiles/天地图-影像`，由 Kotlin 侧传入已 sanitize 的绝对路径）。
 * 行序为标准 XYZ（y 自北向南），与 [TileMatrix] 计算一致，可直接命中 参考实现 已缓存的瓦片。
 *
 * Phase B 只做「读」；Phase C 联网下载后用 [writeTile] 写盘（.tmp 原子重命名，与原 FileTileStore 一致）。
 */
class TileCache {
public:
    TileCache() = default;
    explicit TileCache(std::string baseDir);

    void setBaseDir(std::string baseDir);
    const std::string &baseDir() const { return baseDir_; }
    bool isReady() const { return !baseDir_.empty(); }

    /**
     * 读取瓦片字节。命中返回 true 并填充 outBytes；未命中或读取失败返回 false。
     */
    bool readTile(int z, int x, int y, std::vector<uint8_t> &outBytes) const;

    /**
     * 写入瓦片字节（Phase C 联网下载后落盘）：自动创建 `<baseDir>/<z>/` 目录，
     * 先写 `.tmp` 再原子 rename（与原 FileTileStore 一致，避免读到半个文件）。多线程对不同瓦片并发安全。
     * @return 成功返回 true；baseDir/bytes 为空或 IO 失败返回 false
     */
    bool writeTile(int z, int x, int y, const std::vector<uint8_t> &bytes) const;

    /**
     * 删除磁盘瓦片文件（坏瓦片自愈：读盘命中的字节解码失败时清除，防坏文件永久占位）。
     * 不存在或删除失败静默返回。
     */
    void removeTile(int z, int x, int y) const;

private:
    std::string baseDir_;
};

} // namespace globecore

#endif // GLOBECORE_LAYER_TILE_CACHE_H
