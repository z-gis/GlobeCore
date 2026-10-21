// tile_migrate.cpp —— 旧目录式瓦片缓存 → 单文件 MBTiles 一次性迁移（实现见头文件注释）。
//
// 与 Android migrateLegacyTileDirStatic 的逐点对应：
//   · dir 不存在/非目录直接返回（对应 !dir.isDirectory return）；
//   · GDAL SQLite 驱动 UPDATE 打开 mbtiles（不存在则 Create，对应 openOrCreateDatabase）；
//   · tiles/metadata 建表 + idx_tiles 索引，DDL 与 Android 逐字一致；
//   · BEGIN 事务内逐块 INSERT OR REPLACE（z 目录纯数字、文件名 ^(\d+)_(\d+)\.tile$、
//     非空文件才迁，TMS 翻转 row=(1<<z)-1-y，added_at=文件 mtime 秒、缺失回退当前时间）；
//   · COMMIT 后 remove_all 删旧目录；任一步失败 ROLLBACK、保留旧目录、返回 -1。
#include "tile_migrate.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "gdal/ogrsf_frmts.h"
#include "gdal/gdal_priv.h"
#include "gdal/cpl_error.h"

#include "util/Log.h"

namespace gcbridge {

namespace {

/// SQL 透传并判定成败（SQLITE 方言直交 SQLite；迁移不同于增量写，失败必须可感知以触发回滚）
bool execSql(GDALDataset *ds, const std::string &sql) {
    CPLErrorReset();
    OGRLayer *res = ds->ExecuteSQL(sql.c_str(), nullptr, "SQLITE");
    const bool ok = CPLGetLastErrorType() == CE_None;
    if (res != nullptr) ds->ReleaseResultSet(res);
    return ok;
}

/// 二进制 → 小写 hex（BLOB 经 x'hex' 字面量拼 SQL，同 geopackage_core.cpp hexEncode）
std::string hexEncode(const void *data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    const auto *p = static_cast<const unsigned char *>(data);
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = kHex[p[i] >> 4];
        out[i * 2 + 1] = kHex[p[i] & 0x0F];
    }
    return out;
}

/// 目录名纯数字解析（对应 Kotlin toIntOrNull：非纯数字/越界返回 false）
bool parseZoom(const std::string &name, int &out) {
    if (name.empty() || name.size() > 9) return false;
    for (char c : name) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    const long v = std::strtol(name.c_str(), nullptr, 10);
    if (v < 0 || v > 30) return false; // 超出 (1<<z) 合理范围的目录名不算瓦片层级
    out = static_cast<int>(v);
    return true;
}

/// 文件 mtime 秒（≈原下载落盘时刻）；读取失败/非正回退当前时间（对应 lastModified>0 ?: nowSec）
long long fileMtimeSec(const std::filesystem::path &p, long long fallback) {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) return fallback;
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
    const long long sec = static_cast<long long>(std::chrono::system_clock::to_time_t(sctp));
    return sec > 0 ? sec : fallback;
}

/// 读整个文件字节；失败/空文件返回 false
bool readFileBytes(const std::filesystem::path &p, std::vector<uint8_t> &out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size == 0) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    if (!in) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace

