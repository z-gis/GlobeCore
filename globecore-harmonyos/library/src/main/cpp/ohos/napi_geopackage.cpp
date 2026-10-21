// napi_geopackage.cpp —— GeoPackage 工程数据读写 NAPI 导出（对应 Android 侧
// GeoPackageWriter 的 JNI 段 + GeoPackageDml.kt 的平台 SQLite 增量写）。
//
// 鸿蒙 ArkTS 没有"打开任意路径 SQLite 文件"的平台 API，gpkg 读写全部走引擎 C++：
// 核心在 gcbridge（bridge/geopackage_core.h，GDAL 实现），本文件仅 napi_value ↔ std 类型
// 编解码薄壳，风格照 napi_vector_io.cpp。导出命名对齐全模块 native 前缀约定：
//   nativeReadGeoPackage(path) → string | null
//   nativeWriteGeoPackage(path, json) → boolean（新建整写 + 老库 schema 升级共用，tmp+rename 原子）
//   nativeGpkgSchemaCurrent(path) → boolean
//   nativeGpkgPrepareSchema(path) → boolean
//   nativeGpkgInsertTrack(path, json) → number（fid，失败 -1）
//   nativeGpkgUpdateTrack(path, id, json) → boolean
//   nativeGpkgDeleteTrack(path, id) → boolean
//   nativeGpkgInsertMeasure(path, json) → number（fid，失败 -1）
//   nativeGpkgUpdateMeasure(path, id, json) → boolean
//   nativeGpkgDeleteMeasure(path, id) → boolean
//   nativeGpkgInsertPhoto(path, json) → number（fid，失败 -1）
//   nativeGpkgUpdatePhoto(path, id, json) → boolean
//   nativeGpkgDeletePhoto(path, id) → boolean
//   nativeGpkgAddPhotoAttachment(path, fid, idx, ArrayBuffer) → boolean
//   nativeGpkgReadPhotoBlob(path, fid, idx) → ArrayBuffer | null（idx=0 主照片，>=1 附件）
//   nativeGpkgInsertGroup(path, json) → boolean
//   nativeGpkgUpdateGroup(path, json) → boolean
//   nativeGpkgDeleteGroup(path, id) → boolean
//   nativeGpkgSetGroupPhotosVisible(path, id, visible) → boolean
//   nativeGpkgUpsertSurveyPlot(path, json) → boolean
//   nativeGpkgDeleteSurveyPlot(path, id) → boolean
//   nativeGpkgSyncMeta(path, json) → boolean
//   nativeGpkgSweepPhotoFiles(path, mediaRoot) → boolean
//   nativeMigrateTileDir(dir, mbtiles) → number（迁移块数；-1 失败保留旧目录）
// fid/id 在 ArkTS 侧以 number（<2^53）传输，对齐 JNI jlong 语义；JSON 形状见 index.d.ts 注释。
#include <napi/native_api.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bridge/geopackage_core.h"
#include "bridge/tile_migrate.h"
#include "napi_util.h"

namespace {

/// ArrayBuffer 入参读取（napi_util.h 无二进制助手，局部实现）；非 ArrayBuffer 返回 false
bool NapiGetArrayBuffer(napi_env env, napi_value v, const uint8_t *&data, size_t &len) {
    data = nullptr;
    len = 0;
    if (v == nullptr) return false;
    bool isBuf = false;
    if (napi_is_arraybuffer(env, v, &isBuf) != napi_ok || !isBuf) return false;
    void *p = nullptr;
    size_t n = 0;
    if (napi_get_arraybuffer_info(env, v, &p, &n) != napi_ok || p == nullptr) return false;
    data = static_cast<const uint8_t *>(p);
    len = n;
    return true;
}

/// vector<uint8_t> → ArrayBuffer（空向量返回 0 长度 ArrayBuffer；失败 null）
napi_value NapiMakeArrayBuffer(napi_env env, const std::vector<uint8_t> &v) {
    void *p = nullptr;
    napi_value out = nullptr;
    if (napi_create_arraybuffer(env, v.size(), &p, &out) != napi_ok) {
        return gcohos::NapiMakeNull(env);
    }
    if (!v.empty()) memcpy(p, v.data(), v.size());
    return out;
}

// ── 整读 / 整写 / schema ──

napi_value NativeReadGeoPackage(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    if (path.empty()) return gcohos::NapiMakeNull(env);
    const std::string json = gcbridge::readGeoPackage(path);
    if (json.empty()) return gcohos::NapiMakeNull(env); // 对齐 JNI 空串 → null
    return gcohos::NapiMakeString(env, json);
}

napi_value NativeWriteGeoPackage(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::writeGeoPackage(path, json));
}

