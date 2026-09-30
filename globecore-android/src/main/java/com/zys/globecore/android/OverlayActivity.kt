package com.zys.globecore.android

import android.os.Bundle
import com.zys.globecore.Camera
import com.zys.globecore.NativeMapView
import com.zys.globecore.Position
import com.zys.globecore.VectorStyle
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

/**
 * 演示 04 · 动态叠加层绘制（测量口径，参照 MobileMap-android `MeasureCaptureController`）：
 *  - 「顶点标记层 + 几何层」双图层分工：native 叠加层为整层覆盖式更新、单层单帧只能承载一种几何类型，
 *    故顶点圆点须独立成层——每次点击先把全部测点画成圆点给出「点已加上」的即时反馈（首点即有），
 *    再按点数阈值在几何层连线/成面（折线 ≥2 连线；多边形模式 ==2 先画开链线、≥3 闭合面）；
 *  - [NativeMapView.addOverlayLayer] 建层、updateOverlay* 覆盖式推送、removeOverlayLayer 整类拆除。
 *
 * 交互：单击地图加点 → 顶点即时出点、续点自动连线/成面；「切换多边形」「撤销点」「清空」维护点集。
 */
class OverlayActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_overlay_title)

    private val points = mutableListOf<Position>()

    /** 几何层 index：承载折线 / 多边形 */
    private var geomIdx = -1

    /** 顶点标记层 index：每次点击把全部测点即时画成圆点（单层单帧仅一种几何，点须独立成层） */
    private var markerIdx = -1

    private var polygonMode = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 在线高德影像底图 + 注记层（近景绘制线/面）；本演示默认 2D 平面
        map.setViewMode(NativeMapView.ViewMode.TWO_D)
        addBasemap(Camera(latitude = 39.915, longitude = 116.40, altitude = 12_000.0))

        // 几何层：橙红折线 + 半透明填充（多边形模式生效）
        geomIdx = map.addOverlayLayer(
            VectorStyle(
                fillColor = 0x4DE56B33,
                outlineColor = 0xFFE56B33.toInt(),
                lineColor = 0xFFE56B33.toInt(),
                lineWidth = 3f,
            )
        )
        // 顶点标记层：仅画圆点（半径略大保证醒目），承载每次点击的即时反馈
        markerIdx = map.addOverlayLayer(
            VectorStyle(
                pointColor = 0xFFE56B33.toInt(),
                pointRadiusDp = 7f,
            )
        )

        refresh()
        map.setOnTapListener { x, y ->
            map.screenToGeo(x, y)?.let {
                points.add(it)
                refresh()
            }
        }

        addDemoAction("切换：多边形") { toggleMode() }
        addDemoAction(getString(R.string.action_undo)) {
            if (points.isNotEmpty()) {
                points.removeAt(points.lastIndex)
                refresh()
            }
        }
        addDemoAction(getString(R.string.action_clear)) {
            points.clear()
            refresh()
        }
        addDemoAction("示例圆周") { loadCircleSample() }
        addDemoAction("放大") { zoomIn() }
        addDemoAction("缩小") { zoomOut() }
    }

    private fun toggleMode() {
        polygonMode = !polygonMode
        toast(if (polygonMode) "当前：多边形（首尾自动闭合成面）" else "当前：折线")
        // 按钮文案无法就地改（demo 简化），以 toast 提示当前模式
        refresh()
    }

    /**
     * 覆盖式重绘两类几何：
     *  - 标记层：全量刷新当前点集为圆点（≥1 点即出点，撤销/清空回退到 0 点则清空）——即时点击反馈；
     *  - 几何层：先清空两类几何避免线/面切换残留，再按模式/点数阈值推送——
     *    多边形模式 ≥3 闭合面，否则 ≥2 画开链折线（对齐参照实现：2 点即先给连线反馈）。
     */
    private fun refresh() {
        val lonlat = DoubleArray(points.size * 2).also { arr ->
            points.forEachIndexed { i, p ->
                arr[i * 2] = p.longitude
                arr[i * 2 + 1] = p.latitude
            }
        }
        // ① 顶点圆点即时反馈：每次点击都全量刷新标记层
        if (points.isEmpty()) {
            map.updateOverlayPoints(markerIdx, DoubleArray(0))
        } else {
            map.updateOverlayPoints(markerIdx, lonlat)
        }
        // ② 几何层：先清空再按阈值推送，避免线/面切换时残留旧几何
        map.updateOverlayLines(geomIdx, DoubleArray(0), IntArray(0))
        map.updateOverlayPolygons(geomIdx, DoubleArray(0), IntArray(0), IntArray(0))
        when {
            polygonMode && points.size >= 3 -> {
                // 闭合环：末尾补首个点
                val closed = lonlat + doubleArrayOf(lonlat[0], lonlat[1])
                map.updateOverlayPolygons(
                    geomIdx, closed,
                    ringVertexCounts = intArrayOf(points.size + 1),
                    ringsPerFeature = intArrayOf(1),
                    labels = arrayOf("${points.size} 顶点面"),
                )
            }
            points.size >= 2 -> {
                map.updateOverlayLines(
                    geomIdx, lonlat,
                    vertexCounts = intArrayOf(points.size),
                    labels = arrayOf("${points.size} 顶点线"),
                )
            }
        }
    }

    /** 一键生成示例：以相机为中心的 32 边形（演示批量几何推送） */
    private fun loadCircleSample() {
        val c = map.getCamera() ?: return
        points.clear()
        val rDeg = 0.03
        for (i in 0 until 32) {
            val a = i * 2 * PI / 32
            points.add(
                Position(
                    latitude = c.latitude + rDeg * sin(a),
                    longitude = c.longitude + rDeg * cos(a) / cos(PI / 180 * c.latitude),
                )
            )
        }
        polygonMode = true
        refresh()
        toast("已生成 32 顶点示例多边形")
    }
}
