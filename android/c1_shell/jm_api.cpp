#include "jm_api.h"

#include "jm_crypto.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrlQuery>

#include <memory>

namespace {

// 与 jmcomic 的 APP_HEADERS_TEMPLATE 一致：服务端会按 UA 决定返回哪套数据。
const char *kUserAgent =
    "Mozilla/5.0 (Linux; Android 9; V1938CT Build/PQ3A.190705.11211812; wv) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/91.0.4472.114 Safari/537.36";

QString JStr(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(static_cast<qint64>(v.toDouble()));
    return QString();
}

QStringList JStrList(const QJsonObject &o, const char *key)
{
    QStringList out;
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isArray()) {
        for (const QJsonValue &item : v.toArray()) {
            if (item.isString())
                out << item.toString();
        }
    } else if (v.isString()) {
        // photo 的 tags 是空格分隔的一整串（album 是数组），两种都要吃
        const QString text = v.toString().trimmed();
        if (!text.isEmpty())
            out = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
    return out;
}

QString JAuthor(const QJsonObject &o)
{
    const QJsonValue v = o.value(QLatin1String("author"));
    if (v.isArray()) {
        QStringList names;
        for (const QJsonValue &item : v.toArray()) {
            if (item.isString())
                names << item.toString();
        }
        return names.join(QStringLiteral(", "));
    }
    return v.toString();
}

QVector<JmChapter> JSeries(const QJsonObject &o)
{
    QVector<JmChapter> out;
    for (const QJsonValue &v : o.value(QLatin1String("series")).toArray()) {
        const QJsonObject c = v.toObject();
        JmChapter ch;
        ch.id = JStr(c, "id");
        ch.name = JStr(c, "name");
        ch.sort = JStr(c, "sort");
        if (!ch.id.isEmpty())
            out.append(ch);
    }
    return out;
}

JmComicItem JItem(const QJsonObject &o)
{
    JmComicItem item;
    item.id = JStr(o, "id");
    item.name = JStr(o, "name");
    item.author = JAuthor(o);
    item.cover = JStr(o, "image");
    const QJsonObject cat = o.value(QLatin1String("category")).toObject();
    item.category = JStr(cat, "title");
    return item;
}

}  // namespace

JmApi::JmApi(NetClient *net, QObject *parent)
    : QObject(parent), net_(net), domains_(DefaultDomains()), imageDomains_(DefaultImageDomains())
{
    connect(net_, &NetClient::Finished, this, [this](const NetResult &r) {
        const Handler h = handler_;
        handler_ = nullptr;
        if (h)
            h(r, QString());
        pumping_ = false;
        Pump();
    });
}

QStringList JmApi::DefaultDomains()
{
    return {QStringLiteral("www.cdnhjk.net"), QStringLiteral("www.cdngwc.cc"),
            QStringLiteral("www.cdngwc.net"), QStringLiteral("www.cdngwc.club")};
}

QStringList JmApi::DefaultImageDomains()
{
    return {QStringLiteral("cdn-msp.jmapiproxy1.cc"), QStringLiteral("cdn-msp.jmapiproxy2.cc"),
            QStringLiteral("cdn-msp2.jmapiproxy2.cc")};
}

void JmApi::SetDomains(const QStringList &domains)
{
    if (domains.isEmpty())
        return;
    domains_ = domains;
    index_ = 0;
}

void JmApi::RotateImageDomain()
{
    if (imageDomains_.size() <= 1)
        return;
    imageIndex_ = (imageIndex_ + 1) % imageDomains_.size();
    emit Log(QStringLiteral("image domain -> %1").arg(ImageDomain()));
}

QString JmApi::Ts() const
{
    return QString::number(QDateTime::currentSecsSinceEpoch());
}

void JmApi::Enqueue(const Job &job)
{
    jobs_.append(job);
    Pump();
}

void JmApi::Pump()
{
    if (pumping_ || jobs_.isEmpty() || net_->IsBusy())
        return;
    pumping_ = true;
    const Job job = jobs_.takeFirst();
    job();
}

void JmApi::RotateDomain(const QString &why)
{
    if (domains_.size() <= 1)
        return;
    index_ = (index_ + 1) % domains_.size();
    emit Log(QStringLiteral("domain -> %1 (%2)").arg(Domain(), why));
}

