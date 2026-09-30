# CMake（Linux 版）自动探测与下载。优先级：本地 $MAIN_DIR/cmake/... → 系统 cmake → 都没有则下载。
CMAKE_VERSION=3.22.3
CMAKE_DIR_NAME=cmake-$CMAKE_VERSION-linux-x86_64
CMAKE_INSTALL_DIR=${CMAKE_INSTALL_DIR:-$MAIN_DIR/cmake}
# Kitware 官方分发站（非 GitHub），规避 GitHub 被本地 hosts 拦截
CMAKE_URL=https://www.cmake.org/files/v3.22/$CMAKE_DIR_NAME.tar.gz
CMAKE_LOCAL_BIN="$CMAKE_INSTALL_DIR/$CMAKE_DIR_NAME/bin/cmake"

if [ -x "$CMAKE_LOCAL_BIN" ]; then
  export PATH="$CMAKE_INSTALL_DIR/$CMAKE_DIR_NAME/bin:$PATH"
  echo "使用本地 cmake: $CMAKE_LOCAL_BIN"
elif command -v cmake >/dev/null 2>&1; then
  echo "使用系统 cmake: $(command -v cmake)（$(cmake --version | head -n1)）"
else
  mkdir -p "$CMAKE_INSTALL_DIR"
  TARBALL="$CMAKE_INSTALL_DIR/$CMAKE_DIR_NAME.tar.gz"
  echo "未找到 cmake，下载 Linux 版：$CMAKE_URL"
  if [ -f "$TARBALL" ]; then
    echo "已存在安装包，跳过下载：$TARBALL"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "$TARBALL" "$CMAKE_URL"
  elif command -v curl >/dev/null 2>&1; then
    curl -fL -o "$TARBALL" "$CMAKE_URL"
  else
    echo "错误：需要 wget 或 curl 以下载 cmake"; exit 1
  fi
  echo "解压 cmake 到 $CMAKE_INSTALL_DIR ..."
  tar xzf "$TARBALL" -C "$CMAKE_INSTALL_DIR"
  if [ -x "$CMAKE_LOCAL_BIN" ]; then
    export PATH="$CMAKE_INSTALL_DIR/$CMAKE_DIR_NAME/bin:$PATH"
    echo "cmake 就绪: $CMAKE_LOCAL_BIN"
  else
    echo "错误：解压后未找到 cmake：$CMAKE_LOCAL_BIN"; exit 1
  fi
fi