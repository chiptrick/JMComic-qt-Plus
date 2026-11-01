# coding:utf-8
"""基准 1/3：冷启动到首帧（毫秒）+ 首帧时刻的进程 RSS（MB）

口径（复现方式，改口径等于换尺子）
------------------------------------------------------------------
* **冷启 = 新进程**：每次测量都用 `subprocess` 起一个全新的解释器，
  被测进程里不含任何测量代码（父进程只读它的输出）。
  ⚠ 无 root 时不会清 OS page cache：连续跑 5 次，后几次会因页缓存而偏快。
    要真正的"冷"，先 `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches` 再跑。
* **首帧的判定点**：`src/start.py` 的 `Run(afterShow=...)` 回调 —— 它在
  `main.show()` + `app.processEvents()` 之后、`Main.Init()` 之前被调用，
  也就是"主窗口已把第一帧画出去、业务初始化还没开始"的那一瞬。
  这是代码本来就有的扩展点（Android 用它把重活挪到首帧之后），**没有为测量改任何业务代码**。
* **两个时间**：`startup_first_frame_ms` 由子进程自报（`perf_counter` 从子进程
  第一条语句起算）；`startup_wall_ms` 由父进程墙钟测（从 Popen 到收到标记，
  含解释器启动）。做阈值判定用 `startup_first_frame_ms`，排障时看 wall 与它的差值。
* **RSS** 取首帧那一刻子进程 `/proc/<pid>/status` 的 `VmRSS`（kB→MB，1024 进制）。
* 默认 `QT_QPA_PLATFORM=offscreen`（CI 无显示器）；超分后端默认照真实启动流程初始化，
  想排除它的干扰用 `--skip-sr`。

用法：
    python script/bench_startup.py                    # 5 次，人类可读 + 一行 JSON
    python script/bench_startup.py --runs 10 --json   # 只出 JSON（存库对比用）
    python script/bench_startup.py --out /tmp/startup.json

退出码：0 正常 / 1 测量失败（子进程没起来或超时）/ 2 平台不支持或缺依赖
"""
import os
import subprocess
import sys
import time

import bench_common as bc

MARKER = "@@BENCH_FIRST_FRAME@@"
DEFAULT_TIMEOUT = 180.0     # 冷启超时（秒）：/mnt/c 上首次导入 22MB 的 images_rc 可能很慢


# ------------------------------------------------------------------ 子进程侧
def RunChild():
    """ 子进程：真的把应用启动到首帧，然后在 afterShow 里打标记并立刻退出

    这里刻意**照抄 src/start.py 的 __main__ 顺序**（InitSrEngine() -> Run()），
    保证测的就是用户双击启动走的那条路。
    """
    t0 = time.perf_counter()
    src = ""
    skipSr = "--skip-sr" in sys.argv
    for index, item in enumerate(sys.argv):
        if item == "--src" and index + 1 < len(sys.argv):
            src = sys.argv[index + 1]
    if src:
        sys.path.insert(0, src)

    # 从这条语句起算（解释器已经起来了，但应用的 import 一个都还没做）
    import start

    def Hook():
        elapsed = (time.perf_counter() - t0) * 1000.0
        rss = bc.RssMbOf(os.getpid())
        # 只打一行给父进程解析；应用自己的输出可能混在 stdout 里，父进程按前缀过滤
        print("{} {:.3f} {}".format(MARKER, elapsed, "-" if rss is None else rss), flush=True)
        # 不走正常退出：Run() 后面还有 app.exec() 与 Main.Init()，这里只要首帧
        os._exit(0)

    if not skipSr:
        start.InitSrEngine()
    start.Run(afterShow=Hook)


# ------------------------------------------------------------------ 父进程侧
def Preflight(args):
    """ 解释器必须能 import 应用依赖（缺 curl_cffi 时启动会在建主窗口阶段就失败） """
    code = "import PySide6, curl_cffi; print(PySide6.__version__)"
    try:
        out = subprocess.run([args.python, "-c", code], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=120)
    except Exception as es:
        return "解释器不可用: {} ({})".format(args.python, es)
    if out.returncode != 0:
        tail = (out.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-3:]
        return ("解释器缺少应用依赖: {}\n       {}\n"
                "       提示: 用装了依赖的解释器，例如 "
                "--python ~/jmcomic-build/venv-test/bin/python\n"
                "       （venv311 只装了构建工具链，没有 curl_cffi）").format(
            args.python, " / ".join(tail))
    return ""


