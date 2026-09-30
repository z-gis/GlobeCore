#ifndef MOBILEMAP_SRS_RESOLVE_H
#define MOBILEMAP_SRS_RESOLVE_H

#include <string>
#include <algorithm>
#include <cctype>
#include <cstdio>

#include "gdal/ogr_spatialref.h"
#include "gdal/ogr_geometry.h"
#include "gdal/cpl_vsi.h"
#include "gdal/cpl_conv.h"
#include "gdal/ogrsf_frmts.h"

// ==================== 矢量图层源坐标系解析（DXF/DWG 无内置 SRS 兜底） ====================
//
// GDAL 的 DXF / DWG(CAD) 驱动不携带地理参考（官方文档：DXF files are considered to have
// no georeferencing information through OGR），OGRLayer::GetSpatialRef() 恒返回空，
// 导致投影米制坐标被当作 WGS84 经纬度渲染而整体错位。此处按优先级解析源 SRS：
//   1) 图层自身 SRS（shp / tif 等内嵌坐标系）；
//   2) 同目录同名 .prj 伴随文件（WKT / proj4 文本，GIS 惯例，不修改源数据）；
//   3) CAD 格式（.dxf/.dwg）且坐标含 3 度带带号前缀时，按 CGCS2000 3 度带高斯投影构造
//      （proj4 直接给出椭球与投影参数，不依赖 proj.db 查表）。
// 返回新建的 OGRSpatialReference（已设 OAMS_TRADITIONAL_GIS_ORDER，调用者负责 Release），
// 解析不到返回 nullptr（调用方按原 WGS84 逻辑兜底）。

/** 读取整个文本文件内容（限 1MB），失败返回 nullptr；调用者 CPLFree。 */
static inline char *srsReadWholeFile(const char *path) {
    VSILFILE *f = VSIFOpenL(path, "rb");
    if (f == nullptr) return nullptr;
    VSIFSeekL(f, 0, SEEK_END);
    const vsi_l_offset size = VSIFTellL(f);
    VSIFSeekL(f, 0, SEEK_SET);
    if (size == 0 || size > 1024 * 1024) {
        VSIFCloseL(f);
        return nullptr;
    }
    char *buf = (char *) CPLMalloc((size_t) size + 1);
    const size_t read = VSIFReadL(buf, 1, (size_t) size, f);
    VSIFCloseL(f);
    buf[read] = '\0';
    return buf;
}

/** 替换路径扩展名（无扩展名则追加）。 */
static inline std::string srsReplaceExtension(const std::string &p, const char *ext) {
    const size_t slash = p.find_last_of("/\\");
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return p + ext;
    }
    return p.substr(0, dot) + ext;
}

/** 是否为 CAD 格式（.dxf/.dwg），按扩展名小写比较。 */
static inline bool srsIsCad(const char *path) {
    std::string lp(path);
    std::transform(lp.begin(), lp.end(), lp.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    return lp.size() >= 4 &&
           (lp.compare(lp.size() - 4, 4, ".dxf") == 0 || lp.compare(lp.size() - 4, 4, ".dwg") == 0);
}

/** 按 CGCS2000 3 度带带号构造高斯投影 SRS（proj4 自含参数，不依赖 proj.db）。 */
static inline OGRSpatialReference *srsFromCgcs2000Zone(int zone) {
    const int centralMeridian = zone * 3;
    const double falseEasting = zone * 1000000.0 + 500000.0;
    char proj4[256];
    snprintf(proj4, sizeof(proj4),
             "+proj=tmerc +lat_0=0 +lon_0=%d +k=1 +x_0=%.0f +y_0=0 +ellps=GRS80 +units=m +no_defs",
             centralMeridian, falseEasting);
    OGRSpatialReference *srs = new OGRSpatialReference();
    srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (srs->importFromProj4(proj4) != OGRERR_NONE) {
        delete srs;
        return nullptr;
    }
    return srs;
}

/**
 * 解析图层源 SRS：图层自身 -> 同名 .prj -> CAD 按 CGCS2000 3 度带带号推断。
 * 返回新对象（调用者 Release），无则 nullptr。
 */
static inline OGRSpatialReference *resolveSourceSrs(const char *path, OGRLayer *layer) {
    // 1) 图层自身 SRS
    if (layer != nullptr && layer->GetSpatialRef() != nullptr) {
        OGRSpatialReference *own = layer->GetSpatialRef()->Clone();
        own->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return own;
    }
    // 2) 同目录同名 .prj 伴随文件
    if (path != nullptr) {
        const std::string prjPath = srsReplaceExtension(std::string(path), ".prj");
        char *content = srsReadWholeFile(prjPath.c_str());
        if (content != nullptr) {
            OGRSpatialReference *srs = new OGRSpatialReference();
            srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            if (srs->SetFromUserInput(content) == OGRERR_NONE) {
                CPLFree(content);
                return srs;
            }
            delete srs;
            CPLFree(content);
        }
    }
    // 3) CAD 无 SRS：X 坐标含 3 度带带号前缀（8 位、前两位 25~45）时按 CGCS2000 推断
    if (path != nullptr && srsIsCad(path) && layer != nullptr) {
        OGREnvelope ext;
        if (layer->GetExtent(&ext, TRUE) == OGRERR_NONE) {
            const int zoneMin = (int) (ext.MinX / 1000000.0);
            const int zoneMax = (int) (ext.MaxX / 1000000.0);
            if (zoneMin >= 25 && zoneMin <= 45 && zoneMin == zoneMax) {
                return srsFromCgcs2000Zone(zoneMin);
            }
        }
    }
    return nullptr;
}

/** CAD 格式且解析不到源 SRS（无 .prj 且无带号）：坐标无法定位，调用方应跳过加载并提示。 */
static inline bool cadLacksSrs(const char *path, OGRLayer *layer) {
    if (path == nullptr || !srsIsCad(path)) return false;
    OGRSpatialReference *srs = resolveSourceSrs(path, layer);
    if (srs != nullptr) {
        srs->Release();
        return false;
    }
    return true;
}

#endif // MOBILEMAP_SRS_RESOLVE_H
