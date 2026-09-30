package com.zys.globecore

import android.content.Context
import android.view.Choreographer
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.widget.OverScroller

/**
 * 手势识别器的动作接收端：[MapGestures] 把识别出的平移/缩放动作交给本接口，
 * 由 [NativeMapView] 实现（转调 native 相机操作并请求重绘）。
 */
internal interface GestureSink {
    /** native 实例是否可用（句柄已释放时手势不再驱动 native，对齐原 h != 0L 逐项守卫） */
    fun isReady(): Boolean

    /** 平移 [dxPx]/[dyPx] 像素（符号已在 [MapGestures] 侧折算为内容移动方向） */
    fun panBy(dxPx: Double, dyPx: Double)

    /** 以 ([focusX], [focusY]) 为锚缩放：[scaleFactor] > 1 为放大 */
    fun zoomBy(scaleFactor: Double, focusX: Float, focusY: Float)

    /** 双指旋转：相机 heading 累加增量（度）；「内容跟随手指」的方向折算已在本识别器完成（真机标定），
     * 实现方直接透传 native */
    fun rotateBy(deltaDeg: Double)

    /** 当前是否 3D 透视模式：双指纵向俯仰（tilt）仅在 3D 生效，且 3D 下双指拖动让位 tilt 不再平移 */
    fun is3DMode(): Boolean

    /** 视图高（像素）：供 tilt 按 参考实现 口径 180°·Δcy/视图高 换算俯仰量（与 native panBy 位移同单位） */
    fun viewHeightPx(): Int

    /** 双指纵向拖拽：相机 tilt 累加增量（度，正=向地平线方向倾视）；「上推倾视/下拉回正」的
     * 方向折算与灵敏度换算（180°/视图高，对齐 参考实现）已在本识别器完成，实现方仅做 3D 门控后透传 native（native 钳 [0,80]） */
    fun tiltBy(deltaDeg: Double)

    /** 请求重绘一帧（对齐 GLSurfaceView.requestRender） */
    fun requestRender()
}

/**
 * 地图手势识别器（自 [NativeMapView] 拆出，行为逐项等价）：
 * 拖动平移、双指捏合缩放、双指旋转（3D 方位角）、双指纵向俯仰（3D 仰角 tilt）、
 * 单击透出、长按入口（功能待定）、松手惯性滑行（OverScroller 减速 + Choreographer 逐帧把位移增量交给 [GestureSink.panBy]，
 * 对齐 参考实现 滑动阻尼）。
 *
 * 只负责「识别 + 调度」，不持有 native 句柄：全部动作经 [sink] 回调落地，
 * 句柄守卫、GL 请求由实现方（[NativeMapView]）承担。
 */
