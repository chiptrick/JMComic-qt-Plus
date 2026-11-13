#!/usr/bin/env python3
# coding:utf-8
""" SRMD(ncnn) → 固定 T=192 ONNX（烘焙 19 通道退化图）

模型与预处理（源码实证，nihui/srmd-ncnn-vulkan）：
- 网络：Input(19) → Conv3x3(128,ReLU) ×1 → Conv3x3(128,ReLU) ×11 → Conv3x3(12) → PixelShuffle(scale)
  （param 里 9=1 表示卷积后接 ReLU；无内部 crop，192 → 192*scale）
- 输入 19 通道 = RGB(3) + degradation_vector(15 个常量) + noise_level/255(1 个)
  常量见 srmd_preproc.comp 的 `degradation_vector[15]`；按 noise_level=0（干净图）烘焙
- 权重布局（实测核对）：权重 fp16（仅首数组带 tag 0x01306B47），偏置 fp32，尾部 48B 填充

用法：python script/export_srmd.py --models <models-srmd 目录> --out android/sr_qnn/models [--verify-ncnn]
"""
import argparse
import os
import struct
import sys

import numpy as np
import torch
import torch.nn as nn

TILE = 192
OPSET = 17
TAG_FP32 = 0x01306B47
NOISE_LEVEL = 0                     # 0 = 干净图；-1 时是 15 通道，这里统一用 0

# 来自 srmd_preproc.comp（按源码顺序，勿改）
DEGRADATION_VECTOR = [
    -1.12360956e-08, -1.36899159e-08, 1.85637958e-02, 2.86066886e-08,
    3.35292965e-02, 8.37272935e-08, -2.54424009e-07, -3.16234976e-02,
    -1.35169253e-02, -1.10466090e-08, 3.84753793e-02, 3.79465739e-08,
    -2.44916752e-01, -8.02213490e-01, -5.40549755e-01,
]


def ReadParam(path):
    with open(path, encoding="utf-8") as f:
        lines = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    return lines[2:]


def ParseConvs(lines):
    convs = []
    for l in lines:
        if not l.startswith("Convolution"):
            continue
        kv = {int(a.split("=")[0]): a.split("=")[1] for a in l.split() if "=" in a}
        convs.append(kv)
    return convs


class BinReader(object):
    """ ncnn bin：权重 fp16（首数组前可能带 tag），偏置 fp32 """

    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.off = 0

    def read_f16(self, n):
        if self.off + 4 <= len(self.data):
            tag = struct.unpack_from("<I", self.data, self.off)[0]
            if tag == TAG_FP32:
                self.off += 4
        arr = np.frombuffer(self.data, dtype="<f2", count=n, offset=self.off).astype("float32")
        self.off += n * 2
        return arr

    def read_f32(self, n):
        arr = np.frombuffer(self.data, dtype="<f4", count=n, offset=self.off).astype("float32")
        self.off += n * 4
        return arr


