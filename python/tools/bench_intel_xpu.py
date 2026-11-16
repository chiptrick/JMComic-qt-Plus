#!/usr/bin/env python3
"""Intel XPU / AI Boost NPU optimization A/B benchmark for the JMComic SR models.

Runs on the local Core Ultra 5 125H (NPU 3720) with OpenVINO 2026.4.1.
Every case uses a fresh ov.Core + freshly read model so compile-side caches
can be controlled explicitly.

Usage (Python 3.14 with openvino / numpy / psutil / nncf / onnxruntime):
    py -3.14 python/tools/bench_intel_xpu.py --group baseline
    py -3.14 python/tools/bench_intel_xpu.py --group all --out result.json

Groups: baseline, cache, compiler, precision, parallel, memory, ops, aot, all
"""
from __future__ import annotations

import argparse
import gc
import json
import os
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
import openvino as ov
import psutil

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MODELS = REPO / "cpp" / "shell" / "ui" / "build" / "sr_models"
DEFAULT_MODEL = "waifu2x_cunet_up2x_denoise3.onnx"
TILE = 192
ITER = 30
WARMUP = 5


def rss_mb() -> float:
    return psutil.Process(os.getpid()).memory_info().rss / 1048576.0


def npu_ddr_mb(core) -> float:
    try:
        return core.get_property("NPU", "NPU_DEVICE_ALLOC_MEM_SIZE") / 1048576.0
    except Exception:
        return -1.0


