// MapDemoPage.ets —— 综合演示页：在线高德影像底图 + 2D/3D + 手势 + 矢量层 + 本地栅格 + 拾取高亮 + 测量。
//
// 对齐 Android 侧演示口径（0.1.1.1：删除离线基图，默认高德影像；单页承载全部能力开关，Index 清单经 params.demo 分流）：
//  0 瓦片底图：只呈现 3D 球体（在线高德影像层）+ 放大/缩小/旋转/仰角 按钮 + 单击取点；
//  1 Globe 3D：setViewMode 2D/3D 切换，共用相机（视角连续）+ 放大/缩小；
//  2 Vector Layer：默认 2D，addVectorLayer 读沙箱矢量（内置 sample.geojson 经 ensureSampleData 落盘，
//    亦可 hdc 推送 shp/kml 等到 filesDir 覆盖）+ 单击拾取高亮 + 放大/缩小；
//  3 Overlay：默认 2D，测量口径——顶点标记层即时出点 + 几何层连线（对齐 Android/MobileMap 双图层）+ 放大/缩小；
//  4 Location Marker：setLocationMarker 蓝点 + 方位角箭头。
// 注意：NativeMapView 为 @Component struct 不可 new，组件引用经 onReady 回调挂载后再驱动 API。
import { promptAction, router } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import fs from '@ohos.file.fs';
import {
  NativeMapView, ViewMode, Camera, Position, VectorStyle, PickResult, FeatureGeometry,
} from '@zys/globecore';

const DEMO_TAG: string = 'MapDemoPage';

/** 高德影像瓦片（{rand=1,2,3,4} 为负载子域，native TileLoader 轮询取值） */
const AMAP_IMAGERY: string =
  'https://webst0{rand=1,2,3,4}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}';
const AMAP_MAX_LEVEL: number = 18;

/** 路由参数：与 Index 清单序号一致（见文件头演示项分流说明） */
class DemoParams {
  demo: number;

  constructor(demo: number) {
    this.demo = demo;
  }
}

@Entry
@Component
struct MapDemoPage {
  /** 组件引用由 NativeMapView.onReady 在子组件 aboutToAppear 时挂载（native 句柄已就绪） */
  private mapView: NativeMapView | undefined = undefined;
  private context = getContext(this) as common.UIAbilityContext;
  private demoIndex: number = 0;
  @State private viewMode3d: boolean = false;
  @State private statusText: string = '底图瓦片加载中…';
  @State private measureCount: number = 0;
  private vectorIndex: number = -1;
  private vectorOn: boolean = false;
  private rasterIndex: number = -1;
  private rasterOn: boolean = false;
  private highlightIndex: number = -1;
  private highlightReady: boolean = false;
  /** 测量叠加层 index（demo 3）：几何层（折线）+ 顶点标记层（即时点击反馈）与已采集点集 */
  private measureOverlay: number = -1;
  private measureMarker: number = -1;
  private measurePts: Position[] = [];

  /** 瓦片磁盘缓存目录（沙箱 filesDir 下，与 Android /调查宝/tiles 布局同构） */
  private imageryCacheDir(): string {
    return `${this.context.filesDir}/tiles/gaode`;
  }

  /** filesDir 里第一个匹配后缀的样例文件路径；无则 undefined */
  private findSample(suffixes: string[]): string | undefined {
    try {
      const names = fs.listFileSync(this.context.filesDir);
      for (let i = 0; i < names.length; i++) {
        const lower = names[i].toLowerCase();
        for (let j = 0; j < suffixes.length; j++) {
          if (lower.endsWith(suffixes[j])) {
            return `${this.context.filesDir}/${names[i]}`;
          }
        }
      }
    } catch (e) {
      console.error(`${DEMO_TAG}: list filesDir failed: ${e}`);
    }
    return undefined;
  }

