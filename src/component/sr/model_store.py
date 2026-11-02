# coding:utf-8
""" sr 模型仓库（A3a）：外部目录 + 清单校验 + 按需下载

设计要点（对应用户拍板的三条）：
1. **零内置**：模型只从 GetAndroidModelDir() 指向的目录读
   （Android: /storage/emulated/0/Android/data/<pkg>/models，免权限、用户可手工投放）；
2. **GitHub Release 分发**：清单里的 base_url 指向 Release 目录，文件名与本地一致；
3. **断网**：只展示已装模型，未装的置灰（本模块只负责"能不能下"，UI 负责灰不灰）。

不做的事：不自动重试到天荒地老（只 3 次线性退避）、不并发下载（一次一个，避免抢带宽
与 QNN 初始化抢内存）、不信任清单之外的路径（file 必须是 basename，防 ../ 越界）。
"""
import hashlib
import json
import os
import time
import urllib.error
import urllib.request

DEFAULT_MANIFEST = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "config", "sr_models_manifest.json")

STATE_INSTALLED = "installed"
STATE_MISSING = "missing"
STATE_CORRUPT = "corrupt"


class ModelOffline(RuntimeError):
    """ 网络不可用/下载失败：UI 据此把入口置灰而不是弹一堆错误 """


class ModelStore(object):
    def __init__(self, root=None, manifest_path=None, timeout=10):
        self.timeout = timeout
        self.manifest_path = manifest_path or DEFAULT_MANIFEST
        self.root = root or self._DefaultRoot()
        self._doc = None
        self._online = None

    # ---------- 基础 ----------
    @staticmethod
    def _DefaultRoot():
        try:
            from tools import platform_mobile
            return platform_mobile.GetAndroidModelDir()
        except Exception:
            return os.path.expanduser("~/.jmcomic/models")

    def Manifest(self):
        if self._doc is None:
            try:
                with open(self.manifest_path, "r", encoding="utf-8") as f:
                    self._doc = json.load(f)
            except (OSError, ValueError):
                self._doc = {"version": 0, "base_url": "", "models": [], "texts": []}
        return self._doc

    def BaseUrl(self):
        base = (self.Manifest().get("base_url") or "").strip()
        return (base.rstrip("/") + "/") if base else ""

    def CanDownload(self):
        """ 有下载源才允许"可下载"，否则 UI 一律置灰（清单/网络任一缺失都不下） """
        return bool(self.BaseUrl())

    def Path(self, entry):
        name = os.path.basename(entry.get("file") or "")
        if not name or name != (entry.get("file") or ""):
            raise ValueError("非法文件名: {!r}".format(entry.get("file")))
        return os.path.join(self.root, name)

    # ---------- 状态 ----------
    def IsInstalled(self, entry):
        p = self.Path(entry)
        try:
            if not os.path.isfile(p):
                return False
            want = entry.get("size")
            return (want is None) or os.path.getsize(p) == want
        except OSError:
            return False

    def Verify(self, entry):
        """ sha256 校验（只对已装文件有意义；大批量扫描时不要每次都调） """
        p = self.Path(entry)
        want = entry.get("sha256")
        if not os.path.isfile(p):
            return False
        if not want:
            return True
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest() == want

    def State(self, entry):
        p = self.Path(entry)
        if not os.path.isfile(p):
            return STATE_MISSING
        if not self.IsInstalled(entry):
            return STATE_CORRUPT
        return STATE_INSTALLED

    def List(self):
        """ 返回 [{'entry': 原清单项, 'state': ..., 'installed': bool}]，UI 直接消费 """
        out = []
        for e in self.Manifest().get("models", []):
            st = self.State(e)
            out.append({"entry": e, "state": st, "installed": st == STATE_INSTALLED})
        return out

    def ListByScale(self):
        """ 按放大倍数分组：{2: [...], 4: [...]}，UI 的"同倍数内回退"就靠它 """
        g = {}
        for item in self.List():
            g.setdefault(int(item["entry"].get("scale") or 0), []).append(item)
        return g

    # ---------- 网络 ----------
    def Probe(self, force=False):
        """ 快速探活：只发 HEAD，3 秒超时；无 base_url 直接判 offline """
        if self._online is not None and not force:
            return self._online
        if not self.CanDownload():
            self._online = False
            return False
        try:
            req = urllib.request.Request(self.BaseUrl(), method="HEAD")
            urllib.request.urlopen(req, timeout=3).close()
            self._online = True
        except Exception:
            self._online = False
        return self._online

    def Download(self, entry, progress=None, retries=3):
        """ 断点续传下载单模型 → 校验 → 原子改名。

        progress(done, total) 每 16KB 调一次；返回本地路径。
        失败一律抛 ModelOffline（UI 只处理这一种异常）。
        """
        url = self.BaseUrl() + os.path.basename(entry["file"])
        if not self.CanDownload():
            raise ModelOffline("未配置下载源（manifest.base_url 为空）")
        os.makedirs(self.root, exist_ok=True)
        final = self.Path(entry)
        part = final + ".part"
        total = int(entry.get("size") or 0)

        last_err = None
        for attempt in range(retries):
            try:
                return self._DownloadOnce(url, part, final, total, progress)
            except Exception as e:                    # noqa: BLE001 网络层错误种类太多
                last_err = e
                time.sleep(1 + attempt)                # 线性退避，不做指数爆炸
        raise ModelOffline("下载失败 {}: {}".format(os.path.basename(entry["file"]), last_err))

    def _DownloadOnce(self, url, part, final, total, progress):
        done = os.path.getsize(part) if os.path.isfile(part) else 0
        if total and done == total:
            os.replace(part, final)
            return final
        req = urllib.request.Request(url)
        if done:
            req.add_header("Range", "bytes={}-".format(done))
        try:
            resp = urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.HTTPError as e:
            if e.code == 416:                          # Range 越界：本地 .part 已完整
                os.replace(part, final)
                return final
            raise
        mode = "ab" if done and resp.status == 206 else "wb"
        if mode == "wb":
            done = 0
        with open(part, mode) as f:
            while True:
                chunk = resp.read(16384)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        resp.close()
        if total and os.path.getsize(part) != total:
            raise ModelOffline("大小不符: {} != {}".format(os.path.getsize(part), total))
        if not self.Verify(dict(entry, file=os.path.basename(part))):
            os.remove(part)
            raise ModelOffline("sha256 校验失败，已删除半包")
        os.replace(part, final)
        return final
