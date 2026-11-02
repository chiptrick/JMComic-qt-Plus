#!/usr/bin/env python3
# coding:utf-8
"""C1-W6/P4：Python 子进程桥 —— 每行一个 JSON 请求，回一行 JSON 响应。

协议（v1，见 docs/c1_bridge_protocol.md）：

  请求  {"id": 1, "method": "net.request", "params": {...}}
  成功  {"id": 1, "method": "net.request", "ok": true,  "result": {...}}
  失败  {"id": 1, "method": "net.request", "ok": false, "error": {"code": "...", "message": "..."}}

方法：
  ping / echo / crash   自检与故障注入（W6 原有，保持兼容）
  net.request           真实 HTTP 请求（P4 新增）
  net.status            子进程自述：版本/后端/内存/计数器
  net.close             优雅关闭：回响应后退出

响应里恒定回带 `method`：C++ 侧若只按 pendingId 关联，无法判断回来的到底是哪个方法，
main.cpp 早期就因此永远认不出 pong（既有真实缺陷，这里一并修掉）。
"""

import json
import sys

# 与 bridge.py 同目录时 sys.path[0] 就是脚本目录，net_client 可直接 import。
from net_client import PROTOCOL_VERSION, RequestError, probe_status, request

_STATS = {"requests": 0, "failures": 0, "last_error": ""}


def _error(code, message):
    return {"ok": False, "error": {"code": code, "message": message}}


def _handle_ping(params):
    return {"ok": True, "result": "pong"}


def _handle_echo(params):
    return {"ok": True, "result": params}


def _handle_crash(params):
    # 故意崩溃，用于验证 C++ 侧的崩溃检测与自动重启
    sys.stdout.flush()
    raise SystemExit(3)


def _handle_net_request(params):
    if not isinstance(params, dict):
        return _error("UserError", "params must be an object")
    try:
        result = request(
            method=params.get("method", "GET"),
            url=params.get("url", ""),
            headers=params.get("headers"),
            body=params.get("body"),
            timeout_ms=params.get("timeout_ms"),
            retries=params.get("retries", 1),
            max_bytes=params.get("max_bytes"),
            include_body=params.get("include_body", True),
            binary=params.get("binary", False),
            proxy=params.get("proxy"),
        )
        return {"ok": True, "result": result}
    except RequestError as exc:
        # 把 attempts/elapsed 一起带回去：能区分"第一次就失败"和"重试后才失败"
        return {
            "ok": False,
            "error": {
                "code": exc.code,
                "message": exc.message,
                "attempts": getattr(exc, "attempts", 1),
                "elapsed_ms": getattr(exc, "elapsed_ms", 0),
            },
        }
    except Exception as exc:  # noqa: BLE001 - 桥不能因为未预期异常直接死掉
        return _error("NetError", "%s: %s" % (type(exc).__name__, exc))


def _handle_net_status(params):
    info = probe_status()
    info["stats"] = dict(_STATS)
    return {"ok": True, "result": info}


def _handle_net_close(params):
    return {"ok": True, "result": {"closing": True}}


_HANDLERS = {
    "ping": _handle_ping,
    "echo": _handle_echo,
    "crash": _handle_crash,
    "net.request": _handle_net_request,
    "net.status": _handle_net_status,
    "net.close": _handle_net_close,
}


def _emit(req_id, method, res):
    out = {"id": req_id, "method": method}
    out.update(res)
    out["protocol"] = PROTOCOL_VERSION
    sys.stdout.write(json.dumps(out, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            _emit(None, None, _error("UserError", "bad json"))
            continue
        if not isinstance(req, dict):
            _emit(None, None, _error("UserError", "request must be an object"))
            continue

        req_id = req.get("id")
        method = req.get("method")
        params = req.get("params") or {}

        handler = _HANDLERS.get(method)
        if handler is None:
            _STATS["failures"] += 1
            _STATS["last_error"] = "unknown method: %s" % method
            _emit(req_id, method, _error("UserError", "unknown method: %s" % method))
            continue

        try:
            res = handler(params)
        except SystemExit:
            raise
        except Exception as exc:  # noqa: BLE001
            res = _error("NetError", "%s: %s" % (type(exc).__name__, exc))

        _STATS["requests"] += 1
        if not res.get("ok"):
            _STATS["failures"] += 1
            _STATS["last_error"] = res["error"]["message"]
        _emit(req_id, method, res)

        # net.close：先保证响应写出去再退出，否则 C++ 侧会看到"没回话就断"
        if method == "net.close":
            break


if __name__ == "__main__":
    main()
