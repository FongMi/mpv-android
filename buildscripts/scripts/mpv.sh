#!/bin/bash -e
set -o pipefail

. ../../include/path.sh

build=_build$ndk_suffix

motion_receipt="$prefix_dir/share/mpv-motion/provenance.properties"
if [ "$1" == "build" ]; then
	# A failed rebuild must not leave an older archive accepted as current.
	rm -f "$motion_receipt"
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

unset CC CXX # meson wants these unset

motion_core=video/out/gpu_next/motion.c
motion_header=video/out/gpu_next/motion.h
motion_commit=$(git rev-parse HEAD)
motion_source_sha=$(sha256sum "$motion_core" | cut -d ' ' -f 1)
motion_header_sha=$(sha256sum "$motion_header" | cut -d ' ' -f 1)
git diff --quiet HEAD -- "$motion_core" "$motion_header" LICENSE.LGPL || {
	echo "Motion SDK source differs from its commit." >&2
	exit 1
}

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-Diconv=disabled -D{lua,libcurl}=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dvulkan=enabled \
	-Dmanpage-build=disabled

ninja -C $build -j$cores
if [ -f $build/libmpv.a ]; then
	echo >&2 "Meson fucked up, forcing rebuild."
	$0 clean
	exec $0 build
fi
DESTDIR="$prefix_dir" ninja -C $build install

# Bind the installed archive to the source actually used by this build.
[ "$motion_commit" = "$(git rev-parse HEAD)" ] &&
[ "$motion_source_sha" = "$(sha256sum "$motion_core" | cut -d ' ' -f 1)" ] &&
[ "$motion_header_sha" = "$(sha256sum "$motion_header" | cut -d ' ' -f 1)" ] &&
git diff --quiet HEAD -- "$motion_core" "$motion_header" LICENSE.LGPL &&
cmp -s "$motion_header" "$prefix_dir/include/mpv-motion/motion.h" || {
	echo "Motion SDK source or installed header changed during build." >&2
	exit 1
}
motion_archive_sha=$(sha256sum "$prefix_dir/lib/libmpv-motion.a" | cut -d ' ' -f 1)
mkdir -p "$(dirname "$motion_receipt")"
cat >"$motion_receipt.tmp" <<EOF
schema.version=1
mpv-motion.arch=$prefix_name
mpv-motion.commit=$motion_commit
mpv-motion.source.sha256=$motion_source_sha
mpv-motion.header.sha256=$motion_header_sha
mpv-motion.archive.sha256=$motion_archive_sha
EOF
mv "$motion_receipt.tmp" "$motion_receipt"
