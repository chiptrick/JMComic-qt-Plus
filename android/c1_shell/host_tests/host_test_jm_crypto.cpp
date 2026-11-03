// C3 解密链路的宿主自检：向量由 openssl CLI 独立生成（不是拿被测实现自证）。
//
// 覆盖：
//   1. token = md5(ts+secret)
//   2. base64 -> AES-256-ECB -> 去 PKCS#7 -> 明文（3 组，含 /setting 的 domain-secret 一组）
//   3. 错 ts / 错密文长度必须返回空（不能把垃圾当 JSON 交给上层）
//   4. 图片切割数（四个门槛分支）与横带重排
#include "jm_crypto.h"

#include <QGuiApplication>
#include <QImage>

#include <cstdio>

namespace {

int g_failed = 0;

void Check(bool ok, const QString &name, const QString &detail = QString())
{
    if (ok) {
        std::printf("[ok]   %s\n", qPrintable(name));
        return;
    }
    ++g_failed;
    std::printf("[FAIL] %s%s%s\n", qPrintable(name),
                detail.isEmpty() ? "" : " -- ", qPrintable(detail));
}

void CheckEq(const QString &got, const QString &want, const QString &name)
{
    Check(got == want, name, QStringLiteral("got=[%1] want=[%2]").arg(got, want));
}

void CheckEqInt(int got, int want, const QString &name)
{
    Check(got == want, name, QStringLiteral("got=%1 want=%2").arg(got).arg(want));
}

}  // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    // ---- 1. token ----
    CheckEq(QString::fromLatin1(JmCrypto::Md5Hex(QStringLiteral("1700566805185Hcomic3PAPP7R"))),
            QStringLiteral("cce2cb071cd0cf371a2e34fd5ad66fd6"),
            QStringLiteral("md5(ts+secret) 与 Python 一致"));
    CheckEq(JmCrypto::Token(QStringLiteral("1700566805")),
            QStringLiteral("cce2cb071cd0cf371a2e34fd5ad66fd6"),
            QStringLiteral("token = md5(ts + 185Hcomic3PAPP7R)"));
    CheckEq(JmCrypto::TokenParam(QStringLiteral("1700566805")),
            QStringLiteral("1700566805,2.1.7"),
            QStringLiteral("tokenparam = \"<ts>,<ver>\""));
    CheckEq(JmCrypto::Token(QStringLiteral("1700566805"), QStringLiteral("18comicAPPContent")),
            QString::fromLatin1(JmCrypto::Md5Hex(QStringLiteral("170056680518comicAPPContent"))),
            QStringLiteral("view_template 用独立 secret"));

    // ---- 2. 接口响应解密（openssl enc -aes-256-ecb 生成） ----
    {
        const QByteArray plain = JmCrypto::DecodeRespData(
            "5P6f3Nu3NCJwYLemCYmf21H4DOLlzm7DQDEXE5fPx6U=", QStringLiteral("1700566805"));
        CheckEq(QString::fromUtf8(plain), QStringLiteral("{\"code\":200,\"msg\":\"hello\"}"),
                QStringLiteral("decode #1 (app secret)"));
    }
    {
        const QByteArray plain = JmCrypto::DecodeRespData(
            "TMe1+R1GyYVAG4gM58HWqmnN82qE/DDJrfk3c0KjuPFeJznvHwmZpScDy0BrYPk0",
            QStringLiteral("1751500000"));
        CheckEq(QString::fromUtf8(plain), QStringLiteral("{\"id\":\"441923\",\"name\":\"[MANA] test\"}"),
                QStringLiteral("decode #2 (带中文/引号)"));
    }
    {
        const QByteArray plain = JmCrypto::DecodeRespData(
            "+FOEPX0oB6LPEQ+kxb8k7QSWWlMH3vpkjGSIrj1YZq4=", QStringLiteral("0"),
            QStringLiteral("diosfjckwpqpdfjkvnqQjsik"));
        CheckEq(QString::fromUtf8(plain), QStringLiteral("{\"Server\":[\"www.cdnhjk.net\"]}"),
                QStringLiteral("decode #3 (domain server secret)"));
    }

    // ---- 3. 失败必须显式返回空 ----
    Check(JmCrypto::DecodeRespData("5P6f3Nu3NCJwYLemCYmf21H4DOLlzm7DQDEXE5fPx6U=",
                                   QStringLiteral("1700566806"))
              .isEmpty(),
          QStringLiteral("错 ts 必须解不出（返回空）"));
    Check(JmCrypto::DecodeRespData("abc", QStringLiteral("1700566805")).isEmpty(),
          QStringLiteral("非 16 字节整数倍必须返回空"));
    Check(JmCrypto::DecodeRespData(QByteArray(), QStringLiteral("1")).isEmpty(),
          QStringLiteral("空输入返回空"));

    // ---- 4. 图片切割 ----
    CheckEqInt(JmCrypto::SegmentationNum(100, 220980, QStringLiteral("00001")), 0,
               QStringLiteral("epsId < scrambleId -> 0（原图）"));
    CheckEqInt(JmCrypto::SegmentationNum(250000, 220980, QStringLiteral("00001")), 10,
               QStringLiteral("268850 以下 -> 固定 10 条"));
    CheckEqInt(JmCrypto::SegmentationNum(420000, 220980, QStringLiteral("00003")), 8,
               QStringLiteral("421926 以下 -> %10*2+2"));
    CheckEqInt(JmCrypto::SegmentationNum(1479594, 220980, QStringLiteral("00001")), 12,
               QStringLiteral("421926 以上 -> %8*2+2"));
    CheckEqInt(JmCrypto::SegmentationNum(3000000, 421926, QStringLiteral("00007")), 2,
               QStringLiteral("大 id 分支 #2"));

    {
        // 8 行、每行像素值=行号的图，4 条横带（每条 2 行）反转后应为 6,7,4,5,2,3,0,1
        QImage src(1, 8, QImage::Format_RGB32);
        for (int y = 0; y < 8; ++y)
            src.setPixel(0, y, qRgb(y, y, y));
        const QImage out = JmCrypto::Descramble(src, 4);
        bool ok = (out.height() == 8);
        QVector<int> rows;
        for (int y = 0; ok && y < 8; ++y) {
            const int v = qRed(out.pixel(0, y));
            rows.append(v);
            static const int want[8] = {6, 7, 4, 5, 2, 3, 0, 1};
            ok = (v == want[y]);
        }
        Check(ok, QStringLiteral("横带反转重排"),
              QStringLiteral("rows=[%1]").arg([&rows] {
                  QStringList parts;
                  for (int v : rows) parts << QString::number(v);
                  return parts.join(QLatin1Char(','));
              }()));
    }
    Check(JmCrypto::Descramble(QImage(), 4).isNull(), QStringLiteral("空图原样返回"));
    {
        QImage img(1, 4, QImage::Format_RGB32);
        Check(JmCrypto::Descramble(img, 0).height() == 4, QStringLiteral("num=0 时原样返回"));
    }

    std::printf(g_failed == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
