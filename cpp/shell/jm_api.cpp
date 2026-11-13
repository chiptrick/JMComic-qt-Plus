#include "jm_api.h"

#include "doh_client.h"
#include "jm_crypto.h"

#include <QDate>
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

qint64 JInt64(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
    if (v.isString())
        return v.toString().toLongLong();
    return 0;
}

// 整数也可能是字符串（不同账号类型的响应不一致），两种都吃。
int JInt(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        return static_cast<int>(v.toDouble());
    if (v.isString())
        return v.toString().toInt();
    return 0;
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

// /chapter 的 images：旧版是文件名数组（"00001.webp"），新版可能是
// {"page":1,"image":"..."} 的对象数组。两种都吃，统一取"末段文件名"。
QStringList JImageNames(const QJsonObject &o)
{
    QStringList out;
    for (const QJsonValue &v : o.value(QLatin1String("images")).toArray()) {
        QString name;
        if (v.isString()) {
            name = v.toString();
        } else if (v.isObject()) {
            const QJsonObject item = v.toObject();
            name = JStr(item, "image");
            if (name.isEmpty())
                name = JStr(item, "name");
        }
        name = name.trimmed();
        if (name.isEmpty())
            continue;
        const int slash = name.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0)
            name = name.mid(slash + 1);
        if (!name.isEmpty())
            out << name;
    }
    return out;
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
    // 列表接口的 image 字段实测全为空，Python 版也是按 id 拼固定命名。
    // 这里统一补齐，UI 只负责加图片 CDN 前缀。
    if (item.cover.isEmpty() && !item.id.isEmpty())
        item.cover = QStringLiteral("media/albums/%1_3x4.jpg").arg(item.id);
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

void JmApi::SetImageDomains(const QStringList &domains)
{
    if (domains.isEmpty())
        return;
    imageDomains_ = domains;
    imageIndex_ = 0;
}

void JmApi::SetProxyRoute(const QString &proxyHost, const QString &originHost)
{
    proxyHost_ = proxyHost.trimmed();
    proxyOrigin_ = originHost.trimmed();
    emit Log(QStringLiteral("proxy route %1")
                 .arg(proxyHost_.isEmpty() ? QStringLiteral("off") : proxyHost_));
}

void JmApi::SetNodeOverride(const QString &ip, bool enabled)
{
    nodeIp_ = ip.trimmed();
    nodeOverride_ = enabled && !nodeIp_.isEmpty();
    emit Log(QStringLiteral("node override %1 (%2)")
                 .arg(nodeOverride_ ? nodeIp_ : QStringLiteral("off"),
                      enabled ? QStringLiteral("cdn line") : QStringLiteral("domain line")));
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

void JmApi::RestoreSession(const QByteArray &avs, const QString &userName)
{
    avs_ = avs;
    userName_ = userName;
    if (!avs_.isEmpty())
        emit Log(QStringLiteral("session restored user=%1").arg(userName_));
}

void JmApi::Enqueue(const Job &job)
{
    jobs_.append(job);
    Pump();
}

void JmApi::EnqueueFront(const Job &job)
{
    jobs_.prepend(job);
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
        const QString apiHost = Domain();
        QUrl url(QStringLiteral("https://") + apiHost + path);
        QUrlQuery q;
        for (const auto &kv : query)
            q.addQueryItem(kv.first, kv.second);
        if (!q.isEmpty())
            url.setQuery(q);
        if (!proxyHost_.isEmpty()) {
            // 与 Python __DealHeaders 一致：host 改成反代域名，原 host 进路径第一段
            const QString origin = proxyOrigin_.isEmpty() ? apiHost : proxyOrigin_;
            url.setHost(proxyHost_);
            url.setPath(QLatin1Char('/') + origin + url.path(QUrl::FullyEncoded));
        }

        NetRequest req;
        req.method = post ? QStringLiteral("POST") : QStringLiteral("GET");
        req.url = url;
        // 24s 总预算(12s/次)：真机上单次 20s 太钝，两次超时就是 40s 干等；
        // 12s 已经覆盖 5s 级的正常响应，超时后尽快换域名更划算。
        req.timeoutMs = 24000;
        req.retries = 1;
        req.maxBytes = maxBytes;
        req.headers.insert(QStringLiteral("User-Agent"), QString::fromLatin1(kUserAgent));
        // 和封面请求保持一致：部分边缘节点对缺 Referer 的请求会直接断连/超时
        // （真机实测同样 curl 2.9s、Qt 连续 20s 超时，加上后明显稳定）。
        req.headers.insert(QStringLiteral("Referer"),
                           QStringLiteral("https://%1/").arg(Domain()));
        req.headers.insert(QStringLiteral("Accept"), QStringLiteral("application/json, */*"));
        if (nodeOverride_) {
            // Python 的 SetCurlOpt(dnsIpList) 同语义：TCP 连接落到节点 IP，
            // TLS 的 SNI / 证书校验名 / Host 仍是接口域名（NetClient 走 NodeHttp）。
            req.connectIp = nodeIp_;
            req.peerName = apiHost;
        } else if (proxyHost_.isEmpty() && DohClient::I().Active()) {
            // DoH 接管域名解析：连接落到 DoH 返回的 IP，SNI/Host 仍是接口域名。
            // 缓存未命中时本次先用系统 DNS，同时异步预热，下次请求命中。
            const QStringList ips = DohClient::I().CachedIps(apiHost);
            if (!ips.isEmpty()) {
                req.connectIp = ips.first();
                req.peerName = apiHost;
            } else {
                DohClient::I().Resolve(apiHost);
            }
        }
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

QByteArray JmApi::Unwrap(const NetResult &r, const QString &ts, QString *error, int *code) const
{
    if (code)
        *code = -1;
    if (!r.ok) {
        if (error)
            *error = r.errorText.isEmpty() ? r.errorCode : r.errorText;
        return QByteArray();
    }
    if (r.httpError) {
        if (error)
            *error = QStringLiteral("http %1").arg(r.httpStatus);
        // 401 有两种形态：CDN/节点线路直接回 HTTP 401（实测 100B 正文），
        // 源站则回 HTTP 200 + {"code":401}。两种都要能被"会话失效"识别。
        if (code && r.httpStatus == 401)
            *code = 401;
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
    const int envelopeCode = obj.value(QLatin1String("code")).toInt(-1);
    if (code)
        *code = envelopeCode;
    if (envelopeCode != 200) {
        if (error)
            *error = QStringLiteral("code %1").arg(envelopeCode);
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

void JmApi::FetchPromote(int page)
{
    Enqueue([this, page] {
        const QVector<QPair<QString, QString>> q = {
            {QStringLiteral("page"), QString::number(page)},
            {QStringLiteral("lang"), QStringLiteral("cn")},
        };
        // 返回体是加密后的 JSON 数组：每个元素是一个首页区块
        // （title / type / slug / filter_val / content[]），见 Python ParseIndex2。
        Request(QStringLiteral("/promote/"), q, [this](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit PromoteReady(false, {}, err);
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(plain);
            if (!doc.isArray()) {
                emit PromoteReady(false, {}, QStringLiteral("promote: not an array"));
                return;
            }
            QVector<JmHomeBlock> blocks;
            for (const QJsonValue &v : doc.array()) {
                const QJsonObject o = v.toObject();
                JmHomeBlock b;
                b.id = JStr(o, "id");
                b.title = JStr(o, "title");
                b.type = JStr(o, "type");
                b.slug = JStr(o, "slug");
                b.filterVal = JStr(o, "filter_val");
                for (const QJsonValue &bv : o.value(QLatin1String("content")).toArray())
                    b.items.append(JItem(bv.toObject()));
                if (!b.title.isEmpty() && !b.items.isEmpty())
                    blocks.append(b);
            }
            emit Log(QStringLiteral("promote ok, blocks=%1").arg(blocks.size()));
            emit PromoteReady(true, blocks, QString());
        }, false, QByteArray(), QString(), 2 * 1024 * 1024);
    });
}

void JmApi::FetchLatestPage(int page)
{
    Enqueue([this, page] {
        const QVector<QPair<QString, QString>> q = {
            {QStringLiteral("page"), QString::number(page)},
            {QStringLiteral("lang"), QStringLiteral("cn")},
        };
        // 首屏 /latest 偶发返回 200 + 空数组（桌面与真机都见过）：首页"最新上传"
        // 整块空掉比多花一次请求糟糕得多，这里换域名重试一次。
        auto retryLeft = std::make_shared<int>(1);
        Request(QStringLiteral("/latest/"), q, [this, page, q, retryLeft](const NetResult &r,
                                                                        const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit LatestReady(false, {}, page, err);
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(plain);
            if (!doc.isArray()) {
                emit LatestReady(false, {}, page, QStringLiteral("latest: not an array"));
                return;
            }
            QVector<JmComicItem> items;
            for (const QJsonValue &v : doc.array())
                items.append(JItem(v.toObject()));
            if (page == 0 && items.isEmpty() && *retryLeft > 0) {
                --*retryLeft;
                const QString before = Domain();
                RotateDomain(QStringLiteral("latest empty"));
                if (Domain() != before) {
                    emit Log(QStringLiteral("latest empty on %1, retry on %2")
                                 .arg(before, Domain()));
                    FetchLatestPage(page);
                    return;
                }
            }
            if (page == 0) {
                QStringList ids;
                for (int i = 0; i < qMin(8, items.size()); ++i)
                    ids << items.at(i).id;
                emit Log(QStringLiteral("latest ids: %1").arg(ids.join(QLatin1Char(','))));
            }
            emit Log(QStringLiteral("latest ok, page=%1 items=%2").arg(page).arg(items.size()));
            emit LatestReady(true, items, page, QString());
        }, false, QByteArray(), QString(), 2 * 1024 * 1024);
    });
}

JmAlbum JmApi::ParseAlbumPayload(const QByteArray &plain, const QString &albumId,
                                 const QString &imageDomain)
{
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    JmAlbum album;
    album.id = JStr(o, "id");
    // 响应没给 id 时用请求参数回填：详情页的"迟到响应"判定与单章节兜底都依赖它，
    // 空 id 会让这两条逻辑同时失效（表现为静默丢弃 / 章节列表为空）。
    if (album.id.isEmpty())
        album.id = albumId;
    album.name = JStr(o, "name");
    album.author = JAuthor(o);
    album.description = JStr(o, "description");
    album.tags = JStrList(o, "tags");
    album.totalPhotos = JInt(o, "total_photos");
    album.chapters = JSeries(o);

    if (album.chapters.isEmpty() && !album.id.isEmpty()) {
        // 单章节作品：整本只有一张 photo，/album 的 series 是空数组
        // （实测 1471982：series=[]、total_photos=13、series_id="0"）。
        // 服务端把作品 id 本身当作 chapter id（/chapter?id=<作品id> 可正常取到页图），
        // 与 Python 版 ParseBookInfo2 的 else 分支同语义。不补这一条，界面只剩
        // "暂无章节"，用户没有任何入口可以读这本。
        JmChapter only;
        only.id = album.id;
        only.sort = QStringLiteral("1");
        album.chapters.append(only);
    }

    // 列表/详情都用同一套封面规格；详情接口不一定给 images，
    // 直接按 album id 拼，避免详情页顶部留空。
    if (!album.id.isEmpty())
        album.cover = QStringLiteral("https://%1/media/albums/%2_3x4.jpg")
                          .arg(imageDomain, album.id);
    const QStringList images = JStrList(o, "images");
    if (!images.isEmpty() && !album.id.isEmpty()) {
        const QString suffix = JmCrypto::SuffixOf(images.first());
        album.cover = QStringLiteral("https://%1/media/photos/%2/00001%3")
                          .arg(imageDomain, album.id,
                               suffix.isEmpty() ? QStringLiteral(".jpg") : suffix);
    }
    return album;
}

void JmApi::FetchAlbum(const QString &albumId)
{
    Enqueue([this, albumId] {
        Request(QStringLiteral("/album"), {{QStringLiteral("id"), albumId}},
                [this, albumId](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        // 失败也带上 album id：外壳据此丢弃"切作品后才到"的错误，
                        // 否则会给当前作品弹一条别人的失败提示。
                        JmAlbum failed;
                        failed.id = albumId;
                        emit AlbumReady(false, failed, err);
                        return;
                    }
                    const JmAlbum album = ParseAlbumPayload(plain, albumId, ImageDomain());
                    emit Log(QStringLiteral("album %1 '%2' chapters=%3 first=%4 photos=%5")
                                 .arg(album.id, album.name)
                                 .arg(album.chapters.size())
                                 .arg(album.chapters.isEmpty() ? QStringLiteral("-")
                                                               : album.chapters.first().id)
                                 .arg(album.totalPhotos));
                    emit AlbumReady(true, album, QString());
                });
    });
}

JmChapterDetail JmApi::ParseChapterPayload(const QByteArray &plain, const QString &chapterId,
                                           const QString &imageDomain)
{
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    JmChapterDetail ch;
    ch.id = JStr(o, "id");
    if (ch.id.isEmpty())
        ch.id = chapterId;
    ch.name = JStr(o, "name");
    ch.siblings = JSeries(o);

    // 页图 URL 必须按服务端给的真实文件名拼，不能假设 00001..0000N 连续。
    // 实测 313833：images 有 27 项，末项是 00028.webp（00027 被下架，编号有洞）。
    // 旧实现按序号生成，第 27 页请求 00027.webp → 502，真正的 00028 永远拿不到。
    const QStringList names = JImageNames(o);
    const QString photoId = ch.id.isEmpty() ? chapterId : ch.id;
    for (const QString &name : names)
        ch.images << QStringLiteral("https://%1/media/photos/%2/%3").arg(imageDomain, photoId, name);
    return ch;
}

void JmApi::FetchChapter(const QString &chapterId)
{
    Enqueue([this, chapterId] {
        Request(QStringLiteral("/chapter"), {{QStringLiteral("id"), chapterId}},
                [this, chapterId](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        // 失败也带 chapter id：PageLoader 靠它丢弃"切章后才到"的旧错误，
                        // 否则旧章的失败会把正在加载的新章直接判死。
                        JmChapterDetail failed;
                        failed.id = chapterId;
                        emit ChapterReady(false, failed, err);
                        return;
                    }
                    JmChapterDetail ch = ParseChapterPayload(plain, chapterId, ImageDomain());
                    if (ch.images.isEmpty()) {
                        // 空 images 是有效响应的"无内容"形态（下架/需购买），
                        // 必须显式失败，不能当成 0 页成功往上抛。
                        emit Log(QStringLiteral("chapter %1 has no images (siblings=%2)")
                                     .arg(ch.id)
                                     .arg(ch.siblings.size()));
                        emit ChapterReady(false, ch, QStringLiteral("该章节没有可读页面"));
                        return;
                    }
                    emit Log(QStringLiteral("chapter %1 pages=%2 siblings=%3")
                                 .arg(ch.id)
                                 .arg(ch.images.size())
                                 .arg(ch.siblings.size()));

                    // 接着取 scramble_id：拿不到就按 220980 兜底（jmcomic 的默认行为），
                    // 但必须把"兜底"写进日志，否则图错位时无从判断。
                    Request(QStringLiteral("/chapter_view_template"),
                            {{QStringLiteral("id"), ch.id},
                             {QStringLiteral("mode"), QStringLiteral("vertical")},
                             {QStringLiteral("page"), QStringLiteral("0")},
                             {QStringLiteral("app_img_shunt"), QStringLiteral("1")},
                             {QStringLiteral("express"), QStringLiteral("off")},
                             {QStringLiteral("v"), Ts()}},
                            [this, ch](const NetResult &r2, const QString &) mutable {
                                QString scramble = QStringLiteral("220980");
                                const QString text = QString::fromUtf8(r2.body);
                                const QRegularExpression re(
                                    QStringLiteral("var\\s+scramble_id\\s*=\\s*(\\d+)"));
                                const auto m = re.match(text);
                                if (m.hasMatch()) {
                                    scramble = m.captured(1);
                                } else {
                                    emit Log(QStringLiteral("scramble missing for %1 (%2); fallback 220980")
                                                 .arg(ch.id,
                                                      r2.ok ? QStringLiteral("no match")
                                                            : r2.errorCode));
                                }
                                ch.scrambleId = scramble;
                                emit ChapterReady(true, ch, QString());
                            },
                            false, QByteArray(), QString::fromLatin1(JmCrypto::kTokenSecretView));
                });
    });
}

JmUserInfo JmApi::ParseLoginPayload(const QByteArray &plain)
{
    JmUserInfo u;
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    if (o.isEmpty())
        return u;
    u.uid = JInt64(o, "uid");
    u.name = JStr(o, "username");
    u.levelName = JStr(o, "level_name");
    u.level = JInt(o, "level");
    u.coins = JInt(o, "coin");
    u.favorites = JInt(o, "album_favorites");
    u.favoritesMax = JInt(o, "album_favorites_max");
    u.exp = JInt(o, "exp");
    u.nextExp = JInt(o, "nextLevelExp");
    // "有资料"的判据：至少拿到 uid 或 username。只拿到 s（会话票据）时视为
    // 服务端没给账号字段，UI 保留缓存值而不是把 0 当真实值写进侧栏。
    u.hasProfile = u.uid > 0 || !u.name.isEmpty();
    return u;
}

void JmApi::Login(const QString &username, const QString &password)
{
    EnqueueFront([this, username, password] {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("username"), username);
        form.addQueryItem(QStringLiteral("password"), password);
        const QByteArray body = form.toString(QUrl::FullyEncoded).toLatin1();
        Request(QStringLiteral("/login"), {}, [this, username](const NetResult &r, const QString &ts) {
            QString err;
            const QByteArray plain = Unwrap(r, ts, &err);
            if (plain.isEmpty()) {
                emit LoginReady(false, JmUserInfo(), err);
                return;
            }
            const QJsonObject o = QJsonDocument::fromJson(plain).object();
            JmUserInfo user = ParseLoginPayload(plain);
            if (user.name.isEmpty())
                user.name = username;
            avs_ = JStr(o, "s").toLatin1();
            userName_ = user.name;
            // uid 是 /daily 与 /daily_chk 的必填参数（Python: user.uid）
            uid_ = user.uid;
            emit Log(QStringLiteral("login ok user=%1 uid=%2 avs=%3 level=%4 coin=%5 fav=%6/%7")
                         .arg(user.name)
                         .arg(uid_)
                         .arg(avs_.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                         .arg(user.level)
                         .arg(user.coins)
                         .arg(user.favorites)
                         .arg(user.favoritesMax));
            emit LoginReady(true, user, QString());
        }, true, body);
    });
}

void JmApi::FetchDailySign()
{
    if (avs_.isEmpty() || uid_ == 0) {
        emit DailyReady(false, false, 0, QString(),
                        QStringLiteral("未登录或缺少用户 id"));
        return;
    }
    Enqueue([this] {
        Request(QStringLiteral("/daily"),
                {{QStringLiteral("user_id"), QString::number(uid_)}},
                [this](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        emit Log(QStringLiteral("daily fail: %1").arg(err));
                        emit DailyReady(false, false, 0, QString(), err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    dailyId_ = o.value(QLatin1String("daily_id")).toInt();
                    const int today = QDate::currentDate().day();
                    bool signedNow = false;
                    // record 是"周 -> [ {date, signed} ]"的二维数组（见 Python GetSignDailyBack）
                    for (const QJsonValue &week : o.value(QLatin1String("record")).toArray()) {
                        for (const QJsonValue &item : week.toArray()) {
                            const QJsonObject rec = item.toObject();
                            const QJsonValue dv = rec.value(QLatin1String("date"));
                            const int day = dv.isString() ? dv.toString().toInt() : dv.toInt();
                            if (day == today)
                                signedNow = rec.value(QLatin1String("signed")).toBool();
                        }
                    }
                    signedToday_ = signedNow;
                    emit Log(QStringLiteral("daily ok id=%1 signed=%2")
                                 .arg(dailyId_)
                                 .arg(int(signedNow)));
                    emit DailyReady(true, signedNow, dailyId_, QString(), QString());
                    if (pendingSign_) {
                        pendingSign_ = false;
                        if (!signedNow)
                            SignDaily();
                    }
                });
    });
}

void JmApi::SignDaily()
{
    if (avs_.isEmpty() || uid_ == 0) {
        emit SignReady(false, QString(), QStringLiteral("未登录或缺少用户 id"));
        return;
    }
    if (signedToday_) {
        emit SignReady(false, QStringLiteral("今天已经签到"), QString());
        return;
    }
    if (dailyId_ == 0) {
        // 还没取到签到活动 id：先拉 /daily，回来再自动补发（不丢用户的点击）
        pendingSign_ = true;
        FetchDailySign();
        return;
    }
    EnqueueFront([this] {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("user_id"), QString::number(uid_));
        form.addQueryItem(QStringLiteral("daily_id"), QString::number(dailyId_));
        Request(QStringLiteral("/daily_chk"), {},
                [this](const NetResult &r, const QString &ts) {
                    QString err;
                    const QByteArray plain = Unwrap(r, ts, &err);
                    if (plain.isEmpty()) {
                        if (err == QLatin1String("empty data")) {
                            signedToday_ = true;
                            emit SignReady(true, QStringLiteral("打卡成功"), QString());
                            return;
                        }
                        emit Log(QStringLiteral("sign fail: %1").arg(err));
                        emit SignReady(false, QString(), err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    const QString msg = JStr(o, "msg");
                    signedToday_ = true;
                    emit Log(QStringLiteral("sign ok msg=%1").arg(msg));
                    emit SignReady(true, msg.isEmpty() ? QStringLiteral("打卡成功") : msg,
                                   QString());
                },
                true, form.toString(QUrl::FullyEncoded).toLatin1());
    });
}

void JmApi::FavoriteFolder(int page)
{
    if (avs_.isEmpty()) {
        // 没有会话就别发请求：/favorite 匿名必然 401，白等一次超时还把
        // "未登录"和"网络失败"混成同一个错误。
        emit FavoriteReady(false, {}, 0, QStringLiteral("未登录"));
        return;
    }
    Enqueue([this, page] {
        Request(QStringLiteral("/favorite"),
                {{QStringLiteral("page"), QString::number(page)},
                 {QStringLiteral("o"), QStringLiteral("mr")},
                 {QStringLiteral("folder_id"), QStringLiteral("0")}},
                [this, page](const NetResult &r, const QString &ts) {
                    QString err;
                    int code = -1;
                    const QByteArray plain = Unwrap(r, ts, &err, &code);
                    if (plain.isEmpty()) {
                        if (code == 401) {
                            // AVS 已失效：清掉本地会话，别让侧栏继续显示上一个账号
                            avs_.clear();
                            uid_ = 0;
                            emit Log(QStringLiteral("favorite auth expired (code 401)"));
                            emit SessionExpired(QStringLiteral("登录已失效，请重新登录"));
                        }
                        emit FavoriteReady(false, {}, 0, err);
                        return;
                    }
                    const QJsonObject o = QJsonDocument::fromJson(plain).object();
                    QVector<JmComicItem> items;
                    for (const QJsonValue &v : o.value(QLatin1String("list")).toArray())
                        items.append(JItem(v.toObject()));
                    const int total = o.value(QLatin1String("total")).toInt();
                    emit Log(QStringLiteral("favorite page=%1 items=%2 total=%3")
                                 .arg(page).arg(items.size()).arg(total));
                    emit FavoriteReady(true, items, total, QString());
                });
    });
}

void JmApi::FavoriteAdd(const QString &albumId)
{
    if (avs_.isEmpty()) {
        emit FavoriteAddReady(false, QStringLiteral("未登录"));
        return;
    }
    Enqueue([this, albumId] {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("aid"), albumId);
        Request(QStringLiteral("/favorite"), {}, [this, albumId](const NetResult &r, const QString &ts) {
            QString err;
            int code = -1;
            const QByteArray plain = Unwrap(r, ts, &err, &code);
            if (plain.isEmpty()) {
                if (code == 401) {
                    avs_.clear();
                    uid_ = 0;
                    emit Log(QStringLiteral("favorite add auth expired (code 401)"));
                    emit SessionExpired(QStringLiteral("登录已失效，请重新登录"));
                }
                emit FavoriteAddReady(false, err);
                return;
            }
            emit Log(QStringLiteral("favorite add %1: %2")
                         .arg(albumId, QString::fromUtf8(plain.left(120))));
            emit FavoriteAddReady(true, QString());
        }, true, form.toString(QUrl::FullyEncoded).toLatin1());
    });
}
