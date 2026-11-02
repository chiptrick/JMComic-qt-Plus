#!/usr/bin/env python3
# coding:utf-8
""" model_store 的宿主自检（不需要 Android/PySide6）

覆盖四条真实分支：已装 / 缺失 / 损坏（大小不符）/ sha256 校验 + 断点续传 + 离线判定。
用法：python script/test_model_store.py
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from component.sr.model_store import (  # noqa: E402
    ModelStore, ModelOffline, STATE_INSTALLED, STATE_MISSING, STATE_CORRUPT)

FAIL = []


def Check(cond, msg):
    print(("  PASS " if cond else "  FAIL ") + msg)
    if not cond:
        FAIL.append(msg)


def main():
    tmp = tempfile.mkdtemp(prefix="srstore.")
    root = os.path.join(tmp, "models")
    os.makedirs(root)
    blob = b"onnx-fake-weights" * 1000
    src = os.path.join(tmp, "a.onnx")
    with open(src, "wb") as f:
        f.write(blob)
    digest = hashlib.sha256(blob).hexdigest()
    manifest = {
        "version": 1,
        "base_url": "",
        "models": [
            {"name": "a", "file": "a.onnx", "scale": 2, "denoise": 0, "tile": 192,
             "size": len(blob), "sha256": digest},
            {"name": "b", "file": "b.onnx", "scale": 4, "denoise": 1, "tile": 192,
             "size": 10, "sha256": "00" * 32},
        ],
    }
    mp = os.path.join(tmp, "manifest.json")
    with open(mp, "w", encoding="utf-8") as f:
        json.dump(manifest, f)

    st = ModelStore(root=root, manifest_path=mp)

    print("== 1. 缺失态 ==")
    l = st.List()
    Check(l[0]["state"] == STATE_MISSING, "未放文件时 state=missing")
    Check(not st.CanDownload(), "base_url 为空 → CanDownload()=False（UI 应置灰）")
    Check(not st.Probe(), "无源时 Probe()=False")

    print("== 2. 已装 + 校验 ==")
    shutil.copyfile(src, os.path.join(root, "a.onnx"))
    l = st.List()
    Check(l[0]["state"] == STATE_INSTALLED and l[0]["installed"], "放入后 state=installed")
    Check(st.Verify(l[0]["entry"]), "sha256 校验通过")

    print("== 3. 损坏（大小不符 / 内容被改） ==")
    with open(os.path.join(root, "a.onnx"), "wb") as f:
        f.write(b"short")
    l = st.List()
    Check(l[0]["state"] == STATE_CORRUPT, "大小不符 → state=corrupt（不是 missing）")

    print("== 4. 非法文件名防护 ==")
    bad = {"file": "../evil.onnx"}
    try:
        st.Path(bad)
        Check(False, "../ 路径应被拒绝")
    except ValueError:
        Check(True, "../ 路径被拒绝")

    print("== 5. 断点续传 + 大小/哈希校验（file:// 模拟远端） ==")
    remote = os.path.join(tmp, "remote")
    os.makedirs(remote)
    shutil.copyfile(src, os.path.join(remote, "a.onnx"))
    st2 = ModelStore(root=os.path.join(tmp, "m2"), manifest_path=mp)
    st2.Manifest()["base_url"] = "file://" + remote
    # 预先放一个半包（前半截），验证 Range 续传路径
    os.makedirs(st2.root)
    with open(os.path.join(st2.root, "a.onnx.part"), "wb") as f:
        f.write(blob[:5000])
    got = st2.Download(manifest["models"][0])
    Check(os.path.isfile(got) and open(got, "rb").read() == blob, "半包续传后内容完整")
    Check(not os.path.exists(got + ".part"), ".part 已清理")

    print("== 6. 离线异常类型 ==")
    st3 = ModelStore(root=os.path.join(tmp, "m3"), manifest_path=mp)
    try:
        st3.Download(manifest["models"][1])
        Check(False, "无下载源时应抛 ModelOffline")
    except ModelOffline:
        Check(True, "无下载源抛 ModelOffline（UI 只处理这一种）")

    shutil.rmtree(tmp, ignore_errors=True)
    print()
    if FAIL:
        print("[FAIL] {} 项未通过".format(len(FAIL)))
        return 1
    print("[OK] model_store 自检全过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
