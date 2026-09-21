#!/bin/bash -e

## Dependency versions
# Make sure to keep v_ndk and v_ndk_n in sync, both are listed on the NDK download page

v_sdk=11076708_latest
v_ndk=r30
v_ndk_n=30.0.16248370
v_sdk_platform=36
v_sdk_build_tools=36.0.0

v_lua=5.2.4
v_unibreak=8.0
v_harfbuzz=14.5.0
v_fribidi=1.0.17
v_freetype=2.14.3
v_mbedtls=3.6.7
v_libxml2=2.15.4
v_libaribcaption=1.1.1
v_fontconfig=2.18.3
v_curl=8.22.0
v_libiconv=1.19
v_uchardet=0.0.8
v_bzip2=1.0.8
v_xz=5.8.1
v_zstd=1.5.7
v_libarchive=3.8.7
v_rubberband=4.0.0


## Dependency tree

dep_libiconv=()
dep_uchardet=(libiconv)
dep_bzip2=()
dep_xz=()
dep_zstd=()
dep_mbedtls=()
dep_dav1d=()
dep_libxml2=()
dep_freetype2=()
dep_libaribcaption=(freetype2)
dep_uavs3d=()
dep_ffmpeg=(mbedtls dav1d libxml2 libaribcaption uavs3d)
dep_fontconfig=(libxml2 freetype2)
dep_fribidi=()
dep_harfbuzz=()
dep_unibreak=()
dep_libass=(freetype2 fontconfig fribidi harfbuzz unibreak)
dep_lua=()
dep_shaderc=()
dep_libplacebo=(shaderc)
dep_curl=(mbedtls)
dep_libarchive=(libiconv bzip2 xz zstd)
dep_rubberband=()
dep_mpv=(ffmpeg libass lua libplacebo curl libiconv uchardet libarchive rubberband)
dep_mpv_android=(mpv)


## for CI workflow

# CI resolves these movable branches to immutable commits before selecting a cache.
v_ci_ffmpeg=release-9.0-fongmi
v_ci_uavs3d=e72f7c34be2f4de725c1ae498e8c4f3559e99b5b
v_ci_dav1d=master
v_ci_libass=master
v_ci_libplacebo=fongmi
# bump when the prefix build recipe changes without a dependency version change
v_ci_prefix=39

# filename used to uniquely identify a build prefix
ci_tarball="prefix-ndk-${v_ndk}-opengl-vulkan-shaderc-lua-${v_lua}-unibreak-${v_unibreak}-harfbuzz-${v_harfbuzz}-fribidi-${v_fribidi}-freetype-${v_freetype}-libxml2-${v_libxml2}-libaribcaption-${v_libaribcaption}-fontconfig-${v_fontconfig}-mbedtls-${v_mbedtls}-curl-${v_curl}-libiconv-${v_libiconv}-uchardet-${v_uchardet}-bzip2-${v_bzip2}-xz-${v_xz}-zstd-${v_zstd}-libarchive-${v_libarchive}-rubberband-${v_rubberband}-ffmpeg-${v_ci_ffmpeg}-prefix-${v_ci_prefix}.tgz"
