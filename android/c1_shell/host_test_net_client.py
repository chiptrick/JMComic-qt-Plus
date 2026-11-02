#!/usr/bin/env python3
# coding:utf-8
"""C1-P4 协议级回归：不需要 Qt，直接用本地 HTTP 服务验证 bridge.py 的行协议。

为什么单独有这一层（与 host_test_python_bridge.cpp 并存而不是二选一）：
  * C++ 那份验的是 PythonBridge 的进程管理语义（起停/超时/崩溃重启）；
  * 这份验的是**协议与请求层**语义（方法分发/错误码/重试次数/响应裁剪），
    在 CI 里不必装 Qt 也能跑，把"改协议不跑 C++ 测试"的口子堵住。

用法：python android/c1_shell/host_test_net_client.py
退出码 0 = 全 PASS。
"""

import http.server
import json
import os
import socket
import ssl
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "bridge.py")
sys.path.insert(0, HERE)

import net_client  # noqa: E402  (要先插 sys.path 才能 import)

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    print("[%s] %s%s" % ("PASS" if cond else "FAIL", name, (" — " + detail) if detail and not cond else ""))
    return bool(cond)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):  # 静音
        pass

    def do_GET(self):
        if self.path == "/ok":
            body = b"hello-c1"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/big":
            body = b"x" * 5000
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = b"not found"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length)
        body = json.dumps({"echo": raw.decode("utf-8", "replace")}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class Bridge(object):
    def __init__(self, prog):
        self.proc = subprocess.Popen(
            [sys.executable, BRIDGE],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=prog,
            text=True,
            bufsize=1,
        )
        self.next_id = 1

    def call(self, method, params=None, timeout=20):
        req = {"id": self.next_id, "method": method, "params": params or {}}
        self.next_id += 1
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            return {"ok": False, "error": {"code": "NoResponse", "message": "eof"}}
        return json.loads(line)


def main():
    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = "http://127.0.0.1:%d" % port

    bridge = Bridge(HERE)
    try:
        # 0) 纯函数：错误分类必须是确定性的，不依赖本机网络环境
        check("_classify(ConnectionRefusedError) -> ConnectErr",
              net_client._classify(ConnectionRefusedError(10061, "refused")) == "ConnectErr")
        check("_classify(gaierror) -> DnsError",
              net_client._classify(socket.gaierror(-2, "name")) == "DnsError")
        check("_classify(SSLError) -> SSLErr",
              net_client._classify(ssl.SSLError("bad")) == "SSLErr")
        check("_classify(TimeoutError) -> TimeOut",
              net_client._classify(TimeoutError("t")) == "TimeOut")

        res = bridge.call("ping")
        check("ping ok", res.get("ok") is True and res.get("result") == "pong", str(res))
        check("ping 回带 method", res.get("method") == "ping", str(res))
        check("ping 回带 protocol=1", res.get("protocol") == 1, str(res))

        res = bridge.call("echo", {"a": 1})
        check("echo 原样返回", res.get("ok") and res.get("result") == {"a": 1}, str(res))

        res = bridge.call("net.status")
        result = res.get("result") or {}
        check("net.status backend=stdlib", result.get("backend") == "stdlib", str(res))
        check("net.status 有 rss_kb", isinstance(result.get("rss_kb"), int), str(res))

        res = bridge.call("net.request", {"url": base + "/ok"})
        result = res.get("result") or {}
        check("net.request 200", res.get("ok") and result.get("http_status") == 200, str(res))
        check("net.request body", result.get("body") == "hello-c1", str(res))
        check("net.request attempts=1", result.get("attempts") == 1, str(res))

        res = bridge.call("net.request", {"method": "POST", "url": base + "/echo",
                                          "body": {"k": "v"}})
        result = res.get("result") or {}
        echoed = ""
        try:
            echoed = (json.loads(result.get("body") or "{}") or {}).get("echo", "")
        except Exception:
            echoed = ""
        check("net.request POST 回显", res.get("ok") and echoed == '{"k": "v"}', str(res))

        res = bridge.call("net.request", {"url": base + "/missing"})
        result = res.get("result") or {}
        check("404 视为拿到响应而非错误",
              res.get("ok") and result.get("http_status") == 404 and result.get("http_error") is True,
              str(res))

        res = bridge.call("net.request", {"url": base + "/big", "max_bytes": 128})
        result = res.get("result") or {}
        check("max_bytes 截断", result.get("body_len") == 128 and result.get("truncated") is True, str(res))

        res = bridge.call("net.request", {"url": base + "/big", "include_body": False})
        result = res.get("result") or {}
        check("include_body=False 不带体", "body" not in result and result.get("body_len") == 5000, str(res))

        res = bridge.call("net.request", {"url": "ftp://x/y"})
        err = res.get("error") or {}
        check("非法 scheme -> UserError", err.get("code") == "UserError", str(res))

        closed_port = free_port()
        res = bridge.call("net.request", {"url": "http://127.0.0.1:%d/x" % closed_port,
                                          "timeout_ms": 800, "retries": 1})
        err = res.get("error") or {}
        # 本机（Windows + 安全软件）会把环回拒连静默丢包，导致表现成 TimeOut 而不是
        # ConnectErr；分类本身由上面的纯函数用例锁定，这里只要求"是传输层错误"，
        # 不把环境差异写成假绿也不写成假红。
        check("拒连 -> 传输层错误（非 UserError）",
              err.get("code") in ("ConnectErr", "TimeOut", "ResetErr"), str(res))
        check("retries=1 实际尝试 2 次", err.get("attempts") == 2, str(res))

        res = bridge.call("net.request", {"url": "http://10.255.255.1:9/x", "timeout_ms": 300,
                                          "retries": 0})
        err = res.get("error") or {}
        check("黑洞地址 -> TimeOut 或 ConnectErr",
              err.get("code") in ("TimeOut", "ConnectErr"), str(res))

        res = bridge.call("no.such.method")
        err = res.get("error") or {}
        check("未知方法 -> UserError", err.get("code") == "UserError", str(res))

        res = bridge.call("net.close")
        check("net.close 回响应", res.get("ok") and (res.get("result") or {}).get("closing") is True, str(res))
        code = bridge.proc.wait(timeout=10)
        check("net.close 后进程退出码 0", code == 0, "exit=%s" % code)
    finally:
        try:
            bridge.proc.kill()
        except Exception:
            pass
        server.shutdown()

    failed = [r for r in RESULTS if not r[1]]
    print("\n%d/%d passed" % (len(RESULTS) - len(failed), len(RESULTS)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
