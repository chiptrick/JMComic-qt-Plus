#!/usr/bin/env python3
# coding:utf-8
"""生成桌面超分后端插件清单 sr_backend_manifest.json，并（可选）打出 tar.gz 分发包。

用法:
  python tools/gen_sr_backend_manifest.py \
      --ovep cpp/shell/ui/build/ovep \
      --out python/src/config/sr_backend_manifest.json \
      --pack-dir cpp/dist/sr_backend_packages \
      --base-url https://github.com/<owner>/<repo>/releases/download/sr-backends-v1/

说明：SR-Vulkan 着色器包已改回随安装包分发（不再作为可安装/删除的插件），本清单只保留
      OpenVINO 这类需要额外大体积 runtime 的二进制后端。
每个后端 = 一个 tar.gz（含目标目录里的全部文件，平铺在归档根）。
安装 = 下载 <base-url><id>.tar.gz -> 解压到 <bin>/<targetSubdir>。
files[] 是逐文件 sha256/size，用于解压后完整性校验。
"""
import argparse, hashlib, json, os, tarfile

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()

def collect(d):
    out = []
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            out.append({"name": name, "size": os.path.getsize(p), "sha256": sha256(p)})
    return out

def pack(d, files, tarpath):
    os.makedirs(os.path.dirname(tarpath), exist_ok=True)
    with tarfile.open(tarpath, "w:gz") as tf:
        for f in files:
            tf.add(os.path.join(d, f["name"]), arcname=f["name"])
    return os.path.getsize(tarpath), sha256(tarpath)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ovep", default="cpp/shell/ui/build/ovep")
    ap.add_argument("--out", default="python/src/config/sr_backend_manifest.json")
    ap.add_argument("--pack-dir", default="")
    ap.add_argument("--base-url", default="https://github.com/chiptrick/JMComic-qt-Plus/releases/download/sr-backends-v1/")
    a = ap.parse_args()

    backends = []
    if os.path.isdir(a.ovep):
        files = collect(a.ovep)
        bk = {
            "id": "openvino-ep",
            "name": "Intel OpenVINO 推理插件（NPU / GPU / CPU）",
            "desc": "ORT 的 OpenVINO 插件 EP 及相关 runtime（约 228MB）。Intel NPU/核显专用；"
                    "下载解压到 ovep/ 后，「推理后端」里的 Intel NPU / XPU 档才可用。",
            "kind": "binary",
            "platform": "win-x64",
            "targetSubdir": "ovep",
            "installedByDefault": False,
            "requiresBinary": True,
            "files": files,
        }
        if a.pack_dir:
            sz, sha = pack(a.ovep, files, os.path.join(a.pack_dir, "openvino-ep.tar.gz"))
            bk["archiveSize"] = sz
            bk["archiveSha256"] = sha
        backends.append(bk)

    man = {
        "base_url": a.base_url,
        "note": "桌面超分后端插件：每个后端一个 tar.gz，下载解压到 <bin>/<targetSubdir>。"
                "SR-Vulkan 着色器随安装包分发，不作为插件。",
        "backends": backends,
    }
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(man, f, ensure_ascii=False, indent=2, sort_keys=False)
    print("wrote", a.out)
    for b in backends:
        print("  backend", b["id"], "files=%d" % len(b["files"]),
              "archiveSize=%s" % b.get("archiveSize", "-"))

if __name__ == "__main__":
    main()