  /**
   * 把 entry rawfile 内置的 sample.geojson 解到 filesDir（对齐 Android assets→filesDir 落盘）：
   * 使矢量演示无需 hdc 推送即有数据可拾取（修复“该页面点击不了要素”——此前无样例文件即无可拾取几何）。
   * 已存在则跳过；失败仅记日志。
   */
  private ensureSampleData(): void {
    const dest = `${this.context.filesDir}/sample.geojson`;
    try {
      if (fs.accessSync(dest)) {
        return;
      }
      const content: Uint8Array = this.context.resourceManager.getRawFileContentSync('sample.geojson');
      const file = fs.openSync(dest, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
      fs.writeSync(file.fd, content.buffer);
      fs.closeSync(file);
    } catch (e) {
      console.error(`${DEMO_TAG}: extract bundled sample.geojson failed: ${e}`);
    }
  }

  private toast(text: string): void {
    promptAction.showToast({ message: text, duration: 2000 });
  }

  aboutToAppear(): void {
    // build 前读取清单序号；native 就绪后 initMap 按序号装配初始演示态
    const params = router.getParams() as DemoParams;
    if (params !== undefined && params !== null) {
      this.demoIndex = params.demo;
    }
  }

  /** native 实例就绪：底图/相机/拾取回调装配 + 按 demo 序号分流初始态 */
  private initMap(view: NativeMapView): void {
    this.mapView = view;
    this.ensureSampleData();
    // 在线高德影像底图（联网取瓦片）
    view.addTileLayer(this.imageryCacheDir(), AMAP_IMAGERY, AMAP_MAX_LEVEL, false);
    // 单击 → 拾取/测量/反算（按 demo 分流，见 onMapTap）
    view.setOnTapListener((x: number, y: number): void => {
      this.onMapTap(x, y);
    });

    if (this.demoIndex === 0) {
      // 01 瓦片底图：只呈现 3D 球体
      this.viewMode3d = true;
      view.setViewMode(ViewMode.THREE_D);
      view.setCamera(new Camera(39.9, 116.4, 3000000.0));
      this.statusText = '01 瓦片底图（3D）：单击取点，下方按钮缩放/旋转/仰角';
    } else if (this.demoIndex === 1) {
      // 02 2D/3D 切换：起步 3D 球体，可切回 2D
      this.viewMode3d = true;
      view.setViewMode(ViewMode.THREE_D);
      view.setCamera(new Camera(30.0, 110.0, 30000000.0));
      this.statusText = '02 2D/3D 切换：点“切到2D/3D”按钮';
    } else if (this.demoIndex === 2) {
      // 03 矢量：默认 2D，自动加载内置样（就绪即可拾取高亮）
      view.setViewMode(ViewMode.TWO_D);
      this.viewMode3d = false;
      view.setCamera(new Camera(39.91, 116.41, 25000.0));
      this.toggleVector();
      this.statusText = '03 矢量拾取（默认2D）：单击要素高亮';
    } else if (this.demoIndex === 3) {
      // 04 测量：默认 2D，单击加点
      view.setViewMode(ViewMode.TWO_D);
      this.viewMode3d = false;
      view.setCamera(new Camera(39.915, 116.40, 12000.0));
      this.initMeasure();
      this.statusText = '04 测量（默认2D）：单击先出点，续点自动连线';
    } else if (this.demoIndex === 4) {
      view.setViewMode(ViewMode.TWO_D);
      this.viewMode3d = false;
      this.demoMarker();
    } else {
      view.setCamera(new Camera(35.0, 105.0, 12000000.0));
    }
  }

  /**
   * 单击处理：demo 3 走测量加点；demo 2 走矢量拾取高亮；底图类（0/1）与其余仅反算经纬度。
   * 拾取（pickVector）与高亮通路两模式共用——3D 下 native 走屏幕空间投影拾取（见 pickVector3D）。
   */
  private onMapTap(x: number, y: number): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const pos: Position | undefined = map.screenToGeo(x, y);
    if (this.demoIndex === 3) {
      if (pos !== undefined) {
        this.measurePts.push(pos);
        this.refreshMeasure();
      }
      this.statusText = `测量点数 ${this.measurePts.length}`
        + (pos !== undefined ? ` | ${pos.longitude.toFixed(5)}, ${pos.latitude.toFixed(5)}` : '');
      return;
    }
    if (this.demoIndex === 2) {
      const hit: PickResult | undefined = map.pickVector(x, y);
      if (hit !== undefined) {
        this.statusText = `命中 层${hit.layerIndex} FID ${hit.fid}`
          + (pos !== undefined ? ` | ${pos.longitude.toFixed(5)}, ${pos.latitude.toFixed(5)}` : '');
        this.showHighlight(map, hit);
      } else if (pos !== undefined) {
        this.statusText = `经度 ${pos.longitude.toFixed(5)} 纬度 ${pos.latitude.toFixed(5)}`;
      }
      return;
    }
    // 底图 / 标记等：仅反算经纬度展示
    this.statusText = pos !== undefined
      ? `经度 ${pos.longitude.toFixed(5)} 纬度 ${pos.latitude.toFixed(5)}`
      : '未命中地面点';
  }