def BuildModel(convs, reader, scale):
    mods = []
    # 输入通道从首层 weight_data_size 反推：srmd=19(含 noise 平面)，srmdnf=18(只有常量)
    in_c = int(convs[0][6]) // (int(convs[0][0]) * int(convs[0][1]) ** 2)
    for p in convs:
        out_c, k = int(p[0]), int(p[1])
        act = int(p.get(9, 0))
        w = reader.read_f16(out_c * in_c * k * k).reshape(out_c, in_c, k, k)
        b = reader.read_f32(out_c)
        conv = nn.Conv2d(in_c, out_c, k, padding=k // 2, bias=True)
        with torch.no_grad():
            conv.weight.copy_(torch.from_numpy(w))
            conv.bias.copy_(torch.from_numpy(b))
        mods.append(conv)
        if act == 1:
            mods.append(nn.ReLU(inplace=True))
        in_c = out_c
    return nn.Sequential(*mods, nn.PixelShuffle(scale))


class Wrap(nn.Module):
    """ RGB → 18/19 通道（烘焙退化常量，srmd 额外带 noise 平面）→ 网络 """

    def __init__(self, net, n_in):
        super().__init__()
        self.net = net
        self.n_in = n_in
        self.register_buffer("deg", torch.tensor(DEGRADATION_VECTOR).view(1, -1, 1, 1))
        self.register_buffer("noise", torch.full((1, 1, 1, 1), NOISE_LEVEL / 255.0))

    def forward(self, x):
        b, _, h, w = x.shape
        parts = [x, self.deg.expand(b, -1, h, w)]
        if self.n_in >= 19:
            parts.append(self.noise.expand(b, -1, h, w))
        return self.net(torch.cat(parts, dim=1))


def convs_n_in(param):
    """ 首层输入通道（srmd=19 / srmdnf=18） """
    c = ParseConvs(ReadParam(param))[0]
    return int(c[6]) // (int(c[0]) * int(c[1]) ** 2)


def Convert(param, binf, scale, out_path):
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    convs = ParseConvs(ReadParam(param))
    net = BuildModel(convs, BinReader(binf), scale).eval()
    n_in = int(convs[0][6]) // (int(convs[0][0]) * int(convs[0][1]) ** 2)
    model = Wrap(net, n_in).eval()
    dummy = torch.rand(1, 3, TILE, TILE)
    with torch.no_grad():
        ref = model(dummy).numpy()
    torch.onnx.export(model, (dummy,), out_path, input_names=["x"], output_names=["y"],
                      opset_version=OPSET, do_constant_folding=True, dynamic_axes=None)
    import onnxruntime as ort
    sess = ort.InferenceSession(out_path, providers=["CPUExecutionProvider"])
    got = sess.run(None, {"x": dummy.numpy()})[0]
    diff = float(np.abs(got - ref).max())
    print("  -> {:24s} {}  {:.2f} MB  max|ort-torch|={:.2e} ({:.3f}/255) {}".format(
        os.path.basename(out_path), got.shape, os.path.getsize(out_path) / 1048576.0,
        diff, diff * 255, "OK" if diff < 1e-4 else "FAIL"))
    return diff < 1e-4, ref, dummy


def VerifyWithNcnn(param, binf, dummy, ref_rgb, scale, n_in=19):
    """ 用 ncnn 运行时跑同一输入，与 torch 输出比对——验证 bin 解析正确 """
    try:
        import ncnn
    except ImportError:
        print("  [skip] 无 ncnn，跳过真值校验")
        return True
    net = ncnn.Net()
    net.load_param(param)
    net.load_model(binf)
    deg = torch.tensor(DEGRADATION_VECTOR).view(1, -1, 1, 1).expand(1, 15, TILE, TILE)
    parts = [dummy, deg]
    if n_in >= 19:
        parts.append(torch.full((1, 1, TILE, TILE), NOISE_LEVEL / 255.0))
    inp = torch.cat(parts, dim=1).numpy()[0]
    net.opt.num_threads = 4
    ex = net.create_extractor()
    ex.input("input", ncnn.Mat(np.ascontiguousarray(inp)))
    ret, out = ex.extract("output")
    arr = np.array(out).reshape(-1, TILE * scale, TILE * scale)
    d = float(np.abs(arr - ref_rgb[0]).max())
    print("  ncnn vs torch: max|Δ|={:.2e} ({:.3f}/255) {}".format(d, d * 255, "OK" if d < 2e-3 else "DIFF"))
    return d < 2e-3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True)
    ap.add_argument("--out", default="android/sr_qnn/models")
    ap.add_argument("--only", default=None)
    ap.add_argument("--verify-ncnn", action="store_true")
    args = ap.parse_args()

    jobs = []
    for scale in (2, 3, 4):
        jobs.append(("srmd_x%d" % scale, scale, "srmd_x%d.onnx" % scale))
        jobs.append(("srmdnf_x%d" % scale, scale, "srmdnf_x%d.onnx" % scale))
    bad = 0
    for stem, scale, out_name in jobs:
        if args.only and args.only not in stem:
            continue
        param = os.path.join(args.models, stem + ".param")
        binf = os.path.join(args.models, stem + ".bin")
        if not (os.path.isfile(param) and os.path.isfile(binf)):
            print("  [skip] 缺文件", stem)
            continue
        print("[{}]".format(stem))
        ok, ref, dummy = Convert(param, binf, scale, os.path.join(args.out, out_name))
        if not ok:
            bad += 1
        if args.verify_ncnn:
            n_in = int(convs_n_in(param))
            VerifyWithNcnn(param, binf, dummy, ref, scale, n_in)
    print("[{}] 完成，FAIL={}".format("OK" if bad == 0 else "FAIL", bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
