#!/usr/bin/env python3
"""Check dependency cache keys and restoration with local fixture builds."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def verify_install(root, bash, environment):
    cache_root = root.parent / "cache"
    base_cache = cache_root / "base"
    libplacebo_cache = cache_root / "libplacebo"
    base_cache.mkdir(parents=True)
    libplacebo_cache.mkdir()
    trace = root / "trace"
    environment.update(CACHE_MODE="folder", CACHE_FOLDER=base_cache.as_posix(),
                       LIBPLACEBO_CACHE_FOLDER=libplacebo_cache.as_posix(),
                       TRACE_FILE=trace.as_posix(), ANDROID_HOME="")
    (root / "deps/mpv").mkdir(parents=True)

    def stub(path, script):
        file = root / path
        file.write_bytes(("#!/bin/bash -e\n" + script).encode())
        file.chmod(0o755)

    stub("include/download-sdk.sh", "exit 0\n")
    stub("include/download-deps.sh", '''
echo "download $1" >> "$TRACE_FILE"
if [[ "$1" == "${FAIL_DOWNLOAD:-}" ]]; then
    exit 7
fi
''')
    stub("buildall.sh", '''
arch=$2
case "${@: -1}" in
    mpv-base)
        echo "base $arch" >> "$TRACE_FILE"
        mkdir -p "prefix/$arch/lib"
        printf '%s' "$FFMPEG_GIT_COMMIT" > "prefix/$arch/lib/libffmpeg.so"
        echo shaderc > "prefix/$arch/lib/libshaderc.a"
        ;;
    libplacebo)
        echo "libplacebo $arch" >> "$TRACE_FILE"
        test -s "prefix/$arch/lib/libffmpeg.so"
        dest="$LIBPLACEBO_DESTDIR/usr/local"
        mkdir -p "$dest/lib/pkgconfig" "$dest/include/libplacebo" \\
            "$dest/share/licenses/libplacebo"
        printf '%s' "$LIBPLACEBO_GIT_COMMIT" > "$dest/lib/libplacebo.a"
        echo pkgconfig > "$dest/lib/pkgconfig/libplacebo.pc"
        echo header > "$dest/include/libplacebo/config.h"
        echo license > "$dest/share/licenses/libplacebo/LICENSE"
        ;;
    *) exit 2 ;;
esac
''')

    def install(expected):
        trace.unlink(missing_ok=True)
        result = subprocess.run([bash, "-e", "include/ci.sh", "install"], cwd=root,
                                env=environment, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert (trace.read_text().splitlines() if trace.exists() else []) == expected
        for arch in ("armv7l", "arm64"):
            lib = root / "prefix" / arch / "lib"
            assert (lib / "libffmpeg.so").read_text() == environment["FFMPEG_GIT_COMMIT"]
            assert (lib / "libplacebo.a").read_text() == environment["LIBPLACEBO_GIT_COMMIT"]

    base_steps = ["download base", "base armv7l", "base arm64"]
    libplacebo_steps = ["download libplacebo", "libplacebo armv7l", "libplacebo arm64"]
    install(base_steps + libplacebo_steps)
    install([])
    environment["LIBPLACEBO_GIT_COMMIT"] = "b" * 40
    # A restored base archive must contain no renderer from the preceding run.
    listing = subprocess.run(
        [bash, "-c", 'tar -tzf - < "$CACHE_FOLDER/data.tgz"'], env=environment,
        capture_output=True, text=True, check=True, timeout=30).stdout
    assert "libplacebo" not in listing
    install(libplacebo_steps)
    (libplacebo_cache / "data.tgz").write_bytes(b"truncated cache")
    install(libplacebo_steps)
    (base_cache / "data.tgz").write_bytes(b"truncated cache")
    install(base_steps)
    environment["FFMPEG_GIT_COMMIT"] = "c" * 40
    trace.unlink()
    environment["FAIL_DOWNLOAD"] = "base"
    failure = subprocess.run([bash, "-e", "include/ci.sh", "install"], cwd=root,
                             env=environment, text=True, capture_output=True, timeout=30)
    assert failure.returncode == 7, failure.stdout + failure.stderr
    assert trace.read_text().splitlines() == ["download base"]
    del environment["FAIL_DOWNLOAD"]
    install(base_steps + libplacebo_steps)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildscripts", type=Path, required=True)
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    environment = os.environ.copy()
    environment["MPV_ANDROID_ARCHES"] = "armv7l arm64"
    for index, name in enumerate(("FFMPEG", "DAV1D", "LIBASS", "LIBPLACEBO"), 1):
        environment[f"{name}_GIT_COMMIT"] = str(index) * 40

    with tempfile.TemporaryDirectory(prefix="mpv-ci-cache-") as directory:
        root = Path(directory) / "buildscripts"
        shutil.copytree(args.buildscripts, root,
                        ignore=shutil.ignore_patterns("deps", "prefix", "sdk", "tests"))
        # Model Git's LF checkout used by the Linux producer on Windows hosts too.
        for path in root.rglob("*"):
            if path.is_file():
                path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))

        def identity():
            result = subprocess.run(
                [args.bash, "-e", "include/ci.sh", "export"], cwd=root, env=environment,
                text=True, capture_output=True, check=True, timeout=30)
            values = dict(line.split("=", 1) for line in result.stdout.splitlines())
            assert values["FFMPEG_GIT_COMMIT"] == environment["FFMPEG_GIT_COMMIT"]
            assert values["LIBPLACEBO_GIT_COMMIT"] == environment["LIBPLACEBO_GIT_COMMIT"].lower()
            keys = (values["CACHE_IDENTIFIER"], values["LIBPLACEBO_CACHE_IDENTIFIER"])
            assert all(len(key) <= 512 for key in keys), keys
            return keys

        def changed(path, baseline, expected):
            original = path.read_bytes()
            try:
                path.write_bytes(original + b"\n# cache regression fixture\n")
                assert tuple(a != b for a, b in zip(identity(), baseline)) == expected, (
                    str(path.relative_to(root)))
            finally:
                path.write_bytes(original)

        baseline = identity()
        assert identity() == baseline
        for path in ("patches/libdvdnav-7.0.0-menu-availability.patch",
                     "scripts/libdvdnav.sh", "scripts/shaderc.sh",
                     "patches/shaderc-r30-validator-friendly-names.patch",
                     "include/download-deps.sh", "include/ffmpeg-shared.py",
                     "scripts/ffmpeg.sh"):
            changed(root / path, baseline, (True, True))
        changed(root / "scripts/libplacebo.sh", baseline, (False, True))
        changed(root / "scripts/mpv.sh", baseline, (False, False))
        changed(root / "scripts/mpv-android.sh", baseline, (False, False))
        added = root / "patches/libdvdnav-cache-fixture.patch"
        added.write_text("new dependency patch\n")
        assert all(a != b for a, b in zip(identity(), baseline))
        added.unlink()
        assert identity() == baseline
        for name in ("FFMPEG", "DAV1D", "LIBASS", "LIBPLACEBO"):
            key = f"{name}_GIT_COMMIT"
            original = environment[key]
            try:
                environment[key] = "a" * 40
                base, libplacebo = identity()
                assert (base != baseline[0]) == (name != "LIBPLACEBO")
                assert libplacebo != baseline[1]
            finally:
                environment[key] = original
            assert identity() == baseline
        environment["LIBPLACEBO_GIT_COMMIT"] = "A" * 40
        normalized = identity()
        environment["LIBPLACEBO_GIT_COMMIT"] = "a" * 40
        assert identity() == normalized
        for commit in ("fongmi", "a" * 39, "g" * 40):
            environment["LIBPLACEBO_GIT_COMMIT"] = commit
            result = subprocess.run(
                [args.bash, "-e", "include/ci.sh", "export"], cwd=root, env=environment,
                text=True, capture_output=True, timeout=30)
            assert result.returncode != 0
            assert "Invalid libplacebo commit" in result.stderr
            assert not result.stdout
        environment["LIBPLACEBO_GIT_COMMIT"] = "a" * 40
        verify_install(root, args.bash, environment)
    print("Dependency cache identity and layered install: passed")


if __name__ == "__main__":
    main()
