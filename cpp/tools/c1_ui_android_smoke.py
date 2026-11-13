# coding: utf-8
"""C++ UI Android 冒烟测试（AVD / x86_64）。

用 adb 驱动真机式交互：启动 → 抽屉 → 返回键 → 详情 → 收藏切换 →
阅读器翻页 → 长按菜单 → 横竖屏 → 返回退出；每一步截图并做像素差异断言，
最后检查 logcat 里没有本应用的 FATAL/ANR。

注：返回键用例刻意不依赖抽屉里的具体导航项坐标——真机安全区/字体缩放会让
1080x2400 基准坐标落到别的项上，曾因此把"首页按返回退出"误报成返回键缺陷。

跑法：
  python cpp/tools/c1_ui_android_smoke.py
  python cpp/tools/c1_ui_android_smoke.py --apk cpp/dist/JMComic-ui-0.88-x86_64-debug.apk

前置：设备已启动（sys.boot_completed=1），adb 可用。
坐标以 1080x2400 为基准，按实际 wm size 等比缩放（AVD 与真机共用一套用例）。
产物：docs/shots/android/*.png
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCALAPPDATA = os.environ.get("LOCALAPPDATA", os.path.expanduser("~\\AppData\\Local"))


def _resolve_adb():
    """adb 位置：显式 $ADB > PATH > Windows SDK 默认路径。

    桌面回归和 Android 冒烟现在都能在 Linux 上跑，所以不能写死 .exe。
    """
    env = os.environ.get("ADB")
    if env and os.path.exists(env):
        return env
    found = shutil.which("adb")
    if found:
        return found
    return os.path.join(LOCALAPPDATA, "Android", "Sdk", "platform-tools", "adb.exe")


ADB = _resolve_adb()
PKG = "org.jmcomic.ui"
ACT = "org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity"
OUT = os.path.join(ROOT, "docs", "shots", "android")

Failures = []
Skips = []
Passes = 0


def Check(ok, name, detail=""):
    global Passes
    print(("  ok   " if ok else "  FAIL ") + name + (("  <" + detail + ">") if detail else ""))
    if ok:
        Passes += 1
    else:
        Failures.append(name + ((" <" + detail + ">") if detail else ""))


def Adb(*args, timeout=60):
    p = subprocess.run([ADB] + list(args), capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def Shot(name):
    path = os.path.join(OUT, name + ".png")
    with open(path, "wb") as f:
        p = subprocess.run([ADB, "exec-out", "screencap", "-p"], stdout=f,
                           stderr=subprocess.DEVNULL, timeout=60)
    return path


def Info(path):
    """尺寸 + 稀疏采样 md5（用于比较画面是否变化）。"""
    if not os.path.exists(path):
        return None
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
    try:
        w, h = [int(x) for x in probe.stdout.strip().split(",")]
    except Exception:
        return None
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
    if len(raw) != w * h * 3:
        return None
    step = 29 * 3
    total = total2 = 0
    colors = set()
    n = 0
    for i in range(0, len(raw) - 2, step):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = (r * 299 + g * 587 + b * 114) // 1000
        total += v
        total2 += v * v
        colors.add((r >> 4, g >> 4, b >> 4))
        n += 1
    mean = total / n
    std = max(0.0, total2 / n - mean * mean) ** 0.5
    return {"w": w, "h": h, "std": std, "colors": len(colors), "md5": hashlib.md5(raw).hexdigest()}


def RawRGB(path):
    """解码成 (w, h, bytes)；失败返回 None。"""
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
    try:
        w, h = [int(x) for x in probe.stdout.strip().split(",")]
    except Exception:
        return None
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
    if len(raw) != w * h * 3:
        return None
    return w, h, raw


def Settle(tag, max_s=12.0, quiet_s=1.2, band=(0.25, 0.75)):
    """等画面稳定（连续多次 md5 相同）再返回截图路径。

    Android 上详情页是"演示数据先渲染、真实数据再覆盖"，布局会二次位移。
    固定 sleep 之后按旧坐标点击必然落空（06_detail 与 08_reader 的按钮行相差 50px）。
    """
    path = os.path.join(OUT, "_settle.png")
    prev, stable, waited = None, 0.0, 0.0
    while waited < max_s:
        with open(path, "wb") as f:
            subprocess.run([ADB, "exec-out", "screencap", "-p"], stdout=f,
                           stderr=subprocess.DEVNULL, timeout=60)
        info = Info(path)
        md5 = info["md5"] if info else None
        if md5 is not None and md5 == prev:
            stable += 0.4
            if stable >= quiet_s:
                break
        else:
            stable = 0.0
        prev = md5
        time.sleep(0.4)
        waited += 0.4
    final = os.path.join(OUT, tag + ".png")
    with open(final, "wb") as f:
        subprocess.run([ADB, "exec-out", "screencap", "-p"], stdout=f,
                       stderr=subprocess.DEVNULL, timeout=60)
    return final


def OrangeButtonY(path, y0=0.25, y1=0.85, min_run=0.06):
    """取"实心橙色按钮"所在行的中心 y（最上面一簇）。

    不能只数橙色像素：章节胶囊的描边、选中态文字也是橙色，会把中心拉到章节区。
    这里要求同一行里有连续的橙色像素段（>= 6% 宽度），只有按钮本体满足；
    再按行聚类取第一簇，即"开始阅读/下载本章/收藏"那一行。
    """
    got = RawRGB(path)
    if not got:
        return None
    w, h, raw = got
    need = int(w * min_run)
    rows = []
    for y in range(int(h * y0), int(h * y1), 3):
        base = y * w * 3
        run = best = 0
        for x in range(0, w, 2):
            i = base + x * 3
            r, g, b = raw[i], raw[i + 1], raw[i + 2]
            if r > 195 and 85 < g < 195 and b < 100:
                run += 2
                best = max(best, run)
            else:
                run = 0
        if best >= need:
            rows.append(y)
    if not rows:
        return None
    clusters = []
    start = prev = rows[0]
    for y in rows[1:]:
        if y - prev <= 9:
            prev = y
        else:
            clusters.append((start, prev))
            start = prev = y
    clusters.append((start, prev))
    a, b = clusters[0]
    return (a + b) // 2


def Diff(a, b):
    if not a or not b or a["md5"] == b["md5"]:
        return 0.0
    return 1.0 if a["colors"] != b["colors"] else 0.5


def Alive():
    rc, out, _ = Adb("shell", "pidof", PKG)
    return rc == 0 and out.strip() != ""


def Focused():
    rc, out, _ = Adb("shell", "dumpsys", "window")
    for line in out.splitlines():
        if "mCurrentFocus" in line:
            return PKG in line
    return False


# 基准分辨率；启动时按 wm size 计算缩放
BASE_W, BASE_H = 1080, 2400
SX, SY = 1.0, 1.0


def SetScale(w, h):
    global SX, SY
    SX = w / float(BASE_W)
    SY = h / float(BASE_H)


def Tap(x, y, wait=1.5):
    Adb("shell", "input", "tap", str(int(x * SX)), str(int(y * SY)))
    time.sleep(wait)


def Key(code, wait=1.5):
    Adb("shell", "input", "keyevent", str(code))
    time.sleep(wait)


def Swipe(x1, y1, x2, y2, ms=300, wait=1.5):
    Adb("shell", "input", "swipe", str(int(x1 * SX)), str(int(y1 * SY)),
        str(int(x2 * SX)), str(int(y2 * SY)), str(ms))
    time.sleep(wait)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk", default="")
    ap.add_argument("--no-install", action="store_true")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    rc, out, _ = Adb("devices")
    Check("device" in out, "adb 有在线设备", out.strip().replace("\n", " ")[:80])
    if "device" not in out:
        return 2
    rc, boot, _ = Adb("shell", "getprop", "sys.boot_completed")
    Check(boot.strip() == "1", "AVD 已启动完成")
    rc, size, _ = Adb("shell", "wm", "size")
    m = re.search(r"(\d+)x(\d+)", size)
    sw, sh = (int(m.group(1)), int(m.group(2))) if m else (BASE_W, BASE_H)
    if sw > sh:   # 设备当前横屏时按长边当高
        sw, sh = sh, sw
    SetScale(sw, sh)
    Check(sw >= 1080 and sh > sw, "屏幕竖屏且宽 >= 1080（坐标按比例缩放）",
          "%dx%d scale=%.2fx%.2f" % (sw, sh, SX, SY))

    if args.apk and not args.no_install:
        print("install " + args.apk)
        rc, out, err = Adb("install", "-r", "-t", args.apk, timeout=300)
        Check(rc == 0 and "Success" in out, "安装 APK", err.strip()[-80:])

    Adb("shell", "am", "force-stop", PKG)
    Adb("shell", "settings", "put", "system", "user_rotation", "0")
    # 冷启动后设备可能停在锁屏：先唤醒再拉起，否则 am start 后会停在锁屏/桌面，
    # 后续所有坐标点击都会打到系统界面上（表现为"抽屉点不开"）。
    Adb("shell", "input", "keyevent", "224")
    Adb("shell", "wm", "dismiss-keyguard")
    time.sleep(1)
    Adb("logcat", "-c")
    Adb("shell", "am", "start", "-n", ACT)
    time.sleep(9)

    home = Shot("01_home")
    i_home = Info(home)
    Check(Alive(), "应用启动后进程存活")
    Check(i_home is not None and i_home["std"] > 8 and i_home["colors"] > 20,
          "首页非空白", "%dx%d std=%.1f colors=%d" %
          (i_home["w"], i_home["h"], i_home["std"], i_home["colors"]) if i_home else "no shot")
    Check(i_home is not None and i_home["w"] < i_home["h"], "默认竖屏")

    # ---- 抽屉 ----
    Tap(69, 171, 1.5)
    drawer = Shot("02_drawer")
    i_drawer = Info(drawer)
    if i_drawer is not None and i_drawer["md5"] == i_home["md5"]:
        Tap(69, 171, 1.5)      # 首次启动偶发"点太早"，补一次
        drawer = Shot("02_drawer")
        i_drawer = Info(drawer)
    Check(i_drawer is not None and i_drawer["md5"] != i_home["md5"], "点菜单打开抽屉")

    # 历史上这里点「搜索」再按返回，断言"回首页且留在应用内"。真机 1440x3168 上
    # 基准坐标 (153,1953) 落在抽屉的「首页」项（安全区 + 字体缩放让抽屉项下移），
    # 03_search.png 实际仍是首页；于是在首页按返回 —— 按设计就该退出应用。
    # 0.90 用 logcat 确认了这一点（jm-key: page=0），因此改为确定性断言：
    # 抽屉展开时按返回，只收起抽屉、应用留在前台。这才是"返回只退一级"的本意。
    Key(4, 1.5)
    back_home = Shot("04_back_home")
    i_back = Info(back_home)
    Check(Focused() and i_back is not None and i_drawer is not None
          and i_back["md5"] != i_drawer["md5"],
          "返回键收起抽屉且留在应用内（只退一级）")

    # ---- 首页滚动 / 分页 ----
    i_scrolled = None
    for attempt in range(2):
        Swipe(540, 1900, 540, 700, 400, 2.0)
        scrolled = Settle("05_scrolled", max_s=4.0, quiet_s=0.4)
        i_scrolled = Info(scrolled)
        if i_scrolled is not None and i_scrolled["md5"] != i_home["md5"]:
            break
    Check(i_scrolled is not None and i_scrolled["md5"] != i_home["md5"],
          "首页上滑滚动")

    # ---- 详情 / 收藏 ----
    Adb("shell", "input", "swipe", str(int(540 * SX)), str(int(700 * SY)),
        str(int(540 * SX)), str(int(2000 * SY)), "300")  # 回顶部
    time.sleep(1.5)
    Tap(282, 655, 1.0)
    # 真实数据到达后标签行会让按钮行下移，先等画面稳定再定位
    detail = Settle("06_detail")
    i_detail = Info(detail)
    Check(Alive() and i_detail is not None and i_detail["md5"] != i_home["md5"], "点卡片进详情")
    # 详情页会在"演示数据 → 真实数据"之间再刷一次，布局跟着位移，
    # 因此每次点击前都重新等稳定 + 重新定位按钮行，不复用上一次的 y。
    def detail_row():
        snap = Settle("_detail_now", max_s=8.0)
        y = OrangeButtonY(snap)
        return snap, (y if y is not None else int(1263 * SY))

    snap, row_y = detail_row()
    Tap(0.524 * BASE_W, row_y / SY, 1.2)   # 收藏 / 已收藏
    fav = Shot("07_favorite")
    i_fav = Info(fav)
    Check(i_fav is not None and Info(snap) is not None
          and i_fav["md5"] != Info(snap)["md5"], "收藏按钮切换",
          "button_row_y=%d" % row_y)

    # ---- 阅读器 ----
    snap, row_y = detail_row()
    Tap(0.136 * BASE_W, row_y / SY, 2.0)   # 开始阅读
    reader = Settle("08_reader")
    i_reader = Info(reader)
    opened = (Alive() and i_reader is not None and i_detail is not None
              and i_reader["md5"] != i_detail["md5"])
    if not opened:
        # 布局又变过 / 数据侧 0 话：重新定位后再试一次
        snap, row_y = detail_row()
        Tap(0.136 * BASE_W, row_y / SY, 2.5)
        reader = Settle("08_reader")
        i_reader = Info(reader)
        opened = (i_reader is not None and i_detail is not None
                  and i_reader["md5"] != i_detail["md5"])
    Check(Alive() and opened, "进入阅读器", "" if opened else "详情页 0 话时无法进入")
    if opened:
        Tap(0.85 * BASE_W, 0.80 * BASE_H, 1.2)   # 右下角 = 下一页
        reader2 = Settle("09_reader_next", max_s=6.0)
        i_reader2 = Info(reader2)
        Check(i_reader2 is not None and i_reader["md5"] != i_reader2["md5"],
              "点按翻到下一页")
    else:
        Skips.append("点按翻到下一页（未进入阅读器）")
    # 断言放在"第一次返回"：这才是要验的性质（返回不会直接把应用 finish 掉）。
    # 第二次返回若已到首页，按设计会退出应用（最后一条用例专门验这个），
    # 所以不再对第二次做 Focused 断言；后续用例需要首页时在退出后重新拉起。
    Key(4, 1.5)           # 阅读器 -> 详情/首页
    after_back = Shot("11_after_reader_back")
    Check(Focused(), "阅读器返回详情/首页后仍在应用内",
          "focus=%s" % ("app" if Focused() else "not-app"))
    Key(4, 1.5)           # 再退一级
    if not Focused():
        Adb("shell", "am", "start", "-n", ACT)
        time.sleep(6)

    # ---- 长按菜单 ----
    Adb("shell", "input", "swipe", "282", "655", "282", "655", "900")
    time.sleep(1.5)
    menu = Shot("10_longpress")
    i_menu = Info(menu)
    Check(i_menu is not None and i_menu["md5"] != i_home["md5"], "长按卡片弹菜单")
    Key(4, 1.2)

    # ---- 横屏（headless AVD 常常不响应旋转，能转就断言，转不动记 skip）----
    Adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
    Adb("shell", "settings", "put", "system", "user_rotation", "1")
    Adb("emu", "rotate")
    rotated = False
    for _ in range(8):
        time.sleep(1)
        probe = Info(Shot("_probe"))
        if probe and probe["w"] > probe["h"]:
            rotated = True
            break
    if rotated:
        land = Shot("11_landscape")
        i_land = Info(land)
        Check(Alive() and i_land is not None and i_land["w"] > i_land["h"], "横屏不崩且铺满宽度")
        Adb("shell", "settings", "put", "system", "user_rotation", "0")
        Adb("emu", "rotate")
        for _ in range(8):
            time.sleep(1)
            probe = Info(Shot("_probe"))
            if probe and probe["h"] > probe["w"]:
                break
        port = Shot("12_portrait")
        i_port = Info(port)
        Check(i_port is not None and i_port["h"] > i_port["w"], "转回竖屏正常")
    else:
        print("  skip  横屏渲染（headless AVD 不响应旋转；真机/带窗口模拟器可验）")
        Skips.append("横屏渲染")
        Shot("12_portrait")
        Check(Alive(), "旋转设置后应用仍存活")

    # ---- 返回退出（首页按返回 = 退出）----
    Key(4, 2.0)
    Check(not Focused(), "首页返回键退出应用（系统行为）")

    # ---- 日志 ----
    rc, log, _ = Adb("logcat", "-d")
    fatal = [l for l in log.splitlines()
             if ("FATAL EXCEPTION" in l or ("ANR in " + PKG) in l)]
    Check(not fatal, "logcat 无 FATAL/ANR", (fatal[-1][:90] if fatal else ""))

    print("")
    print("=" * 66)
    print("PASS %d / FAIL %d / SKIP %d" % (Passes, len(Failures), len(Skips)))
    for f in Failures:
        print("  - " + f)
    for f in Skips:
        print("  ~ " + f)
    return 0 if not Failures else 1


if __name__ == "__main__":
    sys.exit(main())
