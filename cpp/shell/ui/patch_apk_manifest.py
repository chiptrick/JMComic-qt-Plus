#!/usr/bin/env python3
"""In-place patch of an APK's *binary* AndroidManifest.xml (AXML).

Why: on this host Gradle cannot package (utimensat is denied, so
mergeReleaseJniLibFolders fails with EPERM), but androiddeployqt already
produced a perfectly good APK.  Changing a manifest flag does not require a
Gradle rebuild: AXML is a length-stable binary format, so a boolean attribute
can be flipped by rewriting 4 bytes, and a same-length versionName string by
rewriting its UTF-16 payload.

Used by repack_apk.sh --manifest.  The patched file must be re-aligned and
re-signed afterwards (repack_apk.sh does both).

  python3 patch_apk_manifest.py \
      --in-apk ../../dist/JMComic-ui-0.89-arm64-v8a-quic-debug.apk \
      --out-file /tmp/AndroidManifest.xml \
      --no-predictive-back --version-name 0.90 --version-code 90

Scope guard: only the two attributes named below are touched.  Any structural
mismatch (unexpected chunk, missing attribute, length change) is a hard error,
never a best-effort guess.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zipfile
from pathlib import Path

RES_STRING_POOL_TYPE = 0x0001
RES_XML_START_ELEMENT_TYPE = 0x0102
TYPE_INT_DEC = 0x10
TYPE_STRING = 0x03
TYPE_INT_BOOLEAN = 0x12

BACK_ATTR = "enableOnBackInvokedCallback"
VERSION_ATTR = "versionName"
VERSION_CODE_ATTR = "versionCode"


class Axml:
    def __init__(self, data: bytes):
        self.d = bytearray(data)
        t, hs, sz = struct.unpack_from("<HHI", self.d, 0)
        if t != 0x0003 or hs != 8 or sz != len(self.d):
            raise ValueError("not a standalone ResXMLTree (type/hdr/size mismatch)")
        self.chunks: list[tuple[int, int, int]] = []  # (offset, type, size)
        pos = 8
        while pos + 8 <= len(self.d):
            ct, chs, csz = struct.unpack_from("<HHI", self.d, pos)
            if csz < 8 or pos + csz > len(self.d):
                raise ValueError(f"bad chunk at {pos}: size={csz}")
            self.chunks.append((pos, ct, csz))
            pos += csz
        if pos != len(self.d):
            raise ValueError("trailing bytes after last chunk")

        pools = [c for c in self.chunks if c[1] == RES_STRING_POOL_TYPE]
        if len(pools) != 1:
            raise ValueError("expected exactly one string pool")
        self.pool = pools[0][0]
        self.utf8 = self._pool_flags() & 0x100 != 0
        count, _styles, _flags, self.strings_start, _ = struct.unpack_from(
            "<IIIII", self.d, self.pool + 8
        )
        pool_hs = struct.unpack_from("<H", self.d, self.pool + 2)[0]
        self.count = count
        self.offsets = list(struct.unpack_from(f"<{count}I", self.d, self.pool + pool_hs))
        self.strings = [self._decode(i) for i in range(count)]

    def _pool_flags(self) -> int:
        return struct.unpack_from("<I", self.d, self.pool + 16)[0]

    def _decode(self, idx: int) -> str:
        q = self.pool + self.strings_start + self.offsets[idx]
        if self.utf8:
            n = self.d[q]
            q += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | self.d[q]
                q += 1
            return self.d[q:q + n].decode("utf-8", "replace")
        n = struct.unpack_from("<H", self.d, q)[0]
        q += 2
        if n & 0x8000:
            n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", self.d, q)[0]
            q += 2
        return self.d[q:q + n * 2].decode("utf-16le", "replace")

    def index_of(self, text: str) -> int:
        hits = [i for i, s in enumerate(self.strings) if s == text]
        if len(hits) != 1:
            raise ValueError(f"string {text!r} occurs {len(hits)} times in pool")
        return hits[0]

    def payload_offset(self, idx: int) -> tuple[int, int]:
        """Return (offset of string bytes, byte length) for pool entry idx."""
        q = self.pool + self.strings_start + self.offsets[idx]
        if self.utf8:
            n = self.d[q]
            q += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | self.d[q]
                q += 1
            return q, n
        n = struct.unpack_from("<H", self.d, q)[0]
        q += 2
        if n & 0x8000:
            n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", self.d, q)[0]
            q += 2
        return q, n * 2

    def attributes(self):
        """Yield (data_offset, name_index, raw_value, dtype, value, valuesize)."""
        for off, ct, _sz in self.chunks:
            if ct != RES_XML_START_ELEMENT_TYPE:
                continue
            ext = off + struct.unpack_from("<H", self.d, off + 2)[0]
            astart, asize, acount = struct.unpack_from("<HHH", self.d, ext + 8)
            if asize != 20:
                raise ValueError(f"unexpected attribute size {asize} at {off}")
            for i in range(acount):
                a = ext + astart + i * asize
                _ns, name, raw = struct.unpack_from("<III", self.d, a)
                vsize, _res0, dtype, value = struct.unpack_from("<HBBI", self.d, a + 12)
                yield a + 16, name, raw, dtype, value, vsize

    def set_boolean(self, attr_name: str, wanted: bool) -> int:
        idx = self.index_of(attr_name)
        hits = [a for a in self.attributes() if a[1] == idx]
        if len(hits) != 1:
            raise ValueError(f"attribute {attr_name} occurs {len(hits)} times")
        data_off, _name, _raw, dtype, value, vsize = hits[0]
        if dtype != TYPE_INT_BOOLEAN or vsize != 8:
            raise ValueError(f"{attr_name} is not TYPE_INT_BOOLEAN (dtype={dtype:#x})")
        new = 0xFFFFFFFF if wanted else 0x00000000
        if value == new:
            return 0
        struct.pack_into("<I", self.d, data_off, new)
        return 1

    def set_int_attr(self, attr_name: str, wanted: int) -> int:
        idx = self.index_of(attr_name)
        hits = [a for a in self.attributes() if a[1] == idx]
        if len(hits) != 1:
            raise ValueError(f"attribute {attr_name} occurs {len(hits)} times")
        data_off, _name, _raw, dtype, value, vsize = hits[0]
        if dtype != TYPE_INT_DEC or vsize != 8:
            raise ValueError(f"{attr_name} is not TYPE_INT_DEC (dtype={dtype:#x})")
        if value == wanted:
            return 0
        struct.pack_into("<I", self.d, data_off, wanted)
        return 1

    def set_string_attr(self, attr_name: str, wanted: str) -> int:
        idx = self.index_of(attr_name)
        hits = [a for a in self.attributes() if a[1] == idx]
        if len(hits) != 1:
            raise ValueError(f"attribute {attr_name} occurs {len(hits)} times")
        _off, _name, raw, dtype, value, _vsize = hits[0]
        if dtype != TYPE_STRING or raw != value:
            raise ValueError(f"{attr_name} is not a plain TYPE_STRING attribute")
        payload, size = self.payload_offset(value)
        encoded = wanted.encode("utf-16le") if not self.utf8 else wanted.encode("utf-8")
        if len(encoded) != size:
            raise ValueError(
                f"length-stable replacement required: {self.strings[value]!r} "
                f"({size}B) -> {wanted!r} ({len(encoded)}B)"
            )
        if self.strings[value] == wanted:
            return 0
        self.d[payload:payload + size] = encoded
        # re-decode so the verification below sees the new value
        self.strings[value] = self._decode(value)
        return 1

    def verify(self, back_false: bool, version: str | None, version_code: int | None) -> None:
        fresh = Axml(bytes(self.d))
        if back_false:
            idx = fresh.index_of(BACK_ATTR)
            vals = [a[4] for a in fresh.attributes() if a[1] == idx]
            if vals != [0]:
                raise ValueError(f"verification failed: {BACK_ATTR} = {vals}")
        if version is not None:
            idx = fresh.index_of(VERSION_ATTR)
            vals = [a[4] for a in fresh.attributes() if a[1] == idx]
            if not vals or fresh.strings[vals[0]] != version:
                raise ValueError("verification failed: versionName not updated")
        if version_code is not None:
            idx = fresh.index_of(VERSION_CODE_ATTR)
            vals = [a[4] for a in fresh.attributes() if a[1] == idx]
            if vals != [version_code]:
                raise ValueError(f"verification failed: versionCode = {vals}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in-apk", required=True, help="APK whose AndroidManifest.xml is patched")
    ap.add_argument("--out-file", required=True, help="path to write the patched AXML")
    ap.add_argument("--no-predictive-back", action="store_true",
                    help="set android:enableOnBackInvokedCallback=false (Qt 6.12 / Android 17)")
    ap.add_argument("--version-name", help="new versionName (must be same length as the old one)")
    ap.add_argument("--version-code", type=int, help="new versionCode (INT_DEC attribute)")
    args = ap.parse_args()

    src = Path(args.in_apk)
    if not src.is_file():
        print(f"[x] no such APK: {src}", file=sys.stderr)
        return 2
    with zipfile.ZipFile(src) as zf:
        axml = Axml(zf.read("AndroidManifest.xml"))

    changed = 0
    if args.no_predictive_back:
        n = axml.set_boolean(BACK_ATTR, False)
        changed += n
        print(f"    {BACK_ATTR}: true -> false" if n else
              f"    {BACK_ATTR}: already false")
    if args.version_name:
        old = axml.strings[
            [a[4] for a in axml.attributes() if a[1] == axml.index_of(VERSION_ATTR)][0]
        ]
        n = axml.set_string_attr(VERSION_ATTR, args.version_name)
        changed += n
        print(f"    {VERSION_ATTR}: {old!r} -> {args.version_name!r}" if n else
              f"    {VERSION_ATTR}: already {args.version_name!r}")
    if args.version_code is not None:
        idx = axml.index_of(VERSION_CODE_ATTR)
        old = [a[4] for a in axml.attributes() if a[1] == idx][0]
        n = axml.set_int_attr(VERSION_CODE_ATTR, args.version_code)
        changed += n
        print(f"    {VERSION_CODE_ATTR}: {old} -> {args.version_code}" if n else
              f"    {VERSION_CODE_ATTR}: already {args.version_code}")
    if not changed:
        print("    no change requested (or already in desired state)")

    axml.verify(args.no_predictive_back, args.version_name, args.version_code)
    dst = Path(args.out_file)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(axml.d))
    print(f"==> patched AXML -> {dst} ({len(axml.d)} bytes, {changed} edit(s), verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
