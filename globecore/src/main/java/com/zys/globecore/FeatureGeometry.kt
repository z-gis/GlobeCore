package com.zys.globecore

/**
 * 单个矢量要素的经纬度几何（WGS84 度），由 [NativeMapView.featureGeometry] 返回，供点击选中后画高亮叠加层。
 *  - [type]：0=点、1=线、2=面（对齐 native PickType）；
 *  - [lonlat]：摊平坐标 [lon0,lat0,lon1,lat1,...]，与 updateOverlay* 入参同构（无需换序）；
 *  - [ringCounts]：线为每段折线顶点数、面为每环顶点数（点为空）；
 *  - [ringsPerFeature]：面专用，本要素环数（首环外环、余为洞；点/线为空）。
 */
class FeatureGeometry(
    val type: Int,
    val lonlat: DoubleArray,
    val ringCounts: IntArray,
    val ringsPerFeature: IntArray
)
