#include "jm_webp.h"

extern "C" {
#include "webp/decode.h"
}

namespace JmWebp {

bool IsWebp(const QByteArray &data)
{
    // RIFF....WEBP：偏移 0 是 "RIFF"，偏移 8 是 "WEBP"
    return data.size() > 12 && qstrncmp(data.constData(), "RIFF", 4) == 0
           && qstrncmp(data.constData() + 8, "WEBP", 4) == 0;
}

QImage Decode(const QByteArray &data)
{
    if (!IsWebp(data))
        return QImage();

    int width = 0, height = 0;
    if (!WebPGetInfo(reinterpret_cast<const uint8_t *>(data.constData()),
                     static_cast<size_t>(data.size()), &width, &height))
        return QImage();
    if (width <= 0 || height <= 0 || static_cast<qint64>(width) * height > 64LL * 1024 * 1024)
        return QImage();  // 64M 像素上限：防止异常响应把内存打爆

    QByteArray rgba;
    rgba.resize(static_cast<int>(static_cast<qint64>(width) * height * 4));
    uint8_t *out = WebPDecodeRGBAInto(
        reinterpret_cast<const uint8_t *>(data.constData()),
        static_cast<size_t>(data.size()), reinterpret_cast<uint8_t *>(rgba.data()),
        static_cast<size_t>(rgba.size()), static_cast<int>(static_cast<qint64>(width) * 4));
    if (!out)
        return QImage();

    // QImage 不持有外部 buffer，必须 copy() 深拷贝后再返回
    const QImage wrapped(reinterpret_cast<const uchar *>(rgba.constData()), width, height,
                         width * 4, QImage::Format_RGBA8888);
    return wrapped.copy();
}

}  // namespace JmWebp
