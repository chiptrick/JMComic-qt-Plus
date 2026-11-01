# coding:utf-8
"""生成 build-info.json：把"这个 APK 是用什么工具链编出来的"落盘

为什么需要它
------------------------------------------------------------------
16KB 事故查不出原因的另一半：全仓库 grep 不到 build-info，拿到一个 APK 只能靠
人的记忆去猜它用的是哪个 NDK / JDK / Gradle / Qt。记忆必然过期，于是同一个问题
反复出现（0.87 带着 15 个未对齐库发出去，事后无法从产物反查任何东西）。

本脚本收集这些事实并写成一份 JSON，由 build_android.sh 在打包前生成、随 APK 打进
`assets/build-info.json`（**能直接 unzip 看到**，不用解 private.tar），运行期由
`src/config/build_info.py` 读出来展示在"设置 → 关于"里。

铁律
------------------------------------------------------------------
1. 任何一项读不到就写 `null`，**绝不让构建失败** —— 这是可观测性工具，不是门禁。
2. 不猜版本号：能从产物/环境读出来的才写，读不出来宁可是 null。

用法
------------------------------------------------------------------
    # 最常见：由构建脚本调用（覆盖值从当前构建环境带进来）
    python3 tools/gen_build_info.py --output build_logs/build-info.json \\
        --set abi=arm64-v8a --set build_type=debug --set app_version=0.88 \\
        --set jdk_version=17 --set gradle_version=9.8.2 --set ndk_version=30.0.16248370

    # 手工看当前仓库能收集到什么
    python3 tools/gen_build_info.py --output /tmp/build-info.json --print

键名固定（顺序即写入顺序）：git_sha / git_describe / git_dirty / build_time /
build_host / operator / app_version / abi / build_type / jdk_version /
gradle_version / agp_version / ndk_version / qt_version / p4a_version /
python_version / allow_unaligned / unaligned_reason
"""
import argparse
import glob
import json
import os
import platform
import re
import socket
import subprocess
import sys

# 写入顺序 = 人读顺序，别随意调
FIELDS = [
    "git_sha", "git_describe", "git_dirty", "build_time", "build_host", "operator",
    "app_version", "abi", "build_type", "jdk_version", "gradle_version", "agp_version",
    "ndk_version", "qt_version", "p4a_version", "python_version",
    "allow_unaligned", "unaligned_reason",
]

# 布尔型字段：命令行 --set 传 "1/true/yes" 时转成 JSON true
BOOL_FIELDS = {"git_dirty", "allow_unaligned"}


# ------------------------------------------------------------------ 基础工具
def Warn(message):
    """统一打到 stderr：stdout 留给 --print 的 JSON，管道下游好处理"""
    print("[build-info] {}".format(message), file=sys.stderr)


def NowIso():
    from datetime import datetime
    try:
        return datetime.now().astimezone().isoformat(timespec="seconds")
    except Exception:
        return datetime.now().isoformat(timespec="seconds")


def Run(cmd, cwd=""):
    """跑一条命令并返回 stdout（失败返回空串）。所有外部命令都必须走这里：
    统一吞掉异常/hang（timeout），保证这个脚本不会拖死构建。"""
    try:
        out = subprocess.run(cmd, cwd=cwd or None, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=20, check=False)
        if out.returncode == 0:
            return (out.stdout or b"").decode("utf-8", "replace").strip()
    except Exception as es:
        Warn("执行 {} 失败: {}".format(" ".join(cmd), es))
    return ""


