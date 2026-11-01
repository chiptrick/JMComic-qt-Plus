# coding:utf-8
"""16KB 页对齐门禁"放行"留痕：写结构化审计记录

为什么需要它
------------------------------------------------------------------
`build_android.sh` 的 16KB 门禁失败时，只要 `ALLOW_UNALIGNED=1` 就只打印一行
warn 然后继续打包。0.87 就是这样带着 15 个未对齐库发出去的 —— 事后既查不到
是谁放的、哪台机器、哪个 commit，也查不到"为什么放行"（原因只存在于当时那个人
的脑子里）。所以放行必须留痕。

写两份（内容完全一致）：
  1. `<build_logs>/unaligned_override.json` —— 就地留档，构建现场一眼可见。
     注意 `android/build_logs/` 已被 .gitignore 忽略，清理缓存就没了。
  2. `<repo>/docs/build_audit/unaligned_overrides.jsonl` —— **追加**进仓库内可
     跟踪的文件，一行一条 JSON。它不会被"清缓存"清掉，也不会被下一个人覆盖。

字段：time / operator / host / git_sha / abi / apk / reason / unaligned
（`unaligned` 是未对齐库名列表，形如 `lib/arm64-v8a/libQt6Core_arm64-v8a.so`）

用法（通常由 build_android.sh 调用，也可手工补录）：
    python3 tools/record_unaligned_override.py --abi arm64-v8a \
        --apk dist/JMComic-0.88-arm64-v8a-debug.apk \
        --align-json .work/arm64-v8a/elf_page_align.json \
        --reason "等升级 libsr_qnn 依赖的 openssl"

本脚本**只负责记录**：任何一步失败都只打印原因，退出码恒为 0 —— 审计写不进去
不应该把一次已经决定放行的构建变成失败。
"""
import argparse
import json
import os
import socket
import subprocess
import sys
from datetime import datetime

DEFAULT_REASON = "unspecified"
LOG_NAME = "unaligned_override.json"
AUDIT_DIR = os.path.join("docs", "build_audit")
AUDIT_NAME = "unaligned_overrides.jsonl"


def NowIso():
    """本地时间的 ISO8601 表示（带时区偏移，便于跨时区事故对齐）"""
    try:
        return datetime.now().astimezone().isoformat(timespec="seconds")
    except Exception:
        return datetime.now().isoformat(timespec="seconds")


def Operator():
    """尽力取"谁放的"：WSL/容器里 root 很常见，取不到就写 unknown"""
    for name in ("SUDO_USER", "USER", "LOGNAME", "USERNAME"):
        value = (os.environ.get(name) or "").strip()
        if value:
            return value
    try:
        import getpass
        return getpass.getuser() or "unknown"
    except Exception:
        return "unknown"


def Host():
    try:
        return socket.gethostname() or "unknown"
    except Exception:
        return "unknown"


def GitSha(repo):
    """仓库内的短 commit；不是 git 仓库（比如拷到别处的构建树）写 nogit"""
    try:
        out = subprocess.run(["git", "-C", repo, "rev-parse", "--short", "HEAD"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             timeout=15, check=False)
        sha = (out.stdout or b"").decode("utf-8", "replace").strip()
        if out.returncode == 0 and sha:
            return sha
    except Exception:
        pass
    return "nogit"


def LoadUnaligned(alignJson):
    """从 check_elf_page_align.py --json 的输出里取未对齐库

    返回 (names, detail)：names 是给人看/给 grep 用的路径列表，
    detail 保留 checker 的原始记录（p_align、不合规段数），排查时不用再跑一遍。
    """
    try:
        with open(alignJson, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return [], []
    rows = data.get("unaligned") or []
    names, detail = [], []
    for row in rows:
        if not isinstance(row, dict):
            continue
        name = str(row.get("name") or "")
        abi = str(row.get("abi") or "")
        if not name:
            continue
        # checker 的 name 只有 basename（见 IterZip），这里补成包内路径
        names.append(name if name.startswith("lib/") else "lib/{}/{}".format(abi, name))
        detail.append({
            "name": name,
            "abi": abi,
            "machine": row.get("machine"),
            "min_align": row.get("min_align"),
            "misaligned_segments": row.get("misaligned_segments"),
            "load_segments": row.get("load_segments"),
        })
    return names, detail


def WriteJson(path, data):
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def AppendJsonl(path, data):
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(data, ensure_ascii=False) + "\n")


def Main():
    parser = argparse.ArgumentParser(
        description="记录一次 16KB 页对齐门禁的放行（ALLOW_UNALIGNED=1）")
    parser.add_argument("--abi", required=True, help="放行的 ABI，如 arm64-v8a")
    parser.add_argument("--apk", required=True, help="产物路径（APK/AAB）")
    parser.add_argument("--align-json", default="",
                        help="check_elf_page_align.py --json 的输出文件（读不到就记空列表）")
    parser.add_argument("--reason", default="",
                        help="放行原因（通常来自 ALLOW_UNALIGNED_REASON）；空则写 unspecified")
    parser.add_argument("--repo", default="", help="仓库根目录（算 git_sha / 审计文件位置）")
    parser.add_argument("--build-logs", default="",
                        help="就地留档目录，默认 <repo>/android/build_logs")
    parser.add_argument("--audit-dir", default="",
                        help="仓库内追加审计的目录，默认 <repo>/docs/build_audit")
    parser.add_argument("--quiet", action="store_true", help="只写文件，不打印结论")
    args = parser.parse_args()

    repo = os.path.abspath(args.repo) if args.repo else os.getcwd()
    buildLogs = os.path.abspath(args.build_logs) if args.build_logs \
        else os.path.join(repo, "android", "build_logs")
    auditDir = os.path.abspath(args.audit_dir) if args.audit_dir \
        else os.path.join(repo, AUDIT_DIR)

    reason = (args.reason or "").strip() or DEFAULT_REASON
    unaligned, detail = LoadUnaligned(args.align_json) if args.align_json else ([], [])
    apk = args.apk or ""
    record = {
        # 时间放第一位：翻 jsonl 时按时间读，字段顺序一致才看得下去
        "time": NowIso(),
        "operator": Operator(),
        "host": Host(),
        "git_sha": GitSha(repo),
        "abi": args.abi,
        # 只留文件名：构建机路径各不相同，同名文件才是人真正要反查的标的
        "apk": os.path.basename(apk),
        "reason": reason,
        "unaligned": unaligned,
        "unaligned_detail": detail,
        "unaligned_count": len(unaligned),
    }

    written = []
    for path, writer in ((os.path.join(buildLogs, LOG_NAME), "json"),
                         (os.path.join(auditDir, AUDIT_NAME), "jsonl")):
        try:
            if writer == "json":
                WriteJson(path, record)
            else:
                AppendJsonl(path, record)
            written.append(path)
        except Exception as es:      # 审计失败不影响构建
            print("[warn] 写审计记录失败 {}: {}".format(path, es), file=sys.stderr)

    if not args.quiet:
        print("放行审计已记录: 未对齐 {} 个 / 原因={}".format(len(unaligned), reason))
        if unaligned:
            print("  未对齐库: {}".format(", ".join(unaligned)))
        for path in written:
            print("  -> {}".format(path))
    return 0


if __name__ == "__main__":
    sys.exit(Main())