def Measure(args, runIndex, timeout):
    """ 跑一次冷启，返回 (firstFrameMs, wallMs, rssMb) 或抛 RuntimeError """
    cmd = [args.python, os.path.abspath(__file__), "--child", "--src", bc.SrcDir(args)]
    if args.skip_sr:
        cmd.append("--skip-sr")

    errFile = os.path.join(args._tmpdir, "startup-{}.err".format(runIndex))
    start = time.perf_counter()
    with open(errFile, "wb") as errHandle:
        proc = subprocess.Popen(cmd, cwd=bc.RepoDir(args), env=bc.QtEnv(args),
                                stdout=subprocess.PIPE, stderr=errHandle,
                                bufsize=1, universal_newlines=True)
        try:
            deadline = start + timeout
            while True:
                if time.perf_counter() > deadline:
                    raise RuntimeError("超过 {:.0f}s 未到首帧".format(timeout))
                line = proc.stdout.readline()
                if not line:
                    if proc.poll() is not None:
                        raise RuntimeError("子进程提前退出 rc={}".format(proc.returncode))
                    continue
                if MARKER in line:
                    wall = (time.perf_counter() - start) * 1000.0
                    parts = line.strip().split()
                    firstFrame = float(parts[1])
                    rss = None if parts[2] == "-" else float(parts[2])
                    return firstFrame, wall, rss
        finally:
            try:
                proc.kill()
                proc.wait(timeout=10)
            except Exception:
                pass
            try:
                proc.stdout.close()
            except Exception:
                pass


def Tail(path, lines=12):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return [l.rstrip("\n") for l in f.readlines()[-lines:]]
    except Exception:
        return []


def Main():
    parser = bc.MakeParser(
        "基准 1/3：冷启动到首帧（毫秒）+ 首帧时刻 RSS（MB）",
        epilog="示例:\n"
               "  python script/bench_startup.py --runs 5\n"
               "  python script/bench_startup.py --runs 20 --json --out /tmp/startup.json\n" 
               "想真正清页缓存(需 root): sync; echo 3 | sudo tee /proc/sys/vm/drop_caches")
    parser.add_argument("--skip-sr", action="store_true",
                        help="启动时不初始化超分后端（用来区分 sr 初始化对首帧的影响）")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="单次冷启超时秒数（默认 {:.0f}）".format(DEFAULT_TIMEOUT))
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.child:      # 子进程分支：不进父进程逻辑，也不打印统计
        RunChild()
        return 0

    if args.runs < 1:
        parser.error("--runs 必须 >= 1")
    bc.RequireRss()
    problem = Preflight(args)
    if problem:
        sys.stderr.write("[error] {}\n".format(problem))
        return 2

    import tempfile
    args._tmpdir = tempfile.mkdtemp(prefix="jmbench-startup-")
    firstFrames, walls, rssList = [], [], []
    try:
        for index in range(args.runs):
            try:
                firstFrame, wall, rss = Measure(args, index, args.timeout)
            except RuntimeError as es:
                sys.stderr.write("[error] 第 {} 次冷启失败: {}\n".format(index + 1, es))
                tail = Tail(os.path.join(args._tmpdir, "startup-{}.err".format(index)))
                if tail:
                    sys.stderr.write("        子进程 stderr 末尾:\n")
                    for line in tail:
                        sys.stderr.write("          {}\n".format(line))
                sys.stderr.write("        常见原因: 已有桌面端实例在跑(单实例 socket 会让新进程直接退出)；\n"
                                 "                  缺 GUI 依赖/字体；--timeout 太小\n")
                return 1
            firstFrames.append(firstFrame)
            walls.append(wall)
            if rss is not None:
                rssList.append(rss)
            sys.stderr.write("  run {}/{}: 首帧 {:.1f} ms, wall {:.1f} ms, RSS {}\n".format(
                index + 1, args.runs, firstFrame, wall,
                "n/a" if rss is None else "{:.1f} MB".format(rss)))
    finally:
        try:
            import shutil
            shutil.rmtree(args._tmpdir, ignore_errors=True)
        except Exception:
            pass

    result = {
        "metric": "startup_first_frame",
        "runs": args.runs,
        "sr_engine": not args.skip_sr,
        "startup_first_frame_ms": bc.Stats(firstFrames),
        "startup_wall_ms": bc.Stats(walls),
        "rss_at_first_frame_mb": bc.Stats(rssList, unit="MB"),
        "env": bc.EnvInfo(args),
    }
    human = [
        "== 冷启动到首帧 =========================================",
        "  {:<28s} {:>9.1f} ms   (p50)   p95 {:>8.1f} ms   n={}".format(
            "首帧(子进程自报)", result["startup_first_frame_ms"]["p50"],
            result["startup_first_frame_ms"]["p95"], args.runs),
        "  {:<28s} {:>9.1f} ms   (p50, 含解释器启动)".format(
            "墙钟(父进程观测)", result["startup_wall_ms"]["p50"]),
        "  {:<28s} {:>9.1f} MB   (p50)".format(
            "首帧时刻 RSS (VmRSS)",
            result["rss_at_first_frame_mb"].get("p50", 0.0)),
        "  环境: Qt={} PySide6={} python={}".format(
            result["env"]["qt_platform"] or "(未设置)", result["env"]["pyside6"],
            result["env"]["python"]),
        "  注意: 无 root 不清页缓存，连续多次跑会偏快；做阈值判定请 --runs 20。",
    ]
    return bc.Emit(result, jsonOnly=args.json, humanLines=human, outPath=args.out)


if __name__ == "__main__":
    import argparse
    sys.exit(Main())