  /** 拾取高亮：featureGeometry → 动态叠加层（面/线/点分流），noPick 退出拾取竞争 */
  private showHighlight(map: NativeMapView, hit: PickResult): void {
    const geo: FeatureGeometry | undefined = map.featureGeometry(hit.layerIndex, hit.fid);
    if (geo === undefined) {
      return;
    }
    // 高亮样式：亮黄填充/描边/点，宽于源层保证可见
    const hlStyle = new VectorStyle(
      0x66FFFF00, 0xFFFFD700, 3.0, 0xFFFFD700, 4.0, 0xFFFFD700, 8.0,
      '', 0xFFFFD700, 1.0, false, 0xFF000000);
    if (!this.highlightReady) {
      this.highlightIndex = map.addOverlayLayer(hlStyle);
      if (this.highlightIndex < 0) {
        return;
      }
      map.setOverlayNoPick(this.highlightIndex, true);
      this.highlightReady = true;
    }
    if (geo.type === 0) {
      map.updateOverlayPoints(this.highlightIndex, geo.lonlat);
    } else if (geo.type === 1) {
      map.updateOverlayLines(this.highlightIndex, geo.lonlat, geo.ringCounts);
    } else {
      map.updateOverlayPolygons(this.highlightIndex, geo.lonlat,
        geo.ringCounts, geo.ringsPerFeature);
    }
  }

  /** 矢量层开关：加层用默认可见样式；已加则仅切可见性（对齐 Android 图层显隐门控） */
  private toggleVector(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    if (this.vectorIndex >= 0) {
      this.vectorOn = !this.vectorOn;
      map.setVectorLayerVisible(this.vectorIndex, this.vectorOn);
      return;
    }
    const path = this.findSample(['.shp', '.kml', '.kmz', '.gpkg', '.dxf', '.geojson', '.json']);
    if (path === undefined) {
      this.toast('filesDir 无矢量样例：内置 sample.geojson 解压失败，或 hdc file send 一个 .shp/.kml');
      return;
    }
    // 默认样式即为可见蓝填充 + 白描边 + 橙红点线（VectorStyle 默认值），近景叠加高德影像清晰可辨
    this.vectorIndex = map.addVectorLayer(path, new VectorStyle());
    this.vectorOn = this.vectorIndex >= 0;
    this.statusText = this.vectorOn ? `矢量层已加载：${path}` : '矢量层加载失败（格式不支持？）';
  }

  /** 栅格层开关：tif/img 即时切片（首屏较慢属预期，缓存后复用） */
  private toggleRaster(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    if (this.rasterIndex >= 0) {
      this.rasterOn = !this.rasterOn;
      map.setLayerVisible(this.rasterIndex, this.rasterOn);
      return;
    }
    const path = this.findSample(['.tif', '.tiff', '.img', '.png', '.jpg']);
    if (path === undefined) {
      this.toast('filesDir 无栅格样例：hdc file send 一个 .tif 到 ' + this.context.filesDir);
      return;
    }
    this.rasterIndex = map.addRasterLayer(`${this.context.filesDir}/tiles/raster`, path);
    this.rasterOn = this.rasterIndex >= 0;
    this.statusText = this.rasterOn ? `栅格层已加入（index ${this.rasterIndex}）` : '栅格打开失败';
  }

