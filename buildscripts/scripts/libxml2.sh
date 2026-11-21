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

unset CC CXX # meson wants these unset

# FFmpeg's TTML demuxer serializes cues as XML packets.
meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	-Dminimum=true -D{push,reader,sax1,iso8859x,pattern,output}=enabled

# The default target also builds the uninstalled testdso module.
meson compile -C "$build" -j "$cores" xml2 xmllint
DESTDIR="$prefix_dir" meson install -C "$build" --no-rebuild
