// WebP 内嵌解码器的宿主自检。
// 背景：Qt 6.12 不带 qwebp 插件，JM 图片全是 webp，必须自带解码端。
// 向量由 Pillow lossless 生成（4x2，8 个已知像素），逐像素比对。
#include "jm_webp.h"

#include <QImage>

#include <cstdio>

namespace {

const char *kWebpB64 =
    "UklGRkIAAABXRUJQVlA4TDYAAAAvA0AAAD9AJgwxHcTU/1WDTNomf/3O/giybTaSu93fYfMf9IAcAdJmOwgF0JhMANm0i+h/YAM=";

struct Pixel { int r, g, b, a; };
const Pixel kWant[8] = {
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255},
    {128, 128, 128, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}, {10, 20, 30, 255},
};

int g_failed = 0;

void Check(bool ok, const char *name)
{
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

}  // namespace

int main()
{
    const QByteArray data = QByteArray::fromBase64(kWebpB64);
    Check(JmWebp::IsWebp(data), "RIFF....WEBP magic detected");

    const QImage img = JmWebp::Decode(data);
    Check(!img.isNull(), "decode returns image");
    if (img.isNull()) {
        std::printf("\n%d FAILED\n", g_failed);
        return 1;
    }
    Check(img.width() == 4 && img.height() == 2, "size is 4x2");

    int bad = -1;
    for (int i = 0; i < 8; ++i) {
        const QColor c = img.pixelColor(i % 4, i / 4);
        const Pixel &w = kWant[i];
        if (c.red() != w.r || c.green() != w.g || c.blue() != w.b || c.alpha() != w.a) {
            bad = i;
            break;
        }
    }
    Check(bad < 0, "all 8 pixels match (lossless)");

    const QImage empty = JmWebp::Decode("not a webp");
    Check(empty.isNull(), "garbage input returns null");

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
