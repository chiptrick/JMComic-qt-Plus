#!/usr/bin/env python3
"""Repack a Qt/Android APK with a freshly built main .so (Windows-host friendly).

Why this exists
---------------
On this host Gradle's `mergeReleaseJniLibFolders` fails with EPERM (utimensat is
denied in the sandbox) and `zipalign.exe` hangs on 300 MB+ APKs.  Compiling and
packaging are independent, so this tool rewrites the APK zip directly:

  * all ``lib/**/*.so`` entries become 16384-byte aligned STORED entries
    (mandatory: the manifest declares ``extractNativeLibs=false``, the loader
    mmaps the .so straight out of the APK; Android 15+ 16 KB-page devices
    require the zip offset to be 16 KB aligned -- AGP/zipalign -p only give
    4096, which trips the "应用兼容性提醒" page-size warning);
  * one entry (by default the main Qt library) is replaced with a new build;
  * everything else is preserved byte-for-byte, including optional QDQ models.

Signing is NOT done here - use build-tools ``apksigner.jar``::

    java -jar <sdk>/build-tools/37.0.0/lib/apksigner.jar sign \\
        --ks ~/.android/debug.keystore --ks-pass pass:android \\
        --key-pass pass:android --ks-key-alias androiddebugkey \\
        --out signed.apk aligned.apk

Usage::

    python cpp/tools/apk_repack.py --base old.apk --so libjmcomic_ui_arm64-v8a.so \\
        --abi arm64-v8a --out unsigned.apk
"""
import argparse
import os
import struct
import sys
import zipfile

# 16 KB page size（Android 15+）。extractNativeLibs=false 时加载器直接从 APK
# mmap .so，条目数据偏移必须按内核页大小对齐；4096 是 AGP/zipalign -p 的历史
# 默认，在 16 KB 设备上会触发"应用兼容性提醒"甚至 INSTALL_FAILED_INVALID_APK。
ALIGN = 16384
ZIPALIGN_EXTRA_ID = 0xD935


def is_old_signature(name):
    """v1 (JAR) signature files from the base APK must be dropped.

    apksigner removes them when signing; if they are left in place, every ZIP
    entry after them shifts by a few bytes and all page-aligned .so offsets
    break (observed: +20 bytes -> INSTALL_FAILED_INVALID_APK on device).
    """
    up = name.upper()
    if not up.startswith("META-INF/"):
        return False
    base = up.rsplit("/", 1)[-1]
    return base == "MANIFEST.MF" or base.endswith((".SF", ".RSA", ".DSA", ".EC"))


