import { router } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';

/**
 * Index：演示功能清单页（对应 Android 侧 MainActivity）。
 * 各项统一路由到综合演示页 MapDemoPage，经 params.demo 序号分流初始动作
 * （各演示项口径见 MapDemoPage 文件头说明）。
 */
class DemoParams {
  demo: number;

  constructor(demo: number) {
    this.demo = demo;
  }
}

@Entry
@Component
struct Index {
  private items: string[] = [
    '01 瓦片底图（3D + 缩放/旋转/仰角）',
    '02 2D / 3D 切换',
    '03 矢量图层与拾取（默认2D）',
    '04 测量叠加层（默认2D）',
    '05 定位标记（蓝点）'
  ];

  build() {
    Column() {
      Text('GlobeCore 鸿蒙演示')
        .fontSize(24)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 24, bottom: 16 })
      List({ space: 8 }) {
        ForEach(this.items, (item: string, idx: number) => {
          ListItem() {
            Text(item)
              .fontSize(18)
              .width('100%')
              .padding(16)
          }
          .onClick(() => {
            router.pushUrl({
              url: 'pages/MapDemoPage',
              params: new DemoParams(idx),
            }).catch((err: BusinessError) => {
              console.error(`push MapDemoPage failed: ${err.code} ${err.message}`);
            });
          })
        }, (item: string) => item)
      }
      .width('100%')
      .layoutWeight(1)
    }
    .width('100%')
    .height('100%')
  }
}
