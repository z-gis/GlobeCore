// tile_migrate.h —— 旧目录式瓦片缓存一次性迁入单文件 MBTiles 的平台无关核心（0.4.0.4 鸿蒙配套）。
//
// 语义逐字对齐 Android MapLayerManager.migrateLegacyTileDirStatic：
//   `<dir>/<z>/<x>_<y>.tile` → MBTiles tiles 表（INSERT OR REPLACE 幂等，TMS y 翻转，
//   schema 与引擎 TileCache 单文件后端逐字一致，含非标 added_at 列）。
// 鸿蒙 ArkTS 无任意路径 SQLite API（relationalStore 只能开 RDB 目录库），迁移只能由引擎
// C++ 完成；实现走 GDAL SQLite 驱动（ExecuteSQL 透传，不直接依赖 sqlite3 符号），
// x'hex' BLOB 字面量写法照 bridge/geopackage_core.cpp photo_att_data 既有模式。
// 本文件仅纳入鸿蒙构建（Android 侧仍用平台 SQLite 实现）。
#ifndef GLOBECORE_BRIDGE_TILE_MIGRATE_H
#define GLOBECORE_BRIDGE_TILE_MIGRATE_H

namespace gcbridge {

/**
 * 旧目录式瓦片缓存迁入单文件 MBTiles。
 * @param dirPath     旧缓存目录（不存在/非目录 → 零开销 no-op，返回 0）
 * @param mbtilesPath 目标 .mbtiles 文件（不存在则建，父目录自动创建）
 * @return 迁移瓦片块数；0 = 无旧目录或无有效瓦片；-1 = 失败（事务回滚、保留旧目录，下次重试）
 */
int gpkgMigrateTileDir(const char *dirPath, const char *mbtilesPath);

} // namespace gcbridge

#endif // GLOBECORE_BRIDGE_TILE_MIGRATE_H
