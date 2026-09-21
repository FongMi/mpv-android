#!/usr/bin/env python3
"""Run BD-J pointer routing regressions using production BDJHelper and real AWT.

Usage: python3 tests/test-bdj-pointer.py deps/libbluray --java-home /path/to/jdk8

The fixture uses the JDK 8 AWT component tree, lightweight peers, mouse listeners
and event queue. Only BD-J context lookup, logging and forced thread shutdown are
substituted. Authored-disc logic, Xlet isolation and Android playback need separate
device tests. This test does not start a window or download dependencies.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


LOGGER = """package org.videolan;
public final class Logger {
    public static Logger getLogger(String name) { return new Logger(); }
    public void error(String message) { throw new AssertionError(message); }
}
"""
PORTING_HELPER = """package org.videolan;
public final class PortingHelper {
    public static void stopThread(Thread thread) {
        throw new AssertionError("Unexpected forced thread shutdown");
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--java-home", type=Path)
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    java = str(args.java_home / "bin" / ("java" + suffix)) if args.java_home else "java"
    javac = str(args.java_home / "bin" / ("javac" + suffix)) if args.java_home else "javac"
    for tool in (java, javac):
        version = subprocess.run([tool, "-version"], capture_output=True, text=True, check=True)
        if "1.8." not in version.stdout + version.stderr:
            parser.error("BD-J AWT regressions require JDK 8; pass --java-home")
    helper = args.source / "src/libbluray/bdj/java/java/awt/BDJHelper.java"
    fixture = Path(__file__).resolve().with_name("BdjPointerTest.java")
    with tempfile.TemporaryDirectory(prefix="bdj-pointer-") as directory:
        work = Path(directory)
        shutil.copyfile(helper, work / "BDJHelper.java")
        shutil.copyfile(fixture, work / "BdjPointerTest.java")
        (work / "Logger.java").write_text(LOGGER, encoding="utf-8")
        (work / "PortingHelper.java").write_text(PORTING_HELPER, encoding="utf-8")
        classes = work / "classes"
        classes.mkdir()
        sources = [str(work / name) for name in (
            "BDJHelper.java", "BdjPointerTest.java", "Logger.java", "PortingHelper.java")]
        subprocess.run([javac, "-d", str(classes)] + sources, cwd=work, check=True)
        subprocess.run([java, "-Xbootclasspath/p:" + str(classes),
                        "-Djava.awt.headless=true", "java.awt.BdjPointerTest"],
                       cwd=work, check=True)


if __name__ == "__main__":
    main()
