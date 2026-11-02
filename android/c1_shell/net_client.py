#!/usr/bin/env python3
# coding:utf-8
"""C1-P4：不依赖 Qt 的真实请求层，供 bridge.py 调用。

为什么单独抽一层（而不是直接 import src/server/server.py）：
  server.py 依赖 QtOwner / TaskBase / GlobalConfig / 单例信号总线，在 JSON 行桥的
  子进程里根本 import 不起来。P4 要的是"能真发一次请求"的最小闭环，不是把整个
  Server 搬过来。

与生产语义对齐的部分（便于用同一套词表读日志）：
  * 错误码直接复用 `src/tools/status.py` 的 Status 名字：DnsError / TimeOut /
    ConnectErr / ResetErr / SSLErr / NetError / ProxyError / UserError；
  * 重试语义与 `Server._Send` 一致：只在"可恢复"的传输层错误上重试，
    4xx/5xx 不重试（重试只会把同一个 404 再打一遍）；
  * 响应裁剪：超过 max_bytes 只读前 max_bytes，避免一张图把子进程内存打爆。

与生产的已知差异（明确写在这里，不假装等价）：
  * 后端只用 stdlib urllib，不做 curl_cffi 的 TLS 指纹伪装。指纹伪装属于 C3（网络收编）
    的范围；P4 的验收判据是"一次真实请求返回预期结果 + 异常可定位"，不是绕过风控。
  * 不继承环境变量里的 http_proxy/https_proxy：桥的行为必须可复现，
    代理只从请求参数显式给。
"""

import base64
import json
import os
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PROTOCOL_VERSION = 1
DEFAULT_TIMEOUT_MS = 10000
DEFAULT_MAX_BYTES = 256 * 1024

# 与 Status 同名（src/tools/status.py），方便同一套日志/报告口径
ST_OK = "Ok"
ST_DNS = "DnsError"
ST_TIMEOUT = "TimeOut"
ST_CONNECT = "ConnectErr"
ST_RESET = "ResetErr"
ST_SSL = "SSLErr"
ST_NET = "NetError"
ST_PROXY = "ProxyError"
ST_USER = "UserError"

# 只有这些状态值得换一次机会重发；4xx/5xx 不在此列
RETRYABLE = (ST_DNS, ST_TIMEOUT, ST_CONNECT, ST_RESET)


class RequestError(Exception):
    """带 Status 词表的异常：code 一定在 Status 名字集合里。"""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _elapsed_ms(start):
    return int(round((time.monotonic() - start) * 1000))


def _classify(exc):
    """把 Python 异常映射到 Status 词表。分类顺序有讲究：DNSError 在部分平台是
    socket.gaierror 的子类，必须先判；超时也要先于 ConnectionError。"""
    if isinstance(exc, socket.gaierror):
        return ST_DNS
    if isinstance(exc, (socket.timeout, TimeoutError)):
        return ST_TIMEOUT
    if isinstance(exc, ssl.SSLError):
        return ST_SSL
    if isinstance(exc, ConnectionResetError):
        return ST_RESET
    if isinstance(exc, ConnectionRefusedError):
        return ST_CONNECT
    if isinstance(exc, urllib.error.URLError):
        reason = getattr(exc, "reason", None)
        if isinstance(reason, socket.gaierror):
            return ST_DNS
        if isinstance(reason, (socket.timeout, TimeoutError)):
            return ST_TIMEOUT
        if isinstance(reason, ssl.SSLError):
            return ST_SSL
        if isinstance(reason, ConnectionResetError):
            return ST_RESET
        if isinstance(reason, ConnectionRefusedError):
            return ST_CONNECT
        return ST_CONNECT
    if isinstance(exc, OSError):
        return ST_NET
    return ST_NET


def _build_opener(proxy):
    # ProxyHandler({}) 显式清空：不让环境变量里的代理偷偷改变桥的行为
    if proxy:
        handler = urllib.request.ProxyHandler({"http": proxy, "https": proxy})
    else:
        handler = urllib.request.ProxyHandler({})
    return urllib.request.build_opener(handler)


def _once(url, method, headers, body, timeout_s, proxy, max_bytes, want_binary):
    opener = _build_opener(proxy)
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    started = time.monotonic()
    try:
        with opener.open(req, timeout=timeout_s) as resp:
            raw = resp.read(max_bytes)
            elapsed = _elapsed_ms(started)
            payload = {
                "status": ST_OK,
                "http_status": int(resp.getcode() or 0),
                "final_url": resp.geturl(),
                "headers": dict(resp.headers.items()) if resp.headers else {},
                "body_len": len(raw),
                "truncated": len(raw) >= max_bytes,
            }
            if want_binary:
                payload["body_b64"] = base64.b64encode(raw).decode("ascii")
            else:
                payload["body"] = raw.decode("utf-8", "replace")
            payload["elapsed_ms"] = elapsed
            return payload
    except urllib.error.HTTPError as exc:
        # HTTP 层错误也是"拿到了响应"：保留状态码与一小段响应体，便于定位 403/404
        raw = b""
        try:
            raw = exc.read(max_bytes)
        except Exception:
            pass
        return {
            "status": ST_OK,
            "http_status": int(exc.code or 0),
            "final_url": url,
            "headers": dict(exc.headers.items()) if exc.headers else {},
            "body_len": len(raw),
            "truncated": False,
            "body": "" if want_binary else raw.decode("utf-8", "replace"),
            "elapsed_ms": _elapsed_ms(started),
            "http_error": True,
        }
    except Exception as exc:  # noqa: BLE001 - 统一分类后抛回调用方
        raise RequestError(_classify(exc), "%s: %s" % (type(exc).__name__, exc))


