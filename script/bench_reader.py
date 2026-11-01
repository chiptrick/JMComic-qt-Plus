# coding:utf-8
"""基准 3/3：阅读器"开章 60 页建场"耗时（毫秒）

口径（复现方式，改口径等于换尺子）
------------------------------------------------------------------
"建场"到底量什么，取决于跑哪条路径（`--mode`）：

| mode | 复刻的代码 | 每次迭代做什么 | 与线上哪一段对应 |
|---|---|---|---|
| `initall`（**默认**） | `ReadGraphicsView.InitAllQLabel()` 滚动模式 | 从 proxy 池取 item → `SetLabel`（64pt 页码字体）→ `addItem` → `resize/setMinimumSize/setMaximumSize` → `setPos` | 开章时**建出 N 页框架**（不含每页图片） |
| `initall-pixmap` | 同上 + `setPixmap(已解码页)` | 建场 + 每页贴一张已解码好的 QPixmap | 框架 + 贴图（解码在别的线程完成） |
| `initall-decode` | 同上 + 解码 | 建场 + 每页 `QPixmap.loadFromData(JPEG)` 再贴 | **最接近"开章后第一屏可见"**（含解码） |
| `build` | 早期口径（无 `SetLabel`、每次 new proxy） | new `ReadQGraphicsProxyWidget` + `setPixmap` + `addItem` + `setPos` | 仅供历史趋势对比，**不要**用来做验收 |

为什么要分这么细：`SetLabel()` 里每页都要 `QFont().setPointSize(64)` + `setText`，
这一步在真实开章里占大头；早期 `build` 口径没有它，数值会比线上小一个量级，
拿它去判断"开章 ≤60ms"会得出错误结论。

其它口径：
* 页图是确定性生成的 720x1080 JPEG（1200x1700 源图裁切/缩放到 720x1080 后解码），
  只生成/解码一次并复用，保证不同 run 之间唯一变量是"建 N 个 item"。
* 每个 run 前 `scene.clear()` + 处理事件；默认 **冷池**（proxy 池为空，模拟"第一次开章"），
  `--warm-pool` 复用上一轮的 item（模拟"连续翻章"）。
* `--runs`（默认 5）= 重建次数；`--pages`（默认 60）= 每章页数。
* 默认 `QT_QPA_PLATFORM=offscreen`（CI 无显示器、无 GPU）——offscreen 的首帧/重绘
  数值**不能**直接与真机比，只适合同机对照。

用法：
    python script/bench_reader.py                              # 60 页 × 5 次（initall 口径）
    python script/bench_reader.py --mode initall-decode         # 含解码
    python script/bench_reader.py --mode initall-pixmap --runs 10
    python script/bench_reader.py --json --out /tmp/reader.json

退出码：0 正常 / 1 测量失败 / 2 平台不支持或缺依赖
"""
import os
import sys
import time

import bench_common as bc

DEFAULT_PAGES = 60
PAGE_W, PAGE_H = 720, 1080
MODES = ("initall", "initall-pixmap", "initall-decode", "build")


def App():
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    try:
        from PySide6.QtGui import QImageReader
        QImageReader.setAllocationLimit(0)
    except Exception:
        pass
    return app


def BuildPagePixmap():
    """ 一张"已解码好"的页图

    直接从确定性 JPEG 解码（而不是用 QPainter 在 Python 里逐块画）：
    既能保证每次运行的图完全一致，也避开 PySide6 6.12.0 在解释器 finalize 阶段
    处理大量 QColor/QRect 临时对象时的崩溃（详见 bench_common.SyntheticJpeg 注释）。
    """
    from PySide6.QtGui import QPixmap

    raw = bc.SyntheticJpeg(1200, 1700, quality=88)
    pixmap = QPixmap()
    if not pixmap.loadFromData(raw):
        raise RuntimeError("测试页图解码失败（Pillow 生成的 JPEG 无法被 Qt 读取）")
    return pixmap.scaled(PAGE_W, PAGE_H)


