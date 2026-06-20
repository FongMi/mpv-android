#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

check_iconv_files () {
	for file in "$prefix_dir/include/iconv.h" "$prefix_dir/lib/libiconv.a" "$prefix_dir/lib/libcharset.a"; do
		if [ ! -f "$file" ]; then
			echo "Missing libiconv file: $file" >&2
			exit 1
		fi
	done
}

patch_mpv_iconv_dependency () {
	local iconv_dep

	# Meson's built-in iconv dependency does not consult iconv.pc, and find_library()
	# has no extra search dirs here. Provide the just-built static libiconv directly.
	iconv_dep="iconv = declare_dependency(compile_args: ['-I$prefix_dir/include'], link_args: ['$prefix_dir/lib/libiconv.a', '$prefix_dir/lib/libcharset.a'])"
	${SED:-sed} -i.bak \
		-e "/^iconv = dependency('iconv', required: get_option('iconv'))$/c\\$iconv_dep" \
		-e "/^iconv = declare_dependency(compile_args: \['-I.*\/include'\], link_args: \['.*\/libiconv\.a', '.*\/libcharset\.a'\])$/c\\$iconv_dep" \
		meson.build
	rm -f meson.build.bak
}
if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

unset CC CXX # meson wants these unset

check_iconv_files
patch_mpv_iconv_dependency

meson setup "$build" --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-Diconv=enabled -Duchardet=enabled \
	-D{lua,libcurl}=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dlibbluray=enabled \
	-Dvulkan=enabled \
	-Dmanpage-build=disabled

ninja -C "$build" -j"$cores"
if [ -f "$build/libmpv.a" ]; then
	echo >&2 "Meson fucked up, forcing rebuild."
	$0 clean
	exec $0 build
fi
DESTDIR="$prefix_dir" ninja -C "$build" install
