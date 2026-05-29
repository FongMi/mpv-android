#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

build=_build$ndk_suffix
: "${android_api:=24}"
: "${prefix_name:=${ndk_suffix#_}}"

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

case "$prefix_name" in
	armv7l)
	android_abi=armeabi-v7a
	;;
	arm64)
	android_abi=arm64-v8a
	;;
	x86)
	android_abi=x86
	;;
	x86_64)
	android_abi=x86_64
	;;
	*)
		echo "Invalid architecture: $prefix_name" >&2
		exit 1
	;;
esac

cmake -S . -B $build -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$DIR/sdk/android-ndk-${v_ndk}/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI="$android_abi" \
	-DANDROID_PLATFORM=android-$android_api \
	-DANDROID_STL=c++_static \
	-DCMAKE_INSTALL_PREFIX=/usr/local \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGLSLANG_ENABLE_INSTALL=ON \
	-DGLSLANG_TESTS=OFF \
	-DENABLE_GLSLANG_BINARIES=OFF \
	-DENABLE_HLSL=OFF \
	-DENABLE_OPT=OFF \
	-DENABLE_PCH=OFF

cmake --build $build --parallel $cores
DESTDIR="$prefix_dir" cmake --install $build
