package com.zys.globecore

/**
 * 矢量屏幕过滤范围（WGS84 经纬度矩形），对应 native `readVectorFile`/`VectorLoader::reload` 的 extent 入参。
 *
 * 用于 [NativeMapView.addVectorLayer]（初始屏幕范围）与 [NativeMapView.updateVectorExtent]（相机静止后
 * 按新范围重载）：宿主（[com.zys.globecore] 之外的图层编排层）据相机可见范围算出本矩形传入，native
 * 按此矩形做空间相交查询（走 .qix/R-tree 索引）只加载屏内要素。[NativeMapView] 内部拆解为 JNI 基元参数
 * 传边界（值类仅在 Kotlin API 层）。传 null 表示整文件全量（小数据直显、不参与相机重载）。
 */
data class VectorExtent(
    /** 西边界（最小经度，度） */
    val minLon: Double,
    /** 南边界（最小纬度，度） */
    val minLat: Double,
    /** 东边界（最大经度，度） */
    val maxLon: Double,
    /** 北边界（最大纬度，度） */
    val maxLat: Double,
)