def make_input(tile: int, seed: int = 7) -> np.ndarray:
    """Structured (non-zero) NCHW float32 tile; all-zero input hides precision loss."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:tile, 0:tile]
    a = np.zeros((1, 3, tile, tile), np.float32)
    a[0, 0] = ((xx * 2 + yy) % 256) / 255.0
    a[0, 1] = ((yy * 3) % 256) / 255.0
    a[0, 2] = ((xx ^ yy) % 256) / 255.0
    a = a * 0.7 + rng.random(a.shape, dtype=np.float32) * 0.15
    return np.ascontiguousarray(a, dtype=np.float32)


def cpu_reference(model_path: str, x: np.ndarray) -> np.ndarray:
    core = ov.Core()
    m = core.read_model(model_path)
    cm = core.compile_model(m, "CPU")
    req = cm.create_infer_request()
    req.infer({0: x})
    return req.get_output_tensor().data.copy()


def accuracy(out: np.ndarray, ref: np.ndarray) -> dict:
    if out.shape != ref.shape:
        return {"maxAbs": None, "psnr": None, "note": f"shape {out.shape} vs {ref.shape}"}
    d = np.abs(out.astype(np.float64) - ref.astype(np.float64))
    mse = float(np.mean((out.astype(np.float64) - ref.astype(np.float64)) ** 2))
    psnr = 99.0 if mse <= 1e-12 else 10.0 * np.log10(1.0 / mse)
    return {"maxAbs": float(d.max()), "meanAbs": float(d.mean()), "psnr": float(psnr)}


def compile_npu(model_path: str, cfg: dict | None = None, cache_dir: str | None = None,
                model: object | None = None):
    core = ov.Core()
    if cache_dir:
        core.set_property({"CACHE_DIR": cache_dir})
    c = dict(cfg or {})
    m = model if model is not None else core.read_model(model_path)
    t = time.perf_counter()
    cm = core.compile_model(m, "NPU", c)
    return core, cm, (time.perf_counter() - t) * 1000.0


def bench_sync(cm, x, mode: str = "zero", n: int = ITER) -> dict:
    """mode='zero' writes into the request's own buffer (no extra copy);
    mode='set' passes a dict to infer() which goes through set_tensor()."""
    req = cm.create_infer_request()
    for _ in range(WARMUP):
        if mode == "zero":
            req.get_input_tensor().data[:] = x
            req.infer()
        else:
            req.infer({0: x})
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        if mode == "zero":
            req.get_input_tensor().data[:] = x
            req.infer()
        else:
            req.infer({0: x})
        ts.append((time.perf_counter() - t) * 1000.0)
    out = req.get_output_tensor().data.copy()
    ts.sort()
    mean = statistics.mean(ts)
    return {"p50_ms": statistics.median(ts), "p95_ms": ts[min(len(ts) - 1, int(len(ts) * 0.95))],
            "mean_ms": mean, "fps": 1000.0 / mean, "out": out}


def bench_async(cm, x, jobs: int, total: int = 120) -> dict:
    q = ov.AsyncInferQueue(cm, jobs)
    for r in q:
        r.get_input_tensor().data[:] = x
    lats: list[float] = []

    def cb(req, _userdata):
        try:
            lats.append(float(req.latency))
        except Exception:
            pass

    q.set_callback(cb)
    for _ in range(jobs * 2):
        q.start_async()
    q.wait_all()
    lats.clear()
    t = time.perf_counter()
    for _ in range(total):
        q.start_async()
    q.wait_all()
    dt = time.perf_counter() - t
    return {"jobs": jobs, "fps": total / dt,
            "p50_req_ms": float(np.median(lats)) if lats else -1.0}


def run_case(name: str, model_path: str, x: np.ndarray, ref: np.ndarray,
             cfg: dict | None = None, model: object | None = None,
             cache_dir: str | None = None, mode: str = "zero",
             n: int = ITER) -> dict:
    gc.collect()
    r0 = rss_mb()
    try:
        core, cm, cms = compile_npu(model_path, cfg, cache_dir, model)
    except Exception as e:
        return {"name": name, "error": f"{type(e).__name__}: {str(e)[:260]}"}
    r1 = rss_mb()
    try:
        b = bench_sync(cm, x, mode=mode, n=n)
    except Exception as e:
        return {"name": name, "compile_ms": cms,
                "error": f"infer {type(e).__name__}: {str(e)[:260]}"}
    acc = accuracy(b["out"], ref)
    ddr = npu_ddr_mb(core)
    row = {"name": name, "compile_ms": round(cms, 1), "rss_delta_mb": round(r1 - r0, 1),
           "npu_ddr_mb": round(ddr, 1) if ddr >= 0 else -1,
           "p50_ms": round(b["p50_ms"], 2), "p95_ms": round(b["p95_ms"], 2),
           "fps": round(b["fps"], 1), **{k: (round(v, 6) if isinstance(v, float) else v)
                                          for k, v in acc.items()}}
    del cm, core
    gc.collect()
    return row


def fmt_row(r: dict) -> str:
    if "error" in r:
        return f"  {r.get('name', '?'):<34} ERROR {r['error']}"
    return (f"  {r['name']:<34} compile={r['compile_ms']:>8.1f}ms "
            f"p50={r['p50_ms']:>7.2f}ms p95={r['p95_ms']:>7.2f}ms "
            f"fps={r['fps']:>7.1f} rss+{r['rss_delta_mb']:>6.1f}MB "
            f"npuDDR={r['npu_ddr_mb']:>7.1f}MB "
            f"maxAbs={r.get('maxAbs')} psnr={r.get('psnr')}")


# ---------------------------------------------------------------- groups ----

def group_baseline(mp, x, ref) -> list:
    print("\n=== baseline (default NPU: LATENCY / FP16 / driver compiler / 2 tiles) ===")
    out = [run_case("baseline", mp, x, ref)]
    # CPU reference for comparison (ORT CPU is slower; OV CPU is a useful second anchor)
    core = ov.Core()
    m = core.read_model(mp)
    cm = core.compile_model(m, "CPU")
    b = bench_sync(cm, x, mode="set")
    out.append({"name": "CPU(OpenVINO) reference", "compile_ms": -1,
                "rss_delta_mb": -1, "npu_ddr_mb": -1,
                "p50_ms": round(b["p50_ms"], 2), "p95_ms": round(b["p95_ms"], 2),
                "fps": round(b["fps"], 1), "maxAbs": 0.0, "psnr": 99.0})
    return out


def group_cache(mp, x, ref) -> list:
    print("\n=== compile / model caching ===")
    out = []
    cd = os.path.join(tempfile.gettempdir(), "jm_ov_cache_bench")
    cases = [
        ("no-cache (bypass UMD)", {"NPU_BYPASS_UMD_CACHING": True}, None),
        ("UMD cache (default)", {}, None),
        ("OV cache cold", {"NPU_BYPASS_UMD_CACHING": True}, cd),
        ("OV cache hot", {"NPU_BYPASS_UMD_CACHING": True}, cd),
    ]
    shutil.rmtree(cd, ignore_errors=True)
    os.makedirs(cd, exist_ok=True)
    for name, cfg, cache_dir in cases:
        r = run_case(name, mp, x, ref, cfg=cfg, cache_dir=cache_dir, n=10)
        out.append(r)
    blobs = [(f, os.path.getsize(os.path.join(cd, f))) for f in os.listdir(cd)]
    out.append({"name": "cache blobs", "files": blobs})
    return out


def group_compiler(mp, x, ref) -> list:
    print("\n=== compiler / scheduling properties ===")
    cases = [
        ("opt-level=0", {"NPU_COMPILATION_MODE_PARAMS": "optimization-level=0",
                         "NPU_BYPASS_UMD_CACHING": True}),
        ("opt-level=1 (default)", {"NPU_COMPILATION_MODE_PARAMS": "optimization-level=1",
                                   "NPU_BYPASS_UMD_CACHING": True}),
        ("opt-level=2", {"NPU_COMPILATION_MODE_PARAMS": "optimization-level=2",
                         "NPU_BYPASS_UMD_CACHING": True}),
        ("hint-override=latency", {"NPU_COMPILATION_MODE_PARAMS": "performance-hint-override=latency",
                                   "NPU_BYPASS_UMD_CACHING": True}),
        ("NPU_TURBO=1", {"NPU_TURBO": True, "NPU_BYPASS_UMD_CACHING": True}),
        ("NPU_TILES=1", {"NPU_TILES": 1, "NPU_BYPASS_UMD_CACHING": True}),
        ("NPU_TILES=2", {"NPU_TILES": 2, "NPU_BYPASS_UMD_CACHING": True}),
        ("compiler=PLUGIN", {"NPU_COMPILER_TYPE": "PLUGIN", "NPU_BYPASS_UMD_CACHING": True}),
        ("PERF_HINT=THROUGHPUT", {"PERFORMANCE_HINT": "THROUGHPUT",
                                  "NPU_BYPASS_UMD_CACHING": True}),
    ]
    return [run_case(n, mp, x, ref, cfg=c, n=20) for n, c in cases]


def group_precision(mp, x, ref) -> list:
    print("\n=== precision / quantization ===")
    out = []
    out.append(run_case("FP16 hint (default)", mp, x, ref,
                        cfg={"INFERENCE_PRECISION_HINT": ov.Type.f16}))
    out.append(run_case("FP32 hint (unsupported?)", mp, x, ref,
                        cfg={"INFERENCE_PRECISION_HINT": ov.Type.f32}))
    try:
        import nncf
        from nncf import CompressWeightsMode, compress_weights

        core = ov.Core()
        base = core.read_model(mp)
        calib = nncf.Dataset([make_input(TILE, seed=100 + i) for i in range(16)])
        t = time.perf_counter()
        q = nncf.quantize(base, calib, subset_size=16)
        qms = (time.perf_counter() - t) * 1000.0
        out.append({"name": "NNCF INT8 PTQ quantize", "note": f"{qms:.0f} ms"})
        out.append(run_case("NNCF INT8 PTQ on NPU", mp, x, ref, model=q,
                            cfg={"NPU_BYPASS_UMD_CACHING": True}))
        del q
        base2 = core.read_model(mp)
        c4 = compress_weights(base2, mode=CompressWeightsMode.INT4_SYM)
        out.append(run_case("NNCF INT4_SYM weights on NPU", mp, x, ref, model=c4,
                            cfg={"NPU_BYPASS_UMD_CACHING": True}))
    except Exception as e:
        out.append({"name": "NNCF", "error": f"{type(e).__name__}: {str(e)[:300]}"})
    return out


def group_parallel(mp, x, ref) -> list:
    print("\n=== parallel / AsyncInferQueue throughput ===")
    out = []
    for hint in ("LATENCY", "THROUGHPUT"):
        try:
            core, cm, cms = compile_npu(mp, {"PERFORMANCE_HINT": hint})
            opt = cm.get_property("OPTIMAL_NUMBER_OF_INFER_REQUESTS")
            for jobs in (1, 2, 4, 8):
                a = bench_async(cm, x, jobs)
                out.append({"name": f"{hint} jobs={jobs}", "compile_ms": round(cms, 1),
                            "optimal_requests": int(opt), **{k: round(v, 2) if isinstance(v, float) else v
                                                              for k, v in a.items()},
                            "p50_ms": round(a["p50_req_ms"], 2), "p95_ms": -1,
                            "fps": round(a["fps"], 1), "rss_delta_mb": -1,
                            "npu_ddr_mb": round(npu_ddr_mb(core), 1),
                            "maxAbs": None, "psnr": None})
            del cm, core
            gc.collect()
        except Exception as e:
            out.append({"name": hint, "error": f"{type(e).__name__}: {str(e)[:200]}"})
    return out


def group_memory(mp, x, ref) -> list:
    print("\n=== memory / data transfer / tile size ===")
    out = []
    out.append(run_case("zero-copy get_tensor", mp, x, ref, mode="zero"))
    out.append(run_case("set_tensor via infer(dict)", mp, x, ref, mode="set"))
    core = ov.Core()
    for tile in (192, 128, 96, 64):
        try:
            xt = make_input(tile)
            m_cpu = core.read_model(mp)
            m_cpu.reshape([1, 3, tile, tile])
            cref = core.compile_model(m_cpu, "CPU")
            rreq = cref.create_infer_request()
            rreq.infer({0: xt})
            rt = rreq.get_output_tensor().data.copy()
            m_npu = core.read_model(mp)
            m_npu.reshape([1, 3, tile, tile])
            out.append(run_case(f"tile={tile}", mp, xt, rt, model=m_npu,
                                cfg={"NPU_BYPASS_UMD_CACHING": True}, n=20))
        except Exception as e:
            out.append({"name": f"tile={tile}", "error": f"{type(e).__name__}: {str(e)[:200]}"})
    return out


def group_ops(mp, x, ref) -> list:
    print("\n=== operator partition & per-layer profiling ===")
    core = ov.Core()
    m = core.read_model(mp)
    try:
        q = core.query_model(m, "NPU")
        from collections import Counter
        split = dict(Counter(str(v) for v in q.values()))
    except Exception as e:
        split = {"error": str(e)[:160]}
    out = [{"name": "query_model partition", "split": split}]
    try:
        cm = core.compile_model(core.read_model(mp), "NPU", {"PERF_COUNT": True,
                                                            "NPU_BYPASS_UMD_CACHING": True})
        req = cm.create_infer_request()
        req.infer({0: x})
        info = req.get_profiling_info()
        rows = []
        for i in info:
            try:
                ms = i.real_time.total_seconds() * 1000.0
            except Exception:
                ms = float(i.real_time) * 1000.0
            rows.append((str(i.node_type), i.node_name, ms))
        rows.sort(key=lambda r: -r[2])
        total = sum(r[2] for r in rows)
        out.append({"name": "per-layer top10", "total_ms": round(total, 2),
                    "layers": [{"type": t, "name": n, "ms": round(v, 3)} for t, n, v in rows[:10]]})
        by_type: dict[str, float] = {}
        for t, _n, v in rows:
            by_type[t] = by_type.get(t, 0.0) + v
        out.append({"name": "cost by op type",
                    "types": sorted([{"type": k, "ms": round(v, 2)} for k, v in by_type.items()],
                                    key=lambda d: -d["ms"])[:10]})
    except Exception as e:
        out.append({"name": "profiling", "error": f"{type(e).__name__}: {str(e)[:200]}"})
    return out


def group_aot(mp, x, ref) -> list:
    """ORT plugin EP: ep.context_enable AOT compile -> session reload."""
    print("\n=== ORT plugin EP context (AOT) ===")
    out = []
    try:
        import onnxruntime as ort
        import onnxruntime_ep_openvino as ovep
        ort.register_execution_provider_library("ovep", ovep.get_library_path())
        devs = [d for d in ort.get_ep_devices()
                if d.ep_name == ovep.get_ep_name() and d.ep_metadata.get("ov_device") == "NPU"]
        if not devs:
            return [{"name": "AOT", "error": "no OpenVINO NPU EP device"}]
        ctx = os.path.join(tempfile.gettempdir(), "jm_ov_context.onnx")
        for f in (ctx, ctx.replace(".onnx", "_OpenVINOExecutionProvider.bin")):
            try:
                os.remove(f)
            except OSError:
                pass

        def so(gen: bool):
            s = ort.SessionOptions()
            s.add_provider_for_devices(devs, {})
            if gen:
                s.add_session_config_entry("ep.context_enable", "1")
                s.add_session_config_entry("ep.context_file_path", ctx)
                s.add_session_config_entry("ep.context_embed_mode", "0")
            return s

        r = []
        for label, path, gen in (("AOT generate (cold)", mp, True),
                                 ("context load (hot)", ctx, False)):
            if not gen and not os.path.exists(ctx):
                r.append({"name": label, "error": "context model not generated"})
                continue
            t = time.perf_counter()
            try:
                sess = ort.InferenceSession(path, so(gen), providers=[])
                ms = (time.perf_counter() - t) * 1000.0
                iname = sess.get_inputs()[0].name
                y = sess.run(None, {iname: x})[0]
                r.append({"name": label, "compile_ms": round(ms, 1), "p50_ms": -1,
                          "p95_ms": -1, "fps": -1, "rss_delta_mb": -1, "npu_ddr_mb": -1,
                          "maxAbs": float(np.max(np.abs(y.astype(np.float64) -
                                                        ref.astype(np.float64)))),
                          "psnr": None})
            except Exception as e:
                r.append({"name": label, "error": f"{type(e).__name__}: {str(e)[:240]}"})
        out = r
    except Exception as e:
        out = [{"name": "AOT", "error": f"{type(e).__name__}: {str(e)[:240]}"}]
    return out


def group_tile_advice(mp, x, ref, scale=2, tiles=(64, 96, 128, 160, 192),
                      advice_out=None, variants_dir=None):
    """Pick the tile with the best *normalized* throughput, not the fastest single tile.

    Uses the exact stitching math from SrEngine::UpscaleEx:
        shrink = max(0, (tile*scale - outEdge)/2)
        pad    = ceil(shrink / scale)
        step   = tile - 2*pad           # effective advance per tile
    so a small tile with large fixed crop can be far slower end-to-end.
    """
    core = ov.Core()
    fp = {}
    for k in ("NPU_DRIVER_VERSION", "NPU_COMPILER_VERSION", "DEVICE_ARCHITECTURE"):
        try:
            fp[k] = str(core.get_property("NPU", k))
        except Exception:
            fp[k] = ""
    fp["openvino"] = ov.get_version()
    base_tile = 192
    rows = []
    for t in tiles:
        try:
            m = core.read_model(mp)
            m.reshape([1, 3, t, t])
            t0 = time.perf_counter()
            cm = core.compile_model(m, "NPU")
            cms = (time.perf_counter() - t0) * 1000.0
            req = cm.create_infer_request()
            xt = make_input(t)
            for _ in range(3):
                req.get_input_tensor().data[:] = xt
                req.infer()
            ts = []
            for _ in range(25):
                t1 = time.perf_counter()
                req.get_input_tensor().data[:] = xt
                req.infer()
                ts.append((time.perf_counter() - t1) * 1000.0)
            out = int(req.get_output_tensor().data.shape[-1])
            p50 = statistics.median(ts)
            fps = 1000.0 / p50
            shrink = max(0, (t * scale - out) // 2)
            pad = (shrink + scale - 1) // scale
            step = t - 2 * pad
            eff = step * scale
            mpx = fps * eff * eff / 1e6
            m_cpu = core.read_model(mp)
            m_cpu.reshape([1, 3, t, t])
            cc = core.compile_model(m_cpu, "CPU")
            cr = cc.create_infer_request()
            cr.infer({0: xt})
            acc = accuracy(req.get_output_tensor().data.copy(),
                           cr.get_output_tensor().data.copy())
            rows.append({"tile": t, "out": out, "shrink": shrink, "pad": pad, "step": step,
                         "p50_ms": round(p50, 3), "fps": round(fps, 1),
                         "mpx_s": round(mpx, 3), "compile_ms": round(cms, 1),
                         "npu_ddr_mb": round(npu_ddr_mb(core), 1),
                         "maxAbs": acc.get("maxAbs"), "psnr": acc.get("psnr")})
            del cm, cc
            gc.collect()
        except Exception as e:
            rows.append({"tile": t, "error": f"{type(e).__name__}: {str(e)[:180]}"})
    valid = [r for r in rows if "mpx_s" in r]
    # Tie-break toward the larger tile: fewer tiles = fewer overlaps and less Python/C++ overhead.
    best = max(valid, key=lambda r: (r["mpx_s"], r["tile"]))["tile"] if valid else base_tile
    variant = ""
    if best != base_tile and variants_dir:
        try:
            import onnx
            mm = onnx.load(mp)
            dims = mm.graph.input[0].type.tensor_type.shape.dim
            dims[2].dim_value = best
            dims[3].dim_value = best
            del mm.graph.value_info[:]          # drop stale shape-inference metadata
            for o in mm.graph.output:
                for dd in o.type.tensor_type.shape.dim:
                    dd.ClearField("dim_value")
                    dd.dim_param = "?"
            stem, ext = Path(mp).stem, Path(mp).suffix
            vp = os.path.join(variants_dir, f"{stem}_t{best}{ext}")
            onnx.save(mm, vp)
            variant = os.path.basename(vp)
        except Exception as e:
            variant = f"ERROR {type(e).__name__}: {str(e)[:160]}"
    name = os.path.basename(mp)
    doc = {"version": 1, "fingerprint": fp, "models": {}}
    if advice_out and os.path.exists(advice_out):
        try:
            doc = json.load(open(advice_out, "r", encoding="utf-8"))
            doc.setdefault("models", {})
        except Exception:
            pass
    doc["version"] = 1
    doc["fingerprint"] = fp
    doc["models"][name] = {"scale": scale, "base": base_tile, "best": best,
                           "variant": variant, "rows": rows}
    if advice_out:
        with open(advice_out, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
    print("  tile  out  shrink  pad  step   p50ms   tiles/s   Mpx/s  DDR(MB)  PSNR")
    for r in rows:
        if "error" in r:
            print(f"  {r['tile']:>4}  ERROR {r['error']}")
        else:
            print(f"  {r['tile']:>4} {r['out']:>4} {r['shrink']:>7} {r['pad']:>4} "
                  f"{r['step']:>5} {r['p50_ms']:>7.2f} {r['fps']:>9.1f} {r['mpx_s']:>8.2f} "
                  f"{r['npu_ddr_mb']:>8.1f} {r['psnr']:>7.1f}")
    print(f"  BEST tile={best} (base={base_tile}) variant={variant or 'none'}")
    print(f"  advice written: {advice_out}")
    return doc["models"][name]


def parse_catalog_scales():
    """Authoritative {file: (scale, arch)} from the C++ kCatalog.

    The engine's UpscaleEx uses spec->scale (an integer), NOT out/tile: cunet
    outputs 312 for a 192 tile but still has scale=2. Guessing from shapes
    gives the wrong shrink/pad/step and badly overestimates small tiles.
    """
    import re
    src = REPO / "cpp" / "shell" / "sr_engine.cpp"
    out = {}
    try:
        text = src.read_text(encoding="utf-8")
    except Exception:
        return out
    m = re.search(r"const Spec kCatalog\[\] = \{(.*?)\n\};", text, re.S)
    if not m:
        return out
    pat = (r'\{\s*"([^"]*)"\s*,\s*"([^"]+\.onnx)"\s*,\s*"([^"]*)"\s*,'
           r'\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,')
    for _name, f, arch, scale, _denoise, _tile in re.findall(pat, m.group(1)):
        out[f] = (int(scale), arch)
    return out


def group_tile_advice_all(advice_out, models_dir, tiles, want_variants=False, limit=0):
    """Calibrate every ONNX model in models_dir into one advice table.

    Writes incrementally so an interrupted run still leaves usable results.
    """
    import re
    core = ov.Core()
    fp = {}
    for k in ("NPU_DRIVER_VERSION", "NPU_COMPILER_VERSION", "DEVICE_ARCHITECTURE"):
        try:
            fp[k] = str(core.get_property("NPU", k))
        except Exception:
            fp[k] = ""
    fp["openvino"] = ov.get_version()
    cat = parse_catalog_scales()
    files = sorted(f for f in os.listdir(models_dir)
                   if f.endswith(".onnx") and not re.search(r"_t\d+\.onnx$", f))
    if limit > 0:
        files = files[:limit]
    doc = {"version": 1, "fingerprint": fp, "models": {}, "summary": {}}
    print(f"calibrating {len(files)} models, tiles={tiles}, variants={want_variants}",
          flush=True)
    t_all = time.perf_counter()
    for idx, f in enumerate(files, 1):
        mp = os.path.join(models_dir, f)
        scale, arch = cat.get(f, (2, "unknown"))
        if f not in cat:
            mm = re.search(r"_up(\d)x", f) or re.search(r"_x(\d)", f)
            scale = int(mm.group(1)) if mm else 2
        quant = "_q16" in f
        rows = []
        for t in tiles:
            try:
                m = core.read_model(mp)
                m.reshape([1, 3, t, t])
                t0 = time.perf_counter()
                cm = core.compile_model(m, "NPU")
                cms = (time.perf_counter() - t0) * 1000.0
                req = cm.create_infer_request()
                xt = make_input(t)
                for _ in range(2):
                    req.get_input_tensor().data[:] = xt
                    req.infer()
                ts = []
                for _ in range(15):
                    t1 = time.perf_counter()
                    req.get_input_tensor().data[:] = xt
                    req.infer()
                    ts.append((time.perf_counter() - t1) * 1000.0)
                out = int(req.get_output_tensor().data.shape[-1])
                p50 = statistics.median(ts)
                fps = 1000.0 / p50
                shrink = max(0, (t * scale - out) // 2)
                pad = (shrink + scale - 1) // scale
                step = t - 2 * pad
                eff = step * scale
                rows.append({"tile": t, "out": out, "shrink": shrink, "pad": pad,
                             "step": step, "p50_ms": round(p50, 3), "fps": round(fps, 1),
                             "mpx_s": round(fps * eff * eff / 1e6, 3),
                             "compile_ms": round(cms, 1)})
                del cm
                gc.collect()
            except Exception as e:
                rows.append({"tile": t, "error": f"{type(e).__name__}: {str(e)[:150]}"})
        valid = [r for r in rows if "mpx_s" in r]
        # QDQ models are not packaged on desktop (CMake filters _q16) and are
        # meant for Qualcomm HTP; mark them non-applicable even if OV can run them.
        applicable = bool(valid) and not quant
        best = max(valid, key=lambda r: (r["mpx_s"], r["tile"]))["tile"] if applicable else 192
        entry = {"scale": scale, "arch": arch, "quant": quant, "base": 192,
                 "best": best, "variant": "", "applicable": applicable, "rows": rows}
        if not applicable:
            entry["note"] = ("QDQ 量化模型，桌面不打包（仅供 QNN HTP）" if quant
                             else "NPU 编译全部失败")
        elif best != 192:
            entry["variant_needed"] = True
            if want_variants:
                try:
                    import onnx
                    mm = onnx.load(mp)
                    dims = mm.graph.input[0].type.tensor_type.shape.dim
                    dims[2].dim_value = best
                    dims[3].dim_value = best
                    del mm.graph.value_info[:]
                    for o in mm.graph.output:
                        for dd in o.type.tensor_type.shape.dim:
                            dd.ClearField("dim_value")
                            dd.dim_param = "?"
                    vp = os.path.join(models_dir, f"{Path(f).stem}_t{best}.onnx")
                    onnx.save(mm, vp)
                    entry["variant"] = os.path.basename(vp)
                except Exception as e:
                    entry["variant"] = f"ERROR {type(e).__name__}: {str(e)[:120]}"
        doc["models"][f] = entry
        top = max((r["mpx_s"] for r in valid), default=-1)
        print(f"[{idx:>2}/{len(files)}] {f:<50} scale={scale} best={best:<4} "
              f"Mpx/s={top:>6.2f} ok={len(valid)}/{len(tiles)} "
              f"{'' if applicable else entry['note']}", flush=True)
        with open(advice_out, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, ensure_ascii=False, indent=2)
    from collections import Counter
    doc["summary"] = {
        "total": len(files),
        "applicable": sum(1 for e in doc["models"].values() if e["applicable"]),
        "quant_skipped": sum(1 for e in doc["models"].values() if e["quant"]),
        "failed": sum(1 for e in doc["models"].values() if not e["applicable"] and not e["quant"]),
        "best_distribution": dict(Counter(str(e["best"]) for e in doc["models"].values())),
        "variant_needed": [f for f, e in doc["models"].items() if e.get("variant_needed")],
        "elapsed_s": round(time.perf_counter() - t_all, 1),
        "tiles": list(tiles),
    }
    with open(advice_out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
    print("\n=== summary ===")
    for k, v in doc["summary"].items():
        print(f"  {k}: {v}")
    print(f"  advice: {advice_out}")
    return doc


def group_tile_finalize(advice_path, models_dir, min_gain=1.15):
    """Apply a significance threshold before the advice table drives the app.

    A 1-10% Mpx/s edge is within run-to-run noise and would cost an extra
    5 MB variant plus more tile overlaps. Keep the measured result in
    `measured_best`/`gain`, but only let `effective_best` differ from 192 when
    the gain clears min_gain. Then generate+verify variants for those only.
    """
    doc = json.load(open(advice_path, "r", encoding="utf-8"))
    sig = []
    for f, e in doc.get("models", {}).items():
        if not e.get("applicable"):
            e.update({"effective_best": 192, "significant": False,
                      "variant_needed": False, "variant": ""})
            continue
        mpx = {r["tile"]: r["mpx_s"] for r in e["rows"] if "mpx_s" in r}
        base = mpx.get(192, 0.0)
        measured = e["best"]
        gain = (mpx.get(measured, 0.0) / base) if base > 0 else 1.0
        significant = measured != 192 and gain >= min_gain
        e.update({"measured_best": measured, "gain": round(gain, 4),
                  "significant": bool(significant),
                  "effective_best": measured if significant else 192,
                  "variant_needed": bool(significant)})
        e.setdefault("variant", "")
        if significant:
            sig.append(f)
    doc.setdefault("summary", {})["min_gain"] = min_gain
    doc["summary"]["measured_non_192"] = [f for f, e in doc["models"].items()
                                          if e.get("measured_best", 192) != 192]
    doc["summary"]["significant_variants"] = sig
    with open(advice_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
    print(f"min_gain={min_gain}  measured_non_192={len(doc['summary']['measured_non_192'])}  "
          f"significant={len(sig)}")
    for f, e in doc["models"].items():
        if e.get("measured_best", 192) != 192:
            print(f"  {f:<50} measured={e['measured_best']:<4} 192={192:<4} "
                  f"gain={e['gain']:.3f}x -> effective={e['effective_best']}")
    if sig:
        group_tile_variants(advice_path, models_dir)
    else:
        print("  no variant generated: all gains below threshold, app keeps 192")
    return doc


def group_tile_variants(advice_path, models_dir, verify=True):
    """Generate ONNX tile variants for every model whose calibrated best != 192.

    Only models that successfully compile on the NPU get their `variant` field
    filled in, so the app never switches to a variant that cannot run.
    """
    import onnx
    doc = json.load(open(advice_path, "r", encoding="utf-8"))
    targets = [(f, e) for f, e in doc.get("models", {}).items()
               if e.get("variant_needed") and e.get("applicable")]
    print(f"variant targets: {len(targets)}")
    core = ov.Core() if verify else None
    made, rejected = [], []
    for f, e in targets:
        mp = os.path.join(models_dir, f)
        best = e["best"]
        vp = os.path.join(models_dir, f"{Path(f).stem}_t{best}.onnx")
        try:
            mm = onnx.load(mp)
            dims = mm.graph.input[0].type.tensor_type.shape.dim
            dims[2].dim_value = best
            dims[3].dim_value = best
            del mm.graph.value_info[:]
            for o in mm.graph.output:
                for dd in o.type.tensor_type.shape.dim:
                    dd.ClearField("dim_value")
                    dd.dim_param = "?"
            onnx.save(mm, vp)
        except Exception as ex:
            e["variant"] = ""
            e["variant_error"] = f"onnx: {type(ex).__name__}: {str(ex)[:120]}"
            rejected.append((f, e["variant_error"]))
            continue
        if verify:
            try:
                m = core.read_model(vp)
                cm = core.compile_model(m, "NPU")
                req = cm.create_infer_request()
                req.infer({0: np.zeros((1, 3, best, best), np.float32)})
                out = req.get_output_tensor().data.shape[-1]
                e["variant_verified"] = True
                e["variant_out"] = int(out)
                del cm
            except Exception as ex:
                e["variant"] = ""
                e["variant_verified"] = False
                e["variant_error"] = f"NPU: {type(ex).__name__}: {str(ex)[:120]}"
                os.remove(vp)
                rejected.append((f, e["variant_error"]))
                continue
        e["variant"] = os.path.basename(vp)
        made.append((f, best, os.path.basename(vp)))
        print(f"  OK   {f:<50} tile={best} -> {e['variant']}")
    for f, why in rejected:
        print(f"  SKIP {f:<50} {why}")
    doc.setdefault("summary", {})["variants_generated"] = len(made)
    doc["summary"]["variants_rejected"] = len(rejected)
    with open(advice_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
    print(f"generated={len(made)} rejected={len(rejected)} advice={advice_path}")


GROUPS = {"baseline": group_baseline, "cache": group_cache, "compiler": group_compiler,
          "precision": group_precision, "parallel": group_parallel, "memory": group_memory,
          "ops": group_ops, "aot": group_aot}


def main() -> int:
    global ITER
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", default="all")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--models-dir", default=str(DEFAULT_MODELS))
    ap.add_argument("--tile", type=int, default=TILE)
    ap.add_argument("--iter", type=int, default=ITER)
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "jm_bench_intel_xpu.json"))
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--tiles", default="64,96,128,160,192")
    ap.add_argument("--advice-out",
                    default=os.path.join(tempfile.gettempdir(), "jm_ov_tile_advice.json"))
    ap.add_argument("--variants-dir", default="")
    ap.add_argument("--variants", action="store_true",
                    help="batch calibration: also generate ONNX tile variants")
    ap.add_argument("--limit", type=int, default=0,
                    help="batch calibration: only first N models (smoke test)")
    ap.add_argument("--min-gain", type=float, default=1.15,
                    help="tile-finalize: min normalized gain over 192 to adopt a smaller tile")
    a = ap.parse_args()
    ITER = a.iter

    mp = os.path.join(a.models_dir, a.model)
    if not os.path.exists(mp):
        print(f"model not found: {mp}", file=sys.stderr)
        return 2
    x = make_input(a.tile)
    ref = cpu_reference(mp, x)
    print(f"model={a.model} tile={a.tile} ref={ref.shape} devices={ov.Core().available_devices}")

    names = list(GROUPS) if a.group == "all" else [a.group]
    results: dict = {}
    if os.path.exists(a.out):
        try:
            results = json.load(open(a.out, "r", encoding="utf-8"))
        except Exception:
            results = {}
    for g in names:
        if g == "tile-advice":
            tiles = tuple(int(v) for v in a.tiles.split(",") if v.strip())
            group_tile_advice(mp, x, ref, a.scale, tiles, a.advice_out,
                              a.variants_dir or os.path.dirname(mp))
            continue
        if g == "tile-advice-all":
            tiles = tuple(int(v) for v in a.tiles.split(",") if v.strip())
            group_tile_advice_all(a.advice_out, a.models_dir, tiles, a.variants,
                                  a.limit)
            continue
        if g == "tile-variants":
            group_tile_variants(a.advice_out, a.models_dir)
            continue
        if g == "tile-finalize":
            group_tile_finalize(a.advice_out, a.models_dir, a.min_gain)
            continue
        if g not in GROUPS:
            print(f"unknown group {g}", file=sys.stderr)
            continue
        rows = GROUPS[g](mp, x, ref)
        results[g] = rows
        for r in rows:
            if "error" in r or "compile_ms" in r:
                print(fmt_row(r))
            else:
                print("  " + json.dumps(r, ensure_ascii=False))
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(results, f, ensure_ascii=False, indent=2)
    print(f"\nwritten: {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
