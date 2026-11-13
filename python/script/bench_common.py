# coding:utf-8
"""桌面基准脚本的公共部分：统一 CLI / 统计口径 / RSS 读取 / 输出格式

为什么要有这一层
------------------------------------------------------------------
`docs/qt_cpp_migration_plan.md` 的 §1.4 基线、A1/A2/A3 验收阈值、§6.1"每 PR 不回退
10%" 都要靠 `script/bench_*.py` 这三把尺子。尺子必须**同一口径**：同样的分位数算法、
同样的单位、同样的 RSS 来源，否则"快了 196ms"这种结论没法比、也没法防回退。

统一约定（三个 bench 脚本都遵守，写进各自 --help）：
  * `--runs N`（默认 5）：重复测量次数。统计在 **run 级**做，不把 run 内的样本混进来。
  * `--json`：stdout 只输出**一行** JSON（便于 `| jq` / 存库对比）。
    不给 `--json` 时先打印人类可读摘要，最后一行仍是那行 JSON。
  * `--out PATH`：把 JSON 另存一份（pretty 缩进），用于留档/画趋势。
  * 分位数用 nearest-rank（对已排序样本取 ceil(p*n)-1），**不是插值**；
    n 很小时 p95 基本等于 max —— 要做阈值判定请 `--runs 20` 以上。
  * 时间单位一律毫秒（float），内存一律 MB（float，1024 进制）。
  * RSS 一律取 `/proc/<pid>/status` 的 `VmRSS`。Windows/mac 上无法取到就
    **明确报"平台不支持"并退出码 2**，绝不编一个假数字出来。

退出码：0 正常；1 测量失败（子进程没起来/超时）；2 平台不支持或缺依赖。
"""
import argparse
import json
import math
import os
import random
import statistics
import sys

SCHEMA = "jmcomic-bench/1"
DEFAULT_RUNS = 5


# ------------------------------------------------------------------ CLI
def MakeParser(description, epilog=""):
    """ 统一的参数解析器：三个脚本的公共参数完全一致 """
    parser = argparse.ArgumentParser(
        description=description,
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS,
                        help="重复测量次数（默认 {}）。统计在 run 级做：每个 run 产出 1 个值".format(
                            DEFAULT_RUNS))
    parser.add_argument("--json", action="store_true",
                        help="stdout 只输出一行 JSON（不打印人类可读摘要）")
    parser.add_argument("--out", default="",
                        help="把结果 JSON 另存一份到该路径（pretty 缩进，便于留档对比）")
    parser.add_argument("--src", default="",
                        help="源码根目录（默认仓库的 src/）。测量的是这份代码，不是安装到 site-packages 的")
    parser.add_argument("--python", default=sys.executable,
                        help="子进程用的解释器（默认当前解释器）。必须装有本项目的依赖")
    return parser


def ParseAndCheck(parser):
    """ 解析参数 + 基本校验（runs 必须 >= 1） """
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs 必须 >= 1")
    return args


def SrcDir(args):
    """ 源码根目录：默认 <repo>/src（本文件在 <repo>/script/ 下） """
    if args.src:
        return os.path.abspath(args.src)
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", "src"))


def RepoDir(args):
    return os.path.abspath(os.path.join(SrcDir(args), ".."))


# ------------------------------------------------------------------ 统计
def Percentile(sortedValues, p):
    """ nearest-rank 分位数：对已排序样本取第 ceil(p*n) 个（1 基），下标减 1

    与 numpy 默认的线性插值不同 —— 插值会给出"两个样本之间"的假数值，
    在 n=5 时尤其没意义。这里刻意用最朴素的口径，任何人手算都能复现。
    """
    if not sortedValues:
        return 0.0
    n = len(sortedValues)
    rank = max(1, min(n, int(math.ceil(p * n))))
    return float(sortedValues[rank - 1])


def Stats(values, unit="ms"):
    """ 一组 run 级样本 → 统计字典（min/p50/p95/max/mean/stdev） """
    data = [float(v) for v in values]
    if not data:
        return {"n": 0}
    ordered = sorted(data)
    return {
        "n": len(data),
        "unit": unit,
        "min": round(ordered[0], 2),
        "p50": round(Percentile(ordered, 0.50), 2),
        "p95": round(Percentile(ordered, 0.95), 2),
        "max": round(ordered[-1], 2),
        "mean": round(statistics.fmean(ordered), 2),
        # stdev 用样本标准差(n-1)；n=1 时无意义，置 0
        "stdev": round(statistics.stdev(ordered), 2) if len(ordered) > 1 else 0.0,
    }


# ------------------------------------------------------------------ RSS
def RssSupported():
    """ /proc/<pid>/status 是否存在（Linux/WSL 有，Windows/macOS 没有） """
    return os.path.isfile("/proc/self/status")


