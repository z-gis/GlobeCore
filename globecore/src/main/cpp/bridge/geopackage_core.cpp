// geopackage_core.cpp —— GeoPackage 工程数据读/整写/增量写的平台无关核心（0.4.0.4 鸿蒙配套）。
//
// 读/整写逻辑从 bridge/geopackage_io.cpp 原样抽取（函数改为 gcbridge 导出，行为零变化，
// JSON 逐字节同构）；增量 DML 按 Android 侧 GeoPackageDml.kt 逐一移植为 GDAL 实现。
// 不直接 #include <sqlite3.h>、不依赖 sqlite3 符号——一切走 GDAL（OGRLayer
// CreateFeature/GetFeature/SetFeature/DeleteFeature + GDALDataset::ExecuteSQL 透传）。
// 本文件仅纳入鸿蒙构建（Android 侧 geopackage_io.cpp / GeoPackageDml.kt 不动）。
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <climits>

#include "gdal/ogrsf_frmts.h"
#include "gdal/gdal_priv.h"
#include "gdal/cpl_error.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include "geopackage_core.h"

using globecore::JsonValue;
using globecore::parseJson;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// ---- JSON 转义（输出侧，复用 geopackage_io.cpp 原样实现）----

static void jsonEscape(std::string &out, const char *s) {
    if (s == nullptr) return;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char) *p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
}

// ---- JSON 读取辅助（解析侧）----

static std::string jsonStr(const JsonValue &v, const char *key) {
    const JsonValue &f = v.at(key);
    return f.isString() ? f.strVal : std::string();
}

static long long jsonInt(const JsonValue &v, const char *key, long long def = 0) {
    const JsonValue &f = v.at(key);
    return f.isNumber() ? (long long) f.numVal : def;
}

static bool jsonBool(const JsonValue &v, const char *key, bool def = false) {
    const JsonValue &f = v.at(key);
    return f.isBool() ? f.boolVal : def;
}

/** 解析 [[lon,lat,alt],...] 点列 */
static void jsonPoints(const JsonValue &v, std::vector<double> &out) {
    const JsonValue &pts = v.at("points");
    if (!pts.isArray()) return;
    for (const JsonValue &pt : pts.arr) {
        if (!pt.isArray() || pt.arr.size() < 2) continue;
        out.push_back(pt.at(0).numVal);
        out.push_back(pt.at(1).numVal);
        const JsonValue &z = pt.at(2);
        out.push_back(z.isNumber() ? z.numVal : 0.0);
    }
}

/** JsonValue → JSON 字符串（用于 survey_plots.attributes / photos.attachments 原样存取） */
static void writeJsonValue(std::string &out, const JsonValue &v) {
    switch (v.type) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool:
            out += v.boolVal ? "true" : "false";
            break;
        case JsonValue::Type::Number: {
            char buf[40];
            double d = v.numVal;
            if (d == std::floor(d) && std::fabs(d) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", (long long) d);
            } else {
                snprintf(buf, sizeof(buf), "%.10g", d);
            }
            out += buf;
            break;
        }
        case JsonValue::Type::String:
            out += '"';
            jsonEscape(out, v.strVal.c_str());
            out += '"';
            break;
        case JsonValue::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out += ',';
                writeJsonValue(out, v.arr[i]);
            }
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            {
                bool first = true;
                for (const auto &kv : v.obj) {
                    if (!first) out += ',';
                    first = false;
                    out += '"';
                    jsonEscape(out, kv.first.c_str());
                    out += "\":";
                    writeJsonValue(out, kv.second);
                }
            }
            out += '}';
            break;
    }
}

// ---- OGR 字段写入辅助 ----

static void setFieldStr(OGRFeature *feat, const char *name, const std::string &val) {
    if (val.empty()) return; // 空串视为未设置（对应 Kotlin null）
    feat->SetField(name, val.c_str());
}

static void setFieldInt(OGRFeature *feat, const char *name, long long val) {
    feat->SetField(name, (GIntBig) val);
}

static void setFieldBool(OGRFeature *feat, const char *name, bool val) {
    feat->SetField(name, val ? 1 : 0);
}

/** 更新侧字符串列：空串写 NULL（对齐 Kotlin putStr；新建侧 setFieldStr 空串跳过不建列值） */
static void setUpdateStr(OGRFeature *feat, const char *name, const std::string &val) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0) return;
    if (val.empty()) feat->SetFieldNull(idx);
    else feat->SetField(idx, val.c_str());
}

// ---- OGR 几何构建 ----

static OGRLineString *pointsToLine(const std::vector<double> &pts) {
    auto *line = new OGRLineString();
    for (size_t i = 0; i + 2 < pts.size(); i += 3) {
        line->addPoint(pts[i], pts[i + 1], pts[i + 2]);
    }
    return line;
}

static OGRPolygon *pointsToPolygon(const std::vector<double> &pts) {
    auto *poly = new OGRPolygon();
    auto *ring = new OGRLinearRing();
    for (size_t i = 0; i + 2 < pts.size(); i += 3) {
        ring->addPoint(pts[i], pts[i + 1], pts[i + 2]);
    }
    ring->closeRings(); // 自动补首点闭合（OGR 面要求）
    poly->addRingDirectly(ring);
    return poly;
}

// ---- 表创建 ----

static OGRSpatialReference *makeWgs84() {
    auto *srs = new OGRSpatialReference();
    srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs->SetWellKnownGeogCS("WGS84");
    return srs;
}

static void createLayerFields(OGRLayer *layer, const std::vector<std::pair<const char *, OGRFieldType>> &fields,
                              const std::vector<int> &widths = {}) {
    for (size_t i = 0; i < fields.size(); i++) {
        OGRFieldDefn fd(fields[i].first, fields[i].second);
        if (i < widths.size() && widths[i] > 0) fd.SetWidth(widths[i]);
        layer->CreateField(&fd);
    }
}

namespace gcbridge {

/**
 * 创建 GeoPackage 文件并写入工程数据（tmp+rename 原子替换，与 Android 写侧同一实现）。
 * 表结构：
 *   - tracks:        LineString, name, date, color, hidden, line_width
 *   - measures:      Geometry(Point/LineString/Polygon), name, date, type, result, color, hidden, closed, line_width
 *   - photos:        Point, name, photo_path, photo_data, timestamp, attachments(JSON), visible, group_id
 *   - media_groups:  (非空间) name, created_time, visible（fid 即 id）
 *   - survey_plots:  Geometry(Point/Polygon), name, plot_type, attributes(JSON)
 *   - project_meta:  key, value
 *   - layer_refs:    layer_path
 */
bool writeGeoPackage(const std::string &filePath, const std::string &projectJson) {
    GDALAllRegister();

    // 解析工程 JSON（失败则只建空表结构）
    JsonValue root = parseJson(projectJson);
    if (!root.isObject()) {
        LOGE("工程 JSON 解析失败: %s", filePath.c_str());
    }

    // 原子写：先写 <path>.tmp 临时文件，全部成功后 rename 原子替换正式文件；
    // 中途失败/进程被杀只留 .tmp 残片，正式工程文件（唯一副本）不受影响
    const std::string tmpPath = filePath + ".tmp";
    remove(tmpPath.c_str());

    // 创建 GeoPackage 数据集
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (driver == nullptr) {
        LOGE("GPKG 驱动未注册");
        return false;
    }

    GDALDataset *ds = driver->Create(tmpPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (ds == nullptr) {
        LOGE("GeoPackage 创建失败: %s", filePath.c_str());
        return false;
    }

    // ---- tracks 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        // 几何均为三维（点列含 alt）：必须建 25D 层，否则 gpkg_geometry_columns.z 落成 0，与实际 blob 不符
        OGRLayer *layer = ds->CreateLayer("tracks", srs, wkbLineString25D, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"date",       OFTString},
                    {"color",      OFTString},
                    {"hidden",     OFTInteger},
                    {"line_width", OFTInteger},
            }, {256, 64, 16, 0, 0});

