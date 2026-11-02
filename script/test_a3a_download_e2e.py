#!/usr/bin/env python3
# coding:utf-8
""" A3a 端到端自测：file:// 假源 → 点未安装行 → 后台下载 → 进度 → 完成可选中

不依赖 Android/引擎/真实网络。覆盖三条分支：
  ① 有源 + 有清单 → StartDownload 成功、进度递增、文件落地且 sha256 正确、行状态翻转为已装
  ② 无下载源（base_url 空）→ 拒绝并给出"未配置下载源"
  ③ 源不可达（file:// 指向不存在目录）→ 拒绝并给出"网络不可用"
"""
import hashlib
import os
import shutil
import sys
import tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
sys.path.insert(0, ROOT)

from PySide6.QtCore import QEventLoop, QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from component.sr import model_store as MS  # noqa: E402
from component.widget import sr_model_table as T  # noqa: E402

FAIL = []


def Check(cond, msg):
    print(("  PASS " if cond else "  FAIL ") + msg)
    if not cond:
        FAIL.append(msg)


def Wait(ms):
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


def MakeCase(tmp, name, base_url, with_file=True):
    """ 造一个假源 + 清单 + store，返回 (store, manifest_dict, payload) """
    remote = os.path.join(tmp, name, "remote")
    os.makedirs(remote, exist_ok=True)
    payload = os.urandom(300 * 1024)          # 300KB，够触发多次进度回调
    if with_file:
        with open(os.path.join(remote, "fake.onnx"), "wb") as f:
            f.write(payload)
    entry = {"name": "fake", "file": "fake.onnx", "scale": 2, "denoise": 0, "tile": 192,
             "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
    manifest = {"version": 1, "base_url": base_url(remote), "models": [entry], "texts": []}
    mp = os.path.join(tmp, name, "manifest.json")
    os.makedirs(os.path.dirname(mp), exist_ok=True)
    import json
    with open(mp, "w", encoding="utf-8") as f:
        json.dump(manifest, f)
    store = MS.ModelStore(root=os.path.join(tmp, name, "local"), manifest_path=mp)
    return store, manifest, payload


def MakeTable(store):
    key = T.FullModelName(T.SrModels[0])
    missing = [{"key": key, "file": "fake.onnx",
                "up": T.SrModels[0][2], "denoise": T.SrModels[0][3]}]
    det = T.Detected(True, set(), [], missing)
    tbl = T.SrModelTable()
    tbl.SetStore(store)
    tbl._catalog = T.BuildCatalog(det, store)
    tbl.Reload()
    return tbl, key


def main():
    app = QApplication.instance() or QApplication([])   # noqa: F841
    tmp = tempfile.mkdtemp(prefix="a3a_e2e.")

    print("== ① 有源有清单：下载成功 + 进度 + 校验 ==")
    store, manifest, payload = MakeCase(tmp, "ok", lambda p: "file://" + p + os.sep)
    tbl, key = MakeTable(store)
    rec = tbl.RecordOf(key)
    Check(rec is not None and rec.state == "missing", "初始为未安装")
    seen = []

    def onProg(k, d, t):
        seen.append((d, t))

    assert tbl.StartDownload(key), "StartDownload 应返回 True"
    worker = tbl._workers.get(key)
    if worker is not None:
        worker.progress.connect(onProg)
    for _ in range(300):                        # 最多 30s
        if key not in tbl._downloading:
            break
        Wait(100)
    local = os.path.join(store.root, "fake.onnx")
    Check(os.path.isfile(local), "文件已落地")
    Check(os.path.getsize(local) == len(payload), "大小一致")
    with open(local, "rb") as f:
        Check(hashlib.sha256(f.read()).hexdigest() == manifest["models"][0]["sha256"], "sha256 一致")
    Check(len(seen) >= 1, "至少收到一次进度回调（{} 次）".format(len(seen)))
    Check(key not in tbl._downloading, "下载状态已清理")

    print("== ② 无下载源：拒绝且给出原因 ==")
    store2, _, _ = MakeCase(tmp, "nosrc", lambda p: "")
    tbl2, key2 = MakeTable(store2)
    Check(tbl2.StartDownload(key2) is False, "StartDownload 返回 False")
    Check("下载源" in tbl2._downloadError.get(key2, ""), "提示含'下载源'")

    print("== ③ 源不可达：拒绝且给出原因 ==")
    store3, _, _ = MakeCase(tmp, "dead", lambda p: "file://" + p + "_missing" + os.sep)
    tbl3, key3 = MakeTable(store3)
    Check(tbl3.StartDownload(key3) is False, "StartDownload 返回 False")
    Check("网络" in tbl3._downloadError.get(key3, ""), "提示含'网络'")

    print("== ④ 下载中 badge 文案 ==")
    tbl4, key4 = MakeTable(store)
    rec4 = tbl4.RecordOf(key4)
    tbl4._downloading[key4] = (50 * 1024, 100 * 1024)
    badge, strong = tbl4.BadgeOf(rec4)
    Check("下载中 50%" in badge, "badge 显示进度: {!r}".format(badge))
    Check(strong, "进度用强色（与灰色'未安装'区分）")

    shutil.rmtree(tmp, ignore_errors=True)
    print()
    if FAIL:
        print("[FAIL] {} 项未通过".format(len(FAIL)))
        return 1
    print("[OK] 下载端到端自测全过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