def request(method="GET", url="", headers=None, body=None, timeout_ms=None,
            retries=1, max_bytes=None, include_body=True, binary=False,
            proxy=None):
    """发一次（可重试的）请求，返回结构化结果。

    返回 dict 的键：
      status          Status 词表里的字符串（成功恒为 Ok）
      http_status     HTTP 状态码（0 表示没拿到响应）
      final_url       跟随重定向后的 URL
      body / body_b64 响应体（include_body=False 时不带，避免桥里塞大对象）
      body_len        实际读到的字节数
      truncated       是否被 max_bytes 截断
      elapsed_ms      端到端耗时（含重试）
      attempts        实际尝试次数
      backend         后端标识（当前恒为 stdlib）
    """
    if not isinstance(url, str) or not url:
        raise RequestError(ST_USER, "url is required")
    scheme = urllib.parse.urlsplit(url).scheme.lower()
    if scheme not in ("http", "https"):
        raise RequestError(ST_USER, "unsupported scheme: %r" % scheme)

    method = (method or "GET").upper()
    if method not in ("GET", "HEAD", "POST", "PUT", "DELETE", "PATCH"):
        raise RequestError(ST_USER, "unsupported method: %r" % method)

    timeout_ms = int(timeout_ms or DEFAULT_TIMEOUT_MS)
    if timeout_ms <= 0:
        raise RequestError(ST_USER, "timeout_ms must be > 0")
    max_bytes = int(max_bytes or DEFAULT_MAX_BYTES)
    if max_bytes <= 0:
        raise RequestError(ST_USER, "max_bytes must be > 0")
    req_headers = {"User-Agent": "JMComic-C1-Bridge/1.0"}
    if headers:
        if not isinstance(headers, dict):
            raise RequestError(ST_USER, "headers must be an object")
        req_headers.update({str(k): str(v) for k, v in headers.items()})

    payload = None
    if body is not None:
        if isinstance(body, (dict, list)):
            payload = json.dumps(body).encode("utf-8")
            req_headers.setdefault("Content-Type", "application/json")
        elif isinstance(body, str):
            payload = body.encode("utf-8")
        else:
            raise RequestError(ST_USER, "body must be string/object/array")

    attempts_allowed = max(1, int(retries) + 1)
    # timeout_ms 是**总预算**，不是每次尝试各自的超时：否则 retries=1 时实际耗时
    # 可以到 2 倍预算，C++ 侧先超时，"超时"就退化成"没回话"，定位不到是哪一层。
    per_attempt_ms = max(200, timeout_ms // attempts_allowed)
    started = time.monotonic()
    last_err = None
    attempt = 0
    while attempt < attempts_allowed:
        attempt += 1
        try:
            payload_out = _once(url, method, req_headers, payload,
                                per_attempt_ms / 1000.0, proxy, max_bytes, binary)
            payload_out["attempts"] = attempt
            payload_out["backend"] = "stdlib"
            payload_out["elapsed_ms"] = _elapsed_ms(started)
            if not include_body:
                payload_out.pop("body", None)
                payload_out.pop("body_b64", None)
            return payload_out
        except RequestError as exc:
            last_err = exc
            if exc.code not in RETRYABLE or attempt >= attempts_allowed:
                break
            time.sleep(min(0.2 * attempt, 1.0))

    # 全部尝试失败：把最后一条错误带上 attempts，让 C++ 侧能区分"没试"和"试过"
    err = last_err or RequestError(ST_NET, "unknown failure")
    err.attempts = attempt
    err.elapsed_ms = _elapsed_ms(started)
    raise err


def probe_status():
    """子进程自述：给 net.status 用。只读 /proc，不引入 psutil。"""
    info = {
        "protocol_version": PROTOCOL_VERSION,
        "python": sys.version.split()[0],
        "backend": "stdlib",
        "pid": os.getpid(),
        "uptime_ms": _elapsed_ms(_PROCESS_STARTED_AT),
    }
    try:
        with open("/proc/self/statm", "r") as fp:
            pages = int(fp.read().split()[1])
        info["rss_kb"] = pages * (os.sysconf("SC_PAGE_SIZE") // 1024)
    except Exception:
        info["rss_kb"] = -1
    return info


_PROCESS_STARTED_AT = time.monotonic()