def RequireRss():
    """ 取不到 RSS 的平台直接退出码 2：宁可"不支持"，也不要静默给 0 或假数字 """
    if RssSupported():
        return
    sys.stderr.write(
        "[unsupported] 取 RSS 需要 Linux 的 /proc/<pid>/status（VmRSS），当前平台不支持。\n"
        "              桌面基准请在有 /proc 的环境里跑：Linux 主机、或 WSL2（推荐）。\n"
        "              Windows 原生 Python 下本脚本不会给出任何内存数字。\n")
    sys.exit(2)


def RssMbOf(pid):
    """ 读 /proc/<pid>/status 的 VmRSS（kB → MB）。取不到返回 None，绝不返回 0 冒充 """
    try:
        with open("/proc/{}/status".format(pid), "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return round(int(line.split()[1]) / 1024.0, 1)
    except Exception:
        return None
    return None


def SelfRssMb():
    return RssMbOf(os.getpid())


# ------------------------------------------------------------------ 输出
def Emit(result, jsonOnly=False, humanLines=None, outPath=""):
    """ 统一输出：最后一行永远是那行 JSON

    :param result: dict，会被写进 JSON（自动补 schema / generated_at）
    :param jsonOnly: True 时只输出 JSON 一行
    :param humanLines: 人类可读摘要行（list[str]）
    :param outPath: 非空时另存一份 pretty JSON
    """
    payload = dict(result)
    payload["schema"] = SCHEMA
    payload["generated_at"] = _NowIso()

    if outPath:
        try:
            parent = os.path.dirname(os.path.abspath(outPath))
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(outPath, "w", encoding="utf-8") as f:
                json.dump(payload, f, ensure_ascii=False, indent=2)
                f.write("\n")
        except Exception as es:
            sys.stderr.write("[warn] 写 --out 失败 {}: {}\n".format(outPath, es))

    if not jsonOnly and humanLines:
        for line in humanLines:
            print(line)
    print(json.dumps(payload, ensure_ascii=False, separators=(", ", ": ")))
    return 0


def _NowIso():
    from datetime import datetime
    try:
        return datetime.now().astimezone().isoformat(timespec="seconds")
    except Exception:
        return datetime.now().isoformat(timespec="seconds")


def EnvInfo(args):
    """ 结果里带上"在什么环境下测的"：换机器/换 Python 的数值不可直接比

    `compare_scope` 是给文档引用时用的护栏：本套脚本默认在 offscreen 平台插件下跑，
    首帧/重绘这类数字**只能同机相对比较**，不能和真机（204~261ms）或原生桌面比。
    """
    import platform
    info = {
        "python": platform.python_version(),
        "interpreter": os.path.abspath(args.python or sys.executable),
        "platform": platform.platform(),
        "cpu_count": os.cpu_count(),
        "qt_platform": os.environ.get("QT_QPA_PLATFORM", ""),
        "compare_scope": "offscreen-relative-only",
    }
    # 机器负载：没有它，同一台机器上的 7254ms 与 5680ms 看起来像"代码变快了"。
    # 实测差 1.5 倍就是背景构建在跑导致的，基线必须能和负载对上号。
    try:
        info["loadavg"] = [round(v, 2) for v in os.getloadavg()]
    except Exception:
        info["loadavg"] = None
    try:
        import PySide6
        info["pyside6"] = getattr(PySide6, "__version__", "")
    except Exception:
        info["pyside6"] = None
    return info


def QtEnv(args):
    """ 子进程环境：默认 offscreen（CI 没显示器），但允许调用方显式覆盖 """
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    env["PYTHONPATH"] = SrcDir(args) + os.pathsep + env.get("PYTHONPATH", "")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    return env


# ------------------------------------------------------------------ 合成测试图
def SyntheticJpeg(width=1200, height=1700, quality=88, seed=11, blocks=20000):
    """ 生成确定性的 JPEG 字节（色块纹理，贴近漫画页的高频细节）

    为什么用 **Pillow** 而不是 QPainter 画：
      1. 确定性：PIL + 固定 seed 在任何机器上得到**完全相同**的字节，数值才可比；
      2. 避开 PySide6 6.12.0 的一个真实缺陷 —— 在 Python 3.11 下，循环里大量创建
         `QColor` / `QRect` 临时对象，会在**解释器 finalize 的 GC** 阶段报
         `Fatal Python error: none_dealloc: deallocating None` 直接崩掉进程
         （PySide6 6.11.2 同代码不崩，已实测）。基准脚本要的是稳定数字，不该被
         上游缺陷干扰，所以干脆让 Python 侧不产生这些包装对象。
    """
    from io import BytesIO

    from PIL import Image, ImageDraw

    rnd = random.Random(seed)
    image = Image.new("RGB", (width, height), (245, 245, 245))
    draw = ImageDraw.Draw(image)
    for _ in range(blocks):
        x = rnd.randrange(width)
        y = rnd.randrange(height)
        draw.rectangle([x, y, x + rnd.randrange(2, 30), y + rnd.randrange(2, 30)],
                       fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    buffer = BytesIO()
    image.save(buffer, "JPEG", quality=quality)
    return buffer.getvalue()

