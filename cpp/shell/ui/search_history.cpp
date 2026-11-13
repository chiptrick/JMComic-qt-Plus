#include "search_history.h"

#include <QSettings>

#include "app_paths.h"

namespace {

constexpr int kMaxItems = 10;
const char *kKey = "search/recent";

QString SettingsPath()
{
    // 与 AppPaths 同源；不可写时返回空串，读写都变成空操作（并由 Report 暴露）。
    return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
}

QString Normalize(const QString &s)
{
    return s.trimmed();
}

}  // namespace

namespace SearchHistory {

int MaxItems()
{
    return kMaxItems;
}

QStringList Items()
{
    const QString path = SettingsPath();
    if (path.isEmpty())
        return QStringList();
    QSettings s(path, QSettings::IniFormat);
    QStringList raw = s.value(QString::fromLatin1(kKey)).toStringList();

    // 落库数据可能被手工编辑过：trim、去空、去重（忽略大小写）、截断。
    QStringList out;
    out.reserve(qMin(raw.size(), kMaxItems));
    for (const QString &item : raw) {
        const QString v = Normalize(item);
        if (v.isEmpty())
            continue;
        bool dup = false;
        for (const QString &seen : out) {
            if (seen.compare(v, Qt::CaseInsensitive) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        out << v;
        if (out.size() >= kMaxItems)
            break;
    }
    return out;
}

bool Add(const QString &keyword)
{
    const QString key = Normalize(keyword);
    if (key.isEmpty())
        return false;

    QStringList list = Items();
    QStringList next;
    next << key;
    for (const QString &item : list) {
        if (item.compare(key, Qt::CaseInsensitive) == 0)
            continue;   // 旧位置的同一个词去掉，由队首替代（MRU）
        next << item;
    }
    while (next.size() > kMaxItems)
        next.removeLast();

    if (next == list)
        return false;   // 已经在队首，不重复写盘

    const QString path = SettingsPath();
    if (path.isEmpty()) {
        qWarning("search-history: 没有可写的配置目录，搜索记录未落库");
        return false;
    }
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QString::fromLatin1(kKey), next);
    s.sync();
    if (s.status() != QSettings::NoError) {
        qWarning("search-history: 写入失败 status=%d path=%s", int(s.status()),
                 qPrintable(path));
        return false;
    }
    return true;
}

void Clear()
{
    const QString path = SettingsPath();
    if (path.isEmpty()) {
        qWarning("search-history: 没有可写的配置目录，无法清空");
        return;
    }
    QSettings s(path, QSettings::IniFormat);
    s.remove(QString::fromLatin1(kKey));
    s.sync();
    if (s.status() != QSettings::NoError)
        qWarning("search-history: 清空失败 status=%d", int(s.status()));
}

QString Report()
{
    const QStringList list = Items();
    return QStringLiteral("history=%1 first=%2")
        .arg(list.size())
        .arg(list.isEmpty() ? QStringLiteral("-") : list.first());
}

}  // namespace SearchHistory
