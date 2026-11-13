# coding: utf-8
"""APK 16 KB 页对齐门禁（Android 15+ / 16 KB page size 设备）。

检查 APK 里 lib/<abi>/*.so 的 PT_LOAD 对齐：Android 16 KB 页要求 p_align >= 0x4000。
只依赖 NDK 自带的 llvm-readelf，不需要设备。

跑法：
  python cpp/tools/check_ui_apk_16kb.py cpp/dist/JMComic-ui-0.88-x86_64-debug.apk
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or \
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")


def FindReadElf():
    # NDK 的 prebuilt 目录按宿主平台命名，且 Windows 才带 .exe。
    # 早期版本只认 windows-x86_64，Linux/macOS 上会误报 "not found"。
    ndk_dir = os.path.join(SDK, "ndk")
    hosts = ("linux-x86_64", "darwin-x86_64", "windows-x86_64")
    names = ("llvm-readelf", "llvm-readelf.exe")
    if os.path.isdir(ndk_dir):
        for ver in sorted(os.listdir(ndk_dir), reverse=True):
            for host in hosts:
                for name in names:
                    exe = os.path.join(ndk_dir, ver, "toolchains", "llvm",
                                       "prebuilt", host, "bin", name)
                    if os.path.exists(exe):
                        return exe
    return shutil.which("llvm-readelf")


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "cpp", "dist", "JMComic-ui-0.88-x86_64-debug.apk")
    readelf = FindReadElf()
    if not readelf:
        print("[x] llvm-readelf not found under NDK")
        return 2
    if not os.path.exists(apk):
        print("[x] apk not found: " + apk)
        return 2

    bad = []
    total = 0
    with tempfile.TemporaryDirectory() as td, zipfile.ZipFile(apk) as z:
        libs = [n for n in z.namelist() if re.match(r"lib/[^/]+/.*\.so$", n)]
        print("apk: %s (%d .so)" % (os.path.basename(apk), len(libs)))
        for name in libs:
            path = z.extract(name, td)
            out = subprocess.run([readelf, "-l", path], capture_output=True, text=True).stdout
            aligns = []
            for line in out.splitlines():
                if line.strip().startswith("LOAD"):
                    parts = line.split()
                    if len(parts) >= 7:
                        try:
                            aligns.append(int(parts[-1], 16))
                        except ValueError:
                            pass
            total += 1
            if not aligns or min(aligns) < 0x4000:
                bad.append((name, min(aligns) if aligns else 0))
            print("  %-58s LOAD align=%s" %
                  (name, ",".join("0x%x" % a for a in sorted(set(aligns))) or "?"))

    print("")
    if bad:
        print("FAIL: %d/%d 个 .so 未按 16 KB 对齐" % (len(bad), total))
        for name, a in bad:
            print("  - %s (min align 0x%x)" % (name, a))
        return 1
    print("OK: %d/%d 个 .so 的 PT_LOAD 均 >= 0x4000（16 KB 页兼容）" % (total, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
