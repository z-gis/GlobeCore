package com.zys.globecore

/**
 * 地理位置值对象，参照 参考实现 Position：纬度 / 经度 / 高度。
 *
 * 单位口径对齐本模块既有约定：经纬度用「度」、高度用「米」（区别于 参考实现的 [Angle] 包装对象）。
 * 用于 NativeMapView 中以「裸经纬度」传递/返回地理点的接口，改为以约定类传参：
 *  - [NativeMapView.setLocationMarker] 的定位标记坐标；
 *  - [NativeMapView.screenToGeo] 屏幕点反算的地理坐标。
 *
 * [altitude] 默认 0（当前 2D 视图的地面点无高度语义，字段先行建模、对齐 参考实现 Position）。
 */
data class Position(
    val latitude: Double,
    val longitude: Double,
    val altitude: Double = 0.0,
)
