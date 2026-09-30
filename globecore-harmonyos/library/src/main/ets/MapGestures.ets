// MapGestures.ets —— 地图手势识别器（对应 Android MapGestures.kt，行为逐项对齐）。
//
// Android 版基于 GestureDetector/ScaleGestureDetector/OverScroller/Choreographer；
// ArkUI 无对应检测器，本类基于 TouchEvent 多指坐标手写识别，输出与 GestureSink 接口不变：
//  - 单指拖动平移（起手 8vp 内不触发，对齐 touchSlop）；
//  - 双指捏合缩放（逐帧距离比，以两指质心为锚）；
//  - 双指旋转（两指连线 atan2 角逐帧增量，3D 门控在 sink 侧，同 Android）；
//  - 双指纵向俯仰 tilt（质心 Y 增量按 参考实现 口径 180°/视图高 换算，仅 3D；3D 下双指拖动让位 tilt 不平移）；
//  - 双击放大 2 倍（300ms/30vp 窗口）；单击透出（300ms 确认期避开双击，供拾取）；
//  - 松手惯性滑行（近 80ms 采样求初速，setTimeout 逐帧 ×0.95 帧衰减平移，
//    对齐 OverScroller 默认摩擦的减速手感）。
// 只负责「识别 + 调度」，不持有 native 句柄：全部动作经 sink 回调落地。

/** 手势识别结果的接收端（NativeMapView 实现，转调 native 相机操作并请求重绘） */
export interface GestureSink {
  /** native 实例是否可用（句柄已释放时手势不再驱动 native） */
  isReady(): boolean;

  /** 平移 dx/dy（vp，符号已折算为内容移动方向） */
  panBy(dxVp: number, dyVp: number): void;

  /** 以 (focusX, focusY) 为锚缩放：scaleFactor > 1 为放大 */
  zoomBy(scaleFactor: number, focusX: number, focusY: number): void;

  /** 双指旋转：相机 heading 累加增量（度），仅 3D 由实现方门控消费 */
  rotateBy(deltaDeg: number): void;

  /** 当前是否 3D 透视模式（双指俯仰/旋转门控 + 3D 下双指拖动让位 tilt） */
  is3DMode(): boolean;

  /** 视图高（vp）：供 tilt 按 参考实现 口径 180°·Δcy/视图高 换算俯仰量（与 TouchEvent 坐标同单位） */
  viewHeightVp(): number;

  /** 双指纵向拖拽：相机 tilt 累加增量（度，正=向地平线倾视），仅 3D 由实现方门控消费 */
  tiltBy(deltaDeg: number): void;

  /** 请求重绘一帧 */
  requestRender(): void;
}

/** 触点本地坐标（vp） */
interface Position2D {
  x: number;
  y: number;
}

interface FingerSample {
  t: number;
  x: number;
  y: number;
}

export class MapGestures {
  /** 单击（非双击/拖拽）确认回调：透出视图内屏幕坐标（vp），供宿主矢量要素拾取 */
  onTap: ((x: number, y: number) => void) | undefined = undefined;

  private sink: GestureSink;

  // ── 拖动 / 单击 / 双击 ──
  private fingers: Map<number, Position2D> = new Map();
  private downX: number = 0;
  private downY: number = 0;
  private downTime: number = 0;
  private dragging: boolean = false;
  private lastTapX: number = 0;
  private lastTapY: number = 0;
  private lastTapTime: number = 0;
  private singleTapTimer: number = -1;

  // ── 捏合 / 旋转 / 俯仰（双指）──
  private pinchInProgress: boolean = false;
  private lastPinchDist: number = 0;
  /** 两指连线 atan2 角（弧度）；非双指态 NaN，POINTER_DOWN 建基准、UP 复位，防丢帧跳变 */
  private lastPinchAngleRad: number = Number.NaN;
  /** 两指质心 Y（vp）：tilt 手势纵向基准 */
  private lastPinchCentroidY: number = Number.NaN;
  /** 俯仰灵敏度采用 参考实现 口径：tiltDelta = 180° · ΔcyVp / 视图高（见 onPinchMove），不再用自定的
   * 固定「度/vp」常量（旧 0.15/0.25 为拍脑袋值，与 参考实现的 180°/屏高不一致） */
  /** 旋转/俯仰解耦阈值（度/事件）：双指连线角逐帧增量绝对值超此值判为「旋转手势」，本帧不据质心 Y 调仰角 */
  private readonly rotateSuppressTiltDeg: number = 0.8;

