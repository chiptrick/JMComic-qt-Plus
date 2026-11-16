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

// /promote 的一个区块（首页除"最新上传"外的每个 tab）
struct JmHomeBlock
{
    QString id;
    QString title;
    QString type;        // novels / library 等，UI 侧会过滤
    QString slug;
    QString filterVal;
    QVector<JmComicItem> items;
};

struct JmChapter
{
    QString id;
    QString name;
    QString sort;
};

// /forum（mode=undefined & uid=<uid>）返回的一条评论。
// 字段对应 Python ToolUtil.ParseBookComment：CID/UID/username/content/likes/addtime
// 以及外层的 expinfo.level_name、关联作品 name/AID。回复（replys）这里不收：
// "我的评论"列表只展示本人发言，回复属于详情页评论区的职责。
struct JmComment
{
    QString id;        // CID
    qint64 uid = 0;
    QString user;      // username
    QString levelName; // expinfo.level_name
    int level = 0;
    QString content;
    int likes = 0;
    qint64 addTime = 0;
    QString albumId;   // AID：点评论可回到对应作品
    QString albumName; // name
};

// /login 返回的账号资料。字段与 Python 版 ToolUtil.ParseLogin2 一一对应：
//   uid / username / level_name / level / coin / album_favorites /
//   album_favorites_max / exp / nextLevelExp
// 侧栏"J Coins / 等级 / 收藏数"与登录页状态行都只认这一份数据——之前只取了
// username/uid，把其余字段丢掉，界面就只能硬编码 0（用户报的"登录后没有账号数据"）。
struct JmUserInfo
{
    qint64 uid = 0;
    QString name;         // username
    QString levelName;    // level_name（服务端称号）
    int level = 0;
    int coins = 0;        // coin
    int favorites = 0;    // album_favorites
    int favoritesMax = 0; // album_favorites_max
    int exp = 0;
    int nextExp = 0;      // nextLevelExp
    // 响应里是否真的带账号字段：老账号/异常响应可能只有 s+username，
    // 此时 UI 保留缓存值，不能拿 0 覆盖（否则又变成"数据全是 0"）。
    bool hasProfile = false;
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
    // /album 的 total_photos：单章节作品的页数只在这里有，series 是空数组。
    // 仅用于日志与"是否真的是空本子"的判定，不参与界面排版。
    int totalPhotos = 0;
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

    // 分流线路：MainWindow 按 ProxyConfig 的"接口线路/图床线路"设置进来。
    void SetImageDomains(const QStringList &domains);

    // 节点 IP 覆盖（等价 curl --resolve）：仅当接口线路是 CDN 线路时由外壳打开。
    void SetNodeOverride(const QString &ip, bool enabled);
    // US 反代线路（Python server.py __DealHeaders）：
    // https://<原 host>/<path> → https://<proxyHost>/<原 host>/<path>
    // 传空 proxyHost 关闭。反代与节点覆盖互斥，由 ProxyConfig::ResolveRoute 裁决。
    void SetProxyRoute(const QString &proxyHost, const QString &originHost);

    // 一次只跑一个；重复调用会排队。
    void FetchSetting();
    void Search(const QString &keyword, int page = 1, const QString &orderBy = QStringLiteral("mr"));
    void FetchAlbum(const QString &albumId);
    void FetchChapter(const QString &chapterId);
    void Login(const QString &username, const QString &password);
    void FavoriteFolder(int page = 1);
    void FavoriteAdd(const QString &albumId);

    // ---- 云端记录 / 我的评论（与 Python GetHistoryReq2 / GetMyCommentReq2 对齐）----
    // GET /watch_list?page=N        账号的跨设备观看记录，匿名访问返回 code=401
    // GET /forum?mode=undefined&uid=<uid>&page=N&lang=cn  本人评论
    // 两者响应体形状与 /favorite 同源（data 段解密后是 {list,total}），因此共用
    // 同一套条目解析；401 走与收藏一致的"会话失效"收口。
    void FetchWatchList(int page = 1);
    void FetchMyComments(int page = 1);
    // GET /serialization?type=<all|manga|hanman>&date=<0..7>&page=N&lang=cn
    // date=0 表示"全部"，1~7 = 周一~周日（与 Python index_view 的 weekBox 同义）。
    // 响应形状同样是 {list,total}，与观看记录共用解析。
    void FetchSerialization(int day, const QString &type, int page = 1);
    // 作品评论：GET /forum?mode=manhua&aid=<id>&page&lang ；POST /comment (aid, comment, comment_id)
    // 列表形状与 /watch_list 同源（{list,total}），评论字段由 ParseCommentListPayload 解析。
    void FetchComments(const QString &albumId, int page = 1);
    void SendComment(const QString &albumId, const QString &text, const QString &replyTo = QString());
    // 回归/单测：解密后的明文 -> 列表（不发网络请求）
    static QVector<JmComicItem> ParseItemListPayload(const QByteArray &plain, int *total,
                                                     bool *ok);
    static QVector<JmComment> ParseCommentListPayload(const QByteArray &plain, int *total,
                                                      bool *ok);
    // 回归/单测：把 /login 解密后的明文解析成账号资料（不发网络请求）。
    static JmUserInfo ParseLoginPayload(const QByteArray &plain);
    // 回归/单测：/album 与 /chapter 解密后的明文 -> 界面模型（不发网络请求）。
    // albumId / chapterId 是请求参数，用于响应缺 id 时回填，保证"切作品/切章"的
    // 迟到响应判定有依据；imageDomain 用于拼封面与页图的 CDN 前缀。
    static JmAlbum ParseAlbumPayload(const QByteArray &plain, const QString &albumId,
                                     const QString &imageDomain);
    static JmChapterDetail ParseChapterPayload(const QByteArray &plain, const QString &chapterId,
                                               const QString &imageDomain);