  /**
   * 测量层初始化：建「几何层 + 顶点标记层」两个 noPick 叠加层（对齐 Android OverlayActivity /
   * MobileMap MeasureCaptureController）——native 叠加层整层覆盖式更新、单层单帧仅一种几何，
   * 故顶点圆点独立成层，每次点击即时出点给出反馈，折线另在几何层按 ≥2 点阈值连线。
   */
  private initMeasure(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    // 几何层：橙红折线
    const lineStyle = new VectorStyle();
    lineStyle.lineColor = 0xFFFF9800;
    lineStyle.lineWidth = 4.0;
    lineStyle.fillColor = 0x4DFF9800;
    this.measureOverlay = map.addOverlayLayer(lineStyle);
    if (this.measureOverlay >= 0) {
      map.setOverlayNoPick(this.measureOverlay, true);
    }
    // 顶点标记层：仅画圆点，承载每次点击的即时反馈
    const pointStyle = new VectorStyle();
    pointStyle.pointColor = 0xFFFF9800;
    pointStyle.pointRadiusDp = 7.0;
    this.measureMarker = map.addOverlayLayer(pointStyle);
    if (this.measureMarker >= 0) {
      map.setOverlayNoPick(this.measureMarker, true);
    }
  }

  /**
   * 覆盖式重绘测量两类几何：标记层全量刷新顶点圆点（≥1 点即出点，即时反馈）；
   * 几何层 ≥2 点画折线、否则清空（撤销/清空回退到阈值以下不留残线）。
   */
  private refreshMeasure(): void {
    const map = this.mapView;
    if (map === undefined || this.measureOverlay < 0 || this.measureMarker < 0) {
      return;
    }
    const n = this.measurePts.length;
    const lonlat: number[] = [];
    for (let i = 0; i < n; i++) {
      lonlat.push(this.measurePts[i].longitude);
      lonlat.push(this.measurePts[i].latitude);
    }
    // ① 顶点圆点即时反馈
    if (n === 0) {
      map.updateOverlayPoints(this.measureMarker, []);
    } else {
      map.updateOverlayPoints(this.measureMarker, lonlat);
    }
    // ② 几何层折线
    if (n >= 2) {
      map.updateOverlayLines(this.measureOverlay, lonlat, [n]);
    } else {
      map.updateOverlayLines(this.measureOverlay, [], []);
    }
    this.measureCount = n;
  }

  private measureUndo(): void {
    if (this.measurePts.length > 0) {
      this.measurePts.pop();
      this.refreshMeasure();
      this.statusText = `测量点数 ${this.measurePts.length}（撤销末点）`;
    }
  }

  private measureClear(): void {
    this.measurePts = [];
    this.refreshMeasure();
    this.statusText = '测量已清空';
  }

  /** demo 4：定位标记蓝点 + 45° 方位角箭头（近景 30km，对齐 Android LocationMarkerActivity 蓝点口径） */
  private demoMarker(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    map.setCamera(new Camera(39.900, 116.400, 30000.0));
    map.setLocationMarker(new Position(39.900, 116.400), true, 45.0);
    this.statusText = '定位标记演示：蓝点 + heading 45° 箭头（可单指平移）';
  }

  /** 2D/3D 切换（demo 1）：共用相机状态，切换保持视角连续 */
  private toggleViewMode(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    this.viewMode3d = !this.viewMode3d;
    map.setViewMode(this.viewMode3d ? ViewMode.THREE_D : ViewMode.TWO_D);
  }

