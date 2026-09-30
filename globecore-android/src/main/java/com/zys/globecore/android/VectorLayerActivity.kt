package com.zys.globecore.android

import android.os.Bundle
import com.zys.globecore.Camera
import com.zys.globecore.FeatureGeometry
import com.zys.globecore.NativeLayerInfo
import com.zys.globecore.NativeMapView
import com.zys.globecore.VectorStyle
import java.io.File

/**
 * 演示 03 · 矢量图层与拾取（对应 tutorials/03「矢量图层与拾取」）：
 *  - [NativeMapView.addVectorLayer]：OGR 可打开的任意格式（此处内置 GeoJSON），
 *    [VectorStyle] 承载面填充/描边/标注样式，异步读取 + 三角剖分，就绪自动重绘；
 *  - [NativeMapView.pickVector]：屏幕点命中 → [layerIndex, fid]；
 *  - [NativeMapView.featureGeometry] + 叠加层：选中要素高亮（noPick 退出拾取竞争）；
 *  - [NativeLayerInfo.getLayerExtent]：建层前探测文件四至，相机一键缩放。
 */
class VectorLayerActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_vector_title)

    private var vectorIdx = -1
    private var highlightIdx = -1
    private lateinit var samplePath: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 把 assets 里的示例 GeoJSON 解到文件目录（OGR 按文件路径读取）
        samplePath = File(filesDir, "sample.geojson").apply {
            assets.open("sample.geojson").use { src ->
                outputStream().use { out -> src.copyTo(out) }
            }
        }.absolutePath

        // 在线高德影像底图；本演示默认 2D 平面
        map.setViewMode(NativeMapView.ViewMode.TWO_D)
        addBasemap(Camera(latitude = 39.91, longitude = 116.41, altitude = 25_000.0))

        addDemoAction(getString(R.string.action_add_vector)) { addVector() }
        addDemoAction("放大") { zoomIn() }
        addDemoAction("缩小") { zoomOut() }
    }

    private fun addVector() {
        if (vectorIdx >= 0) {
            toast("矢量层已加载（index=$vectorIdx）")
            return
        }
        // 建层前探测元信息：四至驱动相机缩放到数据范围
        NativeLayerInfo.getLayerExtent(samplePath)?.let { (minLon, minLat, maxLon, maxLat) ->
            toast("图层四至: [$minLon, $minLat, $maxLon, $maxLat]")
            map.setCamera(Camera(
                latitude = (minLat + maxLat) / 2,
                longitude = (minLon + maxLon) / 2,
                altitude = 22_000.0,
            ))
        }
        // labelField="name"：逐要素取 name 属性作文字标注（native 自动探测系统 CJK 字体）
        vectorIdx = map.addVectorLayer(
            samplePath,
            VectorStyle(
                fillColor = 0x664A8FE3.toInt(),
                outlineColor = 0xFFFFFFFF.toInt(),
                labelField = "name",
                labelOutline = true,
            ),
        )
        toast("矢量层已加入 index=$vectorIdx（异步读取，就绪自动显示）")

        // 单击拾取：命中要素画高亮叠加层（选中态口径），高亮层不参与拾取竞争
        map.setOnTapListener { x, y ->
            val hit = map.pickVector(x, y)
            if (hit == null) {
                clearHighlight()
            } else {
                val layer = hit[0].toInt()
                val fid = hit[1]
                val geo = map.featureGeometry(layer, fid)
                if (geo == null) {
                    toast("命中 layer=$layer fid=$fid（无 CPU 几何）")
                } else {
                    toast("命中 layer=$layer fid=$fid 类型=${geo.typeDesc()}")
                    highlight(geo, fid)
                }
            }
        }
    }

    private fun FeatureGeometry.typeDesc() = when (type) {
        0 -> "点"
        1 -> "线"
        else -> "面"
    }

    /** 选中高亮：金黄样式叠加层，几何直接从 featureGeometry 摊平数组透传（同构无需换序） */
    private fun highlight(geo: FeatureGeometry, fid: Long) {
        if (highlightIdx < 0) {
            highlightIdx = map.addOverlayLayer(
                VectorStyle(
                    fillColor = 0x66FFD700.toInt(),
                    outlineColor = 0xFFFFD700.toInt(),
                    outlineWidth = 3f,
                    lineColor = 0xFFFFD700.toInt(),
                    lineWidth = 4f,
                    pointColor = 0xFFFFD700.toInt(),
                )
            )
            map.setOverlayNoPick(highlightIdx, true)  // 瞬态高亮层不参与拾取
        }
        when (geo.type) {
            0 -> map.updateOverlayPoints(highlightIdx, geo.lonlat, LongArray(geo.lonlat.size / 2) { fid })
            1 -> map.updateOverlayLines(highlightIdx, geo.lonlat, geo.ringCounts, LongArray(geo.ringCounts.size) { fid })
            else -> map.updateOverlayPolygons(
                highlightIdx, geo.lonlat, geo.ringCounts, geo.ringsPerFeature,
                LongArray(geo.ringsPerFeature.size) { fid },
            )
        }
    }

    private fun clearHighlight() {
        if (highlightIdx >= 0) {
            map.removeOverlayLayer(highlightIdx)
            highlightIdx = -1
        }
    }

    override fun onDestroy() {
        clearHighlight()
        super.onDestroy()
    }
}
