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

    def Providers(self):
        """ 按顺序尝试的下载源列表（providers 优先；老的 base_url 兼容） """
        doc = self.Manifest()
        urls = [u for u in (doc.get("providers") or []) if u]
        if not urls:
            legacy = (doc.get("base_url") or "").strip()
            urls = [legacy] if legacy else []
        return [(u.rstrip("/") + "/") for u in urls]

    def BaseUrl(self):
        """ 主源（第一个 provider）；没有就空串 """
        urls = self.Providers()
        return urls[0] if urls else ""

    def CanDownload(self):
        """ 有下载源才允许"可下载"，否则 UI 一律置灰（清单/网络任一缺失都不下）

        注意：官方仓库（nagadomi/waifu2x、xinntao/Real-ESRGAN、bilibili/ailab）**不算**下载源 ——
        它们发的是 .pth/整合包，不是我们清单里的固定 shape ONNX（见 manifest.sources）。
        """
        return bool(self.Providers())

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

    def Quantized(self):
        """ 量化版模型（manifest.quantized）：与 float 同名 + `_q16` 后缀

        引擎在 QNN EP 可用时按文件名约定自动优先它们（列表/UI 不变），
        所以这里只负责"能不能下载"，不参与 List()。
        """
        return self.Manifest().get("quantized", []) or []

    def QuantizedFor(self, base_file):
        """ 查某个 float 模型对应的量化版条目；没有返回 None """
        for item in self.Quantized():
            if item.get("base") == base_file:
                return item
        return None

    def DownloadQuantized(self, base_file, progress=None, retries=3):
        """ 下载某个模型的量化版（体积约为 float 的 27%） """
        entry = self.QuantizedFor(base_file)
        if entry is None:
            raise ModelOffline("清单里没有 {} 的量化版".format(base_file))
        return self.Download(entry, progress=progress, retries=retries)

    # ---------- Vulkan(ncnn) 模型 ----------
    def VkRoot(self):
        """ ncnn 模型放在 <模型目录>/vk/ 下（引擎按该目录里的 models_vk.txt 列模型） """
        return os.path.join(self.root, "vk")

    def Ncnn(self):
        return self.Manifest().get("ncnn", []) or []

    def NcnnFor(self, name):
        for item in self.Ncnn():
            if item.get("name") == name:
                return item
        return None

    def InstalledNcnn(self):
        vk = self.VkRoot()
        out = []
        for item in self.Ncnn():
            if os.path.isfile(os.path.join(vk, item["param"])) and \
                    os.path.isfile(os.path.join(vk, item["bin"])):
                out.append(item["name"])
        return out

    def SyncVkRegistry(self):
        """ 把已下载的 ncnn 模型写进 models_vk.txt（libsr_vk 读它来列/载模型） """
        vk = self.VkRoot()
        os.makedirs(vk, exist_ok=True)
        lines = ["# 由 ModelStore.SyncVkRegistry() 生成，勿手改"]
        for item in self.Ncnn():
            if item["name"] not in self.InstalledNcnn():
                continue
            lines.append("{name}|{param}|{bin}|{scale}|{tile}|{in_ch}".format(**item))
        path = os.path.join(vk, "models_vk.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return path

    def DownloadNcnn(self, name, progress=None):
        """ 下载一个 ncnn 模型（param+bin 一对）并刷新 models_vk.txt """
        item = self.NcnnFor(name)
        if item is None:
            raise ModelOffline("清单里没有 ncnn 模型: {}".format(name))
        vk = self.VkRoot()
        os.makedirs(vk, exist_ok=True)
        sub = ModelStore(root=vk, manifest_path=self.manifest_path, timeout=self.timeout)
        for key in ("param", "bin"):
            sub.Download({"file": item[key], "size": item.get(key + "_size"),
                          "sha256": item.get(key + "_sha256"),
                          "providers": item.get("providers")}, progress=progress)
        self.SyncVkRegistry()
        return os.path.join(vk, item["bin"])

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
        """ 快速探活：只发 HEAD，3 秒超时；无 base_url 直接判 offline

        file:// 源（本地调试/自测）没有 HEAD，按"路径存在即可用"处理。
        """
        if self._online is not None and not force:
            return self._online
        if not self.CanDownload():
            self._online = False
            return False
        # 探活不看目录（raw.githubusercontent 没有目录索引、jsDelivr 的目录页 403），
        # 而是 HEAD 清单里**最小的那个文件**——它一定存在，且响应能代表该源可用。
        probe_file = ""
        models = self.Manifest().get("models") or []
        if models:
            probe_file = os.path.basename(min(models, key=lambda m: m.get("size") or 0)["file"])
        for base in self.Providers():
            if base.startswith("file://"):
                if os.path.isdir(base[len("file://"):]):
                    self._online = True
                    return True
                continue
            url = base + probe_file if probe_file else base
            try:
                req = urllib.request.Request(url, method="HEAD")
                urllib.request.urlopen(req, timeout=5).close()
                self._online = True
                return True
            except Exception:
                continue
        self._online = False
        return False

    def Download(self, entry, progress=None, retries=3):
        """ 断点续传下载单模型 → 校验 → 原子改名。

        progress(done, total) 每 16KB 调一次；返回本地路径。
        失败一律抛 ModelOffline（UI 只处理这一种异常）。
        """
        # 条目可带自己的源：ncnn 文件位于 models_vk/，与 ONNX 的 models/ 不同。
        providers = [(u.rstrip("/") + "/") for u in (entry.get("providers") or []) if u]
        if not providers:
            providers = self.Providers()
        if not providers:
            raise ModelOffline("未配置下载源（manifest.providers 为空）")
        os.makedirs(self.root, exist_ok=True)
        final = self.Path(entry)
        part = final + ".part"
        total = int(entry.get("size") or 0)

        last_err = None
        for base in providers:                       # 多源：主源失败自动换下一个
            url = base + os.path.basename(entry["file"])
            for attempt in range(retries):
                try:
                    return self._DownloadOnce(url, part, final, total, progress)
                except urllib.error.HTTPError as e:
                    last_err = e
                    if 400 <= int(getattr(e, "code", 0) or 0) < 500:
                        break                        # 4xx 换源没意义地重试，直接试下一个源
                    time.sleep(1 + attempt)
                except Exception as e:                # noqa: BLE001 网络层错误种类太多
                    last_err = e
                    time.sleep(1 + attempt)            # 线性退避，不做指数爆炸
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
        mode = "ab" if done and getattr(resp, "status", None) == 206 else "wb"
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
