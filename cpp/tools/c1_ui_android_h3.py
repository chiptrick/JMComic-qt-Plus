# coding: utf-8
"""C++ UI Android 端 HTTP/3 回归（真机 / 模拟器通用）。

0.90 补的这层用例回答一个问题：**"QUIC 到底能不能验收"**。它把 0.89 只能靠
人工看日志的几件事变成可复跑的断言：

  1. 能力      --advanced-status 里 quic=1 / http3=1 / echLib=1
  2. ECH       --ech-probe 走通 OpenSSL 4.x 通道（不再是 NOT_CONFIGURED）
  3. 探针      --h3-probe 拿到 HTTP/3 响应（ok=1 / version=HTTP/3 / alpn=h3）
  4. 回退      --doh-on + --h3-fail-inject bad-port 时业务仍存活，且出现 h3 fallback
  5. 采样      --h3-bench 输出 P50/P95，且成功数达标
  6. 直连 H3   --direct-h3-probe 走 quicOnly 路径；注入失败时回落到 QNAM/HTTP2 且业务成功
  7. Q4 决策   H3 默认开启；注入 ech-reject 时丢弃暴露 SNI 的 H3 结果、回退 TCP+ECH，
              本进程内按 (host,节点) 抑制，冷启动 / 换节点后重新尝试组合

跑法：
  python3 cpp/tools/c1_ui_android_h3.py
  python3 cpp/tools/c1_ui_android_h3.py --serial 10AF5K2B0D001AM
  python3 cpp/tools/c1_ui_android_h3.py --apk cpp/dist/xxx.apk     # 先安装再跑
  python3 cpp/tools/c1_ui_android_h3.py --bench 10 --skip-install

前置：设备已 boot 完成、联网、adb 可用；APK 里带了 QUIC（--with-quic 构建）。
产物：每步的 logcat 摘要留在 dist/h3-regress/<step>.log，便于失败时回看。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PKG = "org.jmcomic.ui"
ACT = PKG + "/org.qtproject.qt.android.bindings.QtActivity"
OUTDIR = os.path.join(ROOT, "cpp", "dist", "h3-regress")

Passes = 0
Failures = []
Skips = []


def _resolve_adb():
    env = os.environ.get("ADB")
    if env and os.path.exists(env):
        return env
    found = shutil.which("adb")
    if found:
        return found
    return os.path.join(
        os.environ.get("LOCALAPPDATA", os.path.expanduser("~\\AppData\\Local")),
        "Android", "Sdk", "platform-tools", "adb.exe")


ADB = _resolve_adb()


def Run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return (p.returncode,
                p.stdout.decode("utf-8", "replace"),
                p.stderr.decode("utf-8", "replace"))
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def Adb(*args, **kw):
    return Run([ADB] + (["-s", SERIAL] if SERIAL else []) + list(args), **kw)


def Shell(cmd, timeout=120):
    """整条命令作为单个参数交给远端 sh。

    不能写成 Adb("shell", "am", "start", ..., "--es", "jm.args", "--h3-probe x")
    —— adb 会把每个参数重新拼成远端命令，带空格的值会被拆散（0.89 踩过）。
    """
    return Adb("shell", cmd, timeout=timeout)


def Check(ok, name, detail=""):
    global Passes
    if ok:
        Passes += 1
        print("  PASS  %s%s" % (name, ("  [" + detail + "]") if detail else ""))
    else:
        Failures.append("%s%s" % (name, ("  [" + detail + "]") if detail else ""))
        print("  FAIL  %s%s" % (name, ("  [" + detail + "]") if detail else ""))


def Skip(name, why):
    Skips.append("%s（%s）" % (name, why))
    print("  skip  %s  (%s)" % (name, why))


def _LaunchOnce(args, wait, tag):
    """force-stop → 清日志 → 带 jm.args 启动 → 等 → 返回 logcat 文本。"""
    Adb("shell", "am force-stop " + PKG)
    Adb("logcat", "-c")
    rc, out, err = Shell("am start -n %s --es jm.args '%s'" % (ACT, args))
    time.sleep(wait)
    # H3 默认开启后启动期业务请求也会做一次 QUIC 探测，日志量变大；
    # 4000 行会被挤掉早期的 advanced-status，放宽到 8000 行。
    _, log, _ = Adb("logcat", "-d", "-t", "8000", timeout=180)
    try:
        if not os.path.isdir(OUTDIR):
            os.makedirs(OUTDIR)
        with open(os.path.join(OUTDIR, tag + ".log"), "w", encoding="utf-8") as f:
            f.write("# am start: %s\n# rc=%d\n%s\n%s\n" % (args, rc, out, log))
    except OSError:
        pass
    return log


def Launch(args, wait, tag):
    """_LaunchOnce + 冷启动无应用日志时统一重放一次。

    force-stop 后首启偶发整段拿不到应用日志（ART/odex 检查 + Qt 初始化；
    H3 默认开后启动期还会多一次 QUIC 探测，出现得更频繁）。以前只有第 1/6
    组各自写了重放，bench / 注入 / ECH 组仍会误报 SKIP 或 FAIL，统一在这里兜底。
    """
    log = _LaunchOnce(args, wait, tag)
    if not re.search(r"\b(jmcomic|C1Net)\s*:", log):
        log = _LaunchOnce(args, wait, tag + "-cold-retry")
    return log


def Pid():
    _, out, _ = Adb("shell", "pidof " + PKG)
    return out.strip()


def Grep(log, pattern):
    m = re.search(pattern, log, re.MULTILINE)
    return m


def main():
    global SERIAL
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default="")
    ap.add_argument("--apk", default="")
    ap.add_argument("--skip-install", action="store_true")
    ap.add_argument("--bench", type=int, default=5)
    ap.add_argument("--album", default="313833", help="回复用例打开的作品 id")
    args = ap.parse_args()

    SERIAL = args.serial
    if not SERIAL:
        _, out, _ = Adb("devices")
        devs = [l.split()[0] for l in out.splitlines()
                if l.strip().endswith("device") and "\t" in l]
        if not devs:
            print("[x] no device in 'device' state; run adb devices")
            return 2
        SERIAL = devs[0]
    print("device: %s" % SERIAL)
    _, model, _ = Shell("getprop ro.product.model")
    _, sdk, _ = Shell("getprop ro.build.version.sdk")
    _, abi, _ = Shell("getprop ro.product.cpu.abi")
    print("model=%s sdk=%s abi=%s" % (model.strip(), sdk.strip(), abi.strip()))

    if args.apk and not args.skip_install:
        rc, out, err = Adb("install", "-r", "-t", args.apk, timeout=300)
        installed_via_pm = False
        if rc != 0 or "Success" not in (out + err):
            # V2463A / Android 17 上 adb install 偶发
            # INSTALL_FAILED_ABORTED: User rejected permissions —— 安装器 UI 被
            # 系统自动拒绝（设备屏幕上并没有可点的确认框）。push 到 /data/local/tmp
            # 后用 shell 侧 pm install 走同一套 PackageManager，可绕开该 UI。
            remote = "/data/local/tmp/jm-h3-regress.apk"
            rc2, out2, err2 = Adb("push", args.apk, remote, timeout=300)
            if rc2 == 0:
                rc3, out3, err3 = Shell("pm install -r -t " + remote, timeout=300)
                if "Success" in (out3 + err3):
                    installed_via_pm = True
                    print("installed via pm install: %s" % os.path.basename(args.apk))
                    rc, out, err = 0, out3, err3
        if rc != 0 or "Success" not in (out + err):
            print("[x] install failed: %s %s" % (out.strip()[-200:], err.strip()[-200:]))
            return 2
        if not installed_via_pm:
            print("installed: %s" % os.path.basename(args.apk))

    # ---- 1. 能力摘要 ----
    print("\n==> 1. 能力摘要")
    # 冷启动 9s 在真机上偶尔不够（Qt 首帧 + 真实首页请求），拿不到就再等一轮
    log = Launch("--advanced-status", 10, "1-capability")
    m = Grep(log, r"advanced-status: (.*)")
    if m is None:
        log = Launch("--advanced-status", 8, "1-capability")
        m = Grep(log, r"advanced-status: (.*)")
    if m is None:
        Skip("能力摘要（quic/http3/echLib）", "未输出 advanced-status")
    else:
        line = m.group(1)
        Check("quic=1" in line and "http3=1" in line,
              "QUIC/HTTP3 在 APK 内可用", line[:150])
        Check("echLib=1" in line, "ECH 通道（OpenSSL 4.x）可加载", line[:150])

    # ---- 2. ECH ----
    print("\n==> 2. ECH 通道")
    # DoH 取 ECHConfig 有时要 10s 以上（尤其首次），拿不到就再等一轮
    log = Launch("--ech-probe cloudflare.com", 20, "2-ech")
    m = Grep(log, r"ech-probe: host=\S+ (ok ech=(\w+)\((-?\d+)\)|failed: (.*))")
    if m is None:
        log = Launch("--ech-probe cloudflare.com", 15, "2-ech")
        m = Grep(log, r"ech-probe: host=\S+ (ok ech=(\w+)\((-?\d+)\)|failed: (.*))")
    if m is None:
        Skip("ECH 握手（--ech-probe）", "未输出结果行")
    else:
        if m.group(2):
            status = m.group(2)
            code = int(m.group(3))
            # SUCCESS / GREASE / GREASE_ECH 都算"通道可用"；FAILED 也算通道可用但
            # 需要区分——这里只断言"不是 NOT_CONFIGURED(-103)"，具体状态打印出来。
            Check(code != -103, "ECH 通道不是 NOT_CONFIGURED",
                  "ech=%s(%d)" % (status, code))
            if status == "SUCCESS":
                print("        ech=SUCCESS（真实 ECH 握手成功）")
        else:
            Check(False, "ECH 握手失败", (m.group(4) or "")[:120])

    # ---- 3. H3 探针 ----
    print("\n==> 3. HTTP/3 探针")
    log = Launch("--h3-probe cloudflare.com --h3-path /", 18, "3-probe")
    m = Grep(log, r"h3-probe: done ok=(\d) status=(\d+)[^\r\n]*?(\d+)ms[^\r\n]*"
                  r"version=(\S*)[^\r\n]*alpn=(\S*)")
    if m is None:
        Skip("HTTP/3 请求（--h3-probe）", "未输出 done 行")
    else:
        ok = m.group(1) == "1"
        Check(ok, "H3 请求成功", m.group(0)[:130])
        if ok:
            Check(m.group(4).startswith("HTTP/3"), "响应版本为 HTTP/3", m.group(4))
            Check(m.group(5).startswith("h3"), "ALPN 协商为 h3", m.group(5))

    # ---- 4. 回退（受控注入）----
    print("\n==> 4. H3 失败回退（注入 bad-port）")
    # 保留 --doh-on：这一项验的是「节点/DoH 路由」的 H3 失败回退（connectIp 非空，
    # 整条请求走 NodeHttp），与第 6 项的「直连路由」是两条独立路径。
    # 0.90 之前直连走 QNAM 且没有 H3 分支，那个覆盖缺口已在 0.90 补上（见第 6 项）。
    log = Launch("--doh-on --http3-on --h3-fail-inject bad-port --open-album %s" % args.album,
                 40, "4-fallback")
    injected = "h3 inject: forced port 9" in log
    # 回退日志走 C1Net（Info），不是 jmcomic（Warn），两者都要看。
    # 0.90 补录（§14.7）：DoH 路由带 ECH 组合时，失败走的是 Q4 分支，
    # 日志为 "ech/h3 rejected ... -> tcp+ech" 而不是通用的 "h3 fallback (...)"；
    # 两条都算成功回退，后者仍是 quicOnly / 无 ECH 配置时的路径。
    fellback = (Grep(log, r"h3 fallback \(") is not None
                or ("ech/h3 rejected" in log and "-> tcp+ech" in log))
    alive = bool(Pid())
    Check(injected, "注入生效（H3 指向 discard 端口）",
          "" if injected else "未见 inject 行")
    Check(fellback, "H3 失败后回退到 TCP（HTTP/1.1 或 TCP+ECH）",
          "" if fellback else "未见 h3 fallback / ech-h3 rejected 行")
    Check(alive, "回退后应用仍存活", "" if alive else "pidof 为空")

    # ---- 5. 采样 ----
    print("\n==> 5. 首包采样（--h3-bench %d）" % args.bench)
    # 真机冷启动 + 5 次全新握手偶尔超过 42s，等宽一点避免把"没等到汇总行"误报成 SKIP
    log = Launch("--h3-bench %d --h3-probe cloudflare.com" % args.bench,
                 18 + args.bench * 8, "5-bench")
    m = Grep(log, r"h3-bench: n=(\d+) ok=(\d+) p50=(\d+)ms p95=(\d+)ms "
                  r"min=(\d+)ms max=(\d+)ms")
    if m is None:
        Skip("首包 P50/P95", "未输出汇总行")
    else:
        n, ok, p50, p95 = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        Check(ok >= max(1, n - 1), "采样成功数达标", "ok=%d/%d" % (ok, n))
        Check(int(p50) > 0, "P50/P95 可读", "p50=%sms p95=%sms" % (p50, p95))

    # ---- 6. 直连 H3（0.90 新增）----
    # --direct-h3-probe 建一个干净的 NetClient，不经 JmApi 的节点/DoH 计划，
    # 因此无论设备当前配置如何，--http3-on 时都会进入「先 H3、失败回 QNAM/HTTP2」。
    # 注意不要复用 --net-probe：那是桌面回归用的同步 QEventLoop 探针。
    print("\n==> 6. 直连请求先试 H3（--direct-h3-probe）")
    cmd = "--http3-on --direct-h3-probe https://cloudflare.com"
    log = Launch(cmd, 20, "6-direct-h3")
    # force-stop 后首启偶发拿不到应用日志（ART/odex 检查 + Qt 初始化），重放一次再判。
    if "via h3-probe" not in log:
        log = Launch(cmd, 20, "6-direct-h3-retry")
    entered = "via h3-probe" in log
    got = "direct-h3-probe: net OK" in log
    Check(entered, "直连请求进入 quicOnly/H3 探测",
          "" if entered else "未见 via h3-probe")
    Check(got, "直连请求拿到响应", "" if got else "未见 direct-h3-probe: net OK")

    cmd = ("--http3-on --h3-fail-inject bad-port "
           "--direct-h3-probe https://cloudflare.com")
    log = Launch(cmd, 34, "6-direct-fallback")
    # 冷启动偶发拿不到应用日志（ART odex 检查 / Qt 初始化慢），重放一次再判。
    if "via h3-probe" not in log:
        log = Launch(cmd, 24, "6-direct-fallback-retry")
    fellback = "h3 probe unavailable, fallback to QNAM" in log
    injected = "h3 inject: forced port 9" in log
    survived = "direct-h3-probe: net OK" in log
    Check(injected, "直连路径的 H3 注入生效", "" if injected else "未见 forced port 9")
    Check(fellback, "直连 H3 失败回落到 QNAM/HTTP2",
          "" if fellback else "未见 h3 probe unavailable")
    Check(survived, "直连回退后业务仍成功", "" if survived else "未见 direct-h3-probe: net OK")

    # ---- 7. Q4：H3 默认开 + 隐藏 SNI 优先 + 重启/换节点重试 ----
    # 用户决策（0.90 收口）：H3 默认开启；服务端拒绝 ECH-over-QUIC 时暂时
    # 回退 TCP+ECH；下一次启动或更换节点时重新尝试 H3+ECH 组合。
    print("\n==> 7. Q4：H3 默认开启 / ECH 策略拒绝后的回退与重试")
    log = Launch("--advanced-status", 10, "7-default-on")
    m = Grep(log, r"advanced-status: (.*)")
    if m is None:
        log = Launch("--advanced-status", 8, "7-default-on-retry")
        m = Grep(log, r"advanced-status: (.*)")
    if m is None:
        Skip("H3 默认开启 / 新进程复位", "未输出 advanced-status")
    else:
        line = m.group(1)
        Check("HTTP3 开" in line,
              "不带 --http3-on 时 H3 仍默认开启", line[:130])
        Check("echH3Blocked=0" in line,
              "新进程无遗留抑制（冷启动重新尝试 H3+ECH）", line[-70:])

    log = Launch("--ech-h3-selftest", 12, "7-selftest")
    if "ech-h3-selftest:" not in log:
        log = Launch("--ech-h3-selftest", 10, "7-selftest-retry")
    m = Grep(log, r"ech-h3-selftest: ([^\r\n]*?)\s*-> (\w+)")
    Check(m is not None and m.group(2) == "PASS",
          "抑制按 (host,节点) 生效，换节点/重置后重新尝试",
          (m.group(1).strip()[:130] if m else "未见 selftest 结果行"))

    # 用 www 固定域名：cloudflare.com 会 301 到 www，重定向后 host 变化会
    # 再登记一条 (host,node) 抑制，让「本进程只试一次组合」的断言变得不确定。
    cmd = "--h3-fail-inject ech-reject --ech-h3-probe https://www.cloudflare.com/"
    log = Launch(cmd, 30, "7-ech-reject")
    if "ech-h3-probe: pass=1" not in log:
        log = Launch(cmd, 24, "7-ech-reject-retry")
    rejected = "ech/h3 rejected" in log and "-> tcp+ech" in log
    p1 = Grep(log, r"ech-h3-probe: pass=1 ok=(\d)[^\r\n]*echH3Blocked=(\d+)")
    p2 = Grep(log, r"ech-h3-probe: pass=2 ok=(\d)[^\r\n]*echH3Blocked=(\d+)")
    suppressed = "combo suppressed" in log
    Check(rejected, "ECH/H3 被拒后丢弃 H3 结果并回退 TCP+ECH",
          "" if rejected else "未见 ech/h3 rejected -> tcp+ech")
    Check(p1 is not None and p1.group(1) == "1" and p1.group(2) == "1",
          "首次请求回退后业务成功且登记进程内抑制",
          p1.group(0)[:130] if p1 else "未见 pass=1")
    Check(p2 is not None and p2.group(1) == "1" and p2.group(2) == "1",
          "同进程第二次请求业务仍成功",
          p2.group(0)[:130] if p2 else "未见 pass=2")
    Check(suppressed, "本进程内暂时不再尝试 H3+ECH 组合",
          "" if suppressed else "未见 combo suppressed")

    print("")
    print("=" * 66)
    print("PASS %d / FAIL %d / SKIP %d" % (Passes, len(Failures), len(Skips)))
    for f in Failures:
        print("  - " + f)
    for f in Skips:
        print("  ~ " + f)
    print("日志目录: %s" % OUTDIR)
    return 0 if not Failures else 1


if __name__ == "__main__":
    SERIAL = ""
    sys.exit(main())
