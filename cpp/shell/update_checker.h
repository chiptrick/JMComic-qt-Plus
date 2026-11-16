// 检查更新 + 更新配置同步（对齐 Python src/server/req.py 的
// CheckUpdateReq / CheckUpdateInfoReq / CheckUpdateConfigReq 与
// src/view/help/help_view.py 的判定逻辑）。
//
// 协议（实测 2026-11-13）：
//   1. GET <base>/version.txt?version=<本地版本>&platform=<平台>
//      -> 纯文本，形如 "v1.3.7"；与本地比较后决定是否有更新。
//   2. 有新版本时 GET <base>/<新版本>.txt?version=&platform=  -> 更新日志正文。
//   3. GET <base>/config.txt?version=&platform= -> key=value 多行文本：
//        Ver / VerTime / UrlList（Web 域名） / Url2List（API 域名）
//        PicUrlList（图床） / ProxyApiUrl / CdnApiUrl / HeaderVer
//
// 版本比较口径与 Python 完全一致：把点分段转成整数权重
//   a*1000 + b*100 + c*10 + d；非预发布模式再 //10（等价只比前三段）。
//
// 关于"本地版本"：C++ UI 自身的构建号（0.9x，见 CHANGELOG 的 cpp-ui 段）与
// 上游发布线（v1.3.x）是两条不同的线，拿 0.94 去比 v1.3.7 只会永远报"有更新"。
// 所以这里显式区分：
//   * ApplicationVersion() —— 界面/关于页显示的构建号（QCoreApplication 的 0.9x）；
//   * ReleaseLine()        —— 参与更新比较的发行线，默认 "v1.3.7"（本移植对齐的
//                             Python 版本），可用 ui.ini [update] version 或
//                             JM_APP_VERSION 覆盖。移植线追上后只需改这一个常量。
//
// 端点可用 JM_UPDATE_BASE（逗号分隔，覆盖整张 base 表）指向本地 mock 服务，
// 用于离线回归；默认三个 base 与 Python config.AppUrl/AppUrl2/AppUrl3 一致。
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class NodeHttp;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    // 服务端下发的配置（只解析，不改动已接入的网络行为）
    struct Config
    {
        int ver = 0;
        QString verTime;
        QString headerVer;
        QStringList webUrls;   // UrlList：注册/找回密码等网页端点用
        QStringList apiUrls;   // Url2List
        QStringList imageUrls; // PicUrlList
        QString proxyApi;
        QString cdnApi;
        bool ok = false;
    };

    explicit UpdateChecker(QObject *parent = nullptr);

    static QString ApplicationVersion();   // "0.94"
    static QString ReleaseLine();          // "v1.3.7"（可覆盖）
    static QStringList DefaultBases();
    QStringList Bases() const { return bases_; }
    void SetBases(const QStringList &bases);

    // 纯函数，供单测/回归直接调用
    static int VersionWeight(const QString &version, bool preRelease = false);
    static bool IsNewer(const QString &remote, const QString &local, bool preRelease = false);
    static Config ParseConfig(const QByteArray &text, bool *ok);
    // 从版本文件正文里取出合法版本号；不是版本号返回空串（Python 侧靠异常吞掉，
    // 这里显式返回空，避免把 HTML 错误页当成版本）
    static QString ParseVersionText(const QByteArray &text);

    void Check();        // version.txt -> CheckFinished（有更新时自动拉 changelog）
    void FetchConfig();  // config.txt  -> ConfigReady

    // 上次成功拿到的配置（进程内缓存；供账号页取 Web 域名）
    const Config &LastConfig() const { return config_; }

signals:
    void CheckFinished(bool ok, bool hasUpdate, const QString &newVersion, const QString &error);
    void ChangelogReady(bool ok, const QString &newVersion, const QString &text,
                        const QString &error);
    void ConfigReady(bool ok, const Config &config, const QString &error);
    void Log(const QString &line);

private:
    void GetText(const QString &path, int timeoutMs,
                 const std::function<void(bool ok, int status, const QByteArray &body,
                                          const QString &error)> &cb);
    void RotateBase(const QString &why);

    NodeHttp *http_ = nullptr;
    QStringList bases_;
    int baseIndex_ = 0;
    Config config_;
    bool checking_ = false;
};
