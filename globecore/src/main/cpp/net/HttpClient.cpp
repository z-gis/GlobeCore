#include "net/HttpClient.h"

#include "util/Log.h"

#include <curl/curl.h>

#include <mutex>

namespace globecore {

std::string HttpClient::caBundlePath_;

namespace {

std::once_flag g_initOnce;
bool g_initOk = false;

// libcurl 写回调：把响应字节追加到 std::vector<uint8_t>
size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::vector<uint8_t> *>(userdata);
    const size_t bytes = size * nmemb;
    out->insert(out->end(), ptr, ptr + bytes);
    return bytes;
}

// 传输进度回调：返回非 0 则中断传输。用于关机时让正在进行的 GET 尽快返回，避免 join 阻塞。
int abortProgress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *flag = static_cast<const std::atomic<bool> *>(clientp);
    return (flag != nullptr && flag->load()) ? 1 : 0;
}

// Android 系统 CA 证书目录（OpenSSL 哈希命名的单个 PEM），供 CURLOPT_CAPATH 直接校验公有 CA
constexpr const char *const kAndroidSystemCaPath = "/system/etc/security/cacerts";

// 线程常驻 easy handle：libcurl 的连接缓存挂在 handle 上，请求结束不销毁 handle 即可跨瓦片
// 复用同 host 的 TCP+TLS 连接（HTTP/1.1 keep-alive）。此前每块瓦片新建/销毁 handle，每块都要
// 付 2~3 个 RTT 的握手代价，瓦片风暴（缩放/旋转到新区域数百块）下总时延成倍放大。
// 线程退出时随 thread_local 析构自动 cleanup。
struct ThreadCurl {
    CURL *h = nullptr;
    ThreadCurl() { h = curl_easy_init(); }
    ~ThreadCurl() {
        if (h != nullptr) curl_easy_cleanup(h);
    }
};

inline CURL *threadHandle() {
    static thread_local ThreadCurl tc;
    return tc.h;
}

} // namespace

bool HttpClient::globalInit() {
    std::call_once(g_initOnce, []() {
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_initOk = (rc == CURLE_OK);
        if (g_initOk) {
            LOGI("curl_global_init ok, version=%s", curl_version());
        } else {
            LOGE("curl_global_init failed rc=%d", static_cast<int>(rc));
        }
    });
    return g_initOk;
}

void HttpClient::globalCleanup() {
    curl_global_cleanup();
}

std::string HttpClient::version() {
    const char *v = curl_version();
    return v != nullptr ? std::string(v) : std::string();
}

void HttpClient::setCaBundle(const std::string &caInfoPath) {
    caBundlePath_ = caInfoPath;
}

bool HttpClient::get(const std::string &url, std::vector<uint8_t> &outBytes, long timeoutMs,
                     const std::atomic<bool> *abortFlag) {
    if (!globalInit()) return false;

    CURL *curl = threadHandle();
    if (curl == nullptr) {
        LOGE("curl_easy_init failed");
        return false;
    }
    curl_easy_reset(curl); // 选项清回默认（含校验/回调），但保留 handle 内连接缓存——keep-alive 复用不丢

    outBytes.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBytes);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);       // 多线程环境下必须，避免 alarm 信号
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 跟随 3xx
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "globecore/0.2.8.1");

    // 可中断传输：传入 abortFlag 时启用进度回调，标志置位则中断
    if (abortFlag != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abortFlag);
    }

    // HTTPS 证书校验：配置了 CA 包则用之；否则用 Android 系统证书目录（CAPATH）校验公有 CA
    if (!caBundlePath_.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath_.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CAPATH, kAndroidSystemCaPath);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    CURLcode rc = curl_easy_perform(curl);

    // 证书校验失败（部分设备系统证书目录不可用/不完整）时降级为不校验重试一次，保证瓦片可取
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
        LOGW("curl TLS 校验失败(%s)，降级不校验重试 url=%s", curl_easy_strerror(rc), url.c_str());
        outBytes.clear();
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        rc = curl_easy_perform(curl);
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    // 刻意不 cleanup：handle 线程常驻，连接缓存跨请求复用（线程退出由 ThreadCurl 析构回收）

    if (rc != CURLE_OK) {
        // 主动中断（abort）属正常关机路径，降为 debug 级避免刷屏
        if (abortFlag != nullptr && abortFlag->load()) {
            outBytes.clear();
            return false;
        }
        LOGW("curl GET 失败 url=%s err=%s", url.c_str(), curl_easy_strerror(rc));
        outBytes.clear();
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        LOGW("curl GET 非 2xx（HTTP %ld）url=%s", httpCode, url.c_str());
        outBytes.clear();
        return false;
    }
    return true;
}

} // namespace globecore