            const JsonValue &tracks = root.at("tracks");
            if (tracks.isArray()) {
                for (const JsonValue &t : tracks.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    const long long recId = jsonInt(t, "id", 0);
                    if (recId > 0) feat->SetFID((GIntBig) recId);  // 持久化记录 id：重写不重编号（分组引用/Media 目录按 id 命名）
                    setFieldStr(feat, "name", jsonStr(t, "name"));
                    setFieldStr(feat, "date", jsonStr(t, "date"));
                    setFieldStr(feat, "color", jsonStr(t, "color"));
                    setFieldBool(feat, "hidden", jsonBool(t, "hidden"));
                    setFieldInt(feat, "line_width", jsonInt(t, "lineWidth", 3));
                    std::vector<double> pts;
                    jsonPoints(t, pts);
                    feat->SetGeometryDirectly(pointsToLine(pts));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("tracks 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- measures 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("measures", srs, wkbUnknown, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"date",       OFTString},
                    {"type",       OFTString},
                    {"result",     OFTString},
                    {"color",      OFTString},
                    {"hidden",     OFTInteger},
                    {"closed",     OFTInteger},
                    {"line_width", OFTInteger},
            }, {256, 64, 64, 512, 16, 0, 0, 0});

            const JsonValue &measures = root.at("measures");
            if (measures.isArray()) {
                for (const JsonValue &m : measures.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    const long long recId = jsonInt(m, "id", 0);
                    if (recId > 0) feat->SetFID((GIntBig) recId);
                    std::string type = jsonStr(m, "type");
                    setFieldStr(feat, "name", jsonStr(m, "name"));
                    setFieldStr(feat, "date", jsonStr(m, "date"));
                    setFieldStr(feat, "type", type);
                    setFieldStr(feat, "result", jsonStr(m, "result"));
                    setFieldStr(feat, "color", jsonStr(m, "color"));
                    setFieldBool(feat, "hidden", jsonBool(m, "hidden"));
                    setFieldBool(feat, "closed", jsonBool(m, "closed"));
                    setFieldInt(feat, "line_width", jsonInt(m, "lineWidth", 3));

                    std::vector<double> pts;
                    jsonPoints(m, pts);
                    OGRGeometry *geom;
                    if (type == "point" && pts.size() >= 3) {
                        geom = new OGRPoint(pts[0], pts[1], pts[2]);
                    } else if (type == "area" || type == "polygon") {
                        geom = pointsToPolygon(pts);
                    } else {
                        // distance / line / angle 均为线
                        geom = pointsToLine(pts);
                    }
                    feat->SetGeometryDirectly(geom);
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("measures 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- photos 图层 ----
    // photo_data BLOB：主照片本体随工程入库（分享单文件自包含）。
    // 写入优先级：photo_path 文件存在 → 读文件；文件缺失 → 从旧库（替换前的 filePath）
    // 同 fid 继承 BLOB（本体被清理后数据仍在）；两者皆无 → NULL。
    // 旧库只读打开，与 .tmp 新建互不冲突。
    {
        OGRLayer *legacyPhotos = nullptr;
        GDALDataset *legacyDs = nullptr;
        if (!filePath.empty()) {
            GDALDataset *probe = static_cast<GDALDataset *>(
                    GDALOpenEx(filePath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
            if (probe != nullptr) {
                legacyPhotos = probe->GetLayerByName("photos");
                if (legacyPhotos != nullptr) {
                    legacyDs = probe;  // 保持打开直到写完
                } else {
                    GDALClose(probe);
                }
            }
        }

        OGRSpatialReference *srs = makeWgs84();
        // 拍照点位几何带 alt：同 tracks，建 25D 层让 z 声明正确
        OGRLayer *layer = ds->CreateLayer("photos", srs, wkbPoint25D, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",        OFTString},
                    {"photo_path",  OFTString},
                    {"photo_data",  OFTBinary},
                    {"timestamp",   OFTInteger64},
                    {"attachments", OFTString},
                    {"visible",     OFTInteger},
                    {"group_id",    OFTInteger64},
            }, {256, 0, 0, 0, 0, 0, 0});

            const JsonValue &photos = root.at("photos");
            if (photos.isArray()) {
                for (const JsonValue &p : photos.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    const long long recId = jsonInt(p, "id", 0);
                    if (recId > 0) feat->SetFID((GIntBig) recId);
                    setFieldStr(feat, "name", jsonStr(p, "name"));
                    setFieldInt(feat, "timestamp", jsonInt(p, "timestamp"));
                    // attachments 数组整体序列化为 JSON 字符串存列；
                    // photo_path 列继续写入（= 首个附件 path），供 QGIS 直接查看主照片
                    std::string mainPath;
                    const JsonValue &atts = p.at("attachments");
                    if (atts.isArray() && !atts.arr.empty()) {
                        std::string attsJson;
                        writeJsonValue(attsJson, atts);
                        setFieldStr(feat, "attachments", attsJson);
                        mainPath = jsonStr(atts.arr[0], "path");
                        setFieldStr(feat, "photo_path", mainPath);
                    }
                    // 主照片 BLOB：文件本体优先，缺失时从旧库同 fid 继承
                    std::vector<GByte> blob;
                    if (!mainPath.empty()) {
                        std::ifstream f(mainPath, std::ios::binary);
                        if (f) {
                            f.seekg(0, std::ios::end);
                            std::streamoff sz = f.tellg();
                            if (sz > 0) {
                                blob.resize(static_cast<size_t>(sz));
                                f.seekg(0, std::ios::beg);
                                f.read(reinterpret_cast<char *>(blob.data()), sz);
                                blob.resize(static_cast<size_t>(f.gcount()));
                            }
                        }
                    }
                    if (!blob.empty()) {
                        feat->SetField(layer->GetLayerDefn()->GetFieldIndex("photo_data"),
                                       (int) blob.size(), blob.data());
                    } else if (legacyPhotos != nullptr && recId > 0) {
                        OGRFeature *old = legacyPhotos->GetFeature((GIntBig) recId);
                        if (old != nullptr) {
                            int bi = old->GetFieldIndex("photo_data");
                            if (bi >= 0 && !old->IsFieldNull(bi)) {
                                int oldSize = 0;
                                GByte *oldData = old->GetFieldAsBinary(bi, &oldSize);
                                if (oldData != nullptr && oldSize > 0) {
                                    feat->SetField(layer->GetLayerDefn()->GetFieldIndex("photo_data"),
                                                   oldSize, oldData);
                                }
                            }
                            OGRFeature::DestroyFeature(old);
                        }
                    }
                    setFieldBool(feat, "visible", jsonBool(p, "visible", true));
                    setFieldInt(feat, "group_id", jsonInt(p, "groupId", -1));
                    double lon = p.at("lon").numVal;
                    double lat = p.at("lat").numVal;
                    double alt = p.at("alt").numVal;
                    feat->SetGeometryDirectly(new OGRPoint(lon, lat, alt));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("photos 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
        if (legacyDs != nullptr) GDALClose(legacyDs);
    }

    // ---- media_groups 图层（非空间表，fid 即 id）----
    {
        OGRLayer *layer = ds->CreateLayer("media_groups", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",         OFTString},
                    {"created_time", OFTInteger64},
                    {"visible",      OFTInteger},
            }, {256, 0, 0});

            const JsonValue &groups = root.at("mediaGroups");
            if (groups.isArray()) {
                for (const JsonValue &g : groups.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    const long long recId = jsonInt(g, "id", 0);
                    if (recId > 0) feat->SetFID((GIntBig) recId);
                    setFieldStr(feat, "name", jsonStr(g, "name"));
                    setFieldInt(feat, "created_time", jsonInt(g, "createdTime"));
                    setFieldBool(feat, "visible", jsonBool(g, "visible", true));
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("media_groups 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- survey_plots 图层 ----
    {
        OGRSpatialReference *srs = makeWgs84();
        OGRLayer *layer = ds->CreateLayer("survey_plots", srs, wkbUnknown, nullptr);
        srs->Release();
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"name",       OFTString},
                    {"plot_type",  OFTString},
                    {"attributes", OFTString},
            }, {256, 64, 4096});

            const JsonValue &plots = root.at("surveyPlots");
            if (plots.isArray()) {
                for (const JsonValue &s : plots.arr) {
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    const long long recId = jsonInt(s, "id", 0);
                    if (recId > 0) feat->SetFID((GIntBig) recId);
                    std::string plotType = jsonStr(s, "plotType");
                    setFieldStr(feat, "name", jsonStr(s, "name"));
                    setFieldStr(feat, "plot_type", plotType);
                    const JsonValue &attrs = s.at("attributes");
                    if (attrs.type != JsonValue::Type::Null) {
                        std::string attrsJson;
                        writeJsonValue(attrsJson, attrs);
                        setFieldStr(feat, "attributes", attrsJson);
                    }

                    std::vector<double> pts;
                    jsonPoints(s, pts);
                    OGRGeometry *geom;
                    if (plotType == "point" && pts.size() >= 3) {
                        geom = new OGRPoint(pts[0], pts[1], pts[2]);
                    } else {
                        geom = pointsToPolygon(pts);
                    }
                    feat->SetGeometryDirectly(geom);
                    if (layer->CreateFeature(feat) != OGRERR_NONE) {
                        LOGE("survey_plots 要素写入失败");
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    // ---- project_meta 图层（非空间表）----
    {
        OGRLayer *layer = ds->CreateLayer("project_meta", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"key",   OFTString},
                    {"value", OFTString},
            }, {256, 4096});

            auto writeMeta = [layer](const char *key, const std::string &value) {
                if (value.empty()) return;
                OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                feat->SetField("key", key);
                feat->SetField("value", value.c_str());
                layer->CreateFeature(feat);
                OGRFeature::DestroyFeature(feat);
            };
            writeMeta("name", jsonStr(root, "name"));
            writeMeta("description", jsonStr(root, "description"));
            writeMeta("srs", jsonStr(root, "srs"));
            writeMeta("createTime", std::to_string(jsonInt(root, "createTime")));
            writeMeta("updateTime", std::to_string(jsonInt(root, "updateTime")));
        }
    }

    // ---- layer_refs 图层（非空间表）----
    {
        OGRLayer *layer = ds->CreateLayer("layer_refs", nullptr, wkbNone, nullptr);
        if (layer != nullptr) {
            createLayerFields(layer, {
                    {"layer_path", OFTString},
            }, {4096});

            const JsonValue &refs = root.at("layerRefs");
            if (refs.isArray()) {
                for (const JsonValue &r : refs.arr) {
                    if (!r.isString() || r.strVal.empty()) continue;
                    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
                    feat->SetField("layer_path", r.strVal.c_str());
                    layer->CreateFeature(feat);
                    OGRFeature::DestroyFeature(feat);
                }
            }
        }
    }

    GDALClose(ds);
    // POSIX rename 原子替换已存在目标；失败时保留 .tmp 供排查，正式文件仍是上次完整副本
    if (rename(tmpPath.c_str(), filePath.c_str()) != 0) {
        LOGE("GeoPackage 替换正式文件失败（.tmp 已保留）: %s -> %s", tmpPath.c_str(), filePath.c_str());
        return false;
    }
    LOGI("GeoPackage 写入成功: %s", filePath.c_str());
    return true;
}

} // namespace gcbridge

// ---- GeoPackage 读取 ----

// ---- OGR 字段读取辅助 ----

static std::string getFieldStr(OGRFeature *feat, const char *name) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return "";
    return feat->GetFieldAsString(idx);
}

static long long getFieldInt(OGRFeature *feat, const char *name) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return 0;
    return (long long) feat->GetFieldAsInteger64(idx);
}

/** 带默认值的整列读取：旧版 gpkg 缺列时回默认值（读侧兼容） */
static long long getFieldIntDef(OGRFeature *feat, const char *name, long long def) {
    int idx = feat->GetFieldIndex(name);
    if (idx < 0 || !feat->IsFieldSet(idx) || feat->IsFieldNull(idx)) return def;
    return (long long) feat->GetFieldAsInteger64(idx);
}

// ---- JSON 输出辅助 ----

static void outStrField(std::string &out, const char *key, const std::string &value, bool &first) {
    if (value.empty()) return; // 空字段跳过（Kotlin 侧 optString 有兜底）
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":\"";
    jsonEscape(out, value.c_str());
    out += "\"";
}

static void outIntField(std::string &out, const char *key, long long value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    out += buf;
}

static void outDoubleField(std::string &out, const char *key, double value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.8f", value);
    out += buf;
}

static void outBoolField(std::string &out, const char *key, bool value, bool &first) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    out += value ? "true" : "false";
}

/** 输出线/环点列为 [[x,y,z],...]；skipClosingDup=true 时去掉与首点重合的末点（面闭合环） */
static void outLinePoints(std::string &out, const OGRLineString *line, bool skipClosingDup) {
    out += "[";
    int n = line->getNumPoints();
    if (skipClosingDup && n > 1 &&
        line->getX(0) == line->getX(n - 1) && line->getY(0) == line->getY(n - 1)) {
        n--;
    }
    for (int i = 0; i < n; i++) {
        if (i) out += ",";
        char buf[128];
        snprintf(buf, sizeof(buf), "[%.8f,%.8f,%.3f]",
                 line->getX(i), line->getY(i), line->getZ(i));
        out += buf;
    }
    out += "]";
}

/** 通用几何 → points 数组（Point→单点；Line→点列；Polygon→外环去闭合点） */
static void outGeomPoints(std::string &out, const OGRGeometry *geom) {
    if (geom == nullptr) {
        out += "[]";
        return;
    }
    switch (wkbFlatten(geom->getGeometryType())) {
        case wkbPoint: {
            auto *pt = (const OGRPoint *) geom;
            char buf[128];
            snprintf(buf, sizeof(buf), "[[%.8f,%.8f,%.3f]]", pt->getX(), pt->getY(), pt->getZ());
            out += buf;
            break;
        }
        case wkbLineString:
            outLinePoints(out, (const OGRLineString *) geom, false);
            break;
        case wkbPolygon: {
            auto *poly = (const OGRPolygon *) geom;
            const OGRLinearRing *ring = poly->getExteriorRing();
            if (ring != nullptr) {
                outLinePoints(out, ring, true);
            } else {
                out += "[]";
            }
            break;
        }
        default:
            out += "[]";
            break;
    }
}

/** 读取指定图层，逐要素输出 JSON 数组元素（first 跨要素维护逗号） */
static void readSpatialLayer(GDALDataset *ds, const char *layerName, std::string &out,
                             bool &firstElem, const char *kind) {
    OGRLayer *layer = ds->GetLayerByName(layerName);
    if (layer == nullptr) return;

    layer->ResetReading();
    OGRFeature *feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        if (!firstElem) out += ",";
        firstElem = false;
        out += "{";

        bool first = true;
        outIntField(out, "id", (long long) feat->GetFID(), first);
        outStrField(out, "name", getFieldStr(feat, "name"), first);

        if (strcmp(kind, "tracks") == 0) {
            outStrField(out, "date", getFieldStr(feat, "date"), first);
            outStrField(out, "color", getFieldStr(feat, "color"), first);
            outBoolField(out, "hidden", getFieldInt(feat, "hidden") != 0, first);
            outIntField(out, "lineWidth", getFieldIntDef(feat, "line_width", 3), first);
        } else if (strcmp(kind, "measures") == 0) {
            outStrField(out, "date", getFieldStr(feat, "date"), first);
            outStrField(out, "type", getFieldStr(feat, "type"), first);
            outStrField(out, "result", getFieldStr(feat, "result"), first);
            outStrField(out, "color", getFieldStr(feat, "color"), first);
            outBoolField(out, "hidden", getFieldInt(feat, "hidden") != 0, first);
            outBoolField(out, "closed", getFieldInt(feat, "closed") != 0, first);
            outIntField(out, "lineWidth", getFieldIntDef(feat, "line_width", 3), first);
        } else if (strcmp(kind, "photos") == 0) {
            // photos 的 lon/lat/alt 由几何提取（下方单独处理）
            long long ts = getFieldInt(feat, "timestamp");
            outIntField(out, "timestamp", ts, first);
            outBoolField(out, "visible", getFieldIntDef(feat, "visible", 1) != 0, first);
            outIntField(out, "groupId", getFieldIntDef(feat, "group_id", -1), first);
            // attachments：优先 attachments 列 JSON（原样嵌入）；空且 photo_path 非空时
            // 回退生成单元素 attachments（name 取文件名、path=photo_path、time=timestamp）
            std::string atts = getFieldStr(feat, "attachments");
            if (atts.empty() || atts[0] != '[') {
                std::string photoPath = getFieldStr(feat, "photo_path");
                if (!photoPath.empty()) {
                    size_t pos = photoPath.find_last_of("/\\");
                    std::string fname = (pos == std::string::npos) ? photoPath : photoPath.substr(pos + 1);
                    std::string fb = "[{\"name\":\"";
                    jsonEscape(fb, fname.c_str());
                    fb += "\",\"path\":\"";
                    jsonEscape(fb, photoPath.c_str());
                    fb += "\",\"time\":";
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", ts);
                    fb += buf;
                    fb += "}]";
                    atts = fb;
                } else {
                    atts = "[]";
                }
            }
            if (!first) out += ",";
            first = false;
            out += "\"attachments\":";
            out += atts;
        } else if (strcmp(kind, "survey_plots") == 0) {
            outStrField(out, "plotType", getFieldStr(feat, "plot_type"), first);
            // attributes 为 JSON 字符串，原样嵌入输出（非转义字符串）
            std::string attrs = getFieldStr(feat, "attributes");
            if (!attrs.empty() && attrs[0] == '{') {
                if (!first) out += ",";
                first = false;
                out += "\"attributes\":";
                out += attrs;
            }
        }

        if (strcmp(kind, "photos") == 0) {
            // PhotoRecord 需要 lon/lat/alt 显式字段
            const OGRGeometry *geom = feat->GetGeometryRef();
            double lon = 0, lat = 0, alt = 0;
            if (geom != nullptr && wkbFlatten(geom->getGeometryType()) == wkbPoint) {
                auto *pt = (const OGRPoint *) geom;
                lon = pt->getX();
                lat = pt->getY();
                alt = pt->getZ();
            }
            outDoubleField(out, "lon", lon, first);
            outDoubleField(out, "lat", lat, first);
            outDoubleField(out, "alt", alt, first);
        } else {
            // 其余空间表输出 points 数组
            if (!first) out += ",";
            first = false;
            out += "\"points\":";
            outGeomPoints(out, feat->GetGeometryRef());
        }

        out += "}";
        OGRFeature::DestroyFeature(feat);
    }
}

namespace gcbridge {

/**
 * 从 GeoPackage 读取工程数据，返回与 Project.fromJson 兼容的 JSON 字符串。
 * 文件不存在或结构不符时返回空串。
 */
std::string readGeoPackage(const std::string &filePath) {
    GDALAllRegister();

    GDALDataset *ds = (GDALDataset *) GDALOpenEx(
            filePath.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr);

    if (ds == nullptr) {
        LOGE("GeoPackage 打开失败: %s", filePath.c_str());
        return "";
    }

    std::string out = "{";
    bool first = true;

    // ---- project_meta ----
    {
        OGRLayer *meta = ds->GetLayerByName("project_meta");
        long long createTime = 0, updateTime = 0;
        if (meta != nullptr) {
            meta->ResetReading();
            OGRFeature *feat;
            while ((feat = meta->GetNextFeature()) != nullptr) {
                std::string key = getFieldStr(feat, "key");
                std::string value = getFieldStr(feat, "value");
                if (key == "name") {
                    outStrField(out, "name", value, first);
                } else if (key == "description") {
                    outStrField(out, "description", value, first);
                } else if (key == "srs") {
                    outStrField(out, "srs", value, first);
                } else if (key == "createTime") {
                    createTime = std::strtoll(value.c_str(), nullptr, 10);
                } else if (key == "updateTime") {
                    updateTime = std::strtoll(value.c_str(), nullptr, 10);
                }
                OGRFeature::DestroyFeature(feat);
            }
        }
        outIntField(out, "createTime", createTime, first);
        outIntField(out, "updateTime", updateTime, first);
    }

    // ---- 空间数据表 ----
    out += ",\"tracks\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "tracks", out, firstElem, "tracks");
    }
    out += "]";

    out += ",\"measures\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "measures", out, firstElem, "measures");
    }
    out += "]";

    out += ",\"photos\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "photos", out, firstElem, "photos");
    }
    out += "]";

    // ---- media_groups ----
    out += ",\"mediaGroups\":[";
    {
        OGRLayer *groups = ds->GetLayerByName("media_groups");
        bool firstElem = true;
        if (groups != nullptr) {
            groups->ResetReading();
            OGRFeature *feat;
            while ((feat = groups->GetNextFeature()) != nullptr) {
                if (!firstElem) out += ",";
                firstElem = false;
                out += "{";
                bool first = true;
                outIntField(out, "id", (long long) feat->GetFID(), first);
                outStrField(out, "name", getFieldStr(feat, "name"), first);
                outIntField(out, "createdTime", getFieldInt(feat, "created_time"), first);
                outBoolField(out, "visible", getFieldIntDef(feat, "visible", 1) != 0, first);
                out += "}";
                OGRFeature::DestroyFeature(feat);
            }
        }
    }
    out += "]";

    out += ",\"surveyPlots\":[";
    {
        bool firstElem = true;
        readSpatialLayer(ds, "survey_plots", out, firstElem, "survey_plots");
    }
    out += "]";

    // ---- layer_refs ----
    out += ",\"layerRefs\":[";
    {
        OGRLayer *refs = ds->GetLayerByName("layer_refs");
        bool firstElem = true;
        if (refs != nullptr) {
            refs->ResetReading();
            OGRFeature *feat;
            while ((feat = refs->GetNextFeature()) != nullptr) {
                std::string path = getFieldStr(feat, "layer_path");
                if (!path.empty()) {
                    if (!firstElem) out += ",";
                    firstElem = false;
                    out += "\"";
                    jsonEscape(out, path.c_str());
                    out += "\"";
                }
                OGRFeature::DestroyFeature(feat);
            }
        }
    }
    out += "]";

    out += "}";

    GDALClose(ds);
    LOGI("GeoPackage 读取成功: %s", filePath.c_str());
    return out;
}

} // namespace gcbridge

// ==================== 增量 DML（GeoPackageDml.kt 的 GDAL 移植）====================

// ---- 增量写公共骨架 ----

/** 独立连接包裹：每次操作独立 GDALOpenEx(UPDATE) → 操作 → GDALClose（等价 Kotlin withDb
 *  的独立事务，防与整写 tmp+rename 换文件产生句柄竞态）；GDALClose 自动提交。 */
struct GpkgDb {
    GDALDataset *ds = nullptr;

