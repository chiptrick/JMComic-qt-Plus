#!/usr/bin/env python3
# coding:utf-8
""" C1-W6 的最小 Python 子进程桥：每行一个 JSON 请求，回一行 JSON 响应。

用途：给 C++ 侧 ProcessBridge 做闭环自检（起停/请求/超时/崩溃重启），
不承担真实网络请求 —— 网络收编是 C3 的事，这里只保证"能被拉起来、能回话"。
"""
import json
import sys


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        method = req.get("method")
        params = req.get("params") or {}
        if method == "ping":
            res = {"ok": True, "result": "pong"}
        elif method == "echo":
            res = {"ok": True, "result": params}
        elif method == "crash":
            # 故意崩溃，用于验证 C++ 侧的崩溃检测与自动重启
            sys.stdout.flush()
            raise SystemExit(3)
        else:
            res = {"ok": False, "error": "unknown method"}
        sys.stdout.write(json.dumps({"id": req.get("id"), **res}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
