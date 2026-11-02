#!/usr/bin/env python3
# coding:utf-8
""" sr_model_table 的 A3a 改动自检（offscreen，不需要引擎/模型文件）

验证：① missing 在 store=None 时仍被隐藏（旧行为不变）
      ② 传 store（带 manifest）时 missing 变成 state=missing + size 可读
      ③ PickModel 拒绝未安装行
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
sys.path.insert(0, ROOT)

from PySide6.QtWidgets import QApplication  # noqa: E402

from component.widget import sr_model_table as T  # noqa: E402

FAIL = []


def Check(cond, msg):
    print(("  PASS " if cond else "  FAIL ") + msg)
    if not cond:
        FAIL.append(msg)


class FakeStore(object):
    def __init__(self, entries):
        self._m = {"version": 1, "base_url": "", "models": entries}

    def Manifest(self):
        return self._m


def main():
    app = QApplication.instance() or QApplication([])   # noqa: F841
    models = [m for m in T.SrModels][:2]
    k0, k1 = T.FullModelName(models[0]), T.FullModelName(models[1])
    missing = [{"key": k1, "file": "x1.onnx", "up": models[1][2], "denoise": models[1][3]}]
    det = T.Detected(True, {k0}, [], missing)

    print("== ① store=None：missing 隐藏（旧行为） ==")
    cat = T.BuildCatalog(det, None)
    keys = [r.key for r in cat]
    Check(k0 in keys and k1 not in keys, "missing 被隐藏，可用模型保留")

    print("== ② store 带清单：missing 可见且带 size ==")
    store = FakeStore([{"file": "x1.onnx", "size": 1048576 * 12,
                        "sha256": "00" * 32, "name": "x1"}])
    cat = T.BuildCatalog(det, store)
    rec = [r for r in cat if r.key == k1]
    Check(len(rec) == 1, "missing 出现在目录里")
    if rec:
        Check(rec[0].state == "missing", "state=missing")
        Check(rec[0].size == 1048576 * 12, "size 来自清单")

    print("== ③ 表实例：未安装不可选、badge 文案 ==")
    tbl = T.SrModelTable()
    tbl._catalog = cat
    tbl._currentName = k1          # 模拟"设置里记着但文件不在"
    Check(tbl.PickModel(k1) is False, "PickModel 拒绝 missing 行")
    badge, strong = tbl.BadgeOf([r for r in cat if r.key == k1][0])
    Check("未安装" in badge and not strong, "badge 显示未安装且不强色: {!r}".format(badge))
    tbl.Reload()
    Check(tbl.rowCount() > 0, "Reload 不崩、有行")

    print()
    if FAIL:
        print("[FAIL] {} 项".format(len(FAIL)))
        return 1
    print("[OK] sr_model_table A3a 改动自检全过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
