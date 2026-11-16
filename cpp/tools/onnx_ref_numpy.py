#!/usr/bin/env python3
"""NumPy 参考实现：把超分 ONNX 图按 ONNX 语义跑一遍，逐节点打印统计。

用途：Vulkan 后端出数值偏差时，用它定位"第一个发散的节点"——
两个后端打印的行格式一致（tN op dims min/max/mean），可以直接 diff。

只覆盖超分模型实际用到的算子；与 C++ 侧 sr_graph.cpp 的算子表一一对应，
但这里是**照 ONNX 规范**写的，不是照抄 C++，否则错了也发现不了。
"""
import argparse
import struct
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0])
from onnx_ops_inventory import fields, read_varint  # noqa: E402


# ---------------------------------------------------------------- ONNX 读取
def read_tensor(v):
    dims, name, dtype, raw = [], "", 0, b""
    floats, ints = [], []
    for f, w, x in fields(v):
        if f == 1:
            if w == 0:
                dims.append(x)
            else:
                j = 0
                while j < len(x):
                    d, j = read_varint(x, j)
                    dims.append(d)
        elif f == 2 and w == 0:
            dtype = x
        elif f == 4 and w == 2:
            floats.extend(struct.unpack("<%df" % (len(x) // 4), x[: len(x) // 4 * 4]))
        elif f == 7 and w == 2:
            j = 0
            while j < len(x):
                d, j = read_varint(x, j)
                ints.append(d)
        elif f == 8 and w == 2:
            name = x.decode()
        elif f == 9 and w == 2:
            raw = x
    return dims, dtype, raw, floats, ints, name


def tensor_value(t):
    dims, dtype, raw, floats, ints, _ = t
    if dtype == 1:
        if raw:
            a = np.frombuffer(raw[: len(raw) // 4 * 4], dtype=np.float32)
        else:
            a = np.array(floats, dtype=np.float32)
    elif dtype == 7:
        if raw:
            a = np.frombuffer(raw[: len(raw) // 8 * 8], dtype=np.int64)
        else:
            a = np.array(ints, dtype=np.int64)
    elif dtype == 6:
        if raw:
            a = np.frombuffer(raw[: len(raw) // 4 * 4], dtype=np.int32)
        else:
            a = np.array(ints, dtype=np.int32)
    else:
        raise ValueError("dtype %s 不支持" % dtype)
    return a.reshape([int(d) for d in dims] if dims else [])


def node_info(n):
    op, ins, outs, attrs = "", [], [], {}
    for f, w, x in fields(n):
        if f == 1 and w == 2:
            ins.append(x.decode())
        elif f == 2 and w == 2:
            outs.append(x.decode())
        elif f == 4 and w == 2:
            op = x.decode()
        elif f == 5 and w == 2:
            nm, ints, i1, s, fl, tv, fv = "", [], None, "", [], None, None
            for a, b, c in fields(x):
                if a == 1 and b == 2:
                    nm = c.decode()
                elif a == 2 and b == 5:
                    fv = struct.unpack("<f", c)[0]
                elif a == 3 and b == 0:
                    i1 = c
                elif a == 4 and b == 2:
                    s = c.decode()
                elif a == 5 and b == 2:
                    tv = c
                elif a == 7 and b == 2:
                    fl = list(struct.unpack("<%df" % (len(c) // 4), c[: len(c) // 4 * 4]))
                elif a == 8:
                    if b == 0:
                        ints.append(c)
                    else:
                        j = 0
                        while j < len(c):
                            d, j = read_varint(c, j)
                            ints.append(d)
            if s:
                attrs[nm] = s
            elif fl:
                attrs[nm] = fl
            elif ints:
                attrs[nm] = ints
            elif tv is not None:
                attrs[nm] = ("tensor", tv)
            elif fv is not None:
                attrs[nm] = fv
            elif i1 is not None:
                attrs[nm] = i1
    return op, ins, outs, attrs


def load_model(path):
    data = open(path, "rb").read()
    graph = [v for f, w, v in fields(data) if f == 7 and w == 2][0]
    inits, nodes, gin, gout = {}, [], [], []
    for f, w, v in fields(graph):
        if f == 1 and w == 2:
            nodes.append(v)
        elif f == 5 and w == 2:
            t = read_tensor(v)
            inits[t[5]] = t
        elif f == 11 and w == 2:
            for f2, w2, v2 in fields(v):
                if f2 == 1 and w2 == 2:
                    gin.append(v2.decode())
        elif f == 12 and w == 2:
            for f2, w2, v2 in fields(v):
                if f2 == 1 and w2 == 2:
                    gout.append(v2.decode())
    return inits, nodes, [n for n in gin if n not in inits], gout


# ---------------------------------------------------------------- 算子
def o_conv(x, w, b, pads, strides):
    ph0, pw0, ph1, pw1 = pads
    sh, sw = strides
    if ph0 or pw0 or ph1 or pw1:
        x = np.pad(x, ((0, 0), (0, 0), (ph0, ph1), (pw0, pw1)))
    _, C, H, W = x.shape
    OC, IC, KH, KW = w.shape
    win = np.lib.stride_tricks.sliding_window_view(x, (KH, KW), axis=(2, 3))
    win = win[:, :, ::sh, ::sw][:, :, : (H - KH) // sh + 1, : (W - KW) // sw + 1]
    cols = win.transpose(0, 2, 3, 1, 4, 5).reshape(-1, IC * KH * KW)
    out = cols @ w.reshape(OC, -1).T
    if b is not None:
        out = out + b
    OH = (H - KH) // sh + 1
    OW = (W - KW) // sw + 1
    return out.reshape(1, OH, OW, OC).transpose(0, 3, 1, 2)


def o_deconv(x, w, b, pads, strides, out_pad):
    ph0, pw0, ph1, pw1 = pads
    sh, sw = strides
    _, IC, H, W = x.shape
    _, OC, KH, KW = w.shape
    OH = (H - 1) * sh - ph0 - ph1 + KH + out_pad[0]
    OW = (W - 1) * sw - pw0 - pw1 + KW + out_pad[1]
    out = np.zeros((1, OC, OH, OW), dtype=np.float32)
    for ky in range(KH):
        for kx in range(KW):
            # oy = iy*sh - ph0 + ky
            ys = np.arange(H) * sh - ph0 + ky
            xs = np.arange(W) * sw - pw0 + kx
            vy = (ys >= 0) & (ys < OH)
            vx = (xs >= 0) & (xs < OW)
            if not vy.any() or not vx.any():
                continue
            contrib = np.einsum("nchw,co->nohw", x[:, :, vy][:, :, :, vx], w[:, :, ky, kx])
            out[np.ix_([0], np.arange(OC), ys[vy], xs[vx])] += contrib
    if b is not None:
        out = out + b.reshape(1, OC, 1, 1)
    return out


def o_pad(x, pads, mode, value=0.0):
    h0, w0 = pads[2], pads[3]
    h1, w1 = pads[6], pads[7]
    _, C, H, W = x.shape
    OH, OW = H + h0 + h1, W + w0 + w1
    out = np.full((1, C, OH, OW), value, dtype=np.float32)
    # 源与目标的重叠区间
    sy0, dy0 = max(0, -h0), max(0, h0)
    sx0, dx0 = max(0, -w0), max(0, w0)
    sy1 = min(H, OH - h1)
    sx1 = min(W, OW - w1)
    if mode == "constant" or sy1 <= sy0 or sx1 <= sx0:
        out[:, :, dy0:dy0 + (sy1 - sy0), dx0:dx0 + (sx1 - sx0)] = \
            x[:, :, sy0:sy1, sx0:sx1]
        return out
    # reflect：先把源按镜像铺满
    idx_y = np.arange(OH) - h0
    idx_x = np.arange(OW) - w0

    def refl(i, n):
        if n <= 1:
            return np.zeros_like(i)
        p = 2 * (n - 1)
        m = i % p
        return np.where(m < n, m, p - m)

    return x[:, :, refl(idx_y, H)][:, :, :, refl(idx_x, W)]


def o_d2s(x, b):
    _, C, H, W = x.shape
    oc = C // (b * b)
    out = np.zeros((1, oc, H * b, W * b), dtype=np.float32)
    for by in range(b):
        for bx in range(b):
            # ic = oc*b*b + by*b + bx
            sub = x[:, by * b + bx:: b * b] if False else None
            idx = [oc_i * b * b + by * b + bx for oc_i in range(oc)]
            out[:, :, by::b, bx::b] = x[:, idx]
    return out


def o_resize(x, oh, ow, mode, coord):
    _, C, H, W = x.shape
    if mode == "nearest":
        iy = np.minimum((np.arange(oh) * H) // oh, H - 1)
        ix = np.minimum((np.arange(ow) * W) // ow, W - 1)
        return x[:, :, iy][:, :, :, ix]
    fy = (np.arange(oh) + 0.5) * (H / oh) - 0.5
    fx = (np.arange(ow) + 0.5) * (W / ow) - 0.5
    cy = np.clip(fy, 0, H - 1)
    cx = np.clip(fx, 0, W - 1)
    y0 = np.floor(cy).astype(int)
    x0 = np.floor(cx).astype(int)
    y1 = np.minimum(y0 + 1, H - 1)
    x1 = np.minimum(x0 + 1, W - 1)
    ty = (cy - y0)[None, None, :, None]
    tx = (cx - x0)[None, None, None, :]
    v00 = x[:, :, y0][:, :, :, x0]
    v01 = x[:, :, y0][:, :, :, x1]
    v10 = x[:, :, y1][:, :, :, x0]
    v11 = x[:, :, y1][:, :, :, x1]
    return (v00 * (1 - tx) + v01 * tx) * (1 - ty) + (v10 * (1 - tx) + v11 * tx) * ty


# ---------------------------------------------------------------- 执行
def run(path, tile, verbose, dump_values=-1):
    inits, nodes, gin, gout = load_model(path)
    env = {}
    for k, v in inits.items():
        env[k] = tensor_value(v)
    x = (np.arange(3 * tile * tile, dtype=np.uint64) * 37 + 11) % 251
    env[gin[0]] = (x.astype(np.float32) / 251.0).reshape(1, 3, tile, tile)

    stats = []
    for i, n in enumerate(nodes):
        op, ins, outs, at = node_info(n)
        a = [env.get(k) for k in ins]

        if op == "Constant":
            t = at.get("value")
            env[outs[0]] = tensor_value(read_tensor(t[1])) if t and t[0] == "tensor" else np.zeros(1, np.float32)
            continue
        if op == "ConstantOfShape":
            shape = [int(v) if int(v) > 0 else 1 for v in a[0].reshape(-1)]
            val = at.get("value")
            fill = float(tensor_value(read_tensor(val[1])).reshape(-1)[0]) if val and val[0] == "tensor" else 0.0
            env[outs[0]] = np.full(shape, fill, dtype=np.float32)
            continue
        if op == "Equal":
            env[outs[0]] = (a[0] == a[1]).astype(np.float32)
            continue
        if op == "Where":
            env[outs[0]] = np.where(a[0] != 0, a[1], a[2]).astype(np.float32)
            continue
        if op == "Expand":
            env[outs[0]] = np.broadcast_to(a[0], [int(v) for v in a[1].reshape(-1)]).astype(np.float32)
            continue
        if op == "Concat":
            ax = int(at.get("axis", 1))
            env[outs[0]] = np.concatenate(a, axis=ax)
            continue

        if op == "Conv":
            pads = [int(v) for v in at.get("pads", [0, 0, 0, 0])]
            strides = [int(v) for v in at.get("strides", [1, 1])]
            r = o_conv(a[0], a[1], a[2] if len(a) > 2 and a[2] is not None else None, pads,
                       strides)
        elif op == "ConvTranspose":
            pads = [int(v) for v in at.get("pads", [0, 0, 0, 0])]
            strides = [int(v) for v in at.get("strides", [1, 1])]
            opad = [int(v) for v in at.get("output_padding", [0, 0])]
            r = o_deconv(a[0], a[1], a[2] if len(a) > 2 and a[2] is not None else None, pads,
                         strides, opad)
        elif op in ("Relu", "LeakyRelu", "Sigmoid", "Clip", "PRelu"):
            if op == "Relu":
                r = np.maximum(a[0], 0)
            elif op == "LeakyRelu":
                al = float(at.get("alpha", 0.01))
                r = np.where(a[0] >= 0, a[0], a[0] * al)
            elif op == "Sigmoid":
                r = 1.0 / (1.0 + np.exp(-a[0]))
            elif op == "Clip":
                lo = float(at.get("min", -3.4e38))
                hi = float(at.get("max", 3.4e38))
                if len(a) > 1 and a[1] is not None:
                    lo = float(np.reshape(a[1], -1)[0])
                if len(a) > 2 and a[2] is not None:
                    hi = float(np.reshape(a[2], -1)[0])
                r = np.clip(a[0], lo, hi)
            else:
                s = a[1].reshape(-1)
                r = np.where(a[0] >= 0, a[0], a[0] * s.reshape(1, -1, 1, 1))
        elif op in ("Add", "Mul", "Sub", "Div"):
            r = {"Add": a[0] + a[1], "Mul": a[0] * a[1], "Sub": a[0] - a[1],
                 "Div": a[0] / a[1]}[op]
        elif op == "Pad":
            pads = [int(v) for v in at.get("pads", np.reshape(a[1], -1))]
            mode = at.get("mode", "constant")
            val = float(np.reshape(a[2], -1)[0]) if len(a) > 2 and a[2] is not None else 0.0
            r = o_pad(a[0], pads, mode, val)
        elif op == "DepthToSpace":
            r = o_d2s(a[0], int(at.get("blocksize", 2)))
        elif op in ("GlobalAveragePool", "ReduceMean"):
            r = a[0].mean(axis=(2, 3), keepdims=True)
        elif op == "Resize":
            if len(a) > 2 and a[2] is not None and a[2].size == 4:
                sc = np.reshape(a[2], -1)
                oh = int(round(a[0].shape[2] * float(sc[2])))
                ow = int(round(a[0].shape[3] * float(sc[3])))
            else:
                sz = np.reshape(a[3], -1)
                oh, ow = int(sz[2]), int(sz[3])
            r = o_resize(a[0], oh, ow, at.get("mode", "nearest"),
                         at.get("coordinate_transformation_mode", "half_pixel"))
        elif op in ("Identity", "Reshape", "Flatten"):
            r = a[0]
        else:
            raise SystemExit("未实现的算子 %s（节点 %d）" % (op, i))

        r = np.asarray(r, dtype=np.float32)
        env[outs[0]] = r
        if i == dump_values:
            print("VALUES t%d [%s]" % (i, ",".join("%.6g" % v for v in r.reshape(-1)[:16])))
        if verbose:
            print("t%-3d %-16s %-18s %-46s min=%.6g max=%.6g mean=%.6g"
                  % (i, op, "x".join(str(d) for d in r.shape), outs[0][:46],
                     float(r.min()), float(r.max()), float(r.mean())))
    out = env[gout[0]]
    print("ref: %s out=%dx%d min=%.6g max=%.6g mean=%.6g"
          % (path.rsplit("/", 1)[-1].rsplit("\\", 1)[-1], out.shape[3], out.shape[2],
             float(out.min()), float(out.max()), float(out.mean())))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--dump-values", type=int, default=-1)
    args = ap.parse_args()
    run(args.model, args.tile, not args.quiet, args.dump_values)
