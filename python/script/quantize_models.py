#!/usr/bin/env python3
# coding:utf-8
""" 批量生成 QNN 友好的量化模型（QDQ：uint16 激活 + uint8 权重）

为什么要这个脚本
----------------
QNN HTP 对量化模型最快（体积也只有 float 的 ~23%），而我们的 39 个 float 模型是给
ORT/QNN 直接吃 float 的。这份脚本把每个 float 模型转成 `<name>_q16.onnx`，
引擎在**检测到 QNN EP** 时会按文件名约定优先加载它（见 sr_qnn.cpp ApplyQuantizedPreference）。

为什么用 q16 而不是 q8
----------------------
实测 waifu2x up2x：q8=34.95dB、q16=37.66dB（float 为基准），两者体积相同（权重都是 uint8）；
HTP 原生支持 uint16 激活，精度优先。

为什么有 lowq 判据
------------------
39 个模型里有 7 个量化后 <35dB（CUGAN pro 系、ESRGAN general 两兄弟、SRMDNF x2/x3）。
这些**不进交付**（改名 `_q16_lowq.onnx` 留档），否则会在 NPU 上悄悄降质。

用法
----
  # 全量（已有 _q16 的会跳过，幂等）
  python script/quantize_models.py --models android/sr_qnn/models --calib <图片目录>

  # 只做某几个
  python script/quantize_models.py --models ... --calib ... --only waifu2x,realcugan_se_up2x

依赖：onnx + onnxruntime（含 QNN 量化配置）+ numpy + pillow
"""
import argparse
import json
import os
import sys
import time

import numpy as np
import onnxruntime as ort
from PIL import Image
from onnxruntime.quantization import CalibrationDataReader, QuantType, quantize
from onnxruntime.quantization.calibrate import CalibrationMethod
from onnxruntime.quantization.execution_providers.qnn import get_qnn_qdq_config, qnn_preprocess_model

TILE = 192
MAX_CALIB = 16
LOWQ_DB = 35.0
SUFFIX = "_q16.onnx"


class Reader(CalibrationDataReader):
    """ 标定读取器：HWC → **NCHW**（漏了转置会直接报维度错，这是原 prepare_models.py 的坑） """

    def __init__(self, src, images):
        self.images = images
        self.index = 0
        self.name = ort.InferenceSession(src, providers=["CPUExecutionProvider"]).get_inputs()[0].name

    def _t(self):
        if self.images:
            im = Image.open(self.images[self.index % len(self.images)]).convert("RGB")
            im = im.resize((TILE, TILE))
            a = np.asarray(im, dtype=np.float32).transpose(2, 0, 1) / 255.0
        else:
            a = np.random.rand(3, TILE, TILE).astype(np.float32)
        return np.expand_dims(a, 0)

    def get_next(self):
        if self.index >= MAX_CALIB:
            return None
        self.index += 1
        return {self.name: self._t()}

    def rewind(self):
        self.index = 0


def Quantize(src, dst, images, act="uint16", per_channel=False):
    """ 生成 QDQ 模型；qnn_preprocess_model 在 ORT 1.31 上对部分模型会抛 metadata 错，失败即跳过 """
    target = src
    pre = dst + ".preproc.onnx"
    try:
        if qnn_preprocess_model(src, pre):
            target = pre
    except Exception as es:                                   # noqa: BLE001
        print("  [warn] qnn_preprocess_model 跳过: {}".format(str(es)[:80]))
    cfg = get_qnn_qdq_config(
        target, Reader(src, images),
        calibrate_method=CalibrationMethod.MinMax,
        activation_type={"uint8": QuantType.QUInt8, "uint16": QuantType.QUInt16}[act],
        weight_type=QuantType.QUInt8,
        per_channel=per_channel,
    )
    quantize(target, dst, cfg)
    if os.path.exists(pre):
        os.remove(pre)


def Run(path, x):
    s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    return s.run(None, {s.get_inputs()[0].name: x})[0]


def Psnr(a, b):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse <= 1e-12 else 10.0 * np.log10(1.0 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="android/sr_qnn/models")
    ap.add_argument("--calib", required=True, help="标定图片目录（建议真实漫画/截图 16+ 张）")
    ap.add_argument("--only", default="", help="逗号分隔的子串过滤")
    ap.add_argument("--report", default="", help="写 JSON 报告路径")
    ap.add_argument("--lowq-db", type=float, default=LOWQ_DB)
    args = ap.parse_args()

    calib = [os.path.join(args.calib, n) for n in sorted(os.listdir(args.calib))
             if n.lower().endswith((".png", ".jpg", ".jpeg", ".webp", ".bmp"))][:MAX_CALIB]
    if not calib:
        print("[fail] 标定目录没有图片: {}".format(args.calib))
        return 2

    filters = [s for s in args.only.split(",") if s]
    names = sorted(n for n in os.listdir(args.models)
                   if n.endswith(".onnx") and not n.endswith(SUFFIX) and "_lowq" not in n)
    if filters:
        names = [n for n in names if any(f in n for f in filters)]
    print("标定 {} 张，待量化 {} 个".format(len(calib), len(names)))

    im = Image.open(calib[0]).convert("RGB").resize((TILE, TILE))
    x = (np.asarray(im, dtype=np.float32) / 255.0).transpose(2, 0, 1)[None]

    results, lowq = [], []
    for i, name in enumerate(names, 1):
        src = os.path.join(args.models, name)
        dst = os.path.join(args.models, name[:-5] + SUFFIX)
        rec = {"model": name, "q16": os.path.basename(dst)}
        t0 = time.time()
        try:
            if not os.path.isfile(dst):
                Quantize(src, dst, calib, act="uint16")
            rec.update(status="ok",
                       float_mb=round(os.path.getsize(src) / 1048576.0, 2),
                       q16_mb=round(os.path.getsize(dst) / 1048576.0, 2),
                       psnr=round(Psnr(Run(src, x), Run(dst, x)), 2),
                       secs=round(time.time() - t0, 1))
            print("  [{:2d}/{}] {:5.2f} -> {:5.2f} MB  PSNR {:5.2f} dB  {:.0f}s  {}".format(
                i, len(names), rec["float_mb"], rec["q16_mb"], rec["psnr"], rec["secs"], name))
            if rec["psnr"] < args.lowq_db:
                # 低精度：改名留档，**不进交付**（引擎只认 _q16.onnx）
                os.replace(dst, dst.replace(SUFFIX, "_q16_lowq.onnx"))
                lowq.append(name)
                print("      ↑ <{}dB，改名 _q16_lowq.onnx，不参与自动优先".format(args.lowq_db))
        except Exception as es:                               # noqa: BLE001
            rec.update(status="fail", error=str(es)[:200])
            print("  [{:2d}/{}] FAIL {}: {}".format(i, len(names), name, str(es)[:120]))
        results.append(rec)

    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump({"results": results, "lowq": lowq}, f, ensure_ascii=False, indent=2)
    ok = [r for r in results if r.get("status") == "ok"]
    avg = sum(r["psnr"] for r in ok) / len(ok) if ok else 0
    print("[完成] 成功 {}/{}，平均 PSNR {:.1f}dB，低精度 {} 个".format(len(ok), len(names), avg, len(lowq)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