    explicit GpkgDb(const std::string &path) {
        GDALAllRegister();
        ds = static_cast<GDALDataset *>(
                GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr));
        if (ds == nullptr) {
            LOGE("gpkg 增量写打开失败: %s", path.c_str());
        }
    }

    ~GpkgDb() {
        if (ds != nullptr) GDALClose(ds);
    }

    bool ok() const { return ds != nullptr; }
};

/** SQL 透传（SQLITE 方言直交 SQLite 执行；非查询语句返回空结果集，错误静默吞掉，
 *  对齐 Kotlin runCatching 语义——增量写失败不致命，下次整文件重建可兜底） */
static void execSql(GDALDataset *ds, const std::string &sql) {
    CPLErrorReset();
    OGRLayer *res = ds->ExecuteSQL(sql.c_str(), nullptr, "SQLITE");
    if (res != nullptr) ds->ReleaseResultSet(res);
}

/** SQL 字符串字面量转义（单引号双写） */
static std::string sqlQuote(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

/** 二进制 → 小写 hex（BLOB 经 x'hex' 字面量拼 SQL 写 photo_att_data） */
static std::string hexEncode(const void *data, size_t size) {
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

/** hex → 二进制（SELECT hex(data) 读回解码）；非法字符/奇数长度返回 false */
static bool hexDecode(const std::string &hex, std::vector<uint8_t> &out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t) ((hi << 4) | lo));
    }
    return true;
}