  // ── 惯性滑行 ──
  private samples: FingerSample[] = [];
  private flingRunning: boolean = false;
  private flingVx: number = 0;
  private flingVy: number = 0;
  private lastFlingTs: number = 0;

  // 识别阈值（vp/ms）
  private readonly touchSlop: number = 8;
  private readonly doubleTapTimeout: number = 300;
  private readonly doubleTapSlop: number = 30;
  private readonly flingMinVelocity: number = 150; // vp/s
  private readonly flingFrictionPerFrame: number = 0.95;

  constructor(sink: GestureSink) {
    this.sink = sink;
  }

  /**
   * 触摸事件入口（NativeMapView 的 onTouchEvent 转发，坐标已折算为视图本地 vp）。
   * 双指旋转/俯仰在分发前自行识别（与捏合/平移共用同一事件序列互不排斥），
   * 行为对齐 Android 版 onTouchEvent 的手动检测段。
   */
  onTouchEvent(event: TouchEvent): boolean {
    switch (event.type) {
      case TouchType.Down:
        this.onFingerDown(event);
        break;
      case TouchType.Move:
        this.onFingerMove(event);
        break;
      case TouchType.Up:
      case TouchType.Cancel:
        this.onFingerUp(event, event.type === TouchType.Cancel);
        break;
      default:
        break;
    }
    return true;
  }

  /** 停掉惯性滑行的逐帧回调（视图销毁/手势重按时调用） */
  cancelFling(): void {
    this.flingRunning = false;
  }

  // ── 内部实现 ──

  private onFingerDown(event: TouchEvent): void {
    this.registerFingers(event);
    this.cancelFling(); // 按下即停住惯性滑行，交回手动控制
    if (this.fingers.size === 1) {
      const f = this.firstFinger();
      this.downX = f.x;
      this.downY = f.y;
      this.downTime = event.timestamp;
      this.dragging = false;
      this.samples = [];
      this.pushSample(event.timestamp, f.x, f.y);
    } else if (this.fingers.size === 2) {
      // 第二指落下：以本帧两指连线角/距离/质心 Y 为旋转、捏合与俯仰基准
      this.startPinch(event);
    }
  }

  private onFingerMove(event: TouchEvent): void {
    this.registerFingers(event);
    if (this.fingers.size >= 2) {
      this.onPinchMove(event);
      return;
    }
    // 单指：平移
    const f = this.firstFinger();
    if (!this.dragging) {
      const slop = Math.abs(f.x - this.downX) + Math.abs(f.y - this.downY);
      if (slop < this.touchSlop) {
        this.pushSample(event.timestamp, f.x, f.y);
        return;
      }
      this.dragging = true;
    }
    const last = this.lastSample();
    const dx = f.x - last.x;
    const dy = f.y - last.y;
    this.pushSample(event.timestamp, f.x, f.y);
    if ((dx !== 0 || dy !== 0) && this.sink.isReady()) {
      this.sink.panBy(dx, dy);
      this.sink.requestRender();
    }
  }

  private onFingerUp(event: TouchEvent, canceled: boolean): void {
    this.registerFingers(event);
    const up = event.changedTouches.length > 0 ? event.changedTouches[0] : null;
    // 移除去掉的手指（Up/Cancel 后剩余数）
    if (up !== null) {
      this.fingers.delete(up.id);
    } else {
      this.fingers.clear();
    }
    if (this.fingers.size < 2) {
      this.pinchInProgress = false;
      this.lastPinchAngleRad = Number.NaN;
      this.lastPinchCentroidY = Number.NaN;
    }
    if (this.fingers.size !== 0 || canceled) {
      return;
    }
    // 全指抬起：判定单击/双击/惯性
    const f = up !== null ? up : this.firstFinger();
    const moved = Math.abs(f.x - this.downX) + Math.abs(f.y - this.downY);
    const duration = event.timestamp - this.downTime;
    if (!this.dragging && !this.pinchInProgress
      && moved < this.touchSlop && duration < this.doubleTapTimeout) {
      const sinceLast = event.timestamp - this.lastTapTime;
      const near = Math.abs(f.x - this.lastTapX) < this.doubleTapSlop
        && Math.abs(f.y - this.lastTapY) < this.doubleTapSlop;
      if (this.lastTapTime > 0 && sinceLast < this.doubleTapTimeout && near) {
        // 双击放大：以点击点为锚放大 2 倍；撤销待确认的单击
        this.clearSingleTapTimer();
        this.lastTapTime = 0;
        if (this.sink.isReady()) {
          this.sink.zoomBy(2.0, f.x, f.y);
          this.sink.requestRender();
        }
      } else {
        this.lastTapX = f.x;
        this.lastTapY = f.y;
        this.lastTapTime = event.timestamp;
        this.scheduleSingleTap(f.x, f.y);
      }
    } else {
      this.lastTapTime = 0;
      // 惯性滑动：以松手速度为初速度减速滑行（缩放进行中/3D 双指收尾不触发，对齐 Android 门控）
      const v = this.computeFlingVelocity();
      if (!this.pinchInProgress && v !== null && this.sink.isReady()) {
        this.startFling(v[0], v[1]);
      }
    }
    this.dragging = false;
    this.samples = [];
  }