void JmApi::Request(const QString &path, const QVector<QPair<QString, QString>> &query,
                    const Handler &handler, bool post, const QByteArray &body,
                    const QString &secret, qint64 maxBytes)
{
    const QString sec = secret.isEmpty() ? QString::fromLatin1(JmCrypto::kTokenSecret) : secret;
    // 域名轮换重发：每次尝试都重新取 ts（token 与 AES key 都绑 ts），
    // 所以 handler 必须收到"这一次"的 ts，而不是最初那个。
    auto left = std::make_shared<int>(domains_.size());
    auto send = std::make_shared<std::function<void()>>();

    *send = [this, path, query, post, body, sec, maxBytes, handler, left, send]() {
        const QString ts = Ts();
        QUrl url(QStringLiteral("https://") + Domain() + path);
        QUrlQuery q;
        for (const auto &kv : query)
            q.addQueryItem(kv.first, kv.second);
        if (!q.isEmpty())
            url.setQuery(q);

        NetRequest req;
        req.method = post ? QStringLiteral("POST") : QStringLiteral("GET");
        req.url = url;
        // 40s 总预算(20s/次)：模拟器/弱网下 7.5s/次会把"头到了体没到"的请求掐成假超时
        req.timeoutMs = 40000;
        req.retries = 1;
        req.maxBytes = maxBytes;
        req.headers.insert(QStringLiteral("User-Agent"), QString::fromLatin1(kUserAgent));
        req.headers.insert(QStringLiteral("token"), JmCrypto::Token(ts, sec));
        req.headers.insert(QStringLiteral("tokenparam"), JmCrypto::TokenParam(ts, appVersion_));
        if (!avs_.isEmpty())
            req.headers.insert(QStringLiteral("Cookie"),
                               QStringLiteral("AVS=%1").arg(QString::fromLatin1(avs_)));
        if (post) {
            req.headers.insert(QStringLiteral("Content-Type"),
                               QStringLiteral("application/x-www-form-urlencoded"));
            req.body = body;
        }

        handler_ = [this, ts, path, handler, left, send](const NetResult &r, const QString &) {
            // 只在"网络层没连上"时换域名：4xx/5xx 说明服务端已经应答，换域名只会更慢。
            if (!r.ok && *left > 1) {
                --*left;
                const QString before = Domain();
                RotateDomain(path + QLatin1Char(' ') + r.errorCode);
                if (Domain() != before) {
                    (*send)();
                    return;
                }
            }
            handler(r, ts);
        };

        emit Log(QStringLiteral("api %1 %2").arg(req.method, url.toString(QUrl::RemoveQuery)));
        if (!net_->Send(req)) {
            // busy 只可能来自"同一帧里有第二个调用"；这种请求必须显式失败，
            // 不能静默丢掉（否则 UI 永远转圈，且日志里什么都看不到）。
            NetResult r;
            r.errorCode = QStringLiteral("Busy");
            r.errorText = net_->GetLastError();
            const Handler h = handler_;
            handler_ = nullptr;
            if (h)
                h(r, ts);
        }
    };
    (*send)();
}

QByteArray JmApi::Unwrap(const NetResult &r, const QString &ts, QString *error) const
{
    if (!r.ok) {
        if (error)
            *error = r.errorText.isEmpty() ? r.errorCode : r.errorText;
        return QByteArray();
    }
    if (r.httpError) {
        if (error)
            *error = QStringLiteral("http %1").arg(r.httpStatus);
        return QByteArray();
    }

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r.body, &perr);
    if (!doc.isObject() && perr.error != QJsonParseError::NoError) {
        // /setting 的真实响应会带前后杂字节（Python 版用 try_parse_json_object 兼容），
        // 这里退一步：取第一个 '{' 到最后一个 '}' 之间的片段再解一次。
        const int start = r.body.indexOf('{');
        const int end = r.body.lastIndexOf('}');
        if (start >= 0 && end > start)
            doc = QJsonDocument::fromJson(r.body.mid(start, end - start + 1), &perr);
    }
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("bad envelope: %1").arg(perr.errorString());
        return QByteArray();
    }
    const QJsonObject obj = doc.object();
    const int code = obj.value(QLatin1String("code")).toInt(-1);
    if (code != 200) {
        if (error)
            *error = QStringLiteral("code %1").arg(code);
        return QByteArray();
    }
    const QString data = obj.value(QLatin1String("data")).toString();
    if (data.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty data");
        return QByteArray();
    }
    const QByteArray plain = JmCrypto::DecodeRespData(data.toLatin1(), ts);
    if (plain.isEmpty()) {
        if (error)
            *error = QStringLiteral("decrypt failed");
        return QByteArray();
    }
    return plain;
}

