// bridge_api.h —— 数据 IO 桥（PROJ/GDAL 能力）的双平台共享核心 API。
//
// 设计口径（0.1.1.0 双平台改造）：桥接层是引擎仅有的三个平台适配点之一（日志/GL宿主/桥接）。
// 各 bridge/*.cpp 的计算逻辑收敛为本文件声明的纯 C++ 函数（std 字符串/容器进出、JSON 文本
// 作传输格式），平台绑定层各自薄封装：Android JNI 导出（同文件 #ifndef __OHOS__ 段）把
// jstring/jarray 转成 std 类型调核心；鸿蒙 NAPI（globecore-harmonyos cpp/ohos/napi_*.cpp）把
// napi_value 转成 std 类型调同一核心。双平台行为逐字节一致，单次改动同时生效。
#ifndef GLOBECORE_BRIDGE_API_H
#define GLOBECORE_BRIDGE_API_H

#include <cstdint>
#include <string>
#include <vector>

namespace wwbridge {

// ── srs.cpp：PROJ 初始化 / 坐标转换 / 版本 ──

/// PROJ 版本号，major*10000 + minor*100 + patch
int projVersion();

/// 设置全局 PROJ 上下文 proj.db 搜索路径（数据目录由宿主解压资源后传入），并做一次 EPSG:4326 校验
void initProjDataPath(const std::string &path);

/// 坐标转换（"源|目标" 管道缓存）：成功返回 true 并写出 outX/outY；管道创建失败或转换出错返回 false
bool convert(double x, double y, const std::string &srcCrs, const std::string &tgtCrs,
             double &outX, double &outY);

// ── gdal_info.cpp：GDAL 版本与驱动清单 ──

/// GDAL 版本字符串（GDALVersionInfo VERSION_NUM，形如 "30804"）
std::string gdalVersion();

/// 全部矢量驱动清单文本，逐行 "NAME（读/写|只读）"
std::string vectorDrivers();

// ── layer_info.cpp：图层四至 / 字段名 / 坐标系 ──

/// 图层四至（矢量优先、栅格兜底，投影坐标系重投影到 WGS84）：
/// 成功写出 extent[4]={minLon,minLat,maxLon,maxLat} 返回 true；打不开/无四至/CAD 无 SRS 返回 false
bool layerExtent(const std::string &path, double extent[4]);

/// 矢量首图层字段名列表（shp 取 DBF 字段）；打不开返回空列表
std::vector<std::string> vectorFieldNames(const std::string &path);

/// 图层原始坐标系描述："名称\nproj4定义"；无 SRS 或打不开返回空串
std::string layerSrs(const std::string &path);

// ── vector_io.cpp：矢量要素读取 / SQL 查询 / 统计 / 属性读写（JSON 传输） ──

/// 读要素集：返回 {"features":[...]}；打开失败返回空串（对应 JNI null）；
/// CAD 无坐标系返回 {"error":"..."}。hasFilter=false 全量读取（SQL 等），NaN 语义与 Android 口径一致：
/// 四至任一为 NaN 视为不过滤。includeAll=false 白名单模式仅保留 labelField 字段；
/// simplify=true 按 simplifyTolerance（度）做 Douglas-Peucker 简化。
std::string readVectorFeatures(const std::string &path,
                               double minLon, double minLat, double maxLon, double maxLat,
                               bool hasFilter, bool includeAll, const std::string &labelField,
                               bool simplify, double simplifyTolerance);

/// SQL 查询（OGR SQL / SQLite 方言）：成功 {"features":[...]}（可空数组），失败 {"error":"..."}；
/// 数据源打不开返回空串（对应 JNI null）
std::string queryVectorFeatures(const std::string &path, const std::string &sql);

/// 要素总数（遍历子图层精确计数）；打不开返回 -1
long long countVectorFeatures(const std::string &path);

/// 按 FID 就地写回属性字段（仅改现有字段；keys/values 等长）；成功 true，只读驱动/未命中 false
bool updateFeatureAttributes(const std::string &path, long long featureId,
                             const std::vector<std::string> &keys,
                             const std::vector<std::string> &values);

/// 按 FID 单要素全字段回取（句柄 LRU 缓存复用）：{"fields":{...}}；未找到/打不开返回空串
std::string getFeatureAttributes(const std::string &path, long long featureId);

} // namespace wwbridge

#endif // GLOBECORE_BRIDGE_API_H
