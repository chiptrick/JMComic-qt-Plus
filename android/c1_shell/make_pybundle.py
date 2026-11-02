#!/usr/bin/env python3
# coding:utf-8
""" 修 p4a libpybundle.tar.gz 的目录权限。

问题：p4a 打出的 bundle 里目录 mode=644（没有 x 位），Android 应用用户解包后
进不去目录，tar 会报 `can't mkdir: Permission denied`。这里重打包一次：
目录 755、.so 755、其它 644。
用法：python make_pybundle.py <in.tar.gz> <out.tar.gz>
"""
import sys
import tarfile


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with tarfile.open(src, "r:gz") as fin, \
            tarfile.open(dst, "w:gz", compresslevel=6) as fout:
        for m in fin:
            if m.isdir():
                m.mode = 0o755
            elif m.isfile() and m.name.endswith(".so"):
                m.mode = 0o755
            else:
                m.mode = 0o644
            m.uid = m.gid = 0
            m.uname = m.gname = "root"
            if m.isfile():
                fout.addfile(m, fin.extractfile(m))
            else:
                fout.addfile(m)
    print("wrote", dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
