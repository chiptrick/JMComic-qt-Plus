# coding: utf-8
"""本机 GUI 回归的"静默执行"支撑层（Windows）。

为什么需要它
------------
`c1_ui_desktop_suite.py` 要在本机反复拉起 jmcomic_ui.exe（几十次）。默认情况下
每个实例都会在主屏上弹出窗口并抢走前台焦点——跑一次回归，用户十几分钟没法干活。
本模块把这件事变得"无感"，分四层：

  1. 窗口静默（默认 hidden）：子进程带 `--quiet` 启动，窗口被移到可视区外，并加上
     WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW —— 不激活、不进任务栏/Alt-Tab、不覆盖任何
     显示器。截图仍走原生渲染路径（真实 DPI 与系统字体），像素与正常显示一致。
  2. 旧 exe 兜底：exe 不认识 `--quiet` 时自动退回 QT_QPA_PLATFORM=offscreen
     （完全没有窗口；代价是拿不到系统字体、DPR 退化为 1，报告里会标注降级）。
  3. 调度让路：本进程与所有子进程默认降到 BELOW_NORMAL_PRIORITY_CLASS，
     前台应用的输入延迟与帧率基本不受影响。
  4. 结果落盘：tee 出 suite.log，结束时写 report.md / results.json，
     失败用例额外保存完整 stderr（截图断言失败时最难复现的就是那几行日志）。

另有一个可选的托盘图标（Tray）：显示进度、完成后气泡提示、双击打开结果目录。
Shell_NotifyIcon 在无 Explorer 的环境会失败，这里一律降级为"没有托盘"。

用法：
    from quiet_runner import QuietExec, Reporter, Tray
    q = QuietExec(exe, base_env=env)
    mode = q.resolve()          # 'hidden' | 'minimized' | 'offscreen' | 'off'
    p = q.run(["--page", "home", "--shot", path], timeout=60)
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time

IS_WIN = (os.name == "nt")

# 静默模式
MODE_AUTO = "auto"
MODE_HIDDEN = "hidden"          # 窗口屏幕外：不抢焦点、不进任务栏（推荐）
MODE_MINIMIZED = "minimized"    # 窗口最小化：仍在任务栏，但不遮挡、不抢焦点
MODE_OFFSCREEN = "offscreen"    # 完全没有窗口：字体/DPR 降级
MODE_OFF = "off"                # 旧行为：正常弹窗（仅排障用）
MODES = (MODE_AUTO, MODE_HIDDEN, MODE_MINIMIZED, MODE_OFFSCREEN, MODE_OFF)

# 子进程优先级
PRIORITY_FLAGS = {"below": 0x00004000, "idle": 0x00000040, "normal": 0x00000000}

# 人在报告里看到的解释（比"hidden"这种词有用）
MODE_NOTES = {
    MODE_HIDDEN: "窗口在可视区外 + WS_EX_NOACTIVATE/WS_EX_TOOLWINDOW：不抢焦点、"
                 "不进任务栏与 Alt-Tab、不遮挡任何窗口；截图仍走原生渲染（真实 DPI/字体）",
    MODE_MINIMIZED: "窗口最小化启动：不进前台、不抢焦点；截图仍走原生渲染",
    MODE_OFFSCREEN: "Qt offscreen 无窗口：拿不到系统字体、DPR 退化为 1（仅作兜底）",
    MODE_OFF: "已关闭静默：窗口会正常出现在前台（仅排障用）",
}


def ApplyOwnPriority(level="below"):
    """把回归脚本自己也降到 below-normal。

    用例进程降级只解决一半问题：Python 侧还要解几十张截图（ffmpeg rawvideo），
    那是真正的 CPU 大户。整个进程树一起让路，前台应用才真的"无感"。
    """
    if not IS_WIN or level not in PRIORITY_FLAGS:
        return False
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.GetCurrentProcess.restype = wt.HANDLE
        return bool(k32.SetPriorityClass(k32.GetCurrentProcess(), PRIORITY_FLAGS[level]))
    except Exception:
        return False


class QuietExec(object):
    """按静默模式拼装环境/参数并执行子进程。"""

    def __init__(self, exe, base_env=None, mode=MODE_AUTO, priority="below",
                 probe_dir=None, probe_log=None):
        self.exe = exe
        self.base_env = dict(base_env or os.environ)
        self.requested = mode if mode in MODES else MODE_AUTO
        self.mode = MODE_OFF
        self.priority = priority if priority in PRIORITY_FLAGS else "below"
        self.note = ""
        self.degraded = False
        self.probe_dir = probe_dir
        self.probe_log = probe_log or (lambda s: None)
        self._probing = False
        self.calls = []          # [(elapsed_s, rc, argv)]

    # ---- 进程参数 ----
    def flags(self):
        if self.mode == MODE_HIDDEN:
            return ["--quiet"]
        if self.mode == MODE_MINIMIZED:
            return ["--quiet-minimized"]
        return []

    def env(self, extra=None):
        env = dict(self.base_env)
        if self.mode == MODE_OFFSCREEN:
            env["QT_QPA_PLATFORM"] = "offscreen"
            env.pop("JM_UI_QUIET", None)
        elif self.mode in (MODE_HIDDEN, MODE_MINIMIZED):
            # exe 支持 --quiet 时由参数驱动；环境变量给同一份 exe 的其它调用点兜底
            env["JM_UI_QUIET"] = self.mode
        else:
            # 普通模式：不动用户自己的 QT_QPA_PLATFORM 等设置
            env.pop("JM_UI_QUIET", None)
        if extra:
            env.update({k: v for k, v in extra.items() if v is not None})
        return env

    def creationflags(self):
        if not IS_WIN:
            return 0
        return PRIORITY_FLAGS.get(self.priority, 0)

    def run(self, args, timeout=60, env_extra=None, cwd=None):
        cmd = [self.exe] + self.flags() + list(args)
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           env=self.env(env_extra), cwd=cwd,
                           creationflags=self.creationflags(),
                           encoding="utf-8", errors="replace")
        if not self._probing:                 # 探测的那几次不算进用例统计
            self.calls.append((time.time() - t0, p.returncode, cmd))
        return p

    # ---- 模式探测 ----
    def _probe_once(self, mode):
        """用一次最便宜的截图运行验证某个模式可用（不会弹窗）。

        探测产物落在 probe_dir（默认跟随报告目录），不放系统临时目录：
        某些受限环境/安全软件会拦掉 %TEMP% 的写入，那会把"模式可用"误判成"不可用"。
        """
        tmp = None
        if self.probe_dir:
            os.makedirs(self.probe_dir, exist_ok=True)
            tmp = self.probe_dir
        else:
            tmp = tempfile.mkdtemp(prefix="jmsuite-probe-")
        shot = os.path.join(tmp, "probe_%s.png" % mode)
        if os.path.exists(shot):
            os.remove(shot)
        saved = (self.mode, self.base_env)
        self.mode = mode
        self._probing = True
        try:
            p = self.run(["--page", "home", "--shot", shot, "--shot-delay", "120"],
                         timeout=90)
            size = os.path.getsize(shot) if os.path.exists(shot) else 0
            ok = p.returncode == 0 and size > 8192
            detail = "rc=%d png=%dB" % (p.returncode, size)
            if not ok:
                tail = (p.stderr or "").strip().splitlines()[-2:]
                detail += ("  " + " | ".join(tail)) if tail else ""
            return ok, detail
        except subprocess.TimeoutExpired:
            return False, "超时（>90s）"
        except Exception as exc:                      # noqa: BLE001 - 探测失败不该中断回归
            return False, "%s: %s" % (type(exc).__name__, exc)
        finally:
            self.mode = saved[0]
            self.base_env = saved[1]
            self._probing = False
            if not self.probe_dir:
                shutil.rmtree(tmp, ignore_errors=True)

    def resolve(self):
        """选定最终模式；返回模式名，失败链写进 self.note。"""
        chain = []
        if self.requested == MODE_OFF:
            self.mode, self.note = MODE_OFF, MODE_NOTES[MODE_OFF]
            return self.mode

        order = [self.requested] if self.requested != MODE_AUTO else [MODE_HIDDEN, MODE_OFFSCREEN]
        if self.requested in (MODE_HIDDEN, MODE_MINIMIZED):
            order += [MODE_OFFSCREEN]   # 旧 exe 不认识 --quiet 时的兜底

        for mode in order:
            ok, detail = self._probe_once(mode)
            chain.append("%s: %s" % (mode, "OK" if ok else "失败(%s)" % detail))
            if ok:
                self.mode = mode
                self.degraded = (mode == MODE_OFFSCREEN and self.requested != MODE_OFFSCREEN)
                self.note = MODE_NOTES[mode]
                if self.degraded:
                    self.note = "降级——exe 不认 --quiet，只能走 offscreen；" + self.note
                break
        else:
            self.mode = MODE_OFF
            self.note = "静默模式全部不可用，退回普通模式（会弹窗）：" + "；".join(chain)

        self.probe_log("静默模式探测：" + " → ".join(chain))
        return self.mode

    # ---- 统计 ----
    def stats(self):
        if not self.calls:
            return {}
        durations = sorted(c[0] for c in self.calls)
        n = len(durations)

        def pct(p):
            return durations[min(n - 1, int(round((n - 1) * p)))]

        return {"runs": n,
                "total_s": round(sum(durations), 1),
                "p50_s": round(pct(0.5), 2),
                "p95_s": round(pct(0.95), 2),
                "max_s": round(durations[-1], 2),
                "nonzero_rc": sum(1 for c in self.calls if c[1] != 0)}


class Reporter(object):
    """stdout tee + report.md + results.json。

    回归跑在后台时，终端不是可靠载体：窗口被切走、控制台滚屏、会话断连都可能
    丢掉"跑到哪了/为什么失败"。这里把结论固化成文件，失败用例再单独存原始 stderr。
    """

    def __init__(self, outdir, title="C++ UI 桌面回归"):
        self.outdir = outdir
        self.title = title
        os.makedirs(outdir, exist_ok=True)
        self.logpath = os.path.join(outdir, "suite.log")
        self.errordir = os.path.join(outdir, "errors")
        self._f = open(self.logpath, "w", encoding="utf-8", buffering=1)
        self.started = time.time()
        self.meta = {}
        self.cases = []
        self.skips = []
        self._finished = False
        self._lock = threading.Lock()

    # file-like（给 Tee 用）
    def write(self, s):
        if self._finished:
            return
        with self._lock:
            self._f.write(s)

    def flush(self):
        if self._finished:
            return
        with self._lock:
            self._f.flush()

    def case(self, ok, name, detail=""):
        self.cases.append({"ok": bool(ok), "name": name, "detail": detail})

    def skip(self, name):
        self.skips.append(name)

    def dump_error(self, name, proc):
        """失败用例的完整 stdout/stderr 落盘，便于事后复盘。"""
        if proc is None:
            return
        os.makedirs(self.errordir, exist_ok=True)
        safe = re.sub(r"[^\w\u4e00-\u9fff.-]+", "_", name)[:80]
        path = os.path.join(self.errordir, safe + ".log")
        try:
            argv = proc.args if isinstance(proc.args, str) else " ".join(proc.args)
        except Exception:
            argv = str(proc.args)
        with open(path, "w", encoding="utf-8") as f:
            f.write("cmd: %s\nrc: %s\n\n===== stdout =====\n%s\n===== stderr =====\n%s\n"
                    % (argv, proc.returncode, proc.stdout or "", proc.stderr or ""))

    def finish(self, passes, failures, extra=None):
        """写 report.md / results.json；重复调用无副作用（Ctrl+C 与正常收尾都走这里）。"""
        if self._finished:
            return None
        self._finished = True
        duration = time.time() - self.started
        failures = list(failures)
        result = {"title": self.title,
                  "started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(self.started)),
                  "duration_s": round(duration, 1),
                  "pass": passes, "fail": len(failures), "skip": len(self.skips),
                  "failures": failures, "skips": self.skips,
                  "meta": dict(self.meta, **(extra or {})),
                  "cases": self.cases}
        with open(os.path.join(self.outdir, "results.json"), "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)

        lines = ["# %s（%s）" % (self.title, result["started"]), ""]
        for k, v in result["meta"].items():
            lines.append("- %s：%s" % (k, v))
        lines += ["- 耗时：%.1fs" % duration,
                  "",
                  "## 汇总",
                  "",
                  "| PASS | FAIL | SKIP |",
                  "|---|---|---|",
                  "| %d | %d | %d |" % (passes, len(failures), len(self.skips)),
                  ""]
        if failures:
            lines += ["## 失败项", ""]
            lines += ["- " + f for f in failures]
            lines += ["", "> 每个失败用例的完整输出在 `errors/` 下。", ""]
        if self.skips:
            lines += ["## 跳过项（环境/网络不满足，不计失败）", ""]
            lines += ["- " + s for s in self.skips]
            lines += [""]
        with open(os.path.join(self.outdir, "report.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        self._f.close()
        return result


class Tee(object):
    """同时写控制台与 Reporter（控制台编码不可靠时不让它中断回归）。"""

    def __init__(self, console, reporter):
        self.console = console
        self.reporter = reporter

    def write(self, s):
        try:
            self.console.write(s)
        except Exception:
            try:
                self.console.write(s.encode("utf-8", "replace").decode("utf-8", "replace"))
            except Exception:
                pass
        self.reporter.write(s)

    def flush(self):
        try:
            self.console.flush()
        except Exception:
            pass
        self.reporter.flush()

    def isatty(self):
        try:
            return self.console.isatty()
        except Exception:
            return False


# ---------------------------------------------------------------------------
# 托盘图标（可选）：让"后台静默跑"这件事可见，但不占屏幕、不抢焦点
# ---------------------------------------------------------------------------
class _GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD),
                ("Data4", ctypes.c_byte * 8)]


class _NOTIFYICONDATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("hWnd", wt.HWND),
                ("uID", wt.UINT),
                ("uFlags", wt.UINT),
                ("uCallbackMessage", wt.UINT),
                ("hIcon", wt.HICON),
                ("szTip", wt.WCHAR * 128),
                ("dwState", wt.DWORD),
                ("dwStateMask", wt.DWORD),
                ("szInfo", wt.WCHAR * 256),
                ("uVersion", wt.UINT),
                ("szInfoTitle", wt.WCHAR * 64),
                ("dwInfoFlags", wt.DWORD),
                ("guidItem", _GUID),
                ("hBalloonIcon", wt.HICON)]


class _WNDCLASS(ctypes.Structure):
    _fields_ = [("style", wt.UINT),
                ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]


class Tray(object):
    """任务栏通知区图标：进度提示 + 完成气泡 + 双击打开结果目录。

    任何一步失败都只记一条日志（`self.error` 里写明原因），不影响回归本身——
    CI、无 Explorer 的会话、被安全软件拦掉的场景都很常见。
    """

    WM_DESTROY = 0x0002
    WM_CLOSE = 0x0010
    WM_LBUTTONUP = 0x0202
    WM_LBUTTONDBLCLK = 0x0203
    NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
    NIF_MESSAGE, NIF_ICON, NIF_TIP, NIF_INFO = 0x01, 0x02, 0x04, 0x10
    NIIF_INFO = 0x01
    CALLBACK_MSG = 0x8000 + 0x21

    def __init__(self, tip, on_activate=None):
        self.tip = tip[:127]
        self.on_activate = on_activate
        self.ok = False
        self.error = ""
        self.hwnd = None
        self._uid = 0x4A4D
        self._thread = None
        self._ready = threading.Event()
        self._user32 = None
        self._shell32 = None
        self._nid = None

    # ---- 生命周期 ----
    def start(self):
        if not IS_WIN:
            self.error = "非 Windows 平台"
            return False
        try:
            self._bind()
            self._thread = threading.Thread(target=self._pump, name="jm-tray", daemon=True)
            self._thread.start()
            self._ready.wait(3.0)
        except Exception as exc:                      # noqa: BLE001
            self.error = "%s: %s" % (type(exc).__name__, exc)
            self.ok = False
        if not self.ok and not self.error:
            self.error = "Shell_NotifyIcon 失败或窗口创建失败"
        return self.ok

    def _bind(self):
        """显式声明原型。

        ctypes 在没有 argtypes 时会把 str 当成 int 传，CreateWindowExW 会直接抛
        TypeError——这正是"托盘永远不可用"的第一个坑，所以这里逐个声明。
        """
        u = ctypes.WinDLL("user32", use_last_error=True)
        s = ctypes.WinDLL("shell32", use_last_error=True)
        k = ctypes.WinDLL("kernel32", use_last_error=True)

        u.RegisterClassW.argtypes = [ctypes.POINTER(_WNDCLASS)]
        u.RegisterClassW.restype = wt.WORD
        u.UnregisterClassW.argtypes = [wt.LPCWSTR, wt.HINSTANCE]
        u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                      ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
        u.CreateWindowExW.restype = wt.HWND
        u.DestroyWindow.argtypes = [wt.HWND]
        u.PostQuitMessage.argtypes = [ctypes.c_int]
        u.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        u.DefWindowProcW.restype = ctypes.c_ssize_t
        u.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
        u.GetMessageW.restype = ctypes.c_int
        u.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
        u.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
        u.DispatchMessageW.restype = ctypes.c_ssize_t
        u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        u.LoadIconW.argtypes = [wt.HINSTANCE, ctypes.c_void_p]
        u.LoadIconW.restype = wt.HICON
        k.GetModuleHandleW.argtypes = [wt.LPCWSTR]
        k.GetModuleHandleW.restype = wt.HINSTANCE
        s.Shell_NotifyIconW.argtypes = [wt.DWORD, ctypes.POINTER(_NOTIFYICONDATA)]
        s.Shell_NotifyIconW.restype = wt.BOOL

        self._user32, self._shell32, self._k32 = u, s, k

    def _pump(self):
        WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
        try:
            u = self._user32
            self._wndproc = WNDPROC(self._on_message)
            cls = _WNDCLASS()
            cls.lpfnWndProc = ctypes.cast(self._wndproc, ctypes.c_void_p)
            cls.hInstance = self._k32.GetModuleHandleW(None)
            cls.lpszClassName = "JMComicUiSuiteTray"
            cls.hIcon = u.LoadIconW(None, ctypes.c_void_p(32512))    # IDI_APPLICATION
            if not u.RegisterClassW(ctypes.byref(cls)) and ctypes.get_last_error() != 1410:
                self.error = "RegisterClassW 失败 err=%d" % ctypes.get_last_error()
                return
            self.hwnd = u.CreateWindowExW(0, cls.lpszClassName, "jm-suite-tray", 0,
                                          0, 0, 0, 0, None, None, cls.hInstance, None)
            if not self.hwnd:
                self.error = "CreateWindowExW 失败 err=%d" % ctypes.get_last_error()
                return
            if not self._add_icon():
                return
            self.ok = True
            self._ready.set()

            msg = wt.MSG()
            while u.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
                u.TranslateMessage(ctypes.byref(msg))
                u.DispatchMessageW(ctypes.byref(msg))
        except Exception as exc:                      # noqa: BLE001
            self.error = "%s: %s" % (type(exc).__name__, exc)
            self.ok = False
        finally:
            self._ready.set()

    def _nid_base(self, flags):
        nid = _NOTIFYICONDATA()
        nid.cbSize = ctypes.sizeof(_NOTIFYICONDATA)
        nid.hWnd = self.hwnd
        nid.uID = self._uid
        nid.uFlags = flags
        nid.uCallbackMessage = self.CALLBACK_MSG
        nid.hIcon = self._user32.LoadIconW(None, ctypes.c_void_p(32512))
        nid.szTip = self.tip
        return nid

    def _add_icon(self):
        self._nid = self._nid_base(self.NIF_MESSAGE | self.NIF_ICON | self.NIF_TIP)
        if not self._shell32.Shell_NotifyIconW(self.NIM_ADD, ctypes.byref(self._nid)):
            self.error = "Shell_NotifyIcon(NIM_ADD) 失败 err=%d（Explorer 未运行？）" \
                         % ctypes.get_last_error()
            return False
        return True

    def _on_message(self, hwnd, msg, wparam, lparam):
        u = self._user32
        if msg == self.CALLBACK_MSG:
            if lparam in (self.WM_LBUTTONUP, self.WM_LBUTTONDBLCLK) and self.on_activate:
                try:
                    self.on_activate()
                except Exception:
                    pass
            return 0
        if msg == self.WM_CLOSE:
            u.DestroyWindow(hwnd)
            return 0
        if msg == self.WM_DESTROY:
            u.PostQuitMessage(0)
            return 0
        return u.DefWindowProcW(hwnd, msg, wparam, lparam)

    # ---- 更新 ----
    def set(self, text):
        if not self.ok:
            return
        try:
            nid = self._nid_base(self.NIF_TIP)
            nid.szTip = text[:127]
            self._shell32.Shell_NotifyIconW(self.NIM_MODIFY, ctypes.byref(nid))
        except Exception:
            pass

    def notify(self, title, text):
        if not self.ok:
            return
        try:
            nid = self._nid_base(self.NIF_INFO)
            nid.szInfoTitle = title[:63]
            nid.szInfo = text[:255]
            nid.dwInfoFlags = self.NIIF_INFO
            self._shell32.Shell_NotifyIconW(self.NIM_MODIFY, ctypes.byref(nid))
        except Exception:
            pass

    def stop(self):
        if not self.ok or not self.hwnd:
            return
        try:
            self._shell32.Shell_NotifyIconW(self.NIM_DELETE, ctypes.byref(self._nid))
            self._user32.PostMessageW(self.hwnd, self.WM_CLOSE, 0, 0)
        except Exception:
            pass
        self.ok = False


