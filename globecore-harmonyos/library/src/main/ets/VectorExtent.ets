// VectorExtent.ets —— 矢量屏幕过滤范围值对象（对应 Android VectorExtent.kt）。
//
// WGS84 经纬度矩形，用于 addVectorLayer 初始范围与 updateVectorExtent 相机静止重载：
// native 按此矩形做空间相交查询（.qix/R-tree 索引）只加载屏内要素。
// undefined/null 表示整文件全量（小数据直显、不参与相机重载）。
export class VectorExtent {
  /** 西边界（最小经度，度） */
  minLon: number;
  /** 南边界（最小纬度，度） */
  minLat: number;
  /** 东边界（最大经度，度） */
  maxLon: number;
  /** 北边界（最大纬度，度） */
  maxLat: number;

  constructor(minLon: number, minLat: number, maxLon: number, maxLat: number) {
    this.minLon = minLon;
    this.minLat = minLat;
    this.maxLon = maxLon;
    this.maxLat = maxLat;
  }
}
