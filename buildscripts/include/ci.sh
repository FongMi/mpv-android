#!/bin/bash -e

# go to buildscripts root folder
cd "$( dirname "${BASH_SOURCE[0]}" )/.."

. ./include/depinfo.sh

msg() {
	printf '==> %s\n' "$1"
}

fetch_prefix() {
	if [[ "$CACHE_MODE" == folder ]]; then
		local text=
		if [ -f "$CACHE_FOLDER/id.txt" ]; then
			text=$(cat "$CACHE_FOLDER/id.txt")
		else
			echo "Cache seems to be empty"
		fi
		printf 'Expecting "%s",\nfound     "%s".\n' "$ci_cache_id" "$text"
		if [[ "$text" == "$ci_cache_id" ]]; then
			tar -xzf "$CACHE_FOLDER/data.tgz" -C prefix && return 0
		fi
	fi
	return 1
}

build_prefix() {
	msg "Building the prefix ($ci_cache_id)..."

	msg "Fetching deps"
	IN_CI=1 ./include/download-deps.sh

	msg "Compiling"
	for arch in $MPV_ANDROID_ARCHES; do
		msg "Compiling dependencies for $arch"
		./buildall.sh --arch "$arch" --only-deps mpv
	done

	if [[ "$CACHE_MODE" == folder && -w "$CACHE_FOLDER" ]]; then
		msg "Compressing the prefix"
		tar -cvzf "$CACHE_FOLDER/data.tgz" -C prefix .
		echo "$ci_cache_id" >"$CACHE_FOLDER/id.txt"
	fi
}

export WGET="wget --progress=bar:force"
: "${MPV_GIT_URL:=https://github.com/FongMi/mpv}"
: "${MPV_ANDROID_ARCHES:=armv7l arm64}"
ci_cache_id="${ci_tarball}-${MPV_ANDROID_ARCHES// /_}"

if [ "$1" = "export" ]; then
	# export variable with unique cache identifier
	echo "CACHE_IDENTIFIER=$ci_cache_id"
	exit 0
elif [ "$1" = "install" ]; then
	# install deps
	if [[ -n "$ANDROID_HOME" && -d "$ANDROID_HOME" ]]; then
		msg "Linking existing SDK"
		mkdir -p sdk
		ln -sv "$ANDROID_HOME" sdk/android-sdk-linux
	fi

	msg "Fetching SDK + NDK"
	IN_CI=1 ./include/download-sdk.sh

	msg "Fetching mpv"
	mkdir -p deps/mpv
	if [ -n "$MPV_GIT_REF" ]; then
		git clone --depth 1 --branch "$MPV_GIT_REF" "$MPV_GIT_URL" deps/mpv
	else
		git clone --depth 1 "$MPV_GIT_URL" deps/mpv
	fi

	msg "Trying to fetch existing prefix"
	mkdir -p prefix
	fetch_prefix || build_prefix
	exit 0
elif [ "$1" = "build" ]; then
	# run build
	:
else
	exit 1
fi

for arch in $MPV_ANDROID_ARCHES; do
	msg "Building mpv ($arch)"
	./buildall.sh --arch "$arch" -n mpv || {
		# show logfile if configure failed
		build_dir="deps/mpv/_build_$arch"
		[ ! -f "$build_dir/config.h" ] && \
			[ -f "$build_dir/meson-logs/meson-log.txt" ] && \
			cat "$build_dir/meson-logs/meson-log.txt"
		exit 1
	}
done

msg "Building mpv-android"
./buildall.sh -n

exit 0