/** gpkg_contents.last_change/bbox 与 gpkg_ogr_contents 要素计数随写更新（表不存在时静默跳过）。
 *  bbox 为 [minx,maxx,miny,maxy]，可空（空则仅刷 last_change）。 */
static void finishTable(GDALDataset *ds, const char *table, const double *bbox) {
    std::string sql = "UPDATE gpkg_contents SET last_change=strftime('%Y-%m-%dT%H:%M:%fZ','now')";
    if (bbox != nullptr) {
        char num[8][40];
        for (int i = 0; i < 4; i++) {
            snprintf(num[i * 2], 40, "%.17g", bbox[i]);
            snprintf(num[i * 2 + 1], 40, "%.17g", bbox[i]);
        }
        sql += ", min_x=min(COALESCE(min_x,";
        sql += num[0]; sql += "),"; sql += num[1];
        sql += "), max_x=max(COALESCE(max_x,"; sql += num[2]; sql += "),"; sql += num[3];
        sql += "), min_y=min(COALESCE(min_y,"; sql += num[4]; sql += "),"; sql += num[5];
        sql += "), max_y=max(COALESCE(max_y,"; sql += num[6]; sql += "),"; sql += num[7];
        sql += ")";
    }
    sql += " WHERE table_name='";
    sql += table;
    sql += "'";
    execSql(ds, sql);

    std::string sql2 = "UPDATE gpkg_ogr_contents SET feature_count=(SELECT COUNT(*) FROM \"";
    sql2 += table;
    sql2 += "\") WHERE table_name='";
    sql2 += table;
    sql2 += "'";
    execSql(ds, sql2);
}

