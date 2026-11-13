// 界面偏好落库（ui.ini 的 [ui] 分组）。
//
// 背景：设置页大多数控件（主题 / 语言 / 开机自启 / 阅读方向 / 亮度 / 自动翻页 /
// 超时 / 并发 / 超分模型…）此前只改内存，进程一退就回默认值。用户看到的就是
// "版本一升级（其实重启也一样）个性化设置全没了"。
//
// 字体（AppFont）、缓存目录（CachePath）、阅读模式（ReaderView）、分流（ProxyConfig）
// 各自有独立的落库实现，不重复接管，这里只补"没地方存"的那批。
#pragma once

#include <QString>
#include <QVariant>

namespace UiPrefs {

QVariant Get(const QString &key, const QVariant &def = QVariant());
int GetInt(const QString &key, int def);
bool GetBool(const QString &key, bool def);
QString GetString(const QString &key, const QString &def = QString());

void Set(const QString &key, const QVariant &value);

// 一行诊断（--data-report 用）：把会影响观感的偏好打出来。
QString Report();

}  // namespace UiPrefs
