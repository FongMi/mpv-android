#!/bin/bash -e

# go to buildscripts root folder
cd "$( dirname "${BASH_SOURCE[0]}" )/.."

. ./include/depinfo.sh

msg() {
	printf '==> %s\n' "$1"
}

ffmpeg_repository=${FFMPEG_GIT_URL:-https://github.com/FongMi/FFmpeg.git}
dav1d_repository=${DAV1D_GIT_URL:-https://github.com/videolan/dav1d}
libass_repository=${LIBASS_GIT_URL:-https://github.com/libass/libass}
libplacebo_repository=${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}

resolve_ci_commit() {
	local configured_commit=$1
	local repository=$2
	local branch=$3
	local name=$4
	local result
	local commit=$configured_commit

	if [[ -z "$commit" ]]; then
		result=$(git ls-remote --exit-code "$repository" "refs/heads/$branch") || {
			echo "Failed to resolve $name branch $branch from $repository." >&2
			return 1
		}
		if [[ $(printf '%s\n' "$result" | grep -c .) != 1 ]]; then
			echo "Expected exactly one ref for $name branch $branch." >&2
			return 1
		fi
		commit=${result%%[[:space:]]*}
	fi
	if [[ ! "$commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
		echo "Invalid $name commit: $commit" >&2
		return 1
	fi
	printf '%s\n' "$commit" | tr '[:upper:]' '[:lower:]'
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
			rm -rf prefix
			mkdir -p prefix
			if tar -xzf "$CACHE_FOLDER/data.tgz" -C prefix; then
				return 0
			fi
			echo "Failed to extract cached prefix; rebuilding."
			rm -rf prefix
			mkdir -p prefix
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

if [[ "$1" == export || "$1" == install ]]; then
	FFMPEG_GIT_COMMIT=$(resolve_ci_commit \
		"${FFMPEG_GIT_COMMIT:-}" "$ffmpeg_repository" "$v_ci_ffmpeg" FFmpeg)
	DAV1D_GIT_COMMIT=$(resolve_ci_commit \
		"${DAV1D_GIT_COMMIT:-}" "$dav1d_repository" "$v_ci_dav1d" dav1d)
	LIBASS_GIT_COMMIT=$(resolve_ci_commit \
		"${LIBASS_GIT_COMMIT:-}" "$libass_repository" "$v_ci_libass" libass)
	LIBPLACEBO_GIT_COMMIT=$(resolve_ci_commit \
		"${LIBPLACEBO_GIT_COMMIT:-}" "$libplacebo_repository" \
		"$v_ci_libplacebo" libplacebo)
	export FFMPEG_GIT_COMMIT DAV1D_GIT_COMMIT LIBASS_GIT_COMMIT
	export LIBPLACEBO_GIT_COMMIT
	native_source_id=$(printf '%s\n' \
		"ffmpeg=$FFMPEG_GIT_COMMIT" \
		"dav1d=$DAV1D_GIT_COMMIT" \
		"libass=$LIBASS_GIT_COMMIT" \
		"libplacebo=$LIBPLACEBO_GIT_COMMIT" | sha256sum)
	native_source_id=${native_source_id%%[[:space:]]*}
	if [[ ! "$native_source_id" =~ ^[0-9a-f]{64}$ ]]; then
		echo "Failed to compute native source cache identifier." >&2
		exit 1
	fi
	ci_cache_id="${ci_tarball}-sources-${native_source_id}-${MPV_ANDROID_ARCHES// /_}"
fi

if [ "$1" = "export" ]; then
	# Export the exact native source revisions used by the cache and build steps.
	echo "FFMPEG_GIT_COMMIT=$FFMPEG_GIT_COMMIT"
	echo "DAV1D_GIT_COMMIT=$DAV1D_GIT_COMMIT"
	echo "LIBASS_GIT_COMMIT=$LIBASS_GIT_COMMIT"
	echo "LIBPLACEBO_GIT_COMMIT=$LIBPLACEBO_GIT_COMMIT"
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