/** 几何四至 → [minx,maxx,miny,maxy]；空几何返回 false */
static bool geomBbox(const OGRGeometry *geom, double bbox[4]) {
    if (geom == nullptr || geom->IsEmpty()) return false;
    OGREnvelope env;
    geom->getEnvelope(&env);
    bbox[0] = env.MinX;
    bbox[1] = env.MaxX;
    bbox[2] = env.MinY;
    bbox[3] = env.MaxY;
    return true;
}

/** 附件表是否已有 (fid, idx) 行 */
static bool auxHas(GDALDataset *ds, long long fid, long long idx) {
    std::string sql = "SELECT 1 FROM photo_att_data WHERE fid=" + std::to_string(fid) +
                      " AND idx=" + std::to_string(idx);
    OGRLayer *res = ds->ExecuteSQL(sql.c_str(), nullptr, "SQLITE");
    bool has = false;
    if (res != nullptr) {
        OGRFeature *f = res->GetNextFeature();
        has = (f != nullptr);
        if (f != nullptr) OGRFeature::DestroyFeature(f);
        ds->ReleaseResultSet(res);
    }
    return has;
}

namespace gcbridge {

// ---- schema 检查 / 准备 ----

bool gpkgSchemaCurrent(const std::string &path) {
    // 文件不存在返回 true，由新建流程负责建库（对齐 Kotlin schemaCurrent）
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) return true;
    }
    GDALAllRegister();
    GDALDataset *ds = static_cast<GDALDataset *>(
            GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (ds == nullptr) return false;

    // 增量写所要求的全部业务表（七表均为 GDAL 建库注册表，GetLayerByName 即可探存在性）
    static const char *kTables[] = {
            "tracks", "measures", "photos", "media_groups", "survey_plots", "project_meta", "layer_refs"};
    bool ok = true;
    for (const char *t : kTables) {
        if (ds->GetLayerByName(t) == nullptr) {
            ok = false;
            break;
        }
    }

    auto hasCols = [ds](const char *layerName, std::initializer_list<const char *> cols) {
        OGRLayer *layer = ds->GetLayerByName(layerName);
        if (layer == nullptr) return false;
        OGRFeatureDefn *defn = layer->GetLayerDefn();
        for (const char *c : cols) {
            if (defn->GetFieldIndex(c) < 0) return false;
        }
        return true;
    };
    ok = ok && hasCols("photos", {"name", "photo_path", "photo_data", "timestamp",
                                  "attachments", "visible", "group_id"})
            && hasCols("tracks", {"name", "date", "color", "hidden", "line_width"})
            && hasCols("measures", {"name", "date", "type", "result", "color",
                                    "hidden", "closed", "line_width"});
    GDALClose(ds);
    return ok;
}

bool gpkgPrepareSchema(const std::string &path) {
    GpkgDb db(path);
    if (!db.ok()) return false;
    // 鸿蒙走 GDAL 不需要清 rtree（GDAL 自带 rtree 模块；Android 清理是因平台 SQLite 无该模块），
    // 只需补建附件 BLOB 附加表（不注册 gpkg_contents，GDAL/QGIS 不可见）
    execSql(db.ds, "CREATE TABLE IF NOT EXISTS photo_att_data (fid INTEGER NOT NULL, "
                   "idx INTEGER NOT NULL, data BLOB NOT NULL, PRIMARY KEY (fid, idx))");
    // gpkg_geometry_columns.z 为 BOOLEAN（只允许 0/1）：老库把混装表写成 z=2、带 Z 的表写成 0，
    // 与实际三维几何不符；四张业务表几何均带 Z，统一修正为 1（与 Android prepareSchema 同口径）
    execSql(db.ds, "UPDATE gpkg_geometry_columns SET z=1 WHERE (z IS NULL OR z<>1) "
                   "AND table_name IN ('tracks','measures','photos','survey_plots')");
    return true;
}

