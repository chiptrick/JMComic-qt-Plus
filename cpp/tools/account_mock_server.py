"""账号网页端点 mock：验证注册/找回密码/验证码的完整请求-响应路径。

用法：python cpp/tools/account_mock_server.py <port>

约定：
  GET  /signup    -> 下发会话 Cookie，返回一个空表单页
  POST /signup    -> verification == "0000" 时回 toastr success，否则回 toastr error
  POST /lost      -> 邮箱包含 "bad" 时回 error，否则回 success
  POST /confirm   -> 固定回 success
  GET  /captcha   -> 一个 40x16 的 PNG

这样可以在没有真实站点（域名不可达 / 需要过 CF）的环境里，验证：
  * 表单字段与 Content-Type 是否正确；
  * 预取 Cookie 是否被带进 POST；
  * toastr 文案解析与失败映射是否正确。
"""
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs


def _png(w=8, h=8, rgb=(200, 60, 60)):
    """生成一张合法的纯色 PNG（避免依赖 Pillow）。"""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        head = struct.pack(">I", len(data)) + tag + data
        return head + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG_1x1 = _png()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, body, ctype="text/html; charset=utf-8", set_cookie=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if set_cookie:
            self.send_header("Set-Cookie", set_cookie)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/captcha":
            self._send(PNG_1x1, "image/png")
            return
        if path in ("/signup", "/lost", "/confirm"):
            self._send("<html><body><form></form></body></html>",
                       set_cookie="jm_mock_session=abc123; Path=/")
            return
        self.send_error(404)

    def do_POST(self):
        path = self.path.split("?")[0]
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace")
        form = {k: v[0] for k, v in parse_qs(raw, keep_blank_values=True).items()}
        cookie = self.headers.get("Cookie") or ""
        hit = "cookie" if "jm_mock_session=abc123" in cookie else "no-cookie"
        if path == "/signup":
            if form.get("verification") == "0000":
                self._send("<script>toastr['success'](\"注册成功\")</script>")
            else:
                self._send("<script>toastr['error'](\"验证码错误\")</script>")
        elif path == "/lost":
            if "bad" in form.get("email", ""):
                self._send("<script>toastr['error'](\"邮箱不存在\")</script>")
            else:
                self._send("<script>toastr['success'](\"找回密码邮件已发送\")</script>")
        elif path == "/confirm":
            self._send("<script>toastr['success'](\"验证邮件已重新发送\")</script>")
        else:
            self.send_error(404)
            return
        sys.stderr.write("mock: POST %s fields=%s %s\n" % (path, sorted(form), hit))

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
