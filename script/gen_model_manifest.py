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
    ap.add_argument("--base-url", default="")
    ap.add_argument("--out", default="src/config/sr_models_manifest.json")
    args = ap.parse_args()

    if not os.path.isdir(args.models):
        print("[fail] 模型目录不存在: {}".format(args.models), file=sys.stderr)
        return 2
    if not args.base_url:
        print("[warn] 未给 --base-url：清单里的 base_url 留空，运行时视为'不可下载'（只列已装）",
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
        "version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "base_url": args.base_url,
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