int gpkgMigrateTileDir(const char *dirPath, const char *mbtilesPath) {
    if (dirPath == nullptr || mbtilesPath == nullptr || dirPath[0] == '\0' || mbtilesPath[0] == '\0') {
        return 0;
    }
    const std::filesystem::path dir(dirPath);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return 0; // 无旧目录：零开销 no-op（对应 !dir.isDirectory return）
    }

    GDALDataset *ds = nullptr;
    bool inTransaction = false;
    int migrated = 0;
    try {
        // 目标父目录预建（对应 mbtiles.parentFile?.mkdirs()）
        const std::filesystem::path parent = std::filesystem::path(mbtilesPath).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        GDALAllRegister();
        ds = static_cast<GDALDataset *>(
                GDALOpenEx(mbtilesPath, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (ds == nullptr) {
            // 文件不存在：经 SQLite 驱动新建（nBands=0 + GDT_Unknown → 矢量库）
            GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("SQLite");
            if (drv != nullptr) {
                ds = drv->Create(mbtilesPath, 0, 0, 0, GDT_Unknown, nullptr);
            }
        }
        if (ds == nullptr) {
            LOGE("瓦片迁移: mbtiles 打开/创建失败: %s", mbtilesPath);
            return -1;
        }

        // schema 与引擎 TileCache 单文件后端逐字一致（含非标 added_at 列）
        if (!execSql(ds, "CREATE TABLE IF NOT EXISTS tiles (zoom_level INTEGER, tile_column INTEGER, "
                         "tile_row INTEGER, tile_data BLOB, added_at INTEGER, "
                         "PRIMARY KEY (zoom_level, tile_column, tile_row))")
            || !execSql(ds, "CREATE TABLE IF NOT EXISTS metadata (name TEXT, value TEXT)")
            || !execSql(ds, "CREATE INDEX IF NOT EXISTS idx_tiles ON tiles (zoom_level, tile_column, tile_row)")) {
            LOGE("瓦片迁移: 建表失败: %s", mbtilesPath);
            GDALClose(ds);
            return -1;
        }

        if (!execSql(ds, "BEGIN")) {
            LOGE("瓦片迁移: 事务开启失败: %s", mbtilesPath);
            GDALClose(ds);
            return -1;
        }
        inTransaction = true;

        const std::regex tileNameRe(R"(^(\d+)_(\d+)\.tile$)");
        const long long nowSec = static_cast<long long>(std::time(nullptr));
        for (const auto &zEntry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!zEntry.is_directory(ec) || ec) continue;
            int z = -1;
            if (!parseZoom(zEntry.path().filename().string(), z)) continue;
            const long long tmsRowMax = (1LL << z) - 1;
            for (const auto &fEntry : std::filesystem::directory_iterator(zEntry.path(), ec)) {
                if (ec) break;
                if (!fEntry.is_regular_file(ec) || ec) continue;
                std::smatch m;
                const std::string fname = fEntry.path().filename().string();
                if (!std::regex_match(fname, m, tileNameRe)) continue;
                const long long x = std::strtoll(m[1].str().c_str(), nullptr, 10);
                const long long y = std::strtoll(m[2].str().c_str(), nullptr, 10);
                std::vector<uint8_t> bytes;
                if (!readFileBytes(fEntry.path(), bytes)) {
                    if (fEntry.file_size(ec) == 0 && !ec) continue; // 空文件跳过（对应 length()==0L continue）
                    LOGE("瓦片迁移: 读取失败: %s", fEntry.path().string().c_str());
                    throw std::runtime_error("read tile failed");
                }
                const long long addedAt = fileMtimeSec(fEntry.path(), nowSec);
                const std::string sql =
                        "INSERT OR REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data, added_at) VALUES ("
                        + std::to_string(z) + "," + std::to_string(x) + ","
                        + std::to_string(tmsRowMax - y) + ",x'" + hexEncode(bytes.data(), bytes.size())
                        + "'," + std::to_string(addedAt) + ")";
                if (!execSql(ds, sql)) {
                    LOGE("瓦片迁移: 入库失败: %s", fEntry.path().string().c_str());
                    throw std::runtime_error("insert tile failed");
                }
                migrated++;
            }
        }

        if (!execSql(ds, "COMMIT")) {
            LOGE("瓦片迁移: 事务提交失败: %s", mbtilesPath);
            throw std::runtime_error("commit failed");
        }
        inTransaction = false;
        GDALClose(ds);
        ds = nullptr;

        // 全部迁完才删旧目录（对应 dir.deleteRecursively()）
        std::filesystem::remove_all(dir, ec);
        LOGI("瓦片缓存迁移完成: %s → %s 共%d块", dirPath, mbtilesPath, migrated);
        return migrated;
    } catch (const std::exception &e) {
        if (ds != nullptr) {
            if (inTransaction) execSql(ds, "ROLLBACK");
            GDALClose(ds);
        }
        LOGE("瓦片缓存迁移失败（保留旧目录，下次重试）: %s: %s", dirPath, e.what());
        return -1;
    }
}

} // namespace gcbridge
