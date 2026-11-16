// DataMigrator 宿主测试：升级迁移 / 备份 / 损坏隔离 / 导出导入。
//
// 跑法（host_tests/build-win 已配置）：
//   build-win\host_test_data_migrator.exe
//
// 用 JM_UI_CONFIG 指到临时目录，ConfigDir 在进程内只解析一次，
// 所以各场景共用同一个目录，场景开始前清空它。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

#include "../ui/app_paths.h"
#include "../ui/data_migrator.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static QString ReadAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

static void WriteText(const QString &path, const QString &text)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(text.toUtf8());
}

static QStringList FindByPattern(const QDir &dir, const QString &prefix)
{
    return dir.entryList(QStringList() << (prefix + QStringLiteral("*")), QDir::Files);
}

static void CleanConfigDir(const QString &dir)
{
    QDir d(dir);
    for (const QFileInfo &fi : d.entryInfoList(QDir::Files))
        QFile::remove(fi.absoluteFilePath());
    if (d.exists(QStringLiteral("backup")))
        QDir(d.filePath(QStringLiteral("backup"))).removeRecursively();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);   // applicationVersion 参与迁移日志
    QCoreApplication::setApplicationVersion(QStringLiteral("0.96-test"));
    // 沙箱/受限环境里 TMP 可能不可写：临时目录建在测试 exe 旁边，保证可用。
    QTemporaryDir tmp(QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("tmp-migr-XXXXXX")));
    tmp.setAutoRemove(true);
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL cannot create temp dir\n");
        return 2;
    }
    qputenv("JM_UI_CONFIG", tmp.path().toUtf8());
    // SessionStore::Path() 不得被 JM_SESSION_FILE 重定向，否则迁移器操作的
    // session.ini 与配置目录脱节。
    qunsetenv("JM_SESSION_FILE");

    const QString cfg = AppPaths::ConfigDir();
    CHECK(!cfg.isEmpty());
    const QDir dir(cfg);

    // ---- 场景 1：全新安装（两份配置都不存在）——不创建文件、不建备份 ----
    DataMigrator::Run();
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("ui.ini"))));
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("session.ini"))));
    CHECK(!dir.exists(QStringLiteral("backup")));
    CHECK(DataMigrator::SchemaVersion() == 1);

    // ---- 场景 2：v1 升级（无 schemaVersion 的旧配置）----
    CleanConfigDir(cfg);
    const QString uiIni = dir.filePath(QStringLiteral("ui.ini"));
    const QString sessionIni = dir.filePath(QStringLiteral("session.ini"));
    WriteText(uiIni, QStringLiteral("[proxy]\nhttp3=true\napiLine=3\n[ui]\ntheme=dark_pink\n"));
    WriteText(sessionIni,
              QStringLiteral("[session]\nloggedIn=true\navs=abcdef\nuid=42\n[user]\nname=tester\n"));
    const QString uiBefore = ReadAll(uiIni);
    const QString sessionBefore = ReadAll(sessionIni);

    DataMigrator::Run();

    // 升级前备份：原始字节
    const QString backupUi = dir.filePath(QStringLiteral("backup/v1/ui.ini"));
    const QString backupSession = dir.filePath(QStringLiteral("backup/v1/session.ini"));
    CHECK(QFileInfo::exists(backupUi));
    CHECK(QFileInfo::exists(backupSession));
    CHECK(ReadAll(backupUi) == uiBefore);
    CHECK(ReadAll(backupSession) == sessionBefore);
    // 迁移结果
    {
        QSettings s(uiIni, QSettings::IniFormat);
        CHECK(s.value(QStringLiteral("meta/schemaVersion")).toInt() == 2);
        CHECK(s.value(QStringLiteral("proxy/http3On")).toBool() == true);
        CHECK(s.value(QStringLiteral("proxy/apiLine")).toInt() == 3);      // 未知键保留
        CHECK(s.value(QStringLiteral("ui/theme")).toString() == QStringLiteral("dark_pink"));
    }
    CHECK(ReadAll(sessionIni) == sessionBefore);   // v1->v2 不动 session
    CHECK(DataMigrator::SchemaVersion() == 2);

    // 幂等：再跑一次文件字节不变、不新建备份
    const QString uiAfterFirst = ReadAll(uiIni);
    DataMigrator::Run();
    CHECK(ReadAll(uiIni) == uiAfterFirst);
    CHECK(!dir.exists(QStringLiteral("backup/v2")));

    // ---- 场景 3：损坏隔离 + .bak 恢复 ----
    CleanConfigDir(cfg);
    // 垃圾字节刻意不带 '='/'[' 行、不带 BOM：让 INI 解析器得到"零个键"的
    // 结果（带 UTF-16 BOM 的形态反而会被解码成一堆乱码键，绕过检测）。
    WriteText(sessionIni + QStringLiteral(".bak"),
              QStringLiteral("[session]\nloggedIn=true\navs=goodbak\n"));
    WriteText(sessionIni, QString::fromUtf8(QByteArray("\x01\x02\x03\x04\x05")));
    WriteText(uiIni, QString::fromUtf8(QByteArray("\x01\x02broken\x03")));
    DataMigrator::Run();
    const QStringList corruptUi = FindByPattern(dir, QStringLiteral("ui.ini.corrupt-"));
    const QStringList corruptSession = FindByPattern(dir, QStringLiteral("session.ini.corrupt-"));
    CHECK(corruptUi.size() == 1);
    CHECK(corruptSession.size() == 1);
    CHECK(ReadAll(sessionIni).contains(QStringLiteral("avs=goodbak")));   // 从 .bak 恢复
    CHECK(!ReadAll(uiIni).contains(QStringLiteral("theme=")));           // 无备份 -> 空配置

    // ---- 场景 4：--export-data / --import-data ----
    CleanConfigDir(cfg);
    WriteText(uiIni, QStringLiteral("[ui]\ntheme=light_teal\n"));
    WriteText(sessionIni,
              QStringLiteral("[session]\nloggedIn=true\navs=exportme\n[user]\nname=exporter\n"));
    // 同样建在 exe 旁（默认 TMP 在本沙箱不可写）
    QTemporaryDir outDir(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("tmp-migr-out-XXXXXX")));
    const QString err = DataMigrator::ExportData(outDir.path());
    CHECK(err.isEmpty());
    CHECK(ReadAll(QDir(outDir.path()).filePath(QStringLiteral("session.ini")))
              .contains(QStringLiteral("avs=exportme")));
    CHECK(ReadAll(QDir(outDir.path()).filePath(QStringLiteral("ui.ini")))
              .contains(QStringLiteral("theme=light_teal")));

    // 导入覆盖当前现场：旧值先进 .bak
    WriteText(sessionIni, QStringLiteral("[session]\nloggedIn=false\n"));
    const QString err2 = DataMigrator::ImportData(outDir.path());
    CHECK(err2.isEmpty());
    CHECK(ReadAll(sessionIni).contains(QStringLiteral("avs=exportme")));
    CHECK(ReadAll(sessionIni + QStringLiteral(".bak")).contains(QStringLiteral("loggedIn=false")));

    // 负例：空目录 / 不存在的目录
    QTemporaryDir emptyDir;
    CHECK(!DataMigrator::ImportData(QDir(emptyDir.path()).filePath(QStringLiteral("nope")))
               .isEmpty());
    CHECK(!DataMigrator::ImportData(emptyDir.path()).isEmpty());

    // ---- 场景 5：RollingBackup 只滚动"解析通过"的文件 ----
    CleanConfigDir(cfg);
    WriteText(uiIni, QStringLiteral("[ui]\ntheme=v5\n"));
    DataMigrator::RollingBackup(uiIni);
    CHECK(ReadAll(uiIni + QStringLiteral(".bak")).contains(QStringLiteral("theme=v5")));
    WriteText(uiIni, QString::fromUtf8(QByteArray("\x07\x08broken")));
    DataMigrator::RollingBackup(uiIni);
    CHECK(ReadAll(uiIni + QStringLiteral(".bak")).contains(QStringLiteral("theme=v5")));  // 坏文件没有滚进去

    // ---- 汇总 ----
    if (g_fail == 0) {
        std::printf("host_test_data_migrator: ALL PASS\n");
        return 0;
    }
    std::printf("host_test_data_migrator: %d FAILURE(S)\n", g_fail);
    return 1;
}
