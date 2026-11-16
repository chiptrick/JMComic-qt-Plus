#include "log_export.h"

#include "app_paths.h"
#include "ui_log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtEndian>

#include <vector>

namespace {

constexpr quint32 kMaxEntryBytes = 8u * 1024u * 1024u;   // 单个日志文件上限
// 整包上限：日志是给维护者看的，不是备份。超过 32MB 的包发不出去，
// 所以按"最新优先"截断，并在 summary 里写明被截断了多少。
constexpr quint32 kMaxTotalBytes = 32u * 1024u * 1024u;
constexpr int kMaxEntries = 24;

struct Crc32
{
    quint32 table[256];
    Crc32()
    {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    }
    quint32 operator()(const QByteArray &data) const
    {
        quint32 c = 0xFFFFFFFFu;
        for (char ch : data)
            c = table[(c ^ quint8(ch)) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
};

const Crc32 &Crc()
{
    static const Crc32 c;
    return c;
}

void Put16(QByteArray &out, quint16 v)
{
    char buf[2];
    qToLittleEndian(v, buf);
    out.append(buf, 2);
}

void Put32(QByteArray &out, quint32 v)
{
    char buf[4];
    qToLittleEndian(v, buf);
    out.append(buf, 4);
}

// DOS 时间：秒按 2 秒粒度，年份从 1980 起
void PutDosTime(QByteArray &out, const QDateTime &dt)
{
    const QDate d = dt.date();
    const QTime t = dt.time();
    const int year = qBound(1980, d.year(), 2107);
    const quint16 dosDate = quint16(((year - 1980) << 9) | (d.month() << 5) | d.day());
    const quint16 dosTime = quint16((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    Put16(out, dosTime);
    Put16(out, dosDate);
}

struct Entry
{
    QByteArray name;     // zip 内路径（UTF-8）
    QByteArray data;
    quint32 crc = 0;
    quint32 offset = 0;
};

// 写一条本地文件头 + 数据；返回数据在 central directory 里需要的信息
bool WriteEntry(QFile &f, Entry &e, const QDateTime &mtime)
{
    QByteArray head;
    Put32(head, 0x04034b50);
    Put16(head, 20);        // version needed
    Put16(head, 0x0800);    // flags: UTF-8 文件名
    Put16(head, 0);         // method: store
    PutDosTime(head, mtime);
    Put32(head, e.crc);
    Put32(head, quint32(e.data.size()));
    Put32(head, quint32(e.data.size()));
    Put16(head, quint16(e.name.size()));
    Put16(head, 0);         // extra len
    head += e.name;
    if (f.write(head) != head.size())
        return false;
    if (!e.data.isEmpty() && f.write(e.data) != e.data.size())
        return false;
    return true;
}

bool WriteCentral(QFile &f, const std::vector<Entry> &entries, const QDateTime &mtime,
                  quint32 cdOffset)
{
    QByteArray cd;
    for (const Entry &e : entries) {
        Put32(cd, 0x02014b50);
        Put16(cd, 20);      // version made by
        Put16(cd, 20);      // version needed
        Put16(cd, 0x0800);
        Put16(cd, 0);
        PutDosTime(cd, mtime);
        Put32(cd, e.crc);
        Put32(cd, quint32(e.data.size()));
        Put32(cd, quint32(e.data.size()));
        Put16(cd, quint16(e.name.size()));
        Put16(cd, 0);       // extra
        Put16(cd, 0);       // comment
        Put16(cd, 0);       // disk start
        Put16(cd, 0);       // internal attrs
        Put32(cd, 0);       // external attrs
        Put32(cd, e.offset);
        cd += e.name;
    }
    const quint32 cdSize = quint32(cd.size());
    if (f.write(cd) != cd.size())
        return false;

    QByteArray eocd;
    Put32(eocd, 0x06054b50);
    Put16(eocd, 0);         // disk
    Put16(eocd, 0);         // cd start disk
    Put16(eocd, quint16(entries.size()));
    Put16(eocd, quint16(entries.size()));
    Put32(eocd, cdSize);
    Put32(eocd, cdOffset);
    Put16(eocd, 0);         // comment len
    return f.write(eocd) == eocd.size();
}

QString ScreenTag()
{
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        if (QScreen *s = app->primaryScreen()) {
            const QSize px = s->size();
            const qreal dpr = s->devicePixelRatio();
            return QStringLiteral("%1x%2 @%3x").arg(px.width()).arg(px.height()).arg(dpr, 0, 'g', 3);
        }
    }
    return QStringLiteral("-");
}

}  // namespace

namespace LogExport {

QString LogDir()
{
    const QString p = JmLog::Path();
    return p.isEmpty() ? QString() : QFileInfo(p).absolutePath();
}

QString DefaultFileName()
{
    return QStringLiteral("JMComic-logs-%1.zip")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QString BuildSummary(const QStringList &extraLines)
{
    QStringList out;
    out << QStringLiteral("JMComic C++ UI 诊断摘要")
        << QStringLiteral("生成时间: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
        << QStringLiteral("应用版本: %1").arg(QCoreApplication::applicationVersion())
        << QStringLiteral("Qt: %1（编译 %2）")
               .arg(QString::fromLatin1(qVersion()), QString::fromLatin1(QT_VERSION_STR))
        << QStringLiteral("系统: %1 %2 / %3")
               .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                    QSysInfo::kernelVersion())
        << QStringLiteral("屏幕: %1").arg(ScreenTag())
        << QStringLiteral("日志文件: %1")
               .arg(JmLog::Path().isEmpty() ? QStringLiteral("(Android 走 logcat，未落盘)")
                                            : JmLog::Path())
        << QString()
        << AppPaths::Report()
        << QString();
    for (const QString &line : extraLines)
        out << line;
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString Export(const QString &destDir, const QStringList &extraLines, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return QString();
    };

    const QString dir = destDir.trimmed().isEmpty()
                            ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                            : destDir;
    if (dir.isEmpty())
        return fail(QStringLiteral("无法确定下载目录"));
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("无法创建目录：%1").arg(dir));

    const QString path = QDir(dir).filePath(DefaultFileName());
    QFile zip(path);
    if (!zip.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("无法写入：%1（%2）").arg(path, zip.errorString()));

    std::vector<Entry> entries;
    int skippedForSize = 0;
    quint32 totalBytes = 0;
    const QString logDir = LogDir();
    if (!logDir.isEmpty()) {
        QDir d(logDir);
        QFileInfoList files = d.entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Time);
        for (const QFileInfo &fi : files) {
            if (entries.size() >= size_t(kMaxEntries))
                break;
            if (fi.size() > qint64(kMaxEntryBytes))
                continue;
            if (totalBytes + quint32(fi.size()) > kMaxTotalBytes) {
                ++skippedForSize;
                continue;   // 按时间倒序枚举：先收最新的，后面的不再塞
            }
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            Entry e;
            e.name = fi.fileName().toUtf8();
            e.data = f.readAll();
            f.close();
            e.crc = Crc()(e.data);
            totalBytes += quint32(e.data.size());
            entries.push_back(e);
        }
    }

    QStringList extra = extraLines;
    if (skippedForSize > 0) {
        extra << QStringLiteral("日志: 因单包 %1MB 上限，另有 %2 个较旧的日志未包含")
                     .arg(kMaxTotalBytes / (1024 * 1024))
                     .arg(skippedForSize);
    }

    Entry summary;
    summary.name = QByteArrayLiteral("summary.txt");
    summary.data = BuildSummary(extra).toUtf8();
    summary.crc = Crc()(summary.data);
    entries.push_back(summary);

    const QDateTime now = QDateTime::currentDateTime();
    for (Entry &e : entries) {
        e.offset = quint32(zip.pos());
        if (!WriteEntry(zip, e, now))
            return fail(QStringLiteral("写入失败：%1").arg(zip.errorString()));
    }
    const quint32 cdOffset = quint32(zip.pos());
    if (!WriteCentral(zip, entries, now, cdOffset))
        return fail(QStringLiteral("写入中央目录失败：%1").arg(zip.errorString()));
    zip.close();

    if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0)
        return fail(QStringLiteral("产物为空：%1").arg(path));
    if (error)
        error->clear();
    return path;
}

}  // namespace LogExport