// ---- 轨迹 ----

long long gpkgInsertTrack(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject()) return -1;
    GpkgDb db(path);
    if (!db.ok()) return -1;
    OGRLayer *layer = db.ds->GetLayerByName("tracks");
    if (layer == nullptr) return -1;

    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    const long long recId = jsonInt(root, "id", 0);
    if (recId > 0) feat->SetFID((GIntBig) recId);
    setFieldStr(feat, "name", jsonStr(root, "name"));
    setFieldStr(feat, "date", jsonStr(root, "date"));
    setFieldStr(feat, "color", jsonStr(root, "color"));
    setFieldBool(feat, "hidden", jsonBool(root, "hidden"));
    setFieldInt(feat, "line_width", jsonInt(root, "lineWidth", 3));
    std::vector<double> pts;
    jsonPoints(root, pts);
    double bbox[4];
    bool hasBbox = false;
    if (pts.size() >= 3) {
        auto *line = pointsToLine(pts);
        hasBbox = geomBbox(line, bbox);
        feat->SetGeometryDirectly(line);
    }
    if (layer->CreateFeature(feat) != OGRERR_NONE) {
        OGRFeature::DestroyFeature(feat);
        LOGE("gpkgInsertTrack 要素写入失败: %s", path.c_str());
        return -1;
    }
    const long long fid = (long long) feat->GetFID();
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "tracks", hasBbox ? bbox : nullptr);
    return fid;
}

bool gpkgUpdateTrack(const std::string &path, long long id, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject() || id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("tracks");
    if (layer == nullptr) return false;
    OGRFeature *feat = layer->GetFeature((GIntBig) id);
    if (feat == nullptr) return false;

    setUpdateStr(feat, "name", jsonStr(root, "name"));
    setUpdateStr(feat, "color", jsonStr(root, "color"));
    setFieldBool(feat, "hidden", jsonBool(root, "hidden"));
    setFieldInt(feat, "line_width", jsonInt(root, "lineWidth", 3));
    const bool ok = layer->SetFeature(feat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "tracks", nullptr);
    return ok;
}

bool gpkgDeleteTrack(const std::string &path, long long id) {
    if (id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("tracks");
    if (layer == nullptr) return false;
    const bool ok = layer->DeleteFeature((GIntBig) id) == OGRERR_NONE;
    finishTable(db.ds, "tracks", nullptr);
    return ok;
}

// ---- 测量 ----

long long gpkgInsertMeasure(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject()) return -1;
    GpkgDb db(path);
    if (!db.ok()) return -1;
    OGRLayer *layer = db.ds->GetLayerByName("measures");
    if (layer == nullptr) return -1;

    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    const long long recId = jsonInt(root, "id", 0);
    if (recId > 0) feat->SetFID((GIntBig) recId);
    std::string type = jsonStr(root, "type");
    setFieldStr(feat, "name", jsonStr(root, "name"));
    setFieldStr(feat, "date", jsonStr(root, "date"));
    setFieldStr(feat, "type", type);
    setFieldStr(feat, "result", jsonStr(root, "result"));
    setFieldStr(feat, "color", jsonStr(root, "color"));
    setFieldBool(feat, "hidden", jsonBool(root, "hidden"));
    setFieldBool(feat, "closed", jsonBool(root, "closed"));
    setFieldInt(feat, "line_width", jsonInt(root, "lineWidth", 3));

    std::vector<double> pts;
    jsonPoints(root, pts);
    double bbox[4];
    bool hasBbox = false;
    if (pts.size() >= 3) {
        OGRGeometry *geom;
        if (type == "point") {
            geom = new OGRPoint(pts[0], pts[1], pts[2]);
        } else if (type == "area" || type == "polygon") {
            geom = pointsToPolygon(pts);
        } else {
            // distance / line / angle 均为线
            geom = pointsToLine(pts);
        }
        hasBbox = geomBbox(geom, bbox);
        feat->SetGeometryDirectly(geom);
    }
    if (layer->CreateFeature(feat) != OGRERR_NONE) {
        OGRFeature::DestroyFeature(feat);
        LOGE("gpkgInsertMeasure 要素写入失败: %s", path.c_str());
        return -1;
    }
    const long long fid = (long long) feat->GetFID();
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "measures", hasBbox ? bbox : nullptr);
    return fid;
}

bool gpkgUpdateMeasure(const std::string &path, long long id, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject() || id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("measures");
    if (layer == nullptr) return false;
    OGRFeature *feat = layer->GetFeature((GIntBig) id);
    if (feat == nullptr) return false;

    // 对齐 updateMeasureStyle：仅颜色与线宽
    setUpdateStr(feat, "color", jsonStr(root, "color"));
    setFieldInt(feat, "line_width", jsonInt(root, "lineWidth", 3));
    const bool ok = layer->SetFeature(feat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "measures", nullptr);
    return ok;
}

bool gpkgDeleteMeasure(const std::string &path, long long id) {
    if (id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("measures");
    if (layer == nullptr) return false;
    const bool ok = layer->DeleteFeature((GIntBig) id) == OGRERR_NONE;
    finishTable(db.ds, "measures", nullptr);
    return ok;
}

// ---- 拍照 ----

long long gpkgInsertPhoto(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject()) return -1;
    GpkgDb db(path);
    if (!db.ok()) return -1;
    OGRLayer *layer = db.ds->GetLayerByName("photos");
    if (layer == nullptr) return -1;

    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    const long long recId = jsonInt(root, "id", 0);
    if (recId > 0) feat->SetFID((GIntBig) recId);
    setFieldStr(feat, "name", jsonStr(root, "name"));
    setFieldInt(feat, "timestamp", jsonInt(root, "timestamp"));
    setFieldBool(feat, "visible", jsonBool(root, "visible", true));
    setFieldInt(feat, "group_id", jsonInt(root, "groupId", -1));
    const double lon = root.at("lon").numVal;
    const double lat = root.at("lat").numVal;
    const double alt = root.at("alt").numVal;
    feat->SetGeometryDirectly(new OGRPoint(lon, lat, alt));
    if (layer->CreateFeature(feat) != OGRERR_NONE) {
        OGRFeature::DestroyFeature(feat);
        LOGE("gpkgInsertPhoto 要素写入失败: %s", path.c_str());
        return -1;
    }
    const long long fid = (long long) feat->GetFID();
    OGRFeature::DestroyFeature(feat);
    const double bbox[4] = {lon, lon, lat, lat};
    finishTable(db.ds, "photos", bbox);
    return fid;
}

bool gpkgUpdatePhoto(const std::string &path, long long id, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject() || id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("photos");
    if (layer == nullptr) return false;
    OGRFeature *feat = layer->GetFeature((GIntBig) id);
    if (feat == nullptr) return false;

    // 键出现才更新（对齐 updatePhotoColumns 可空语义）；空串写 NULL（putStr 口径）
    bool dirty = false;
    const JsonValue &name = root.at("name");
    if (name.isString()) {
        setUpdateStr(feat, "name", name.strVal);
        dirty = true;
    }
    const JsonValue &gid = root.at("groupId");
    if (gid.isNumber()) {
        setFieldInt(feat, "group_id", (long long) gid.numVal);
        dirty = true;
    }
    const JsonValue &vis = root.at("visible");
    if (vis.isBool()) {
        setFieldBool(feat, "visible", vis.boolVal);
        dirty = true;
    }
    // attachments/photo_path：鸿蒙补充键（Android 由 addPhotoAttachment 一并写入，
    // 鸿蒙 NAPI 签名不带 JSON，拆到本函数）；attachments 允许传数组，序列化后存列
    const JsonValue &atts = root.at("attachments");
    if (atts.isString()) {
        setUpdateStr(feat, "attachments", atts.strVal);
        dirty = true;
    } else if (atts.isArray()) {
        std::string attsJson;
        writeJsonValue(attsJson, atts);
        setUpdateStr(feat, "attachments", attsJson);
        dirty = true;
    }
    const JsonValue &pp = root.at("photo_path");
    if (pp.isString()) {
        setUpdateStr(feat, "photo_path", pp.strVal);
        dirty = true;
    }
    bool ok = true;
    if (dirty) ok = layer->SetFeature(feat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "photos", nullptr);
    return ok;
}

