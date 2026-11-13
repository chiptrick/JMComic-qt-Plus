# coding:utf-8
"""基准 2/3：单页"下载字节 → 可显示 pixmap"（毫秒，p50 / p95）

口径（复现方式，改口径等于换尺子）
------------------------------------------------------------------
* 测的是**应用自己的那条路**：`view.read.read_qgraphics_proxy_widget.ReadQGraphicsProxyWidget
  .SetGifData(data, width, height)` —— 阅读器把一页下载字节变成屏幕上一页图，走的就是它
  （内部：动图判定 → `QPixmap.loadFromData` 全解码 → `scaled(SmoothTransformation)` →
  `setFixedSize` + `setPixmap`）。不是另外写一套等价代码，也不是量 Qt 的裸解码。
* 输入是**确定性生成的** JPEG：1200x1700、随机色块（`random.seed(11)`）+ JPEG q=88。
  同一台机器上每次生成的字节完全一致 —— 换机器也能复现同一张图。
  用合成图而不是仓库里的真实漫画页：避免把版权图带进仓库，也避免"图不一样所以数值不可比"。
* `--runs`（默认 5）次 **run 级** 测量；每次 run 内跑 `--iters`（默认 20）页取平均，
  作为该 run 的一个样本。最终 p50/p95 在 run 级样本上算（理由见 bench_common）。
* 不含磁盘 IO / 网络：只量"已经有字节之后"的 CPU 侧成本。
* `--breakdown` 额外打印分步耗时（动图判定 / 解码 / 缩放），用来定位回归发生在哪一步。
* `--main-thread`：**A2/B1s 的关键指标** —— 分别量"主线程被阻塞多久"与"主线程吃掉多少 CPU"。
  两条路径的主线程成本结构完全不同：
    - `qt`（A2 之后）：解码+分块全在**主线程**，主线程阻塞 ≈ 主线程 CPU；
    - `legacy-pil`（A2 之前）：PIL 在 `TaskMulti` **子进程**里跑，主线程只是等 IPC，
      阻塞时间不短但主线程 CPU 很低。
  所以只看"单页总耗时"会漏掉"主线程被占满"这个真正影响首屏的指标。实现上 legacy 分支
  真的起一个 worker 进程（`multiprocessing` spawn）跑 PIL，worker 自报自己的 CPU 时间，
  主线程用 `time.thread_time()` 量自己的 CPU —— 三个数一起看才不会误判。

用法：
    python script/bench_pipeline.py                        # 5×20 页
    python script/bench_pipeline.py --runs 10 --iters 30
    python script/bench_pipeline.py --json --out /tmp/pipeline.json
    python script/bench_pipeline.py --breakdown
    python script/bench_pipeline.py --both --main-thread   # A2 验收：两条路径 × 主线程占用

退出码：0 正常 / 1 测量失败 / 2 平台不支持或缺依赖
"""
import multiprocessing
import os
import random
import sys
import time

import bench_common as bc

PAGE_W, PAGE_H = 1200, 1700     # 输入页尺寸（JM 常见长条图）
VIEW_W, VIEW_H = 720, 1080      # 阅读区目标尺寸（缩放目标）
DEFAULT_ITERS = 20


def BuildSyntheticJpeg(quality=88):
    """ 确定性 JPEG（实现见 bench_common.SyntheticJpeg：PIL 生成，见那里关于 PySide6 6.12 的说明） """
    return bc.SyntheticJpeg(PAGE_W, PAGE_H, quality=quality)


def App():
    """ 建好 offscreen 的 QApplication（只建一次） """
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    # 与 src/start.py 一致：不限制 QImage 分配上限，否则大图解码会走另一条路径
    try:
        from PySide6.QtGui import QImageReader
        QImageReader.setAllocationLimit(0)
    except Exception:
        pass
    return app


