#!/usr/bin/env python3
# coding:utf-8
""" 从官方权重导出固定 shape(T=192) 的 ONNX（Windows 侧跑，需要 torch）

为什么要有这个脚本
------------------
`android/tools/prepare_models.py` 只负责"把已有 ONNX 改固定 shape/量化"，它不产权重。
补齐官方模型（Real-ESRGAN 的 general-x4v3 等）必须走"官方 .pth → torch.onnx.export"，
这一步以前是缺的（见 docs §0A 更新 26 的覆盖核查表）。

支持的族
--------
  realesrgan  官方 .pth → SRVGGNetCompact → ONNX（x4v3 / wdn-x4v3）
  cugan       官方/HF .pth → upcunet_v3 → ONNX（预留）
  waifu2x     HF 现成 ONNX → 固定 shape（预留；需要 onnx 而非 torch）

用法
----
  python script/export_upstream.py realesrgan --out android/sr_qnn/models
  python script/export_upstream.py realesrgan --out <dir> --only wdn

输出命名必须与 C++ 内置表/UI 的约定一致（见 android/sr_qnn/cpp/sr_qnn.cpp BuildBuiltinModels）：
  realesrgan_general_up4x.onnx       (REALESRGAN_GENERAL_UP4X)
  realesrgan_general_wdn_up4x.onnx   (REALESRGAN_GENERAL_WDN_UP4X)
"""
import argparse
import os
import sys
import urllib.request

import torch
import torch.nn as nn
import torch.nn.functional as F

TILE = 192
OPSET = 17

# 官方发布（v0.2.5.0 的资产在 v0.3.0 的 release note 里给出）
REALESRGAN_WEIGHTS = {
    "general": {
        "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth",
        "out": "realesrgan_general_up4x.onnx",
    },
    "wdn": {
        "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-wdn-x4v3.pth",
        "out": "realesrgan_general_wdn_up4x.onnx",
    },
}


class SRVGGNetCompact(nn.Module):
    """ Real-ESRGAN 的轻量超分网络（与 basicsr/realesrgan 实现等价，去依赖后内联）

    结构：Conv → PReLU → (Conv → PReLU) × num_conv → Conv → PixelShuffle，
    输出再与"最近邻上采样的输入"相加（全局残差）。
    """

    def __init__(self, num_in_ch=3, num_out_ch=3, num_feat=64, num_conv=32,
                 upscale=4, act_type="prelu"):
        super().__init__()
        self.upscale = upscale
        self.body = nn.ModuleList()
        self.body.append(nn.Conv2d(num_in_ch, num_feat, 3, 1, 1))
        self.body.append(self._act(act_type, num_feat))
        for _ in range(num_conv):
            self.body.append(nn.Conv2d(num_feat, num_feat, 3, 1, 1))
            self.body.append(self._act(act_type, num_feat))
        self.body.append(nn.Conv2d(num_feat, num_out_ch * upscale * upscale, 3, 1, 1))
        self.upsampler = nn.PixelShuffle(upscale)

    @staticmethod
    def _act(kind, ch):
        if kind == "relu":
            return nn.ReLU(inplace=True)
        if kind == "leakyrelu":
            return nn.LeakyReLU(negative_slope=0.1, inplace=True)
        return nn.PReLU(num_parameters=ch)

    def forward(self, x):
        out = x
        for layer in self.body:
            out = layer(out)
        out = self.upsampler(out)
        base = F.interpolate(x, scale_factor=self.upscale, mode="nearest")
        return out + base


def Download(url, dst):
    if os.path.isfile(dst) and os.path.getsize(dst) > 1024:
        print("  缓存命中: {}".format(dst))
        return dst
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    print("  下载: {}".format(url))
    with urllib.request.urlopen(url, timeout=120) as r, open(dst, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            if total:
                pct = got * 100 // total
                print("\r  {:.1f}/{:.1f} MB ({}%)".format(got / 1048576, total / 1048576, pct),
                      end="", flush=True)
    print()
    return dst


def LoadState(path):
    obj = torch.load(path, map_location="cpu", weights_only=True)
    for key in ("params_ema", "params", "state_dict"):
        if isinstance(obj, dict) and key in obj:
            return obj[key]
    return obj


def ExportRealesrgan(outdir, only=None, cache=None):
    cache = cache or os.path.join(outdir, ".weights")
    os.makedirs(outdir, exist_ok=True)
    ok = 0
    for key, item in REALESRGAN_WEIGHTS.items():
        if only and key != only:
            continue
        print("[{}] {}".format(key, item["url"]))
        pth = Download(item["url"], os.path.join(cache, os.path.basename(item["url"])))
        model = SRVGGNetCompact(num_in_ch=3, num_out_ch=3, num_feat=64,
                                num_conv=32, upscale=4, act_type="prelu")
        state = LoadState(pth)
        missing, unexpected = model.load_state_dict(state, strict=False)
        if missing:
            print("  [warn] missing keys:", missing[:3], "...共", len(missing))
        if unexpected:
            print("  [warn] unexpected keys:", unexpected[:3], "...共", len(unexpected))
        model.eval()

        out = os.path.join(outdir, item["out"])
        dummy = torch.rand(1, 3, TILE, TILE)
        with torch.no_grad():
            ref = model(dummy)
            torch.onnx.export(
                model, (dummy,), out,
                input_names=["input"], output_names=["output"],
                opset_version=OPSET, do_constant_folding=True,
                dynamic_axes=None,          # 固定 shape：QNN/HTP 只认静态图
                export_params=True, verbose=False,
            )

        # 数值等价校验：ORT 与 PyTorch 的差异要求 < 1e-4
        # （20 层卷积 + PReLU 的 fp32 累加差异实测 2e-5 ~ 5e-6；1e-4 相当于 0.025/255 灰度，
        #   远低于 8bit 量化噪声，再严就会把正常浮点噪声判成"实现错误"）
        import numpy as np
        import onnxruntime as ort
        sess = ort.InferenceSession(out, providers=["CPUExecutionProvider"])
        got = sess.run(None, {"input": dummy.numpy()})[0]
        diff = float(np.abs(got - ref.numpy()).max())
        size = os.path.getsize(out)
        print("  -> {}  {:.2f} MB  max|ort-torch|={:.2e} (= {:.3f}/255)  {}".format(
            os.path.basename(out), size / 1048576.0, diff, diff * 255.0,
            "OK" if diff < 1e-4 else "FAIL"))
        if diff >= 1e-4:
            print("[fail] 数值不等价，不要入库")
            return 1
        ok += 1
    print("[OK] 导出 {} 个模型 -> {}".format(ok, outdir))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("family", choices=["realesrgan", "cugan", "waifu2x"])
    ap.add_argument("--out", default="android/sr_qnn/models")
    ap.add_argument("--only", default=None)
    ap.add_argument("--cache", default=None)
    args = ap.parse_args()
    if args.family == "realesrgan":
        return ExportRealesrgan(args.out, args.only, args.cache)
    print("[todo] {} 尚未实现（见脚本头注释的计划）".format(args.family))
    return 2


if __name__ == "__main__":
    sys.exit(main())
