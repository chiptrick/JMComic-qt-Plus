# coding: utf-8
"""C++ UI 桌面端自动化回归（Windows）。

驱动 jmcomic_ui.exe 自带的测试参数（--page/--shot/--demo-*/--drawer-* …），
对产物做三类断言：
  1. 进程：退出码 0、无崩溃标记；
  2. 图像：尺寸符合 2x 逻辑分辨率、非空白（标准差/颜色数）、必要时两图必须不同；
  3. 输出：从 stderr 里解析 bench / touch / demo 行（分页行数、手势结果、字体变更）。

跑法：
  python cpp/tools/c1_ui_desktop_suite.py
  python cpp/tools/c1_ui_desktop_suite.py --exe <path> --out <dir> --only pages
  python cpp/tools/c1_ui_desktop_suite.py --only offline     # 断网可跑，确定性基线
  python cpp/tools/c1_ui_desktop_suite.py --only net         # 只跑联网用例

分层（D2）
----------
每个用例属于两层之一，汇总分开统计，避免"SKIP 被读成全部通过"：

  * offline —— 不依赖外网：页面渲染、DNS wire 解析、配置默认值、路由优先级、
    分页/主题/字体/性能/边界、本地 HTTP（127.0.0.1）与本地 .webp。
    这一层必须 100% 通过，任何 FAIL 都是真回归。
  * net —— 真实接口/测速/探针实连。失败分两种：
      - 没有拿到任何可判定输出（超时、连接失败）→ SKIP，汇总里单列；
      - 拿到了输出但断言不成立 → FAIL（不能用"网络波动"掩盖）。

`--only` 既可给章节标签（pages/paging/flows/proxy/advanced/biz…），也可给层标签
（offline/net），两者可混用：`--only proxy,offline` 表示只在 proxy 章节里跑离线用例。

不打扰前台应用
--------------
默认把每个用例进程按"静默"方式拉起（见 quiet_runner.py）：
  * --quiet  → 窗口移到可视区外 + WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW，
    不抢焦点、不进任务栏/Alt-Tab、不遮挡任何窗口；截图仍走原生渲染（真实 DPI/字体）。
  * 旧 exe 不认识 --quiet 时自动退回 QT_QPA_PLATFORM=offscreen（无窗口，但字体/DPR 降级）。
  * 本进程与子进程统一降到 BELOW_NORMAL 优先级，前台输入与帧率不受影响。
  * `--quiet minimized` 可改成"最小化启动"；`--quiet off` 恢复旧的弹窗行为（排障用）。

结果记录
--------
每次运行都会在 cpp/build_logs/ui_suite_<时间戳>/ 留下：
  suite.log（完整终端输出）、report.md（汇总表）、results.json（机器可读）、
  errors/*.log（失败用例的完整 stdout/stderr）。
终端在跑长回归时可能被切走或断连，这三份文件是唯一可靠的结果载体。
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from quiet_runner import (IS_WIN, MODES, ApplyOwnPriority, QuietExec,  # noqa: E402
                          Reporter, Tee, Tray)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI_DIR = os.path.join(ROOT, "cpp", "shell", "ui")
DEFAULT_EXE = os.path.join(UI_DIR, "build", "jmcomic_ui.exe")
QT_BIN = r"C:\Qt\6.12.0\mingw_64\bin"
MINGW_BIN = r"C:\Qt\Tools\mingw1310_64\bin"
FFMPEG = "ffmpeg"

PAGES = ["home", "category", "search", "detail", "reader", "download", "local",
         "favorite", "history", "comment", "sr", "settings", "about", "login",
         "mycomment", "remote", "localfav", "week", "nas", "batchsr", "help"]

Failures = []
Skips = []
Passes = 0
CaseLog = []

# 分层统计：offline = 断网必须全过；net = 允许 SKIP，但不允许"假 PASS"
LAYERS = ("offline", "net")
Stats = {name: {"pass": 0, "fail": 0, "skip": 0} for name in LAYERS}
# 当前生效的层过滤（空 = 不过滤）；由 Main 在解析 --only 后写入
OnlyLayers = set()

# 当前运行上下文：让 Check 能在失败时把原始 stderr 落盘（截图断言失败时最缺的就是它）
CONTEXT = {"reporter": None, "runner": None, "tray": None}


def WantLayer(layer):
    """层过滤：--only offline 时联网用例整段不执行（省时间，也让断网跑法定量）。"""
    return not OnlyLayers or layer in OnlyLayers


def Check(ok, name, detail="", layer="offline"):
    global Passes
    if isinstance(detail, (list, tuple)):
        detail = " | ".join(str(x) for x in detail)
    print(("  ok   " if ok else "  FAIL ") + name + (("  <" + str(detail) + ">") if detail else ""))
    CaseLog.append((ok, name, detail, layer))
    Stats[layer]["pass" if ok else "fail"] += 1
    reporter = CONTEXT["reporter"]
    if reporter is not None:
        reporter.case(ok, name, detail)
    if ok:
        Passes += 1
    else:
        Failures.append("[%s] " % layer + name + ((" <" + detail + ">") if detail else ""))
        if reporter is not None:
            reporter.dump_error(name, getattr(CONTEXT["runner"], "last", None))
    tray = CONTEXT["tray"]
    if tray is not None:
        tray.set("JMComic UI 回归：PASS %d / FAIL %d" % (Passes, len(Failures)))


def Skip(name, detail="", layer="net"):
    """记一条 SKIP。layer=net 表示"网络没给到可判定的输出"，不是通过。"""
    if isinstance(detail, (list, tuple)):
        detail = " | ".join(str(x) for x in detail)
    print("  skip  " + name + (("  <" + str(detail) + ">") if detail else ""))
    CaseLog.append((None, name, detail, layer))
    Stats[layer]["skip"] += 1
    Skips.append("[%s] " % layer + name + ((" <" + detail + ">") if detail else ""))


def NetCheck(ok, name, detail="", reachable=True):
    """联网用例的统一收口。

    reachable=False → 根本没拿到可判定的输出（超时/连不上/探针没启动）：SKIP。
    reachable=True  → 断言必须成立，否则 FAIL。这一条是 D2 的核心：
    以前把"拿到了 200 但字段为空"也 skip 掉，等于用网络波动掩盖真缺陷。
    """
    if not reachable:
        Skip(name, detail)
        return
    Check(ok, name, detail, layer="net")



class Runner:
    def __init__(self, exe, outdir, quiet_mode="auto", priority="below", probe_dir=None):
        self.exe = exe
        self.out = outdir
        self.last = None
        self.cache = os.path.join(outdir, "_cache")
        self.appdata = os.path.join(outdir, "_appdata")
        os.makedirs(self.out, exist_ok=True)
        os.makedirs(self.cache, exist_ok=True)
        os.makedirs(self.appdata, exist_ok=True)
        self.env = dict(os.environ)
        self.env["PATH"] = QT_BIN + os.pathsep + MINGW_BIN + os.pathsep + self.env.get("PATH", "")
        self.env["JM_CACHE_DIR"] = self.cache
        self.env["APPDATA"] = self.appdata   # 隔离 ui.ini，跑测不改用户字体设置
        self.env["LOCALAPPDATA"] = self.appdata
        self.session = os.path.join(outdir, "session.ini")
        self.env["JM_SESSION_FILE"] = self.session
        self.covers = os.path.join(outdir, "_covers")
        shutil.rmtree(self.covers, ignore_errors=True)
        self.env["JM_COVER_CACHE"] = self.covers
        # 分流配置单独隔离：Qt 的 AppConfigLocation 在 Windows 上走 Known Folder API，
        # 只改 APPDATA 环境变量并不生效，必须用 JM_UI_CONFIG 显式指定。
        # 否则上一轮"选中节点/CDN 线路"会留在 ui.ini 里，让后续用例莫名其妙全走节点。
        self.uicfg = os.path.join(outdir, "_uicfg")
        shutil.rmtree(self.uicfg, ignore_errors=True)
        self.env["JM_UI_CONFIG"] = self.uicfg
        # 每轮从"未登录"起跑，否则上一轮留下的会话会让登录态用例失去对照
        if os.path.exists(self.session):
            os.remove(self.session)
        # 静默执行：窗口不上屏/不抢焦点 + 子进程降优先级。模式在 Main 里探测。
        self.q = QuietExec(exe, base_env=self.env, mode=quiet_mode, priority=priority,
                           probe_dir=probe_dir, probe_log=print)

    def run(self, args, timeout=60, env_extra=None):
        p = self.q.run(args, timeout=timeout, env_extra=env_extra, cwd=UI_DIR)
        self.last = p
        return p.returncode, p.stdout, p.stderr

    def shot(self, name, args, delay="500", timeout=60, env_extra=None):
        path = os.path.join(self.out, name + ".png")
        if os.path.exists(path):
            os.remove(path)
        argv = list(args) + ["--shot", path, "--shot-delay", delay]
        rc, out, err = self.run(argv, timeout=timeout, env_extra=env_extra)
        return rc, out, err, path


def ImageInfo(path):
    """用 ffmpeg 解成 rawvideo，算尺寸/均值/标准差/颜色数（抽样）。"""
    if not os.path.exists(path):
        return None
    probe = subprocess.run([FFMPEG, "-v", "error", "-i", path,
                            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                           capture_output=True)
    if probe.returncode != 0 or not probe.stdout:
        return None
    data = probe.stdout
    dim = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                         capture_output=True, text=True)
    try:
        w, h = [int(x) for x in dim.stdout.strip().split(",")]
    except Exception:
        return None
    if w * h * 3 != len(data):
        return None
    total = 0
    total2 = 0
    colors = set()
    step = 11 * 3
    for i in range(0, len(data) - 2, step):
        r, g, b = data[i], data[i + 1], data[i + 2]
        v = (r * 299 + g * 587 + b * 114) // 1000
        total += v
        total2 += v * v
        colors.add((r >> 3, g >> 3, b >> 3))
    n = len(range(0, len(data) - 2, step))
    mean = total / n
    var = max(0.0, total2 / n - mean * mean)
    return {"w": w, "h": h, "mean": mean, "std": var ** 0.5,
            "colors": len(colors), "md5": hashlib.md5(data).hexdigest(), "bytes": len(data)}


def DiffRatio(a, b):
    if not a or not b or len(a["raw"]) != len(b["raw"]):
        return 1.0
    d = 0
    step = 11 * 3
    n = 0
    ra, rb = a["raw"], b["raw"]
    for i in range(0, len(ra) - 2, step):
        n += 1
        if abs(ra[i] - rb[i]) > 12 or abs(ra[i + 1] - rb[i + 1]) > 12 or abs(ra[i + 2] - rb[i + 2]) > 12:
            d += 1
    return d / n if n else 1.0


def DiffRatioRegion(a, b, x0, y0, x1, y1):
    """只比较指定矩形（设备像素）。小面积变化（侧栏用户区）用整图比会被稀释到 0.1%。"""
    if not a or not b or a["w"] != b["w"] or a["h"] != b["h"]:
        return 1.0
    w, h, ra, rb = a["w"], a["h"], a["raw"], b["raw"]
    x1, y1 = min(x1, w), min(y1, h)
    n = d = 0
    for y in range(max(0, y0), y1, 3):
        base = y * w * 3
        for x in range(max(0, x0), x1, 3):
            i = base + x * 3
            n += 1
            if abs(ra[i] - rb[i]) > 12 or abs(ra[i + 1] - rb[i + 1]) > 12 or abs(ra[i + 2] - rb[i + 2]) > 12:
                d += 1
    return d / n if n else 1.0


def Load(path):
    info = ImageInfo(path)
    if not info:
        return None
    with open(path, "rb") as f:
        _ = f.read(1)
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
    info["raw"] = raw
    return info


def NonBlank(info, min_std=5.0, min_colors=12):
    return bool(info) and info["std"] >= min_std and info["colors"] >= min_colors


def FileMd5(path):
    try:
        h = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()[:12]
    except Exception:
        return "?"


def OpenPath(path):
    try:
        if IS_WIN:
            os.startfile(path)      # noqa: S606 - 打开结果目录，不是执行文件
        else:
            subprocess.Popen(["xdg-open", path])
    except Exception:
        pass


def Main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "shots", "suite"))
    ap.add_argument("--only", default="")
    ap.add_argument("--quiet", default="auto", choices=list(MODES),
                    help="静默执行方式：auto/hidden/minimized/offscreen/off（默认 auto）")
    ap.add_argument("--priority", default="below", choices=["below", "idle", "normal"],
                    help="本进程与用例进程的调度优先级（默认 below，给前台应用让路）")
    ap.add_argument("--report-dir", default="",
                    help="结果目录（默认 cpp/build_logs/ui_suite_<时间戳>）")
    traygrp = ap.add_mutually_exclusive_group()
    traygrp.add_argument("--tray", dest="tray", action="store_true", default=None,
                         help="强制启用托盘进度（默认仅在交互式终端下启用）")
    traygrp.add_argument("--no-tray", dest="tray", action="store_false")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("[x] exe not found: " + args.exe)
        return 2

    stamp = time.strftime("%Y%m%d-%H%M%S")
    repdir = args.report_dir or os.path.join(ROOT, "cpp", "build_logs", "ui_suite_" + stamp)
    reporter = Reporter(repdir)
    reporter.meta.update({
        "可执行文件": "%s（%dKB, md5 %s）" % (args.exe, os.path.getsize(args.exe) // 1024,
                                          FileMd5(args.exe)),
        "截图目录": args.out,
        "进程优先级": args.priority,
    })
    ApplyOwnPriority(args.priority)

    interactive = args.tray if args.tray is not None else (
        IS_WIN and not os.environ.get("CI") and sys.stdout.isatty())
    # 控制台可能是 GBK：先统一成 utf-8 + replace，再 tee，否则日志里会出现编码异常
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.stdout = Tee(sys.stdout, reporter)

    r = Runner(args.exe, args.out, quiet_mode=args.quiet, priority=args.priority,
               probe_dir=os.path.join(repdir, "_probe"))
    CONTEXT["runner"] = r
    CONTEXT["reporter"] = reporter
    mode = r.q.resolve()
    reporter.meta["静默方式"] = "%s — %s" % (mode, r.q.note)
    print("[i] 静默方式：%s（%s）" % (mode, r.q.note))

    if interactive:
        tray = Tray("JMComic UI 回归", on_activate=lambda: OpenPath(repdir))
        if tray.start():
            CONTEXT["tray"] = tray
            print("[i] 托盘：已启用（双击图标打开结果目录）")
        else:
            print("[i] 托盘：不可用（%s），继续静默运行" % (tray.error or "未知原因"))
    print("[i] 结果目录：" + repdir)
    raw_only = set(x.strip() for x in args.only.split(",") if x.strip())
    global OnlyLayers
    OnlyLayers = raw_only & set(LAYERS)
    only = raw_only - set(LAYERS)
    if raw_only and not only and not OnlyLayers:
        print("[x] --only 未知标签：%s（可用：%s 或章节标签）"
              % (",".join(sorted(raw_only)), "|".join(LAYERS)))
        return 2
    if OnlyLayers:
        print("[i] 层过滤：%s（离线层必须全过；联网层允许 SKIP）" % ",".join(sorted(OnlyLayers)))

    def want(tag):
        return not only or tag in only

    print("== 1. 页面覆盖（21 页）")
    base = {}
    if want("pages") and WantLayer("offline"):
        for p in PAGES:
            rc, _, err, path = r.shot("page_" + p, ["--page", p])
            info = Load(path)
            ok = rc == 0 and NonBlank(info)
            if p == "home" and info:
                base["home"] = info
            Check(ok, "页面 %s 渲染" % p,
                  "" if ok else ("rc=%d std=%.1f colors=%d err=%s" %
                                 (rc, info["std"] if info else -1,
                                  info["colors"] if info else -1, err.strip()[-160:])))
        sizes = set()
        for p in PAGES:
            i = ImageInfo(os.path.join(r.out, "page_" + p + ".png"))
            if i:
                sizes.add((i["w"], i["h"]))
        # DPR 1 / 1.5 / 2 都算合法：Windows 缩放 150% 时截图是 1920×1260，
        # 断言的本意是"所有页同一套逻辑尺寸"，不是钉死 200% 缩放。
        scales = {(1280, 840), (1920, 1260), (2560, 1680)}
        Check(len(sizes) == 1 and sizes.issubset(scales),
              "页面截图尺寸一致（1280×840 / 1.5x / @2x 2560×1680）", str(sizes))

    print("== 2. 首页首屏（真实接口）")
    if want("paging") and WantLayer("net"):
        # 演示数据已移除：首页只由 /latest + /promote 填充，因此本节改为联网断言。
        rc, out, err, path = r.shot("home_first", ["--page", "home"], delay="30000", timeout=120)
        m = re.search(r"homeRows=(\d+)", err)
        rs = re.search(r"real-data: sections=(\d+) latest=(\d+)", err)
        if m is None and rs is None:
            Skip("首页首屏渲染", "接口不可用或超时")
        else:
            NetCheck(rc == 0 and m is not None and int(m.group(1)) >= 18,
                     "首页首屏渲染 ≥18 行真实数据", (m.group(0) if m else err.strip()[-120:]))

    print("== 3. 关键交互流程")
    if want("flows") and WantLayer("offline"):
        rc, _, err, p_home = r.shot("flow_home_base", ["--page", "home"])
        rc, _, err, p_dlg = r.shot("flow_dialog", ["--page", "history", "--demo-dialog"])
        i_dlg = Load(p_dlg)
        rc2, _, _, p_hist = r.shot("flow_history", ["--page", "history"])
        Check(rc == 0 and NonBlank(i_dlg) and DiffRatio(Load(p_hist), i_dlg) > 0.05,
              "确认框弹出并覆盖页面")

        # 屏蔽词：走真实侧栏动作行（不再注入预置词）
        rc, _, err, p_words = r.shot("flow_hidewords",
                                     ["--page", "home", "--nav-action", "hide"], delay="900")
        i_words = Load(p_words)
        Check(rc == 0 and NonBlank(i_words) and DiffRatio(Load(p_home), i_words) > 0.03,
              "屏蔽词输入框弹出")

        rc, _, err, p_col = r.shot("flow_collapse", ["--page", "home", "--demo-collapse"], delay="700")
        Check(rc == 0 and DiffRatio(Load(p_home), Load(p_col)) > 0.02, "侧栏收起")

        rc, _, err, p_end = r.shot("flow_nav_end", ["--page", "home", "--demo-nav-end"], delay="700")
        m_nav = re.search(r"nav-scroll: max=(\d+) value=(\d+)", err)
        nav_ok = (m_nav is not None
                  and (int(m_nav.group(1)) == 0
                       or int(m_nav.group(2)) == int(m_nav.group(1))))
        Check(rc == 0 and nav_ok, "侧栏可滚到末端（内容不足一屏时 max=0）",
              (m_nav.group(0) if m_nav else err.strip()[-120:]))

        rc, _, err, p_drag = r.shot("flow_edge_drag", ["--page", "detail", "--demo-drag", "120"],
                                    delay="900")
        m = re.search(r"edge-drag dx=120 -> page=(\d+)", err)
        Check(rc == 0 and m is not None and int(m.group(1)) != 3,   # 3 = Detail
              "边缘右滑返回（detail 起手）", (m.group(0) if m else err.strip()[-120:]))

        rc, _, err, p_back = r.shot("flow_back_swipe", ["--page", "detail", "--demo-back-swipe"],
                                    delay="900")
        line = next((l for l in err.splitlines() if "back-swipe" in l), "")
        Check(rc == 0 and "navigated back" in err, "返回手势导航成功", line)

        rc, _, err, p_anim = r.shot("flow_anim50", ["--page", "settings",
                                                    "--anim-from", "home", "--anim-progress", "0.5"],
                                    delay="700")
        i_anim = Load(p_anim)
        half = None
        if i_anim and i_anim["raw"]:
            w, h = i_anim["w"], i_anim["h"]
            ra = i_anim["raw"]
            diffcols = 0
            for x in range(0, w, 17):
                for y in range(0, h, 23):
                    i = (y * w + x) * 3
                    j = (y * w + (x + 1) % w) * 3
                    if abs(ra[i] - ra[j]) > 20:
                        diffcols += 1
            half = diffcols > 50
        Check(rc == 0 and NonBlank(i_anim) and bool(half), "QML 换页动画冻结在 50%")

    if want("flows") and WantLayer("net"):
        # 搜索链路只能由 /search 产生结果：联网层断言"换页 + 结果非空"
        rc, _, err, p_search = r.shot("flow_search", ["--real-search", "恋爱"],
                                      delay="15000", timeout=90)
        m = re.search(r"search-real: kw=\S+ page=(\d+) items=(\d+) total=(\d+)", err)
        if m is None:
            Skip("搜索链路", "接口不可用或超时")
        else:
            NetCheck(rc == 0 and int(m.group(2)) > 0,
                     "搜索返回服务端结果", m.group(0))

    print("== 4. 移动端布局（抽屉 / 触摸）")
    if want("mobile") and WantLayer("offline"):
        rc, out, err, path = r.shot("mobile_home", ["--page", "home", "--win-size", "420x860",
                                                     "--drawer"], delay="700")
        info = Load(path)
        Check(rc == 0 and info is not None
              and (info["w"], info["h"]) in ((420, 860), (630, 1290), (840, 1720)),
              "420×860 竖屏截图尺寸", str((info["w"], info["h"])) if info else "none")
        rc, out, err, path = r.shot("mobile_drawer", ["--page", "home", "--win-size", "420x860",
                                                      "--demo-drawer"], delay="900")
        Check(rc == 0 and NonBlank(Load(path)), "抽屉展开渲染")
        rc2, out2, err2 = r.run(["--page", "home", "--win-size", "420x860", "--drawer-selftest"])
        lines = [l for l in (out2 + err2).splitlines() if "selftest:" in l]
        Check(rc2 == 0 and "open=1" in (out2 + err2) and "esc_closed=1" in (out2 + err2)
              and "scrim_closed=1" in (out2 + err2),
              "抽屉自检 open/Esc/遮罩", " | ".join(lines)[-120:])
        rc, out, err, path = r.shot("mobile_safearea", ["--page", "home", "--touch",
                                                        "--safe-insets", "0,48,0,0"], delay="700")
        Check(rc == 0 and NonBlank(Load(path)), "安全区（刘海 48）不压内容")
        rc, out, err, path = r.shot("mobile_narrow", ["--page", "home", "--win-size", "320x480",
                                                      "--drawer"], delay="700")
        Check(rc == 0 and NonBlank(Load(path)), "最小窗口 320×480 不崩")

    print("== 5. 主题 / 字体 / 大列表 / 异常参数")
    if want("theme") and WantLayer("offline"):
        hashes = {}
        for t in ["dark_orange", "light_orange", "dark_pink", "light_pink", "dark_teal", "light_teal"]:
            rc, _, err, path = r.shot("theme_" + t, ["--page", "settings", "--theme", t])
            info = Load(path)
            ok = rc == 0 and NonBlank(info)
            hashes[t] = info["md5"] if info else "x"
            Check(ok, "主题 %s" % t)
        Check(len(set(hashes.values())) == 6, "六套主题两两不同",
              str(len(set(hashes.values()))))

    if want("font") and WantLayer("offline"):
        rc, _, err, p_custom = r.shot("font_custom", ["--page", "settings", "--font-family",
                                                      "Segoe UI", "--font-weight", "700",
                                                      "--font-width", "120", "--font-size", "12"])
        Check(rc == 0 and NonBlank(Load(p_custom)), "自定义字体（家族/字重/字宽/字号）")
        rc, _, err, p_reset = r.shot("font_reset", ["--page", "settings", "--font-reset"])
        Check(rc == 0 and NonBlank(Load(p_reset)), "恢复跟随系统字体")
        rc, _, err, p_sys = r.shot("font_syschange", ["--page", "home",
                                                      "--demo-sysfont", "Segoe UI,11"],
                                   delay="1200")
        line = next((l for l in err.splitlines() if "forced system font" in l), "")
        Check(rc == 0 and line and NonBlank(Load(p_sys)),
              "运行期系统字体变更后重排", line)

    if want("edge") and WantLayer("offline"):
        # 注意：不带 --shot 时 app 会停在事件循环里（这是正常行为），边界用例一律带截图退出
        rc, _, err, p = r.shot("edge_bad_page", ["--page", "不存在的页面"])
        Check(rc == 0 and NonBlank(Load(p)), "非法 --page 回退首页且不崩", "rc=%d" % rc)
        rc, _, err, p = r.shot("edge_bad_theme", ["--theme", "不存在的主题"])
        Check(rc == 0 and NonBlank(Load(p)), "非法 --theme 不崩", "rc=%d" % rc)
        rc, out, err = r.run(["--net-probe", "https://example.com"], timeout=30)
        tail = err.strip().splitlines()[-1] if err.strip() else ""
        Check(rc == 0 and "net-probe:" in err, "网络不可用时 net-probe 优雅退出", tail)

    print("== 6. 本轮新增能力（图标 / 会话 / 平板 / 横屏阅读 / WebP / 真实接口）")
    if want("new") and WantLayer("offline"):
        rc, _, err, p_set = r.shot("new_settings", ["--page", "settings"])
        Check(rc == 0 and "qt.svg" not in err, "主题图标不再缺资源（无 qt.svg 警告）",
              [l for l in err.splitlines() if "qt.svg" in l][:1])

        # ---- 会话落库：残留的"无 AVS 登录态"必须被丢弃并回写 ----
        for p in (r.session, r.session + ".bak"):
            if os.path.exists(p):
                os.remove(p)
        rc, _, err, p_out = r.shot("new_logged_out", ["--page", "home", "--data-report"])
        m = re.search(r"data-report: loggedIn=(\d+) name=(\S*)", err)
        Check(rc == 0 and m is not None and m.group(1) == "0",
              "无会话文件时侧栏为未登录", (m.group(0) if m else err[-120:]))

        # 旧版本"演示登录"写下的 session.ini：loggedIn=true 但 avs 为空。
        # 这种记录不是真实会话，Load 必须丢弃账号字段并回写清理。
        os.makedirs(os.path.dirname(r.session), exist_ok=True)
        with open(r.session, "w", encoding="utf-8") as f:
            f.write("[user]\nname=南野\ntitle=Lv.7 · 收藏家\ncoins=1280\nlevel=7\n"
                    "favorites=42\nfavoritesMax=0\nexp=0\nnextExp=0\n"
                    "proxy=自动\nproxyImg=自动\nhideWords=@Invalid()\n\n"
                    "[session]\nloggedIn=true\navs=\nuid=0\nsignedToday=false\n")
        rc, _, err, _ = r.shot("new_stale_session", ["--page", "home", "--data-report"],
                               delay="900")
        txt = ""
        if os.path.exists(r.session):
            with open(r.session, encoding="utf-8") as f:
                txt = f.read()
        Check(rc == 0 and "loggedIn=0 name=" in err and "南野" not in txt,
              "残留演示登录态被丢弃并回写", err.splitlines()[-1] if err else "")

        # ---- 平板断点 ----
        rc, _, err, p_tab = r.shot("new_tablet", ["--page", "home", "--win-size", "800x1280",
                                                 "--touch"], delay="900")
        i_tab = Load(p_tab)
        Check("layout: w=800 compact=0 rail=1" in err, "800dp 走常驻侧栏（非抽屉）",
              [l for l in err.splitlines() if "layout:" in l][:1])
        Check(i_tab is not None
              and (i_tab["w"], i_tab["h"]) in ((800, 1280), (1200, 1920), (1600, 2560)),
              "平板截图尺寸", str((i_tab["w"], i_tab["h"])) if i_tab else "none")

        # 横屏适宽/适高探针改到第 7 节：现在必须有真实章节图片才会触发量测
        # （演示页已移除，空阅读器不再生成可缩放页面）。


        # ---- 真实 /promote + /latest（联网层：拿不到输出才 skip）----
        if WantLayer("net"):
            err = ""
            for attempt in range(2):
                rc, _, err, p_real = r.shot("new_real_data", ["--page", "home"],
                                            delay="45000", timeout=90)
                if re.search(r"real-data: sections=\d+ latest=\d+", err):
                    break
                time.sleep(3)
            m = re.search(r"real-data: sections=(\d+) latest=(\d+)", err)
            if m is None:
                Skip("真实接口首页", "接口不可用或超时")
            else:
                i_real = Load(p_real)
                i_base = Load(os.path.join(r.out, "page_home.png"))
                NetCheck(int(m.group(1)) >= 2 and int(m.group(2)) > 0 and i_real is not None
                         and DiffRatio(i_base, i_real) > 0.15,
                         "真实接口首页（/promote + /latest）", m.group(0))


    print("== 7. 业务闭环（/album 详情 / /chapter 阅读 / /search / 登录失败路径 / latest 翻页）")
    # 整节都是联网用例：--only offline 时跳过，避免"断网跑法"被真实接口拖慢
    if want("biz") and WantLayer("net"):
        # ---- 真实详情：章节列表 + 真实封面（接口可用才断言）----
        err = ""
        for attempt in range(2):
            rc, _, err, p_det = r.shot("biz_detail",
                                       ["--page", "home", "--open-album", "313833"],
                                       delay="25000", timeout=90)
            if re.search(r"detail-real: id=313833 chapters=\d+", err):
                break
            time.sleep(3)
        m = re.search(r"detail-real: id=313833 chapters=(\d+)", err)
        if m is None:
            Skip("真实详情页", "接口不可用")
        else:
            i_det = Load(p_det)
            i_demo = Load(os.path.join(r.out, "page_detail.png"))
            diff = DiffRatio(i_demo, i_det) if i_demo is not None else 1.0
            # 详情页布局与演示页一致，差异只来自文本/封面 → 阈值取 0.05
            NetCheck(int(m.group(1)) >= 10 and i_det is not None and NonBlank(i_det)
                     and diff > 0.05,
                     "真实详情页（章节数与画面差异）",
                     "chapters=%s diff=%.2f" % (m.group(1), diff))

        # ---- 真实章节阅读：下载 + 还原 ----
        err = ""
        for attempt in range(2):
            rc, _, err, p_rd = r.shot("biz_reader",
                                      ["--page", "home", "--open-chapter", "313833",
                                       "--win-size", "900x520"],
                                      # /chapter + chapter_view_template 两次请求各 40s 预算，
                                      # 弱网下整链可能到 ~70s，delay 给足避免"还没回来就截图退出"
                                      delay="75000", timeout=170,
                                      env_extra={"JM_READER_PROBE": "1"})
            if re.search(r"chapter-shown: pages=(\d+)", err):
                break
            time.sleep(3)
        m = re.search(r"chapter-shown: pages=(\d+)", err)
        if m is None:
            tail = [l for l in err.splitlines() if l.startswith(("pages:", "chapter", "api:"))]
            Skip("真实章节阅读", tail[-3:] if tail else "接口不可用")
        else:
            i_rd = Load(p_rd)
            i_demo = Load(os.path.join(r.out, "page_reader.png"))
            diff = DiffRatio(i_demo, i_rd) if i_demo is not None else 1.0
            probe = next((l for l in err.splitlines() if l.startswith("reader-probe:")), "")
            if probe:
                pm = re.search(r"viewport=(\d+)x(\d+) target=(\d+)x(\d+)", probe)
                NetCheck(pm is not None and int(pm.group(4)) <= int(pm.group(2)) + 2,
                         "横屏整页可见（target 高 ≤ 视口高）", probe)
            else:
                Skip("横屏整页可见（target 高 ≤ 视口高）", "章节已加载但未触发放缩量测")
            NetCheck(int(m.group(1)) > 0 and i_rd is not None and NonBlank(i_rd) and diff > 0.10,
                     "真实章节阅读（下载还原后渲染成功）",
                     "pages=%s diff=%.2f" % (m.group(1), diff))

        # ---- 真实搜索 ----
        rc, _, err, p_se = r.shot("biz_search", ["--page", "home", "--real-search", "love"],
                                  delay="22000", timeout=90)
        m = re.search(r"search-real: kw=.* page=1 items=(\d+) total=(\d+)", err)
        if m is None:
            Skip("真实搜索", "接口不可用")
        else:
            NetCheck(int(m.group(1)) > 0 and int(m.group(2)) >= int(m.group(1)),
                     "真实搜索（hits/total）", m.group(0))

        # ---- 登录失败路径（不依赖账号：错误凭据也应收到服务端 401）----
        rc, _, err, _ = r.shot("biz_login_fail",
                               ["--page", "home", "--login", "wronguser_probe:wrongpass"],
                               delay="18000", timeout=90)
        m = re.search(r"login-fail: (.+)", err)
        if m is not None:
            NetCheck(True, "真实登录失败路径（服务端拒绝并回显）", m.group(0))
        elif "api: api POST" in err:
            # 请求确实发出，但没等到响应：属于链路问题，不能记 PASS
            Skip("真实登录失败路径", "响应超时")
        else:
            Skip("真实登录失败路径", "接口不可用")


        # ---- 首页 /latest 翻页 ----
        err = ""
        for attempt in range(2):
            rc, _, err, p_lm = r.shot("biz_latest_more",
                                      ["--page", "home", "--latest-more"],
                                      delay="48000", timeout=150)
            if re.search(r"latest-more: page=1 items=(\d+)", err):
                break
            time.sleep(3)
        m = re.search(r"latest-more: page=1 items=(\d+)", err)
        if m is None:
            tail = [l for l in err.splitlines() if l.startswith(("api:", "real-data:", "latest"))]
            Skip("首页 /latest 第 2 页追加", tail[-3:] if tail else "接口不可用")
        else:
            NetCheck(int(m.group(1)) > 0, "首页 /latest 第 2 页追加", m.group(0))


    print("== 8. 分流与节点（页面 / 测速 / 节点作用于真实流量）")
    if want("proxy"):
        # ---- 页面化：从主页切到分流页，画面应显著不同 ----
        rc, _, err, p_px = r.shot("proxy_page", ["--page", "proxy"], delay="1200", timeout=40)
        i_px = Load(p_px)
        i_home = Load(os.path.join(r.out, "page_home.png"))
        if i_px is not None and NonBlank(i_px):
            diff = DiffRatio(i_home, i_px) if i_home is not None else 1.0
            # 首页在无网络时不渲染卡片，两页都是深色底 → 像素差异比"有内容"时小，
            # 阈值按 0.02 取；关键是 layout/标题确实换成了分流页。
            Check(diff > 0.02, "分流页可作为一级页面打开（非模态对话框）",
                  "diff=%.2f" % diff)
        else:
            Check(False, "分流页可作为一级页面打开（非模态对话框）", "截图缺失")

        # ---- 排序/选优策略（不联网，确定性）----
        rc, _, err, _ = r.shot("proxy_sort", ["--page", "home", "--proxy-selftest"],
                               delay="900", timeout=40)
        m = re.search(r"proxy-selftest: order=(\S+) best=(\S+) keep=(\S+) keepfail=(\S+)", err)
        ok = (m is not None
              and m.group(1) == "10.0.0.2,10.0.0.1,10.0.0.9,10.0.0.3"
              and m.group(2) == "10.0.0.2"    # Python CheckSelectBox：无选中取排序第一
              and m.group(3) == "10.0.0.3"    # 已选且图片失败：保持
              and m.group(4) == "10.0.0.9")   # 已选且 ping 失败：仍保持（Python 语义）
        Check(ok, "节点排序与选优策略（Python IpItem.__lt__ + CheckSelectBox）",
              m.group(0) if m is not None else "未输出 proxy-selftest")

        # ---- 节点测速线程池（离线确定性自检：不联网、不读写 ui.ini）----
        # 注意别覆盖上面的 err：后续 proxy-align 断言复用的就是 proxy_sort 的输出
        rc_b, _, err_b, _ = r.shot("node_bench_selftest", ["--node-bench-selftest"],
                                   delay="300", timeout=40)
        m_bench = re.search(r"node-bench: all=(PASS|FAIL)", err_b)
        m_cap = re.search(r"node-bench: cap batch tasks=(\d+) threads=(\d+) peak=(\d+)", err_b)
        m_cancel = re.search(r"node-bench: cancel tasks=100 executed=(\d+)", err_b)
        ok_bench = (m_bench is not None and m_bench.group(1) == "PASS"
                    and m_cap is not None and int(m_cap.group(2)) == 16
                    and int(m_cap.group(3)) <= 16
                    and m_cancel is not None and int(m_cancel.group(1)) < 100)
        Check(ok_bench,
              "节点测速线程池（16 上限 / 任务唯一 / 同序汇总 / 异常超时取消）",
              m_bench.group(0) if m_bench is not None else "未输出 node-bench-selftest")

        # ---- 与 Python 版对齐的确定性事实 ----
        a = re.search(r"proxy-align: lines=(\d+) order=(\S+) labels=", err)
        Check(a is not None and a.group(1) == "6" and a.group(2) == "1,2,3,4,6,5",
              "线路表 6 条且页面顺序与 Python 一致",
              a.group(0) if a is not None else "未输出 proxy-align lines")
        a = re.search(r"proxy-align: nodes=(\d+) proxy=(\d+) custom=(\d+) cf=(\d+) first=(\S+) last=(\S+)", err)
        Check(a is not None and a.group(1) == "56" and a.group(2) == "26"
              and a.group(3) == "6" and a.group(4) == "24",
              "三类 IP 节点配置/顺序（ProxyIP 26 + 自定义 6 + 优选 24）",
              a.group(0) if a is not None else "未输出 proxy-align nodes")
        a = re.search(r"proxy-align: filter proxy=(\d+) cf=(\d+)", err)
        Check(a is not None and a.group(1) == "32" and a.group(2) == "24",
              "节点筛选匹配（ProxyIP+自定义 / 优选）",
              a.group(0) if a is not None else "未输出 proxy-align filter")
        a = re.search(r"proxy-align: defaults api=(\d+) img=(\d+) ip=(\S+) mode=(\d+) apiUrl=(\S+) imgUrl=(\S+)", err)
        Check(a is not None and a.group(1) == "1" and a.group(2) == "1"
              and a.group(3) == "-" and a.group(4) == "0"
              and a.group(5).endswith("www.cdnhjk.net")
              and a.group(6).endswith("cdn-msp.jmapiproxy1.cc"),
              "默认选择行为（线路 1 / 无节点 / 无代理）",
              a.group(0) if a is not None else "未输出 proxy-align defaults")
        a = re.search(r"proxy-align: fallback api7=(\S+) img0=(\S+)", err)
        Check(a is not None and a.group(1).endswith("www.cdnhjk.net")
              and a.group(2).endswith("cdn-msp.jmapiproxy1.cc"),
              "异常索引回退（>=7 与 <=0 均落到线路 1）",
              a.group(0) if a is not None else "未输出 proxy-align fallback")
        a = re.search(r"proxy-align: route line=(\d+) viaNode=(\d+) reason=(\S+)", err)
        Check(a is not None and a.group(1) == "1" and a.group(2) == "0",
              "请求路由默认为域名直连",
              a.group(0) if a is not None else "未输出 proxy-align route")

        # ---- 以下为联网用例：--only offline 时整段跳过 ----
        node_ip = ""
        if WantLayer("net"):
            # ---- 线路测速：覆盖全部 6 条可选线路（2 次采样取最优）----
            # 6 条线路各自 2 次 ping + 2 次 speed，网络差时会顶到 40s 以上
            rc, _, err, _ = r.shot("proxy_lines", ["--page", "proxy", "--proxy-test", "lines"],
                                   delay="95000", timeout=180)
            m = re.search(r"line-test: (\d+) lines", err)
            if m is None:
                Skip("分流线路测速", "接口不可用")
            else:
                NetCheck(int(m.group(1)) == 6 and "line-test done: 6 lines" in err,
                         "分流线路测速（6 条全覆盖 + 稳定采样）", m.group(0))

            # ---- 节点测速：HTTPS + SNI 探测，产出排序与选中 ----
            # 56 个节点 / 并发 8 / 单次 3s：两阶段最坏 ~50s + 获取 10s
            rc, _, err, _ = r.shot("proxy_nodes", ["--page", "proxy", "--proxy-test", "nodes"],
                                   delay="110000", timeout=200)
            m = re.search(r"node-test done: total=(\d+) ping_ok=(\d+) img_ok=(\d+) selected=(\S+)",
                          err)
            node_ip = m.group(4) if m is not None else ""
            if m is None:
                Skip("CDN 节点测速", "接口不可用")
            elif int(m.group(2)) < 1 or node_ip in ("", "-"):
                # 有输出但一个可达节点都没有：环境问题，不能算通过
                Skip("CDN 节点测速", m.group(0))
            else:
                NetCheck(True, "CDN 节点测速（0.8*延迟+0.2*下载 排序后自动选中）", m.group(0))

            # ---- 选中节点作用于真实 API 流量（连接落 IP、SNI 仍是域名）----
            # 节点可达性受网络影响很大（实测同一 IP 时好时坏）：
            #   * 拿到 detail-real → PASS（证明整条链路通）
            #   * 请求确实走了节点（via node + sni）但超时 → SKIP（环境问题，不伪造 PASS）
            if node_ip and node_ip != "-":
                cfg_dir = os.path.join(r.out, "_cfg_node")
                shutil.rmtree(cfg_dir, ignore_errors=True)
                rc, _, err, p_nd = r.shot(
                    "proxy_node_traffic",
                    ["--page", "home", "--use-node", node_ip, "--open-album", "313833"],
                    delay="30000", timeout=90, env_extra={"JM_UI_CONFIG": cfg_dir})
                m2 = re.search(r"detail-real: id=313833 chapters=(\d+)", err)
                if m2 is not None:
                    NetCheck(int(m2.group(1)) > 0,
                             "选中节点作用于真实 API 流量（/album 经节点返回）",
                             "node=%s %s" % (node_ip, m2.group(0)))
                elif ("via node " + node_ip) in err and "sni=" in err:
                    tail = [l for l in err.splitlines() if "node-http" in l or "album-fail" in l]
                    Skip("节点作用于真实流量", tail[-2:] if tail else "节点在当前网络不可达")
                else:
                    # 没走节点：这是路由缺陷，必须 FAIL
                    Check(False, "选中节点作用于真实 API 流量（/album 经节点返回）",
                          "请求未走节点", layer="net")
            else:
                Skip("节点作用于真实流量", "没有可用节点")


    print("== 9. DOH / ECH / HTTP3")
    if want("advanced") or want("proxy"):
        # ---- 确定性：C++ 壳的配置默认值 + Qt 后端能力 + DNS wire 解析 ----
        # 0.90 补录（§14.7）：C++ 壳的 HTTP3 默认开（http3=1），刻意与
        # Python ProxySetting.IsOpenHTTP3 默认 0 分歧；其余字段仍同值。
        # 900ms 会在 dohquery 输出前就截图退出（stderr 被截断），留足打印余量
        rc, _, err, _ = r.shot("advanced_selftest", ["--page", "home", "--proxy-selftest"],
                               delay="2000", timeout=40)
        a = re.search(r"proxy-align: advanced doh=(\S+) active=(\d+) ech=(\d+) http3=(\d+) "
                      r"supported=(\d+) echOff=(\d+) http3Off=(\d+)", err)
        Check(a is not None and a.group(1) == "https://parse.jpacg.cc/parse"
              and a.group(2) == "0" and a.group(3) == "1" and a.group(4) == "1"
              and a.group(5) == "1" and a.group(6) == "0" and a.group(7) == "0"
              and "ech=1" in err and "echLib=1" in err
              and "quic=1" in err and "http3=1" in err and "stack=ngtcp2+nghttp3" in err,
              "DOH/ECH/HTTP3 配置默认与后端能力（ECH 通道 + QUIC 栈均可用）",
              a.group(0) if a is not None else "未输出 advanced")
        # Q5：自检里的回退/降级计数必须存在，且与页面 AdvancedStatus 同一份数据源
        c = re.search(r"proxy-align: counters h3ok=(\d+) h3fail=(\d+) h3fb=(\d+) "
                      r"echok=(\d+) echfail=(\d+)", err)
        Check(c is not None, "回退/降级计数可读（H3/ECH）",
              c.group(0) if c is not None else "未输出 proxy-align: counters")
        # ---- 网络：HTTP/3（QUIC）真实请求（ngtcp2 握手 + nghttp3 请求）----
        # 注意：不要复用 err —— 后面的 dohquery 断言还在用自检那次的 stderr。
        # 这条注释是回归防线：D2 之前这里复用了 err，H3 的 stderr 会把 dohquery 断言冲掉。
        if WantLayer("net"):
            rc, _, h3err, _ = r.shot("h3_probe", ["--page", "home", "--h3-probe", "cloudflare.com",
                                                  "--h3-path", "/"],
                                     delay="26000", timeout=80)
            m = re.search(r"h3-probe: host=\S+ path=\S+ ip=\S+ ech=\d+B backend=(\S+)", h3err)
            if m is None:
                Skip("HTTP/3 探针加载 ngtcp2/nghttp3 后端", "探针未启动")
            else:
                NetCheck(m.group(1) == "ngtcp2", "HTTP/3 探针加载 ngtcp2/nghttp3 后端",
                         m.group(0))
                m = re.search(r"h3-probe: done ok=(\d) status=(\d+) bytes=(\d+) (\d+)ms "
                              r"version=(\S+) alpn=(\S+) tls=(\S+)", h3err)
                if m is None:
                    Skip("HTTP/3 实际请求", "网络不可达")
                elif m.group(1) != "1" or int(m.group(2)) <= 0:
                    # 有输出但请求没成功：真失败（H3 侧回归），不是网络波动
                    Check(False, "HTTP/3 请求完成（HTTP/3 + h3 + TLS1.3）", m.group(0),
                          layer="net")
                else:
                    NetCheck(m.group(5) == "HTTP/3" and m.group(6) == "h3"
                             and m.group(7) == "TLSv1.3",
                             "HTTP/3 请求完成（HTTP/3 + h3 + TLS1.3）", m.group(0))

        a = re.search(r"proxy-align: dohquery len=(\d+) a=(\S+) ech=(\d+) wire=(\d+)", err)
        Check(a is not None and a.group(2) == "1.2.3.4" and a.group(3) == "1"
              and a.group(4) == "1",
              "DoH DNS wire 解析（A 记录 + HTTPS RR ech 参数）",
              a.group(0) if a is not None else "未输出 dohquery")

        rc, _, err, _ = r.shot("advanced_status", ["--page", "home", "--advanced-status"],
                               delay="900", timeout=40)
        Check("advanced-status:" in err and "DOH" in err and "ECH" in err and "HTTP3" in err,
              "高级设置状态文案（页面与自检同一份）",
              "advanced-status" if "advanced-status:" in err else "未输出")

        if WantLayer("net"):
            # ---- 网络：DoH 真的解析出 IP ----
            rc, _, err, _ = r.shot("doh_resolve",
                                   ["--page", "home", "--doh-test", "www.cdnhjk.net"],
                                   delay="12000", timeout=60)
            m = re.search(r"doh-test: host=\S+ viaDoh=(\d+) ips=(\S+)", err)
            if m is None:
                Skip("DoH 解析真实域名（JSON/HTTPS）", "DoH 服务不可用")
            else:
                NetCheck(m.group(1) == "1" and "," in m.group(2),
                         "DoH 解析真实域名（JSON/HTTPS）", m.group(0))
            m_ech = re.search(r"ech config ok len=(\d+)", err)
            if m_ech:
                NetCheck(True, "ECH 配置经 DoH HTTPS RR 获取", m_ech.group(0))
            else:
                Skip("ECH 配置获取", "DoH HTTPS RR 不可用")

            # ---- ECH 真实握手：SSL_ECH_STATUS_SUCCESS + TLS1.3 + 证书校验 ----
            rc, _, err, _ = r.shot("ech_probe",
                                   ["--page", "home", "--ech-probe", "www.cdnhjk.net"],
                                   delay="16000", timeout=70)
            m = re.search(r"ech-probe: host=\S+ ok ech=(\S+)\((-?\d+)\) version=(\S+) "
                          r"alpn=(\S+) verify=(\d)", err)
            if m is None:
                Skip("ECH 真实握手（SUCCESS + TLS1.3 + 证书校验）", "网络/ECH 服务不可用")
            else:
                # 拿到状态就严格判定：FAILED 说明 ECH 真没成功，不能 skip 掉
                NetCheck(m.group(1) == "SUCCESS" and m.group(3).startswith("TLSv1.3")
                         and m.group(5) == "1",
                         "ECH 真实握手（SUCCESS + TLS1.3 + 证书校验）", m.group(0))

            # ---- 连接池：同域 N 个并发请求只应发生 1 次 QUIC 握手（Q2）----
            pool_cfg = os.path.join(r.out, "_cfg_pool")
            shutil.rmtree(pool_cfg, ignore_errors=True)
            rc, _, perr, _ = r.shot(
                "h3_pool", ["--page", "home", "--h3-burst", "10",
                            "--h3-probe", "cloudflare.com", "--h3-path", "/"],
                delay="30000", timeout=90, env_extra={"JM_UI_CONFIG": pool_cfg})
            m = re.search(r"h3-burst: n=(\d+) ok=(\d+) handshakes=(\d+) reused=(\d+) "
                          r"conns=(\d+) jobs=(\d+) queued=(\d+)", perr)
            if m is None:
                Skip("连接池复用（10 并发 / 1 次握手）", "未输出 h3-burst")
            else:
                NetCheck(int(m.group(2)) == int(m.group(1)) and int(m.group(3)) == 1
                         and int(m.group(4)) >= int(m.group(1)) - 1,
                         "连接池复用（10 并发 / 1 次握手）", m.group(0))

            # ---- T2：证书错误必须失败（QUIC 不得跳过校验）----
            bad_ca = os.path.join(ROOT, "docs", "testdata", "tls", "not-a-ca.pem")
            if os.path.exists(bad_ca):
                rc, _, terr, _ = r.shot(
                    "h3_bad_ca", ["--page", "home", "--h3-probe", "cloudflare.com",
                                  "--h3-path", "/"],
                    delay="15000", timeout=60, env_extra={"JM_CA_BUNDLE": bad_ca})
                # 注意别用 $ 收尾：stderr 后面还有别的行，非 MULTILINE 下 $ 只认全文末尾
                mt = re.search(r"h3-probe: done ok=(\d) status=(\d+)[^\r\n]*err=([^\r\n]*)", terr)
                if mt is None:
                    Skip("证书校验不可被跳过（错误 CA 必失败）", "探针未启动")
                else:
                    NetCheck(mt.group(1) == "0"
                             and ("certificate verify failed" in mt.group(3)
                                  or "verify=" in mt.group(3)),
                             "证书校验不可被跳过（错误 CA 必失败）", mt.group(3)[:120])

            # ---- 网络：DoH 解析结果作用于真实 API（连接 IP + 域名 SNI）----
            rc, _, err, _ = r.shot("doh_traffic",
                                   ["--page", "home", "--doh-on", "--open-album", "313833"],
                                   delay="30000", timeout=90)
            reached = "doh-on: host=" in err and "via node" in err
            if not reached:
                Skip("DoH 解析结果作用于 API 流量（/album 80 章）", "接口不可用")
            else:
                NetCheck("detail-real: id=313833" in err,
                         "DoH 解析结果作用于 API 流量（/album 80 章）",
                         "doh-on + via node + detail-real" if "detail-real: id=313833" in err
                         else "doh-on 已生效但详情未返回")
                if "node-http ech-tls" in err and "ech=SUCCESS" in err:
                    NetCheck(True, "API 流量经 ECH 通道（ech-tls + SUCCESS）",
                             "node-http ech-tls + ech=SUCCESS")
                else:
                    Skip("API 经 ECH 通道（ech-tls + SUCCESS）", "ECH 配置未就绪")

    print("== 10. 收尾布局（留白高度 / 开关同行）")
    if want("blank") and WantLayer("offline"):
        # 每页单独起一次进程：隐藏页的 QStackedWidget 子页没有真实布局，
        # 必须从"当前页"读数，否则开关行的紧凑布局不会被触发。
        reports = {}
        for pg in ("search", "settings"):
            rc, _, err, _ = r.shot("blank_" + pg,
                                   ["--page", pg, "--touch", "--win-size", "420x860",
                                    "--blank-report"], delay="800")
            reports[pg] = (rc, " | ".join(l for l in err.splitlines()
                                          if "blank-report:" in l))
        m_search = re.search(r"search headerH=(\d+) titleTextH=(\d+)", reports["search"][1])
        m_set = re.search(r"settings toggleRows=(\d+) sameLine=(\d+) maxCenterDy=(\d+)",
                          reports["settings"][1])
        Check(all(reports[p][0] == 0 for p in reports) and m_search and m_set,
              "blank-report 页面齐全", str(reports)[-200:])
        if m_search:
            h, t = int(m_search.group(1)), int(m_search.group(2))
            Check(t > 0 and t <= h <= t + 6,
                  "搜索页分组标题行仅比文字略高（%d vs %d）" % (h, t),
                  reports["search"][1][-180:])
        # 有结果时的统计行：它是搜索框与卡片墙之间唯一的一行字。曾经没有拉伸因子，
        # 富余高度被它和卡片墙平分 → 标签被拉到上千 px、文字垂直居中，屏幕上下
        # 各留一条比字高几十倍的空白带（0.91 用户报的"搜索页留白过大"）。
        rc, _, err, _ = r.shot("blank_search_result",
                               ["--page", "search", "--real-search", "恋爱", "--touch",
                                "--win-size", "420x860", "--blank-report"], delay="800")
        m_hint = re.search(r"search hintH=(\d+) hintTextH=(\d+)", err)
        Check(rc == 0 and bool(m_hint), "搜索页结果统计行可量测", (err or "")[-200:])
        if m_hint:
            h, t = int(m_hint.group(1)), int(m_hint.group(2))
            Check(t > 0 and t <= h <= t + 6,
                  "搜索页结果统计行仅占字高（%d vs %d）" % (h, t), err[-180:])
        if m_set:
            total, same, dy = (int(m_set.group(i)) for i in (1, 2, 3))
            Check(total >= 3 and same == total and dy <= 12,
                  "设置页开关全部与文字同排（%d/%d, 中心差 %dpx）" % (same, total, dy),
                  reports["settings"][1][-180:])

    # 首页底部状态行的留白只能在拿到真实数据后量（演示数据已移除）
    if want("blank") and WantLayer("net"):
        rc, _, err, _ = r.shot("blank_home",
                               ["--page", "home", "--touch", "--win-size", "420x860",
                                "--blank-report"], delay="15000", timeout=90)
        m_home = re.search(r"home pagerH=(\d+) pagerTextH=(\d+)", err)
        if rc != 0 or m_home is None or int(m_home.group(1)) <= 0:
            Skip("首页底部状态行高度", "首页数据未就绪")
        else:
            h, t = int(m_home.group(1)), int(m_home.group(2))
            # 状态行 = 文字 + 34px 的"加载更多"按钮，高度应被按钮约束而不是被拉伸
            NetCheck(t > 0 and h <= t + 40,
                     "首页底部状态行高度受按钮约束（%d vs %d）" % (h, t), m_home.group(0))

    print("== 11. 账号数据（未登录侧栏 / 会话失效清理）")
    if want("account"):
        # 本节从"未登录"起跑：清掉上一节可能写入的会话（含 .bak 回退副本）。
        for p in (r.session, r.session + ".bak"):
            if os.path.exists(p):
                os.remove(p)
        if WantLayer("offline"):
            # 未登录：三行必须是 —（不是 0），收藏页必须是"去登录"而不是空白/演示卡片
            rc, _, err, _ = r.shot("acct_anon", ["--page", "home", "--user-report"],
                                   delay="900")
            m = re.search(r"user-report: loggedIn=0 .*favState=not-logged-in cards=0", err)
            Check(rc == 0 and m, "未登录：侧栏无账号数据、收藏页给去登录",
                  err.splitlines()[-1] if err else "")

        if WantLayer("net"):
            # 会话失效：无效 AVS 打 /favorite 必须收到 401，随后清账号字段并落盘
            with open(r.session, "w", encoding="utf-8") as f:
                f.write("[user]\nname=expire_probe\ntitle=Lv.9\ncoins=999\nlevel=9\n"
                        "favorites=88\nfavoritesMax=500\nexp=1\nnextExp=2\n"
                        "proxy=自动\nproxyImg=自动\nhideWords=@Invalid()\n\n"
                        "[session]\nloggedIn=true\navs=expired-token\nuid=18240\n"
                        "signedToday=false\n")
            rc, _, err, _ = r.shot("acct_expired",
                                   ["--page", "home", "--user-report"],
                                   delay="40000", timeout=120)
            txt = ""
            if os.path.exists(r.session):
                with open(r.session, encoding="utf-8") as f:
                    txt = f.read()
            if "session: expired" in err:
                NetCheck("loggedIn=false" in txt and "name=expire_probe" not in txt,
                         "会话失效后清空账号数据并落盘", "session.ini loggedIn=false")
            elif "favorite-list: fail" in err:
                NetCheck(False, "会话失效识别（收到 401 未清理登录态）", err[-180:])
            else:
                Skip("会话失效识别", "接口不可用或超时")

    print("== 12. 搜索历史（写入 → 重启读回 → 清空）")
    if want("history") and WantLayer("offline"):
        # 关键词一律用 ASCII：Windows 控制台里 qWarning 的中文会乱码，
        # 断言只认真实落盘/读回的 ASCII 标识，避免编码造成的假失败。
        def hist_report(shot_name, extra):
            rc, _, err, _ = r.shot(shot_name, ["--page", "search"] + extra, delay="500")
            m = re.search(r"data-report: history=(\d+) first=(\S+)", err)
            return rc, int(m.group(1)) if m else -1, (m.group(2) if m else ""), err

        rc, n, first, err = hist_report("hist_clear", ["--history-clear", "--data-report"])
        Check(rc == 0 and n == 0 and first == "-",
              "搜索历史可清空（history=0）", err[-120:])

        r.shot("hist_a", ["--page", "search", "--real-search", "HistA"], delay="400")
        rc, n, first, err = hist_report("hist_read1", ["--data-report"])
        Check(rc == 0 and n == 1 and first == "HistA",
              "搜索一次后重启能读回（history=1 HistA）", err[-120:])

        r.shot("hist_b", ["--page", "search", "--real-search", "HistB"], delay="400")
        rc, n, first, err = hist_report("hist_read2", ["--data-report"])
        Check(rc == 0 and n == 2 and first == "HistB",
              "两条记录按 MRU 排列（history=2 HistB 在前）", err[-120:])

        ini = os.path.join(r.uicfg, "ui.ini")
        txt = ""
        if os.path.exists(ini):
            with open(ini, encoding="utf-8") as f:
                txt = f.read()
        Check("[search]" in txt and "recent=HistB, HistA" in txt,
              "ui.ini 里存在 [search] recent", txt[-120:])

        rc, n, first, err = hist_report("hist_clear2", ["--history-clear", "--data-report"])
        txt2 = ""
        if os.path.exists(ini):
            with open(ini, encoding="utf-8") as f:
                txt2 = f.read()
        Check(rc == 0 and n == 0 and "recent=" not in txt2,
              "清空后落盘且重启不再出现", txt2[-120:])

    print("")
    print("=" * 70)
    # 分段汇总：SKIP 必须单列，否则"全 PASS"会被误读成"全部验证过"
    for name in LAYERS:
        s = Stats[name]
        total = s["pass"] + s["fail"]
        if name == "offline":
            print("offline PASS %d/%d%s" % (s["pass"], total,
                                            "" if not s["fail"] else "  FAIL %d" % s["fail"]))
        else:
            print("net     PASS %d/%d  SKIP %d%s" % (s["pass"], total, s["skip"],
                                                     "" if not s["fail"] else "  FAIL %d" % s["fail"]))
    print("-" * 70)
    print("PASS %d / FAIL %d / SKIP %d" % (Passes, len(Failures), len(Skips)))
    for f in Failures:
        print("  - " + f)
    if Skips:
        print("[i] SKIP（未验证，不是通过）：")
        for s in Skips:
            print("  - " + s)
    print("[i] 报告：%s" % os.path.join(repdir, "report.md"))

    reporter.skips = list(Skips)
    reporter.meta["分层统计"] = "；".join(
        "%s PASS %d/%d SKIP %d" % (name, Stats[name]["pass"],
                                   Stats[name]["pass"] + Stats[name]["fail"],
                                   Stats[name]["skip"]) for name in LAYERS)
    stats = r.q.stats()

    reporter.meta["用例进程"] = ("%d 次 / 合计 %.1fs / p50 %.2fs / p95 %.2fs / 非零退出 %d"
                             % (stats.get("runs", 0), stats.get("total_s", 0),
                                stats.get("p50_s", 0), stats.get("p95_s", 0),
                                stats.get("nonzero_rc", 0))) if stats else "无"
    reporter.finish(Passes, Failures)

    tray = CONTEXT.get("tray")
    if tray is not None:
        tray.notify("UI 回归完成", "PASS %d / FAIL %d / SKIP %d"
                    % (Passes, len(Failures), len(Skips)))
        tray.set("JMComic UI 回归：完成 PASS %d / FAIL %d" % (Passes, len(Failures)))
    return 0 if not Failures else 1


if __name__ == "__main__":
    code = 1
    try:
        code = Main()
    except KeyboardInterrupt:
        print("\n[i] 已被用户中断（Ctrl+C）")
        code = 130
    finally:
        # 任何退出路径都要留下报告、摘掉托盘图标（否则会留下一个"幽灵"图标）
        reporter = CONTEXT.get("reporter")
        if reporter is not None:
            reporter.skips = list(Skips)
            reporter.finish(Passes, Failures)
        tray = CONTEXT.get("tray")
        if tray is not None:
            tray.stop()
    sys.exit(code)
