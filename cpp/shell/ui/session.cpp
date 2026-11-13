#include "session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include "app_paths.h"
#include "models.h"

namespace {

QString FilePath()
{
    // 测试/回归：JM_SESSION_FILE 显式指定路径，避免把"沙箱里 APPDATA 被重定向"的
    // 平台差异带进断言（Windows 上 AppConfigLocation 实际落在 LocalLow）。
    const QString override = qEnvironmentVariable("JM_SESSION_FILE");
    if (!override.isEmpty()) {
        QDir().mkpath(QFileInfo(override).absolutePath());
        return override;
    }
    // 统一走 AppPaths：Android 上绝不再退回 applicationDirPath()（只读且每次安装都变），
    // 目录不可写时返回空串，由 Save() 明确报错而不是静默丢配置。
    AppPaths::MigrateLegacyFile(QStringLiteral("session.ini"));
    return AppPaths::ConfigFile(QStringLiteral("session.ini"));
}

}  // namespace

namespace SessionStore {

void Load()
{
    QString path = FilePath();
    if (path.isEmpty())
        return;
    // 主文件缺失或读不出内容时，退回上一份成功写入的备份。
    // 场景：升级/掉电/文件被杀毒软件清掉，主文件没了但 .bak 还在。
    const auto usable = [](const QString &p) {
        if (p.isEmpty() || !QFileInfo::exists(p))
            return false;
        QSettings probe(p, QSettings::IniFormat);
        return probe.status() == QSettings::NoError && !probe.allKeys().isEmpty();
    };
    if (!usable(path)) {
        const QString bak = path + QStringLiteral(".bak");
        if (usable(bak)) {
            qWarning("session: 主会话文件不可用，改用备份 %s", qPrintable(bak));
            path = bak;
        } else {
            return;
        }
    }
    UserState &u = UserSession::Ref();
    QString avs;
    bool loggedIn = false;
    {
        // QSettings 必须先释放：同一进程里另开一个 QSettings 写同一文件会拿到
        // AccessError（Windows 下实测 status=1），清理残留会话时正是这种情况。
        QSettings s(path, QSettings::IniFormat);
        // 本地偏好与登录态无关：退出登录/会话失效后仍要保留，否则用户会看到
        // "退出登录把屏蔽词和分流名一起清掉了"（这两项只存在 session.ini 里）。
        u.proxy = s.value(QStringLiteral("user/proxy"), u.proxy).toString();
        u.proxyImg = s.value(QStringLiteral("user/proxyImg"), u.proxyImg).toString();
        const QStringList words = s.value(QStringLiteral("user/hideWords")).toStringList();
        if (!words.isEmpty())
            u.hideWordList = words;

        avs = s.value(QStringLiteral("session/avs"), QString()).toString();
        loggedIn = s.value(QStringLiteral("session/loggedIn"), false).toBool();
        if (loggedIn && !avs.isEmpty()) {
            u.loggedIn = true;
            u.name = s.value(QStringLiteral("user/name"), u.name).toString();
            u.title = s.value(QStringLiteral("user/title"), u.title).toString();
            u.coins = s.value(QStringLiteral("user/coins"), u.coins).toInt();
            u.level = s.value(QStringLiteral("user/level"), u.level).toInt();
            u.favorites = s.value(QStringLiteral("user/favorites"), u.favorites).toInt();
            u.favoritesMax = s.value(QStringLiteral("user/favoritesMax"), u.favoritesMax).toInt();
            u.exp = s.value(QStringLiteral("user/exp"), u.exp).toInt();
            u.nextExp = s.value(QStringLiteral("user/nextExp"), u.nextExp).toInt();
            u.avs = avs;
            u.uid = s.value(QStringLiteral("session/uid"), qint64(0)).toLongLong();
            u.signedToday = s.value(QStringLiteral("session/signedToday"), false).toBool();
        }
    }

    if (!loggedIn || avs.isEmpty()) {
        // 没有 AVS 的"已登录"不是真实会话：旧版本的点"演示登录"曾把
        // name/coins/level 写进 session.ini，若照单全收，侧栏会把本地假账号
        // 当成登录态。这里丢弃账号字段并回写一次，把残留数据清干净。
        UserState keep;
        keep.proxy = u.proxy;
        keep.proxyImg = u.proxyImg;
        keep.hideWordList = u.hideWordList;
        u = keep;
        if (loggedIn)
            Save();
    }
}

void Save()
{
    const UserState &u = UserSession::User();
    const QString path = FilePath();
    if (path.isEmpty()) {
        // 不能静默失败：这条日志是"升级后未登录"最直接的线索。
        qWarning("session: 没有可写的配置目录，登录态无法落库（本次运行有效，重启即丢）");
        return;
    }
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QStringLiteral("session/loggedIn"), u.loggedIn);
    s.setValue(QStringLiteral("user/name"), u.name);
    s.setValue(QStringLiteral("user/title"), u.title);
    s.setValue(QStringLiteral("user/coins"), u.coins);
    s.setValue(QStringLiteral("user/level"), u.level);
    s.setValue(QStringLiteral("user/favorites"), u.favorites);
    s.setValue(QStringLiteral("user/favoritesMax"), u.favoritesMax);
    s.setValue(QStringLiteral("user/exp"), u.exp);
    s.setValue(QStringLiteral("user/nextExp"), u.nextExp);
    s.setValue(QStringLiteral("user/proxy"), u.proxy);
    s.setValue(QStringLiteral("user/proxyImg"), u.proxyImg);
    s.setValue(QStringLiteral("user/hideWords"), u.hideWordList);
    s.setValue(QStringLiteral("session/avs"), u.avs);
    s.setValue(QStringLiteral("session/uid"), u.uid);
    s.setValue(QStringLiteral("session/signedToday"), u.signedToday);
    s.sync();
    if (s.status() != QSettings::NoError) {
        qWarning("session: 写入失败 status=%d path=%s", int(s.status()), qPrintable(path));
        return;
    }
    // 写成功后再滚动备份：主文件被删/损坏时还能恢复登录态。
    // 只在写成功时更新，避免把一份坏文件覆盖到好备份上。
    const QString bak = path + QStringLiteral(".bak");
    QFile::remove(bak);
    QFile::copy(path, bak);
}

void Clear()
{
    // 只清账号字段，保留本地偏好（屏蔽词 / 分流展示名），再写一份
    // loggedIn=false 的会话——直接删文件会让 shield words 永久丢失。
    UserState &u = UserSession::Ref();
    UserState keep;
    keep.proxy = u.proxy;
    keep.proxyImg = u.proxyImg;
    keep.hideWordList = u.hideWordList;
    u = keep;
    Save();
}

QString Path()
{
    return FilePath();
}

}  // namespace SessionStore