internal class MapGestures(
    context: Context,
    private val sink: GestureSink
) {

    /** 单击（非拖拽/长按）回调：透出视图内屏幕像素坐标，供宿主做矢量要素拾取。 */
    var onTap: ((x: Float, y: Float) -> Unit)? = null

    /** 捏合缩放：以焦点为锚调用缩放（scaleFactor>1 即两指张开 → 放大/拉近） */
    private val scaleDetector = ScaleGestureDetector(
        context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                if (sink.isReady()) {
                    sink.zoomBy(detector.scaleFactor.toDouble(), detector.focusX, detector.focusY)
                    sink.requestRender()
                }
                return true
            }
        }
    )

    /** 平移惯性滑动：松手后按 fling 初速度减速滑行（对齐 参考实现 滑动阻尼） */
    private val scroller = OverScroller(context)
    private val choreographer = Choreographer.getInstance()
    private var lastFlingX = 0
    private var lastFlingY = 0
    private var flingRunning = false

    /** 双指旋转基准角（两指连线的 atan2 弧度）；非双指态为 NaN，POINTER_DOWN 建立、UP 复位，防丢帧跳变 */
    private var lastPinchAngleRad = Double.NaN

    /** 双指俯仰基准：两指质心 Y（像素）；非双指态为 NaN，建基准/复位同 [lastPinchAngleRad] */
    private var lastPinchCentroidY = Float.NaN

    /** 俯仰灵敏度采用 参考实现 口径：tiltDelta = 180° · ΔcyPx / 视图高（见 onTouchEvent），不再用自定的
     * 固定「度/像素」常量（旧 0.15/0.25 为拍脑袋值，与 参考实现的 180°/屏高不一致） */

    /** 旋转/俯仰解耦阈值（度/事件）：双指连线角逐帧增量绝对值超此值判为「旋转手势」，本帧不据质心 Y 调仰角
     *（对齐 参考实现：旋转仅改 heading，tilt 为独立手势；我们保留双指俯仰故需此阈值近似解耦） */
    private val rotateSuppressTiltDeg = 0.8

    /** 两指连线夹角（弧度）。屏幕 y 向下 → 该角随手指「视觉顺时针」旋转而增大 */
    private fun pinchAngleRad(event: MotionEvent): Double = Math.atan2(
        (event.getY(1) - event.getY(0)).toDouble(),
        (event.getX(1) - event.getX(0)).toDouble()
    )

    /** 两指质心 Y（像素）：tilt 手势的纵向位移基准 */
    private fun pinchCentroidY(event: MotionEvent): Float =
        (event.getY(0) + event.getY(1)) * 0.5f

    /** fling 逐帧回调：取 OverScroller 当前位移增量透传平移并请求重绘，直到滑行结束 */
    private val flingFrame = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!flingRunning) return
            if (scroller.computeScrollOffset()) {
                val x = scroller.currX
                val y = scroller.currY
                val dx = x - lastFlingX
                val dy = y - lastFlingY
                lastFlingX = x
                lastFlingY = y
                if (sink.isReady() && (dx != 0 || dy != 0)) {
                    sink.panBy(dx.toDouble(), dy.toDouble())
                }
                sink.requestRender()
                choreographer.postFrameCallback(this)
            } else {
                flingRunning = false
            }
        }
    }

    /** 拖动平移（双击放大已移除）：像素位移透传（onScroll 的 distance 与手指位移反向，故取负） */
    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            // 按下
            override fun onDown(e: MotionEvent): Boolean = true

            // 单击（onSingleTapConfirmed：系统在其内部等待双击窗口后才触发；双击缩放已移除，
            // 此处仅保留回调本身，行为与拆分前一致）
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                onTap?.invoke(e.x, e.y)
                return true
            }

            // 长按（单指保持按住、未越过拖动阈值达系统长按时长后触发）：转投 [handleLongPress] 设计入口
            override fun onLongPress(e: MotionEvent) {
                handleLongPress(e.x, e.y)
            }

            // 滑动平移：取 gesture 的像素位移量（与手指位移反向，故取负）
            override fun onScroll(
                e1: MotionEvent?, e2: MotionEvent, distanceX: Float, distanceY: Float
            ): Boolean {
                if (scaleDetector.isInProgress) return false // 缩放进行中不平移，避免抢焦点
                // 3D 下双指纵向拖拽让位 tilt 俯仰（参考实现/主流地图惯例），单指平移不受影响
                if (e2.pointerCount >= 2 && sink.is3DMode()) return false
                if (sink.isReady()) {
                    sink.panBy(-distanceX.toDouble(), -distanceY.toDouble())
                    sink.requestRender()
                }
                return true
            }

            // 双击放大已移除（不重写 onDoubleTap，双击不再驱动任何缩放；首次抬手仍走单击）

            // 惯性滑动：以松手速度为初速度减速滑行
            override fun onFling(
                e1: MotionEvent?, e2: MotionEvent, velocityX: Float, velocityY: Float
            ): Boolean {
                if (scaleDetector.isInProgress) return false // 缩放进行中不触发惯性
                // 3D 双指手势（tilt/旋转）收尾不触发平移惯性，防抬手瞬间地图意外滑行
                if (e2.pointerCount >= 2 && sink.is3DMode()) return false
                if (!sink.isReady()) return false
                // 惯性滑行：以松手速度为初速度，OverScroller 按默认摩擦减速；逐帧把位移增量透传平移。
                // 速度符号与 panBy 一致（正 = 内容随手指方向移动），故直接传入。
                scroller.forceFinished(true)
                scroller.fling(
                    0, 0, velocityX.toInt(), velocityY.toInt(),
                    Int.MIN_VALUE, Int.MAX_VALUE, Int.MIN_VALUE, Int.MAX_VALUE
                )
                lastFlingX = 0
                lastFlingY = 0
                flingRunning = true
                choreographer.postFrameCallback(flingFrame)
                return true
            }
        }
    )

    /**
     * 触摸事件入口：分发捏合缩放与拖动/单击/长按手势识别器（手势只更新相机状态，是否重绘由各回调决定），
     * 并在分发前自行识别双指旋转与双指俯仰（ScaleGestureDetector 无旋转/俯仰输出：两指连线夹角逐帧
     * 增量直供 [GestureSink.rotateBy]，两指质心 Y 逐帧增量经灵敏度换算供 [GestureSink.tiltBy]（仅 3D），
     * 与缩放/平移可同时进行，同真实地图 App 双指操作习惯）。
     * 由 [NativeMapView.onTouchEvent] 转发。
     */
    fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            // 手指按下即停住惯性滑行，交回手动控制
            MotionEvent.ACTION_DOWN -> cancelFling()
            // 第二指落下：以本帧两指连线角/质心 Y 为旋转与俯仰基准；抬起任一指/全部抬起即复位
            MotionEvent.ACTION_POINTER_DOWN ->
                if (event.pointerCount == 2) {
                    lastPinchAngleRad = pinchAngleRad(event)
                    lastPinchCentroidY = pinchCentroidY(event)
                }
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP -> {
                lastPinchAngleRad = Double.NaN
                lastPinchCentroidY = Float.NaN
            }
        }
        if (event.actionMasked == MotionEvent.ACTION_MOVE && event.pointerCount == 2) {
            // 双指旋转：两指连线 atan2 角逐帧增量（内容跟随手指）。先算旋转量，据其大小决定本帧是否让位仰角。
            var rotDeltaDeg = 0.0
            val angle = pinchAngleRad(event)
            if (lastPinchAngleRad.isNaN()) {
                lastPinchAngleRad = angle // 未捕到 POINTER_DOWN 基准（如多指转双指），本帧仅建立基准不旋转
            } else {
                var deltaDeg = Math.toDegrees(angle - lastPinchAngleRad)
                lastPinchAngleRad = angle
                // 跨 ±180° 环绕的回跳：增量归一到 (-180, 180]，防临界帧 360° 大跳
                if (deltaDeg > 180.0) deltaDeg -= 360.0
                if (deltaDeg <= -180.0) deltaDeg += 360.0
                rotDeltaDeg = deltaDeg
                if (deltaDeg != 0.0 && sink.isReady()) {
                    // 两指连线 atan2 角增大（deltaDeg>0）对应手指视觉顺时针旋转；经真机验证，
                    // heading 增大时内容顺时针转（相机相对地转反向），故直接透传 deltaDeg 即内容跟随手指
                    sink.rotateBy(deltaDeg)
                    sink.requestRender()
                }
            }
            // 双指俯仰（仅 3D）：质心 Y 逐帧增量换算倾角——上推（dyPx<0）向地平线倾视、下拉回正；
            // 本帧若为旋转手势（|Δ角|≥阈值）则抑制仰角（对齐 参考实现 旋转不改 tilt），仅更新基准免跳变误触发大仰角。
            if (sink.is3DMode()) {
                val cy = pinchCentroidY(event)
                if (lastPinchCentroidY.isNaN()) {
                    lastPinchCentroidY = cy // 未捕到 POINTER_DOWN 基准（如多指转双指），本帧仅建基准不倾视
                } else {
                    // 双指俯仰：质心 Y 逐帧增量按 参考实现 口径 180°·Δcy/视图高 换算（与 native panBy 同单位）
                    val vhPx = sink.viewHeightPx()
                    val tiltDeltaDeg = if (vhPx > 0) -(cy - lastPinchCentroidY) * 180.0 / vhPx else 0.0
                    lastPinchCentroidY = cy
                    if (Math.abs(rotDeltaDeg) < rotateSuppressTiltDeg && tiltDeltaDeg != 0.0 && sink.isReady()) {
                        sink.tiltBy(tiltDeltaDeg)
                        sink.requestRender()
                    }
                }
            }
        }
        scaleDetector.onTouchEvent(event)
        gestureDetector.onTouchEvent(event)
        return true
    }

    /** 停掉惯性滑行的逐帧回调（视图销毁/手势重按时调用） */
    fun cancelFling() {
        scroller.forceFinished(true)
        flingRunning = false
    }

    /**
     * 长按处理入口（功能尚未确定，仅留占位）：由手势识别器 onLongPress 在单指保持按住、
     * 未越过拖动阈值达系统长按时长后触发，坐标为视图内像素。确定功能后在此实现（候选如
     * 落点上下文菜单 / 打点标注 / 测量起点等），并按需仿 [onTap] 向 [NativeMapView] 暴露宿主回调。
     * 对齐鸿蒙侧 XComponentBridge::handleLongPress 占位（同为一套手势双平台口径）。
     */
    @Suppress("UNUSED_PARAMETER")
    private fun handleLongPress(xPx: Float, yPx: Float) {
        // TODO：长按具体功能待定，确定后在此实现。
    }
}