  /** 相机读-改-写通用助手（放大/缩小/旋转/仰角共用；句柄已释放或 surface 未就绪则忽略） */
  private adjustCamera(alt: number, heading: number, tilt: number): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const c: Camera | undefined = map.getCamera();
    if (c === undefined) {
      return;
    }
    map.setCamera(new Camera(c.latitude, c.longitude, alt, heading, tilt, c.roll, c.fieldOfView, c.altitudeMode));
  }

  private zoomIn(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const c = map.getCamera();
    if (c !== undefined) {
      this.adjustCamera(Math.max(200.0, c.altitude * 0.5), c.heading, c.tilt);
    }
  }

  private zoomOut(): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const c = map.getCamera();
    if (c !== undefined) {
      this.adjustCamera(Math.min(40000000.0, c.altitude * 2.0), c.heading, c.tilt);
    }
  }

  /** 水平旋转（方位角 ±度数，仅 3D 显效）；命名 rotateBy 以避开内置属性 rotate */
  private rotateBy(deg: number): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const c = map.getCamera();
    if (c !== undefined) {
      this.adjustCamera(c.altitude, c.heading + deg, c.tilt);
    }
  }

  /** 俯仰（tilt ±度数，仅 3D 显效；钳 [0,80] 与 native 口径一致） */
  private tiltBy(deg: number): void {
    const map = this.mapView;
    if (map === undefined) {
      return;
    }
    const c = map.getCamera();
    if (c !== undefined) {
      const t = Math.max(0.0, Math.min(80.0, c.tilt + deg));
      this.adjustCamera(c.altitude, c.heading, t);
    }
  }

  build() {
    Column() {
      Stack({ alignContent: Alignment.TopStart }) {
        NativeMapView({
          density: 1.0,
          onReady: (view: NativeMapView): void => {
            this.initMap(view);
          },
        })
          .width('100%')
          .height('100%')
        // 状态栏
        Text(this.statusText)
          .fontSize(13)
          .fontColor(Color.White)
          .backgroundColor('#66000000')
          .padding({ left: 10, right: 10, top: 4, bottom: 4 })
          .borderRadius(6)
          .margin(8)
      }
      .width('100%')
      .layoutWeight(1)

      // 控制条：按演示序号呈现对应动作（横向可滚动，按钮多不挤爆）
      Scroll() {
        Row({ space: 8 }) {
          Button('清单')
            .onClick(() => {
              router.back();
            })
          if (this.demoIndex === 0) {
            Button('放大').onClick(() => this.zoomIn())
            Button('缩小').onClick(() => this.zoomOut())
            Button('旋转 +').onClick(() => this.rotateBy(30))
            Button('旋转 -').onClick(() => this.rotateBy(-30))
            Button('仰角 +').onClick(() => this.tiltBy(15))
            Button('仰角 -').onClick(() => this.tiltBy(-15))
          } else if (this.demoIndex === 1) {
            Button(this.viewMode3d ? '切到2D' : '切到3D').onClick(() => this.toggleViewMode())
            Button('放大').onClick(() => this.zoomIn())
            Button('缩小').onClick(() => this.zoomOut())
          } else if (this.demoIndex === 2) {
            Button('矢量层').onClick(() => this.toggleVector())
            Button('栅格层').onClick(() => this.toggleRaster())
            Button('放大').onClick(() => this.zoomIn())
            Button('缩小').onClick(() => this.zoomOut())
          } else if (this.demoIndex === 3) {
            Button(`撤销(${this.measureCount})`).onClick(() => this.measureUndo())
            Button('清空').onClick(() => this.measureClear())
            Button('放大').onClick(() => this.zoomIn())
            Button('缩小').onClick(() => this.zoomOut())
          } else if (this.demoIndex === 4) {
            Button('定位标记').onClick(() => this.demoMarker())
            Button('放大').onClick(() => this.zoomIn())
            Button('缩小').onClick(() => this.zoomOut())
          }
        }
        .padding({ left: 10, right: 10, top: 8, bottom: 8 })
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .align(Alignment.Start)
      .width('100%')
      .backgroundColor('#B3000000')
    }
    .width('100%')
    .height('100%')
  }
}
