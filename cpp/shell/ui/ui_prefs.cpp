#include "ui_prefs.h"

#include <QSettings>
#include <QStringList>

#include "app_paths.h"

namespace UiPrefs {

QVariant Get(const QString &key, const QVariant &def)
{
    const QString path = AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    if (path.isEmpty())
        return def;
    QSettings s(path, QSettings::IniFormat);
    return s.value(QStringLiteral("ui/") + key, def);
}

int GetInt(const QString &key, int def)
{
    bool ok = false;
    const int v = Get(key).toInt(&ok);
    return ok ? v : def;
}

bool GetBool(const QString &key, bool def)
{
    const QVariant v = Get(key);
    return v.isValid() ? v.toBool() : def;
}

QString GetString(const QString &key, const QString &def)
{
    const QVariant v = Get(key);
    return v.isValid() ? v.toString() : def;
}

void Set(const QString &key, const QVariant &value)
{
    const QString path = AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    if (path.isEmpty()) {
        qWarning("ui-prefs: 没有可写的配置目录，%s 未落库", qPrintable(key));
        return;
    }
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QStringLiteral("ui/") + key, value);
    s.sync();
    if (s.status() != QSettings::NoError)
        qWarning("ui-prefs: 写入失败 status=%d key=%s", int(s.status()), qPrintable(key));
}

QString Report()
{
    QStringList parts;
    parts << QStringLiteral("theme=%1").arg(GetString(QStringLiteral("theme"), QStringLiteral("-")));
    parts << QStringLiteral("language=%1").arg(GetInt(QStringLiteral("language"), 0));
    parts << QStringLiteral("autoStart=%1").arg(int(GetBool(QStringLiteral("autoStart"), true)));
    parts << QStringLiteral("readDirection=%1").arg(GetInt(QStringLiteral("readDirection"), 1));
    parts << QStringLiteral("brightness=%1").arg(GetInt(QStringLiteral("brightness"), 0));
    parts << QStringLiteral("autoNext=%1").arg(int(GetBool(QStringLiteral("autoNext"), false)));
    parts << QStringLiteral("netTimeout=%1").arg(GetInt(QStringLiteral("netTimeout"), 1));
    parts << QStringLiteral("netConcurrency=%1").arg(GetInt(QStringLiteral("netConcurrency"), 1));
    parts << QStringLiteral("srModel=%1").arg(GetInt(QStringLiteral("srModel"), 0));
    parts << QStringLiteral("srNpu=%1").arg(int(GetBool(QStringLiteral("srNpu"), true)));
    return parts.join(QLatin1Char(' '));
}

}  // namespace UiPrefs
