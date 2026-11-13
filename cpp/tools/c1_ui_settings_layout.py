# coding: utf-8
"""设置页布局回归：横向不溢出（hmax==0）+ 字重不改变几何。

为什么单独成脚本
----------------
0.91 修过一次"改字重/字号后整页被拉伸"的回归，成因有两层，都不是肉眼能稳住的：

  1. `SettingRow` 用写死的 `UiStyle::Fpx(430)` 当"横排/堆叠"的门槛。字号 11pt 时
     门槛涨到 526，比常见行宽（480）还大 → 整页被判成"窄屏" → 堆叠布局把控件拉到
     整行宽；而 `MakeSliderField` 里滑块用 `setFixedWidth(150)` 锁死，于是变成
     "滑块挤在左边、数值被顶到行尾、中间一大片空白"——用户看到的就是界面被拉伸。
  2. 该判定读的是"行当前宽度"且无迟滞，滚动条出现/消失（±10px）会让页面在
     "横排/堆叠"之间反复跳变。

两条机器断言
------------
  * overflow —— 任意「窗口宽 × 字号」组合下，设置页的 QScrollArea 必须
    `hmax == 0`，**并且** 内容控件宽度不得大于 viewport。
    只看 hmax 不够：`WrapScroll()` 设了 `ScrollBarAlwaysOff`，真实溢出时
    hmax 也可能读到 0，只有比 widget/viewport 宽度才能发现"卡片压到滚动条下面"。
  * geometry —— 同一尺寸下只改字重（默认 400 ↔ 900），布局必须不位移：
    比对 exe 自己 `--layout-dump` 出来的**容器几何**（SettingRow / 卡片 ListRow /
    页面根 widget）的 x、y、宽与顺序。

    为什么不用截图比对：先试过"强调色行"当几何标记（滑块填充段、开关轨道），
    但强调色也会出现在标题文字和主题色块上——字重一变笔画变粗，这些行的像素
    随之变化，判定就成了"字重改了像素"，测不出真假回归。`--layout-dump` 是
    控件树的真实几何，与渲染无关。

    竖直方向留余量：字重变粗会让"说明文字"多折一行，行高随之 +1 行（宽度不变）。
    这属于文本重排、不是拉伸，所以高度差异只记 WARN，并要求页面总高增量不超过
    3 行；x / 宽 / 顺序的变化一律 FAIL。

跑法
----
  python cpp/tools/c1_ui_settings_layout.py
  python cpp/tools/c1_ui_settings_layout.py --only overflow --sizes 9,11
  python cpp/tools/c1_ui_settings_layout.py --widths 420,548 --no-restore

不打扰前台
----------
复用 `quiet_runner`：子进程 `--quiet`（窗口移到可视区外 + NOACTIVATE/TOOLWINDOW，
不抢焦点、不进任务栏），整套进程树降到 below-normal；**不注入鼠标事件**，
也不需要把窗口拉到前台——取证全部来自 exe 自己的报告与位图。

副作用与还原
------------
用例需要"自定义字体"模式（否则字重/字宽行是禁用的），会通过 `--font-*` 参数改写
ui.ini 的 `[font]` 段。脚本开跑前把该段原文取出，结束时原样写回
（`--no-restore` 可跳过，排障用）。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from quiet_runner import (ApplyOwnPriority, MODE_AUTO, QuietExec,  # noqa: E402
                          Reporter, Tee)

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(TOOLS))
UI_DIR = os.path.join(ROOT, "cpp", "shell", "ui")
DEFAULT_EXE = os.path.join(UI_DIR, "build", "jmcomic_ui.exe")
LOG_ROOT = os.path.join(ROOT, "cpp", "build_logs")

# exe 是 MinGW 构建：build/ 里的 Qt DLL 未必齐（缺 libstdc++-6 / libgcc_s_seh-1），
# 必须把 Qt 与工具链的 bin 补进 PATH，否则进程直接 0xC0000135（DLL not found）。
QT_BIN = r"C:\Qt\6.12.0\mingw_64\bin"
MINGW_BIN = r"C:\Qt\Tools\mingw1310_64\bin"

GEOM_RE = re.compile(r"([\w]+)\[([^\]]*)\] (\d+)x(\d+)\+(-?\d+)\+(-?\d+) vis=(\d)")
LINE_HEIGHT = 24          # 设计基准行高，用于换算"允许的纵向重排余量"
MAX_REFLOW_LINES = 3      # 页面总高允许因文本重排增长的绝对行数
MAX_SLIDER_GAP = 32       # 滑块右端到数值标签的最大允许空隙（正常是一个间距 10px）


# ---------------------------------------------------------------- 报告解析
def ParseScrollReport(err):
    """--scroll-report → [(obj, hmax, vmax, vw, vh, ww, wh)]。"""
    out = []
    pat = re.compile(r"scroll-report: obj=(\S+) hmax=(-?\d+) vmax=(-?\d+) "
                     r"viewport=(\d+)x(\d+) widget=(-?\d+)x(-?\d+)")
    for line in (err or "").splitlines():
        m = pat.search(line)
        if m:
            out.append((m.group(1), int(m.group(2)), int(m.group(3)),
                        int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7))))
    return out


def SliderGaps(dump):
    """滑块右端到它数值标签左端之间的空隙（逻辑像素）。

    这是"界面被拉伸"的直接度量：旧版滑块用 setFixedWidth(150) 锁死、宿主却被
    拉到整行宽，数值标签就被顶到行尾，空隙能到 250px；正常情况下两者只隔一个
    布局间距（10px 左右），堆叠模式下滑块还会主动填满。
    """
    gaps = []
    prev_at = {}
    for e in dump:
        ind = e["indent"]
        if e["name"] == "RowMeta" and e["cls"] == "QLabel":
            sib = prev_at.get(ind)
            if sib and ("Slider" in sib["cls"]):
                gaps.append((sib["w"], e["x"] - (sib["x"] + sib["w"])))
        prev_at[ind] = e
    return gaps


def ParseLayoutDump(err):
    """--layout-dump → [dict(cls,name,indent,w,h,x,y,vis)]，保持树的顺序。"""
    out = []
    for line in (err or "").splitlines():
        if "layout-dump:" not in line:
            continue
        body = line.split("layout-dump:", 1)[1]
        indent = (len(body) - len(body.lstrip(" "))) // 2
        m = GEOM_RE.search(body)
        if not m:
            continue
        cls, name, w, h, x, y, vis = m.groups()
        out.append({"cls": cls, "name": name, "indent": indent,
                    "w": int(w), "h": int(h), "x": int(x), "y": int(y),
                    "vis": int(vis)})
    return out


def Containers(dump):
    """挑出"应该与字体无关"的控件：页面根、卡片、SettingRow，以及可交互控件。

    * 排除文本控件（QLabel/QPushButton/QLineEdit/QToolButton）：它们的宽度本来就
      跟着文字度量走，字重一变就变，拿它们当断言对象只会把"字体变了"误报成"布局坏了"。
    * 保留滑块/下拉/开关：这些是"被拉伸"的直接受害者——滑块一旦从 150 被撑到整行，
      这里立刻能看到宽度变化。
    """
    interactive = {"TouchSlider", "QSlider", "TouchCombo", "QComboBox",
                   "ToggleSwitch", "QCheckBox"}
    root = None
    for i, e in enumerate(dump):
        if e["name"] == "qt_scrollarea_viewport":
            for nxt in dump[i + 1:]:
                if nxt["indent"] == e["indent"] + 1:
                    root = nxt
                    break
            break
    picked = []
    for i, e in enumerate(dump):
        if e is root or e["cls"] == "SettingRow" or e["name"] == "ListRow" \
                or e["cls"] in interactive:
            picked.append((i, e))
    return picked


# ---------------------------------------------------------------- 配置快照
def ConfigIni(exe, q, probe_shot):
    """问 exe 自己的配置目录（不同平台 AppConfigLocation 不一样，别猜）。"""
    p = q.run(["--path-report", "--shot", probe_shot, "--shot-delay", "200"], timeout=90)
    for line in (p.stderr or "").splitlines():
        m = re.search(r"path-report: AppConfigLocation=(\S+)", line)
        if m:
            return os.path.join(m.group(1).replace("\\", "/"), "ui.ini")
    return None


def ReadSection(path, section):
    """取出 ini 里 [section] 段的原文（含段头），找不到返回 None。"""
    if not path or not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    out, inside = [], False
    for line in lines:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            if inside:
                break
            inside = (s == "[%s]" % section)
        if inside:
            out.append(line)
    return "".join(out) if out else None


def RestoreSection(path, section, block):
    """把 [section] 段整段替换回快照；文件或快照缺失则什么都不做。"""
    if not path or block is None or not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    cur = ReadSection(path, section)
    if cur is not None:
        text = text.replace(cur, block, 1)
    elif text and not text.endswith("\n"):
        text += "\n" + block
    else:
        text += block
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return True


# ---------------------------------------------------------------- 用例
class Case(object):
    def __init__(self, q, out, delay, timeout):
        self.q = q
        self.out = out
        self.delay = delay
        self.timeout = timeout
        os.makedirs(os.path.join(out, "shots"), exist_ok=True)
        self.n = 0

    def run(self, width, height, weight, size, extra=()):
        self.n += 1
        tag = "w%d_s%d_wt%d" % (width, size, weight)
        shot = os.path.join(self.out, "shots", "%02d_%s.png" % (self.n, tag))
        argv = ["--page", "settings", "--touch",
                "--win-size", "%dx%d" % (width, height),
                "--font-weight", str(weight),
                "--font-width", "100",
                "--font-size", str(size)] + list(extra) + \
               ["--shot", shot, "--shot-delay", str(self.delay)]
        proc = self.q.run(argv, timeout=self.timeout)
        return proc, shot, tag


def CheckOverflow(case, widths, sizes, height, rep, failures):
    for width in widths:
        for size in sizes:
            proc, _shot, tag = case.run(width, height, 400, size, ["--scroll-report"])
            name = "overflow/%s" % tag
            if proc.returncode != 0:
                rep.case(False, name, "rc=%d" % proc.returncode)
                failures.append("%s：进程退出码 %d" % (name, proc.returncode))
                rep.dump_error(name, proc)
                continue
            areas = ParseScrollReport(proc.stderr)
            if not areas:
                rep.case(False, name, "没有 scroll-report 行")
                failures.append("%s：scroll-report 缺失（exe 不支持该参数？）" % name)
                rep.dump_error(name, proc)
                continue
            bad = [a for a in areas if a[1] != 0 or a[5] > a[3]]
            if bad:
                detail = "；".join("obj=%s hmax=%d widget=%d viewport=%d"
                                  % (a[0], a[1], a[5], a[3]) for a in bad)
                rep.case(False, name, detail)
                failures.append("%s：横向溢出 %s" % (name, detail))
                rep.dump_error(name, proc)
                print("  %-30s FAIL" % tag)
            else:
                a = areas[0]
                rep.case(True, name, "hmax=0 widget=%d viewport=%d" % (a[5], a[3]))
                print("  %-30s OK" % tag)


def CheckGeometry(case, widths, sizes, height, weights, rep, failures, warns):
    lo, hi = weights
    line_h = LINE_HEIGHT
    for width in widths:
        for size in sizes:
            dumps = {}
            for weight in (lo, hi):
                proc, _shot, tag = case.run(width, height, weight, size, ["--layout-dump", "9"])
                name = "geometry/%s" % tag
                if proc.returncode != 0:
                    rep.case(False, name, "rc=%d" % proc.returncode)
                    failures.append("%s：进程退出码 %d" % (name, proc.returncode))
                    rep.dump_error(name, proc)
                    dumps = None
                    break
                dump = ParseLayoutDump(proc.stderr)
                if not dump:
                    rep.case(False, name, "没有 layout-dump 行")
                    failures.append("%s：layout-dump 缺失（exe 不支持该参数？）" % name)
                    rep.dump_error(name, proc)
                    dumps = None
                    break
                dumps[weight] = (Containers(dump), dump)
            if not dumps:
                continue

            name = "geometry/w%d_s%d_%dvs%d" % (width, size, lo, hi)
            (a, raw_lo), (b, _raw_hi) = dumps[lo], dumps[hi]

            # 0) 滑块与数值标签的空隙：这条直接对应"界面被拉伸"那个缺陷，
            #    与字重无关，所以两档字重都查（旧代码这里是 250px）。
            gaps = [g for _w, g in SliderGaps(raw_lo)]
            wide = [g for g in gaps if g > MAX_SLIDER_GAP]
            if wide:
                detail = "滑块与数值之间空隙 %s px（上限 %d）" % (
                    ",".join(str(g) for g in wide[:3]), MAX_SLIDER_GAP)
                rep.case(False, name, detail)
                failures.append("%s：%s" % (name, detail))
                print("  %-30s FAIL" % name.split("/", 1)[1])
                continue

            if len(a) != len(b):
                rep.case(False, name, "容器数量 %d vs %d" % (len(a), len(b)))
                failures.append("%s：容器数量不同（%d vs %d）" % (name, len(a), len(b)))
                print("  %-30s FAIL" % name.split("/", 1)[1])
                continue

            # 硬要求：横向几何不变（x、宽、顺序）。字重只该让笔画变粗，不该把任何
            # 控件拉宽或挪到别的列上——"界面被拉伸"就是这一条没守住。
            # 软要求：纵向。字重变粗会让说明文字多折一行，行高 +1 行并把它下面的
            # 容器整体下移；这是文本重排，不是变形，所以只 WARN，并给页面总高
            # 设上限（超过 3 行说明不是"多折一行"能解释的，判 FAIL）。
            moved_x, moved_y, resized, grow = [], [], [], []
            for (_, ea), (_, eb) in zip(a, b):
                label = "%s/%s" % (ea["cls"], ea["name"])
                if ea["x"] != eb["x"]:
                    moved_x.append("%s x %d->%d" % (label, ea["x"], eb["x"]))
                if ea["y"] != eb["y"]:
                    moved_y.append("%s y %d->%d" % (label, ea["y"], eb["y"]))
                if ea["w"] != eb["w"]:
                    resized.append("%s w %d->%d" % (label, ea["w"], eb["w"]))
                if ea["h"] != eb["h"]:
                    grow.append("%s h %d->%d" % (label, ea["h"], eb["h"]))

            page_delta = abs(a[0][1]["h"] - b[0][1]["h"]) if a and b else 0
            limit = MAX_REFLOW_LINES * line_h

            if moved_x or resized or page_delta > limit:
                detail = "；".join((moved_x + resized)[:3])
                if page_delta > limit:
                    detail += "；页面总高变化 %dpx（上限 %dpx）" % (page_delta, limit)
                rep.case(False, name, detail)
                failures.append("%s：布局横向位移 %s" % (name, detail))
                print("  %-30s FAIL" % name.split("/", 1)[1])
            elif grow or moved_y:
                detail = "%d 个容器加高、%d 个下移（页面 ±%dpx）：%s" % (
                    len(grow), len(moved_y), page_delta, "；".join((grow + moved_y)[:2]))
                rep.case(True, name, "无横向位移；" + detail)
                warns.append("%s：%s" % (name, detail))
                print("  %-30s OK（文本重排 +%dpx，无横向位移）"
                      % (name.split("/", 1)[1], page_delta))
            else:
                rep.case(True, name, "%d 个容器几何完全一致" % len(a))
                print("  %-30s OK（%d 项一致）" % (name.split("/", 1)[1], len(a)))


def main():
    ap = argparse.ArgumentParser(description="设置页布局回归（横向不溢出 / 字重不改几何）")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out", default=None,
                    help="报告目录（默认 cpp/build_logs/ui_settings_layout_<ts>）")
    ap.add_argument("--only", default="overflow,geometry",
                    help="overflow / geometry，可逗号分隔")
    ap.add_argument("--widths", default="420,480,548,640")
    ap.add_argument("--sizes", default="8,9,11,12")
    ap.add_argument("--weights", default="400,900", help="geometry 比较的两档字重")
    ap.add_argument("--height", type=int, default=1099)
    ap.add_argument("--delay", type=int, default=1100)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--quiet", default=MODE_AUTO, help="quiet_runner 模式（auto/hidden/off…）")
    ap.add_argument("--priority", default="below")
    ap.add_argument("--no-restore", action="store_true", help="不还原 ui.ini 的 [font] 段")
    args = ap.parse_args()

    only = [s.strip() for s in args.only.split(",") if s.strip()]
    widths = [int(v) for v in args.widths.split(",") if v.strip()]
    sizes = [int(v) for v in args.sizes.split(",") if v.strip()]
    weights = tuple(int(v) for v in args.weights.split(",")[:2])

    if not os.path.exists(args.exe):
        print("[x] 找不到可执行文件：%s" % args.exe)
        return 2

    stamp = time.strftime("%Y%m%d_%H%M%S")
    outdir = args.out or os.path.join(LOG_ROOT, "ui_settings_layout_" + stamp)
    rep = Reporter(outdir, title="设置页布局回归（横向不溢出 / 字重不改几何）")
    sys.stdout = Tee(sys.stdout, rep)
    ApplyOwnPriority(args.priority)

    # 环境：补 PATH（否则 0xC0000135）+ 把 ui.ini 隔离到报告目录，
    # 跑测不碰用户自己的配置（AppPaths 的候选顺序里 JM_UI_CONFIG 最优先）。
    cfgdir = os.path.join(outdir, "cfg")
    os.makedirs(cfgdir, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = QT_BIN + os.pathsep + MINGW_BIN + os.pathsep + env.get("PATH", "")
    env["JM_UI_CONFIG"] = cfgdir.replace("\\", "/")
    env["APPDATA"] = cfgdir
    env["LOCALAPPDATA"] = cfgdir

    q = QuietExec(args.exe, base_env=env, mode=args.quiet, priority=args.priority,
                  probe_dir=outdir, probe_log=lambda s: print("[quiet] " + s))
    mode = q.resolve()

    rep.meta["可执行文件"] = "%s（%dKB）" % (args.exe, os.path.getsize(args.exe) // 1024)
    rep.meta["静默模式"] = "%s（%s）" % (mode, q.note or "—")
    rep.meta["窗口宽度"] = ",".join(str(w) for w in widths)
    rep.meta["字号"] = ",".join(str(s) for s in sizes)
    rep.meta["字重对比"] = "%d vs %d" % weights

    ini = ConfigIni(args.exe, q, os.path.join(outdir, "probe_path.png"))
    font_block = ReadSection(ini, "font")
    print("[cfg] ui.ini=%s  [font] 段快照=%s" % (ini, font_block is not None))

    failures, warns = [], []
    case = Case(q, outdir, args.delay, args.timeout)
    passes = 0
    try:
        if "overflow" in only:
            print("\n=== 1/2 横向溢出（hmax 必须 0，widget 不得宽于 viewport）===")
            CheckOverflow(case, widths, sizes, args.height, rep, failures)
        if "geometry" in only:
            print("\n=== 2/2 字重不改变几何（容器 x/宽/顺序必须一致）===")
            CheckGeometry(case, widths, sizes, args.height, weights, rep, failures, warns)
    except KeyboardInterrupt:
        print("\n[!] 中断，已跑的用例照常出报告")
    finally:
        if not args.no_restore and font_block is not None:
            print("\n[cfg] [font] 段已还原：%s"
                  % RestoreSection(ini, "font", font_block))
        passes = sum(1 for c in rep.cases if c["ok"])
        rep.finish(passes, failures, extra=q.stats())

    print("\n=== 汇总 ===\nPASS %d / FAIL %d  report=%s"
          % (passes, len(failures), os.path.join(outdir, "report.md")))
    for w in warns:
        print("  ~ " + w)
    for f in failures:
        print("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
