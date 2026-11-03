// C3：禁漫移动端接口的加解密（纯 C++，无第三方 crypto 依赖）。
//
// 为什么要自己实现 AES：
//   * Qt 没有公开 AES API；链 OpenSSL 会让宿主测试（Windows/MinGW）也跟着拉一份
//     dll，而这条链路的验收方式是"用独立实现的密文做向量比对"，不是"能调库"；
//   * 只需要 AES-256-ECB **解密**（服务端加密、客户端解），代码量可控（本文件 ~200 行），
//     且能被 host_tests/test_jm_crypto 直接拿 openssl 生成的向量逐条钉住。
//
// 常量来源：hect0x7/JMComic-Crawler-Python 2.7.7
//   jmcomic/jm_config.py   JmMagicConstants.APP_TOKEN_SECRET / APP_DATA_SECRET / APP_VERSION
//   jmcomic/jm_toolkit.py  JmCryptoTool.token_and_tokenparam / decode_resp_data
//   jmcomic/jm_toolkit.py  JmApiAdaptTool（album/photo 字段）
//   src/tools/tool.py      ToolUtil.GetSegmentationNum / SegmentationPicture（图片切割重排）
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace JmCrypto {

// JM 移动端常量（2.7.7）。APP_VERSION 会被 /setting 返回的 jm3_version 覆盖。
constexpr const char *kTokenSecret = "185Hcomic3PAPP7R";       // 普通接口
constexpr const char *kTokenSecretView = "18comicAPPContent";   // /chapter_view_template 专用
constexpr const char *kDataSecret = "185Hcomic3PAPP7R";
constexpr const char *kDomainServerSecret = "diosfjckwpqpdfjkvnqQjsik";
constexpr const char *kAppVersion = "2.1.7";

// md5(ts + secret) 的小写十六进制；token 与 AES key 都是这个值。
QByteArray Md5Hex(const QString &text);

// tokenparam = "<ts>,<ver>"；token = md5(ts+secret)
QString TokenParam(const QString &ts, const QString &ver = QString::fromLatin1(kAppVersion));
QString Token(const QString &ts, const QString &secret = QString::fromLatin1(kTokenSecret));

// AES-256-ECB 解密（key 必须是 32 字节；长度不符返回空）。
// 不解 padding：调用方按 JM 的 `data[:-data[-1]]` 语义自己裁。
QByteArray Aes256EcbDecrypt(const QByteArray &in, const QByteArray &key);

// 完整还原一次接口响应：base64 -> AES(key=md5(ts+secret)) -> 去 PKCS#7 -> 明文。
// 失败返回空 QByteArray（调用方据此判定"解密失败"，不要拿空当合法 JSON）。
QByteArray DecodeRespData(const QByteArray &base64Data, const QString &ts,
                          const QString &secret = QString::fromLatin1(kDataSecret));

// 图片被切成 num 条横带后**上下颠倒**存放；num<=1 表示原图。
int SegmentationNum(qint64 epsId, qint64 scrambleId, const QString &pictureName);

// 按 num 把横带顺序反转，还原成可显示的图。num<=1 或空图原样返回。
QImage Descramble(const QImage &src, int num);

// images[0] 形如 "00001.webp" -> ".webp"；识别不出就返回空（调用方回落 .jpg）
QString SuffixOf(const QString &fileName);

}  // namespace JmCrypto