  /** 双指序列：把当前所有触点快照进 fingers（id → vp 坐标） */
  private registerFingers(event: TouchEvent): void {
    const next: Map<number, Position2D> = new Map();
    for (let i = 0; i < event.touches.length; i++) {
      const t = event.touches[i];
      next.set(t.id, { x: t.x, y: t.y } as Position2D);
    }
    this.fingers = next;
  }

  private firstFinger(): Position2D {
    const it = this.fingers.values();
    const v = it.next();
    return v.done ? { x: 0, y: 0 } as Position2D : v.value;
  }

  private twoFingers(): Position2D[] | null {
    if (this.fingers.size !== 2) {
      return null;
    }
    const arr: Position2D[] = [];
    this.fingers.forEach((p: Position2D) => {
      arr.push(p);
    });
    return arr;
  }

  private startPinch(event: TouchEvent): void {
    const two = this.twoFingers();
    if (two === null) {
      return;
    }
    this.pinchInProgress = true;
    this.dragging = true; // 双指期间不触发单击判定
    this.lastPinchDist = MapGestures.distance(two[0], two[1]);
    this.lastPinchAngleRad = MapGestures.angleRad(two[0], two[1]);
    this.lastPinchCentroidY = (two[0].y + two[1].y) * 0.5;
  }

  private onPinchMove(event: TouchEvent): void {
    const two = this.twoFingers();
    if (two === null) {
      return;
    }
    if (!this.pinchInProgress) {
      this.startPinch(event);
      return;
    }
    const dist = MapGestures.distance(two[0], two[1]);
    const cx = (two[0].x + two[1].x) * 0.5;
    const cy = (two[0].y + two[1].y) * 0.5;

    // 捏合缩放：逐帧距离比（factor>1 张开放大），以质心为锚
    if (dist > 0 && this.lastPinchDist > 0 && this.sink.isReady()) {
      this.sink.zoomBy(dist / this.lastPinchDist, cx, cy);
      this.sink.requestRender();
    }
    this.lastPinchDist = dist;

    // 双指旋转：两指连线 atan2 角逐帧增量（内容跟随手指；3D 门控在 sink 侧）。先算旋转量，据其大小决定本帧是否让位仰角。
    let rotDeltaDeg = 0;
    const angle = MapGestures.angleRad(two[0], two[1]);
    if (Number.isNaN(this.lastPinchAngleRad)) {
      this.lastPinchAngleRad = angle;
    } else {
      let deltaDeg = (angle - this.lastPinchAngleRad) * 180 / Math.PI;
      this.lastPinchAngleRad = angle;
      // 跨 ±180° 环绕回跳：归一到 (-180, 180]
      if (deltaDeg > 180) {
        deltaDeg -= 360;
      }
      if (deltaDeg <= -180) {
        deltaDeg += 360;
      }
      rotDeltaDeg = deltaDeg;
      if (deltaDeg !== 0 && this.sink.isReady()) {
        this.sink.rotateBy(deltaDeg);
        this.sink.requestRender();
      }
    }

    // 双指俯仰（仅 3D）：质心 Y 逐帧增量换算倾角——上推（dy<0）向地平线倾视、下拉回正；
    // 本帧若为旋转手势（|Δ角|≥阈值）则抑制仰角（对齐 参考实现 旋转不改 tilt），仅更新基准免跳变误触发大仰角。
    if (this.sink.is3DMode()) {
      if (Number.isNaN(this.lastPinchCentroidY)) {
        this.lastPinchCentroidY = cy; // 未捕到基准（如多指转双指），本帧仅建基准
      } else {
        // 双指俯仰：质心 Y 逐帧增量按 参考实现 口径 180°·Δcy/视图高 换算（cy 与视图高同为 vp）
        const vh = this.sink.viewHeightVp();
        const tiltDelta = vh > 0 ? -(cy - this.lastPinchCentroidY) * 180.0 / vh : 0;
        this.lastPinchCentroidY = cy;
        if (Math.abs(rotDeltaDeg) < this.rotateSuppressTiltDeg && tiltDelta !== 0 && this.sink.isReady()) {
          this.sink.tiltBy(tiltDelta);
          this.sink.requestRender();
        }
      }
    }

    // 2D 下双指拖动平移让位（3D 让位 tilt）：与 Android onScroll 双指门控同口径，此处双指不平移
  }

