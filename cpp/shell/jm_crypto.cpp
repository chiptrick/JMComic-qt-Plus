#include "jm_crypto.h"

#include <QCryptographicHash>
#include <QPair>
#include <QVector>

#include <cstring>

namespace JmCrypto {
namespace {

// ---------------------------------------------------------------- AES 基础表
const unsigned char kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

const unsigned char kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

inline unsigned char XTime(unsigned char x)
{
    return static_cast<unsigned char>((x << 1) ^ ((x >> 7) * 0x1b));
}

// GF(2^8) 乘法，只用得到 9/11/13/14 四个常量，直接做移位加法，不建表。
inline unsigned char Mul(unsigned char a, unsigned char b)
{
    unsigned char r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = XTime(a);
        b >>= 1;
    }
    return r;
}

// AES-256：Nk=8, Nr=14, 轮密钥 15*16 = 240 字节
void KeyExpansion(const unsigned char *key, unsigned char *rk)
{
    std::memcpy(rk, key, 32);
    unsigned char rcon = 1;
    for (int i = 32; i < 240; i += 4) {
        unsigned char t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 32 == 0) {
            const unsigned char first = t[0];
            t[0] = static_cast<unsigned char>(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[first];
            rcon = XTime(rcon);
        } else if (i % 32 == 16) {
            for (int k = 0; k < 4; ++k)
                t[k] = kSbox[t[k]];
        }
        for (int k = 0; k < 4; ++k)
            rk[i + k] = static_cast<unsigned char>(rk[i + k - 32] ^ t[k]);
    }
}

void AddRoundKey(unsigned char *s, const unsigned char *rk)
{
    for (int i = 0; i < 16; ++i)
        s[i] = static_cast<unsigned char>(s[i] ^ rk[i]);
}

void InvSubBytes(unsigned char *s)
{
    for (int i = 0; i < 16; ++i)
        s[i] = kInvSbox[s[i]];
}

// 状态按列优先：s[row + 4*col]。InvShiftRows 把第 row 行循环右移 row 位。
void InvShiftRows(unsigned char *s)
{
    for (int row = 1; row < 4; ++row) {
        unsigned char t[4];
        for (int col = 0; col < 4; ++col)
            t[col] = s[row + 4 * ((col + 4 - row) % 4)];
        for (int col = 0; col < 4; ++col)
            s[row + 4 * col] = t[col];
    }
}

void InvMixColumns(unsigned char *s)
{
    for (int col = 0; col < 4; ++col) {
        unsigned char *c = s + 4 * col;
        const unsigned char a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
        c[0] = static_cast<unsigned char>(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
        c[1] = static_cast<unsigned char>(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
        c[2] = static_cast<unsigned char>(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
        c[3] = static_cast<unsigned char>(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
    }
}

void DecryptBlock(unsigned char *s, const unsigned char *rk)
{
    AddRoundKey(s, rk + 14 * 16);
    for (int round = 13; round >= 1; --round) {
        InvShiftRows(s);
        InvSubBytes(s);
        AddRoundKey(s, rk + round * 16);
        InvMixColumns(s);
    }
    InvShiftRows(s);
    InvSubBytes(s);
    AddRoundKey(s, rk);
}

}  // namespace

QByteArray Md5Hex(const QString &text)
{
    return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Md5).toHex();
}

QString TokenParam(const QString &ts, const QString &ver)
{
    return ts + QLatin1Char(',') + ver;
}

QString Token(const QString &ts, const QString &secret)
{
    return QString::fromLatin1(Md5Hex(ts + secret));
}

QByteArray Aes256EcbDecrypt(const QByteArray &in, const QByteArray &key)
{
    if (key.size() != 32 || in.isEmpty() || (in.size() % 16) != 0)
        return QByteArray();

    unsigned char rk[240];
    KeyExpansion(reinterpret_cast<const unsigned char *>(key.constData()), rk);

    QByteArray out = in;
    auto *p = reinterpret_cast<unsigned char *>(out.data());
    for (int off = 0; off < out.size(); off += 16)
        DecryptBlock(p + off, rk);
    return out;
}

QByteArray DecodeRespData(const QByteArray &base64Data, const QString &ts, const QString &secret)
{
    // 接口里 data 字段可能有换行/空白（服务端历史上出现过），先清掉再解 base64。
    QByteArray clean = base64Data;
    clean.replace("\r", "").replace("\n", "").replace(" ", "");
    const QByteArray cipher = QByteArray::fromBase64(clean);
    if (cipher.isEmpty() || (cipher.size() % 16) != 0)
        return QByteArray();

    const QByteArray key = Md5Hex(ts + secret);
    QByteArray plain = Aes256EcbDecrypt(cipher, key);
    if (plain.isEmpty())
        return QByteArray();

    // JM 的 padding 处理：data[:-data[-1]]（等价 PKCS#7，但对越界值要兜住）
    const int pad = static_cast<unsigned char>(plain.at(plain.size() - 1));
    if (pad <= 0 || pad > 16 || pad > plain.size())
        return QByteArray();
    return plain.left(plain.size() - pad);
}

QString ScrambleStem(const QString &nameOrUrl)
{
    QString s = nameOrUrl.trimmed();
    // 接口偶尔把整条带查询串的 URL 塞进 images（"?v=..."），查询串不能参与哈希
    const int query = s.indexOf(QLatin1Char('?'));
    if (query >= 0)
        s.truncate(query);
    const int slash = s.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.mid(slash + 1);
    // 只剥最后一段扩展名；点开头（隐藏文件）与无扩展名保持原样
    const int dot = s.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        s.truncate(dot);
    return s;
}

int SegmentationNum(qint64 epsId, qint64 scrambleId, const QString &pictureName)
{
    // 与 ToolUtil.GetSegmentationNum 完全一致：先比 scramble_id，再按 268850/421926
    // 两个门槛分三档。注意不能给 scrambleId 加 "<=0 就返回 0" 的守卫——Python 端没有，
    // 加了会让 scramble 缺失（0）的图直接不还原，看着像"图没乱但就是不对"。
    if (epsId < scrambleId)
        return 0;
    if (epsId < 268850)
        return 10;
    if (epsId > 421926) {
        const QByteArray hex = Md5Hex(QString::number(epsId) + pictureName);
        const char last = hex.isEmpty() ? '0' : hex.at(hex.size() - 1);
        return (last % 8) * 2 + 2;
    }
    const QByteArray hex = Md5Hex(QString::number(epsId) + pictureName);
    const char last = hex.isEmpty() ? '0' : hex.at(hex.size() - 1);
    return (last % 10) * 2 + 2;
}

QImage Descramble(const QImage &src, int num)
{
    if (src.isNull() || num <= 1)
        return src;

    const int height = src.height();
    const int width = src.width();
    const int copyHeight = height / num;
    if (copyHeight <= 0)
        return src;
    const int rem = height % num;

    // 切成 num 条，最后一条吃掉余数（与 Python 端 h = copyHeight*(i+1); i==last += rem 一致）
    QVector<QPair<int, int>> blocks;
    blocks.reserve(num);
    int total = 0;
    for (int i = 0; i < num; ++i) {
        int end = copyHeight * (i + 1);
        if (i == num - 1)
            end += rem;
        blocks.append(qMakePair(total, end));
        total = end;
    }

    QImage out(src.size(), src.format());
    if (out.isNull())
        return src;
    // 用 bytesPerLine() 而不是 width*depth/8：行尾可能有对齐填充（如 8 位灰度/索引色），
    // 少拷那几字节会让整幅图的列错位。
    const int srcStride = src.bytesPerLine();
    const int dstStride = out.bytesPerLine();
    const int copyBytes = qMin(srcStride, dstStride);
    int destY = 0;
    for (int i = num - 1; i >= 0; --i) {
        const int start = blocks.at(i).first;
        const int end = blocks.at(i).second;
        const int blockH = end - start;
        for (int y = 0; y < blockH; ++y) {
            std::memcpy(out.scanLine(destY + y), src.scanLine(start + y),
                        static_cast<size_t>(copyBytes));
        }
        destY += blockH;
    }
    return out;
}

QString SuffixOf(const QString &fileName)
{
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot + 1 >= fileName.size())
        return QString();
    const QString suffix = fileName.mid(dot).toLower();
    static const char *const known[] = {".jpg", ".jpeg", ".webp", ".png", ".gif"};
    for (const char *k : known) {
        if (suffix == QLatin1String(k))
            return suffix;
    }
    return QString();
}

}  // namespace JmCrypto