bool gpkgDeletePhoto(const std::string &path, long long id) {
    if (id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("photos");
    if (layer == nullptr) return false;
    const bool ok = layer->DeleteFeature((GIntBig) id) == OGRERR_NONE;
    execSql(db.ds, "DELETE FROM photo_att_data WHERE fid=" + std::to_string(id));
    finishTable(db.ds, "photos", nullptr);
    return ok;
}

bool gpkgAddPhotoAttachment(const std::string &path, long long fid, long long idx,
                            const void *data, size_t size) {
    if (fid <= 0 || data == nullptr || size == 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;

    if (idx <= 0) {
        // 主照片 BLOB：photos.photo_data（OFTBinary，OGR 二进制列直写）
        OGRLayer *layer = db.ds->GetLayerByName("photos");
        if (layer == nullptr) return false;
        OGRFeature *feat = layer->GetFeature((GIntBig) fid);
        if (feat == nullptr) return false;
        const int bi = feat->GetFieldIndex("photo_data");
        if (bi < 0) {
            OGRFeature::DestroyFeature(feat);
            return false;
        }
        feat->SetField(bi, (int) size, data);
        const bool ok = layer->SetFeature(feat) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feat);
        finishTable(db.ds, "photos", nullptr);
        return ok;
    }

    // 附件 BLOB：photo_att_data（不注册 gpkg_contents 的附加表），BLOB 经 x'hex' 字面量拼 SQL
    const std::string hex = hexEncode(data, size);
    execSql(db.ds, "INSERT OR REPLACE INTO photo_att_data (fid, idx, data) VALUES (" +
                   std::to_string(fid) + "," + std::to_string(idx) + ",x'" + hex + "')");
    finishTable(db.ds, "photos", nullptr);
    return auxHas(db.ds, fid, idx); // 回查确认落库（execSql 吞错，以结果为准）
}

bool gpkgReadPhotoBlob(const std::string &path, long long fid, long long idx,
                       std::vector<uint8_t> &out) {
    out.clear();
    if (path.empty() || fid <= 0) return false;
    GDALAllRegister();
    GDALDataset *ds = static_cast<GDALDataset *>(
            GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (ds == nullptr) return false;

    bool ok = false;
    if (idx <= 0) {
        // 主照片：photos.photo_data（OFTBinary 读出 Binary.paData/nCount 等价物）
        OGRLayer *layer = ds->GetLayerByName("photos");
        if (layer != nullptr) {
            OGRFeature *feat = layer->GetFeature((GIntBig) fid);
            if (feat != nullptr) {
                const int bi = feat->GetFieldIndex("photo_data");
                if (bi >= 0 && feat->IsFieldSetAndNotNull(bi)) {
                    int n = 0;
                    GByte *p = feat->GetFieldAsBinary(bi, &n);
                    if (p != nullptr && n > 0) {
                        out.assign(p, p + n);
                        ok = true;
                    }
                }
                OGRFeature::DestroyFeature(feat);
            }
        }
    } else {
        // 附件：SELECT hex(data) 再解码（附加表 GDAL 不可见，走 SQL 透传）
        const std::string sql = "SELECT hex(data) FROM photo_att_data WHERE fid=" +
                                std::to_string(fid) + " AND idx=" + std::to_string(idx);
        OGRLayer *res = ds->ExecuteSQL(sql.c_str(), nullptr, "SQLITE");
        if (res != nullptr) {
            OGRFeature *f = res->GetNextFeature();
            if (f != nullptr) {
                const char *hexStr = f->GetFieldAsString(0);
                if (hexStr != nullptr && hexDecode(hexStr, out) && !out.empty()) ok = true;
                OGRFeature::DestroyFeature(f);
            }
            ds->ReleaseResultSet(res);
        }
    }
    GDALClose(ds);
    return ok;
}

// ---- 分组 ----

bool gpkgInsertGroup(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject()) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("media_groups");
    if (layer == nullptr) return false;

    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    const long long recId = jsonInt(root, "id", 0);
    if (recId > 0) feat->SetFID((GIntBig) recId);
    setFieldStr(feat, "name", jsonStr(root, "name"));
    setFieldInt(feat, "created_time", jsonInt(root, "createdTime"));
    setFieldBool(feat, "visible", jsonBool(root, "visible", true));
    const bool ok = layer->CreateFeature(feat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "media_groups", nullptr);
    return ok;
}

bool gpkgUpdateGroup(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    const long long id = jsonInt(root, "id", 0);
    if (!root.isObject() || id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("media_groups");
    if (layer == nullptr) return false;
    OGRFeature *feat = layer->GetFeature((GIntBig) id);
    if (feat == nullptr) return false;

    setUpdateStr(feat, "name", jsonStr(root, "name"));
    const bool ok = layer->SetFeature(feat) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "media_groups", nullptr);
    return ok;
}

bool gpkgDeleteGroup(const std::string &path, long long id) {
    if (id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("media_groups");
    if (layer == nullptr) return false;
    const bool ok = layer->DeleteFeature((GIntBig) id) == OGRERR_NONE;
    // 组内点位移到未分组（-1），与内存语义一致
    execSql(db.ds, "UPDATE photos SET group_id=-1 WHERE group_id=" + std::to_string(id));
    finishTable(db.ds, "media_groups", nullptr);
    finishTable(db.ds, "photos", nullptr);
    return ok;
}

bool gpkgSetGroupPhotosVisible(const std::string &path, long long groupId, bool visible) {
    GpkgDb db(path);
    if (!db.ok()) return false;
    execSql(db.ds, std::string("UPDATE photos SET visible=") + (visible ? "1" : "0") +
                   " WHERE group_id=" + std::to_string(groupId));
    finishTable(db.ds, "photos", nullptr);
    return true;
}

// ---- 样地 ----

bool gpkgUpsertSurveyPlot(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    const long long id = jsonInt(root, "id", 0);
    if (!root.isObject() || id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("survey_plots");
    if (layer == nullptr) return false;

    // fid 冲突替换（对齐 Kotlin insertWithOnConflict CONFLICT_REPLACE）
    bool exists = false;
    {
        OGRFeature *probe = layer->GetFeature((GIntBig) id);
        if (probe != nullptr) {
            exists = true;
            OGRFeature::DestroyFeature(probe);
        }
    }

    OGRFeature *feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetFID((GIntBig) id);
    std::string plotType = jsonStr(root, "plotType");
    setFieldStr(feat, "name", jsonStr(root, "name"));
    setFieldStr(feat, "plot_type", plotType);
    const JsonValue &attrs = root.at("attributes");
    if (attrs.isString()) {
        setFieldStr(feat, "attributes", attrs.strVal);
    } else if (attrs.type != JsonValue::Type::Null) {
        std::string attrsJson;
        writeJsonValue(attrsJson, attrs);
        setFieldStr(feat, "attributes", attrsJson);
    }

    std::vector<double> pts;
    jsonPoints(root, pts);
    double bbox[4];
    bool hasBbox = false;
    if (pts.size() >= 3) {
        OGRGeometry *geom;
        if (plotType == "point") {
            geom = new OGRPoint(pts[0], pts[1], pts[2]);
        } else {
            geom = pointsToPolygon(pts);
        }
        hasBbox = geomBbox(geom, bbox);
        feat->SetGeometryDirectly(geom);
    }
    const bool ok = exists ? (layer->SetFeature(feat) == OGRERR_NONE)
                           : (layer->CreateFeature(feat) == OGRERR_NONE);
    OGRFeature::DestroyFeature(feat);
    finishTable(db.ds, "survey_plots", hasBbox ? bbox : nullptr);
    return ok;
}

bool gpkgDeleteSurveyPlot(const std::string &path, long long id) {
    if (id <= 0) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *layer = db.ds->GetLayerByName("survey_plots");
    if (layer == nullptr) return false;
    const bool ok = layer->DeleteFeature((GIntBig) id) == OGRERR_NONE;
    finishTable(db.ds, "survey_plots", nullptr);
    return ok;
}

// ---- 工程元数据 ----

bool gpkgSyncMeta(const std::string &path, const std::string &json) {
    JsonValue root = parseJson(json);
    if (!root.isObject()) return false;
    GpkgDb db(path);
    if (!db.ok()) return false;

    // 五键 upsert（DELETE+INSERT，project_meta.key 无唯一索引）；空值删行对齐写侧
    auto upsertMeta = [&db](const char *key, const std::string &value) {
        execSql(db.ds, std::string("DELETE FROM project_meta WHERE key='") + sqlQuote(key) + "'");
        if (value.empty()) return;
        execSql(db.ds, std::string("INSERT INTO project_meta (key, value) VALUES ('") +
                       sqlQuote(key) + "','" + sqlQuote(value) + "')");
    };
    upsertMeta("name", jsonStr(root, "name"));
    upsertMeta("description", jsonStr(root, "description"));
    upsertMeta("srs", jsonStr(root, "srs"));
    upsertMeta("createTime", std::to_string(jsonInt(root, "createTime")));
    upsertMeta("updateTime", std::to_string(jsonInt(root, "updateTime")));

    // layer_refs 全替换
    const JsonValue &refs = root.at("layerRefs");
    if (refs.isArray()) {
        execSql(db.ds, "DELETE FROM layer_refs");
        for (const JsonValue &r : refs.arr) {
            if (!r.isString() || r.strVal.empty()) continue;
            execSql(db.ds, std::string("INSERT INTO layer_refs (layer_path) VALUES ('") +
                           sqlQuote(r.strVal) + "')");
        }
        finishTable(db.ds, "layer_refs", nullptr);
    }
    finishTable(db.ds, "project_meta", nullptr);
    return true;
}

// ---- 照片文件清扫 ----

/** 解析某照片全部附件 (idx, path)：attachments JSON 优先，回退 photo_path 单附件
 *  （照 GeoPackageDml.attachmentPathsOf） */
static std::vector<std::pair<long long, std::string>> attachmentPathsOf(
        const std::string &photoPath, const std::string &attachments) {
    std::vector<std::pair<long long, std::string>> out;
    if (!attachments.empty()) {
        JsonValue arr = parseJson(attachments);
        if (arr.isArray()) {
            for (size_t i = 0; i < arr.arr.size(); i++) {
                const JsonValue &o = arr.arr[i];
                if (!o.isObject()) continue;
                std::string p = jsonStr(o, "path");
                if (!p.empty()) out.emplace_back((long long) i, p);
            }
        }
    }
    if (out.empty() && !photoPath.empty()) out.emplace_back(0, photoPath);
    return out;
}

bool gpkgSweepPhotoFiles(const std::string &path, const std::string &mediaRoot) {
    // 列不齐时先整文件重建，photo_data 列可能不存在（对齐 Kotlin 入口检查）
    if (!gpkgSchemaCurrent(path)) return false;
    char rootBuf[PATH_MAX];
    if (realpath(mediaRoot.c_str(), rootBuf) == nullptr) return false;
    const std::string root = rootBuf;

    GpkgDb db(path);
    if (!db.ok()) return false;
    OGRLayer *photos = db.ds->GetLayerByName("photos");
    if (photos == nullptr) return false;

    struct Row {
        long long fid;
        std::string photoPath;
        std::string attachments;
        bool hasMain;
    };
    std::vector<Row> rows;
    photos->ResetReading();
    OGRFeature *feat;
    while ((feat = photos->GetNextFeature()) != nullptr) {
        Row r;
        r.fid = (long long) feat->GetFID();
        r.photoPath = getFieldStr(feat, "photo_path");
        r.attachments = getFieldStr(feat, "attachments");
        const int bi = feat->GetFieldIndex("photo_data");
        r.hasMain = bi >= 0 && feat->IsFieldSetAndNotNull(bi);
        rows.push_back(std::move(r));
        OGRFeature::DestroyFeature(feat);
    }

    bool swept = false;
    for (const Row &row : rows) {
        for (const auto &kv : attachmentPathsOf(row.photoPath, row.attachments)) {
            const long long idx = kv.first;
            const std::string &p = kv.second;
            if (p.empty()) continue;
            // 绝对路径直接用；相对路径拼 mediaRoot（媒体根外的不动）
            const std::string full = (p[0] == '/') ? p : (root + "/" + p);
            char canonBuf[PATH_MAX];
            if (realpath(full.c_str(), canonBuf) == nullptr) continue; // 文件不存在
            const std::string canon = canonBuf;
            if (canon.compare(0, root.size(), root) != 0) continue; // 非托管目录

            const bool has = (idx == 0) ? row.hasMain : auxHas(db.ds, row.fid, idx);
            if (has) {
                // BLOB 已在库 → 删除磁盘原件（历史版本的照片文件退役）
                if (std::remove(canon.c_str()) == 0) swept = true;
                continue;
            }
            // BLOB 缺失且磁盘原件存在 → 读入库（主照片 photo_data / 附件附加表）后删除文件
            std::ifstream f(canon, std::ios::binary);
            if (!f) continue;
            f.seekg(0, std::ios::end);
            std::streamoff sz = f.tellg();
            if (sz <= 0) continue;
            std::vector<char> bytes((size_t) sz);
            f.seekg(0, std::ios::beg);
            f.read(bytes.data(), sz);
            bytes.resize((size_t) f.gcount());
            if (bytes.empty()) continue;

            bool stored = false;
            if (idx == 0) {
                OGRFeature *pf = photos->GetFeature((GIntBig) row.fid);
                if (pf != nullptr) {
                    const int bi = pf->GetFieldIndex("photo_data");
                    if (bi >= 0) {
                        pf->SetField(bi, (int) bytes.size(), bytes.data());
                        stored = photos->SetFeature(pf) == OGRERR_NONE;
                    }
                    OGRFeature::DestroyFeature(pf);
                }
            } else {
                execSql(db.ds, "INSERT OR REPLACE INTO photo_att_data (fid, idx, data) VALUES (" +
                               std::to_string(row.fid) + "," + std::to_string(idx) + ",x'" +
                               hexEncode(bytes.data(), bytes.size()) + "')");
                stored = auxHas(db.ds, row.fid, idx);
            }
            if (stored && std::remove(canon.c_str()) == 0) swept = true;
        }
    }
    if (swept) finishTable(db.ds, "photos", nullptr);
    return swept;
}

} // namespace gcbridge
