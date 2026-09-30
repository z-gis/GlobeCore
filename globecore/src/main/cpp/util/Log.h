#ifndef GLOBECORE_UTIL_LOG_H
#define GLOBECORE_UTIL_LOG_H

// 统一日志封装，TAG 与模块名一致，便于 logcat（Android）/ hilog（鸿蒙）过滤。
// 平台差异仅收敛在本头：Android 走 __android_log_print；OHOS 走 vsnprintf 预格式化 +
// OH_LOG_Print("%{public}s")——hilog 默认脱敏未标 public 的参数，先自行拼装整条消息，
// 使全部调用点的 printf 格式串与 Android 行为完全一致，无需逐处改 public 标记。
#if defined(__OHOS__)

#include <hilog/log.h>
#include <stdarg.h>
#include <stdio.h>

#define WWJNI_LOG_TAG "globecore"
// hilog 应用域（0x0000-0xFFFF 用户域内自取一值，便于 hdc hilog -T 过滤）
#define WWJNI_LOG_DOMAIN 0x8811

__attribute__((format(printf, 2, 3)))
static inline void wwjni_log(unsigned int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OH_LOG_Print(LOG_APP, (LogLevel)level, WWJNI_LOG_DOMAIN, WWJNI_LOG_TAG, "%{public}s", buf);
}

#define LOGD(...) wwjni_log(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) wwjni_log(LOG_INFO, __VA_ARGS__)
#define LOGW(...) wwjni_log(LOG_WARN, __VA_ARGS__)
#define LOGE(...) wwjni_log(LOG_ERROR, __VA_ARGS__)

#else // Android

#include <android/log.h>

#define WWJNI_LOG_TAG "globecore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, WWJNI_LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, WWJNI_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, WWJNI_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, WWJNI_LOG_TAG, __VA_ARGS__)

#endif // __OHOS__

#endif // GLOBECORE_UTIL_LOG_H
