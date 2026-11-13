#!/usr/bin/env python3
# coding:utf-8
""" 用官方 SE 权重前向，与仓库现有 11 个 ONNX 输出对账（证明模型确为官方权重导出）。

只做数值比对，不导出：同一随机输入 → torch 前向（官方数学） vs ONNX 推理 → max|Δ|。
"""
import os
import sys
import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, r"D:\jm-cugan\src")
from upcunet_v3 import UpCunet2x, UpCunet3x, UpCunet4x  # noqa: E402

TILE = 192
W = r"D:\jm-cugan\extracted\RealCUGAN_for_win10_torch1.10.0cu111_20220226\weights_v3"
MODELS = r"C:\Users\WQ159\source\repos\JMComic-qt\android\sr_qnn\models"

JOBS = [
    ("up2x-latest-no-denoise.pth", UpCunet2x, 2, "realcugan_se_up2x.onnx"),
    ("up2x-latest-conservative.pth", UpCunet2x, 2, "realcugan_se_up2x_conservative.onnx"),
    ("up2x-latest-denoise1x.pth", UpCunet2x, 2, "realcugan_se_up2x_denoise1.onnx"),
    ("up2x-latest-denoise2x.pth", UpCunet2x, 2, "realcugan_se_up2x_denoise2.onnx"),
    ("up2x-latest-denoise3x.pth", UpCunet2x, 2, "realcugan_se_up2x_denoise3.onnx"),
    ("up3x-latest-no-denoise.pth", UpCunet3x, 3, "realcugan_se_up3x.onnx"),
    ("up3x-latest-conservative.pth", UpCunet3x, 3, "realcugan_se_up3x_conservative.onnx"),
    ("up3x-latest-denoise3x.pth", UpCunet3x, 3, "realcugan_se_up3x_denoise3.onnx"),
    ("up4x-latest-no-denoise.pth", UpCunet4x, 4, "realcugan_se_up4x.onnx"),
    ("up4x-latest-conservative.pth", UpCunet4x, 4, "realcugan_se_up4x_conservative.onnx"),
    ("up4x-latest-denoise3x.pth", UpCunet4x, 4, "realcugan_se_up4x_denoise3.onnx"),
]


def main():
    import onnxruntime as ort
    torch.manual_seed(0)
    x = torch.rand(1, 3, TILE, TILE) * 0.6 + 0.2
    bad = 0
    for pth, cls, scale, onnx_name in JOBS:
        state = torch.load(r"{}\{}".format(W, pth), map_location="cpu", weights_only=False)
        base = cls(in_channels=3, out_channels=3)
        base.load_state_dict(state, strict=True)
        base.eval()
        with torch.no_grad():
            ref = base(x, tile_mode=0, cache_mode=2, alpha=1, pro=False)
        if ref.dtype != torch.float32:
            ref = ref.float() / 255.0
        ref = ref.numpy()
        sess = ort.InferenceSession(r"{}\{}".format(MODELS, onnx_name),
                                    providers=["CPUExecutionProvider"])
        got = sess.run(None, {"x": x.numpy()})[0]
        if got.shape != ref.shape:
            print("  {:38s} shape {} vs {}  MISMATCH".format(onnx_name, got.shape, ref.shape))
            bad += 1
            continue
        d = float(np.abs(got - ref).max())
        ok = d < 1e-4
        print("  {:38s} max|Δ|={:.2e} ({:.3f}/255) {}".format(onnx_name, d, d * 255, "OK" if ok else "FAIL"))
        if not ok:
            bad += 1
    print("[{}] {} 个模型对账完成，FAIL={}".format("OK" if bad == 0 else "FAIL", len(JOBS), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