def ReadText(path, limit=256 * 1024):
    """读文本文件前 limit 字节；读不到返回空串（大文件如 build.gradle 足够小）"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except Exception:
        return ""


def ToBool(value):
    if isinstance(value, bool):
        return value
    text = str(value or "").strip().lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off", ""):
        return False
    return None


# ------------------------------------------------------------------ 各字段来源
def CollectGit(repo):
    """git_sha / git_describe / git_dirty：不是仓库时全写 None（不写 nogit ——
    那是审计记录的口径，build-info 里"读不到"就是 null）"""
    inside = Run(["git", "-C", repo, "rev-parse", "--is-inside-work-tree"])
    if inside != "true":
        return None, None, None
    sha = Run(["git", "-C", repo, "rev-parse", "--short", "HEAD"]) or None
    if sha is None:
        # 有 .git 但没有 commit（新仓库）时其余项也没有意义
        return None, None, None
    describe = Run(["git", "-C", repo, "describe", "--tags", "--always", "--dirty"]) or None
    # 工作区脏（含未跟踪文件）：构建产物与仓库内容对不上时，这一条是唯一线索
    dirty = bool(Run(["git", "-C", repo, "status", "--porcelain"]))
    return sha, describe, dirty


def CollectJdk():
    """优先级：JM_JDK_VER(构建脚本选中的主版本) > JAVA_HOME/release > java -version"""
    for name in ("JM_JDK_VER",):
        value = (os.environ.get(name) or "").strip()
        if value:
            return value
    for home in (os.environ.get("JAVA_HOME") or "", os.environ.get("JM_JDK") or ""):
        if not home:
            continue
        release = ReadText(os.path.join(home, "release"), 8192)
        found = re.search(r'^JAVA_VERSION="?([^"\n]+)"?', release, re.M)
        if found:
            return found.group(1).strip()
    output = Run(["java", "-version"])
    found = re.search(r'version "([^"]+)"', output)
    if found:
        return found.group(1).strip()
    return None


def CollectAgp(root, repo):
    """AGP 版本不写在仓库里，只在 p4a 生成的 dist 的 build.gradle 里。

    读不到就 None —— 构建早期（还没生成 dist）调用本脚本时属正常情况。
    """
    patterns = [
        os.path.join(root, ".buildozer", "android", "platform", "build-*", "dists", "*", "build.gradle"),
        os.path.join(root, ".buildozer", "android", "platform", "build-*", "dists", "*", "build.gradle.kts"),
        os.path.join(root, ".work", "**", "build.gradle"),
    ]
    candidates = []
    for pattern in patterns:
        candidates.extend(glob.glob(pattern, recursive=True))
    for path in sorted(candidates):
        text = ReadText(path)
        # p4a 模板把 AGP 写成插件版本(classpath 'com.android.tools.build:gradle:8.x.y')
        found = re.search(r"com\.android\.tools\.build:gradle:([0-9][0-9A-Za-z.\-]*)", text)
        if found:
            return found.group(1)
        found = re.search(r"com\.android\.application[\"']?\s*(?:version\s*)?[\"']([0-9][0-9A-Za-z.\-]*)", text)
        if found:
            return found.group(1)
    return None


def CollectNdk():
    """优先级：JM_NDK_VER > <NDK>/source.properties > $ANDROID_HOME/ndk 下最高版本"""
    value = (os.environ.get("JM_NDK_VER") or "").strip()
    if value:
        return value
    for home in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "JM_NDK"):
        path = (os.environ.get(home) or "").strip()
        if not path:
            continue
        props = ReadText(os.path.join(path, "source.properties"), 8192)
        found = re.search(r"^Pkg\.Revision\s*=\s*([0-9][0-9A-Za-z.\-]*)", props, re.M)
        if found:
            return found.group(1)
    sdk = (os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "").strip()
    if not sdk:
        return None
    versions = []
    try:
        for name in os.listdir(os.path.join(sdk, "ndk")):
            if os.path.isdir(os.path.join(sdk, "ndk", name)):
                versions.append(name)
    except Exception:
        return None
    if not versions:
        return None
    # 目录名就是版本号；按数值段排序取最高（与 tools/resolve_ndk.sh 同口径）
    def _key(name):
        parts = []
        for item in re.split(r"[.\-_]", name):
            parts.append(int(item) if item.isdigit() else 0)
        return parts
    return sorted(versions, key=_key)[-1]


def VersionFromWheel(path):
    """从 wheel 文件名取版本：pyside6-6.12.0-6.12.0-cp310-abi3-android_aarch64.whl"""
    name = os.path.basename(path or "")
    found = re.match(r"[A-Za-z0-9_.]+?-(\d[^-]*)-", name)
    if found:
        return found.group(1)
    return None


def CollectQt(root, qtWheel=""):
    """优先级：JM_QT_VERSION > 显式给的 wheel > android/wheels 里的 wheel > import PySide6

    wheel 优先于 import：构建机 venv 里装的 PySide6 是**宿主**版本（旧链路是 6.11.2），
    与实际打进 APK 的 wheel 不是一回事，用 import 会写出误导性的版本。
    """
    value = (os.environ.get("JM_QT_VERSION") or "").strip()
    if value:
        return value
    if qtWheel:
        found = VersionFromWheel(qtWheel)
        if found:
            return found
    for path in sorted(glob.glob(os.path.join(root, "wheels", "pyside6-*.whl"))):
        found = VersionFromWheel(path)
        if found:
            return found
    try:
        import PySide6
        return getattr(PySide6, "__version__", None)
    except Exception:
        return None


def CollectP4a(root):
    """p4a 版本：JM_P4A_VERSION > 受管源码树的 __init__.py > import pythonforandroid"""
    value = (os.environ.get("JM_P4A_VERSION") or "").strip()
    if value:
        return value
    for tree in ((os.environ.get("JM_P4A_SRC") or "").strip(),
                 os.path.join(root, ".p4a-src")):
        if not tree:
            continue
        init = ReadText(os.path.join(tree, "pythonforandroid", "__init__.py"), 16384)
        found = re.search(r"__version__\s*=\s*[\"']([^\"']+)[\"']", init)
        if found:
            return found.group(1)
    try:
        import pythonforandroid
        return getattr(pythonforandroid, "__version__", None)
    except Exception:
        return None


def CollectPython(root):
    """APK 里的 Python 版本才是用户关心的那个。

    优先看 p4a 的源码包名（android/wheels/Python-3.14.8.tgz），
    退而用当前解释器版本（宿主版本，至少能说明是谁生成的）。
    """
    for path in sorted(glob.glob(os.path.join(root, "wheels", "Python-*.tgz"))):
        found = re.match(r"Python-(\d+\.\d+\.\d+)\.tgz", os.path.basename(path))
        if found:
            return found.group(1)
    return platform.python_version()


def CollectAppVersion(root):
    """从 tools/patch_buildozer_spec.py 的 AppVersion 取默认值（与构建脚本同源）"""
    text = ReadText(os.path.join(root, "tools", "patch_buildozer_spec.py"))
    found = re.search(r'^AppVersion\s*=\s*"([^"]+)"', text, re.M)
    if found:
        return found.group(1).strip()
    return None


# ------------------------------------------------------------------ 主流程
def ParseSets(pairs):
    """--set key=value → dict（键名必须属于 FIELDS，写错立刻报出来）"""
    result = {}
    for item in pairs or []:
        if "=" not in item:
            raise argparse.ArgumentTypeError("--set 需要 key=value 形式: {}".format(item))
        key, value = item.split("=", 1)
        key = key.strip()
        if key not in FIELDS:
            raise argparse.ArgumentTypeError(
                "--set 不认识的键: {}(可用: {})".format(key, ", ".join(FIELDS)))
        result[key] = value.strip()
    return result


def Build(repo, sets, qtWheel, hostPython):
    """收集全部字段；单字段失败只影响该字段"""
    root = os.path.join(repo, "android")
    info = dict.fromkeys(FIELDS, None)

    collectors = [
        ("git", lambda: CollectGit(repo)),
        ("app_version", lambda: CollectAppVersion(root)),
        ("jdk_version", CollectJdk),
        ("gradle_version", lambda: (os.environ.get("JM_GRADLE_VERSION") or "").strip() or None),
        ("agp_version", lambda: CollectAgp(root, repo)),
        ("ndk_version", CollectNdk),
        ("qt_version", lambda: CollectQt(root, qtWheel)),
        ("p4a_version", lambda: CollectP4a(root)),
        ("python_version", lambda: hostPython or CollectPython(root)),
    ]
    for name, func in collectors:
        try:
            value = func()
        except Exception as es:
            Warn("收集 {} 失败(写 null): {}".format(name, es))
            value = None
        if name == "git":
            info["git_sha"], info["git_describe"], info["git_dirty"] = value
        else:
            info[name] = value

    info["build_time"] = NowIso()
    try:
        info["build_host"] = socket.gethostname() or None
    except Exception:
        info["build_host"] = None
    info["operator"] = (os.environ.get("USER") or os.environ.get("LOGNAME")
                        or os.environ.get("USERNAME") or "").strip() or None

    # 兜底：全部来自环境变量（构建脚本没显式 --set 时也能拿到）
    envDefaults = {
        "allow_unaligned": ToBool(os.environ.get("ALLOW_UNALIGNED")),
        "unaligned_reason": (os.environ.get("ALLOW_UNALIGNED_REASON") or "").strip() or None,
    }
    for key, value in envDefaults.items():
        if info.get(key) is None:
            info[key] = value

    # 显式覆盖最后生效（构建脚本是权威来源）
    for key, value in sets.items():
        if key in BOOL_FIELDS:
            info[key] = ToBool(value)
        else:
            info[key] = value or None

    # unaligned_reason 跟着 allow_unaligned 走：没放行就不该有"放行原因"
    if info.get("allow_unaligned") is not True:
        info["unaligned_reason"] = None

    # 固定顺序输出（json 本身无序，但 diff / 人读都需要稳定）
    return {key: info.get(key) for key in FIELDS}


def WriteOutput(path, info):
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    # 原子写：构建中途被打断不会留下半截 json（运行期读到半截会 parse 失败）
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def Main():
    parser = argparse.ArgumentParser(description="生成 build-info.json（工具链落盘，随 APK 分发）")
    parser.add_argument("--output", default="", help="输出路径（默认 <repo>/android/build_logs/build-info.json）")
    parser.add_argument("--repo", default="", help="仓库根目录（默认本脚本往上两级）")
    parser.add_argument("--set", dest="sets", action="append", default=[], metavar="KEY=VALUE",
                        help="覆盖单个字段，可重复；键名见脚本头部说明")
    parser.add_argument("--qt-wheel", default="",
                        help="实际打进包的 PySide6 android wheel 路径（从文件名取 Qt 版本）")
    parser.add_argument("--python-version", default="",
                        help="APK 内 Python 版本覆盖值（宿主解释器不是 APK 的那一个）")
    parser.add_argument("--print", dest="printJson", action="store_true",
                        help="把生成的 JSON 也打到 stdout")
    args = parser.parse_args()

    if args.repo:
        repo = os.path.abspath(args.repo)
    else:
        # 本文件在 <repo>/android/tools/ 下：往上两级是 android/，再往上一级才是仓库根
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    output = args.output or os.path.join(repo, "android", "build_logs", "build-info.json")

    try:
        sets = ParseSets(args.sets)
    except argparse.ArgumentTypeError as es:
        Warn(str(es))
        return 2

    try:
        info = Build(repo, sets, args.qt_wheel, args.python_version.strip() or "")
    except Exception as es:
        # 兜底：连收集都整体炸了就写全 null 的骨架，绝不阻断构建
        Warn("收集构建信息失败，写空骨架: {}".format(es))
        info = dict.fromkeys(FIELDS, None)

    try:
        WriteOutput(output, info)
        Warn("已写入 {}".format(output))
    except Exception as es:
        Warn("写 {} 失败(不影响构建): {}".format(output, es))
        if args.printJson:
            print(json.dumps(info, ensure_ascii=False, indent=2))
        return 0

    if args.printJson:
        print(json.dumps(info, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(Main())
