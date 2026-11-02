#!/usr/bin/env python3
# coding:utf-8
""" 生成 sr 模型清单（A3a）：models.txt + *.onnx -> src/config/sr_models_manifest.json

为什么需要这份清单：
- 模型不再随 APK 分发（199MB 移出），运行时要靠它知道"有哪些模型、多大、sha256 多少"；
- 清单本身只有几 KB，随源码进 private.tar，离线时也能列出"未安装"的模型（只灰色不可下载）；
- 下载完成后用它做完整性校验（sha256），避免半包/被篡改的 .onnx 让 QNN 崩在底层。

用法：
    python script/gen_model_manifest.py \
        --models android/sr_qnn/models \
        --base-url https://github.com/<owner>/<repo>/releases/download/models-v1/ \
        --out src/config/sr_models_manifest.json

base_url 约定：Release 里文件名与本地一致（basename），下载时直接 base_url + file。
"""
import argparse
import hashlib
import json
import os
import sys
import time

TEXT_SUFFIX = (".txt", ".json")

# 模型来源（provenance）。2026-11-02 逐个用 GitHub API 核对过：
#   - nagadomi/waifu2x v0.13.2：release 无任何资产，权重靠仓库里的 download_models.sh 外取；
#   - xinntao/Real-ESRGAN v0.3.0：无资产；v0.2.5.0 挂的是 .pth（PyTorch 权重），不是 ONNX；
#   - bilibili/ailab（Real-CUGAN）：release 是 Windows 整合包 zip（853MB + 2.1GB 分卷），权重在包内。
# 结论：**这三家都不能当 base_url** —— 它们不提供我们这种固定 shape 的 ONNX。
# 它们只作为"权重来源/出处"记录在清单里，供复现与致谢；运行时下载仍指向我们自己的 Release。
SOURCES = [
    {
        "family": "waifu2x",
        "repo": "https://github.com/nagadomi/waifu2x",
        "license": "MIT",
        "release_tag": "v0.13.2",
        "assets": "none",
        "weights": "仓库内 download_models.sh 从外部拉取（非 release 资产）",
        "usable_as_base_url": False,
        "note": "我们的 cunet 系列由 waifu2x 权重导出并固化为 T=192",
    },
    {
        "family": "real-esrgan",
        "repo": "https://github.com/xinntao/Real-ESRGAN",
        "license": "BSD-3-Clause",
        "release_tag": "v0.3.0",
        "assets": "v0.3.0 无资产；v0.2.5.0 提供 .pth",
        "weights": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth",
        "usable_as_base_url": False,
        "note": "PyTorch 权重，需本地 torch.onnx.export 后才是可用的 ONNX",
    },
    {
        "family": "real-cugan",
        "repo": "https://github.com/bilibili/ailab",
        "license": "MIT (见仓库 LICENSE)",
        "release_tag": "Real-CUGAN-add-low-memory-mode",
        "assets": "RealCUGAN_for_win10_torch1.10.0cu111_20220226.zip.001/.002（整合包）",
        "weights": "权重在整合包 zip 内，无独立直链",
        "usable_as_base_url": False,
        "note": "需要解包后取 .pth 再导出 ONNX",
    },
]



def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_models_txt(path):
    """ models.txt: <name> <onnx file> <scale> <denoise> <tile>，# 开头是注释 """
    out = []
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            item = {"name": parts[0], "file": parts[1]}
            if len(parts) >= 5:
                try:
                    item.update(scale=int(parts[2]), denoise=int(parts[3]), tile=int(parts[4]))
                except ValueError:
                    pass
            out.append(item)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="android/sr_qnn/models")
    ap.add_argument("--base-url", action="append", default=[],
                    help="可重复：运行时按顺序尝试的下载源；第一个为主源")
    ap.add_argument("--out", default="src/config/sr_models_manifest.json")
    args = ap.parse_args()

    if not os.path.isdir(args.models):
        print("[fail] 模型目录不存在: {}".format(args.models), file=sys.stderr)
        return 2
    providers = [u for u in (args.base_url or []) if u]
    if not providers:
        print("[warn] 未给 --base-url：base_url/providers 留空，运行时视为'不可下载'（只列已装）",
              file=sys.stderr)

    entries = parse_models_txt(os.path.join(args.models, "models.txt"))
    seen = set()
    models, texts = [], []
    for e in entries:
        f = e["file"]
        p = os.path.join(args.models, f)
        if f in seen:
            continue
        seen.add(f)
        if not os.path.isfile(p):
            print("[warn] 清单里有、文件不存在: {}（跳过）".format(f), file=sys.stderr)
            continue
        e["size"] = os.path.getsize(p)
        e["sha256"] = sha256_of(p)
        models.append(e)

    # models.txt 本身也登记（下载器不用它，但校验时能发现"清单与模型不配套"）
    mt = os.path.join(args.models, "models.txt")
    if os.path.isfile(mt):
        texts.append({"file": "models.txt", "size": os.path.getsize(mt), "sha256": sha256_of(mt)})

    doc = {
        "version": 2,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        # base_url = 主源（兼容旧读取方）；providers = 按顺序尝试的全部源
        "base_url": providers[0] if providers else "",
        "providers": providers,
        # 模型族出处（官方仓库）；它们**不能**当下载源，仅用于复现与致谢
        "sources": SOURCES,
        "total_bytes": sum(m["size"] for m in models),
        "models": models,
        "texts": texts,
    }
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")

    print("写出 {}：{} 个模型，合计 {:.1f} MB".format(
        args.out, len(models), doc["total_bytes"] / 1048576.0))
    print("Release 上传（在模型目录执行）：")
    print("  gh release create models-v1 --title 'SR models v1' --notes 'waifu2x/realcugan ONNX (fixed-shape)'")
    for m in models:
        print("  gh release upload models-v1 {}".format(m["file"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