  private pushSample(t: number, x: number, y: number): void {
    this.samples.push({ t: t, x: x, y: y } as FingerSample);
    // 仅保留近 100ms 采样
    while (this.samples.length > 2 && t - this.samples[0].t > 100) {
      this.samples.shift();
    }
  }

  private lastSample(): FingerSample {
    if (this.samples.length === 0) {
      const f = this.firstFinger();
      return { t: 0, x: f.x, y: f.y } as FingerSample;
    }
    return this.samples[this.samples.length - 1];
  }

  /** 近 80ms 位移/时长求初速（vp/s）；样本不足或过慢返回 null */
  private computeFlingVelocity(): number[] | null {
    const n = this.samples.length;
    if (n < 2) {
      return null;
    }
    const first = this.samples[0];
    const last = this.samples[n - 1];
    const dtMs = last.t - first.t;
    if (dtMs <= 0 || dtMs > 120) {
      return null;
    }
    const vx = (last.x - first.x) * 1000 / dtMs;
    const vy = (last.y - first.y) * 1000 / dtMs;
    if (Math.sqrt(vx * vx + vy * vy) < this.flingMinVelocity) {
      return null;
    }
    return [vx, vy];
  }

  private startFling(vx: number, vy: number): void {
    this.flingVx = vx;
    this.flingVy = vy;
    this.lastFlingTs = 0;
    this.flingRunning = true;
    setTimeout(this.flingFrame, 16);
  }

  /** fling 逐帧：按帧间隔把衰减后的速度增量透传平移，直到速度低于阈值 */
  private flingFrame = (): void => {
    if (!this.flingRunning) {
      return;
    }
    const now = Date.now();
    if (this.lastFlingTs === 0) {
      this.lastFlingTs = now;
      setTimeout(this.flingFrame, 16);
      return;
    }
    const dt = (now - this.lastFlingTs) / 1000;
    this.lastFlingTs = now;
    // 摩擦衰减：按 16ms 基准帧归一（0.95/frame ≈ OverScroller 默认摩擦手感）
    const decay = Math.pow(this.flingFrictionPerFrame, dt / 0.016);
    this.flingVx *= decay;
    this.flingVy *= decay;
    if (Math.sqrt(this.flingVx * this.flingVx + this.flingVy * this.flingVy) < 20) {
      this.flingRunning = false;
      return;
    }
    if (this.sink.isReady()) {
      this.sink.panBy(this.flingVx * dt, this.flingVy * dt);
      this.sink.requestRender();
    }
    setTimeout(this.flingFrame, 16);
  };

  private scheduleSingleTap(x: number, y: number): void {
    this.clearSingleTapTimer();
    this.singleTapTimer = setTimeout(() => {
      this.singleTapTimer = -1;
      if (this.onTap !== undefined) {
        this.onTap(x, y);
      }
    }, this.doubleTapTimeout);
  }

  private clearSingleTapTimer(): void {
    if (this.singleTapTimer >= 0) {
      clearTimeout(this.singleTapTimer);
      this.singleTapTimer = -1;
    }
  }

  private static distance(a: Position2D, b: Position2D): number {
    return Math.sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
  }

  /** 两指连线 atan2 角（弧度）：屏幕 y 向下 → 该角随手指「视觉顺时针」旋转而增大 */
  private static angleRad(a: Position2D, b: Position2D): number {
    return Math.atan2(b.y - a.y, b.x - a.x);
  }
}