void JmApi::FetchSetting()
{
    Enqueue([this] {
        Request(QStringLiteral("/setting"), {}, [this](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit SettingReady(false, QString(), err);
                return;
            }
            const QJsonObject o = QJsonDocument::fromJson(plain).object();
            const QString ver = JStr(o, "jm3_version");
            if (!ver.isEmpty())
                appVersion_ = ver;
            emit Log(QStringLiteral("setting ok, jm3_version=%1").arg(appVersion_));
            emit SettingReady(true, appVersion_, QString());
        });
    });
}

void JmApi::Search(const QString &keyword, int page, const QString &orderBy)
{
    Enqueue([this, keyword, page, orderBy] {
        const QVector<QPair<QString, QString>> q = {
            {QStringLiteral("main_tag"), QStringLiteral("0")},
            {QStringLiteral("search_query"), keyword},
            {QStringLiteral("page"), QString::number(page)},
            {QStringLiteral("o"), orderBy},
            {QStringLiteral("t"), QStringLiteral("a")},
        };
        Request(QStringLiteral("/search"), q, [this, keyword, page](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit SearchReady(false, {}, 0, page, err);
                return;
            }
            const QJsonObject o = QJsonDocument::fromJson(plain).object();
            QVector<JmComicItem> items;
            for (const QJsonValue &v : o.value(QLatin1String("content")).toArray())
                items.append(JItem(v.toObject()));
            const int total = o.value(QLatin1String("total")).toInt();
            emit Log(QStringLiteral("search '%1' page=%2 hits=%3 total=%4")
                         .arg(keyword).arg(page).arg(items.size()).arg(total));
            emit SearchReady(true, items, total, page, QString());
        });
    });
}

