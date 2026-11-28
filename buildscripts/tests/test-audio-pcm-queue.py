#!/usr/bin/env python3
"""Run the production PCM queue against deterministic audio frames."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpv", required=True, type=Path)
    parser.add_argument("--cxx", default="cl" if os.name == "nt" else "c++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = Path(__file__).resolve().with_name("audio-pcm-queue.cpp")
    headers = args.mpv.resolve() / "include"

    with tempfile.TemporaryDirectory(prefix="audio-pcm-queue-") as directory:
        work = Path(directory)
        output = work / ("test.exe" if os.name == "nt" else "test")
        if Path(args.cxx).stem.lower() in ("cl", "clang-cl"):
            command = [args.cxx, "/nologo", "/EHsc", "/W4", "/WX", f"/I{root}",
                       f"/I{headers}", f"/Fe{output}", f"/Fo{work / 'test.obj'}", str(source)]
        else:
            command = [args.cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pthread",
                       f"-I{root}", f"-I{headers}", str(source), "-o", str(output)]
        subprocess.run(command, cwd=work, check=True, timeout=60)
        subprocess.run([str(output)], cwd=work, check=True, timeout=10)


if __name__ == "__main__":
    main()
