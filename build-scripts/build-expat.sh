
export EXPAT_VER=2.4.8

if [ ! -f expat-$EXPAT_VER.tar.gz ]; then
  wget https://github.com/libexpat/libexpat/releases/download/R_${EXPAT_VER//./_}/expat-$EXPAT_VER.tar.gz
fi

#删除目录
rm -rf expat-$EXPAT_VER
#解压
tar xzf expat-$EXPAT_VER.tar.gz

cd expat-$EXPAT_VER

rm -rf build-$TARGET-$ABI

mkdir build-$TARGET-$ABI && cd build-$TARGET-$ABI

# 目标工具链参数由 build-all.sh 按 TARGET 注入（android: NDK toolchain；ohos: ohos.toolchain）
cmake .. \
  $WW_CMAKE_TARGET_ARGS \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=$PREFIX
make -j$(nproc)
make install
cd $MAIN_DIR
echo " "
echo "expat done ---------------------------------------"
echo " "