# ------------------------------------------------------------------ legacy 口径
def LegacyPilBytes(raw, blocks=10):
    """ 复刻**现状桌面链路**：PIL 解码 → 按块逆序还原 → 重编码回 JPEG 字节

    这条链路就是 `ToolUtil.SegmentationPicture()` 在"图确实被分块"时的形态
    （PIL 解码 + crop/paste + save 回 bytes），之后才交给 Qt 的 `QPixmap.loadFromData`
    再解码一次。方案 §1.4 的 39.6ms 很可能就是这个口径 —— 它比"只有 Qt 解码+缩放"
    多了一整趟 "PIL 解码 + JPEG 编码 + Qt 再解码"。

    这里用同样的合成图、同样的块数，只把**多出来的那一趟**显式量出来，
    这样"去掉 PIL 能省多少"才有对照基准（否则用不含 PIL 的口径去验"去掉 PIL"，
    结论必然是"几乎没变"）。
    """
    from io import BytesIO

    from PIL import Image

    source = Image.open(BytesIO(raw))
    source.load()
    width, height = source.size
    target = Image.new(source.mode, (width, height))

    rest = height % blocks
    blockH = height // blocks
    spans, total = [], 0
    for index in range(blocks):
        end = blockH * (index + 1) + (rest if index == blocks - 1 else 0)
        spans.append((total, end))
        total = end

    offset = 0
    for start, end in reversed(spans):
        piece = source.crop((0, start, width, end))
        target.paste(piece, (0, offset, width, offset + (end - start)))
        offset += end - start

    buffer = BytesIO()
    target.save(buffer, source.format or "JPEG")
    return buffer.getvalue()


def MeasureRun(proxy, raw, iters):
    """ 一个 run：iters 次"字节 → 可显示 pixmap"，返回每页平均毫秒 """
    # 预热一次：第一次调用会触发 Qt 插件/字体缓存等一次性初始化
    proxy.SetGifData(raw, VIEW_W, VIEW_H)
    start = time.perf_counter()
    for _ in range(iters):
        proxy.SetGifData(raw, VIEW_W, VIEW_H)
    return (time.perf_counter() - start) * 1000.0 / iters


def MeasureRunLegacy(proxy, raw, iters):
    """ legacy 口径：**把 PIL 那一趟也算进去**（PIL 解码+分块+重编码 + Qt 解码+缩放）

    只把"重编码后的字节"喂给 Qt 是不对的：那样量到的只是"更小的 JPEG 解码更快"，
    会得出"legacy 反而更快"的假结论（实测 -1.5ms 就是这么来的）。
    生产中 PIL 那段在 TaskMulti 子进程里跑，这里串行计入 —— 所以这个数代表
    **CPU 总成本**之差，不等于用户感知的首图延迟之差（后者取决于并行度）。
    """
    proxy.SetGifData(LegacyPilBytes(raw), VIEW_W, VIEW_H)     # 预热
    start = time.perf_counter()
    for _ in range(iters):
        data = LegacyPilBytes(raw)
        proxy.SetGifData(data, VIEW_W, VIEW_H)
    return (time.perf_counter() - start) * 1000.0 / iters


# ------------------------------------------------------------------ 主线程占用
def LegacyWorker(queueIn, queueOut):
    """ 子进程：复刻 `TaskMulti` 的角色（PIL 解码+分块+重编码），并自报自己的 CPU 时间

    用它来回答一个"单页总耗时答不了"的问题：A2 之前主线程**只是等**子进程，
    阻塞时间不短但主线程 CPU 很低；A2 之后主线程自己解码+分块，主线程 CPU 会显著上升。
    """
    while True:
        item = queueIn.get()
        if item is None:
            break
        raw, blocks = item
        cpu0 = time.process_time()
        data = LegacyPilBytes(raw, blocks)
        queueOut.put((data, (time.process_time() - cpu0) * 1000.0))


