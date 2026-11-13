// UI 数据模型：与 Python 版 view/ 层用到的字段对齐（只保留界面需要的部分）。
//
// 说明：这些结构是"界面契约"，不是网络层模型。所有展示数据只能由 JmApi 的
// 服务端接口（/promote、/latest、/search、/album、/chapter、/favorite、/login、
// /daily）或本地真实扫描结果填充；数据未到达时界面显示加载态，取不到就显示
// 空态/错误态。这里不再提供任何本地样例、占位条目或演示回退。
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct Comic {
    QString id;
    QString title;
    QString author;
    QStringList tags;
    QString status;      // 连载中 / 已完结
    QString desc;
    // 章节数。0 = **未知**（列表接口 /latest、/search、/promote、/favorite 都不返回
    // 章节总数），不是"这本书有 0 话"。唯一权威来源是 /album 的 series，
    // 拿到后由 ComicFacts 记住并按 id 刷新已显示的卡片。
    int chapters = 0;
    int readChapter = 0; // 0 表示未读
    double rating = 0.0;
    bool favorite = false;
    QString updated;     // "3 小时前"
    QString coverUrl;    // http(s):// 或本地路径；空则显示封面占位图
};

// ---------------------------------------------------------------- 作品事实缓存
// 列表接口不返回章节数，只有 /album 才有。用户在详情页打开过一本之后，
// 把真实章节数记在这里：列表卡片按 id 取真实值，回到首页/搜索/收藏即可看到
// "作者 · N 话"，而不是把"未知"渲染成"0 话"。
//
// 刻意只放内存、不落盘：连载作品的话数会随更新变化，持久化旧值会让卡片长期
// 显示过期数字（比"未知"更误导）。每次打开详情都会用 /album 的最新结果覆盖。
namespace ComicFacts {

inline QHash<QString, int> &ChapterMap()
{
    static QHash<QString, int> m;
    return m;
}

inline int Chapters(const QString &id)
{
    return id.isEmpty() ? 0 : ChapterMap().value(id, 0);
}

inline void RememberChapters(const QString &id, int count)
{
    if (id.isEmpty() || count <= 0)
        return;
    ChapterMap().insert(id, count);
}

}  // namespace ComicFacts

// 卡片副标题（封面下方第二行）：作者 + 话数。
// 话数未知时只显示作者——绝不把 chapters==0 拼成"0 话"。
// QML delegate 与 C++ CardPaint 必须共用这一份文案，否则两条渲染路径会不一致。
inline QString ComicMetaText(const Comic &c)
{
    const QString author = c.author.trimmed();
    if (c.chapters > 0) {
        return author.isEmpty() ? QStringLiteral("%1 话").arg(c.chapters)
                                : QStringLiteral("%1 · %2 话").arg(author).arg(c.chapters);
    }
    return author;
}

struct Chapter {
    QString id;
    QString name;
    QString date;
    bool read = false;
};

// 内置支持的超分模型目录已迁到 SrEngine::Catalog()（sr_engine.h）：
// 那份表与 android/sr_qnn/models/models.txt 一一对应，并且会按磁盘/APK 里的
// 实际文件填 installed，界面只列真正能跑起来的模型。这里不再重复定义一份
// "三个模型"的假目录。

// ---------------------------------------------------------------- 首页分区
// 与 Python 版首页一致：
//   * "最新上传"来自 /latest（GetLatestInfo），支持加载更多；
//   * 其余分区来自 /promote 的每个区块（title 取接口字段），一次性给全。
enum class HomeSectionKind {
    Latest,   // 最新上传：/latest，支持滚到底/按钮加载下一页
    Promo,    // 推荐区块：/promote 一次性给全
};

struct HomeSection {
    QString title;
    HomeSectionKind kind = HomeSectionKind::Promo;
    QVector<Comic> comics;
};

// ---------------------------------------------------------------- 用户
// 账号状态：只由 /login 的响应、session.ini 的缓存、/daily 的签到状态写入。
// 没有任何默认账号值——未登录时字段全为空/0，界面显示"未登录"。
struct UserState {
    bool loggedIn = false;
    QString name;
    QString title;
    int coins = 0;
    int level = 0;
    int favorites = 0;
    // 服务端资料的其余字段：等级/收藏数两行用 tooltip 展示，不占横向空间
    int favoritesMax = 0;
    int exp = 0;
    int nextExp = 0;
    QString proxy = QStringLiteral("自动");
    QString proxyImg = QStringLiteral("自动");
    QStringList hideWordList;   // 对应 Python 的 Setting.ForbidWords
    // 真实登录拿到的 AVS cookie（JmApi 后续请求要带）
    QString avs;
    // 真实登录返回的 uid：/daily 与 /daily_chk 的必填参数（Python: user.uid）
    qint64 uid = 0;
    // 今天是否已打卡（服务端 /daily 的 signed 字段；仅用于按钮文案）
    bool signedToday = false;
};

namespace UserSession {

inline UserState &Ref()
{
    static UserState u;
    return u;
}

inline UserState User() { return Ref(); }

}  // namespace UserSession