    // ---- 每日签到（打卡）----
    // GET  /daily?user_id=<uid>   取签到信息（daily_id + 本月已签记录）
    // POST /daily_chk             body: user_id & daily_id
    // 与 Python 版 src/server/req.py 的 GetDailyReq2 / SignDailyReq2 一致。
    void FetchDailySign();
    void SignDaily();
    bool SignedToday() const { return signedToday_; }
    qint64 Uid() const { return uid_; }
    // 会话恢复：AVS 与 uid 一起落库，冷启动后打卡仍可用
    void SetUid(qint64 uid) { uid_ = uid; }

    // 首页：/promote 返回全部推荐区块；/latest 返回"最新上传"。
    // 两者都是 data 段解密后的 JSON 数组（见 Python ToolUtil.ParseIndex2/ParseLatest2）。
    void FetchPromote(int page = 0);
    void FetchLatestPage(int page = 0);

    QString AppVersion() const { return appVersion_; }
    QString UserName() const { return userName_; }
    bool LoggedIn() const { return !avs_.isEmpty(); }
    QByteArray Avs() const { return avs_; }

    // 启动时恢复上次登录：AVS 来自 session.ini，之后所有请求自动带 Cookie。
    void RestoreSession(const QByteArray &avs, const QString &userName);

signals:
    void SettingReady(bool ok, const QString &version, const QString &error);
    void SearchReady(bool ok, const QVector<JmComicItem> &items, int total, int page,
                     const QString &error);
    void AlbumReady(bool ok, const JmAlbum &album, const QString &error);
    void ChapterReady(bool ok, const JmChapterDetail &chapter, const QString &error);
    void LoginReady(bool ok, const JmUserInfo &user, const QString &error);
    void FavoriteReady(bool ok, const QVector<JmComicItem> &items, int total, const QString &error);
    // 加/取消收藏：与列表分开，避免"谁在飞"只能靠一个 int 猜（列表请求把
    // 加收藏的响应当成自己的，刷新收藏页时会出现空列表/错提示）。
    void FavoriteAddReady(bool ok, const QString &error);
    // 观看记录：page 原样带回，调用方据此丢弃过期响应（快速切页会并发两页）
    void WatchListReady(bool ok, const QVector<JmComicItem> &items, int total, int page,
                        const QString &error);
    void MyCommentsReady(bool ok, const QVector<JmComment> &items, int total, int page,
                         const QString &error);
    void SerializationReady(bool ok, const QVector<JmComicItem> &items, int total, int page,
                            const QString &error);
    void CommentsReady(bool ok, const QVector<JmComment> &items, int total, int page,
                       const QString &albumId, const QString &error);
    void CommentSendReady(bool ok, const QString &message, const QString &error);
    // 会话失效（服务端 code 401）：AVS 已不可用，外壳必须清登录态并提示重新登录。
    // 实测 /favorite 匿名访问返回 {"code":401,"errorMsg":"請先登入會員"}。
    void SessionExpired(const QString &message);
    // 签到信息：signedToday 为"今天已打卡"，dailyId 为服务端下发的签到活动 id
    void DailyReady(bool ok, bool signedToday, int dailyId, const QString &message,
                    const QString &error);
    // 打卡结果：ok 表示签到成功；message 用服务端文案（"今天已经签到"也算失败文案）
    void SignReady(bool ok, const QString &message, const QString &error);
    void PromoteReady(bool ok, const QVector<JmHomeBlock> &blocks, const QString &error);
    void LatestReady(bool ok, const QVector<JmComicItem> &items, int page, const QString &error);
    void Log(const QString &line);

private:
    using Job = std::function<void()>;
    // 第二个参数是本次尝试实际使用的 ts：域名轮换后 ts 会变，解密必须用对应的那个，
    // 否则解出来是乱码（这个坑在 Python 版里表现为"偶发解密失败"）。
    using Handler = std::function<void(const NetResult &, const QString &ts)>;

    void Enqueue(const Job &job);
    // 登录/收藏这类"用户正在等"的请求插队：首页拉取可能正占着单通道，
    // 不插队的话点登录后要等前面 20~40s 的请求跑完（真机实测）。
    void EnqueueFront(const Job &job);
    void Pump();
    void Request(const QString &path, const QVector<QPair<QString, QString>> &query,
                 const Handler &handler, bool post = false, const QByteArray &body = QByteArray(),
                 const QString &secret = QString(), qint64 maxBytes = 512 * 1024);
    void RotateDomain(const QString &why);
    QString Ts() const;
    QByteArray Unwrap(const NetResult &r, const QString &ts, QString *error,
                      int *code = nullptr) const;

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
    // ---- 签到 ----
    qint64 uid_ = 0;
    int dailyId_ = 0;
    bool signedToday_ = false;
    bool pendingSign_ = false;    // 拿到 daily_id 后自动补发一次 /daily_chk
    QString nodeIp_;          // 空 = 不覆盖
    bool nodeOverride_ = false;
    QString proxyHost_;       // US 反代域名，空 = 不改写
    QString proxyOrigin_;     // 被反代的原始 host
};