def MeasureRunMainThread(proxy, raw, iters, queueIn=None, queueOut=None):
    """ 同时量三个数（每页平均，毫秒）：主线程阻塞 / 主线程 CPU / 子进程 CPU

    * 主线程阻塞 = 主线程从"发起这一页"到"贴图完成"的墙钟 —— 用户感受到的串行度；
    * 主线程 CPU = `time.thread_time()` 增量 —— 主线程真的花了多少 CPU；
    * 子进程 CPU = worker 自报 —— legacy 路径里那 18ms PIL 成本落在这里。

    legacy 路径**真的**起 worker 进程（multiprocessing spawn），不是在主线程里假装并行；
    否则第 2、3 个数字会完全失真。
    """
    useWorker = queueIn is not None
    # 预热一页（不计入样本）
    if useWorker:
        queueIn.put((raw, 10))
        warm, _ = queueOut.get()
        proxy.SetGifData(warm, VIEW_W, VIEW_H)
    else:
        proxy.SetGifData(raw, VIEW_W, VIEW_H)

    wall = cpu = worker = 0.0
    for _ in range(iters):
        wall0 = time.perf_counter()
        cpu0 = time.thread_time()
        if useWorker:
            queueIn.put((raw, 10))
            data, workerCpu = queueOut.get()
            proxy.SetGifData(data, VIEW_W, VIEW_H)
            worker += workerCpu
        else:
            proxy.SetGifData(raw, VIEW_W, VIEW_H)
        wall += time.perf_counter() - wall0
        cpu += time.thread_time() - cpu0
    return (wall * 1000.0 / iters,
            cpu * 1000.0 / iters,
            (worker / iters) if useWorker else None)


def Breakdown(raw, iters):
    """ 分步耗时：定位"慢在哪一步"（动图判定 / 解码 / 平滑缩放） """
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QPixmap
    from tools.tool import ToolUtil

    out = {}
    start = time.perf_counter()
    for _ in range(iters):
        ToolUtil.GetAnimationFormat(raw)
    out["anim_format_ms"] = (time.perf_counter() - start) * 1000.0 / iters

    start = time.perf_counter()
    pixmap = QPixmap()
    for _ in range(iters):
        pixmap.loadFromData(raw)
    out["pixmap_load_ms"] = (time.perf_counter() - start) * 1000.0 / iters

    start = time.perf_counter()
    for _ in range(iters):
        pixmap.scaled(VIEW_W, VIEW_H, Qt.AspectRatioMode.KeepAspectRatio,
                      Qt.TransformationMode.SmoothTransformation)
    out["smooth_scale_ms"] = (time.perf_counter() - start) * 1000.0 / iters
    return out


