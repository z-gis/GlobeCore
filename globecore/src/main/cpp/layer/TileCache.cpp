#include "layer/TileCache.h"

#include "util/Log.h"

#include <cstdio>
#include <sys/stat.h>
#include <utility>

namespace globecore {

TileCache::TileCache(std::string baseDir) : baseDir_(std::move(baseDir)) {}

void TileCache::setBaseDir(std::string baseDir) {
    baseDir_ = std::move(baseDir);
    LOGI("TileCache baseDir=%s", baseDir_.c_str());
}

bool TileCache::readTile(int z, int x, int y, std::vector<uint8_t> &outBytes) const {
    if (baseDir_.empty()) return false;

    // 路径：<baseDir>/<z>/<x>_<y>.tile
    char path[2048];
    const int written = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (written <= 0 || written >= static_cast<int>(sizeof(path))) return false;

    FILE *f = fopen(path, "rb");
    if (f == nullptr) return false;

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    const size_t read = fread(outBytes.data(), 1, static_cast<size_t>(size), f);
    fclose(f);

    if (read != static_cast<size_t>(size)) {
        outBytes.clear();
        return false;
    }
    return true;
}

bool TileCache::writeTile(int z, int x, int y, const std::vector<uint8_t> &bytes) const {
    if (baseDir_.empty() || bytes.empty()) return false;

    // 逐级确保目录存在（baseDir 可能尚未创建；mkdir 对已存在目录返回 EEXIST，忽略即可）
    mkdir(baseDir_.c_str(), 0777);
    char dir[2048];
    int n = snprintf(dir, sizeof(dir), "%s/%d", baseDir_.c_str(), z);
    if (n <= 0 || n >= static_cast<int>(sizeof(dir))) return false;
    mkdir(dir, 0777);

    char path[2048];
    n = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (n <= 0 || n >= static_cast<int>(sizeof(path))) return false;
    char tmp[2112];
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || n >= static_cast<int>(sizeof(tmp))) return false;

    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) return false;
    const size_t wrote = fwrite(bytes.data(), 1, bytes.size(), f);
    fflush(f);
    fclose(f);
    if (wrote != bytes.size()) {
        remove(tmp);
        return false;
    }
    // 原子重命名：保证读端要么看不到、要么看到完整文件
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

void TileCache::removeTile(int z, int x, int y) const {
    if (baseDir_.empty()) return;
    char path[2048];
    const int written = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (written <= 0 || written >= static_cast<int>(sizeof(path))) return;
    remove(path); // 不存在时失败即忽略（自愈语义，无需区分）
}

} // namespace globecore
