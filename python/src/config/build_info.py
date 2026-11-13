# coding:utf-8
""" 构建工具链信息（APK 内 assets/build-info.json 的读取端）

为什么要读这个文件
------------------------------------------------------------------
拿到一个 APK 时，"它到底是用哪个 NDK / JDK / Gradle / Qt 编的"以前只能靠人的记忆。
0.87 带着 15 个未对齐的库发出去，事后无法从产物反查任何工具链信息，就是这个缺口
造成的。现在 android/tools/gen_build_info.py 在打包前把这份信息写成 json 并作为
**独立条目**打进 assets/（unzip 就能看到，不用解 private.tar），这里负责读出来。

读取顺序（读不到就返回空 dict，**绝不抛异常**）：
    1. `assets:/build-info.json` —— Android 运行时（Qt 的 assets: 映射到 APK assets/）
    2. `<仓库>/android/build_logs/build-info.json` —— 桌面开发环境（构建脚本落的副本）
    3. 都没有 -> {}，界面显示"未找到"

对外只有两个函数：Get() 返回 dict，Summary() 返回可直接展示的多行文本。
"""
import json
import os

from PySide6.QtCore import QFile, QIODevice

# APK 内的落点：build_android.sh -> gen_build_info.py -> buildozer.spec 的 add_assets
AssetPath = "assets:/build-info.json"
# 桌面开发环境的副本（android/build_logs/ 被 .gitignore 忽略，只在构建机上有）
DevRelativePath = os.path.join("android", "build_logs", "build-info.json")

_cache = None          # 解析结果缓存：dict 或 {}（用 None 表示"还没读过"）


def _RepoRoot():
    """ 仓库根目录：本文件在 <repo>/src/config/ 下，往上两级 """
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(here))


def _LoadJson(text):
    """ 解析 JSON 文本；失败返回 None（内容损坏等同于没有，不该让界面报错） """
    try:
        data = json.loads(text)
    except Exception:
        return None
    return data if isinstance(data, dict) else None


def _ReadAssets():
    """ 从 APK 的 assets/ 读（Android）。QFile 不支持这个 scheme 时静默返回 None """
    try:
        f = QFile(AssetPath)
        if not f.exists():
            return None
        if not f.open(QIODevice.ReadOnly):
            return None
        try:
            raw = bytes(f.readAll())
        finally:
            f.close()
    except Exception:
        return None
    try:
        return _LoadJson(raw.decode("utf-8", "replace"))
    except Exception:
        return None


def _ReadDevCopy():
    """ 桌面开发环境：读构建脚本落在仓库里的副本 """
    path = os.path.join(_RepoRoot(), DevRelativePath)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return _LoadJson(f.read())
    except Exception:
        return None


def Get():
    """ 返回构建信息 dict；任何情况下都不抛异常，读不到返回 {} """
    global _cache
    if _cache is not None:
        return _cache
    data = _ReadAssets()
    if not data:
        data = _ReadDevCopy()
    _cache = data or {}
    return _cache


def ClearCache():
    """ 丢掉缓存（重新打包后热更新调试用；正常流程用不到） """
    global _cache
    _cache = None
    return


def _Text(data, key, default=""):
    value = data.get(key)
    if value is None:
        return default
    if isinstance(value, bool):
        return "是" if value else "否"
    return str(value)


def Summary():
    """ 给界面用的多行摘要：只列读到的字段，缺的整行不出现 """
    data = Get()
    if not data:
        return "未找到构建信息（assets/build-info.json）。\n" \
               "开发环境可在仓库里跑: python3 android/tools/gen_build_info.py"

    lines = []
    # 第一行：包是谁、什么架构/构建类型 —— 反馈问题时最先被问到的三件事
    version = _Text(data, "app_version", "-")
    abi = _Text(data, "abi", "-")
    buildType = _Text(data, "build_type", "-")
    lines.append("版本: {}  ({}, {})".format(version, abi, buildType))

    if data.get("build_time"):
        lines.append("构建时间: {}".format(_Text(data, "build_time")))
    host = _Text(data, "build_host")
    operator = _Text(data, "operator")
    if host or operator:
        lines.append("构建机: {} ({})".format(host or "-", operator or "-"))

    # 工具链：16KB 对齐/崩溃类问题基本都能在这三个版本号上找到线索
    toolchain = []
    for key, label in (("ndk_version", "NDK"), ("jdk_version", "JDK"),
                       ("gradle_version", "Gradle"), ("agp_version", "AGP")):
        value = _Text(data, key)
        if value:
            toolchain.append("{} {}".format(label, value))
    if toolchain:
        lines.append("工具链: " + " · ".join(toolchain))

    stack = []
    for key, label in (("qt_version", "Qt"), ("p4a_version", "p4a"),
                       ("python_version", "Python")):
        value = _Text(data, key)
        if value:
            stack.append("{} {}".format(label, value))
    if stack:
        lines.append("运行时: " + " · ".join(stack))

    gitSha = _Text(data, "git_sha")
    if gitSha:
        describe = _Text(data, "git_describe")
        dirty = data.get("git_dirty")
        suffix = ""
        if describe and describe != gitSha:
            suffix = " ({})".format(describe)
        if dirty is True:
            suffix += " · 工作区有改动"
        lines.append("提交: {}{}".format(gitSha, suffix))

    # 放行 16KB 门禁是一件需要长期可查的事，界面上也要能看到"这版是放行的"
    if data.get("allow_unaligned") is True:
        reason = _Text(data, "unaligned_reason", "unspecified")
        lines.append("16KB: 本版允许未对齐放行（原因: {}）".format(reason))
    return "\n".join(lines)