def Main():
    parser = bc.MakeParser(
        "基准 2/3：单页 解码+缩放+显示（毫秒，p50/p95）",
        epilog="示例:\n"
               "  python script/bench_pipeline.py --runs 5 --iters 20\n"
               "  python script/bench_pipeline.py --json --out /tmp/pipeline.json --breakdown")
    parser.add_argument("--iters", type=int, default=DEFAULT_ITERS,
                        help="每个 run 内重复处理的页数（默认 {}），run 值取平均".format(DEFAULT_ITERS))
    parser.add_argument("--breakdown", action="store_true",
                        help="额外测量分步耗时（动图判定 / 解码 / 平滑缩放）")
    parser.add_argument("--legacy-pil", action="store_true",
                        help="改测**现状桌面链路**：PIL 解码+分块还原+重编码回 JPEG，再交给 Qt 解码显示"
                             "（方案 §1.4 的 39.6ms 很可能是这个口径；默认口径只含 Qt 解码+缩放）")
    parser.add_argument("--both", action="store_true",
                        help="同一批样本上同时测两条路径并给出差值（相当于 --legacy-pil 的对照模式）")
    parser.add_argument("--main-thread", action="store_true",
                        help="额外量主线程占用：主线程阻塞 / 主线程 CPU / 子进程 CPU（A2、B1s 的关键指标）")
    args = parser.parse_args()
    if args.runs < 1 or args.iters < 1:
        parser.error("--runs / --iters 必须 >= 1")
    bc.RequireRss()

    src = bc.SrcDir(args)
    if not os.path.isdir(src):
        sys.stderr.write("[error] 找不到源码目录: {}\n".format(src))
        return 2
    sys.path.insert(0, src)
    if not os.environ.get("QT_QPA_PLATFORM"):
        os.environ["QT_QPA_PLATFORM"] = "offscreen"     # CI 没有显示器

    App()
    try:
        from view.read.read_qgraphics_proxy_widget import ReadQGraphicsProxyWidget
    except Exception as es:
        sys.stderr.write("[error] 导入被测模块失败: {}\n"
                         "        提示: 用装了应用依赖的解释器（--python <venv>/bin/python）\n".format(es))
        return 2

    raw = BuildSyntheticJpeg()
    legacyRaw = LegacyPilBytes(raw) if (args.legacy_pil or args.both) else None
    proxy = ReadQGraphicsProxyWidget()
    rssBase = bc.SelfRssMb()

    def RunSamples(payload, tag, legacy=False):
        values = []
        for index in range(args.runs):
            runner = MeasureRunLegacy if legacy else MeasureRun
            ms = runner(proxy, payload, args.iters)
            values.append(ms)
            sys.stderr.write("  run {}/{} [{}]: {:.2f} ms/页\n".format(
                index + 1, args.runs, tag, ms))
        return values

    # 三条分支共用同一份样本数 / iters / 预热方式，只换"喂给 SetGifData 的字节"：
    #   default → 直接喂原始下载字节（Qt 解码 + 缩放）
    #   legacy  → 先走 PIL 解码+分块+重编码，再喂给 Qt（现状桌面链路）
    #   both    → 两条都测，给出差值（A2"去掉 PIL"的收益必须用这个对照才成立）
    samples, legacySamples = [], []
    if args.both:
        samples = RunSamples(raw, "qt")
        legacySamples = RunSamples(raw, "legacy-pil", legacy=True)
    elif args.legacy_pil:
        samples = RunSamples(raw, "legacy-pil", legacy=True)
    else:
        samples = RunSamples(raw, "qt")

    # 主线程占用：与总耗时同一次进程、同一台机器，只是把"墙钟"拆成
    # 主线程阻塞 / 主线程 CPU / 子进程 CPU 三个数（见 MeasureRunMainThread 注释）。
    mainThread = {}
    if args.main_thread:
        context = multiprocessing.get_context("spawn")
        queueIn, queueOut = context.Queue(), context.Queue()
        worker = context.Process(target=LegacyWorker, args=(queueIn, queueOut), daemon=True)
        worker.start()
        try:
            if args.both or args.legacy_pil:
                wall, cpu, wcpu = [], [], []
                for index in range(args.runs):
                    result3 = MeasureRunMainThread(proxy, raw, args.iters, queueIn, queueOut)
                    wall.append(result3[0])
                    cpu.append(result3[1])
                    wcpu.append(result3[2] or 0.0)
                    sys.stderr.write("  run {}/{} [legacy-pil/main-thread]: 阻塞 {:.2f} "
                                     "主线程CPU {:.2f} 子进程CPU {:.2f} ms/页\n".format(
                                         index + 1, args.runs, result3[0], result3[1], result3[2] or 0.0))
                mainThread["legacy-pil"] = {
                    "main_block_ms": bc.Stats(wall),
                    "main_cpu_ms": bc.Stats(cpu),
                    "worker_cpu_ms": bc.Stats(wcpu),
                }
            if args.both or not args.legacy_pil:
                wall, cpu = [], []
                for index in range(args.runs):
                    result3 = MeasureRunMainThread(proxy, raw, args.iters)
                    wall.append(result3[0])
                    cpu.append(result3[1])
                    sys.stderr.write("  run {}/{} [qt/main-thread]: 阻塞 {:.2f} 主线程CPU {:.2f} ms/页\n".format(
                        index + 1, args.runs, result3[0], result3[1]))
                mainThread["qt"] = {
                    "main_block_ms": bc.Stats(wall),
                    "main_cpu_ms": bc.Stats(cpu),
                }
        finally:
            try:
                queueIn.put(None)
                worker.join(timeout=5)
                if worker.is_alive():
                    worker.terminate()
            except Exception:
                pass

    result = {
        "metric": "pipeline_page",
        "path": "both" if args.both else ("legacy-pil" if args.legacy_pil else "qt"),
        "runs": args.runs,
        "iters": args.iters,
        "input": {"w": PAGE_W, "h": PAGE_H, "jpeg_kb": round(len(raw) / 1024.0, 1)},
        "target": {"w": VIEW_W, "h": VIEW_H},
        "pipeline_page_ms": bc.Stats(samples),
        "rss_after_mb": bc.SelfRssMb(),
        "rss_base_mb": rssBase,
        "env": bc.EnvInfo(args),
    }
    if args.both:
        result["pipeline_page_ms_legacy"] = bc.Stats(legacySamples)
        result["legacy_overhead_ms"] = round(
            result["pipeline_page_ms_legacy"]["p50"] - result["pipeline_page_ms"]["p50"], 2)
        result["legacy_input_kb"] = round(len(legacyRaw) / 1024.0, 1)
    if args.breakdown:
        result["breakdown_ms"] = {k: round(v, 2) for k, v in Breakdown(raw, args.iters).items()}
    if mainThread:
        result["main_thread"] = mainThread

    human = [
        "== 单页 解码+缩放+显示 ==================================",
        "  输入 {}x{} JPEG {:.0f} KB → 目标 {}x{}".format(
            PAGE_W, PAGE_H, len(raw) / 1024.0, VIEW_W, VIEW_H),
        "  口径: {}".format("两条路径对照" if args.both else
                          ("legacy-pil（PIL 解码+分块+重编码 → Qt 解码）" if args.legacy_pil
                           else "qt（Qt 解码+缩放，不含 PIL）")),
        "  {:<28s} {:>9.2f} ms   (p50)   p95 {:>8.2f} ms   n={}×{}页".format(
            "单页耗时" if not args.both else "Qt 直通路径",
            result["pipeline_page_ms"]["p50"],
            result["pipeline_page_ms"]["p95"], args.runs, args.iters),
    ]
    if args.both:
        human.append("  {:<28s} {:>9.2f} ms   (p50)   p95 {:>8.2f} ms   legacy 输入 {:.0f} KB".format(
            "legacy-pil 路径", result["pipeline_page_ms_legacy"]["p50"],
            result["pipeline_page_ms_legacy"]["p95"], result["legacy_input_kb"]))
        human.append("  {:<28s} {:>9.2f} ms   ← A2「去掉 PIL」的收益上限（p50 之差）".format(
            "legacy 多出来的开销", result["legacy_overhead_ms"]))
    human.append("  环境: Qt={} PySide6={}".format(
        result["env"]["qt_platform"] or "(未设置)", result["env"]["pyside6"]))
    if args.breakdown:
        parts = result["breakdown_ms"]
        human.append("  分步: 动图判定 {:.2f} + 解码 {:.2f} + 平滑缩放 {:.2f} ms".format(
            parts.get("anim_format_ms", 0.0), parts.get("pixmap_load_ms", 0.0),
            parts.get("smooth_scale_ms", 0.0)))
    if mainThread:
        human.append("  主线程占用（每页；阻塞=墙钟，CPU=thread_time，子进程=worker 自报）:")
        for tag in ("legacy-pil", "qt"):
            if tag not in mainThread:
                continue
            entry = mainThread[tag]
            line = "    {:<12s} 阻塞 {:>7.2f} ms | 主线程CPU {:>7.2f} ms".format(
                tag, entry["main_block_ms"]["p50"], entry["main_cpu_ms"]["p50"])
            if "worker_cpu_ms" in entry:
                line += " | 子进程CPU {:>7.2f} ms".format(entry["worker_cpu_ms"]["p50"])
            human.append(line)
        human.append("    判读: 主线程CPU 才是「卡不卡」的指标；阻塞高但 CPU 低 = 在等子进程（可接受）。")
    return bc.Emit(result, jsonOnly=args.json, humanLines=human, outPath=args.out)


if __name__ == "__main__":
    sys.exit(Main())
