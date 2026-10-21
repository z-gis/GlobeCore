// geopackage_core.h —— GeoPackage 工程数据读/整写/增量写的平台无关核心（0.4.0.4 鸿蒙配套）。
//
// 读/整写逻辑从 bridge/geopackage_io.cpp 原样抽取（行为零变化，JSON 逐字节同构）；
// 增量 DML 按 Android 侧 GeoPackageDml.kt 逐一移植为 GDAL 实现（不直接依赖 sqlite3 符号，
// 一切走 OGRLayer CreateFeature/GetFeature/SetFeature/DeleteFeature + GDALDataset::ExecuteSQL 透传）。
// Android 仍走 geopackage_io.cpp（JNI 薄封装 + 平台 SQLite 增量写），本文件仅纳入鸿蒙构建。
#ifndef GLOBECORE_BRIDGE_GEOPACKAGE_CORE_H
#define GLOBECORE_BRIDGE_GEOPACKAGE_CORE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gcbridge {

// ── 整读 / 整写（JSON 与 Android Project.fromJson 兼容，逐字节同构）──

/// 从 GeoPackage 读取工程数据；文件不存在或结构不符返回空串（对应 JNI null）
std::string readGeoPackage(const std::string &path);

/// 创建 GeoPackage 并写入工程 JSON（内部 tmp+rename 原子替换；老库 schema 升级共用此入口）
bool writeGeoPackage(const std::string &path, const std::string &projectJson);

// ── schema 检查 / 准备 ──

/// 业务七表与增量写所需列是否齐备（文件不存在返回 true，由新建流程负责建库）
bool gpkgSchemaCurrent(const std::string &path);

/// 写前准备（幂等）：补建附件 BLOB 附加表 photo_att_data。
/// 鸿蒙走 GDAL 不需要清 rtree（GDAL 自带 rtree 模块，Android 清理是因平台 SQLite 无该模块）
bool gpkgPrepareSchema(const std::string &path);

// ── 增量 DML（JSON 传输，键与 Android 工程 JSON / GeoPackageDml 入参逐字一致）──

/// 新增轨迹：{"id","name","date","color","hidden","lineWidth","points":[[lon,lat,alt],...]}
/// id<=0 自动编号；返回实际 fid，失败 -1
long long gpkgInsertTrack(const std::string &path, const std::string &json);

/// 更新轨迹样式：{"name","color","hidden","lineWidth"}（几何不动，对齐 updateTrack）
bool gpkgUpdateTrack(const std::string &path, long long id, const std::string &json);

bool gpkgDeleteTrack(const std::string &path, long long id);

/// 新增测量：{"id","name","date","type","result","color","hidden","closed","lineWidth","points"}
/// type=point→点，area/polygon→面，其余→线；返回实际 fid，失败 -1
long long gpkgInsertMeasure(const std::string &path, const std::string &json);

/// 更新测量样式：{"color","lineWidth"}（对齐 updateMeasureStyle）
bool gpkgUpdateMeasure(const std::string &path, long long id, const std::string &json);

bool gpkgDeleteMeasure(const std::string &path, long long id);

/// 新增照片点位：{"id","name","lon","lat","alt","timestamp","visible","groupId"}；返回 fid，失败 -1
long long gpkgInsertPhoto(const std::string &path, const std::string &json);

/// 更新照片列（键出现才更新，对齐 updatePhotoColumns 可空语义）：
/// {"name"?,"groupId"?,"visible"?,"attachments"?,"photo_path"?}
/// 其中 attachments/photo_path 为鸿蒙补充键（Android 由 addPhotoAttachment 一并写入，
/// 鸿蒙 NAPI 签名不带 JSON，拆到本函数）；空串写 NULL，对齐 putStr
bool gpkgUpdatePhoto(const std::string &path, long long id, const std::string &json);

/// 删除照片（连同 photo_att_data 附件行）
bool gpkgDeletePhoto(const std::string &path, long long id);

/// 写照片 BLOB：idx=0 主照片 photos.photo_data（OGR 二进制列）；idx>=1 附件 photo_att_data
/// （ExecuteSQL x'hex' 字面量 INSERT OR REPLACE）
bool gpkgAddPhotoAttachment(const std::string &path, long long fid, long long idx,
                            const void *data, size_t size);

/// 读照片 BLOB：idx=0 主照片，idx>=1 附件（SELECT hex(data) 解码）；无记录返回 false
bool gpkgReadPhotoBlob(const std::string &path, long long fid, long long idx,
                       std::vector<uint8_t> &out);

/// 新增分组：{"id","name","createdTime","visible"}（fid 即 id）
bool gpkgInsertGroup(const std::string &path, const std::string &json);

/// 更新分组名：{"id","name"}
bool gpkgUpdateGroup(const std::string &path, const std::string &json);

/// 删除分组：组内照片移到未分组（group_id=-1，与内存语义一致）
bool gpkgDeleteGroup(const std::string &path, long long id);

/// 分组内照片批量显隐
bool gpkgSetGroupPhotosVisible(const std::string &path, long long groupId, bool visible);

/// 样地 upsert（fid 冲突替换）：{"id","name","plotType","attributes":object|string,"points"}
/// plotType=point→点，其余→面（自动闭合）
bool gpkgUpsertSurveyPlot(const std::string &path, const std::string &json);

/// 删除样地（按 fid DeleteFeature）
bool gpkgDeleteSurveyPlot(const std::string &path, long long id);

/// 元数据一把梭：project_meta 五键 upsert（空值删行）+ layer_refs 全替换 + finish 元数据。
/// {"name"?,"description"?,"srs"?,"createTime"?,"updateTime"?,"layerRefs"?:[string,...]}
bool gpkgSyncMeta(const std::string &path, const std::string &json);

/// 照片文件清扫（幂等，打开工程时后台执行，语义照 GeoPackageDml.sweepPhotoFiles）：
/// BLOB 已在库 → 删磁盘原件；BLOB 缺失且磁盘原件存在 → 读入库后删文件。
/// 仅处理 mediaRoot 目录下文件；返回是否发生任何清扫
bool gpkgSweepPhotoFiles(const std::string &path, const std::string &mediaRoot);

} // namespace gcbridge

#endif // GLOBECORE_BRIDGE_GEOPACKAGE_CORE_H
