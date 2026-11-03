// WebP 解码（内嵌 libwebp 解码端）。
//
// 为什么需要它：Qt 6.12 的安装包（android_* 与 mingw_64）都不带 qwebp 插件，
// QImage::fromData 对 webp 直接失败；而 JM 下发的图片**全是 webp**（CDN 对 .jpg
// 后缀回 502）。没有这一层，C2/C3 的图片链路在"下载成功"之后整段断掉。
// 解码端源码取自 qtimageformats-everywhere-src-6.12.0 的 3rdparty/libwebp（BSD-3，
// 见 3rdparty/libwebp/COPYING），只编 dec/dsp/utils，不含编码器。
#pragma once

#include <QByteArray>
#include <QImage>

namespace JmWebp {

// 输入是完整 webp 文件字节（RIFF....WEBP）；解码失败返回空 QImage。
QImage Decode(const QByteArray &data);

// 是否是 webp 文件头（RIFF....WEBP）
bool IsWebp(const QByteArray &data);

}  // namespace JmWebp
