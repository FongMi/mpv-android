#!/usr/bin/env python3
"""Check buildscript failure propagation and install-tool selection on the host."""

import argparse
import hashlib
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
                   "v_ndk=fixture\nv_ndk_n=30.0.16248370\ndep_failure=()\n"
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

    def prepare_shaderc(self):
        recipe = self.copy_source("scripts/shaderc.sh")
        patch = self.copy_source("patches/shaderc-r30-validator-friendly-names.patch")
        # A minimal real patch context, with fixture hashes replacing only the
        # pinned production pre/post identities. Never mutate the installed NDK.
        lines = patch.read_text().splitlines(keepends=True)
        hunk = lines[next(i for i, line in enumerate(lines) if line.startswith("@@")) + 1:]
        original = "".join(line[1:] for line in hunk if line.startswith((" ", "-")))
        patched = "".join(line[1:] for line in hunk if line.startswith((" ", "+")))
        text = recipe.read_text()
        for name, value in (("original_sha", original), ("patched_sha", patched)):
            text = re.sub(name + r"=[0-9a-f]{64}",
                          name + "=" + hashlib.sha256(value.encode()).hexdigest(), text)
        recipe.write_text(text)
        base = "buildscripts/sdk/android-ndk-fixture"
        source = base + "/sources/third_party/shaderc"
        for name in ("Android.mk", "CHANGES", "libshaderc/source", "third_party/source", "utils/source"):
            self.write(source + "/" + name, "source fixture\n")
        wrapper = self.write(source + "/libshaderc_util/src/spirv_tools_wrapper.cc", original)
        self.write(source + "/obj/shared-sentinel", "untouched\n")
        self.write(source + "/libs/shared-sentinel", "untouched\n")
        self.write(base + "/ndk-build", '#!/bin/bash -e\n'
                   'echo "$2" >> "$TRACE_FILE"\n'
                   '[ "${SHADERC_FAIL:-0}" = 0 ] || exit 17\n'
                   '[ "${SHADERC_MISSING:-0}" = 0 ] || exit 0\n'
                   'for arg in "$@"; do case "$arg" in APP_ABI=*) abi=${arg#APP_ABI=};; esac; done\n'
                   'mkdir -p "$2/libs/c++_static/$abi" "$2/include/shaderc"\n'
                   'printf "%s" "$abi" > "$2/libs/c++_static/$abi/libshaderc.a"\n'
                   'echo header > "$2/include/shaderc/shaderc.h"\n', executable=True)
        directory = self.root / "buildscripts/deps/shaderc"
        directory.mkdir(parents=True)
        return directory, wrapper

    def shaderc(self, directory, arch="arm64", action="build"):
        prefix = (self.root / "buildscripts/prefix" / arch).as_posix()
        return self.run_shell("prefix_name=" + arch + " prefix_dir=" + shlex.quote(prefix)
                              + " ../../scripts/shaderc.sh " + action, directory)

    def test_shaderc_private_build_and_clean(self):
        directory, wrapper = self.prepare_shaderc()
        before = {p.relative_to(wrapper.parents[2]): p.read_bytes()
                  for p in wrapper.parents[2].rglob("*") if p.is_file()}
        for arch in ("arm64", "armv7l", "arm64"):
            result = self.shaderc(directory, arch)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        private = directory / "_source_arm64-v8a"
        self.assertFalse((private / "obj/shared-sentinel").exists())
        self.assertFalse((private / "libs/shared-sentinel").exists())
        self.assertEqual((private / "libshaderc_util/src/spirv_tools_wrapper.cc")
                         .read_text().count("SetFriendlyNames(false)"), 1)
        self.assertEqual(self.shaderc(directory, action="clean").returncode, 0)
        self.assertFalse(private.exists())
        self.assertTrue((directory / "_source_armeabi-v7a").is_dir())
        self.assertFalse((self.root / "buildscripts/prefix/arm64/lib/libshaderc.a").exists())
        self.assertEqual(before, {p.relative_to(wrapper.parents[2]): p.read_bytes()
                                 for p in wrapper.parents[2].rglob("*") if p.is_file()})

    def test_shaderc_source_drift_and_build_failures(self):
        directory, wrapper = self.prepare_shaderc()
        original = wrapper.read_bytes()
        wrapper.write_bytes(original + b"drift\n")
        result = self.shaderc(directory)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pinned source", result.stderr)
        self.assertEqual(self.trace(), [])
        wrapper.write_bytes(original)
        self.environment["SHADERC_FAIL"] = "1"
        self.assertEqual(self.shaderc(directory).returncode, 17)
        self.environment.pop("SHADERC_FAIL")
        self.environment["SHADERC_MISSING"] = "1"
        result = self.shaderc(directory)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build output not found", result.stderr)
        self.assertFalse((self.root / "buildscripts/prefix/arm64/lib/libshaderc.a").exists())
        patch = self.root / "buildscripts/patches/shaderc-r30-validator-friendly-names.patch"
        saved_patch = patch.read_text()
        patch.write_text(saved_patch + "\nChanged patch identity\n")
        before_calls = self.trace()
        result = self.shaderc(directory)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Private shaderc source identity changed", result.stderr)
        self.assertEqual(self.trace(), before_calls)
        patch.write_text(saved_patch)
        private = directory / "_source_arm64-v8a/libshaderc_util/src/spirv_tools_wrapper.cc"
        private.write_text(private.read_text() + "drift\n")
        result = self.shaderc(directory)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Private shaderc source identity changed", result.stderr)

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

    def test_patch_callers(self):
        self.copy_source("include/patch-libbluray.sh")
        patches = ("still-eof", "checked-seek", "menu-info", "bdj-pointer")
        for name in patches:
            self.write("buildscripts/patches/libbluray-1.4.1-" + name + ".patch", "fixture\n")
        directory = self.root / "buildscripts/deps/libbluray"
        directory.mkdir(parents=True)
        self.tool("patch", 'case " $* " in *" --dry-run "*) exit 0 ;; esac\n'
                  'if [ ! -f "$TRACE_FILE" ]; then\n'
                  '    echo failed > "$TRACE_FILE"\n    exit 7\nfi\n'
                  'echo continued >> "$TRACE_FILE"\n')
        for caller, cwd in (("include/download-deps.sh", directory.parent),
                            ("build-bdj-jars.sh", directory.parent.parent)):
            with self.subTest(caller=caller):
                (self.root / "trace.txt").unlink(missing_ok=True)
                lines = (self.buildscripts / caller).read_text().splitlines()
                command = next(line for line in lines if "include/patch-libbluray.sh" in line)
                result = self.run_shell("source_dir=deps/libbluray\n" + command, cwd)
                self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
                self.assertEqual(self.trace(), ["failed"])

    def test_ci_existing_ffmpeg_revision(self):
        source = (self.buildscripts / "include/download-deps.sh").read_text()
        command = source.split("# ffmpeg\n", 1)[1].split("# AVS3", 1)[0]
        directory = self.root / "buildscripts/deps"
        repo = directory / "ffmpeg"
        repo.mkdir(parents=True)
        result = self.run_shell(
            "git init -q ffmpeg\n"
            "git -C ffmpeg config user.email fixture@example.invalid\n"
            "git -C ffmpeg config user.name Fixture\n"
            "echo first > ffmpeg/source.c\n"
            "git -C ffmpeg add source.c\n"
            "git -C ffmpeg commit -qm first\n"
            "git -C ffmpeg rev-parse HEAD", directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        commit = result.stdout.strip()
        self.environment.update(IN_CI="1", FFMPEG_GIT_COMMIT=commit)
        for content in ("first\n", "recipe patch\n"):
            (repo / "source.c").write_text(content)
            result = self.run_shell(command, directory)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.environment["FFMPEG_GIT_COMMIT"] = "0" * 40
        result = self.run_shell(command, directory)
        self.assertNotEqual(result.returncode, 0, "stale FFmpeg checkout accepted")
        self.assertEqual((repo / "source.c").read_text(), "recipe patch\n")
        result = self.run_shell("git -C ffmpeg rev-parse HEAD", directory)
        self.assertEqual(result.stdout.strip(), commit)

    def test_ci_existing_libplacebo_source(self):
        source = (self.buildscripts / "include/download-deps.sh").read_text()
        start = source.index("download_libplacebo() {")
        end = source.index('\nif [[ "$scope" == libplacebo ]]; then', start)
        command = source[start:end] + "\ndownload_libplacebo\n"
        directory = self.root / "buildscripts/deps"
        repo = directory / "libplacebo"
        repo.mkdir(parents=True)
        result = self.run_shell(
            "git init -q libplacebo\n"
            "git -C libplacebo config user.email fixture@example.invalid\n"
            "git -C libplacebo config user.name Fixture\n"
            "echo first > libplacebo/source.c\n"
            "git -C libplacebo add source.c\n"
            "git -C libplacebo commit -qm first\n"
            "git -C libplacebo rev-parse HEAD", directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        commit = result.stdout.strip()
        self.environment.update(IN_CI="1", LIBPLACEBO_GIT_COMMIT=commit)
        result = self.run_shell(command, directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.environment["LIBPLACEBO_GIT_COMMIT"] = "0" * 40
        result = self.run_shell(command, directory)
        self.assertNotEqual(result.returncode, 0, "stale checkout accepted")
        self.assertIn("libplacebo", result.stderr)
        self.environment["LIBPLACEBO_GIT_COMMIT"] = commit
        for state in ("unstaged", "staged", "untracked"):
            with self.subTest(state=state):
                path = repo / ("extra.c" if state == "untracked" else "source.c")
                path.write_text("changed\n")
                if state == "staged":
                    result = self.run_shell("git -C libplacebo add source.c", directory)
                    self.assertEqual(result.returncode, 0, result.stderr)
                result = self.run_shell(command, directory)
                self.assertNotEqual(result.returncode, 0, state + " source accepted")
                self.assertEqual(path.read_text(), "changed\n")
                if state == "untracked":
                    path.unlink()
                else:
                    path.write_text("first\n")
                    result = self.run_shell("git -C libplacebo add source.c", directory)
                    self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_shell("git -C libplacebo rev-parse HEAD", directory)
        self.assertEqual(result.stdout.strip(), commit)
        git_dir = repo / ".git"
        saved_git = directory / "saved-git"
        git_dir.rename(saved_git)
        try:
            result = self.run_shell(command, directory)
            self.assertNotEqual(result.returncode, 0, "non-repository accepted")
            self.assertEqual((repo / "source.c").read_text(), "first\n")
        finally:
            saved_git.rename(git_dir)
        result = self.run_shell(
            "git -c protocol.file.allow=always -C libplacebo submodule add -q "
            "./ local-submodule\n"
            "git -C libplacebo commit -qm submodule\n"
            "git -C libplacebo rev-parse HEAD", directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.environment["LIBPLACEBO_GIT_COMMIT"] = result.stdout.strip()
        result = self.run_shell(command, directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        submodule = repo / "local-submodule"
        nested_source = submodule / "source.c"
        nested_source.write_text("nested change\n")
        result = self.run_shell(command, directory)
        self.assertNotEqual(result.returncode, 0, "dirty submodule accepted")
        self.assertEqual(nested_source.read_text(), "nested change\n")
        nested_source.write_text("first\n")
        saved_submodule = directory / "saved-submodule"
        submodule.rename(saved_submodule)
        try:
            result = self.run_shell(command, directory)
            self.assertNotEqual(result.returncode, 0, "uninitialized submodule accepted")
            self.assertEqual((saved_submodule / "source.c").read_text(), "first\n")
        finally:
            saved_submodule.rename(submodule)

    @staticmethod
    def header_digest(prefix):
        base = prefix / "include"
        rows = []
        for path in sorted((base / "libplacebo").rglob("*")):
            if path.is_file():
                rows.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  "
                            + path.relative_to(base).as_posix() + "\n")
        return hashlib.sha256("".join(rows).encode()).hexdigest()

    def update_lp_receipt(self, arch):
        prefix = self.root / "buildscripts/prefix" / arch
        receipt = prefix / "share/mpv-motion/provenance.properties"
        lines = [line for line in receipt.read_text().splitlines()
                 if not line.startswith("libplacebo.")]
        lines.extend(["libplacebo.archive.sha256="
                      + hashlib.sha256((prefix / "lib/libplacebo.a").read_bytes()).hexdigest(),
                      "libplacebo.headers.sha256=" + self.header_digest(prefix)])
        receipt.write_text("\n".join(lines) + "\n")

    def prepare_sdk(self):
        self.copy_source("export-renderer-sdk.sh")
        headers = {"config.h": "#define PL_API_VER 1\n#define PL_HAVE_OPENGL 1\n"
                   "#define PL_HAVE_VULKAN 1\n#define PL_HAVE_SHADERC 1\n",
                   "opengl.h": "bool external_yuv;\n",
                   "vulkan.h": "bool disable_storage;\nstruct pl_vulkan_ycbcr_params {\n"
                   "const struct pl_vulkan_ycbcr_params *ycbcr;\n",
                   "utils/libav.h": "pl_map_avframe_dovi_metadata(\n",
                   "renderer.h": "".join("#define PL_HAVE_" + name + " 1\n" for name in
                       ("CUSTOM_FRAME_MIXER", "SOURCE_PEAK_TIMELINE", "SOURCE_PEAK_IMAGE",
                        "SOURCE_HOOK_PARAMETERS")),
                   "colorspace.h": "#define PL_HAVE_HDR_LINEAR 1\n"}
        for arch in ("armv7l", "arm64"):
            prefix = "buildscripts/prefix/" + arch
            for name, text in headers.items():
                self.write(prefix + "/include/libplacebo/" + name, text)
            for name in ("libplacebo.a", "libshaderc.a", "libmpv-motion.a"):
                self.write(prefix + "/lib/" + name, "fixture\n")
        self.write("buildscripts/prefix/arm64/share/licenses/libplacebo/LICENSE", "license\n")
        core = "buildscripts/deps/mpv/"
        self.write(core + "video/out/gpu_next/motion.c", "motion source\n")
        self.write(core + "video/out/gpu_next/motion.h", "enum mp_motion_result mp_motion_render_views(\n")
        self.write(core + "LICENSE.LGPL", "motion license\n")
        for arch in ("armv7l", "arm64"):
            self.write("buildscripts/prefix/" + arch + "/include/mpv-motion/motion.h",
                       "enum mp_motion_result mp_motion_render_views(\n")
            self.write("buildscripts/prefix/" + arch + "/share/mpv-motion/provenance.properties",
                       "schema.version=1\nmpv-motion.arch=" + arch + "\nmpv-motion.commit=" + "1" * 40 + "\n"
                       + "".join("mpv-motion." + field + ".sha256="
                                 + hashlib.sha256(content).hexdigest() + "\n"
                                 for field, content in (("source", b"motion source\n"),
                                                        ("header", b"enum mp_motion_result mp_motion_render_views(\n"),
                                                        ("archive", b"fixture\n"))))
        for arch in ("armv7l", "arm64"):
            self.update_lp_receipt(arch)
        # Fake archive/symbol output tests export decisions, not native linkage.
        self.write("buildscripts/sdk/android-ndk-fixture/toolchains/llvm/prebuilt/fixture/bin/llvm-nm",
                   '#!/bin/bash -e\necho "mp_motion_render_views T 0 1"\n', executable=True)
        self.tool("git", 'case "$3" in\n'
                  'rev-parse) echo ' + "1" * 40 + ';;\n'
                  'remote) echo https://example.invalid/mpv.git;;\n'
                  'diff|cat-file) exit 0;;\n'
                  '*) exit 2;;\nesac\n')
        return "buildscripts/export-renderer-sdk.sh"

    def test_sdk_export_failure(self):
        self.prepare_sdk()
        self.tool("cp", 'echo copy >> "$TRACE_FILE"\nexit 7\n')
        workflow = (self.buildscripts.parent / ".github/workflows/build.yml").read_text()
        match = re.search(r"(?m)^\s*- name: Export renderer SDK\s*\n\s*run: (.+)$", workflow)
        self.assertIsNotNone(match)
        assert (self.root / "build/renderer-sdk").resolve().is_relative_to(self.root)
        result = self.run_shell(match.group(1))
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertEqual(self.trace(), ["copy"])
        self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())

    def test_sdk_export_motion_provenance_and_manifests(self):
        command = self.prepare_sdk()
        result = self.run_shell(command)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = self.root / "build/renderer-sdk"
        properties = dict(line.split("=", 1) for line in
                          (output / "provenance.properties").read_text().splitlines())
        self.assertEqual(properties["schema.version"], "3")
        self.assertEqual(properties["android.ndk.version"], "30.0.16248370")
        self.assertEqual(properties["renderer-sdk.library.path"], "lib/libplacebo.a")
        self.assertEqual(properties["mpv-motion.commit"], "1" * 40)
        self.assertEqual(properties["mpv-motion.repository"], "https://example.invalid/mpv.git")
        for field, content in (("source", b"motion source\n"),
                               ("header", b"enum mp_motion_result mp_motion_render_views(\n")):
            self.assertEqual(properties["mpv-motion." + field + ".sha256"],
                             hashlib.sha256(content).hexdigest())
        self.assertEqual((output / "LICENSE.mpv-motion").read_text(), "motion license\n")
        manifest = (output / "files.sha256").read_text()
        for abi in ("armeabi-v7a", "arm64-v8a"):
            self.assertIn("./" + abi + "/lib/libmpv-motion.a", manifest)
            self.assertIn("./" + abi + "/include/mpv-motion/motion.h", manifest)
            self.assertIn("./" + abi + "/motion-build.properties", manifest)
            # Preserve the existing libplacebo-only header contract.
            self.assertNotIn("mpv-motion", (output / abi / "headers.sha256").read_text())
        self.assertIn("./LICENSE.mpv-motion", manifest)
        checked = self.run_shell("sha256sum -c files.sha256", output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_sdk_export_rejects_missing_motion_components(self):
        command = self.prepare_sdk()
        paths = ["buildscripts/deps/mpv/LICENSE.LGPL",
                 "buildscripts/deps/mpv/video/out/gpu_next/motion.c",
                 "buildscripts/deps/mpv/video/out/gpu_next/motion.h"]
        for arch in ("armv7l", "arm64"):
            paths.extend(["buildscripts/prefix/" + arch + "/lib/libmpv-motion.a",
                          "buildscripts/prefix/" + arch + "/include/mpv-motion/motion.h",
                          "buildscripts/prefix/" + arch + "/share/mpv-motion/provenance.properties"])
        for name in paths:
            with self.subTest(component=name):
                path = self.root / name
                original = path.read_bytes()
                path.unlink()
                try:
                    result = self.run_shell(command)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("missing", result.stderr)
                finally:
                    path.write_bytes(original)

    def test_sdk_export_rejects_header_drift_and_abi_api_mismatch(self):
        command = self.prepare_sdk()
        for name, replacement, error in (
                ("include/mpv-motion/motion.h", "stale header\n", "Installed motion header"),
                ("include/libplacebo/config.h", "#define PL_API_VER 2\n", "API differs"),
                ("include/libplacebo/opengl.h", "", "renderer capability")):
            with self.subTest(component=name):
                path = self.root / "buildscripts/prefix/arm64" / name
                original = path.read_bytes()
                path.write_text(replacement)
                try:
                    result = self.run_shell(command)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(error, result.stderr)
                finally:
                    path.write_bytes(original)

    def test_sdk_export_rejects_stale_build_receipt(self):
        command = self.prepare_sdk()
        result = self.run_shell(command)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name, content, field in (
                ("buildscripts/deps/mpv/video/out/gpu_next/motion.c",
                 b"new committed core with unchanged header\n", "source.sha256"),
                ("buildscripts/prefix/arm64/lib/libmpv-motion.a",
                 b"replaced archive\n", "archive.sha256"),
                ("buildscripts/prefix/arm64/share/mpv-motion/provenance.properties",
                 b"schema.version=1\nmpv-motion.arch=arm64\nmpv-motion.commit=" + b"2" * 40 + b"\n", "commit")):
            with self.subTest(field=field):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(content)
                try:
                    result = self.run_shell(command)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("build receipt mismatch", result.stderr)
                    self.assertIn(field, result.stderr)
                    self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())
                finally:
                    path.write_bytes(original)

    def test_sdk_export_rejects_archive_and_receipt_from_other_abi(self):
        command = self.prepare_sdk()
        prefix = self.root / "buildscripts/prefix"
        for name in ("lib/libmpv-motion.a", "share/mpv-motion/provenance.properties"):
            (prefix / "arm64" / name).write_bytes((prefix / "armv7l" / name).read_bytes())
        result = self.run_shell(command)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build receipt mismatch for arm64-v8a: mpv-motion.arch", result.stderr)
        self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())

    def test_sdk_export_requires_new_capabilities(self):
        command = self.prepare_sdk()
        for arch in ("armv7l", "arm64"):
            for capability in ("CUSTOM_FRAME_MIXER", "SOURCE_PEAK_TIMELINE", "SOURCE_PEAK_IMAGE",
                               "SOURCE_HOOK_PARAMETERS", "HDR_LINEAR"):
                header = "colorspace.h" if capability == "HDR_LINEAR" else "renderer.h"
                path = self.root / "buildscripts/prefix" / arch / "include/libplacebo" / header
                original = path.read_bytes()
                needle = ("#define PL_HAVE_" + capability + " 1\n").encode()
                for replacement in (b"", needle.replace(b" 1", b" 0")):
                    with self.subTest(arch=arch, capability=capability, replacement=replacement):
                        path.write_bytes(original.replace(needle, replacement))
                        try:
                            result = self.run_shell(command)
                            self.assertNotEqual(result.returncode, 0)
                            self.assertIn("PL_HAVE_" + capability, result.stderr)
                        finally:
                            path.write_bytes(original)

    def test_sdk_export_binds_libplacebo_content_not_mtime(self):
        command = self.prepare_sdk()
        for arch in ("armv7l", "arm64"):
            prefix = self.root / "buildscripts/prefix" / arch
            for relative in ("lib/libplacebo.a", "include/libplacebo/renderer.h"):
                path = prefix / relative
                original = path.read_bytes()
                stamp = path.stat()
                # Headers remain valid while contents change with equal size/mtime.
                changed = (original[:-1] + b" ")
                path.write_bytes(changed)
                os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                try:
                    result = self.run_shell(command)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("libplacebo.", result.stderr)
                    self.assertIn("build receipt mismatch", result.stderr)
                finally:
                    path.write_bytes(original)
                    os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            receipt = prefix / "share/mpv-motion/provenance.properties"
            original = receipt.read_bytes()
            for field in ("libplacebo.archive.sha256", "libplacebo.headers.sha256"):
                receipt.write_bytes(b"\n".join(line for line in original.split(b"\n")
                                               if not line.startswith(field.encode() + b"=")))
                try:
                    result = self.run_shell(command)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(field, result.stderr)
                finally:
                    receipt.write_bytes(original)

    def test_sdk_export_requires_motion_view_declaration(self):
        command = self.prepare_sdk()
        # Keep the source/install comparison valid; reject the missing API itself.
        for name in ("buildscripts/deps/mpv/video/out/gpu_next/motion.h",
                     "buildscripts/prefix/armv7l/include/mpv-motion/motion.h",
                     "buildscripts/prefix/arm64/include/mpv-motion/motion.h"):
            self.write(name, "legacy motion header\n")
        result = self.run_shell(command)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("header is missing mp_motion_render_views", result.stderr)
        self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())

    def test_sdk_export_requires_defined_motion_view_symbol(self):
        command = self.prepare_sdk()
        name = "buildscripts/sdk/android-ndk-fixture/toolchains/llvm/prebuilt/fixture/bin/llvm-nm"
        for body in ('echo "mp_motion_render_views U"\n',
                     'echo "mp_motion_render_views_other T 0 1"\n',
                     'printf "mp_motion_render_views T 0 1\\nmp_motion_render_views T 0 1\\n"\n',
                     'exit 7\n'):
            with self.subTest(nm=body):
                self.write(name, "#!/bin/bash -e\n" + body, executable=True)
                result = self.run_shell(command)
                self.assertNotEqual(result.returncode, 0)
                if body.startswith("exit"):
                    self.assertEqual(result.returncode, 7)
                else:
                    self.assertIn("archive must define mp_motion_render_views once", result.stderr)
                self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())

    def prepare_motion_build(self):
        self.copy_source("scripts/mpv.sh")
        prefix = self.root / "buildscripts/prefix/arm64"
        self.environment.update(prefix_dir=prefix.as_posix(), prefix_name="arm64",
                                ndk_suffix="_fixture")
        for name in ("include/iconv.h", "lib/libiconv.a", "lib/libcharset.a"):
            self.write("buildscripts/prefix/arm64/" + name, "fixture\n")
        self.write("buildscripts/prefix/arm64/lib/libplacebo.a", "LP archive fixture\n")
        self.write("buildscripts/prefix/arm64/include/libplacebo/renderer.h", "LP header fixture\n")
        core = "buildscripts/deps/mpv/"
        self.write(core + "meson.build", "# fixture\n")
        self.write(core + "video/out/gpu_next/motion.c", "motion source\n")
        self.write(core + "video/out/gpu_next/motion.h", "enum mp_motion_result mp_motion_render_views(\n")
        self.write(core + "LICENSE.LGPL", "motion license\n")
        self.write(core + "_build_fixture/libmpv.so", "shared library fixture\n")
        self.tool("git", 'case "$1" in\nrev-parse) echo ' + "1" * 40
                  + ';;\ndiff) exit 0;;\n*) exit 2;;\nesac\n')
        self.tool("meson", "exit 0\n")
        install = ('case " $* " in *" install "*)\n'
                   'mkdir -p "$prefix_dir/lib" "$prefix_dir/include/mpv-motion"\n'
                   'cp video/out/gpu_next/motion.h "$prefix_dir/include/mpv-motion/"\n'
                   'echo fixture > "$prefix_dir/lib/libmpv-motion.a";;\nesac\n')
        self.tool("ninja", install)
        return self.root / core, prefix / "share/mpv-motion/provenance.properties", install

    def test_motion_build_records_installed_archive(self):
        directory, receipt, _ = self.prepare_motion_build()
        result = self.run_shell("../../scripts/mpv.sh build", directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        properties = dict(line.split("=", 1) for line in receipt.read_text().splitlines())
        self.assertEqual(properties["mpv-motion.commit"], "1" * 40)
        self.assertEqual(properties["mpv-motion.arch"], "arm64")
        prefix = receipt.parents[2]
        self.assertEqual(properties["libplacebo.headers.sha256"], self.header_digest(prefix))
        self.assertEqual(properties["libplacebo.archive.sha256"],
                         hashlib.sha256((prefix / "lib/libplacebo.a").read_bytes()).hexdigest())
        for field, content in (("source", b"motion source\n"),
                               ("header", b"enum mp_motion_result mp_motion_render_views(\n"), ("archive", b"fixture\n")):
            self.assertEqual(properties["mpv-motion." + field + ".sha256"],
                             hashlib.sha256(content).hexdigest())

    def test_motion_build_rejects_changed_libplacebo_inputs(self):
        directory, receipt, install = self.prepare_motion_build()
        for relative in ("lib/libplacebo.a", "include/libplacebo/renderer.h"):
            with self.subTest(component=relative):
                receipt.parent.mkdir(parents=True, exist_ok=True)
                receipt.write_text("old receipt\n")
                self.tool("ninja", install + 'echo changed >> "$prefix_dir/' + relative + '"\n')
                result = self.run_shell("../../scripts/mpv.sh build", directory)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("libplacebo inputs changed", result.stderr)
                self.assertFalse(receipt.exists())

    def test_motion_build_failure_or_source_change_invalidates_receipt(self):
        directory, receipt, install = self.prepare_motion_build()
        for body in ("exit 7\n", install + "exit 7\n",
                     install + "echo changed >> video/out/gpu_next/motion.c\n"):
            with self.subTest(ninja=body):
                receipt.parent.mkdir(parents=True, exist_ok=True)
                receipt.write_text("old receipt\n")
                self.tool("ninja", body)
                result = self.run_shell("../../scripts/mpv.sh build", directory)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(receipt.exists())

    def test_motion_build_missing_shared_library_fails_without_rebuild(self):
        directory, receipt, install = self.prepare_motion_build()
        (directory / "_build_fixture/libmpv.so").unlink()
        self.write("buildscripts/deps/mpv/_build_fixture/libmpv.a", "static fixture\n")
        receipt.parent.mkdir(parents=True, exist_ok=True)
        receipt.write_text("stale receipt\n")
        self.tool("meson", 'echo meson >> "$TRACE_FILE"\n')
        self.tool("ninja", 'echo ninja >> "$TRACE_FILE"\n' + install)
        result = self.run_shell("../../scripts/mpv.sh build", directory)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required shared libmpv", result.stderr)
        self.assertEqual(self.trace(), ["meson", "ninja"])
        self.assertFalse(receipt.exists())
        self.assertTrue((directory / "_build_fixture/libmpv.a").is_file())

    def test_motion_build_shared_output_ignores_stale_static_archive(self):
        directory, receipt, install = self.prepare_motion_build()
        self.write("buildscripts/deps/mpv/_build_fixture/libmpv.a", "stale static fixture\n")
        self.tool("meson", 'echo meson >> "$TRACE_FILE"\n')
        self.tool("ninja", 'echo ninja >> "$TRACE_FILE"\n' + install)
        result = self.run_shell("../../scripts/mpv.sh build", directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.trace(), ["meson", "ninja", "ninja"])
        self.assertTrue(receipt.is_file())

    def test_sdk_export_rejects_unresolved_provenance(self):
        command = self.prepare_sdk()
        for stage, error in (("rev-parse", "resolved mpv commit"),
                             ("cat-file", "absent from its commit")):
            with self.subTest(stage=stage):
                self.tool("git", 'case "$3" in\n'
                          'rev-parse) echo ' + ("unknown" if stage == "rev-parse"
                                                else "1" * 40) + ';;\n'
                          'cat-file) exit 1;;\n'
                          '*) exit 2;;\nesac\n')
                result = self.run_shell(command)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)

    def test_sdk_export_rejects_dirty_motion_source(self):
        command = self.prepare_sdk()
        self.tool("git", 'case "$3" in\n'
                  'rev-parse) echo ' + "1" * 40 + ';;\n'
                  'remote) echo https://example.invalid/mpv.git;;\n'
                  'cat-file) exit 0;;\n'
                  'diff) exit 1;;\n'
                  '*) exit 2;;\nesac\n')
        result = self.run_shell(command)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differs from its source commit", result.stderr)

    def check_installers(self, selected):
        real_install = subprocess.check_output([self.bash, "-c", "command -v install"],
                                               text=True, encoding="utf-8").strip()
        self.environment["REAL_INSTALL"] = real_install
        self.environment["INSTALL"] = "ginstall" if selected else ""
        body = 'echo %s >> "$TRACE_FILE"\nexec "$REAL_INSTALL" "$@"\n'
        self.tool("ginstall", body % "ginstall")
        self.tool("install", 'exit 99\n' if selected else body % "install")
        for name in ("meson", "ninja", "cmake"):
            self.tool(name, "exit 0\n")
        self.tool("git", "echo " + "1" * 40 + "\n")
        self.copy_source("include/cmake-android.sh")
        prefix = self.root / "buildscripts/prefix/arm64"
        self.environment.update(prefix_dir=prefix.as_posix(), prefix_name="arm64",
                                ndk_suffix="_fixture")
        self.write("buildscripts/prefix/arm64/lib/libshaderc.a", "fixture\n")
        for recipe, license_name in (("libplacebo", "LICENSE"), ("uavs3d", "COPYING")):
            with self.subTest(recipe=recipe):
                (self.root / "trace.txt").unlink(missing_ok=True)
                self.copy_source("scripts/" + recipe + ".sh")
                base = "buildscripts/deps/" + recipe
                self.write(base + "/" + license_name, "license\n")
                self.write(base + "/_build_fixture/src/include/libplacebo/config.h",
                           "#define PL_HAVE_OPENGL 1\n#define PL_HAVE_VULKAN 1\n")
                self.write("buildscripts/prefix/arm64/lib/pkgconfig/" + recipe + ".pc", "Libs:\n")
                command = "../../scripts/" + recipe + ".sh build"
                result = self.run_shell(command, self.root / base)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.trace(), ["ginstall" if selected else "install"])
                target = prefix / "share/licenses" / recipe / license_name
                self.assertEqual(target.read_text(), "license\n")
                if os.name != "nt":
                    self.assertEqual(target.stat().st_mode & 0o777, 0o644)

    def test_selected_installers(self):
        self.check_installers(selected=True)

    def test_fallback_installers(self):
        self.check_installers(selected=False)

    def test_libplacebo_staged_install(self):
        self.copy_source("scripts/libplacebo.sh")
        base = "buildscripts/deps/libplacebo"
        self.write(base + "/LICENSE", "license\n")
        self.write(base + "/_build_fixture/src/include/libplacebo/config.h",
                   "#define PL_HAVE_OPENGL 1\n#define PL_HAVE_VULKAN 1\n")
        self.write("buildscripts/prefix/arm64/lib/libshaderc.a", "shaderc\n")
        prefix = self.root / "buildscripts/prefix/arm64"
        stage = self.root / "renderer"
        self.environment.update(prefix_dir=prefix.as_posix(), ndk_suffix="_fixture",
                                LIBPLACEBO_DESTDIR=stage.as_posix())
        self.tool("meson", 'test "${3}" = --cross-file\n'
                  'test "${4}" = "$prefix_dir/crossfile.txt"\n')
        self.tool("ninja", 'if [ "$3" = install ]; then\n'
                  '    test "$DESTDIR" = "$LIBPLACEBO_DESTDIR"\n'
                  '    pc="$DESTDIR/usr/local/lib/pkgconfig"\n'
                  '    mkdir -p "$pc"\n'
                  '    echo "Libs: -lplacebo" > "$pc/libplacebo.pc"\n'
                  'fi\n')
        result = self.run_shell("../../scripts/libplacebo.sh build", self.root / base)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        installed = stage / "usr/local"
        self.assertEqual((installed / "lib/pkgconfig/libplacebo.pc").read_text(),
                         "Libs: -lplacebo -lshaderc -lc++\n")
        self.assertEqual((installed / "share/licenses/libplacebo/LICENSE").read_text(),
                         "license\n")
        self.assertFalse((prefix / "lib/pkgconfig/libplacebo.pc").exists())


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
