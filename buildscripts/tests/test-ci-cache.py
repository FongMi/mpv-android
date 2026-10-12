#!/usr/bin/env python3
"""Check dependency cache identity without resolving remote branches or building."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildscripts", type=Path, required=True)
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    environment = os.environ.copy()
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
            assert len(values["CACHE_IDENTIFIER"]) <= 512, len(values["CACHE_IDENTIFIER"])
            return values["CACHE_IDENTIFIER"]

        def changed(path, baseline, expected):
            original = path.read_bytes()
            try:
                path.write_bytes(original + b"\n# cache regression fixture\n")
                assert (identity() != baseline) == expected, str(path.relative_to(root))
            finally:
                path.write_bytes(original)

        baseline = identity()
        assert identity() == baseline
        changed(root / "patches/libdvdnav-7.0.0-menu-availability.patch", baseline, True)
        changed(root / "scripts/libdvdnav.sh", baseline, True)
        changed(root / "include/download-deps.sh", baseline, True)
        changed(root / "include/ffmpeg-shared.py", baseline, True)
        changed(root / "scripts/ffmpeg.sh", baseline, True)
        changed(root / "scripts/mpv.sh", baseline, False)
        changed(root / "scripts/mpv-android.sh", baseline, False)
        added = root / "patches/libdvdnav-cache-fixture.patch"
        added.write_text("new dependency patch\n")
        assert identity() != baseline
        added.unlink()
        assert identity() == baseline
        for name in ("FFMPEG", "LIBPLACEBO"):
            key = f"{name}_GIT_COMMIT"
            original = environment[key]
            try:
                environment[key] = "a" * 40
                assert identity() != baseline
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
    print("Dependency cache identity: passed (patches, recipes, native refs, mpv isolation)")


if __name__ == "__main__":
    main()