def alignment_extra(zout, info, align):
    """Extra field that makes this entry's data start at an ``align`` boundary.

    apksigner pads every DEFLATED entry to 4 bytes with the very same 0xd935
    extra field.  If we do not pre-pad those entries ourselves, signing shifts
    all later local headers (observed +7/+7/+6 = +20 bytes) and every
    page-aligned .so becomes misaligned -> INSTALL_FAILED_INVALID_APK.
    """
    pos = zout.fp.tell()
    header_len = 30 + len(info.filename.encode("utf-8"))
    pad = (-(pos + header_len)) % align
    if pad == 0:
        return b""
    if pad < 4:
        pad += align
    payload = pad - 4
    return struct.pack("<HH", ZIPALIGN_EXTRA_ID, payload) + b"\x00" * payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", help="existing signed APK (resources / Qt libs)")
    ap.add_argument("--out", help="output unsigned APK")
    ap.add_argument("--so", default="", help="new main .so (already stripped)")
    ap.add_argument("--abi", default="arm64-v8a")
    ap.add_argument("--entry", default="", help="zip entry to replace (default lib/<abi>/<so name>)")
    ap.add_argument("--drop-quant", action="store_true",
                    help="drop *_q16*/lowq onnx (old behaviour; off by default so the "
                         "QDQ models stay selectable)")
    ap.add_argument("--add-so", action="append", default=[],
                    help="追加 .so 条目：路径 或 路径:zip条目名（lib/ 下走 STORED+16K 对齐；"
                         "用于把 QNN 运行库打进 base APK，可重复传多个）")
    ap.add_argument("--verify-only", action="store_true",
                    help="only check alignment of an existing (e.g. already signed) APK")
    args = ap.parse_args()

    if args.verify_only:
        if not args.base or not os.path.isfile(args.base):
            sys.exit("[x] --verify-only needs --base pointing to an APK")
        return verify(args.base)
    if not args.base or not args.out:
        sys.exit("[x] --base and --out are required")
    if not os.path.isfile(args.base):
        sys.exit("[x] no such base APK: %s" % args.base)
    new_data = None
    entry = args.entry
    if args.so:
        if not os.path.isfile(args.so):
            sys.exit("[x] no such .so: %s" % args.so)
        new_data = open(args.so, "rb").read()
        if not entry:
            entry = "lib/%s/%s" % (args.abi, os.path.basename(args.so))

    dropped = 0
    replaced = 0
    aligned = 0
    with zipfile.ZipFile(args.base, "r") as zin, \
            zipfile.ZipFile(args.out, "w", allowZip64=True) as zout:
        zout.comment = zin.comment
        for info in zin.infolist():
            name = info.filename
            if is_old_signature(name):
                continue
            if args.drop_quant and name.startswith("assets/sr_models/") and (
                    "_q16" in name or "lowq" in name):
                dropped += 1
                continue
            data = new_data if (entry and name == entry) else zin.read(name)
            if name == entry:
                replaced += 1
            zi = zipfile.ZipInfo(name, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            zi.internal_attr = info.internal_attr
            zi.create_system = info.create_system
            zi.comment = info.comment
            if name.startswith("lib/") and name.endswith(".so"):
                zi.compress_type = zipfile.ZIP_STORED
                zi.extra = alignment_extra(zout, zi, ALIGN)
                aligned += 1
            else:
                # apksigner pads *every* non-lib entry (STORED and DEFLATED alike)
                # to 4 bytes; pre-padding here keeps local header offsets stable.
                zi.extra = alignment_extra(zout, zi, 4)
            zout.writestr(zi, data)

        # 追加外部 .so（QNN 运行库等）：base 里没有的条目，lib/ 下走 STORED+16K 对齐
        added = 0
        existing = set(zin.namelist())
        for item in args.add_so:
            if ":" in item:
                path, add_entry = item.rsplit(":", 1)
            else:
                path = item
                add_entry = "lib/%s/%s" % (args.abi, os.path.basename(item))
            if add_entry in existing:
                sys.exit("[x] --add-so 条目与 base APK 冲突: %s" % add_entry)
            if not os.path.isfile(path):
                sys.exit("[x] --add-so 找不到文件: %s" % path)
            data = open(path, "rb").read()
            zi = zipfile.ZipInfo(add_entry, date_time=(2026, 11, 14, 0, 0, 0))
            if add_entry.startswith("lib/") and add_entry.endswith(".so"):
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = 0o644 << 16
                zi.create_system = 3
                zi.extra = alignment_extra(zout, zi, ALIGN)
                aligned += 1
            else:
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.extra = alignment_extra(zout, zi, 4)
            zout.writestr(zi, data)
            added += 1

    if entry and replaced != 1:
        sys.exit("[x] entry not found in base APK: %s" % entry)
    print("apk-repack: aligned_so=%d replaced=%d dropped_quant=%d added_so=%d unsigned=%.1fMB" %
          (aligned, replaced, dropped, added, os.path.getsize(args.out) / 1048576.0))
    return verify(args.out)


def verify(path):
    """Check every lib/**/*.so is STORED and its *local* data offset is page-aligned."""
    bad = []
    models = 0
    aligned = 0
    with zipfile.ZipFile(path) as z, open(path, "rb") as raw:
        for info in z.infolist():
            if info.filename.startswith("assets/sr_models/") and info.filename.endswith(".onnx"):
                models += 1
            if not (info.filename.startswith("lib/") and info.filename.endswith(".so")):
                continue
            aligned += 1
            if info.compress_type != zipfile.ZIP_STORED:
                bad.append((info.filename, "compressed"))
                continue
            raw.seek(info.header_offset)
            lh = raw.read(30)
            if lh[:4] != b"PK\x03\x04":
                bad.append((info.filename, "bad local header"))
                continue
            nlen, elen = struct.unpack("<HH", lh[26:30])
            off = info.header_offset + 30 + nlen + elen
            if off % ALIGN:
                bad.append((info.filename, "offset=%d mod=%d" % (off, off % ALIGN)))
    print("apk-verify: %s entries=%d aligned_so=%d models=%d size=%.1fMB bad=%d" %
          (os.path.basename(path), len(z.infolist()), aligned, models,
           os.path.getsize(path) / 1048576.0, len(bad)))
    for b in bad[:5]:
        print("  BAD", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