def SetLabelReal(label, index):
    """ 复刻 read_graphics.SetLabel：居中的 64pt 页码

    真实代码里这一句在每页都要做一次（QFont 构造 + setText 触发文本布局），
    是"建场"成本的主要来源之一 —— 少了它就不叫同一个口径了。
    """
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QFont

    font = QFont()
    font.setPointSize(64)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setFont(font)
    label.setText(str(index + 1))


class ProxyPool(object):
    """ 复刻 QtReadImgPoolManager：池里没有才新建，用完放回 """

    def __init__(self, factory):
        self._factory = factory
        self.items = []

    def Get(self):
        if not self.items:
            return self._factory()
        return self.items.pop()

    def Recycle(self, items):
        for item in items:
            item.setPos(0, 0)
        self.items.extend(items)


def BuildScene(view, scene, pagePixmap, pages, radius, factory, pool, mode, raw):
    """ 按 mode 建 N 页，返回 (建场毫秒, 建场+事件毫秒) """
    from PySide6.QtWidgets import QApplication

    scene.clear()
    view.viewport().update()
    QApplication.processEvents()

    start = time.perf_counter()
    items = []
    for index in range(pages):
        if mode == "build":
            item = factory()                       # 早期口径：每次新建
            item.setPixmap(pagePixmap)
            scene.addItem(item)
            item.setPos(0, index * PAGE_H)
        else:
            item = pool.Get()                      # 真实口径：从池里取
            label = item.widget()
            SetLabelReal(label, index)             # InitAllQLabel 里每页都做
            scene.addItem(item)
            label.resize(radius[0], radius[1])
            label.setMinimumSize(radius[0], radius[1])
            label.setMaximumSize(radius[0], radius[1])
            if mode == "initall-pixmap":
                item.setPixmap(pagePixmap)
            elif mode == "initall-decode":
                from PySide6.QtGui import QPixmap
                pixmap = QPixmap()
                pixmap.loadFromData(raw)
                item.setPixmap(pixmap)
            item.setPos(0, index * PAGE_H)
        items.append(item)
    buildMs = (time.perf_counter() - start) * 1000.0

    start = time.perf_counter()
    QApplication.processEvents()
    buildWithEventsMs = buildMs + (time.perf_counter() - start) * 1000.0
    return buildMs, buildWithEventsMs, items


