# coding: utf-8
"""APK 16 KB 页对齐门禁（Android 15+ / 16 KB page size 设备）。

两个维度都必须过，缺一不可——系统安装器在 16 KB 设备上弹"应用兼容性提醒"、
甚至 INSTALL_FAILED_INVALID_APK，依据的就是这两条：

  1. ELF PT_LOAD p_align >= 0x4000：lib/<abi>/*.so 的加载段要按 16 KB 对齐
     （NDK r27+ 链接器默认满足；旧 NDK 需 -Wl,-z,max-page-size=16384）。
  2. zip 条目数据偏移 % 16384 == 0：manifest 声明 extractNativeLibs=false 时
     加载器直接从 APK mmap .so，条目必须 STORED 且按页对齐（AGP 与
     zipalign -p 只给 4096，需 zipalign -P 16 或 apk_repack.py 重对齐）。

只依赖 NDK 自带的 llvm-readelf + 标准库 zipfile，不需要设备。

跑法（不带参数时自动选 cpp/dist 下最新的 APK）：
  python cpp/tools/check_ui_apk_16kb.py cpp/dist/JMComic-ui-0.95-vk-arm64-v8a-debug.apk
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or \
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")

PAGE = 16384


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


def DefaultApk():
    dist = os.path.join(ROOT, "cpp", "dist")
    apks = sorted(glob.glob(os.path.join(dist, "*.apk")), key=os.path.getmtime)
    return apks[-1] if apks else ""


def ZipOffsets(apk):
    """每个 lib/**/*.so 条目的 (compress_type, data_offset)。

    data_offset = local header 偏移 + 30 + 文件名长 + extra 长。
    """
    result = {}
    with zipfile.ZipFile(apk) as z, open(apk, "rb") as raw:
        for info in z.infolist():
            if not (info.filename.startswith("lib/") and info.filename.endswith(".so")):
                continue
            raw.seek(info.header_offset)
            lh = raw.read(30)
            if lh[:4] != b"PK\x03\x04":
                result[info.filename] = (info.compress_type, -1)
                continue
            nlen, elen = struct.unpack("<HH", lh[26:30])
            off = info.header_offset + 30 + nlen + elen
            result[info.filename] = (info.compress_type, off)
    return result


def main():
    apk = sys.argv[1] if len(sys.argv) > 1 else DefaultApk()
    readelf = FindReadElf()
    if not readelf:
        print("[x] llvm-readelf not found under NDK (set ANDROID_SDK_ROOT)")
        return 2
    if not apk or not os.path.exists(apk):
        print("[x] apk not found: " + str(apk))
        return 2

    offsets = ZipOffsets(apk)
    bad_zip = []       # (name, reason)
    bad_elf = []       # (name, min_align)
    total = len(offsets)
    print("apk: %s (%d .so)" % (os.path.basename(apk), total))

    with tempfile.TemporaryDirectory() as td, zipfile.ZipFile(apk) as z:
        for name in sorted(offsets):
            method, off = offsets[name]
            # 维度 2：STORED + 页对齐
            if method != zipfile.ZIP_STORED:
                bad_zip.append((name, "compressed (extractNativeLibs=false requires STORED)"))
            elif off < 0 or off % PAGE:
                bad_zip.append((name, "zip offset %d mod 16K = %d"
                                % (off, off % PAGE if off >= 0 else -1)))
            # 维度 1：ELF PT_LOAD p_align
            path = z.extract(name, td)
            # Hexagon DSP 侧的 QNN Skel 库（ELF32、e_machine=164/QDSP6）由 ADSP
            # 加载，不进 ARM 进程；Android 15 的 16KB 要求只针对 ARM64/x86_64
            # ELF，安装器对它们也不做该检查——豁免 ELF 维度，zip 维度保留。
            out = subprocess.run([readelf, "-h", path], capture_output=True, text=True).stdout
            machine = ""
            for line in out.splitlines():
                if line.strip().startswith("Machine:"):
                    machine = line.split(":", 1)[-1].strip()
                    break
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
            if "QDSP6" in machine or "Qualcomm" in machine:
                print("  %-58s zip=%s elf=%s (Hexagon DSP 库，豁免)"
                      % (name, "ok" if (method == zipfile.ZIP_STORED and off >= 0
                                         and off % PAGE == 0) else "BAD", machine))
                continue
            if not aligns or min(aligns) < PAGE:
                bad_elf.append((name, min(aligns) if aligns else 0))
            zip_ok = "ok" if (method == zipfile.ZIP_STORED and off >= 0 and off % PAGE == 0) \
                else "BAD"
            print("  %-58s zip=%s elf=%s" %
                  (name, zip_ok, ",".join("0x%x" % a for a in sorted(set(aligns))) or "?"))

    print("")
    if bad_zip or bad_elf:
        print("FAIL: %d 个 .so 未按 16 KB 页对齐" % (len(bad_zip) + len(bad_elf)))
        for name, why in bad_zip:
            print("  - zip 维度: %s (%s)" % (name, why))
        for name, a in bad_elf:
            print("  - elf 维度: %s (min PT_LOAD align 0x%x)" % (name, a))
        print("修复：zipalign -f -P 16 4（或 python cpp/tools/apk_repack.py）重对齐后重新签名；")
        print("      ELF 维度不过时用 NDK r27+ 链接（或 -Wl,-z,max-page-size=16384）。")
        return 1
    print("OK: %d/%d 个 .so 的 zip 偏移与 ELF PT_LOAD 均满足 16 KB 页对齐" % (total, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
