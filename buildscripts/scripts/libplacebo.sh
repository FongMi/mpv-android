#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

unset CC CXX
if [ ! -f "$prefix_dir/lib/libshaderc.a" ]; then
	echo "shaderc dependency is missing: $prefix_dir/lib/libshaderc.a" >&2
	exit 1
fi
export CFLAGS="-I$prefix_dir/include ${CFLAGS:-}"
export CXXFLAGS="-I$prefix_dir/include ${CXXFLAGS:-}"
export LDFLAGS="-L$prefix_dir/lib ${LDFLAGS:-}"
meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	-Dvulkan=enabled -Dshaderc=enabled -Dglslang=disabled \
	-Ddemos=false

ninja -C $build -j$cores
DESTDIR="$prefix_dir" ninja -C $build install

link_libs=()
for lib in shaderc; do
	[ -f "$prefix_dir/lib/lib${lib}.a" ] && link_libs+=("-l${lib}")
done
link_libs+=("-lc++")

# add missing libraries for static linking
# this isn't "-lstdc++" due to a meson bug: https://github.com/mesonbuild/meson/issues/11300
${SED:-sed} "/^Libs:/ s|$| ${link_libs[*]}|" "$prefix_dir/lib/pkgconfig/libplacebo.pc" -i