def Main():
    parser = bc.MakeParser(
        "基准 3/3：阅读器 60 页建场耗时（毫秒）",
        epilog="mode 说明（详见文件头注释）:\n"
               "  initall        = InitAllQLabel 口径：proxy 池 + SetLabel(64pt) + addItem + resize + setPos（默认）\n"
               "  initall-pixmap = 上面 + 每页 setPixmap(已解码)\n"
               "  initall-decode = 上面 + 每页解码+贴图（最接近开章可见）\n"
               "  build          = 早期口径（无 SetLabel），只用于历史趋势对比\n\n"
               "示例:\n"
               "  python script/bench_reader.py --pages 60 --runs 5\n"
               "  python script/bench_reader.py --mode initall-decode --json --out /tmp/reader.json")
    parser.add_argument("--pages", type=int, default=DEFAULT_PAGES,
                        help="每章页数（默认 {}）".format(DEFAULT_PAGES))
    parser.add_argument("--mode", choices=MODES, default="initall",
                        help="建场口径（默认 initall，与 InitAllQLabel 对齐）")
    parser.add_argument("--width", type=int, default=800, help="阅读区宽（默认 800）")
    parser.add_argument("--height", type=int, default=1200, help="阅读区高（默认 1200）")
    parser.add_argument("--warm-pool", action="store_true",
                        help="复用上一轮的 proxy（模拟连续翻章；默认冷池=第一次开章）")
    args = parser.parse_args()
    if args.runs < 1 or args.pages < 1:
        parser.error("--runs / --pages 必须 >= 1")
    bc.RequireRss()

    src = bc.SrcDir(args)
    if not os.path.isdir(src):
        sys.stderr.write("[error] 找不到源码目录: {}\n".format(src))
        return 2
    sys.path.insert(0, src)
    if not os.environ.get("QT_QPA_PLATFORM"):
        os.environ["QT_QPA_PLATFORM"] = "offscreen"

    App()
    try:
        from view.read.read_qgraphics_proxy_widget import ReadQGraphicsProxyWidget
        from PySide6.QtWidgets import QApplication, QGraphicsScene, QGraphicsView
    except Exception as es:
        sys.stderr.write("[error] 导入被测模块失败: {}\n"
                         "        提示: 用装了应用依赖的解释器（--python <venv>/bin/python）\n".format(es))
        return 2

    scene = QGraphicsScene()
    view = QGraphicsView(scene)
    view.resize(args.width, args.height)
    view.show()
    QApplication.processEvents()

    pagePixmap = BuildPagePixmap()
    raw = bc.SyntheticJpeg(1200, 1700, quality=88) if args.mode == "initall-decode" else b""
    pool = ProxyPool(ReadQGraphicsProxyWidget)
    radius = (args.width, args.height)

    # 预热一次（不计入样本）：第一次建场景要触发布局/字体/Qt 插件的一次性初始化，
    # 混进样本会让 p50 被"第一次"带偏（实测首次 24ms、之后 6~8ms）。
    _, _, warmItems = BuildScene(view, scene, pagePixmap, args.pages, radius,
                                 ReadQGraphicsProxyWidget, pool, args.mode, raw)
    if not args.warm_pool:
        pool = ProxyPool(ReadQGraphicsProxyWidget)      # 冷池：模拟"第一次开章"
    else:
        pool.Recycle(warmItems)
    rssBase = bc.SelfRssMb()

    builds, buildsWithEvents = [], []
    for index in range(args.runs):
        buildMs, withEventsMs, items = BuildScene(view, scene, pagePixmap, args.pages, radius,
                                                  ReadQGraphicsProxyWidget, pool, args.mode, raw)
        builds.append(buildMs)
        buildsWithEvents.append(withEventsMs)
        sys.stderr.write("  run {}/{}: 建 {} 页 {:.1f} ms (含事件 {:.1f} ms)\n".format(
            index + 1, args.runs, args.pages, buildMs, withEventsMs))
        if args.warm_pool:
            pool.Recycle(items)

    rssAfter = bc.SelfRssMb()
    result = {
        "metric": "reader_build",
        "mode": args.mode,
        "pages": args.pages,
        "runs": args.runs,
        "warm_pool": bool(args.warm_pool),
        "viewport": {"w": args.width, "h": args.height},
        "page": {"w": PAGE_W, "h": PAGE_H},
        "reader_build_ms": bc.Stats(builds),
        "reader_build_plus_events_ms": bc.Stats(buildsWithEvents),
        "rss_base_mb": rssBase,
        "rss_after_mb": rssAfter,
        "rss_delta_mb": None if (rssBase is None or rssAfter is None)
                        else round(rssAfter - rssBase, 1),
        "env": bc.EnvInfo(args),
    }
    human = [
        "== 阅读器 {} 页建场（mode={}{}）=========================".format(
            args.pages, args.mode, "，热池" if args.warm_pool else "，冷池"),
        "  {:<28s} {:>9.1f} ms   (p50)   p95 {:>8.1f} ms   n={}".format(
            "建场", result["reader_build_ms"]["p50"],
            result["reader_build_ms"]["p95"], args.runs),
        "  {:<28s} {:>9.1f} ms   (p50, 含首次布局/事件)".format(
            "建场+processEvents", result["reader_build_plus_events_ms"]["p50"]),
        "  {:<28s} {:>9.1f} MB   (整个进程的 RSS 增量)".format(
            "RSS 增量", result["rss_delta_mb"] or 0.0),
        "  口径: {}".format({
            "initall": "InitAllQLabel（proxy 池 + SetLabel 64pt + addItem + resize + setPos）",
            "initall-pixmap": "InitAllQLabel + 每页 setPixmap(已解码)",
            "initall-decode": "InitAllQLabel + 每页解码+贴图（最接近开章可见）",
            "build": "早期口径（无 SetLabel，每次 new proxy）——仅历史趋势对比",
        }[args.mode]),
        "  环境: Qt={} PySide6={}".format(
            result["env"]["qt_platform"] or "(未设置)", result["env"]["pyside6"]),
    ]
    return bc.Emit(result, jsonOnly=args.json, humanLines=human, outPath=args.out)


if __name__ == "__main__":
    sys.exit(Main())
