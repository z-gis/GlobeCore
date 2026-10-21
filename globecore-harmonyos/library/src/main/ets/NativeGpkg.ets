// NativeGpkg.ets —— GeoPackage 工程数据读写门面（对应 Android GeoPackageWriter.kt + GeoPackageDml.kt，
// 实现见 engine bridge/geopackage_core.cpp gcbridge 核心 + ohos/napi_geopackage.cpp NAPI 导出）。
//
// 整读写走 GDAL（tmp+rename 原子替换，仅新建工程/老库 schema 升级用）；日常增量写走
// GDAL 增量 DML（CreateFeature/SetFeature/DeleteFeature + ExecuteSQL）。照片本体 BLOB 入库：
// 主照片 photos.photo_data、第 2..N 张附件 photo_att_data（未注册表，GDAL/QGIS 不可见）。
// JSON 形状与 Android 工程 JSON 逐字一致（详见 index.d.ts 各函数注释）。
import gc from 'libglobecore.so';

export class NativeGpkg {

  /** 读工程 JSON（与 Android Project.fromJson 兼容）；打不开/结构不符 null */
  static nativeReadGeoPackage(path: string): string | null {
    return gc.nativeReadGeoPackage(path);
  }

  /** 整写工程（新建 + 老库 schema 升级共用；tmp+rename 原子替换） */
  static nativeWriteGeoPackage(path: string, json: string): boolean {
    return gc.nativeWriteGeoPackage(path, json);
  }

  /** 业务七表与增量写所需列是否齐备（文件不存在 true，由新建流程负责建库） */
  static nativeGpkgSchemaCurrent(path: string): boolean {
    return gc.nativeGpkgSchemaCurrent(path);
  }

  /** 写前准备（幂等）：补建附件 BLOB 附加表 photo_att_data */
  static nativeGpkgPrepareSchema(path: string): boolean {
    return gc.nativeGpkgPrepareSchema(path);
  }

  /** 插入轨迹，返回 fid（失败 -1）；json 含 id/name/date/color/hidden/lineWidth/points */
  static nativeGpkgInsertTrack(path: string, json: string): number {
    return gc.nativeGpkgInsertTrack(path, json);
  }

  /** 更新轨迹属性（几何不动）：json {"name"?,"color"?,"hidden"?,"lineWidth"?} */
  static nativeGpkgUpdateTrack(path: string, id: number, json: string): boolean {
    return gc.nativeGpkgUpdateTrack(path, id, json);
  }

  static nativeGpkgDeleteTrack(path: string, id: number): boolean {
    return gc.nativeGpkgDeleteTrack(path, id);
  }

  /** 插入测量，返回 fid（失败 -1）；type=point→点，area/polygon→面，其余→线 */
  static nativeGpkgInsertMeasure(path: string, json: string): number {
    return gc.nativeGpkgInsertMeasure(path, json);
  }

  /** 更新测量样式：json {"color"?,"lineWidth"?} */
  static nativeGpkgUpdateMeasure(path: string, id: number, json: string): boolean {
    return gc.nativeGpkgUpdateMeasure(path, id, json);
  }

  static nativeGpkgDeleteMeasure(path: string, id: number): boolean {
    return gc.nativeGpkgDeleteMeasure(path, id);
  }

  /** 插入照片点位行，返回 fid（失败 -1）；BLOB 由 nativeGpkgAddPhotoAttachment 写入 */
  static nativeGpkgInsertPhoto(path: string, json: string): number {
    return gc.nativeGpkgInsertPhoto(path, json);
  }

  /** 更新照片列（键出现才更新）：{"name"?,"groupId"?,"visible"?,"attachments"?,"photo_path"?} */
  static nativeGpkgUpdatePhoto(path: string, id: number, json: string): boolean {
    return gc.nativeGpkgUpdatePhoto(path, id, json);
  }

  /** 删除照片（连同 photo_att_data 附件行） */
  static nativeGpkgDeletePhoto(path: string, id: number): boolean {
    return gc.nativeGpkgDeletePhoto(path, id);
  }

  /** 写照片 BLOB：idx=0 主照片 photos.photo_data；idx>=1 附件 photo_att_data（INSERT OR REPLACE） */
  static nativeGpkgAddPhotoAttachment(path: string, fid: number, idx: number, data: ArrayBuffer): boolean {
    return gc.nativeGpkgAddPhotoAttachment(path, fid, idx, data);
  }

  /** 读照片 BLOB：idx=0 主照片，idx>=1 附件；无记录 null */
  static nativeGpkgReadPhotoBlob(path: string, fid: number, idx: number): ArrayBuffer | null {
    return gc.nativeGpkgReadPhotoBlob(path, fid, idx);
  }

  /** 插入媒体分组：json {"id","name","createdTime","visible"}（fid 即 id） */
  static nativeGpkgInsertGroup(path: string, json: string): boolean {
    return gc.nativeGpkgInsertGroup(path, json);
  }

  /** 更新分组名：json {"id","name"} */
  static nativeGpkgUpdateGroup(path: string, json: string): boolean {
    return gc.nativeGpkgUpdateGroup(path, json);
  }

  /** 删除分组：组内照片移到未分组（group_id=-1） */
  static nativeGpkgDeleteGroup(path: string, id: number): boolean {
    return gc.nativeGpkgDeleteGroup(path, id);
  }

  /** 分组内照片批量显隐 */
  static nativeGpkgSetGroupPhotosVisible(path: string, id: number, visible: boolean): boolean {
    return gc.nativeGpkgSetGroupPhotosVisible(path, id, visible);
  }

  /** 样地 upsert（fid 冲突替换）：json {"id","name","plotType","attributes","points"} */
  static nativeGpkgUpsertSurveyPlot(path: string, json: string): boolean {
    return gc.nativeGpkgUpsertSurveyPlot(path, json);
  }

  /** 删除样地（按 fid DeleteFeature） */
  static nativeGpkgDeleteSurveyPlot(path: string, id: number): boolean {
    return gc.nativeGpkgDeleteSurveyPlot(path, id);
  }

  /** 元数据一把梭：project_meta 五键 upsert + layer_refs 全替换 + gpkg_contents/ogr_contents 刷新 */
  static nativeGpkgSyncMeta(path: string, json: string): boolean {
    return gc.nativeGpkgSyncMeta(path, json);
  }

  /** 照片文件清扫（幂等）：BLOB 已在库删磁盘原件；BLOB 缺失且原件存在则读入库后删；仅限 mediaRoot 下 */
  static nativeGpkgSweepPhotoFiles(path: string, mediaRoot: string): boolean {
    return gc.nativeGpkgSweepPhotoFiles(path, mediaRoot);
  }
}
