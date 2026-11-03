#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C1 W7 人工 checklist 的自动化取证。

原 W7 报告把 F2/F3/F4/F5/F6/F7 与 S2/S3 标成"待人工"。这些用例本质是
"点一下、看界面/日志是否符合预期"，可以用 adb 输入 + uiautomator dump 判定，
不必依赖人眼。判据来自 docs/c1_w7_report.md §5。

关键前提：C1 是 Qt Widgets 应用，Qt 6 的 Android accessibility 桥会把
每个 QPushButton 的 text 暴露成 content-desc、QLabel 的文字暴露成 content-desc，
并给出 bounds。因此可以按文字定位控件，而不是硬编码像素坐标。

判定分三类：
  PASS / FAIL  —— 有确定判据，脚本自己判
  REVIEW       —— 需要人眼（观感/图像内容），脚本存截图并写明看什么
  UNREACHABLE  —— 该输入路径在设备上不可达（如无物理键盘），如实记录，不算通过

用法：
  python android/tools/c1_w7_manual_check.py --serial emulator-5554
  python android/tools/c1_w7_manual_check.py --serial 10AF5K2B0D001AM --out android/build_logs/manual-xxx
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

PKG = "org.jmcomic.c1shell"
ACT = "org.jmcomic.c1shell/org.qtproject.qt.android.bindings.QtActivity"

# 常见 adb 位置（Windows 上 Git Bash 里没有 adb 在 PATH）
ADB_CANDIDATES = [
    os.environ.get("ADB", ""),
    r"C:\Users\WQ159\AppData\Local\Android\Sdk\platform-tools\adb.exe",
    "adb",
]


def find_adb():
    for c in ADB_CANDIDATES:
        if not c:
            continue
        if os.path.isabs(c):
            if os.path.exists(c):
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    return "adb"