void JmApi::FetchAlbum(const QString &albumId)
{
    Enqueue([this, albumId] {
        Request(QStringLiteral("/album"), {{QStringLiteral("id"), albumId}},
                [this, albumId](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        emit AlbumReady(false, {}, err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    JmAlbum album;
                    album.id = JStr(o, "id");
                    album.name = JStr(o, "name");
                    album.author = JAuthor(o);
                    album.description = JStr(o, "description");
                    album.tags = JStrList(o, "tags");
                    album.chapters = JSeries(o);
                    const QStringList images = JStrList(o, "images");
                    if (!images.isEmpty() && !album.id.isEmpty()) {
                        const QString suffix = JmCrypto::SuffixOf(images.first());
                        album.cover = QStringLiteral("https://%1/media/photos/%2/00001%3")
                                          .arg(ImageDomain(), album.id,
                                               suffix.isEmpty() ? QStringLiteral(".jpg") : suffix);
                    }
                    emit Log(QStringLiteral("album %1 '%2' chapters=%3")
                                 .arg(album.id, album.name).arg(album.chapters.size()));
                    emit AlbumReady(true, album, QString());
                });
    });
}

void JmApi::FetchChapter(const QString &chapterId)
{
    Enqueue([this, chapterId] {
        Request(QStringLiteral("/chapter"), {{QStringLiteral("id"), chapterId}},
                [this, chapterId](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        emit ChapterReady(false, {}, err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    JmChapterDetail ch;
                    ch.id = JStr(o, "id");
                    ch.name = JStr(o, "name");
                    ch.siblings = JSeries(o);
                    const QStringList names = JStrList(o, "images");
                    QString suffix = names.isEmpty() ? QString() : JmCrypto::SuffixOf(names.first());
                    if (suffix.isEmpty())
                        suffix = QStringLiteral(".jpg");

                    // 图片 URL 由 章节 id + 序号拼；域名是 CDN 表，不是 API 域名。
                    const QString photoId = ch.id.isEmpty() ? chapterId : ch.id;
                    for (int i = 1; i <= names.size(); ++i) {
                        ch.images << QStringLiteral("https://%1/media/photos/%2/%3%4")
                                         .arg(ImageDomain(), photoId,
                                              QString::number(i).rightJustified(5, QLatin1Char('0')),
                                              suffix);
                    }
                    emit Log(QStringLiteral("chapter %1 pages=%2 siblings=%3")
                                 .arg(photoId).arg(ch.images.size()).arg(ch.siblings.size()));

                    // 接着取 scramble_id：拿不到就按 220980 兜底（jmcomic 的默认行为），
                    // 但必须把"兜底"写进日志，否则图错位时无从判断。
                    Request(QStringLiteral("/chapter_view_template"),
                            {{QStringLiteral("id"), photoId},
                             {QStringLiteral("mode"), QStringLiteral("vertical")},
                             {QStringLiteral("page"), QStringLiteral("0")},
                             {QStringLiteral("app_img_shunt"), QStringLiteral("1")},
                             {QStringLiteral("express"), QStringLiteral("off")},
                             {QStringLiteral("v"), Ts()}},
                            [this, ch, photoId](const NetResult &r2, const QString &) mutable {
                                QString scramble = QStringLiteral("220980");
                                const QString text = QString::fromUtf8(r2.body);
                                const QRegularExpression re(
                                    QStringLiteral("var\\s+scramble_id\\s*=\\s*(\\d+)"));
                                const auto m = re.match(text);
                                if (m.hasMatch()) {
                                    scramble = m.captured(1);
                                } else {
                                    emit Log(QStringLiteral("scramble missing for %1; fallback 220980")
                                                 .arg(photoId));
                                }
                                ch.scrambleId = scramble;
                                emit ChapterReady(true, ch, QString());
                            },
                            false, QByteArray(), QString::fromLatin1(JmCrypto::kTokenSecretView));
                });
    });
}

void JmApi::Login(const QString &username, const QString &password)
{
    Enqueue([this, username, password] {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("username"), username);
        form.addQueryItem(QStringLiteral("password"), password);
        const QByteArray body = form.toString(QUrl::FullyEncoded).toLatin1();
        Request(QStringLiteral("/login"), {}, [this, username](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit LoginReady(false, QString(), err);
                return;
            }
            const QJsonObject o = QJsonDocument::fromJson(plain).object();
            const QString name = JStr(o, "username").isEmpty() ? username : JStr(o, "username");
            avs_ = JStr(o, "s").toLatin1();
            userName_ = name;
            emit Log(QStringLiteral("login ok user=%1 avs=%2").arg(name, avs_.isEmpty() ? "no" : "yes"));
            emit LoginReady(true, name, QString());
        }, true, body);
    });
}

void JmApi::FavoriteFolder(int page)
{
    Enqueue([this, page] {
        Request(QStringLiteral("/favorite"),
                {{QStringLiteral("page"), QString::number(page)},
                 {QStringLiteral("o"), QStringLiteral("mr")},
                 {QStringLiteral("folder_id"), QStringLiteral("0")}},
                [this, page](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        emit FavoriteReady(false, {}, 0, err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    QVector<JmComicItem> items;
                    for (const QJsonValue &v : o.value(QLatin1String("list")).toArray())
                        items.append(JItem(v.toObject()));
                    const int total = o.value(QLatin1String("total")).toInt();
                    emit Log(QStringLiteral("favorite page=%1 items=%2").arg(page).arg(items.size()));
                    emit FavoriteReady(true, items, total, QString());
                });
    });
}

void JmApi::FavoriteAdd(const QString &albumId)
{
    Enqueue([this, albumId] {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("aid"), albumId);
        Request(QStringLiteral("/favorite"), {}, [this, albumId](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit FavoriteReady(false, {}, 0, err);
                return;
            }
            emit Log(QStringLiteral("favorite add %1: %2")
                         .arg(albumId, QString::fromUtf8(plain.left(120))));
            emit FavoriteReady(true, {}, 0, QString());
        }, true, form.toString(QUrl::FullyEncoded).toLatin1());
    });
}
