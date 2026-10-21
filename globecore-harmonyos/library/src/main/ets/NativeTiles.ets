// NativeTiles.ets —— 瓦片缓存维护门面（对应 Android MapLayerManager 的瓦片迁移段，
// 实现见 library cpp bridge/tile_migrate.cpp gcbridge 核心 + ohos/napi_geopackage.cpp NAPI 导出）。
//
// 旧目录式瓦片缓存（<dir>/<z>/<x>_<y>.tile）一次性迁入单文件 MBTiles：INSERT OR REPLACE 幂等、
// TMS y 翻转与引擎 TileCache 单文件后端同构、schema 逐字一致（含非标 added_at 列）。
// 鸿蒙 ArkTS 无任意路径 SQLite API（relationalStore 只能开 RDB 目录库），故迁移走引擎 C++。
import gc from 'libglobecore.so';

export class NativeTiles {

  /**
   * 旧目录式瓦片缓存迁入单文件 MBTiles（同步阻塞；全部迁完才删旧目录，失败回滚保留旧目录）。
   * @param dir 旧缓存目录（不存在/非目录 → 零开销 no-op）
   * @param mbtiles 目标 .mbtiles 文件（不存在则建）
   * @returns 迁移块数；0 = 无旧目录或无可迁瓦片；-1 = 失败（保留旧目录，下次重试）
   */
  static nativeMigrateTileDir(dir: string, mbtiles: string): number {
    return gc.nativeMigrateTileDir(dir, mbtiles);
  }
}
