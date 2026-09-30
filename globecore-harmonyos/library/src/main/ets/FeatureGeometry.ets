// FeatureGeometry.ets —— 单要素经纬度几何值对象（对应 Android FeatureGeometry.kt）。
//
// 由 NativeMapView.featureGeometry 返回，供点击选中后画高亮叠加层。
//  - type：0=点、1=线、2=面（对齐 native PickType）；
//  - lonlat：摊平坐标 [lon0,lat0,lon1,lat1,...]，与 updateOverlay* 入参同构；
//  - ringCounts：线为每段折线顶点数、面为每环顶点数（点为空）；
//  - ringsPerFeature：面专用，本要素环数（首环外环、余为洞）。

/// 拾取命中结果（Android 版 pickVector 返回 LongArray[2]，鸿蒙以值对象承载）
export class PickResult {
  /** 命中层 index（addVectorLayer/addOverlayLayer 加入次序） */
  layerIndex: number;
  /** 源数据要素 ID（<2^53，供按 FID 回取属性） */
  fid: number;

  constructor(layerIndex: number, fid: number) {
    this.layerIndex = layerIndex;
    this.fid = fid;
  }
}

export class FeatureGeometry {
  type: number;
  lonlat: number[];
  ringCounts: number[];
  ringsPerFeature: number[];

  constructor(type: number, lonlat: number[], ringCounts: number[], ringsPerFeature: number[]) {
    this.type = type;
    this.lonlat = lonlat;
    this.ringCounts = ringCounts;
    this.ringsPerFeature = ringsPerFeature;
  }
}
