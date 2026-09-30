import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { NativeSrs, NativeNet } from 'globecore-harmonyos';

const TAG: string = 'EntryAbility';
const DOMAIN: number = 0x8811;

/**
 * EntryAbility：globecore-harmonyos 演示入口能力（对应 Android 侧 MainActivity）。
 * 加载 pages/Index 功能清单，各项路由到综合演示页 MapDemoPage。
 */
export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    // 首次解压 HAR rawfile/proj 到沙箱并配置 PROJ 搜索路径（幂等；须在任何坐标转换/栅格重投影前）
    NativeSrs.initProjData(this.context);
    // 首次解压 HAR rawfile/ca/cacert.pem 到沙箱并配置 HTTPS 证书校验（幂等；须在任何瓦片联网前）
    NativeNet.initCaBundle(this.context);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed, code: %{public}d', err.code);
        return;
      }
      hilog.info(DOMAIN, TAG, 'loadContent succeeded');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, 'onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground');
  }
}
