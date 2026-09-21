#!/usr/bin/env python3
"""Check buildscript failure propagation and install-tool selection on the host."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


class BuildscriptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mpv-buildscripts-")
        self.root = Path(self.temporary.name).resolve()
        assert self.root.parent == Path(tempfile.gettempdir()).resolve()
        assert self.root.name.startswith("mpv-buildscripts-")
        self.addCleanup(self.temporary.cleanup)
        self.environment = os.environ.copy()
        for name in ("BASH_ENV", "ENV", "SHELLOPTS", "INSTALL"):
            self.environment.pop(name, None)
        self.environment.update(cores="1", MPV_ANDROID_NATIVE_ONLY="1",
                                TRACE_FILE=(self.root / "trace.txt").as_posix())
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.copy_source("include/path.sh")
        self.write("buildscripts/include/depinfo.sh",
                   "v_ndk=fixture\nv_ndk_n=fixture\ndep_failure=()\n"
                   "dep_mpv_android=()\nv_ci_uavs3d=" + "1" * 40 + "\n")

    def write(self, name, text, executable=False):
        path = self.root / name
        assert path.resolve().is_relative_to(self.root)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        if executable:
            path.chmod(0o755)
        return path

    def copy_source(self, name):
        # Model the LF checkout used by Linux CI on Windows hosts too.
        text = (self.buildscripts / name).read_text(encoding="utf-8")
        return self.write("buildscripts/" + name, text, executable=True)

    def tool(self, name, body):
        return self.write("bin/" + name, "#!/bin/bash -e\n" + body, executable=True)

    def run_shell(self, command, directory=None):
        command = ('export PATH="$(cd ' + shlex.quote(self.bin.as_posix())
                   + ' && pwd):$PATH"\n' + command)
        return subprocess.run([self.bash, "-e", "-c", command],
                              cwd=directory or self.root, env=self.environment,
                              text=True, encoding="utf-8", capture_output=True, timeout=30)

    def trace(self):
        path = self.root / "trace.txt"
        return path.read_text().splitlines() if path.exists() else []

    def prepare_dispatcher(self):
        script = self.copy_source("buildall.sh")
        toolchain = self.root / "buildscripts/sdk/android-ndk-fixture/toolchains/llvm/prebuilt/fixture/bin"
        toolchain.mkdir(parents=True)
        self.write("buildscripts/prefix/arm64/lib/libmpv.so", "fixture\n")
        self.tool("pkg-config", "exit 0\n")
        return shlex.quote(script.as_posix()) + " -n --arch arm64"

    def test_recipe_failure(self):
        command = self.prepare_dispatcher()
        (self.root / "buildscripts/deps/failure").mkdir(parents=True)
        self.write("buildscripts/scripts/failure.sh",
                   '#!/bin/bash -e\nprintf "%s\\n" "$1" >> "$TRACE_FILE"\n'
                   'false\nprintf continued >> "$TRACE_FILE"\n', executable=True)
        for phase, option in (("build", ""), ("clean", "--clean")):
            with self.subTest(phase=phase):
                (self.root / "trace.txt").unlink(missing_ok=True)
                result = self.run_shell(command + " " + option + " failure")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(self.trace(), [phase])

    def test_recipe_success(self):
        command = self.prepare_dispatcher()
        (self.root / "buildscripts/deps/failure").mkdir(parents=True)
        self.write("buildscripts/scripts/failure.sh",
                   '#!/bin/bash -e\nprintf "%s\\n" "$1" >> "$TRACE_FILE"\n',
                   executable=True)
        result = self.run_shell(command + " failure")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.trace(), ["build"])

    def test_native_only_failure(self):
        command = self.prepare_dispatcher()
        self.copy_source("scripts/mpv-android.sh")
        self.tool("ndk-build", "exit 7\n")
        result = self.run_shell(command + " mpv-android")
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertNotIn("Skipping Gradle APK build", result.stdout)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildscripts", type=Path, required=True)
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    BuildscriptTests.buildscripts = args.buildscripts.resolve()
    BuildscriptTests.bash = args.bash
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BuildscriptTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