napi_value NativeGpkgSchemaCurrent(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    if (path.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgSchemaCurrent(path));
}

napi_value NativeGpkgPrepareSchema(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    if (path.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgPrepareSchema(path));
}

// ── 轨迹 ──

napi_value NativeGpkgInsertTrack(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeInt64(env, -1);
    return gcohos::NapiMakeInt64(env, gcbridge::gpkgInsertTrack(path, json));
}

napi_value NativeGpkgUpdateTrack(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    const std::string json = gcohos::NapiGetString(env, args[2]);
    if (path.empty() || id <= 0 || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgUpdateTrack(path, id, json));
}

napi_value NativeGpkgDeleteTrack(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    if (path.empty() || id <= 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgDeleteTrack(path, id));
}

// ── 测量 ──

napi_value NativeGpkgInsertMeasure(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeInt64(env, -1);
    return gcohos::NapiMakeInt64(env, gcbridge::gpkgInsertMeasure(path, json));
}

napi_value NativeGpkgUpdateMeasure(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    const std::string json = gcohos::NapiGetString(env, args[2]);
    if (path.empty() || id <= 0 || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgUpdateMeasure(path, id, json));
}

napi_value NativeGpkgDeleteMeasure(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    if (path.empty() || id <= 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgDeleteMeasure(path, id));
}

// ── 拍照 ──

napi_value NativeGpkgInsertPhoto(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeInt64(env, -1);
    return gcohos::NapiMakeInt64(env, gcbridge::gpkgInsertPhoto(path, json));
}

napi_value NativeGpkgUpdatePhoto(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    const std::string json = gcohos::NapiGetString(env, args[2]);
    if (path.empty() || id <= 0 || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgUpdatePhoto(path, id, json));
}

napi_value NativeGpkgDeletePhoto(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    if (path.empty() || id <= 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgDeletePhoto(path, id));
}

napi_value NativeGpkgAddPhotoAttachment(napi_env env, napi_callback_info info) {
    napi_value args[4];
    gcohos::NapiGetArgs(env, info, 4, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t fid = 0, idx = 0;
    gcohos::NapiGetInt64(env, args[1], fid);
    gcohos::NapiGetInt64(env, args[2], idx);
    const uint8_t *data = nullptr;
    size_t len = 0;
    if (path.empty() || fid <= 0 || !NapiGetArrayBuffer(env, args[3], data, len) || len == 0) {
        return gcohos::NapiMakeBool(env, false);
    }
    return gcohos::NapiMakeBool(env, gcbridge::gpkgAddPhotoAttachment(path, fid, idx, data, len));
}

napi_value NativeGpkgReadPhotoBlob(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t fid = 0, idx = 0;
    gcohos::NapiGetInt64(env, args[1], fid);
    gcohos::NapiGetInt64(env, args[2], idx);
    if (path.empty() || fid <= 0) return gcohos::NapiMakeNull(env);
    std::vector<uint8_t> blob;
    if (!gcbridge::gpkgReadPhotoBlob(path, fid, idx, blob)) return gcohos::NapiMakeNull(env);
    return NapiMakeArrayBuffer(env, blob);
}

// ── 分组 ──

napi_value NativeGpkgInsertGroup(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgInsertGroup(path, json));
}

