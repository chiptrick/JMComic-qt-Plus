// C3：禁漫移动端 API 的 C++ 客户端（网络收编）。
//
// 与 Python 版的关系：
//   * 加解密常量/算法照搬 jmcomic 2.7.7（见 jm_crypto.h 头注释的出处）；
//   * 请求头、token 派生、域名轮换、接口路径照搬 src/server/req.py + JmApiClient；
//   * 但**不再需要 Python 子进程**：一次请求只有一次 QNetworkAccessManager 往返。
//
// 设计约束（都是踩过的坑）：
//   * NetClient 是单通道（一次只有一个在飞请求），所以本类内部排队，不做并发；
//   * 域名轮换只在"网络层失败"时发生：4xx/5xx 说明已经连上了，换域名只会更慢；
//   * /chapter_view_template 用的是另一套 secret，且返回 HTML 不是 JSON —— 单独处理；
//   * 解密失败必须显式失败（ok=false），绝不把空字符串当 JSON 往上抛。
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "net_client.h"

struct JmComicItem
{
    QString id;
    QString name;
    QString author;
    QString category;
    QString cover;
};

struct JmChapter
{
    QString id;
    QString name;
    QString sort;
};

struct JmAlbum
{
    QString id;
    QString name;
    QString author;
    QString description;
    QString cover;
    QStringList tags;
    QVector<JmChapter> chapters;
};

struct JmChapterDetail
{
    QString id;
    QString name;
    QString scrambleId;
    QStringList images;          // 完整可下载 URL
    QVector<JmChapter> siblings;  // 同本子的其它章节（接口 series 字段）
};

class JmApi : public QObject
{
    Q_OBJECT
public:
    explicit JmApi(NetClient *net, QObject *parent = nullptr);

    // 默认域名表与 jmcomic 的 DOMAIN_API_LIST 一致（顺序固定，便于日志复现）
    static QStringList DefaultDomains();
    static QStringList DefaultImageDomains();

    QString Domain() const { return domains_.value(index_); }
    void SetDomains(const QStringList &domains);

    // 图片 CDN 与 API 域名是两套：某个 CDN 在特定网络下会连不上（实测
    // cdn-msp.jmapiproxy1.cc 在部分网络直接超时，jmapiproxy2.cc 正常），
    // 所以下载失败时按这张表轮换并重建 URL。
    QString ImageDomain() const { return imageDomains_.value(imageIndex_); }
    void RotateImageDomain();

    // 一次只跑一个；重复调用会排队。
    void FetchSetting();
    void Search(const QString &keyword, int page = 1, const QString &orderBy = QStringLiteral("mr"));
    void FetchAlbum(const QString &albumId);
    void FetchChapter(const QString &chapterId);
    void Login(const QString &username, const QString &password);
    void FavoriteFolder(int page = 1);
    void FavoriteAdd(const QString &albumId);

    QString AppVersion() const { return appVersion_; }
    QString UserName() const { return userName_; }
    bool LoggedIn() const { return !avs_.isEmpty(); }

signals:
    void SettingReady(bool ok, const QString &version, const QString &error);
    void SearchReady(bool ok, const QVector<JmComicItem> &items, int total, int page,
                     const QString &error);
    void AlbumReady(bool ok, const JmAlbum &album, const QString &error);
    void ChapterReady(bool ok, const JmChapterDetail &chapter, const QString &error);
    void LoginReady(bool ok, const QString &userName, const QString &error);
    void FavoriteReady(bool ok, const QVector<JmComicItem> &items, int total, const QString &error);
    void Log(const QString &line);

private:
    using Job = std::function<void()>;
    // 第二个参数是本次尝试实际使用的 ts：域名轮换后 ts 会变，解密必须用对应的那个，
    // 否则解出来是乱码（这个坑在 Python 版里表现为"偶发解密失败"）。
    using Handler = std::function<void(const NetResult &, const QString &ts)>;

    void Enqueue(const Job &job);
    void Pump();
    void Request(const QString &path, const QVector<QPair<QString, QString>> &query,
                 const Handler &handler, bool post = false, const QByteArray &body = QByteArray(),
                 const QString &secret = QString(), qint64 maxBytes = 512 * 1024);
    void RotateDomain(const QString &why);
    QString Ts() const;
    QByteArray Unwrap(const NetResult &r, const QString &ts, QString *error) const;

    NetClient *net_ = nullptr;
    QStringList domains_;
    QStringList imageDomains_;
    int index_ = 0;
    int imageIndex_ = 0;
    QVector<Job> jobs_;
    bool pumping_ = false;
    Handler handler_;
    QString appVersion_ = QStringLiteral("2.1.7");
    QString userName_;
    QByteArray avs_;
};
