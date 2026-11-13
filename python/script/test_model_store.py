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

    print("== 7. 多源 fallback：主源坏 → 自动换备源 ==")
    import json as _json
    mp2 = os.path.join(tmp, "manifest2.json")
    doc2 = dict(manifest)
    doc2["version"] = 2
    doc2["base_url"] = "file://" + os.path.join(tmp, "nope") + "/"
    doc2["providers"] = [doc2["base_url"], "file://" + remote + "/"]
    with open(mp2, "w", encoding="utf-8") as f:
        _json.dump(doc2, f)
    st4 = ModelStore(root=os.path.join(tmp, "m4"), manifest_path=mp2)
    Check(len(st4.Providers()) == 2, "Providers() 读到两个源")
    got4 = st4.Download(doc2["models"][0])
    Check(open(got4, "rb").read() == blob, "主源不可用时从备源下载成功")

    print("== 8. 量化版下载（manifest.quantized） ==")
    mp3 = os.path.join(tmp, "manifest3.json")
    qblob = b"quantized-weights" * 200
    qsrc = os.path.join(remote, "a_q16.onnx")
    with open(qsrc, "wb") as f:
        f.write(qblob)
    doc3 = dict(manifest)
    doc3["version"] = 3
    doc3["providers"] = ["file://" + remote + "/"]
    doc3["quantized"] = [{"base": "a.onnx", "file": "a_q16.onnx", "size": len(qblob),
                          "sha256": hashlib.sha256(qblob).hexdigest()}]
    with open(mp3, "w", encoding="utf-8") as f:
        _json.dump(doc3, f)
    st5 = ModelStore(root=os.path.join(tmp, "m5"), manifest_path=mp3)
    Check(st5.QuantizedFor("a.onnx") is not None, "QuantizedFor 命中")
    Check(st5.QuantizedFor("b.onnx") is None, "未登记时返回 None")
    qp = st5.DownloadQuantized("a.onnx")
    Check(open(qp, "rb").read() == qblob, "量化版下载并校验通过")
    Check(not any(item["installed"] and item["entry"]["file"] == "a_q16.onnx"
                  for item in st5.List()), "量化版不进入模型列表")

    print("== 9. Vulkan(ncnn) 双文件下载与注册 ==")
    vk_remote = os.path.join(tmp, "vk_remote")
    os.makedirs(vk_remote)
    param_blob = b"7767517\n2 2\n" * 20
    bin_blob = b"ncnn-weights" * 400
    with open(os.path.join(vk_remote, "tiny.param"), "wb") as f:
        f.write(param_blob)
    with open(os.path.join(vk_remote, "tiny.bin"), "wb") as f:
        f.write(bin_blob)
    doc3["ncnn"] = [{
        "name": "SRMD_UP2X", "param": "tiny.param", "bin": "tiny.bin",
        "scale": 2, "tile": 192, "in_ch": 19,
        "param_size": len(param_blob), "param_sha256": hashlib.sha256(param_blob).hexdigest(),
        "bin_size": len(bin_blob), "bin_sha256": hashlib.sha256(bin_blob).hexdigest(),
        # 与 ONNX 的 remote 不同目录，验证条目级 providers 生效
        "providers": ["file://" + vk_remote + "/"],
    }]
    with open(mp3, "w", encoding="utf-8") as f:
        _json.dump(doc3, f)
    st6 = ModelStore(root=os.path.join(tmp, "m6"), manifest_path=mp3)
    st6.SyncVkRegistry()
    Check(st6.InstalledNcnn() == [], "未下载时 ncnn 注册表为空")
    vk_bin = st6.DownloadNcnn("SRMD_UP2X")
    Check(open(vk_bin, "rb").read() == bin_blob, "ncnn bin 从独立源下载并校验通过")
    Check(st6.InstalledNcnn() == ["SRMD_UP2X"], "param/bin 齐备后视为已安装")
    registry = open(os.path.join(st6.VkRoot(), "models_vk.txt"), encoding="utf-8").read()
    Check("SRMD_UP2X|tiny.param|tiny.bin|2|192|19" in registry,
          "models_vk.txt 写入完整模型参数")
    Check(not any(item["entry"]["file"] == "tiny.bin" for item in st6.List()),
          "ncnn 模型不混进 ONNX 列表")

    shutil.rmtree(tmp, ignore_errors=True)
    print()
    if FAIL:
        print("[FAIL] {} 项未通过".format(len(FAIL)))
        return 1
    print("[OK] model_store 自检全过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