napi_value NativeGpkgUpdateGroup(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgUpdateGroup(path, json));
}

napi_value NativeGpkgDeleteGroup(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    if (path.empty() || id <= 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgDeleteGroup(path, id));
}

napi_value NativeGpkgSetGroupPhotosVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    gcohos::NapiGetArgs(env, info, 3, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    bool visible = false;
    gcohos::NapiGetBool(env, args[2], visible);
    if (path.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgSetGroupPhotosVisible(path, id, visible));
}

// ── 样地 / 元数据 / 清扫 ──

napi_value NativeGpkgUpsertSurveyPlot(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgUpsertSurveyPlot(path, json));
}

napi_value NativeGpkgDeleteSurveyPlot(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t id = 0;
    gcohos::NapiGetInt64(env, args[1], id);
    if (path.empty() || id <= 0) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgDeleteSurveyPlot(path, id));
}

napi_value NativeGpkgSyncMeta(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string json = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || json.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgSyncMeta(path, json));
}

napi_value NativeGpkgSweepPhotoFiles(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string mediaRoot = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || mediaRoot.empty()) return gcohos::NapiMakeBool(env, false);
    return gcohos::NapiMakeBool(env, gcbridge::gpkgSweepPhotoFiles(path, mediaRoot));
}

// ── 瓦片缓存迁移 ──

napi_value NativeMigrateTileDir(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string dir = gcohos::NapiGetString(env, args[0]);
    const std::string mbtiles = gcohos::NapiGetString(env, args[1]);
    if (dir.empty() || mbtiles.empty()) return gcohos::NapiMakeInt32(env, 0);
    return gcohos::NapiMakeInt32(env, gcbridge::gpkgMigrateTileDir(dir.c_str(), mbtiles.c_str()));
}

} // namespace

namespace gcohos {

napi_value RegisterGeoPackage(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeReadGeoPackage", nullptr, NativeReadGeoPackage, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeWriteGeoPackage", nullptr, NativeWriteGeoPackage, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgSchemaCurrent", nullptr, NativeGpkgSchemaCurrent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgPrepareSchema", nullptr, NativeGpkgPrepareSchema, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgInsertTrack", nullptr, NativeGpkgInsertTrack, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgUpdateTrack", nullptr, NativeGpkgUpdateTrack, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgDeleteTrack", nullptr, NativeGpkgDeleteTrack, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgInsertMeasure", nullptr, NativeGpkgInsertMeasure, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgUpdateMeasure", nullptr, NativeGpkgUpdateMeasure, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgDeleteMeasure", nullptr, NativeGpkgDeleteMeasure, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgInsertPhoto", nullptr, NativeGpkgInsertPhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgUpdatePhoto", nullptr, NativeGpkgUpdatePhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgDeletePhoto", nullptr, NativeGpkgDeletePhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgAddPhotoAttachment", nullptr, NativeGpkgAddPhotoAttachment, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgReadPhotoBlob", nullptr, NativeGpkgReadPhotoBlob, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgInsertGroup", nullptr, NativeGpkgInsertGroup, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgUpdateGroup", nullptr, NativeGpkgUpdateGroup, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgDeleteGroup", nullptr, NativeGpkgDeleteGroup, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgSetGroupPhotosVisible", nullptr, NativeGpkgSetGroupPhotosVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgUpsertSurveyPlot", nullptr, NativeGpkgUpsertSurveyPlot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgDeleteSurveyPlot", nullptr, NativeGpkgDeleteSurveyPlot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgSyncMeta", nullptr, NativeGpkgSyncMeta, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGpkgSweepPhotoFiles", nullptr, NativeGpkgSweepPhotoFiles, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeMigrateTileDir", nullptr, NativeMigrateTileDir, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace gcohos
