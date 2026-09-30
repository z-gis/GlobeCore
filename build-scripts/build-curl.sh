#!/bin/bash
# build-curl.sh —— 供 build-all.sh 在 build_one_abi 内 source 的 per-ABI 子脚本。
# 前置：build-openssl.sh 已把 libssl.a/libcrypto.a + include/openssl stage 到 $PREFIX；
#      build-minizip.sh 已把 libz.a + zlib.h 装到 $PREFIX。
# 本脚本用 CMake 交叉编译静态 libcurl（启用 OpenSSL + zlib，关掉其余可选依赖），
# 再用 NDK clang 部分链接（-r --whole-archive）把 libssl.a + libcrypto.a 合并进 libcurl.a ——
# 产出一个自含 OpenSSL 的 libcurl.a（与历史 jniLibs 里的 libcurl.a 同构：链接时只需 -lcurl，zlib 由 zstatic 提供）。
#
# 依赖环境变量：ABI PREFIX NDK_ROOT TOOLCHAIN API CC MAIN_DIR BASE_PATH PKG_CACHE TARGET
#               WW_CMAKE_TARGET_ARGS（build-all.sh 按 TARGET 注入：android/ohos 工具链参数）

CURL_VER=${CURL_VER:-8.4.0}
CURL_DOTTED="$CURL_VER"                              # 8.4.0
CURL_UNDER="curl-$(echo "$CURL_VER" | tr '.' '_')"   # curl-8_4_0
CURL_TARBALL="curl-${CURL_DOTTED}.tar.gz"
CURL_URLS=(
  "https://github.com/curl/curl/archive/refs/tags/${CURL_UNDER}.tar.gz"
  "https://curl.se/download/$CURL_TARBALL"
)

# —— 取源码：优先复用 $MAIN_DIR，其次 $PKG_CACHE，最后 wget ——
cd "$MAIN_DIR"
if [ ! -f "$CURL_TARBALL" ]; then
  fetched=0
  for cache in ${PKG_CACHE:-}; do
    if [ -f "$cache/$CURL_TARBALL" ]; then
      cp -f "$cache/$CURL_TARBALL" "$CURL_TARBALL" && { echo "从缓存复用 $CURL_TARBALL"; fetched=1; break; }
    fi
  done
  if [ "$fetched" -eq 0 ]; then
    for u in "${CURL_URLS[@]}"; do
      echo "下载 curl 源码：$u"
      if timeout 300 wget -q -O "$CURL_TARBALL" "$u" && [ "$(stat -c %s "$CURL_TARBALL" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        break
      fi
      rm -f "$CURL_TARBALL"
    done
  fi
fi
[ -f "$CURL_TARBALL" ] || { echo "[curl] 源码包缺失且下载失败：$CURL_TARBALL"; exit 1; }

# —— 解压并规整目录名（github archive 顶层是 curl-curl-8_4_0/）——
SRC_DIR="curl-${CURL_DOTTED}"
if [ ! -d "$SRC_DIR" ]; then
  top=$(tar tzf "$CURL_TARBALL" | head -1 | cut -d/ -f1)
  tar xzf "$CURL_TARBALL"
  [ "$top" != "$SRC_DIR" ] && mv "$top" "$SRC_DIR"
fi
# 目标工具链参数由 build-all.sh 按 TARGET 注入（ohos 时 NDK_ROOT 不存在，不得引用）
[ -n "${WW_CMAKE_TARGET_ARGS:-}" ] || { echo "[curl] WW_CMAKE_TARGET_ARGS 未注入，请经 build-all.sh 调用"; exit 1; }

# —— 源码绝对路径（供后续头文件复制与 cmake 源目录引用）——
SRC_ABS="$MAIN_DIR/$SRC_DIR"

# —— 按目标平台+ABI 隔离的构建目录（置于源码目录之外，避免污染源码树）——
BUILD_DIR="$SRC_ABS/build_${TARGET}_$ABI"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMDCMAKE="${CMAKE:-cmake}"
"$CMDCMAKE" "$SRC_ABS" \
  $WW_CMAKE_TARGET_ARGS \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_TESTING=OFF \
  -DENABLE_MANUAL=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" \
  -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" \
  -DCURL_ZLIB=ON \
  -DZLIB_INCLUDE_DIR="$PREFIX/include" \
  -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
  -DUSE_NGHTTP2=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DCURL_USE_LIBSSH=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_GSSAPI=OFF \
  -DUSE_LIBIDN2=OFF \
  -DCURL_USE_MBEDTLS=OFF \
  -DCURL_USE_GNUTLS=OFF \
  -DCURL_USE_SCHANNEL=OFF \
  -DCURL_USE_WOLFSSL=OFF \
  -DCURL_USE_BEARSSL=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

# 不指定 target（curl 静态目标名是 libcurl_static，直接构建默认 all 更稳妥）
"$CMDCMAKE" --build . --config Release -j"$(nproc)"

# —— 定位产物 libcurl.a（一般在 lib/ 下）——
LIBCURL_BUILT=$(find "$BUILD_DIR" -name libcurl.a -type f 2>/dev/null | head -1)
[ -n "$LIBCURL_BUILT" ] || { echo "[curl] 未生成 libcurl.a（构建树里找不到）"; exit 1; }
echo "[curl] 静态库产物：$LIBCURL_BUILT"

# —— 合并 OpenSSL 进 libcurl.a ——
# llvm-ar 不支持 GNU ar 的 MRI(ADDLIB)；改用 NDK clang 部分链接（-r --whole-archive）
# 把 libcurl + libssl + libcrypto 全部目标文件预链接成单个可重定位 .o，再打成 libcurl.a。
AR_BIN="$TOOLCHAIN/bin/llvm-ar"
mkdir -p "$PREFIX/lib"
rm -f "$PREFIX/lib/libcurl.a"
PARTIAL="$BUILD_DIR/curl_merged.o"
# -nostdlib：阻止 clang 驱动自动挂 sysroot 的 libc.so/libdl.so（部分链接不允许静态链动态对象）
"$CC" -r -nostdlib -o "$PARTIAL" \
  -Wl,--whole-archive "$LIBCURL_BUILT" "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" \
  -Wl,--no-whole-archive
"$AR_BIN" rcs "$PREFIX/lib/libcurl.a" "$PARTIAL"
rm -f "$PARTIAL"

# —— curl 公开头（供 CMakeLists 的 <curl/curl.h>）——
rm -rf "$PREFIX/include/curl"
cp -rf "$SRC_ABS/include/curl" "$PREFIX/include/"

echo "[curl] merged -> $PREFIX/lib/libcurl.a (curl + openssl)，大小 $(du -h "$PREFIX/lib/libcurl.a" | cut -f1)"
