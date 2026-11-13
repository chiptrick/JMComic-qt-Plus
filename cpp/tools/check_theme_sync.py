# coding: utf-8
"""主题资源一致性检查：cpp/assets/theme 是否与 python/res/theme 同步。

C++ 版不直接引用 Python 目录（两套实现互不干扰），主题 QSS 在 cpp/assets/theme
保留一份副本。这个脚本用来发现"只改了一边"的漂移。

用法：
  python cpp/tools/check_theme_sync.py          # 只检查（默认）
  python cpp/tools/check_theme_sync.py --fix    # 用 Python 侧覆盖 C++ 副本
"""
import argparse
import hashlib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PY_THEME = os.path.join(ROOT, "python", "res", "theme")
CPP_THEME = os.path.join(ROOT, "cpp", "assets", "theme")


def digest(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true", help="用 python/res/theme 覆盖 cpp/assets/theme")
    args = ap.parse_args()

    if not os.path.isdir(PY_THEME) or not os.path.isdir(CPP_THEME):
        print("[x] 主题目录缺失：%s / %s" % (PY_THEME, CPP_THEME))
        return 2

    names = sorted(n for n in os.listdir(PY_THEME) if n.endswith(".qss"))
    missing, drift, same = [], [], []
    for n in names:
        a = os.path.join(PY_THEME, n)
        b = os.path.join(CPP_THEME, n)
        if not os.path.exists(b):
            missing.append(n)
            if args.fix:
                shutil.copy2(a, b)
        elif digest(a) != digest(b):
            drift.append(n)
            if args.fix:
                shutil.copy2(a, b)
        else:
            same.append(n)

    print("主题 QSS：共 %d 个，一致 %d，漂移 %d，缺失 %d" %
          (len(names), len(same), len(drift), len(missing)))
    for n in drift:
        print("  drift  " + n)
    for n in missing:
        print("  missing " + n)

    if args.fix and (drift or missing):
        print("已用 python/res/theme 覆盖 cpp/assets/theme")
        return 0
    return 0 if not (drift or missing) else 1


if __name__ == "__main__":
    sys.exit(main())
