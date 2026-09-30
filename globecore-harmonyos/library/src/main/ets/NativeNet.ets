// NativeNet.ets —— 网络配置门面（对应引擎 net/HttpClient 的对外可配置项）。
//
// 符号实现见 ohos/napi_globe_engine.cpp 的 NativeSetCaBundle（HttpClient::setCaBundle 直通）。
// OHOS 无 Android 系统 CA 目录（/system/etc/security/cacerts），HTTPS 证书校验须随包内置
// CA 证书包（rawfile/ca/cacert.pem）：宿主启动时调一次 initCaBundle(context) 即完成沙箱
// 解压与配置；配置后瓦片首握手即校验通过，消除逐块「校验失败→降级不校验重试」的双 TLS
// 握手时延（表现为缩放/旋转后瓦片迟迟不补齐）；未配置不致命，引擎仍走既有降级路径。
import { NativeLib } from './NativeLib';
import common from '@ohos.app.ability.common';

export class NativeNet {

  /**
   * 初始化 HTTPS CA 证书包：把本 HAR rawfile 的 ca/cacert.pem 拷到沙箱 filesDir 并交给
   * 引擎 HttpClient。宿主须在首次联网取瓦片前调用一次；已存在则跳过拷贝（幂等）。
   * 失败仅记日志不抛出（引擎仍可降级为不校验重试，功能不受影响）。
   */
  static initCaBundle(context: common.UIAbilityContext): void {
    NativeLib.initCaBundle(context);
  }
}
