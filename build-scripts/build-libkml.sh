
LIBKML_VERSION=1.3.0
if [ ! -f libkml-${LIBKML_VERSION}.tar.gz ]; then
  wget https://github.com/libkml/libkml/archive/refs/tags/${LIBKML_VERSION}.tar.gz -O libkml-${LIBKML_VERSION}.tar.gz
fi

rm -rf libkml-${LIBKML_VERSION}
tar xzf libkml-${LIBKML_VERSION}.tar.gz
cd libkml-${LIBKML_VERSION}
# libkml 自带 ExternalProject 会去 http://zlib.net/zlib-1.2.8.tar.gz 拉它捆绑的 zlib：
#   1) 该 URL 现已 404（旧版被 zlib.net 挪到 /fossils/ 子目录）；
#   2) 且 cmake 内置 file(DOWNLOAD) 对 zlib.net 常卡死（本环境实测停在 ~86%）。
# 故优先用本地已缓存的 zlib-1.2.8.tar.gz 以 file:// 引用（内容与 MD5 同官方 44d667c142d7cda120332623eab69f40，校验照过），
# 本地缺失才回落：先试 PKG_CACHE，再试 wget 预取 fossils，最后才退回 fossils http URL。
ZLIB_NAME=zlib-1.2.8.tar.gz
ZLIB_LOCAL="${MAIN_DIR}/${ZLIB_NAME}"
if [ ! -s "$ZLIB_LOCAL" ]; then
  for c in ${PKG_CACHE:-}; do
    [ -s "$c/$ZLIB_NAME" ] && ZLIB_LOCAL="$c/$ZLIB_NAME" && break
  done
fi
if [ -s "$ZLIB_LOCAL" ]; then
  sed -i "s#http://zlib.net/${ZLIB_NAME}#file://${ZLIB_LOCAL}#" cmake/External_zlib.cmake
elif wget -q "http://zlib.net/fossils/${ZLIB_NAME}" -O "$MAIN_DIR/$ZLIB_NAME"; then
  sed -i "s#http://zlib.net/${ZLIB_NAME}#file://${MAIN_DIR}/${ZLIB_NAME}#" cmake/External_zlib.cmake
else
  sed -i "s#http://zlib.net/${ZLIB_NAME}#http://zlib.net/fossils/${ZLIB_NAME}#" cmake/External_zlib.cmake
fi
rm -rf build-$TARGET-$ABI
mkdir build-$TARGET-$ABI && cd build-$TARGET-$ABI
# 目标工具链参数由 build-all.sh 按 TARGET 注入（android: NDK toolchain；ohos: ohos.toolchain）
cmake .. \
  $WW_CMAKE_TARGET_ARGS \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DEXPAT_INCLUDE_DIR=$PREFIX/include \
  -DEXPAT_LIBRARY=$PREFIX/lib/libexpat.a \
  -DMINIZIP_INCLUDE_DIR=$PREFIX/include \
  -DMINIZIP_LIBRARY=$PREFIX/lib/libminizip.a \
  -DURIPARSER_INCLUDE_DIR=$PREFIX/include \
  -DURIPARSER_LIBRARY=$PREFIX/lib/liburiparser.a \
  -DBOOST_ROOT=$PREFIX \
  -DBoost_INCLUDE_DIR=$PREFIX/include
make -j$(nproc)
make install
cd $MAIN_DIR
echo "libkml done"