#ifndef GLOBECORE_NET_HTTP_CLIENT_H
#define GLOBECORE_NET_HTTP_CLIENT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace globecore {

/**
 * libcurl 封装，对应 参考实现的网络取瓦片职责（Phase C 起用于联网下载瓦片）。
 *
 * libcurl.a 已静态打包 OpenSSL + zlib，故 HTTPS 与 gzip 均可用，无需额外链接 ssl/crypto/z。
 * 采用「同步 GET 到内存」的最简模型；Phase C 会在其上做多线程池 + 写缓存 + 异步纹理上传。
 *
 * 注意：HTTPS 服务端证书校验需要 CA 证书包。天地图为 HTTPS，落地时需通过 [setCaBundle]
 * 指定 cacert.pem（随包内置）或按安全策略配置；未配置 CA 时校验会失败。
 */
class HttpClient {
public:
    /// 进程内一次性全局初始化（curl_global_init，线程安全、幂等）。成功返回 true。
    static bool globalInit();

    /// 全局清理（curl_global_cleanup）。进程退出前调用（可选）。
    static void globalCleanup();

    /// libcurl 版本串（自检用），失败返回空串
    static std::string version();

    /**
     * 同步 GET，把响应体写入 outBytes。仅 HTTP 2xx 视为成功。
     * @param url       完整 URL（占位符须已由调用方替换）
     * @param outBytes  输出：响应字节
     * @param timeoutMs 整体超时（毫秒）
     * @param abortFlag 可选中断标志：传输中一旦置 true 则立即中断返回 false（用于关机时快速 join 工作线程）
     * @return 成功返回 true；网络/超时/中断/非 2xx 返回 false（outBytes 被清空）
     */
    static bool get(const std::string &url, std::vector<uint8_t> &outBytes, long timeoutMs = 15000,
                    const std::atomic<bool> *abortFlag = nullptr);

    /// 设置 CA 证书包路径（cacert.pem），用于 HTTPS 证书校验；空串表示用默认行为
    static void setCaBundle(const std::string &caInfoPath);

private:
    static std::string caBundlePath_;
};

} // namespace globecore

#endif // GLOBECORE_NET_HTTP_CLIENT_H