class Device:
    def __init__(self, adb, serial):
        self.adb = adb
        self.serial = serial

    def run(self, args, timeout=40, binary=False):
        cmd = [self.adb, "-s", self.serial] + args
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
        if binary:
            return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace")
        return (
            p.returncode,
            p.stdout.decode("utf-8", "replace"),
            p.stderr.decode("utf-8", "replace"),
        )

    # ---- 基础动作 ----
    def sh(self, cmd, timeout=40):
        return self.run(["shell", cmd], timeout=timeout)

    def start(self):
        self.sh(f"am force-stop {PKG}")
        time.sleep(0.8)
        rc, out, err = self.run(["shell", "am", "start", "-W", "-n", ACT], timeout=60)
        return out

    def pid(self):
        _, out, _ = self.sh(f"pidof {PKG}")
        return out.strip()

    def logcat_clear(self):
        self.run(["logcat", "-c"], timeout=20)

    def logcat(self):
        _, out, _ = self.run(["logcat", "-d"], timeout=60)
        return out.replace("\r", "")

    def screenshot(self, path):
        rc, data, err = self.run(["exec-out", "screencap", "-p"], timeout=60, binary=True)
        if rc != 0 or len(data) < 1000:
            return False
        with open(path, "wb") as f:
            f.write(data)
        return True

    # ---- UI 树 ----
    def nodes(self, retries=4):
        """返回 [{desc, cls, bounds:(l,t,r,b), enabled}]，失败重试。"""
        for i in range(retries):
            self.sh("uiautomator dump /sdcard/ui.xml", timeout=30)
            _, xml, _ = self.sh("cat /sdcard/ui.xml", timeout=30)
            start = xml.find("<?xml")
            if start < 0:
                start = xml.find("<hierarchy")
            if start >= 0:
                try:
                    root = ET.fromstring(xml[start:])
                except ET.ParseError:
                    time.sleep(0.6)
                    continue
                out = []
                for n in root.iter("node"):
                    b = n.get("bounds", "")
                    m = re.match(r"\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]", b)
                    if not m:
                        continue
                    l, t, r, bo = (int(x) for x in m.groups())
                    out.append(
                        {
                            "desc": n.get("content-desc", "") or n.get("text", ""),
                            "cls": n.get("class", ""),
                            "rid": n.get("resource-id", ""),
                            "bounds": (l, t, r, bo),
                            "enabled": n.get("enabled") == "true",
                            "clickable": n.get("clickable") == "true",
                        }
                    )
                if out:
                    return out
            time.sleep(0.6)
        return []

    @staticmethod
    def center(node):
        l, t, r, b = node["bounds"]
        return (l + r) // 2, (t + b) // 2

    def find(self, desc, nodes=None, exact=True):
        nodes = nodes if nodes is not None else self.nodes()
        for n in nodes:
            if exact and n["desc"] == desc:
                return n
        if not exact:
            for n in nodes:
                if desc in n["desc"]:
                    return n
        return None

    def title(self, nodes=None):
        """标题 QLabel：位于顶栏下方（y 小于 300），且不含按钮类。"""
        nodes = nodes if nodes is not None else self.nodes()
        for n in nodes:
            if n["cls"].endswith("TextView") and "QLabel" in n["rid"]:
                l, t, r, b = n["bounds"]
                if t < 320 and n["desc"]:
                    return n["desc"]
        return ""

    def status(self, nodes=None):
        """底部状态 QLabel：y > 2200。"""
        nodes = nodes if nodes is not None else self.nodes()
        for n in nodes:
            if n["cls"].endswith("TextView") and "QLabel" in n["rid"]:
                l, t, r, b = n["bounds"]
                if t > 2200 and n["desc"]:
                    return n["desc"]
        return ""

    # ---- 输入 ----
    def tap(self, desc, wait=0.45, exact=True):
        n = self.find(desc, exact=exact)
        if not n:
            return False
        x, y = self.center(n)
        self.sh(f"input tap {x} {y}")
        time.sleep(wait)
        return True

    def tap_xy(self, x, y, wait=0.45):
        self.sh(f"input tap {x} {y}")
        time.sleep(wait)

    def screen(self):
        """(宽, 高)。滑动坐标按它算，换分辨率不用改脚本。"""
        if not hasattr(self, "_screen"):
            _, out, _ = self.sh("wm size")
            m = re.search(r"(\d+)x(\d+)", out)
            self._screen = (int(m.group(1)), int(m.group(2))) if m else (1080, 2340)
        return self._screen

    def swipe_page(self, forward=True, ms=120, wait=0.5):
        """整页横向滑动。forward=True 表示"向右滑"（Rel.x>0）。"""
        w, h = self.screen()
        y = h * 62 // 100
        xa, xb = w * 82 // 100, w * 18 // 100
        if forward:
            self.swipe(xa, y, xb, y, ms=ms, wait=wait)      # 左滑
        else:
            self.swipe(xb, y, xa, y, ms=ms, wait=wait)      # 右滑

    def swipe(self, x1, y1, x2, y2, ms=120, wait=0.5):
        self.sh(f"input swipe {x1} {y1} {x2} {y2} {ms}")
        time.sleep(wait)

    def key(self, code, wait=0.4):
        self.sh(f"input keyevent {code}")
        time.sleep(wait)

    def back(self, wait=0.6):
        self.sh("input keyevent 4")
        time.sleep(wait)

    def wait_title(self, pattern, timeout=12):
        rx = re.compile(pattern)
        t0 = time.time()
        last = ""
        while time.time() - t0 < timeout:
            last = self.title()
            if rx.search(last or ""):
                return last
            time.sleep(0.4)
        return last

    def wait_ready(self, timeout=25):
        """等应用真的起来了（能 dump 到 Home 标题）。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            nodes = self.nodes(retries=2)
            if nodes and self.title(nodes):
                return True
            time.sleep(0.8)
        return False


def sample_grid(png_path, gw=24, gh=48, ffmpeg="ffmpeg"):
    """把截图缩成 gw x gh 的小网格，返回 [[(r,g,b), ...], ...]。

    用 ffmpeg 做解码+缩放，避免依赖 Pillow；纯色测试图下足以判定
    "左半红、右半蓝" 这类空间布局。
    """
    try:
        p = subprocess.run(
            [ffmpeg, "-v", "error", "-i", png_path,
             "-vf", f"scale={gw}:{gh}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            capture_output=True, timeout=30,
        )
        if p.returncode != 0 or len(p.stdout) < gw * gh * 3:
            return None
        d = p.stdout
        return [[tuple(d[(y * gw + x) * 3:(y * gw + x) * 3 + 3]) for x in range(gw)]
                for y in range(gh)]
    except Exception:
        return None


def dominant(grid, x0, x1, y0, y1, dark_cut=60):
    """区域内非暗像素的平均色。"""
    if not grid:
        return None
    acc = [0, 0, 0]
    n = 0
    for y in range(max(0, y0), min(len(grid), y1)):
        for x in range(max(0, x0), min(len(grid[0]), x1)):
            r, g, b = grid[y][x]
            if r + g + b < dark_cut:      # 跳过黑色留白/状态栏
                continue
            acc[0] += r; acc[1] += g; acc[2] += b
            n += 1
    if n == 0:
        return None
    return tuple(v // n for v in acc)


def is_red(c):
    return bool(c) and c[0] > 120 and c[1] < 110 and c[2] < 110


def is_blue(c):
    return bool(c) and c[2] > 120 and c[0] < 110 and c[1] < 130


def is_green(c):
    return bool(c) and c[1] > 110 and c[0] < 110 and c[2] < 110


def mean_luma(png_path, ffmpeg="ffmpeg"):
    """整屏平均亮度（0-255）。用 ffmpeg 缩到 1x1 取像素，避免依赖 Pillow。"""
    try:
        p = subprocess.run(
            [ffmpeg, "-v", "error", "-i", png_path, "-vf", "scale=1:1",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            capture_output=True, timeout=30,
        )
        if p.returncode != 0 or len(p.stdout) < 3:
            return None
        r, g, b = p.stdout[0], p.stdout[1], p.stdout[2]
        return round(0.299 * r + 0.587 * g + 0.114 * b, 1)
    except Exception:
        return None


class Checks:
    def __init__(self, dev, outdir):
        self.dev = dev
        self.out = outdir
        self.shots = os.path.join(outdir, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.results = []

    def shot(self, name):
        path = os.path.join(self.shots, name + ".png")
        ok = self.dev.screenshot(path)
        return path if ok else None

    def add(self, cid, name, criteria, observed, verdict, evidence=""):
        self.results.append(
            dict(id=cid, name=name, criteria=criteria, observed=observed,
                 verdict=verdict, evidence=evidence)
        )
        print(f"  [{verdict:11s}] {cid} {name}: {observed}")

    def preflight(self):
        """确认阅读目录里的页数，供 F3 判据使用。"""
        _, out, _ = self.dev.sh(f"run-as {PKG} ls files/reader")
        files = [x for x in out.split() if re.search(r"\.(jpg|jpeg|png|webp|bmp|gif)$", x, re.I)]
        return len(files), files

    # ------------------------------------------------------------------
    def F3_dir_load(self, expected):
        self.dev.start()
        self.dev.wait_ready()
        self.dev.tap("Reader")
        title = self.dev.wait_title(r"Reader\s+\d+/\d+|Reader \(no pages\)")
        nodes = self.dev.nodes()
        m = re.search(r"Reader\s+(\d+)/(\d+)", title)
        total = int(m.group(2)) if m else -1
        verdict = "PASS" if expected > 0 and total == expected else "FAIL"
        self.add("F3", "目录加载", f"页数 == 目录内图片数 {expected}",
                 f"标题=\"{title}\"（total={total}）", verdict,
                 self.shot("F3_dir"))

    def F2_page_stack(self):
        self.dev.start()
        self.dev.wait_ready()
        dev = self.dev
        trail = []
        nodes = dev.nodes()
        trail.append(("Home", dev.title(nodes),
                      (dev.find("Back", nodes) or {}).get("enabled")))
        dev.tap("Reader")
        nodes = dev.nodes()
        trail.append(("Reader", dev.title(nodes),
                      (dev.find("Back", nodes) or {}).get("enabled")))
        dev.tap("Settings")
        nodes = dev.nodes()
        trail.append(("Settings", dev.title(nodes),
                      (dev.find("Back", nodes) or {}).get("enabled")))
        dev.tap("Back")
        nodes = dev.nodes()
        trail.append(("Back#1", dev.title(nodes),
                      (dev.find("Back", nodes) or {}).get("enabled")))
        dev.tap("Back")
        nodes = dev.nodes()
        trail.append(("Back#2", dev.title(nodes),
                      (dev.find("Back", nodes) or {}).get("enabled")))

        # 判据：标题跟随当前页；Back 在非栈底可用、栈底禁用；回退顺序 Reader → Home
        t_home, t_reader, t_set = trail[0][1], trail[1][1], trail[2][1]
        ok = (
            t_home == "Home"
            and t_reader.startswith("Reader")
            and t_set == "Settings"
            and trail[0][2] is False
            and trail[1][2] is True
            and trail[3][1].startswith("Reader")
            and trail[4][1] == "Home"
            and trail[4][2] is False
        )
        self.add("F2", "三页切换 + Back 回退",
                 "标题跟随；Back 依次 Reader→Home 且栈底禁用",
                 " → ".join(f"{s}:{t}" for s, t, _ in trail), "PASS" if ok else "FAIL",
                 self.shot("F2_back_home"))

    def F4_direction(self):
        """判据：RTL 下左右键语义反转，且滑动方向与键盘一致。

        先前的用例错在"在首页做上一页"——那本来就不会翻页，会把正确实现判成
        FAIL。这里从 1/3 出发，先用键盘把页码推到有回退空间的位置再测。
        """
        dev = self.dev
        dev.start()
        dev.wait_ready()
        dev.tap("Reader")
        dev.wait_title(r"Reader\s+\d+/\d+")
        time.sleep(0.4)
        # Android 在窗口刚拿到焦点时，第一个按键通常只用于激活窗口、不派发给应用。
        # 先发一对"前进+后退"把这一帧吃掉，回到原页后再正式记录 —— 否则会把
        # 窗口激活行为记成"右键不翻页"的产品缺陷（实测正是如此）。
        dev.key(22)
        dev.key(21)
        time.sleep(0.3)
        t0 = dev.title()
        steps = [("起始", t0)]

        dev.key(22); steps.append(("LTR 右键", dev.title()))
        dev.key(22); steps.append(("LTR 右键", dev.title()))
        dev.key(21); steps.append(("LTR 左键", dev.title()))
        dev.swipe_page(False); steps.append(("LTR 右滑", dev.title()))
        dev.swipe_page(True); steps.append(("LTR 左滑", dev.title()))

        dev.tap("RTL"); steps.append(("切 RTL", dev.title()))
        dev.key(22); steps.append(("RTL 右键", dev.title()))
        dev.key(21); steps.append(("RTL 左键", dev.title()))
        dev.swipe_page(False); steps.append(("RTL 右滑", dev.title()))
        dev.swipe_page(True); steps.append(("RTL 左滑", dev.title()))
        dev.tap("RTL")  # 复位 LTR
        shot = self.shot("F4_rtl")

        nums = [self._page_no(t) for _, t in steps]
        expect = [1, 2, 3, 2, 3, 2, 2, 1, 2, 1, 2]
        ok = nums == expect
        detail = " → ".join(f"{n}={p}" for (n, _), p in zip(steps, nums))
        self.add("F4", "翻页方向（RTL 反转）",
                 "LTR 右键/右滑=下一页；RTL 右键/右滑=上一页（语义反转）",
                 detail + ("" if ok else f"（期望 {expect}）"),
                 "PASS" if ok else "FAIL", shot)

    @staticmethod
    def _page_no(title):
        m = re.search(r"(\d+)/\d+", title or "")
        return int(m.group(1)) if m else -1

    def F5_zoom(self):
        dev = self.dev
        dev.start()
        dev.wait_ready()
        dev.tap("Reader")
        time.sleep(0.6)
        before = dev.status(dev.nodes())
        zoom_shots = []
        reached = False
        for code in (81, 69, 7):  # PLUS / MINUS / 0
            dev.key(code)
            st = dev.status(dev.nodes())
            if st.startswith("zoom"):
                reached = True
                zoom_shots.append(st)
        if reached:
            verdict = "PASS"
            observed = "键盘可达：" + " → ".join(zoom_shots)
        else:
            verdict = "REVIEW"
            observed = (f"键盘事件未进阅读器（状态仍为 \"{before[:28]}\"）；"
                        "工具条无缩放按钮 → 缩放只剩捏合手势，需人眼确认")
        self.add("F5", "缩放（+/−/0 与捏合）",
                 "zoom 状态变化并能复位 fit-width；捏合手势需真机", observed, verdict,
                 self.shot("F5_zoom"))

    def F6_two_page(self, expected):
        """判据：双页并排（左页在前）、步进 2 页、末页不足退化为单页。

        视觉判定用纯色测试图（page01 红 / page02 蓝 / page03 绿）做像素采样：
        双页时左半应为红、右半为蓝；末页单页时整屏应以绿为主。
        """
        dev = self.dev
        dev.start()
        dev.wait_ready()
        dev.tap("Reader")
        p_before = dev.wait_title(r"Reader\s+\d+/\d+")
        dev.tap("2-up")
        time.sleep(0.6)
        p_two = dev.title()
        shot_a = self.shot("F6_spread")
        dev.tap("Next page")
        time.sleep(0.7)
        p_next = dev.title()
        shot_b = self.shot("F6_last_single")

        g_a = sample_grid(shot_a) if shot_a else None
        g_b = sample_grid(shot_b) if shot_b else None
        # 阅读区大致在 y 20%~92%；左右各取中间 40%
        left = dominant(g_a, 1, 11, 12, 40)
        right = dominant(g_a, 13, 23, 12, 40)
        whole = dominant(g_b, 1, 23, 12, 40)

        log = dev.sh("logcat -d -s C1Reader:V | tail -20")[1]
        on = "two-page on" in log
        step = self._page_no(p_next) - self._page_no(p_two)
        spread_ok = is_red(left) and is_blue(right)
        single_ok = is_green(whole)
        ok = on and step == expected - self._page_no(p_two) and spread_ok and single_ok
        if not on or step != 2:
            ok = False
        self.add("F6", "双页与末页退化",
                 "2-up 并排（左红右蓝）；步进 2 页；末页单页（绿）",
                 f"{p_before} →2-up:{p_two} →Next:{p_next}（step={step}）；"
                 f"左={left} 右={right} 末页={whole}",
                 "PASS" if ok else "FAIL", f"{shot_a}; {shot_b}")

    def F7_brightness(self):
        dev = self.dev
        dev.start()
        dev.wait_ready()
        dev.tap("Reader")
        dev.wait_title(r"Reader\s+\d+/\d+")
        time.sleep(0.5)
        levels = []
        shots = [self.shot("F7_level0")]
        levels.append(mean_luma(shots[0]) if shots[0] else None)
        for i in range(1, 5):
            dev.tap("Dim")
            time.sleep(0.45)
            p = self.shot(f"F7_level{i}")
            v = mean_luma(p) if p else None
            # screencap 偶尔会抓到上一帧（overlay 还没合成），表现为相邻两级亮度相同。
            # 这不是产品缺陷（Reader 日志里四级都在），重拍一次即可。
            if v is not None and levels[-1] is not None and abs(v - levels[-1]) < 1.0:
                time.sleep(0.8)
                p2 = self.shot(f"F7_level{i}_retry")
                v2 = mean_luma(p2) if p2 else None
                if v2 is not None:
                    p, v = p2, v2
            shots.append(p)
            levels.append(v)
        # 判据：1.0 > 0.75 > 0.5 > 0.25，第 4 次回到 1.0
        vals = [v for v in levels if v is not None]
        ok = len(vals) == 5 and vals[0] > vals[1] > vals[2] > vals[3] and abs(vals[4] - vals[0]) <= 2.0
        log = dev.sh("logcat -d -s C1Reader:V | grep brightness | tail -6")[1]
        self.add("F7", "亮度循环", "平均亮度单调下降，第 4 次回到 1.0（±2）",
                 f"luma={levels}；日志={log.strip().splitlines()[-4:] if log.strip() else 'NA'}",
                 "PASS" if ok else "FAIL", "; ".join(s for s in shots if s))

    def S2_ten_turns(self):
        dev = self.dev
        dev.start()
        dev.wait_ready()
        dev.tap("Reader")
        dev.wait_title(r"Reader\s+\d+/\d+")
        pid0 = dev.pid()
        dev.logcat_clear()
        for _ in range(10):
            dev.tap("Next page", wait=0.25)
        time.sleep(1.2)
        pid1 = dev.pid()
        log = dev.logcat()
        fatal = len(re.findall(r"FATAL EXCEPTION", log))
        title = dev.title(dev.nodes())
        ok = bool(pid1) and pid0 == pid1 and fatal == 0
        self.add("S2", "连续切页 10 次", "进程不重启、无 FATAL",
                 f"pid {pid0}→{pid1}，FATAL={fatal}，末页={title}", "PASS" if ok else "FAIL",
                 self.shot("S2_after10"))

    def S3_network_fault(self):
        """断网 → 请求失败 → 恢复网络 → 仍能成功请求（证明通道未被卡死）。"""
        dev = self.dev
        dev.start()
        dev.wait_ready()

        def probe(timeout=26):
            dev.tap("Net test (bridge)")
            deadline = time.time() + timeout
            while time.time() < deadline:
                time.sleep(1.0)
                st = dev.status(dev.nodes())
                if st.startswith("net "):
                    return st
            return dev.status(dev.nodes())

        dev.sh("cmd connectivity airplane-mode enable")
        time.sleep(3)
        # 确认飞行模式真的断网了，否则这条用例没有意义（部分模拟器虚拟网卡不受影响）
        _, pong, _ = dev.sh("ping -c 1 -W 2 223.5.5.5")
        really_offline = "1 received" not in pong and "1 packets received" not in pong
        offline = probe() if really_offline else ""
        self.shot("S3_offline")

        dev.sh("cmd connectivity airplane-mode disable")
        time.sleep(8)
        online = probe() if really_offline else ""
        self.shot("S3_online")

        if not really_offline:
            self.add("S3", "网络异常后恢复", "断网返回错误码；恢复后仍能成功",
                     "飞行模式未真正断网（模拟器虚拟网卡不受影响），本用例无法在此设备取证",
                     "SKIP", "S3_offline.png")
            return

        off_bad = "net ERR" in offline
        on_ok = "net OK 2" in online
        self.add("S3", "网络异常后恢复",
                 "断网返回 net ERR；恢复网络后仍能 net OK 2xx",
                 f"断网=\"{offline}\"; 恢复=\"{online}\"",
                 "PASS" if (off_bad and on_ok) else "FAIL",
                 "S3_offline.png / S3_online.png")


def write_report(out, results, meta):
    lines = []
    lines.append(f"# C1 W7 人工 checklist 自动化取证\n")
    lines.append(f"- 设备：`{meta['serial']}`（{meta['model']}，Android {meta['sdk']}）")
    lines.append(f"- 包：`{meta['pkg']}` v{meta['version']}")
    lines.append(f"- 时间：{meta['time']}")
    lines.append(f"- 截图目录：`shots/`\n")
    lines.append("| 用例 | 判据 | 实测 | 判定 |")
    lines.append("|---|---|---|---|")
    for r in results:
        obs = r["observed"].replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {r['id']} {r['name']} | {r['criteria']} | {obs} | **{r['verdict']}** |")

    review = [r for r in results if r["verdict"] == "REVIEW"]
    unreach = [r for r in results if r["verdict"] in ("UNREACHABLE", "SKIP")]
    if review:
        lines.append("\n## 需人眼复核\n")
        for r in review:
            lines.append(f"- **{r['id']}** {r['name']}：看图 `{r['evidence']}`")
    if unreach:
        lines.append("\n## 未取证 / 输入路径不可达（不算通过）\n")
        for r in unreach:
            lines.append(f"- **{r['id']}** {r['name']}：{r['observed']}")

    passed = sum(1 for r in results if r["verdict"] == "PASS")
    lines.append(f"\n合计：PASS {passed} / {len(results)}，"
                 f"REVIEW {len(review)}，SKIP/UNREACHABLE {len(unreach)}，"
                 f"FAIL {sum(1 for r in results if r['verdict'] == 'FAIL')}")
    path = os.path.join(out, "report.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--adb", default="")
    ap.add_argument("--skip-s3", action="store_true", help="跳过断网用例（会改设备网络状态）")
    args = ap.parse_args()

    adb = args.adb or find_adb()
    dev = Device(adb, args.serial)
    _, _, err = dev.run(["get-state"], timeout=20)
    if "device" not in dev.run(["get-state"], timeout=20)[1]:
        print(f"设备 {args.serial} 不可用：{err}")
        return 2

    out = args.out or os.path.join(
        "android", "build_logs", "c1_w7_manual_" + time.strftime("%Y%m%d-%H%M%S")
    )
    os.makedirs(out, exist_ok=True)
    checks = Checks(dev, out)

    model = dev.sh("getprop ro.product.model")[1].strip()
    sdk = dev.sh("getprop ro.build.version.sdk")[1].strip()
    ver = dev.sh(f"dumpsys package {PKG} | grep versionName")[1].strip()
    ver = ver.split("=")[-1] if "=" in ver else ver
    meta = dict(serial=args.serial, model=model, sdk=sdk, pkg=PKG,
                version=ver or "?", time=time.strftime("%Y-%m-%d %H:%M:%S"))

    print(f"=== 设备 {model} / Android {sdk} / {args.serial} ===")
    expected, files = checks.preflight()
    print(f"=== 阅读目录 {expected} 页：{files} ===")
    if expected == 0:
        print("阅读目录为空，F3/F6 会失真；请先投图（见 c1_w7_regression.sh）")

    checks.F3_dir_load(expected)
    checks.F2_page_stack()
    checks.F4_direction()
    checks.F5_zoom()
    checks.F6_two_page(expected)
    checks.F7_brightness()
    checks.S2_ten_turns()
    if not args.skip_s3:
        checks.S3_network_fault()

    path = write_report(out, checks.results, meta)
    print(f"\n报告：{path}")
    bad = [r for r in checks.results if r["verdict"] == "FAIL"]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
