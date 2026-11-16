"""本地更新服务 mock：验证"有新版本 + 更新日志 + 配置"与失败路径。

用法：python cpp/tools/update_mock_server.py <port>
  /JMComic/version.txt      -> v1.3.9
  /JMComic/v1.3.9.txt       -> 更新日志
  /JMComic/config.txt       -> key=value 配置
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

VERSION = b"v1.3.9"
CHANGELOG = "1)测试更新日志第一行\n2)第二行：修复若干问题\n".encode("utf-8")
CONFIG = (
    "Ver=99\n"
    "UrlList=https://web1.example,https://web2.example\n"
    "Url2List=https://api1.example,https://api2.example,https://api3.example\n"
    "PicUrlList=https://img1.example,https://img2.example\n"
    "ProxyApiUrl=https://api1.example\n"
    "CdnApiUrl=https://api1.example\n"
    "HeaderVer=9.9.9\n"
    "VerTime=2027-1-1\n"
).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/JMComic/version.txt":
            body = VERSION
        elif path == "/JMComic/v1.3.9.txt":
            body = CHANGELOG
        elif path == "/JMComic/config.txt":
            body = CONFIG
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
