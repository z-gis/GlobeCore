package com.zys.globecore.android

/**
 * 演示用瓦片图源常量。
 *
 * 0.1.2.0 起 demo 默认使用**高德在线影像瓦片**（无需 token、国内可达），不再内置离线底图；
 * 正式宿主请替换为自己有授权的图源 URL 模板（如天地图，Token 须已替换进模板）。
 */
object TileSources {

    /**
     * 高德影像（卫星）瓦片 XYZ：native 按 {z}/{x}/{y}/{rand} 占位替换。
     * {rand=1,2,3,4} 为四台负载子域（webst01..04），native 轮询取值以摊薄单域压力（见 TileLoader）。
     */
    const val AMAP_IMAGERY =
        "https://webst0{rand=1,2,3,4}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}"

    /**
     * 高德注记（路网/地名）瓦片：透明 PNG，作 overlay 叠在影像之上（style=8 为注记层，与影像同源子域）。
     * demo 0.1.2.0 已只保留影像底图（不挂注记层），此常量留存供宿主自行启用。
     */
    const val AMAP_LABEL =
        "https://webst0{rand=1,2,3,4}.is.autonavi.com/appmaptile?style=8&x={x}&y={y}&z={z}"

    /** 高德瓦片图源最大级别 */
    const val AMAP_MAX_LEVEL = 18

    /** OSM 标准瓦片（备用图源）：native 按 {z}/{x}/{y} 占位替换 */
    const val OSM = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

    /** OSM 标准瓦片图源最大级别 */
    const val OSM_MAX_LEVEL = 19
}
