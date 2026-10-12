#!/usr/bin/env python3
"""Check FFmpeg aggregation with real Android ELF archives and versioned consumers."""

import argparse
import importlib.util
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


class FfmpegSharedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location(
            "ffmpeg_shared", cls.buildscripts / "include/ffmpeg-shared.py")
        cls.shared = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.shared)
        cls.temporary = tempfile.TemporaryDirectory(prefix="ffmpeg-shared-fixtures-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.fixtures = Path(cls.temporary.name)
        cls.clang = str(cls.toolchain / ("clang++.exe" if (cls.toolchain / "clang++.exe").exists()
                                         else "clang++"))
        suffix = ".exe" if cls.clang.endswith(".exe") else ""
        cls.ar = str(cls.toolchain / ("llvm-ar" + suffix))
        cls.readelf = str(cls.toolchain / ("llvm-readelf" + suffix))
        cls.strip = str(cls.toolchain / ("llvm-strip" + suffix))
        cls.targets = ("armv7a-linux-androideabi24", "aarch64-linux-android24")
        for target in cls.targets:
            cls.create_fixture(cls.fixtures / target, target)

    @classmethod
    def command(cls, arguments):
        result = subprocess.run(arguments, text=True, encoding="utf-8",
                                capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result

    @classmethod
    def create_fixture(cls, root, target):
        prefix = root / "prefix"
        libdir = prefix / "lib"
        (libdir / "pkgconfig").mkdir(parents=True)
        compiler = [cls.clang, "--target=" + target]
        dependency = root / "dependency.cpp"
        dependency.write_text('#include <new>\nextern "C" void *dependency_api() '
                              '{ return ::operator new(16); }\n'
                              'extern "C" int dependency_private() { return 99; }\n')
        cls.command(compiler + ["-fPIC", "-c", str(dependency), "-o", str(root / "dependency.o")])
        cls.command([cls.ar, "rcs", str(libdir / "libdependency.a"), str(root / "dependency.o")])
        shared_source = root / "shared.cpp"
        shared_source.write_text('extern "C" { int ff_shared_table[] = {1}; }\n')
        shared_object = root / "shared.o"
        cls.command(compiler + ["-fPIC", "-c", str(shared_source), "-o", str(shared_object)])
        for index, library in enumerate(reversed(cls.shared.LIBRARIES)):
            base = "lib" + library
            directory = root / "build" / base
            directory.mkdir(parents=True)
            source = directory / "fixture.cpp"
            if library == "avcodec":
                body = ('#include <new>\nextern "C" int avutil_version();\n'
                        'extern "C" void *dependency_api();\n'
                        'extern "C" int avcodec_version() {\n'
                        '  ::operator delete(dependency_api()); return avutil_version() '
                        '+ ff_shared_table[0];\n}\n')
            else:
                body = (f'extern "C" int {library}_version() '
                        f'{{ return {index + 1} + ff_shared_table[0]; }}\n')
            if library == "avutil":
                body += 'extern "C" int av_overlapping_prefix() { return 10; }\n'
            source.write_text('extern "C" int ff_shared_table[];\n' + body)
            obj = directory / "fixture.o"
            cls.command(compiler + ["-fPIC", "-c", str(source), "-o", str(obj)])
            # CONFIG_SHARED adds duplicate SHLIBOBJS to FFmpeg static archives.
            objects = [str(obj), str(shared_object)]
            if library == "avcodec":
                dovi_source = directory / "dovi.cpp"
                dovi_source.write_text("".join('extern "C" int ' + symbol
                                               + '() { return 1; }\n'
                                               for symbol in sorted(cls.shared.DOVI_SYMBOLS)))
                dovi_object = directory / "dovi.o"
                cls.command(compiler + ["-fPIC", "-c", str(dovi_source), "-o", str(dovi_object)])
                objects.append(str(dovi_object))
            archive = directory / (base + ".a")
            cls.command([cls.ar, "rcs", str(archive)] + objects)
            version = "LIB" + library.upper() + "_" + str(61 + index)
            version_map = directory / (base + ".ver")
            pattern = "av*" if library.startswith("av") else "sw*"
            version_map.write_text(version + " { global: " + pattern + "; local: *; };\n")
            flags = ["-L" + str(libdir), "-ldependency"]
            if library == "avcodec":
                flags.append("-lavutil")
            cls.command(compiler + ["-shared", "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384",
                                    "-Wl,-soname," + base + ".so",
                                    "-Wl,--version-script," + str(version_map)] + objects
                        + flags + ["-o", str(directory / (base + ".so"))])
            for suffix in (".a", ".so"):
                shutil.copyfile(directory / (base + suffix), libdir / (base + suffix))
            (libdir / "pkgconfig" / (base + ".pc")).write_text(
                "prefix=/usr/local\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n"
                f"Name: {base}\nDescription: fixture\nVersion: {61 + index}.0.0\n"
                "Requires:\nRequires.private: libavutil\n"
                f"Libs: -L${{libdir}} -l{library}\nLibs.private: -ldependency\n"
                "Cflags: -I${includedir}\n")

    def copy_fixture(self, target):
        temporary = tempfile.TemporaryDirectory(prefix="ffmpeg-shared-test-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        shutil.copytree(self.fixtures / target, root, dirs_exist_ok=True)
        return root

    def aggregate(self, root, target, libraries=None):
        if libraries is None:
            libraries = ["-L" + str(root / "prefix/lib"), "-ldependency"]
            libraries += ["-l" + library for library in self.shared.LIBRARIES]
        self.shared.consolidate(
            root / "build", root / "prefix",
            shlex.join([self.clang, "--target=" + target]),
            "-Wl,-z,max-page-size=16384", libraries, self.readelf)

    def test_both_abis_preserve_versions_and_link_consumers(self):
        for target in self.targets:
            with self.subTest(target=target):
                root = self.copy_fixture(target)
                self.aggregate(root, target)
                libdir = root / "prefix/lib"
                self.assertEqual(sorted(path.name for path in libdir.glob("*.so")), ["libffmpeg.so"])
                self.assertEqual(sorted(path.name for path in libdir.glob("*.a")), ["libdependency.a"])
                symbols = self.shared.exports(self.readelf, libdir / "libffmpeg.so")
                self.assertEqual(symbols["av_overlapping_prefix"], "LIBAVUTIL_61")
                self.assertEqual(symbols["avcodec_version"], "LIBAVCODEC_64")
                self.assertTrue(self.shared.DOVI_SYMBOLS <= symbols.keys())
                self.assertNotIn("dependency_api", symbols)
                for pc in (libdir / "pkgconfig").glob("*.pc"):
                    text = pc.read_text()
                    self.assertIn("Libs: -L${libdir} -lffmpeg\n", text)
                    self.assertIn("Requires.private:\n", text)
                    self.assertIn("Libs.private:\n", text)
                consumer = root / "consumer.cpp"
                consumer.write_text('extern "C" int avcodec_version();\n'
                                    'extern "C" int av_overlapping_prefix();\n'
                                    'extern "C" int consume() { return avcodec_version() '
                                    '+ av_overlapping_prefix(); }\n')
                output = root / "consumer.so"
                self.command([self.clang, "--target=" + target, "-shared", "-fPIC",
                              "-Wl,--no-undefined", str(consumer), "-L" + str(libdir),
                              "-lffmpeg", "-o", str(output)])
                dynamic = self.shared.readelf(self.readelf, "--dynamic", output)
                self.assertIn("[libffmpeg.so]", dynamic)
                self.assertNotIn("[libavcodec.so]", dynamic)
                versions = self.shared.readelf(self.readelf, "--version-info", output)
                self.assertIn("LIBAVCODEC_64", versions)
                self.assertIn("LIBAVUTIL_61", versions)
                abi = "armeabi-v7a" if target.startswith("armv7a") else "arm64-v8a"
                package = root / abi
                package.mkdir()
                shutil.copyfile(libdir / "libffmpeg.so", package / "libffmpeg.so")
                for name in ("mpv", "player"):
                    self.command([self.clang, "--target=" + target, "-shared", "-fPIC",
                                  "-Wl,--no-undefined", "-Wl,-soname,lib" + name + ".so",
                                  str(consumer), "-L" + str(libdir), "-lffmpeg",
                                  "-o", str(package / ("lib" + name + ".so"))])
                triple = "arm-linux-androideabi" if target.startswith("armv7a") else "aarch64-linux-android"
                runtime = self.toolchain.parent / "sysroot/usr/lib" / triple / "libc++_shared.so"
                shutil.copyfile(runtime, package / "libc++_shared.so")
                self.command([self.strip, "--strip-unneeded", str(package / "libffmpeg.so")])
                self.assertEqual(self.shared.exports(self.readelf, package / "libffmpeg.so"), symbols)
                self.shared.verify_package(package, self.readelf)
                wrong_abis = ("arm64-v8a", "x86") if abi == "armeabi-v7a" else ("armeabi-v7a", "x86_64")
                for wrong_abi in wrong_abis:
                    wrong_package = root / wrong_abi
                    shutil.copytree(package, wrong_package)
                    with self.assertRaisesRegex(ValueError, "ELF ABI mismatch"):
                        self.shared.verify_package(wrong_package, self.readelf)
                shutil.copyfile(root / "build/libavcodec/libavcodec.so", package / "libavcodec.so")
                with self.assertRaisesRegex(ValueError, "Unexpected native package"):
                    self.shared.verify_package(package, self.readelf)
                (package / "libavcodec.so").unlink()
                self.command([self.clang, "--target=" + target, "-shared", "-fPIC",
                              "-Wl,--no-undefined", "-Wl,-soname,libplayer.so", str(consumer),
                              "-L" + str(root / "build/libavcodec"),
                              "-L" + str(root / "build/libavutil"), "-lavcodec", "-lavutil",
                              "-o", str(package / "libplayer.so")])
                with self.assertRaisesRegex(ValueError, "still needs split FFmpeg"):
                    self.shared.verify_package(package, self.readelf)

    def test_missing_archive_keeps_installed_split_libraries(self):
        root = self.copy_fixture(self.targets[0])
        (root / "build/libavcodec/libavcodec.a").unlink()
        with self.assertRaisesRegex(ValueError, "Missing FFmpeg PIC archive"):
            self.aggregate(root, self.targets[0])
        self.assertFalse((root / "prefix/lib/libffmpeg.so").exists())
        self.assertEqual(len(list((root / "prefix/lib").glob("*.so"))), 7)

    def test_incorrect_version_map_is_rejected_before_install(self):
        root = self.copy_fixture(self.targets[0])
        (root / "build/libavcodec/libavcodec.ver").write_text(
            "LIBAVCODEC_999 { global: av*; local: *; };\n")
        with self.assertRaisesRegex(ValueError, "Missing versioned public API"):
            self.aggregate(root, self.targets[0])
        self.assertFalse((root / "prefix/lib/libffmpeg.so").exists())

    def test_missing_external_dependency_fails_without_install(self):
        root = self.copy_fixture(self.targets[0])
        with self.assertRaises(subprocess.CalledProcessError):
            self.aggregate(root, self.targets[0], libraries=[])
        self.assertFalse((root / "prefix/lib/libffmpeg.so").exists())
        self.assertEqual(len(list((root / "prefix/lib").glob("*.so"))), 7)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildscripts", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True,
                        help="NDK LLVM prebuilt bin directory")
    args = parser.parse_args()
    FfmpegSharedTests.buildscripts = args.buildscripts.resolve()
    FfmpegSharedTests.toolchain = args.toolchain.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FfmpegSharedTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
